/* Guest-only overrides for libc calls whose syscalls the miniBox surface
 * does not carry. The guest is one static link, so defining these here means
 * musl's versions are never pulled in.
 *
 * QEMU's event loop lives on eventfd + ppoll; glib's main context wants a
 * wakeup fd too. None of that machinery exists in the sandbox, so a tiny
 * in-guest fd space stands in: fds at FAKE_BASE and above are counters in
 * guest memory, read/write/poll on them is bookkeeping, and a blocking poll
 * yields to the other green threads - which is where all progress (the vCPU,
 * the virtual clock) comes from anyway. Determinism falls out: everything
 * here is guest state driven by guest execution.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sched.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define FAKE_BASE 0x40000000
#define FAKE_MAX  256

typedef struct {
    int used;
    int is_eventfd;    /* else: pipe end */
    int peer;          /* pipe: index of the other end; -1 for eventfd */
    int readable_end;  /* pipe: 1 if this is the read end */
    uint64_t counter;  /* eventfd counter / pipe pending byte count */
} FakeFd;

static FakeFd g_fds[FAKE_MAX];

static int fake_alloc(void)
{
    for (int i = 0; i < FAKE_MAX; i++) {
        if (!g_fds[i].used) {
            memset(&g_fds[i], 0, sizeof g_fds[i]);
            g_fds[i].used = 1;
            return i;
        }
    }
    errno = EMFILE;
    return -1;
}

static FakeFd *fake_of(int fd)
{
    int i = fd - FAKE_BASE;
    if (i < 0 || i >= FAKE_MAX || !g_fds[i].used) {
        return 0;
    }
    return &g_fds[i];
}

int eventfd(unsigned int initval, int flags)
{
    (void)flags;
    int i = fake_alloc();
    if (i < 0) {
        return -1;
    }
    g_fds[i].is_eventfd = 1;
    g_fds[i].peer = -1;
    g_fds[i].counter = initval;
    return FAKE_BASE + i;
}

int pipe2(int fds[2], int flags)
{
    (void)flags;
    int r = fake_alloc();
    if (r < 0) {
        return -1;
    }
    int w = fake_alloc();
    if (w < 0) {
        g_fds[r].used = 0;
        return -1;
    }
    g_fds[r].peer = w;
    g_fds[r].readable_end = 1;
    g_fds[w].peer = r;
    fds[0] = FAKE_BASE + r;
    fds[1] = FAKE_BASE + w;
    return 0;
}

int pipe(int fds[2])
{
    return pipe2(fds, 0);
}

/* the real libc entry points, reached for real fds */
ssize_t __libc_read(int, void *, size_t);
ssize_t __libc_write(int, const void *, size_t);
int __libc_close(int);

/* musl exports read/write/close as strong symbols; override them and go
 * through syscall() for the real ones */
#include <sys/syscall.h>
long syscall(long, ...);

ssize_t read(int fd, void *buf, size_t n)
{
    FakeFd *f = fake_of(fd);
    if (!f) {
        return syscall(SYS_read, fd, buf, n);
    }
    FakeFd *src = f->is_eventfd ? f : f; /* pipe read end holds the count */
    if (src->counter == 0) {
        errno = EAGAIN;
        return -1;
    }
    if (f->is_eventfd) {
        if (n < 8) {
            errno = EINVAL;
            return -1;
        }
        uint64_t v = src->counter;
        src->counter = 0;
        memcpy(buf, &v, 8);
        return 8;
    }
    /* pipe: hand back up to n pending bytes (content is meaningless - the
     * users of these pipes signal, they do not transfer data) */
    size_t take = src->counter < n ? (size_t)src->counter : n;
    src->counter -= take;
    memset(buf, 0, take);
    return (ssize_t)take;
}

ssize_t write(int fd, const void *buf, size_t n)
{
    FakeFd *f = fake_of(fd);
    if (!f) {
        return syscall(SYS_write, fd, buf, n);
    }
    if (f->is_eventfd) {
        if (n < 8) {
            errno = EINVAL;
            return -1;
        }
        uint64_t v;
        memcpy(&v, buf, 8);
        f->counter += v;
        return 8;
    }
    /* pipe write end: credit the read end */
    FakeFd *r = &g_fds[f->peer];
    r->counter += n;
    return (ssize_t)n;
}

