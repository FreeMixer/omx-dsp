// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The gate's knee range (omx-contract#21: kneeStartDb, kneeEndDb), through the instance face
// fx/omx_gate_instance.h into omx_gate_knee and the gain computer's omx_gaincomp_db_shifted. At
// every declared rate, contracts on, an empty ledger after each arm:
//   A  neutral: a knee start equal to its end, wherever the pair sits (the travel's floor and
//      roof, the threshold, between, out of travel), resolves to the hard knee (width 0, offset 0)
//      and the run is omx_gate_resolve + omx_gate_run, the gate before the knee range, bit for bit
//      — with hold and hysteresis 0 and with both set;
//   B  closed form: the static curve of a moved knee, read from the resolved atom, is the
//      expander's line at the lower edge, 0 dB at the upper one and the Bezier midpoint
//      `(ratio - 1) xl / 4` at `x = (xl + xh) / 4` — centred, leaning low, leaning high, the edges
//      given high-first, a threshold outside the range (the range moved whole onto it) — and,
//      across the whole range, an independent double-precision Bezier solved by bisection; the
//      join is C1 (the slope either side of each edge agrees) and the curve never falls as the
//      level rises; a centred range is omx_gaincomp_db's own knee of that width;
//   C  live: a steady tone at the lower edge, the midpoint and the upper edge settles to the
//      closed-form gain through the whole kernel (detector, gain computer, gain), and a moved knee
//      changes the gate's output against the hard knee;
//   D  no allocation: init, resolve and run with a knee range, allocators wrapped at link time.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_gate_instance.h>

#include "fx_rates.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- the allocation counter (linked with --wrap=malloc,calloc,realloc,free) ---------------- */

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);

static int g_counting = 0;
static long g_allocs = 0;

void *__wrap_malloc(size_t n) {
  if (g_counting) g_allocs++;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t sz) {
  if (g_counting) g_allocs++;
  return __real_calloc(n, sz);
}
void *__wrap_realloc(void *p, size_t n) {
  if (g_counting) g_allocs++;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  if (g_counting) g_allocs++;
  __real_free(p);
}

static int g_checks = 0, g_failed = 0;
static const char *g_arm = "";

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s — measured %.9g, limit %.9g\n", g_arm, what, measured, limit);
  }
}

static void expect_clean(void) {
  const uint32_t seen = omx_contract_log.count;
  const uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (omx_contract_record_ready(r)) printf("VIOLATION [%s] %s %s\n", r->stage, r->kind, r->token);
  }
  ok(seen == 0u, "no contract violation in this arm", (double)seen, 0.0);
  omx_contract_reset();
}

static uint32_t g_seed;
static float rnd(void) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return (float)(g_seed >> 8) / 16777216.0f * 2.0f - 1.0f;
}

#define N 3000u /* not a multiple of OMX_GATE_CHUNK */

static OmxGateInstance g_inst;
static struct omx_gate g_ref;

static void fill(float *l, float *r, float *k) {
  g_seed = 81u;
  for (uint32_t i = 0; i < N; i++) {
    const float amp = ((i / 700u) % 2u == 0u) ? 0.5f : 0.002f;
    l[i] = amp * rnd();
    r[i] = amp * rnd();
    k[i] = ((i / 500u) % 2u == 0u) ? 0.3f * rnd() : 0.0f;
  }
}

/* ---- the reference: the knee range in double, the Bezier solved by bisection ----------------- */

/** One gate's static curve as the operator states it. */
struct knee_case {
  double t, ks, ke, ratio, range;
};

/** The range's edges relative to the threshold, after moving the range whole onto a threshold
 * outside it: `*xl <= 0 <= *xh`. */
static void ref_edges(const struct knee_case *c, double *xl, double *xh) {
  double lo = c->ks < c->ke ? c->ks : c->ke, hi = c->ks < c->ke ? c->ke : c->ks;
  const double w = hi - lo;
  if (c->t < lo) { lo = c->t; hi = c->t + w; }
  if (c->t > hi) { hi = c->t; lo = c->t - w; }
  *xl = lo - c->t;
  *xh = hi - c->t;
}

