// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The transient designer's instance oracle (omx_transient_instance.h), at every declared rate:
//   A  the instance IS the kernel: bit-identical to omx_transient_resolve + omx_transient_process
//      over the same controls, in odd block sizes, with separate and aliased in/out buffers;
//   B  every hostile port word (±Inf, NaN, far outside the travel) is clamped before the kernel:
//      the output is bit-identical to the kernel's at the clamped controls, and the ledger is clean;
//   C  bypassed and not-ready are the identity byte for byte;
//   D  re-engaging clears the state: the first block after the edge equals a fresh instance's;
//   E  the output knob's gain at full scale, within 0.05 dB, and the published latency is zero.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_transient_instance.h>

#include "instance_oracle.h"

#define N 8192u

static float in_l[N], in_r[N], out_l[N], out_r[N], ref_l[N], ref_r[N];

static void run_blocks(OmxTransientInstance *s, int alias) {
  static const uint32_t sizes[] = {1u, 64u, 127u, 256u, 33u};
  uint32_t o = 0, k = 0;
  if (alias) {
    memcpy(out_l, in_l, sizeof out_l);
    memcpy(out_r, in_r, sizeof out_r);
  }
  while (o < N) {
    uint32_t b = sizes[k++ % 5u];
    if (b > N - o) b = N - o;
    if (alias)
      omx_transient_instance_run(s, out_l + o, out_r + o, out_l + o, out_r + o, b);
    else
      omx_transient_instance_run(s, in_l + o, in_r + o, out_l + o, out_r + o, b);
    o += b;
  }
}

static void kernel_ref(float a, float su, float at, float st, float od) {
  struct omx_transient t;
  struct omx_transient_state st0;
  omx_transient_resolve(&t, 0, a, su, at, st, od, g_sr);
  omx_transient_state_init(&st0);
  memcpy(ref_l, in_l, sizeof ref_l);
  memcpy(ref_r, in_r, sizeof ref_r);
  omx_transient_process(ref_l, ref_r, N, &t, &st0);
}

static int same(void) { return memcmp(out_l, ref_l, sizeof out_l) == 0 && memcmp(out_r, ref_r, sizeof out_r) == 0; }

static void arm_identity(void) {
  g_arm = "A identity";
  for (int alias = 0; alias < 2; alias++) {
    OmxTransientInstance s;
    ok(omx_transient_instance_init(&s, g_sr) == 1, "init at a declared rate", 0, 1);
    omx_transient_instance_resolve(&s, 0, 9.0f, -6.0f, 10.0f, 250.0f, 1.5f);
    run_blocks(&s, alias);
    kernel_ref(9.0f, -6.0f, 10.0f, 250.0f, 1.5f);
    ok(same(), alias ? "aliased in/out equals the kernel" : "separate in/out equals the kernel", 0, 0);
  }
  expect_clean();
}

static void arm_clamps(void) {
  g_arm = "B clamps";
  const float inf = INFINITY, nan = NAN;
  /* {host words} -> {the controls the kernel must have seen} */
  static const struct { float w[5], c[5]; } rows[] = {
      {{100.0f, -100.0f, 0.0f, 1e9f, 99.0f}, {24.0f, -24.0f, 2.0f, 2000.0f, 12.0f}},
      {{-1e30f, 1e30f, 1e30f, -5.0f, -1e30f}, {-24.0f, 24.0f, 50.0f, 50.0f, -24.0f}},
  };
  for (unsigned k = 0; k < sizeof rows / sizeof rows[0]; k++) {
    OmxTransientInstance s;
    omx_transient_instance_init(&s, g_sr);
    omx_transient_instance_resolve(&s, 0, rows[k].w[0], rows[k].w[1], rows[k].w[2], rows[k].w[3], rows[k].w[4]);
    run_blocks(&s, 0);
    kernel_ref(rows[k].c[0], rows[k].c[1], rows[k].c[2], rows[k].c[3], rows[k].c[4]);
    ok(same(), "out-of-travel words equal the kernel at the travel's ends", k, 0);
  }
  /* Non-finite: a gain reads its default (0 dB), a time NaN its floor, ±Inf its ends. */
  OmxTransientInstance s;
  omx_transient_instance_init(&s, g_sr);
  omx_transient_instance_resolve(&s, 0, nan, inf, nan, inf, -inf);
  run_blocks(&s, 0);
  kernel_ref(0.0f, 0.0f, OMX_TRANSIENT_ATTACK_TIME_MS_MIN, OMX_TRANSIENT_SUSTAIN_TIME_MS_MAX, 0.0f);
  ok(same(), "non-finite words read the declared default / floor / end", 0, 0);
  omx_transient_instance_resolve(&s, 0, nan, -inf, -inf, nan, nan);
  ok(s.atom.attack_db == 0.0f && s.atom.sustain_db == 0.0f && s.atom.output_db == 0.0f &&
         s.attack_time_ms == OMX_TRANSIENT_ATTACK_TIME_MS_MIN && s.sustain_time_ms == OMX_TRANSIENT_SUSTAIN_TIME_MS_MIN,
     "the atom holds the clamped controls", s.atom.sustain_db, 0);
  expect_clean();
}

