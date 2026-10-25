// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The reverb's return (omx_reverb.h's omx_reverb_process_return, omx_reverb_instance.h): early
// reflections, the tail's level, the wet duck and the wet de-esser, at each of the nine oracle rates
// (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  the come-up values (no early reflections, the tail at 0 dB, no duck, no de-esser) are
//      omx_reverb_process bit for bit, every algorithm;
//   B  the early reflections: with the tail off, an impulse's wet is the tap table, each tap at the
//      pre-delay plus round(REVERB_EARLY_TAP_MS x scale x rate / 1000) samples (+-1) with its
//      REVERB_EARLY_TAP_GAINS gain x the early level (1e-6), even taps left, odd right, at three sizes,
//      the scale running REVERB_EARLY_SIZE_MIN to REVERB_EARLY_SIZE_MAX; the ring holds the longest tap;
//   C  the tail's level: at -6 dB the wet is today's wet x 10^(-6/20) (1e-5 of its peak), and the
//      early reflections at the floor leave it today's, bit for bit;
//   D  the duck: a dry of steady magnitude L brings the wet back at max(duckDb, min(0, T - L)) against
//      the unducked wet (0.01 dB);
//   E  the wet de-esser: a steady sine at deessFreqHz made the whole wet comes out as the declared split
//      transfer g x, g from the ABOVE characteristic (DEESS_KNEE_DB, ratio and floor written out here) of
//      the detector's level, sample for sample within a derived bound, 10 dB over the threshold; 20 dB
//      under it the wet is itself, bit for bit;
//   F  hostile words land inside the declared travels, the ledger is clean, nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_reverb_instance.h>

#include "instance_oracle.h"

#include <float.h>
#include <stdlib.h>

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

/* The declarations, stated here so a moved one turns arm B red (reverb_return-perturb.sh). */
static const double TAP_MS[8] = {3.1, 5.3, 7.9, 10.7, 13.9, 17.3, 21.1, 25.3};
static const double TAP_G[8] = {0.84, 0.75, 0.67, 0.6, 0.53, 0.47, 0.42, 0.37};
#define EARLY_SIZE_MIN 0.5
#define EARLY_SIZE_MAX 1.5

#define MAXN 400000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_al[MAXN], g_ar[MAXN];

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

struct ret {
  float early, tail, size, duck, thr, rel;
  int deess;
  float dfreq, dthr;
};
static const struct ret IDLE = {-60.0f, 0.0f, 50.0f, 0.0f, -30.0f, 250.0f, 0, 7000.0f, -30.0f};

/* Both cuts off (a highcut of 0 is none), so the wet arrives as the network and the taps made it. */
static OmxReverbInstance g_v, g_w;

static void setup(OmxReverbInstance *v, int algo, float predelay, float mix, struct ret x) {
  omx_reverb_instance_init(v, g_sr);
  omx_reverb_instance_resolve(v, 0, 100.0f, mix, 0.6f, 0.4f, 1.0f, predelay, 0.0f, 0.0f, 300.0f, 100.0f, 200.0f,
                              -40.0f, algo, x.early, x.tail, x.size, x.duck, x.thr, x.rel, x.deess, x.dfreq, x.dthr);
  drain();
}

static void run(OmxReverbInstance *v, const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 256u) {
    omx_reverb_instance_run(v, il + o, ir + o, ol + o, orr + o, n - o < 256u ? n - o : 256u);
    drain();
  }
}

/* ---- A: the come-up values ---------------------------------------------------------------- */
static void arm_identity(void) {
  g_arm = "A";
  const uint32_t n = (uint32_t)(g_sr * 0.4f);
  stimulus(g_l, g_r, n, 0x1234567u, 3000u);
  for (int algo = OMX_REVERB_ROOM; algo <= OMX_REVERB_GATED; algo++) {
    setup(&g_v, algo, 12.0f, 0.4f, IDLE);
    run(&g_v, g_l, g_r, g_ol, g_or, n);
    memcpy(g_al, g_l, n * sizeof(float));
    memcpy(g_ar, g_r, n * sizeof(float));
    omx_reverb_instance_init(&g_w, g_sr);
    omx_reverb_instance_resolve(&g_w, 0, 100.0f, 0.4f, 0.6f, 0.4f, 1.0f, 12.0f, 0.0f, 0.0f, 300.0f, 100.0f, 200.0f,
                                -40.0f, algo, IDLE.early, IDLE.tail, IDLE.size, IDLE.duck, IDLE.thr, IDLE.rel, IDLE.deess,
                                IDLE.dfreq, IDLE.dthr);
    drain();
    for (uint32_t o = 0; o < n; o += 256u) {
      omx_reverb_process(g_al + o, g_ar + o, n - o < 256u ? n - o : 256u, &g_w.atom, &g_w.state, g_sr);
      drain();
    }
    ok(memcmp(g_ol, g_al, n * sizeof(float)) == 0 && memcmp(g_or, g_ar, n * sizeof(float)) == 0,
       "the return at its come-up values is omx_reverb_process, bit for bit", algo, 0);
  }
  clean();
}