/** The gain, dB, at `level` dB: the line, 0 dB, or the Bezier from (xl, (R-1) xl) through the
 * control point (0, 0) to (xh, 0), its parameter found by bisection on x(u) (monotone). */
static double ref_gain(const struct knee_case *c, double level) {
  double xl, xh;
  ref_edges(c, &xl, &xh);
  const double x = level - c->t;
  double g;
  if (x <= xl) g = (c->ratio - 1.0) * x;
  else if (x >= xh) g = 0.0;
  else {
    double a = 0.0, b = 1.0;
    for (int i = 0; i < 80; i++) {
      const double u = 0.5 * (a + b), xu = (1.0 - u) * (1.0 - u) * xl + u * u * xh;
      if (xu < x) a = u;
      else b = u;
    }
    const double u = 0.5 * (a + b);
    g = (1.0 - u) * (1.0 - u) * (c->ratio - 1.0) * xl;
  }
  return g < c->range ? c->range : g;
}

static const struct knee_case CASES[] = {
    {-40.0, -43.0, -37.0, 16.0, -90.0}, /* the desk's default: centred on the threshold */
    {-35.0, -38.0, -30.0, 20.0, -60.0}, /* leaning high (the "tight" preset's shape) */
    {-45.0, -54.0, -42.0, 3.0, -40.0},  /* leaning low */
    {-50.0, -44.0, -56.0, 2.0, -40.0},  /* the edges given high-first */
    {-30.0, -43.0, -37.0, 4.0, -80.0},  /* the threshold above the range: moved up onto it */
    {-60.0, -43.0, -37.0, 4.0, -80.0},  /* the threshold below the range: moved down onto it */
    {-40.0, -60.0, -20.0, 1.5, -24.0},  /* wide and gentle */
};
#define NCASES (sizeof CASES / sizeof CASES[0])

static void resolve_case(const struct knee_case *c, float attack_ms) {
  omx_gate_instance_resolve(&g_inst, 0, 0, (float)c->t, (float)c->range, (float)c->ks, (float)c->ke, attack_ms,
                            0.0f, 50.0f, 0.0f, (float)c->ratio);
}

static float atom_gain_db(float level_db) {
  return omx_gaincomp_db_shifted(&g_inst.atom.gc, g_inst.atom.knee_shift_db, level_db);
}

/* ---- the arms ---------------------------------------------------------------------------------- */

static void arm_neutral(float sr) {
  g_arm = "A neutral";
  static float l[N], r[N], k[N], ol[N], or_[N], el[N], er[N];
  static const float at[] = {-80.0f, 0.0f, -30.0f, -12.5f, -64.0f, 40.0f, -500.0f};
  for (int i = 0; i < 7; i++)
    for (int hh = 0; hh < 2; hh++) {
      const float hold = hh ? 25.0f : 0.0f, hyst = hh ? 6.0f : 0.0f;
      fill(l, r, k);
      omx_gate_instance_init(&g_inst, sr);
      omx_gate_instance_resolve(&g_inst, 0, 1, -30.0f, -90.0f, at[i], at[i], 0.1f, hold, 50.0f, hyst, 16.0f);
      ok(g_inst.atom.gc.knee_db == 0.0f && g_inst.atom.knee_shift_db == 0.0f,
         "a knee start equal to its end is the hard knee: width 0, offset 0", g_inst.atom.gc.knee_db, at[i]);
      omx_gate_instance_run(&g_inst, k, l, r, ol, or_, N);
      const float en = 1.0f, kx = 1.0f, t = -30.0f, ra = 16.0f, rg = -90.0f, a = 0.1f, rl = 50.0f;
      const struct omx_gate_controls c = {&en, &kx, &t, &ra, &rg, &a, &rl};
      omx_gate_init(&g_ref, sr);
      struct omx_dyn p;
      omx_gate_resolve(&g_ref, &c, &p);
      p.hold_frames = omx_dyn_hold_frames(hold, sr);
      p.hyst_db = hyst;
      omx_gate_run(&g_ref, &p, k, &kx, l, r, el, er, N);
      uint32_t mis = 0;
      for (uint32_t j = 0; j < N; j++) mis += ol[j] != el[j] || or_[j] != er[j];
      ok(mis == 0u, "a zero-width knee is the gate before the knee range, bit for bit", mis, at[i]);
    }
}