int close(int fd)
{
    FakeFd *f = fake_of(fd);
    if (!f) {
        return syscall(SYS_close, fd);
    }
    f->used = 0;
    return 0;
}

int fcntl(int fd, int cmd, ...)
{
    if (fake_of(fd)) {
        return 0; /* O_NONBLOCK et al: taken as given */
    }
    /* the real fcntl syscall is not in the miniBox surface either; mounted
     * files need nothing from it */
    return 0;
}

/* ---- poll: fake-fd aware, blocking by yielding --------------------------- */

static int poll_once(struct pollfd *fds, nfds_t n)
{
    int ready = 0;
    for (nfds_t i = 0; i < n; i++) {
        fds[i].revents = 0;
        FakeFd *f = fake_of(fds[i].fd);
        if (!f) {
            /* a real fd in the set: mounted files are always readable */
            if (fds[i].fd >= 0) {
                fds[i].revents = fds[i].events & (POLLIN | POLLOUT);
            }
        } else {
            if ((fds[i].events & POLLIN) && f->counter > 0) {
                fds[i].revents |= POLLIN;
            }
            if (fds[i].events & POLLOUT) {
                fds[i].revents |= POLLOUT;
            }
        }
        if (fds[i].revents) {
            ready++;
        }
    }
    return ready;
}

int ppoll(struct pollfd *fds, nfds_t n, const struct timespec *ts,
          const sigset_t *mask)
{
    (void)mask;
    int64_t budget_ns = -1;
    if (ts) {
        budget_ns = (int64_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
    }
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int ready = poll_once(fds, n);
        if (ready > 0) {
            return ready;
        }
        if (budget_ns == 0) {
            return 0;
        }
        if (budget_ns > 0) {
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            int64_t elapsed = (int64_t)(t1.tv_sec - t0.tv_sec) * 1000000000
                            + (t1.tv_nsec - t0.tv_nsec);
            if (elapsed >= budget_ns) {
                return 0;
            }
        }
        /* yield to the other green threads - the vCPU, the clocks */
        struct timespec tiny = { 0, 1 };
        nanosleep(&tiny, 0);
    }
}

int poll(struct pollfd *fds, nfds_t n, int timeout_ms)
{
    if (timeout_ms < 0) {
        return ppoll(fds, n, 0, 0);
    }
    struct timespec ts = { timeout_ms / 1000, (timeout_ms % 1000) * 1000000L };
    return ppoll(fds, n, &ts, 0);
}

/* ---- scatter I/O at explicit offsets (the aio thread pool's diet) -------- */

ssize_t pread(int fd, void *buf, size_t n, off_t off)
{
    off_t save = lseek(fd, 0, SEEK_CUR);
    if (save < 0) {
        return -1;
    }
    if (lseek(fd, off, SEEK_SET) < 0) {
        return -1;
    }
    ssize_t r = read(fd, buf, n);
    lseek(fd, save, SEEK_SET);
    return r;
}

ssize_t pwrite(int fd, const void *buf, size_t n, off_t off)
{
    off_t save = lseek(fd, 0, SEEK_CUR);
    if (save < 0) {
        return -1;
    }
    if (lseek(fd, off, SEEK_SET) < 0) {
        return -1;
    }
    ssize_t r = write(fd, buf, n);
    lseek(fd, save, SEEK_SET);
    return r;
}

ssize_t preadv(int fd, const struct iovec *iov, int cnt, off_t off)
{
    ssize_t total = 0;
    for (int i = 0; i < cnt; i++) {
        ssize_t r = pread(fd, iov[i].iov_base, iov[i].iov_len, off + total);
        if (r < 0) {
            return total > 0 ? total : r;
        }
        total += r;
        if ((size_t)r < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

ssize_t pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
    ssize_t total = 0;
    for (int i = 0; i < cnt; i++) {
        ssize_t r = pwrite(fd, iov[i].iov_base, iov[i].iov_len, off + total);
        if (r < 0) {
            return total > 0 ? total : r;
        }
        total += r;
        if ((size_t)r < iov[i].iov_len) {
            break;
        }
    }
    return total;
}

/* ---- signals: none exist here -------------------------------------------- */

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    (void)sig;
    (void)act;
    if (old) {
        memset(old, 0, sizeof *old);
    }
    return 0;
}

/* ---- entropy: deterministic by construction ------------------------------ */

ssize_t getrandom(void *buf, size_t n, unsigned int flags)
{
    (void)flags;
    /* a fixed keyed counter stream: stable across runs and platforms */
    static uint64_t ctr;
    uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) {
        uint64_t x = ctr++ * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;
        x ^= x >> 32;
        p[i] = (uint8_t)x;
    }
    return (ssize_t)n;
}

