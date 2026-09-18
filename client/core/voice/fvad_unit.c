/* libfvad (BSD-3-Clause, third_party/libfvad), compiled here and nowhere else:
 * the WebRTC voice-activity detector voice input segments speech with
 * (ARCH-112). Vendored source, not ours, so our warnings are held off around it
 * rather than its code edited -- the arrangement miniaudio has in audio_dev.c. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wshift-negative-value"
#endif
/* Its compile-time checks use C11's static_assert, which glibc's assert.h
 * provides to C99 under _GNU_SOURCE and mingw's does not; the compiler has the
 * keyword either way. */
#include <assert.h>
#ifndef static_assert
#define static_assert _Static_assert
#endif
#include "../../../third_party/libfvad/src/fvad.c"
#include "../../../third_party/libfvad/src/signal_processing/division_operations.c"
#include "../../../third_party/libfvad/src/signal_processing/energy.c"
#include "../../../third_party/libfvad/src/signal_processing/get_scaling_square.c"
#include "../../../third_party/libfvad/src/signal_processing/resample_48khz.c"
#include "../../../third_party/libfvad/src/signal_processing/resample_by_2_internal.c"
#include "../../../third_party/libfvad/src/signal_processing/resample_fractional.c"
#include "../../../third_party/libfvad/src/signal_processing/spl_inl.c"
#include "../../../third_party/libfvad/src/vad/vad_core.c"
#include "../../../third_party/libfvad/src/vad/vad_filterbank.c"
#include "../../../third_party/libfvad/src/vad/vad_gmm.c"
#include "../../../third_party/libfvad/src/vad/vad_sp.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