static void arm_closed_form(float sr) {
  g_arm = "B closed form";
  omx_gate_instance_init(&g_inst, sr);
  for (unsigned ci = 0; ci < NCASES; ci++) {
    const struct knee_case *c = &CASES[ci];
    resolve_case(c, 1.0f);
    double xl, xh;
    ref_edges(c, &xl, &xh);
    ok(fabs((double)g_inst.atom.gc.knee_db - (xh - xl)) < 1e-5, "the knee width is the range's", g_inst.atom.gc.knee_db,
       xh - xl);
    ok(fabs((double)g_inst.atom.knee_shift_db - 0.5 * (xl + xh)) < 1e-5, "the knee centre is the range's, moved onto the threshold",
       g_inst.atom.knee_shift_db, 0.5 * (xl + xh));
    const double tol = 2e-3;
    const double g_lo = atom_gain_db((float)(c->t + xl)), g_hi = atom_gain_db((float)(c->t + xh));
    const double want_lo = (c->ratio - 1.0) * xl < c->range ? c->range : (c->ratio - 1.0) * xl;
    ok(fabs(g_lo - want_lo) < tol, "at the lower edge the gain is the expander's line", g_lo, want_lo);
    ok(fabs(g_hi) < tol, "at the upper edge the gate is open, 0 dB", g_hi, 0.0);
    const double xm = 0.25 * (xl + xh), want_m = 0.25 * (c->ratio - 1.0) * xl;
    const double g_m = atom_gain_db((float)(c->t + xm));
    ok(fabs(g_m - (want_m < c->range ? c->range : want_m)) < tol, "the Bezier midpoint is (ratio - 1) xl / 4", g_m, want_m);
    double worst = 0.0, prev = -1e9;
    int monotone = 1;
    for (int s = 0; s <= 400; s++) {
      const double lv = c->t + xl - 3.0 + (xh - xl + 6.0) * s / 400.0;
      const double g = atom_gain_db((float)lv), d = fabs(g - ref_gain(c, lv));
      if (d > worst) worst = d;
      if (g < prev - 1e-6) monotone = 0;
      prev = g;
    }
    ok(worst < tol, "across the range the curve is the reference Bezier", worst, tol);
    ok(monotone, "the gain never falls as the level rises", monotone, 1);
    /* C1: the slope just inside each edge is the line's (or 0) just outside it. */
    const double h = 1e-2;
    if (xh - xl > 4.0 * h && (c->ratio - 1.0) * (xl - h) > c->range) {
      if (xl < 0.0) {
        const double in = (atom_gain_db((float)(c->t + xl + h)) - atom_gain_db((float)(c->t + xl))) / h;
        ok(fabs(in - (c->ratio - 1.0)) < 0.05 * (c->ratio - 1.0) + 0.05, "the knee meets the line with its slope", in,
           c->ratio - 1.0);
      }
      if (xh > 0.0) {
        const double in = (atom_gain_db((float)(c->t + xh)) - atom_gain_db((float)(c->t + xh - h))) / h;
        ok(fabs(in) < 0.05 * (c->ratio - 1.0) + 0.05, "the knee meets unity flat", in, 0.0);
      }
    }
    if (ci == 0) {
      struct omx_gaincomp_params gc = g_inst.atom.gc;
      uint32_t mis = 0;
      for (int s = 0; s <= 200; s++) {
        const float lv = -50.0f + 0.1f * (float)s;
        mis += omx_gaincomp_db_shifted(&gc, g_inst.atom.knee_shift_db, lv) != omx_gaincomp_db(&gc, lv);
      }
      ok(g_inst.atom.knee_shift_db == 0.0f && mis == 0u, "a centred range is omx_gaincomp_db's own knee, bit for bit", mis, 0);
    }
  }
}

