// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// fastmath_partial_render.c — one render of the loops omx_fastmath.h exists for, compiled twice by
// `make fastmath-partial`: once with the flags of a libmvec that has log10f and expf but no powf and no tanhf
// (aarch64 glibc before 2.40), once on the plain libm. OMX_PARTIAL_SIDE names the side; fastmath_partial.test.c
// calls both and compares them.
#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/omx_dyn.h>

#define OMX_PARTIAL_CAT_(a, b) a##_##b
#define OMX_PARTIAL_CAT(a, b) OMX_PARTIAL_CAT_(a, b)

#ifndef OMX_DRIVE_LANES
/* a header that does not say: its tanhf loop carries the `omp simd` pragma whenever any libmvec function is declared */
#ifdef OMX_FASTMATH_MVEC
#define OMX_DRIVE_LANES 1
#else
#define OMX_DRIVE_LANES 0
#endif
#endif

/** @brief Bit 1: the dynamics loops take the branch-free lanes; bit 2: the drive's tanhf loop is vectorised. */
int OMX_PARTIAL_CAT(OMX_PARTIAL_SIDE, lanes)(void) { return (OMX_DYN_LANES ? 1 : 0) | (OMX_DRIVE_LANES ? 2 : 0); }

static uint32_t seed;
static float rnd(void) {
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  return (float)(seed >> 8) / 16777216.0f * 2.0f - 1.0f;
}

/** @brief The cases' count, and the samples each writes. */
#define OMX_PARTIAL_CASES 6u
#define OMX_PARTIAL_N 8192u

/**
 * @brief Render every case into `out` (OMX_PARTIAL_CASES * OMX_PARTIAL_N samples): the dynamics slot in ABOVE
 *        with a soft and a hard knee, in UPWARD, in BELOW with its knee off centre and at +knee/2, and the drive's
 *        tanh curve.
 */
void OMX_PARTIAL_CAT(OMX_PARTIAL_SIDE, render)(float *out) {
  /* mode, threshold, ratio, knee, range, shift */
  static const float cases[5][6] = {
      {OMX_DYN_ABOVE, -20.0f, 4.0f, 6.0f, 0.0f, 0.0f},   {OMX_DYN_ABOVE, -20.0f, 4.0f, 0.0f, 0.0f, 0.0f},
      {OMX_DYN_UPWARD, -40.0f, 2.0f, 6.0f, 12.0f, 0.0f}, {OMX_DYN_BELOW, -40.0f, 4.0f, 8.0f, -80.0f, 2.0f},
      {OMX_DYN_BELOW, -40.0f, 4.0f, 8.0f, -80.0f, 4.0f},
  };
  const float sr = 48000.0f;
  for (uint32_t c = 0; c < 5u; c++) {
    struct omx_dyn p;
    memset(&p, 0, sizeof p);
    p.enabled = 1;
    p.gc.mode = (int)cases[c][0];
    p.gc.thresh_db = cases[c][1];
    p.gc.ratio = cases[c][2];
    p.gc.knee_db = cases[c][3];
    p.gc.range_db = cases[c][4];
    p.gc.makeup_lin = 1.0f;
    p.knee_shift_db = cases[c][5];
    p.detect = OMX_DETECT_PEAK;
    p.ovs_mode = OMX_DYN_OVS_OFF;
    p.attack_ms = 2.0f;
    p.attack_coeff = omx_pole_from_time_ms(2.0f, sr);
    p.release_coeff = omx_pole_from_time_ms(80.0f, sr);
    struct omx_dyn_state st;
    omx_dyn_state_init(&st, 1u);
    float *x = out + c * OMX_PARTIAL_N;
    seed = 977u + c;
    for (uint32_t i = 0; i < OMX_PARTIAL_N; i++) {
      /* a swell from about -100 dBFS to -3 dBFS: every case's knee and both of its sides */
      const float t = (float)i / (float)OMX_PARTIAL_N;
      x[i] = powf(10.0f, (-100.0f + 97.0f * t) / 20.0f) * rnd();
    }
    for (uint32_t off = 0; off < OMX_PARTIAL_N; off += 128u) omx_dynamics_keyed(x + off, NULL, NULL, 128u, &p, &st);
  }
  float *x = out + 5u * OMX_PARTIAL_N;
  seed = 4242u;
  for (uint32_t i = 0; i < OMX_PARTIAL_N; i++) x[i] = rnd();
  const struct omx_drive_bias b = omx_drive_bias_for(OMX_DRIVE_SOFT, 0.3f);
  omx_drive_shape_block(OMX_DRIVE_SOFT, &b, 4.0f, x, OMX_PARTIAL_N);
}
