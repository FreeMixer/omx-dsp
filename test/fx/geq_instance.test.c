// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The graphic EQ's instance oracle (omx_geq_instance.h), at every declared rate:
//   A  the instance IS the kernel: bit-identical to omx_eq_design at the ISO centres and the
//      declared Q + omx_geq_set + omx_geq_process, in odd block sizes, separate and aliased buffers;
//   B  every hostile band word (±Inf, NaN, far outside the travel) is clamped before the design:
//      the output is bit-identical to the kernel's at the clamped gains, and the ledger is clean;
//   C  flat, bypassed and not-ready are the identity byte for byte;
//   D  re-engaging clears the histories: the first block after the edge equals a fresh instance's;
//   E  a band's gain at its centre, within 0.1 dB, and the published latency is zero.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_geq_instance.h>

#include "instance_oracle.h"

#define N 32768u

static float in_l[N], in_r[N], out_l[N], out_r[N], ref_l[N], ref_r[N];

static void run_blocks(OmxGeqInstance *s, int alias) {
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
      omx_geq_instance_run(s, out_l + o, out_r + o, out_l + o, out_r + o, b);
    else
      omx_geq_instance_run(s, in_l + o, in_r + o, out_l + o, out_r + o, b);
    o += b;
  }
}

static void kernel_ref(const float g[OMX_GEQ_BANDS]) {
  static const double centres[] = OMX_GEQ_INSTANCE_CENTRES_HZ;
  double c[OMX_GEQ_BANDS][5];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) omx_eq_design(OMX_EQ_PEAKING, centres[k], OMX_GEQ_BAND_Q_DOUBLE, g[k], g_sr, c[k]);
  struct omx_geq p;
  struct omx_geq_state st;
  omx_geq_set(&p, 1, (const double(*)[5])c, g);
  omx_geq_state_init(&st);
  memcpy(ref_l, in_l, sizeof ref_l);
  memcpy(ref_r, in_r, sizeof ref_r);
  omx_geq_process(ref_l, ref_r, N, &p, &st);
}

static int same(void) { return memcmp(out_l, ref_l, sizeof out_l) == 0 && memcmp(out_r, ref_r, sizeof out_r) == 0; }

static void arm_identity(void) {
  g_arm = "A identity";
  float g[OMX_GEQ_BANDS];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) g[k] = (k % 3 == 0) ? 0.0f : (float)((k * 7) % 31 - 15);
  for (int alias = 0; alias < 2; alias++) {
    OmxGeqInstance s;
    ok(omx_geq_instance_init(&s, g_sr) == 1, "init at a declared rate", 0, 1);
    omx_geq_instance_resolve(&s, 0, g);
    run_blocks(&s, alias);
    kernel_ref(g);
    ok(same(), alias ? "aliased in/out equals the kernel" : "separate in/out equals the kernel", 0, 0);
  }
  expect_clean();
}

static void arm_clamps(void) {
  g_arm = "B clamps";
  float w[OMX_GEQ_BANDS], c[OMX_GEQ_BANDS];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    switch (k % 5) {
      case 0: w[k] = 40.0f; c[k] = OMX_EQ_GAIN_RANGE_MAX; break;
      case 1: w[k] = -1e30f; c[k] = OMX_EQ_GAIN_RANGE_MIN; break;
      case 2: w[k] = NAN; c[k] = 0.0f; break;
      case 3: w[k] = (k & 1) ? INFINITY : -INFINITY; c[k] = 0.0f; break;
      default: w[k] = 3.5f; c[k] = 3.5f; break;
    }
  }
  OmxGeqInstance s;
  omx_geq_instance_init(&s, g_sr);
  omx_geq_instance_resolve(&s, 0, w);
  run_blocks(&s, 0);
  kernel_ref(c);
  ok(same(), "hostile words equal the kernel at the clamped gains", 0, 0);
  int held = 1;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) held &= s.gain_db[k] == c[k];
  ok(held, "the instance holds the clamped gains", 0, 0);
  expect_clean();
}