/* ---- B: the early reflections --------------------------------------------------------------- */
static void arm_early(void) {
  g_arm = "B";
  static const float SIZE[3] = {0.0f, 50.0f, 100.0f};
  const float predelay = 7.0f;
  for (int z = 0; z < 3; z++) {
    struct ret x = IDLE;
    x.early = -3.0f;
    x.tail = -60.0f;
    x.size = SIZE[z];
    setup(&g_v, OMX_REVERB_ROOM, predelay, 1.0f, x);
    const uint32_t n = (uint32_t)(g_sr * 0.08f);
    memset(g_l, 0, n * sizeof(float));
    g_l[0] = 1.0f;
    memcpy(g_r, g_l, n * sizeof(float));
    run(&g_v, g_l, g_r, g_ol, g_or, n);
    const double scale = EARLY_SIZE_MIN + (EARLY_SIZE_MAX - EARLY_SIZE_MIN) * SIZE[z] / 100.0, lin = pow(10.0, -3.0 / 20.0);
    const uint32_t pre = (uint32_t)(predelay * 0.001f * g_sr);
    int hits = 0, where = 1, gains = 1;
    for (int t = 0; t < 8; t++) {
      const float *y = (t & 1) ? g_or : g_ol;
      const double at = pre + TAP_MS[t] * scale * g_sr / 1000.0;
      uint32_t best = 0;
      for (uint32_t i = 1; i < n; i++)
        if (fabsf(y[i]) > fabsf(y[best]) && fabs((double)i - at) <= 1.5) best = i;
      where &= fabs((double)best - at) <= 1.0;
      gains &= fabs(y[best] - TAP_G[t] * lin) <= 1e-6;
      hits += y[best] != 0.0f;
    }
    ok(where && hits == 8, "each early tap lands at the pre-delay plus its table time x scale (+-1 sample)", hits, 8);
    ok(gains, "each early tap carries its table gain x the early level (1e-6)", gains, 1);
    /* everything off the taps is silence: no tail at its floor */
    uint32_t stray = 0;
    for (uint32_t i = 0; i < n; i++) {
      int on_tap = 0;
      for (int t = 0; t < 8; t++) on_tap |= fabs((double)i - (pre + TAP_MS[t] * scale * g_sr / 1000.0)) <= 1.0;
      stray += !on_tap && (g_ol[i] != 0.0f || g_or[i] != 0.0f);
    }
    ok(stray == 0, "with the tail at its floor the wet is the taps alone", stray, 0);
  }
  ok(TAP_MS[7] * EARLY_SIZE_MAX * 0.001 * OMX_REVERB_MAX_RATE < (double)OMX_REVERB_INSTANCE_EARLY_CAP,
     "the early ring holds the longest tap at the largest size at the highest rate", TAP_MS[7], 0);
  clean();
}

/* ---- C: the tail's level ------------------------------------------------------------------ */
static void arm_tail(void) {
  g_arm = "C";
  const uint32_t n = (uint32_t)(g_sr * 0.3f);
  stimulus(g_l, g_r, n, 0x2468u, 5000u);
  struct ret x = IDLE;
  x.tail = -6.0f;
  setup(&g_v, OMX_REVERB_HALL, 0.0f, 1.0f, x);
  run(&g_v, g_l, g_r, g_ol, g_or, n);
  setup(&g_w, OMX_REVERB_HALL, 0.0f, 1.0f, IDLE);
  run(&g_w, g_l, g_r, g_al, g_ar, n);
  const double g = pow(10.0, -6.0 / 20.0);
  double peak = 0.0, err = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    peak = fmax(peak, fabs(g_al[i]));
    err = fmax(err, fabs(g_ol[i] - g * g_al[i]));
  }
  ok(err <= 1e-5 * peak, "the tail at -6 dB is today's wet x 10^(-6/20) (1e-5 of its peak)", err, 1e-5 * peak);
  clean();
}

