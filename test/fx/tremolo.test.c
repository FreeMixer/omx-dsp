// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * tremolo.test.c — the tremolo / auto-pan kernel against its closed form, at every declared
 * rate (`OMX_DECLARED_RATES`, generated from core's STANDARD_SAMPLE_RATES — never a local list).
 *
 * Spec: docs/design/specs/2026-09-26-native-fx-catalogue.md §2 row "M32 Tremolo / Panner", §6
 * ("every SMALL EXTENSION: an identity arm at the default and a movement arm").
 *
 * The stage is a gain, so a DC input of 1 on both legs reads the gain itself sample by sample.
 * The closed form: the parabola `4t(1−|t|)` spans [−1, 1], so the tremolo's gain spans
 * [1 − mix·depth, 1] and crosses its midpoint `1 − mix·depth/2` rising once a period; the pan's
 * legs each span the same [1 − mix·depth, 1] and at every sample one of them is exactly 1 (the
 * balance law attenuates the far leg only).
 *
 *   A — DEPTH: trough and peak of the gain against `1 − mix·depth` and 1, tremolo mode, three
 *       depths × two mixes × LFO rates {0.1, 1, 5, 20} Hz (the row's travel ends and between).
 *   B — RATE: the interval between rising midpoint crossings against `sr / rate`.
 *   C — PAN (the `mode` field's own arm): each leg's trough is `1 − mix·depth`, the legs are in
 *       opposition (the left trough lands where the right leg is 1), and one leg is always 1.
 *   D — IDENTITY: `depth = 0`, `mix = 0` and `enabled = 0`, both modes, memcmp-identical on noise.
 *   E — NO LEVEL ADDED: |y| ≤ |x| on noise, both modes, full depth.
 *
 * Pure C, `-lm`, no PipeWire. Built with -DOMX_CONTRACTS: every arm drains the contract log.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/fx/omx_tremolo.h>

static int g_checks = 0, g_failed = 0;
static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s — measured %.9g, limit %.9g\n", what, measured, limit);
  }
}

static void drain_violations(const char *where) {
  uint32_t seen = omx_contract_log.count;
  uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++)
    printf("VIOLATION [%s] %s %s (frame %u)\n", omx_contract_log.rec[i].stage,
           omx_contract_log.rec[i].kind, omx_contract_log.rec[i].token,
           omx_contract_log.rec[i].frame);
  ok(seen == 0u, where, (double)seen, 0.0);
  omx_contract_log.count = 0u;
}

#define BLOCK 256u
static const float LFO_RATES[4] = {0.1f, 1.0f, 5.0f, 20.0f};
static const float DEPTHS[3] = {0.25f, 0.5f, 1.0f};
static const float MIXES[2] = {1.0f, 0.5f};

static struct omx_tremolo atom(float sr, float rate, float depth, float mix, int mode) {
  struct omx_tremolo p;
  p.enabled = 1;
  p.mode = mode;
  p.lfo_inc = omx_lfo_inc(rate, sr);
  p.depth = depth;
  p.mix = mix;
  return p;
}

/* What one run of a DC input over `periods` LFO periods reads back. */
struct reading {
  float min_l, max_l, min_r, max_r;
  double first_rise, last_rise; /* the midpoint's rising crossings, fractional samples */
  uint32_t rises;
  float r_at_l_trough;           /* the right leg where the left one bottoms out */
  float max_of_min_leg;          /* over every frame, 1 − max(gl, gr) at its largest */
};