static void arm_bypass(void) {
  g_arm = "C bypass";
  OmxTransientInstance s;
  omx_transient_instance_init(&s, g_sr);
  omx_transient_instance_resolve(&s, 1, 24.0f, 24.0f, 10.0f, 250.0f, 12.0f);
  run_blocks(&s, 0);
  ok(memcmp(out_l, in_l, sizeof out_l) == 0 && memcmp(out_r, in_r, sizeof out_r) == 0, "bypass is the identity", 0, 0);
  OmxTransientInstance bad;
  ok(omx_transient_instance_init(&bad, 12345.0f) == 0, "an undeclared rate is refused", 1, 0);
  omx_transient_instance_resolve(&bad, 0, 24.0f, 24.0f, 10.0f, 250.0f, 12.0f);
  run_blocks(&bad, 0);
  ok(memcmp(out_l, in_l, sizeof out_l) == 0 && memcmp(out_r, in_r, sizeof out_r) == 0, "not ready is the identity", 0, 0);
  expect_clean();
}

static void arm_edge(void) {
  g_arm = "D engage edge";
  OmxTransientInstance s, fresh;
  omx_transient_instance_init(&s, g_sr);
  omx_transient_instance_init(&fresh, g_sr);
  omx_transient_instance_resolve(&s, 0, 12.0f, 6.0f, 10.0f, 250.0f, 0.0f);
  omx_transient_instance_run(&s, in_l, in_r, out_l, out_r, N / 2);
  omx_transient_instance_resolve(&s, 1, 12.0f, 6.0f, 10.0f, 250.0f, 0.0f);
  omx_transient_instance_run(&s, in_l, in_r, out_l, out_r, 256u);
  omx_transient_instance_resolve(&s, 0, 12.0f, 6.0f, 10.0f, 250.0f, 0.0f);
  omx_transient_instance_run(&s, in_l, in_r, out_l, out_r, N / 2);
  omx_transient_instance_resolve(&fresh, 0, 12.0f, 6.0f, 10.0f, 250.0f, 0.0f);
  omx_transient_instance_run(&fresh, in_l, in_r, ref_l, ref_r, N / 2);
  ok(memcmp(out_l, ref_l, N / 2 * sizeof(float)) == 0 && memcmp(out_r, ref_r, N / 2 * sizeof(float)) == 0,
     "re-engaged output equals a fresh instance's", 0, 0);
  expect_clean();
}

static void arm_gain(void) {
  g_arm = "E gain";
  OmxTransientInstance s;
  omx_transient_instance_init(&s, g_sr);
  omx_transient_instance_resolve(&s, 0, 0.0f, 0.0f, 10.0f, 250.0f, 6.0f);
  sine(in_l, N, 1000.0, g_sr, 0.5);
  memcpy(in_r, in_l, sizeof in_r);
  omx_transient_instance_run(&s, in_l, in_r, out_l, out_r, N);
  const double db = 20.0 * log10(rms(out_l, N / 2, N) / rms(in_l, N / 2, N));
  ok(fabs(db - 6.0) < 0.05, "output knob +6 dB", db, 6.0);
  ok(OMX_TRANSIENT_INSTANCE_LATENCY_FRAMES == 0.0f && omx_transient_latency() == 0, "latency zero", 0, 0);
  expect_clean();
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = OMX_DECLARED_RATES[ri];
    stimulus(in_l, in_r, N, 0x1234567u, 2048u);
    arm_identity();
    arm_clamps();
    arm_bypass();
    arm_edge();
    arm_gain();
  }
  return instance_oracle_end("transient_instance");
}