/* ---- D: the duck -------------------------------------------------------------------------- */
static void arm_duck(void) {
  g_arm = "D";
  static const double LEVEL_DB[3] = {-36.0, -20.0, -6.0};
  const uint32_t n = (uint32_t)(g_sr * 0.6f);
  for (int k = 0; k < 3; k++) {
    const double a = pow(10.0, LEVEL_DB[k] / 20.0);
    for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)((i & 1u) ? -a : a);
    struct ret x = IDLE;
    x.duck = -12.0f;
    setup(&g_v, OMX_REVERB_ROOM, 0.0f, 1.0f, x);
    run(&g_v, g_l, g_r, g_ol, g_or, n);
    setup(&g_w, OMX_REVERB_ROOM, 0.0f, 1.0f, IDLE);
    run(&g_w, g_l, g_r, g_al, g_ar, n);
    double eo = 0.0, ea = 0.0;
    for (uint32_t i = n / 2u; i < n; i++) {
      eo += (double)g_ol[i] * g_ol[i];
      ea += (double)g_al[i] * g_al[i];
    }
    const double got = 10.0 * log10(eo / ea), want = fmax(-12.0, fmin(0.0, -30.0 - LEVEL_DB[k]));
    ok(fabs(got - want) <= 0.01, "the wet comes back at max(depth, min(0, T - L)) (0.01 dB)", got, want);
  }
  clean();
}

/* ---- E: the wet de-esser ------------------------------------------------------------------ */
/* The de-esser's declared transfer (omx_deesser.h), split: y = x + (g - 1) B, B the cookbook bandpass
 * at deessFreqHz, g = 10^(d/20), d the ABOVE characteristic of the band's peak level L in dB, soft knee
 * DEESS_KNEE_DB, ratio DEESS_RATIO, floored at DEESS_RANGE, all written out here. The wet is made one
 * steady sine on both legs (width 0, the tail at its floor, the early taps alone, a sine at the band's
 * centre, where the cookbook bandpass passes it whole and its notch complement has a zero), so B = x
 * once the notch's transient has died and y = g x. L is read from the detector's state after each
 * sample, and the de-essed wet is compared, sample by sample, with g x, x the wet with the de-esser off.
 * The tolerance is derived: the level in dB is within 1e-4 dB (omx_lin_to_db_poly), which moves d by
 * at most (1 - 1/ratio) 1e-4 dB, the gain back to linear is within 2e-6 relative (omx_db_to_lin_poly),
 * so g is within 2e-5 relative; the notch's residual is at most A r^m, r the section's pole radius and
 * m the samples since the last tap came in, the float section adds (1 - g) A 8 eps / (1 - r), and y's
 * two float operations add 2 ulp of A. */
#define DEESS_KNEE_DB 6.0
#define DEESS_RATIO 4.0
#define DEESS_RANGE -12.0
static double deess_gain(double level_db, double thr) {
  const double x = level_db - thr, half = 0.5 * DEESS_KNEE_DB, slope = 1.0 / DEESS_RATIO - 1.0;
  double d = x <= -half ? 0.0 : x < half ? slope * (x + half) * (x + half) / (2.0 * DEESS_KNEE_DB) : slope * x;
  d = d < DEESS_RANGE ? DEESS_RANGE : d;
  return pow(10.0, d / 20.0);
}