static struct reading run_dc(const struct omx_tremolo *p, float sr, float rate, double periods) {
  struct omx_tremolo_state s;
  omx_tremolo_state_init(&s);
  const uint64_t frames = (uint64_t)(periods * (double)sr / (double)rate);
  const double mid = 1.0 - (double)p->mix * (double)p->depth * 0.5;
  struct reading out = {2.0f, -1.0f, 2.0f, -1.0f, -1.0, -1.0, 0u, 0.0f, 0.0f};
  float l[BLOCK], r[BLOCK], prev = 1.0f;
  uint64_t at = 0;
  while (at < frames) {
    const uint32_t n = (frames - at) < BLOCK ? (uint32_t)(frames - at) : BLOCK;
    for (uint32_t i = 0; i < n; i++) l[i] = r[i] = 1.0f;
    omx_tremolo_process(l, r, n, p, &s);
    for (uint32_t i = 0; i < n; i++) {
      if (l[i] < out.min_l) { out.min_l = l[i]; out.r_at_l_trough = r[i]; }
      if (l[i] > out.max_l) out.max_l = l[i];
      if (r[i] < out.min_r) out.min_r = r[i];
      if (r[i] > out.max_r) out.max_r = r[i];
      const float top = l[i] > r[i] ? l[i] : r[i];
      if (1.0f - top > out.max_of_min_leg) out.max_of_min_leg = 1.0f - top;
      if (at + i > 0u && (double)prev < mid && (double)l[i] >= mid) {
        const double x = (double)(at + i) - 1.0 + (mid - prev) / ((double)l[i] - prev);
        if (out.rises == 0u) out.first_rise = x;
        out.last_rise = x;
        out.rises++;
      }
      prev = l[i];
    }
    at += n;
  }
  return out;
}

static void arm_a_b_depth_and_rate(void) {
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    for (int k = 0; k < 4; k++)
      for (int d = 0; d < 3; d++)
        for (int m = 0; m < 2; m++) {
          const struct omx_tremolo p = atom(sr, LFO_RATES[k], DEPTHS[d], MIXES[m],
                                            OMX_TREMOLO_MODE_TREMOLO);
          const struct reading g = run_dc(&p, sr, LFO_RATES[k], 3.0);
          const double trough = 1.0 - (double)MIXES[m] * DEPTHS[d];
          char what[160];
          snprintf(what, sizeof what, "A %.0f Hz lfo %.1f depth %.2f mix %.1f: trough = 1 - mix*depth",
                   sr, LFO_RATES[k], DEPTHS[d], MIXES[m]);
          ok(fabs(g.min_l - trough) < 1e-4, what, g.min_l, trough);
          snprintf(what, sizeof what, "A %.0f Hz lfo %.1f depth %.2f mix %.1f: peak = 1",
                   sr, LFO_RATES[k], DEPTHS[d], MIXES[m]);
          ok(fabs(g.max_l - 1.0) < 1e-4 && g.max_l <= 1.0f, what, g.max_l, 1.0);
          snprintf(what, sizeof what, "A %.0f Hz lfo %.1f: both legs carry the one gain",
                   sr, LFO_RATES[k]);
          ok(g.min_l == g.min_r && g.max_l == g.max_r, what, g.min_r, g.min_l);
          /* B — three periods hold three rising crossings (u = 0.5 of each turn). */
          const double period = (double)sr / (double)LFO_RATES[k];
          const double measured = g.rises > 1u ? (g.last_rise - g.first_rise) / (g.rises - 1u) : 0.0;
          snprintf(what, sizeof what, "B %.0f Hz lfo %.1f depth %.2f mix %.1f: period = sr/rate (samples)",
                   sr, LFO_RATES[k], DEPTHS[d], MIXES[m]);
          ok(g.rises == 3u && fabs(measured - period) <= 1e-3 * period, what, measured, period);
        }
    drain_violations("A/B contracts");
  }
}

static void arm_c_pan(void) {
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    for (int k = 0; k < 4; k++)
      for (int d = 0; d < 3; d++) {
        const struct omx_tremolo p = atom(sr, LFO_RATES[k], DEPTHS[d], 1.0f, OMX_TREMOLO_MODE_PAN);
        const struct reading g = run_dc(&p, sr, LFO_RATES[k], 1.25);
        const double trough = 1.0 - DEPTHS[d];
        char what[160];
        snprintf(what, sizeof what, "C %.0f Hz lfo %.1f depth %.2f: each leg's trough = 1 - depth",
                 sr, LFO_RATES[k], DEPTHS[d]);
        ok(fabs(g.min_l - trough) < 1e-4 && fabs(g.min_r - trough) < 1e-4, what, g.min_l, trough);
        snprintf(what, sizeof what, "C %.0f Hz lfo %.1f depth %.2f: the far leg is 1 at the trough",
                 sr, LFO_RATES[k], DEPTHS[d]);
        ok(g.r_at_l_trough == 1.0f, what, g.r_at_l_trough, 1.0);
        snprintf(what, sizeof what, "C %.0f Hz lfo %.1f depth %.2f: one leg is always exactly 1",
                 sr, LFO_RATES[k], DEPTHS[d]);
        ok(g.max_of_min_leg == 0.0f, what, g.max_of_min_leg, 0.0);
      }
    /* The mix folds into the pan's legs the same way: the trough is 1 − mix·depth. */
    const struct omx_tremolo half = atom(sr, 5.0f, 1.0f, 0.5f, OMX_TREMOLO_MODE_PAN);
    const struct reading h = run_dc(&half, sr, 5.0f, 1.25);
    ok(fabs(h.min_l - 0.5) < 1e-4 && fabs(h.min_r - 0.5) < 1e-4,
       "C pan at mix 0.5, depth 1: each leg's trough = 0.5", h.min_l, 0.5);
    drain_violations("C contracts");
  }
}