/* musl's open() issues an internal SYS_fcntl when it sees O_CLOEXEC - a
 * syscall the sandbox does not carry, and one with no meaning here (nothing
 * ever execs). Overriding open() replaces that whole code path. */
#include <stdarg.h>
int open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
    }
    return (int)syscall(SYS_open, path, flags & ~O_CLOEXEC, mode);
}

#include <sys/resource.h>
int getrlimit(int res, struct rlimit *rl)
{
    (void)res;
    rl->rlim_cur = 65536;
    rl->rlim_max = 65536;
    return 0;
}
int setrlimit(int res, const struct rlimit *rl)
{
    (void)res;
    (void)rl;
    return 0;
}

int prctl(int op, ...)
{
    (void)op; /* thread names, dumpable bits: nothing here cares */
    return 0;
}

int memfd_create(const char *name, unsigned int flags)
{
    (void)name;
    (void)flags; /* no shared memory here; QEMU has fallbacks */
    errno = ENOSYS;
    return -1;
}

#include <sys/epoll.h>
int epoll_create1(int flags)
{
    (void)flags; /* aio falls back to its poll fdmon */
    errno = ENOSYS;
    return -1;
}
int epoll_ctl(int ep, int op, int fd, struct epoll_event *ev)
{
    (void)ep; (void)op; (void)fd; (void)ev;
    errno = ENOSYS;
    return -1;
}
int epoll_wait(int ep, struct epoll_event *ev, int cnt, int ms)
{
    (void)ep; (void)ev; (void)cnt; (void)ms;
    errno = ENOSYS;
    return -1;
}

#include <sys/signalfd.h>
int signalfd(int fd, const sigset_t *mask, int flags)
{
    (void)mask;
    (void)flags;
    if (fd != -1) {
        return fd; /* re-arm of an existing one */
    }
    /* a fake fd that is never readable: no signal will ever arrive */
    int i = fake_alloc();
    if (i < 0) {
        return -1;
    }
    g_fds[i].is_eventfd = 1;
    g_fds[i].peer = -1;
    g_fds[i].counter = 0;
    return FAKE_BASE + i;
}

#include <sys/sysinfo.h>
int sysinfo(struct sysinfo *si)
{
    memset(si, 0, sizeof *si);
    si->totalram = 2ull << 30; /* a fixed story: 2GB, one unit */
    si->freeram = 1ull << 30;
    si->mem_unit = 1;
    si->procs = 1;
    return 0;
}

/* musl's own callers (sysconf _SC_PHYS_PAGES) go through this internal name */
int __lsysinfo(struct sysinfo *si)
{
    return sysinfo(si);
}

/* ---- odds and ends -------------------------------------------------------- */

int access(const char *path, int mode)
{
    (void)mode;
    struct stat st;
    return stat(path, &st);
}

ssize_t readlink(const char *path, char *buf, size_t n)
{
    (void)path;
    (void)buf;
    (void)n;
    errno = EINVAL; /* nothing here is a symlink */
    return -1;
}

uid_t getuid(void) { return 0; }
uid_t geteuid(void) { return 0; }
gid_t getgid(void) { return 0; }
gid_t getegid(void) { return 0; }
mode_t umask(mode_t m) { (void)m; return 022; }

int dup(int fd) { (void)fd; errno = EMFILE; return -1; }
int dup2(int od, int nd) { (void)od; (void)nd; errno = EMFILE; return -1; }


int mkdir(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = EROFS;
    return -1;
}

int unlink(const char *path)
{
    (void)path;
    errno = EROFS;
    return -1;
}

char *getcwd(char *buf, size_t size)
{
    if (!buf || size < 2) {
        errno = ERANGE;
        return 0;
    }
    buf[0] = '/';
    buf[1] = '\0';
    return buf;
}
