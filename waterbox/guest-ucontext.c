/* A minimal ucontext for the waterbox guest - just enough for QEMU's
 * coroutine-ucontext backend, which is the one coroutine flavour that works
 * without signals.
 *
 * QEMU's usage (util/coroutine-ucontext.c): getcontext() once for a valid
 * ucontext_t, makecontext() to aim it at coroutine_trampoline(i0, i1) on a
 * fresh stack, swapcontext() to enter it exactly once - the return trip and
 * every later switch is sigsetjmp/siglongjmp. The old-context half of
 * swapcontext is never resumed, so nothing needs to be captured.
 *
 * musl ships the <ucontext.h> types but deliberately no implementation.
 */
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ucontext.h>

struct stash {
    void (*fn)(int, int);
    long a0, a1;
    void *sp;
};

int getcontext(ucontext_t *uc)
{
    memset(&uc->uc_mcontext, 0, sizeof uc->uc_mcontext);
    return 0;
}

void makecontext(ucontext_t *uc, void (*fn)(void), int argc, ...)
{
    struct stash st;
    va_list ap;

    va_start(ap, argc);
    st.fn = (void (*)(int, int))fn;
    st.a0 = argc >= 1 ? va_arg(ap, int) : 0;
    st.a1 = argc >= 2 ? va_arg(ap, int) : 0;
    va_end(ap);

    uintptr_t top = (uintptr_t)uc->uc_stack.ss_sp + uc->uc_stack.ss_size;
    st.sp = (void *)(top & ~(uintptr_t)15);

    _Static_assert(sizeof(struct stash) <= sizeof(uc->uc_mcontext),
                   "stash must fit in mcontext");
    memcpy(&uc->uc_mcontext, &st, sizeof st);
}

int swapcontext(ucontext_t *ouc, const ucontext_t *uc)
{
    (void)ouc; /* nobody ever resumes it - QEMU returns via siglongjmp */
    struct stash st;
    memcpy(&st, &uc->uc_mcontext, sizeof st);

    register long rdi __asm__("rdi") = st.a0;
    register long rsi __asm__("rsi") = st.a1;
    register void *rax __asm__("rax") = (void *)st.fn;
    register void *rdx __asm__("rdx") = st.sp;
    __asm__ volatile(
        "mov %%rdx, %%rsp\n\t"
        "xor %%ebp, %%ebp\n\t"
        "call *%%rax\n\t"
        "ud2\n\t" /* the trampoline never returns */
        :
        : "r"(rdi), "r"(rsi), "r"(rax), "r"(rdx)
        : "memory");
    __builtin_unreachable();
}

int setcontext(const ucontext_t *uc)
{
    (void)uc;
    abort(); /* nothing here uses it */
}