#define NOISE 4096u
static void noise(float *l, float *r, uint32_t n, uint32_t seed) {
  for (uint32_t i = 0; i < n; i++) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    l[i] = (float)((int32_t)seed) / 2147483648.0f;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    r[i] = (float)((int32_t)seed) / 2147483648.0f;
  }
}

/* Run a whole noise buffer through `p` in blocks; answer whether it came out byte-identical. */
static int identical(const struct omx_tremolo *p, uint32_t seed) {
  static float l[NOISE], r[NOISE], l0[NOISE], r0[NOISE];
  noise(l, r, NOISE, seed);
  memcpy(l0, l, sizeof l);
  memcpy(r0, r, sizeof r);
  struct omx_tremolo_state s;
  omx_tremolo_state_init(&s);
  for (uint32_t at = 0; at < NOISE; at += 97u) {
    const uint32_t n = NOISE - at < 97u ? NOISE - at : 97u;
    omx_tremolo_process(l + at, r + at, n, p, &s);
  }
  return memcmp(l, l0, sizeof l) == 0 && memcmp(r, r0, sizeof r) == 0;
}

static void arm_d_identity(void) {
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    for (int mode = 0; mode < 2; mode++) {
      struct omx_tremolo p = atom(sr, 20.0f, 0.0f, 1.0f, mode);
      ok(identical(&p, 0x9e3779b9u + ri), "D depth = 0 is memcmp-identical", 0, 0);
      p = atom(sr, 20.0f, 1.0f, 0.0f, mode);
      ok(identical(&p, 0x85ebca6bu + ri), "D mix = 0 is memcmp-identical", 0, 0);
      p = atom(sr, 20.0f, 1.0f, 1.0f, mode);
      p.enabled = 0;
      ok(identical(&p, 0xc2b2ae35u + ri), "D enabled = 0 is memcmp-identical", 0, 0);
      /* The control: the same noise at full depth MOVES, so the arm above can fail. */
      p.enabled = 1;
      ok(!identical(&p, 0x27d4eb2fu + ri), "D control: full depth is not identical", 0, 0);
    }
    drain_violations("D contracts");
  }
}

static void arm_e_no_level_added(void) {
  static float l[NOISE], r[NOISE], l0[NOISE], r0[NOISE];
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    for (int mode = 0; mode < 2; mode++) {
      const struct omx_tremolo p = atom(sr, 20.0f, 1.0f, 1.0f, mode);
      struct omx_tremolo_state s;
      omx_tremolo_state_init(&s);
      noise(l, r, NOISE, 0x165667b1u + ri);
      memcpy(l0, l, sizeof l);
      memcpy(r0, r, sizeof r);
      omx_tremolo_process(l, r, NOISE, &p, &s);
      uint32_t over = 0u;
      for (uint32_t i = 0; i < NOISE; i++)
        if (fabsf(l[i]) > fabsf(l0[i]) || fabsf(r[i]) > fabsf(r0[i])) over++;
      ok(over == 0u, "E no sample leaves louder than it came in", over, 0);
    }
    drain_violations("E contracts");
  }
}

int main(void) {
  arm_a_b_depth_and_rate();
  arm_c_pan();
  arm_d_identity();
  arm_e_no_level_added();
  printf("mix_tremolo: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