static void arm_live(float sr) {
  g_arm = "C live";
  static float l[N], r[N];
  const uint32_t settle = (uint32_t)(0.5f * sr);
  for (unsigned ci = 0; ci < NCASES; ci++) {
    const struct knee_case *c = &CASES[ci];
    double xl, xh;
    ref_edges(c, &xl, &xh);
    const double points[3] = {xl, 0.25 * (xl + xh), xh};
    for (int pi = 0; pi < 3; pi++) {
      const double level = c->t + points[pi];
      const float amp = (float)pow(10.0, level / 20.0);
      omx_gate_instance_init(&g_inst, sr);
      resolve_case(c, 1.0f);
      float last = 0.0f;
      for (uint32_t done = 0; done < settle;) {
        const uint32_t m = settle - done < N ? settle - done : N;
        for (uint32_t i = 0; i < m; i++) l[i] = r[i] = amp;
        omx_gate_instance_run(&g_inst, NULL, l, r, l, r, m);
        last = l[m - 1];
        done += m;
      }
      const double got = 20.0 * log10((double)last / (double)amp), want = ref_gain(c, level);
      ok(fabs(got - want) < 0.02, "a steady level settles to the closed-form gain through the kernel", got, want);
    }
  }
  /* a moved knee is heard: the default range against a hard knee at the same threshold */
  static float k[N], ol[N], or_[N], el[N], er[N];
  fill(l, r, k);
  omx_gate_instance_init(&g_inst, sr);
  omx_gate_instance_resolve(&g_inst, 0, 0, -40.0f, -90.0f, -40.0f, -40.0f, 1.0f, 0.0f, 50.0f, 0.0f, 4.0f);
  omx_gate_instance_run(&g_inst, NULL, l, r, el, er, N);
  omx_gate_instance_init(&g_inst, sr);
  omx_gate_instance_resolve(&g_inst, 0, 0, -40.0f, -90.0f, -52.0f, -34.0f, 1.0f, 0.0f, 50.0f, 0.0f, 4.0f);
  omx_gate_instance_run(&g_inst, NULL, l, r, ol, or_, N);
  uint32_t moved = 0;
  for (uint32_t j = 0; j < N; j++) moved += ol[j] != el[j];
  ok(moved > 0u, "a knee range changes the gate's output against the hard knee", moved, 0);
}

static void arm_no_alloc(float sr) {
  g_arm = "D no allocation";
  static float l[N], r[N], k[N];
  fill(l, r, k);
  g_allocs = 0;
  g_counting = 1;
  const int ready = omx_gate_instance_init(&g_inst, sr);
  omx_gate_instance_resolve(&g_inst, 0, 1, -35.0f, -90.0f, -38.0f, -30.0f, 0.1f, 25.0f, 50.0f, 6.0f, 16.0f);
  omx_gate_instance_run(&g_inst, k, l, r, l, r, 512u);
  omx_gate_instance_resolve(&g_inst, 0, 0, -35.0f, -90.0f, -50.0f, -30.0f, 2.0f, 0.0f, 50.0f, 0.0f, 16.0f);
  omx_gate_instance_run(&g_inst, NULL, l, r, l, r, 512u);
  g_counting = 0;
  ok(ready && g_allocs == 0, "init, resolve and run with a knee range allocate nothing", (double)g_allocs, 0);
  void *(*volatile alloc)(size_t) = malloc;
  void (*volatile release)(void *) = free;
  g_allocs = 0;
  g_counting = 1;
  release(alloc(16));
  g_counting = 0;
  ok(g_allocs == 2, "the allocation counter counts (a malloc and a free inside the window)", (double)g_allocs, 2);
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    arm_neutral(sr);
    expect_clean();
    arm_closed_form(sr);
    expect_clean();
    arm_live(sr);
    expect_clean();
    arm_no_alloc(sr);
    expect_clean();
  }
  printf("fx/gate_knee: %d checks, %d failed, %u rates\n", g_checks, g_failed, (unsigned)OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
