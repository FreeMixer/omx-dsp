// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// A BELOW-mode slot's flat gain (omx_dyn.h, omx_dyn_flat_bounds and omx_dyn_flat_gain, #56): past
// the open bound and the floor bound the kernel takes a constant instead of the dB path, and that
// constant must be the float the gain computer makes there. Three arms:
//   SELECTOR  over a grid of gate parameter cells (threshold, ratio, range, knee, knee shift,
//             make-up), omx_dyn_flat_gain against omx_gaincomp_gain_shifted, bit for bit, at every
//             float within 4096 ulps of each bound and of each closed-form edge, and over a log
//             sweep from 1e-10 to 1e3;
//   KERNEL    omx_dynamics_keyed, mono, stereo and keyed, peak and RMS, with and without a dry
//             share, against a per-sample reference that runs the gain computer on every sample,
//             at the nine RME rates;
//   CALLS     log10f and powf per 128-sample block with the gate open and on its floor: the flat
//             path calls them a handful of times per block, not once per sample.
// Release flags: the calls are counted through the two names below, ahead of the headers.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned long g_log10f, g_powf;
static float counted_log10f(float x) { g_log10f++; return log10f(x); }
static float counted_powf(float x, float y) { g_powf++; return powf(x, y); }
#define log10f counted_log10f
#define powf counted_powf

#include <omxdsp/omx_dyn.h>

#include "fx_rates.h"

static int g_checks = 0, g_failed = 0;
static const char *g_arm = "";

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s — measured %.9g, limit %.9g\n", g_arm, what, measured, limit);
  }
}

static uint32_t g_seed;
static float rnd(void) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return (float)(g_seed >> 8) / 16777216.0f * 2.0f - 1.0f;
}

static int same_bits(float a, float b) { return memcmp(&a, &b, sizeof a) == 0; }

static struct omx_dyn gate(float thresh, float ratio, float range, float knee, float shift, float makeup) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = OMX_DYN_BELOW;
  d.gc.thresh_db = thresh;
  d.gc.ratio = ratio;
  d.gc.range_db = range;
  d.gc.knee_db = knee;
  d.gc.makeup_lin = makeup;
  d.knee_shift_db = shift;
  d.detect = OMX_DETECT_PEAK;
  d.ovs_mode = OMX_DYN_OVS_OFF;
  return d;
}

/* ---- SELECTOR -------------------------------------------------------------------------------- */

static long g_sel_diff, g_sel_evals;

static void sel_at(const struct omx_dyn_flat *f, const struct omx_dyn *p, float level) {
  if (!(level >= 0.0f) || level - level != 0.0f) return;
  g_sel_evals++;
  const float want = omx_gaincomp_gain_shifted(&p->gc, p->knee_shift_db, level);
  if (!same_bits(omx_dyn_flat_gain(f, p, level), want)) g_sel_diff++;
}

static void sel_around(const struct omx_dyn_flat *f, const struct omx_dyn *p, float centre) {
  if (!(centre >= 0.0f) || centre - centre != 0.0f) return;
  float up = centre, dn = centre;
  sel_at(f, p, centre);
  for (int k = 0; k < 4096; k++) {
    up = nextafterf(up, INFINITY);
    dn = nextafterf(dn, 0.0f);
    sel_at(f, p, up);
    sel_at(f, p, dn);
  }
}

