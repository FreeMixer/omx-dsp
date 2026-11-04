// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// fastmath_blocks.test.c — the block size is not observable on the libmvec path either.
//
// dynamics_keyed.test.c's arm D holds "uneven blocks are one block, bit for bit", but it runs with
// contracts on, where omx_dyn.h takes no lane (OMX_DYN_LANES needs contracts compiled out). A consumer
// builds the kernels with contracts out and omxdsp.pc's libmvec flags, and there a vectorised loop
// over n samples ran its last n mod W samples through the libm's scalar call: a sample's gain then
// depended on where the caller's block ended, and an engine that hands a strip 1, 3 or 65 frames
// rendered different bits from one that hands it 256. This oracle is built like that consumer
// (CFLAGS with the libmvec flags, no OMX_CONTRACTS) and holds, at the nine oracle rates:
//   A  omx_dynamics_keyed: one call against uneven blocks (1, 3, 13, 64, 65, 200, 1000), bit for bit,
//      for a compressor at factor 1 and at 4 and for a downward expander (the flat gain block), centred
//      and off-centre knee;
//   B  omx_drive_shape_block: the tanh curves the same, one block against the uneven blocks.
// Without libmvec in the build it is the scalar path, which holds it trivially, and says so.
#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/omx_dyn.h>
#include <omxdsp/omxdsp_definitions.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0, g_failed = 0;

static void ok(int cond, const char *what, float sr, uint32_t measured) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s @ %.0f Hz: %u samples differ\n", what, (double)sr, measured);
  }
}

static uint32_t g_seed;
static float rnd(void) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return (float)(g_seed >> 8) / 16777216.0f * 2.0f - 1.0f;
}

#define N 8192u
static const uint32_t SIZES[] = {1u, 3u, 13u, 64u, 65u, 200u, 1000u};
#define NSIZES (sizeof SIZES / sizeof SIZES[0])

/* A ramp of bursts: levels sweep through the knee, so every arm of the gain computer is taken. */
static void program(float *x, uint32_t n, uint32_t seed) {
  g_seed = seed;
  for (uint32_t i = 0; i < n; i++) x[i] = (0.02f + 0.9f * (float)(i % 2048u) / 2048.0f) * rnd();
}

static uint32_t mismatches(const float *a, const float *b, uint32_t n) {
  uint32_t m = 0;
  for (uint32_t i = 0; i < n; i++) m += memcmp(&a[i], &b[i], sizeof(float)) != 0;
  return m;
}

static struct omx_dyn atom(int mode, float knee_db, float shift_db, float att_ms, int ovs, float sr) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = mode;
  d.gc.thresh_db = -24.0f;
  d.gc.ratio = mode == OMX_DYN_BELOW ? 2.0f : 4.0f;
  d.gc.knee_db = knee_db;
  d.gc.range_db = mode == OMX_DYN_BELOW ? -40.0f : 0.0f;
  d.gc.makeup_lin = 1.0f;
  d.knee_shift_db = shift_db;
  d.detect = OMX_DETECT_PEAK;
  d.ovs_mode = ovs;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(80.0f, sr);
  return d;
}

static void arm_dynamics(float sr) {
  static float la[N], ra[N], lb[N], rb[N];
  struct {
    const char *what;
    struct omx_dyn d;
  } c[] = {
      {"A compressor, factor 1", atom(OMX_DYN_ABOVE, 6.0f, 0.0f, 5.0f, OMX_DYN_OVS_OFF, sr)},
      {"A compressor, factor 4", atom(OMX_DYN_ABOVE, 6.0f, 0.0f, 0.1f, OMX_DYN_OVS_X4, sr)},
      {"A expander, centred knee", atom(OMX_DYN_BELOW, 6.0f, 0.0f, 5.0f, OMX_DYN_OVS_OFF, sr)},
      {"A expander, off-centre knee", atom(OMX_DYN_BELOW, 6.0f, 2.0f, 5.0f, OMX_DYN_OVS_OFF, sr)},
  };
  for (size_t k = 0; k < sizeof c / sizeof c[0]; k++) {
    program(la, N, 41u);
    program(ra, N, 42u);
    memcpy(lb, la, sizeof la);
    memcpy(rb, ra, sizeof ra);
    struct omx_dyn_state sa, sb;
    omx_dyn_state_init(&sa, 1u);
    omx_dyn_state_init(&sb, 1u);
    omx_dynamics_keyed(la, ra, NULL, N, &c[k].d, &sa);
    for (uint32_t off = 0, j = 0; off < N; j++) {
      uint32_t m = SIZES[j % NSIZES];
      if (off + m > N) m = N - off;
      omx_dynamics_keyed(lb + off, rb + off, NULL, m, &c[k].d, &sb);
      off += m;
    }
    const uint32_t mm = mismatches(la, lb, N) + mismatches(ra, rb, N);
    ok(mm == 0u, c[k].what, sr, mm);
  }
}

static void arm_drive(float sr) {
  static float a[N], b[N];
  const int curves[] = {OMX_DRIVE_SOFT, OMX_DRIVE_EXCITER};
  const char *names[] = {"B drive soft", "B drive exciter"};
  for (int k = 0; k < 2; k++) {
    const struct omx_drive_bias s = omx_drive_bias_for(curves[k], 0.3f);
    program(a, N, 7u + (uint32_t)k + (uint32_t)sr);
    memcpy(b, a, sizeof a);
    omx_drive_shape_block(curves[k], &s, 4.0f, a, N);
    for (uint32_t off = 0, j = 0; off < N; j++) {
      uint32_t m = SIZES[j % NSIZES];
      if (off + m > N) m = N - off;
      omx_drive_shape_block(curves[k], &s, 4.0f, b + off, m);
      off += m;
    }
    ok(mismatches(a, b, N) == 0u, names[k], sr, mismatches(a, b, N));
  }
}

int main(void) {
  static const float rates[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;
  for (unsigned r = 0; r < OMX_ORACLE_FULL_RATES_COUNT; r++) {
    arm_dynamics(rates[r]);
    arm_drive(rates[r]);
  }
  printf("fastmath_blocks: %s, dynamics lanes %d, drive lanes %d: %d checks, %d failed\n",
         omx_fastmath_enabled() ? "libmvec" : "the libm (no libmvec in this build)", OMX_DYN_LANES, OMX_DRIVE_LANES,
         g_checks, g_failed);
  return g_failed ? 1 : 0;
}
