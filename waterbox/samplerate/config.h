/* CHIMERA: fixed configuration for the vendored libsamplerate 0.2.2 (BSD-2,
 * see COPYING). Only the SINC_FASTEST converter is compiled - it is the one
 * the MCPX VP uses - so only fastest_coeffs.h rides along. Vendoring the
 * sources into the xemu build makes the native reference and the guest
 * compile the SAME resampler with the SAME flags: a library built twice
 * (distro vs static, glibc vs musl headers) rounds a handful of samples one
 * ULP apart, which the audio gate leg refuses.
 */
#define HAVE_CONFIG_H_SEEN 1
#define HAVE_STDBOOL_H 1
#define ENABLE_SINC_FAST_CONVERTER 1
#define CPU_CLIPS_POSITIVE 0
#define CPU_CLIPS_NEGATIVE 0
#define PACKAGE "libsamplerate"
#define VERSION "0.2.2"