static void arm_selector(void) {
  g_arm = "selector";
  static const float thresh[] = {-80.0f, -50.0f, -30.0f, -10.0f, 0.0f};
  static const float ratio[] = {1.0f, 1.5f, 4.0f, 40.0f, 100.0f};
  static const float range[] = {-90.0f, -40.0f, -6.0f, 0.0f};
  static const float knee[] = {0.0f, 6.0f, 24.0f};
  static const float shift_frac[] = {0.0f, -1.0f, -0.5f, 0.5f, 1.0f};
  static const float makeup[] = {1.0f, 2.0f};
  long cells = 0, with_open = 0, with_floor = 0;
  g_sel_diff = g_sel_evals = 0;
  for (size_t a = 0; a < sizeof thresh / sizeof *thresh; a++)
    for (size_t b = 0; b < sizeof ratio / sizeof *ratio; b++)
      for (size_t c = 0; c < sizeof range / sizeof *range; c++)
        for (size_t d = 0; d < sizeof knee / sizeof *knee; d++)
          for (size_t e = 0; e < sizeof shift_frac / sizeof *shift_frac; e++)
            for (size_t m = 0; m < sizeof makeup / sizeof *makeup; m++) {
              if (knee[d] == 0.0f && e > 0) continue; /* a hard knee has no range to shift */
              const float shift = shift_frac[e] * 0.5f * knee[d];
              const struct omx_dyn p = gate(thresh[a], ratio[b], range[c], knee[d], shift, makeup[m]);
              struct omx_dyn_flat f;
              omx_dyn_flat_bounds(&p, &f);
              cells++;
              with_open += f.hi == f.hi;
              with_floor += f.lo == f.lo;
              sel_around(&f, &p, f.hi);
              sel_around(&f, &p, f.lo);
              const float half = 0.5f * knee[d];
              sel_around(&f, &p, omx_db_to_lin(thresh[a] + shift + half));
              sel_around(&f, &p, omx_db_to_lin(thresh[a] + shift - half));
              if (ratio[b] > 1.0f) sel_around(&f, &p, omx_db_to_lin(thresh[a] + range[c] / (ratio[b] - 1.0f)));
              for (int k = 0; k <= 2048; k++) sel_at(&f, &p, powf(10.0f, -10.0f + 13.0f * (float)k / 2048.0f));
            }
  ok(g_sel_diff == 0, "the selected gain is the gain computer's, bit for bit", (double)g_sel_diff, 0.0);
  /* not vacuous: the bounds are proven in the cells that have them */
  ok(with_open == cells, "every cell proves its open bound", (double)with_open, (double)cells);
  ok(with_floor * 2 > cells, "most cells prove a floor bound", (double)with_floor, (double)cells / 2.0);
  printf("fx/dyn_flat selector: %ld cells (%ld open, %ld floor bounds), %ld levels, %ld differ\n", cells,
         with_open, with_floor, g_sel_evals, g_sel_diff);
}

/* ---- KERNEL ---------------------------------------------------------------------------------- */

#define KN 6144u
#define KB 128u

/* The slot on its base path with every sample through the gain computer: rectify, the cascade,
 * omx_gaincomp_gain_shifted, the dry share, the multiply. */
static void reference(float *l, float *r, const float *key, uint32_t n, const struct omx_dyn *p,
                      struct omx_env *env) {
  const struct omx_env_params ep = omx_dyn_env_params(p);
  float ac, rc;
  omx_env_stage_poles(&ep, 1u, &ac, &rc);
  const int rms = p->detect == OMX_DETECT_RMS;
  for (uint32_t i = 0; i < n; i++) {
    const float kl = key ? key[i] : l[i];
    const float kr = key ? key[i] : (r ? r[i] : l[i]);
    float lev;
    if (rms) {
      const float sl = kl * kl, sr = kr * kr;
      lev = sl > sr ? sl : sr;
    } else {
      const float al = fabsf(kl), ar = fabsf(kr);
      lev = al > ar ? al : ar;
    }
    float g = omx_gaincomp_gain_shifted(&p->gc, p->knee_shift_db, omx_env_step(env, &ep, lev, ac, rc));
    if (p->dry > 0.0f) g = p->dry + (1.0f - p->dry) * g;
    l[i] *= g;
    if (r) r[i] *= g;
  }
}