static void arm_deess(void) {
  g_arm = "E";
  const double f0 = 6000.0, amp = 0.5;
  const uint32_t n = (uint32_t)(g_sr * 0.12f);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)(amp * sin(2.0 * M_PI * f0 * i / g_sr));
  /* the cookbook bandpass at f0, one octave: its poles' radius sqrt(a2) */
  const double w0 = 2.0 * M_PI * f0 / g_sr, alpha = sin(w0) * sinh(0.5 * log(2.0) * 1.0 * w0 / sin(w0));
  const double radius = sqrt((1.0 - alpha) / (1.0 + alpha));
  /* the float section's own error on B, relative to A: its rounding, a few ulp a sample, summed by the
   * recursion's gain 1 / (1 - r) */
  const double band_err = 8.0 * FLT_EPSILON / (1.0 - radius);
  const uint32_t last_tap = (uint32_t)(5.0f * 0.001f * g_sr) + (uint32_t)ceil(25.3 * 1.0 * 0.001 * g_sr) + 2u;
  /* two thresholds: 10 dB under the wet's level (on the ratio, above the knee, over the floor) and 20 dB
   * over it (below the knee: g = 1, the wet whole) */
  for (int pass = 0; pass < 2; pass++) {
    struct ret x = IDLE;
    x.early = 0.0f;
    x.tail = -60.0f;
    x.deess = (int)OMX_REVERB_WET_DEESSES_ON;
    x.dfreq = (float)f0;
    x.dthr = pass ? 0.0f : -60.0f;
    omx_reverb_instance_init(&g_w, g_sr);
    omx_reverb_instance_resolve(&g_w, 0, 100.0f, 1.0f, 0.6f, 0.4f, 0.0f, 5.0f, 0.0f, 0.0f, 300.0f, 100.0f, 200.0f, -40.0f,
                                OMX_REVERB_ROOM, x.early, x.tail, 50.0f, IDLE.duck, IDLE.thr, IDLE.rel, 0, x.dfreq, x.dthr);
    drain();
    for (uint32_t i = 0; i < n; i++) {
      omx_reverb_instance_run(&g_w, g_l + i, g_r + i, g_al + i, g_ar + i, 1u);
      drain();
    }
    drain();
    double peak = 0.0;
    for (uint32_t i = last_tap; i < n; i++) peak = fmax(peak, fabs(g_al[i]));
    const double thr = pass ? 20.0 * log10(peak) + 20.0 : 20.0 * log10(peak) - 10.0;
    x.dthr = (float)thr;
    omx_reverb_instance_init(&g_v, g_sr);
    omx_reverb_instance_resolve(&g_v, 0, 100.0f, 1.0f, 0.6f, 0.4f, 0.0f, 5.0f, 0.0f, 0.0f, 300.0f, 100.0f, 200.0f, -40.0f,
                                OMX_REVERB_ROOM, x.early, x.tail, 50.0f, IDLE.duck, IDLE.thr, IDLE.rel, x.deess, x.dfreq, x.dthr);
    drain();
    double worst = 0.0, gmin = 1.0;
    uint32_t over = 0u, same = 1u;
    for (uint32_t i = 0; i < n; i++) {
      omx_reverb_instance_run(&g_v, g_l + i, g_r + i, g_ol + i, g_or + i, 1u);
      drain();
      const double level = g_v.rstate.deess.band.env.stage[OMX_DYN_ENV_STAGES - 1];
      const double g = deess_gain(20.0 * log10(level > 1e-9 ? level : 1e-9), (double)x.dthr);
      gmin = fmin(gmin, g);
      same &= g_ol[i] == g_or[i];
      if (pass) same &= g_ol[i] == g_al[i];
      if (i < last_tap + 8u) continue;
      const double tol = 2e-5 * fabs(g_al[i]) + (1.0 - g) * peak * (pow(radius, (double)(i - last_tap)) + band_err) +
                         2.0 * FLT_EPSILON * peak;
      const double e = fabs((double)g_ol[i] - g * g_al[i]);
      worst = fmax(worst, e / tol);
      over += !(e <= tol);
    }
    drain();
    if (pass)
      ok(same, "a wet 20 dB under the threshold is the wet itself, bit for bit", same, 1);
    else {
      printf("  %6.0f Hz: E de-ess gain %.3f dB, worst error over its bound %.3f\n", (double)g_sr, 20.0 * log10(gmin), worst);
      ok(over == 0u, "the de-essed wet is the declared transfer g x of the wet (derived bound)", over, 0);
      ok(gmin < pow(10.0, -6.0 / 20.0), "10 dB over the threshold the de-esser takes at least 6 dB", 20.0 * log10(gmin), -6.0);
    }
  }
  clean();
}

/* ---- F: hostile words, no allocation ------------------------------------------------------ */
static void arm_hostile(void) {
  g_arm = "F";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  for (unsigned h = 0; h < sizeof W / sizeof W[0]; h++) {
    const struct ret x = {W[h], W[h], W[h], W[h], W[h], W[h], 7, W[h], W[h]};
    setup(&g_v, OMX_REVERB_ROOM, 5.0f, 0.5f, x);
    const struct omx_reverb_return *r = &g_v.ret;
    int inside = r->early_lin >= 0.0f && r->early_lin <= 1.0f && r->tail_lin >= 0.0f && r->tail_lin <= 1.0f &&
                 r->duck.depth_db >= OMX_REVERB_DUCK_DB_RANGE_MIN && r->duck.depth_db <= 0.0f && r->deess == NULL;
    for (int t = 0; t < 8; t++) inside &= r->tap_d[t] < g_v.rstate.cap;
    ok(inside, "hostile words: every level, tap and choice at a declared value", h, 0);
    stimulus(g_l, g_r, 4096u, 0x55u, 300u);
    run(&g_v, g_l, g_r, g_ol, g_or, 4096u);
  }
  g_allocs = 0;
  g_counting = 1;
  struct ret x = IDLE;
  x.early = -6.0f;
  x.tail = -3.0f;
  x.duck = -9.0f;
  x.deess = 1;
  setup(&g_v, OMX_REVERB_GATED, 10.0f, 0.5f, x);
  run(&g_v, g_l, g_r, g_ol, g_or, 4096u);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0);
  clean();
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_fx_require_full_rates_cover_declared();
  (void)sine;
  (void)rms;
  (void)expect_clean;
  for (unsigned r = 0; r < OMX_FX_ORACLE_RATE_COUNT; r++) {
    g_sr = OMX_FX_ORACLE_RATES[r];
    arm_identity();
    arm_early();
    arm_tail();
    arm_duck();
    arm_deess();
    arm_hostile();
  }
  return instance_oracle_end("reverb_return");
}