static void arm_identity_paths(void) {
  g_arm = "C identity paths";
  float flat[OMX_GEQ_BANDS] = {0}, loud[OMX_GEQ_BANDS];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) loud[k] = 12.0f;
  OmxGeqInstance s;
  omx_geq_instance_init(&s, g_sr);
  omx_geq_instance_resolve(&s, 0, flat);
  run_blocks(&s, 0);
  ok(memcmp(out_l, in_l, sizeof out_l) == 0 && memcmp(out_r, in_r, sizeof out_r) == 0, "flat is the identity", 0, 0);
  omx_geq_instance_resolve(&s, 1, loud);
  run_blocks(&s, 0);
  ok(memcmp(out_l, in_l, sizeof out_l) == 0 && memcmp(out_r, in_r, sizeof out_r) == 0, "bypass is the identity", 0, 0);
  OmxGeqInstance bad;
  ok(omx_geq_instance_init(&bad, 22050.0) == 0, "a rate below the top centre's Nyquist is refused", 1, 0);
  omx_geq_instance_resolve(&bad, 0, loud);
  run_blocks(&bad, 0);
  ok(memcmp(out_l, in_l, sizeof out_l) == 0 && memcmp(out_r, in_r, sizeof out_r) == 0, "not ready is the identity", 0, 0);
  expect_clean();
}

static void arm_edge(void) {
  g_arm = "D engage edge";
  float g[OMX_GEQ_BANDS];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) g[k] = (k & 1) ? 9.0f : -6.0f;
  OmxGeqInstance s, fresh;
  omx_geq_instance_init(&s, g_sr);
  omx_geq_instance_init(&fresh, g_sr);
  omx_geq_instance_resolve(&s, 0, g);
  omx_geq_instance_run(&s, in_l, in_r, out_l, out_r, N / 2);
  omx_geq_instance_resolve(&s, 1, g);
  omx_geq_instance_run(&s, in_l, in_r, out_l, out_r, 256u);
  omx_geq_instance_resolve(&s, 0, g);
  omx_geq_instance_run(&s, in_l, in_r, out_l, out_r, N / 2);
  omx_geq_instance_resolve(&fresh, 0, g);
  omx_geq_instance_run(&fresh, in_l, in_r, ref_l, ref_r, N / 2);
  ok(memcmp(out_l, ref_l, N / 2 * sizeof(float)) == 0 && memcmp(out_r, ref_r, N / 2 * sizeof(float)) == 0,
     "re-engaged output equals a fresh instance's", 0, 0);
  expect_clean();
}

static void arm_gain(void) {
  g_arm = "E gain";
  static const struct { int band; float db; double hz; } rows[] = {{17, 12.0f, 1000.0}, {7, -9.0f, 100.0}, {25, 15.0f, 6300.0}};
  for (unsigned k = 0; k < sizeof rows / sizeof rows[0]; k++) {
    float g[OMX_GEQ_BANDS] = {0};
    g[rows[k].band] = rows[k].db;
    OmxGeqInstance s;
    omx_geq_instance_init(&s, g_sr);
    omx_geq_instance_resolve(&s, 0, g);
    sine(in_l, N, rows[k].hz, g_sr, 0.1);
    memcpy(in_r, in_l, sizeof in_r);
    omx_geq_instance_run(&s, in_l, in_r, out_l, out_r, N);
    const double db = 20.0 * log10(rms(out_l, N / 2, N) / rms(in_l, N / 2, N));
    ok(fabs(db - rows[k].db) < 0.1, "a band's gain at its centre", db, rows[k].db);
  }
  ok(OMX_GEQ_INSTANCE_LATENCY_FRAMES == 0.0f, "latency zero", 0, 0);
  expect_clean();
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = OMX_DECLARED_RATES[ri];
    stimulus(in_l, in_r, N, 0x1234567u, 0u);
    arm_identity();
    arm_clamps();
    arm_identity_paths();
    arm_edge();
    arm_gain();
  }
  return instance_oracle_end("geq_instance");
}