static void arm_kernel(float sr) {
  g_arm = "kernel";
  static float la[KN], ra[KN], lb[KN], rb[KN], key[KN];
  static const struct { float thresh, ratio, range, knee, shift, dry; } cell[] = {
      {-40.0f, 16.0f, -90.0f, 0.0f, 0.0f, 0.0f},  {-30.0f, 40.0f, -80.0f, 6.0f, 0.0f, 0.0f},
      {-50.0f, 4.0f, -20.0f, 12.0f, -3.0f, 0.0f}, {-20.0f, 2.0f, -40.0f, 24.0f, 12.0f, 0.25f},
      {-60.0f, 100.0f, -6.0f, 6.0f, 0.0f, 0.5f},  {-35.0f, 1.0f, -90.0f, 0.0f, 0.0f, 0.0f},
  };
  long differ = 0, runs = 0;
  for (size_t c = 0; c < sizeof cell / sizeof *cell; c++)
    for (int shape = 0; shape < 3; shape++) /* mono, stereo, keyed */
      for (int det = 0; det < 2; det++) {
        struct omx_dyn p = gate(cell[c].thresh, cell[c].ratio, cell[c].range, cell[c].knee, cell[c].shift, 1.0f);
        p.dry = cell[c].dry;
        p.detect = det ? OMX_DETECT_RMS : OMX_DETECT_PEAK;
        p.attack_ms = 2.0f;
        p.attack_coeff = omx_pole_from_time_ms(2.0f, sr);
        p.release_coeff = omx_pole_from_time_ms(80.0f, sr);
        g_seed = 977u + (uint32_t)(c * 6 + (size_t)shape * 2 + (size_t)det);
        /* loud (open), silence (floor), a slow swell through the knee, near-silence */
        for (uint32_t i = 0; i < KN; i++) {
          const uint32_t seg = i / 1024u;
          const float amp = seg == 0 || seg == 3 ? 0.5f : seg == 1 ? 0.0f : seg == 2 ? (float)(i % 1024u) / 1024.0f * 0.05f : 1e-6f;
          la[i] = amp * rnd();
          ra[i] = amp * rnd();
          key[i] = (seg % 2u == 0u ? 0.3f : 1e-5f) * rnd();
        }
        memcpy(lb, la, sizeof la);
        memcpy(rb, ra, sizeof ra);
        struct omx_dyn_state st;
        omx_dyn_state_init(&st, 1u);
        struct omx_env env;
        memset(&env, 0, sizeof env);
        const float *k = shape == 2 ? key : NULL;
        for (uint32_t off = 0; off < KN; off += KB) {
          omx_dynamics_keyed(la + off, shape ? ra + off : NULL, k ? k + off : NULL, KB, &p, &st);
          reference(lb + off, shape ? rb + off : NULL, k ? k + off : NULL, KB, &p, &env);
        }
        long mis = 0;
        for (uint32_t i = 0; i < KN; i++) mis += !same_bits(la[i], lb[i]) || (shape && !same_bits(ra[i], rb[i]));
        differ += mis;
        runs++;
      }
  ok(differ == 0, "the slot is the per-sample gain computer, bit for bit", (double)differ, 0.0);
  (void)runs;
}

/* ---- CALLS ----------------------------------------------------------------------------------- */

static void arm_calls(float sr) {
  g_arm = "calls";
  static float l[KB];
  for (int open = 0; open <= 1; open++) {
    struct omx_dyn p = gate(-40.0f, 16.0f, -90.0f, 6.0f, 0.0f, 1.0f);
    p.attack_ms = 2.0f;
    p.attack_coeff = omx_pole_from_time_ms(2.0f, sr);
    p.release_coeff = omx_pole_from_time_ms(80.0f, sr);
    struct omx_dyn_state st;
    omx_dyn_state_init(&st, 1u);
    g_seed = 31u;
    /* settle: a second of the signal, so the detector sits open or on the floor */
    const float amp = open ? 0.5f : 0.0f;
    for (uint32_t b = 0; b < (uint32_t)sr / KB; b++) {
      for (uint32_t i = 0; i < KB; i++) l[i] = amp * (0.75f + 0.25f * rnd());
      omx_dynamics_keyed(l, NULL, NULL, KB, &p, &st);
    }
    for (uint32_t i = 0; i < KB; i++) l[i] = amp * (0.75f + 0.25f * rnd());
    g_log10f = g_powf = 0;
    omx_dynamics_keyed(l, NULL, NULL, KB, &p, &st);
    const unsigned long calls = g_log10f + g_powf;
    ok(calls <= 32u, open ? "an open gate calls log10f and powf a handful of times per block"
                          : "a gate on its floor calls log10f and powf a handful of times per block",
       (double)calls, 32.0);
    if (sr == 48000.0f)
      printf("fx/dyn_flat calls: %s, %lu log10f + %lu powf per %u-sample block\n", open ? "open" : "floor",
             g_log10f, g_powf, KB);
  }
}

int main(void) {
  omx_fx_require_rate_floor();
  arm_selector();
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    arm_kernel(sr);
    arm_calls(sr);
  }
  printf("fx/dyn_flat: %d checks, %d failed, %u rates\n", g_checks, g_failed, (unsigned)OMX_FX_RME_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
