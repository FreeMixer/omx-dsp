// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The delay's tempo sync, colour modes and wet duck (omx_delay.h, omx_delay_instance.h) at each of the
// nine oracle rates (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  the come-up values (free, digital, no duck) are omx_fx_delay_process bit for bit, ping-pong and
//      feedback engaged, and the kernel's colour path with a NULL colour is the taps path bit for bit;
//   B  tempo: the first echo of an impulse lands at round(60000 / BPM x beats x rate / 1000) samples,
//      to the sample, for every division at three tempos, held to the time travel's top;
//   C  tape: the k-th repeat's magnitude at 1 kHz is (feedback |LP(f)|)^k, LP the one-pole at
//      DELAY_TAPE_LP_HZ, within 0.005 dB plus the wow's Doppler for its k + 1 passes, and below it by at
//      most the linear read's loss at a half sample, k = 0..4;
//   D  tape's wow: the echo's delay of a steady sine swings from the time to the time plus
//      DELAY_TAPE_WOW_MS, within the delay's top slope pi depth rate over one 10 ms measuring window,
//      and its swings are 1 / DELAY_TAPE_WOW_HZ apart within two windows;
//   E  analog: small signals repeat at (feedback |LP(f)|)^k with LP at DELAY_ANALOG_LP_HZ (0.005 dB); a
//      held input regenerates to the saturated loop's fixed point, the root of y^2 - fb y - 1, where
//      tape's reaches 1 / (1 - fb) (0.1 %); an inverted input is the inverted output, bit for bit;
//   F  duck: a dry of steady magnitude L comes back at max(duckDb, min(0, T - L)) within 0.01 dB, and the
//      follower reaches 1 - 1/e of a step at FX_DUCK_ATTACK_MS;
//   G  hostile words land inside the declared travels and choices, the ledger is clean, and nothing
//      allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_delay_instance.h>

#include "instance_oracle.h"

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

/* The declarations, stated here so a moved one turns its arm red (delay_color-perturb.sh). */
#define TAPE_LP_HZ 6000.0
#define ANALOG_LP_HZ 3000.0
#define WOW_HZ 0.4
#define WOW_MS 0.3
#define FALLBACK_BPM 120.0
#define DUCK_ATTACK_MS 5.0
static const double BEATS[15] = {4, 6, 8.0 / 3.0, 2, 3, 4.0 / 3.0, 1, 1.5, 2.0 / 3.0, 0.5, 0.75, 1.0 / 3.0, 0.25, 0.375, 1.0 / 6.0};

#define MAXN 1200000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_al[MAXN], g_ar[MAXN];

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

struct ctl {
  float time, fb, tone, mix;
  int pingpong, sync, division, mode;
  float duck, thr, rel;
};
static const struct ctl COMEUP = {300.0f, 0.5f, 0.0f, 1.0f, 0, 0, 6, 0, 0.0f, -30.0f, 250.0f};

static OmxDelayInstance g_d;

static void setup(const struct ctl *c) {
  omx_delay_instance_init(&g_d, g_sr);
  omx_delay_instance_resolve(&g_d, 0, c->time, c->fb, c->tone, c->mix, c->pingpong, c->sync, c->division, c->mode,
                             c->duck, c->thr, c->rel);
  drain();
}

static void run(const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 512u) {
    omx_delay_instance_run(&g_d, il + o, ir + o, ol + o, orr + o, n - o < 512u ? n - o : 512u);
    drain();
  }
}

static double mag_at(const float *x, uint32_t n, double f) {
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    re += x[i] * cos(2.0 * M_PI * f * i / g_sr);
    im -= x[i] * sin(2.0 * M_PI * f * i / g_sr);
  }
  return sqrt(re * re + im * im);
}

/** |LP(f)| of the one-pole y = (1 - p) x + p y at the pole the kernel takes for `hz`, in double. */
static double lp_mag(double hz, double f) {
  const double p = exp(-2.0 * M_PI * hz / g_sr), w = 2.0 * M_PI * f / g_sr;
  return (1.0 - p) / sqrt(1.0 - 2.0 * p * cos(w) + p * p);
}

/* ---- A: the come-up values ------------------------------------------------------------- */
static void arm_identity(void) {
  g_arm = "A";
  const uint32_t n = (uint32_t)(g_sr * 0.6f);
  stimulus(g_l, g_r, n, 0x1234567u, 4000u);
  struct ctl c = COMEUP;
  c.time = 37.0f;
  c.fb = 0.7f;
  c.tone = 0.4f;
  c.mix = 0.6f;
  c.pingpong = 1;
  setup(&c);
  run(g_l, g_r, g_ol, g_or, n);
  static OmxDelayInstance r;
  omx_delay_instance_init(&r, g_sr);
  omx_delay_instance_resolve(&r, 0, c.time, c.fb, c.tone, c.mix, c.pingpong, 0, 6, 0, 0.0f, -30.0f, 250.0f);
  memcpy(g_al, g_l, n * sizeof(float));
  memcpy(g_ar, g_r, n * sizeof(float));
  for (uint32_t o = 0; o < n; o += 512u) {
    omx_fx_delay_process(g_al + o, g_ar + o, n - o < 512u ? n - o : 512u, &r.atom, &r.state, g_sr);
    drain();
  }
  ok(memcmp(g_ol, g_al, n * sizeof(float)) == 0 && memcmp(g_or, g_ar, n * sizeof(float)) == 0,
     "free, digital, no duck: the face is omx_fx_delay_process bit for bit", 0, 0);
  clean();
}

/* ---- B: tempo -------------------------------------------------------------------------- */
static void arm_tempo(void) {
  g_arm = "B";
  static const float BPM[3] = {90.0f, 120.0f, 137.5f};
  for (int b = 0; b < 3; b++)
    for (int dv = 0; dv < 15; dv++) {
      struct ctl c = COMEUP;
      c.sync = OMX_DELAY_SYNCS_TEMPO;
      c.division = dv;
      c.fb = 0.0f;
      omx_delay_instance_init(&g_d, g_sr);
      omx_delay_instance_set_tempo(&g_d, BPM[b]);
      omx_delay_instance_resolve(&g_d, 0, c.time, c.fb, c.tone, c.mix, 0, c.sync, c.division, 0, 0.0f, -30.0f, 250.0f);
      drain();
      double ms = 60000.0 / BPM[b] * BEATS[dv];
      ms = ms < OMX_FX_DELAY_TIME_RANGE_MAX ? ms : OMX_FX_DELAY_TIME_RANGE_MAX;
      const uint32_t want = (uint32_t)floor(ms * g_sr / 1000.0 + 0.5);
      ok(g_d.atom.d_l == want && g_d.atom.d_r == want, "the synced time is 60000 / BPM x beats, to the sample", g_d.atom.d_l,
         want);
    }
  /* With no tempo handed, the fallback; the impulse's echo lands where the atom says. */
  struct ctl c = COMEUP;
  c.sync = OMX_DELAY_SYNCS_TEMPO;
  c.division = 9; /* an eighth */
  c.fb = 0.0f;
  setup(&c);
  const uint32_t want = (uint32_t)floor(60000.0 / FALLBACK_BPM * 0.5 * g_sr / 1000.0 + 0.5);
  const uint32_t n = want + 64u;
  memset(g_l, 0, n * sizeof(float));
  g_l[0] = 1.0f;
  memcpy(g_r, g_l, n * sizeof(float));
  run(g_l, g_r, g_ol, g_or, n);
  uint32_t at = 0;
  while (at < n && g_ol[at] == 0.0f) at++;
  ok(at == want && g_ol[at] == 1.0f, "with no tempo handed the fallback's eighth: the echo at its sample", at, want);
  clean();
}

/* ---- C and E: the repeats' magnitude ----------------------------------------------------- */
static double repeats(int mode, double lp_hz, float amp, double *worst) {
  struct ctl c = COMEUP;
  c.time = 100.0f;
  c.fb = 0.6f;
  c.mode = mode;
  setup(&c);
  const uint32_t d = g_d.atom.d_l, n = 6u * d;
  memset(g_l, 0, n * sizeof(float));
  g_l[0] = amp;
  memcpy(g_r, g_l, n * sizeof(float));
  run(g_l, g_r, g_ol, g_or, n);
  const double f = 1000.0;
  double w = 0.0;
  for (int k = 0; k < 5; k++) {
    const double got = 20.0 * log10(mag_at(g_ol + (uint32_t)(k + 1) * d - d / 4u, d, f) / amp);
    const double want = k * 20.0 * log10(0.6 * lp_mag(lp_hz, f));
    /* Tape's head moves: a read whose delay changes by s samples a sample stretches what it reads by
     * 1 / (1 - s), the wow's Doppler. Its slope is at most pi x depth x rate_wow / sr samples a sample,
     * so each pass may move the repeat by up to 20 log10(1 / (1 - s)) dB beside the closed form. */
    const double smax = mode == OMX_DELAY_MODES_TAPE ? M_PI * WOW_MS * 0.001 * WOW_HZ : 0.0;
    const double tol = 0.005 + (k + 1) * 20.0 * log10(1.0 / (1.0 - smax));
    /* and tape's linear read between whole samples loses at most |cos(pi f / sr)| a pass, at a half
     * sample, so a repeat may sit that much lower and never higher */
    const double interp = mode == OMX_DELAY_MODES_TAPE ? (k + 1) * -20.0 * log10(cos(M_PI * f / g_sr)) : 0.0;
    w = fmax(w, fabs(got - want));
    ok(got <= want + tol && got >= want - tol - interp, mode == OMX_DELAY_MODES_TAPE ? "tape: the k-th repeat is (fb |LP|)^k, within its passes' Doppler"
                                                             : "analog: the k-th repeat is (fb |LP|)^k (0.005 dB)",
       got, want);
  }
  *worst = fmax(*worst, w);
  return w;
}

static void arm_tape(void) {
  g_arm = "C";
  double worst = 0.0;
  repeats(OMX_DELAY_MODES_TAPE, TAPE_LP_HZ, 0.5f, &worst);
  printf("  %6.0f Hz: C tape repeats worst %.4f dB\n", (double)g_sr, worst);
  clean();
}

/* ---- D: the wow --------------------------------------------------------------------------- */
static void arm_wow(void) {
  g_arm = "D";
  struct ctl c = COMEUP;
  c.time = 50.0f;
  c.fb = 0.0f;
  c.mode = OMX_DELAY_MODES_TAPE;
  setup(&c);
  const double f = 400.0;
  const uint32_t n = (uint32_t)(g_sr * 6.0f);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)(0.5 * sin(2.0 * M_PI * f * i / g_sr));
  run(g_l, g_r, g_ol, g_or, n);
  /* The echo's delay per 10 ms window, from its phase against the dry's: inside one period of f. */
  const uint32_t win = (uint32_t)(g_sr / 100.0f), d = g_d.atom.d_l;
  double lo = 1e9, hi = -1e9, t_lo1 = -1, t_lo2 = -1, prev = 0;
  int falling = 0;
  for (uint32_t o = (uint32_t)(g_sr * 0.5f); o + win <= n; o += win) {
    double rw = 0, iw = 0, rd = 0, id = 0;
    for (uint32_t i = o; i < o + win; i++) {
      rw += g_ol[i] * cos(2.0 * M_PI * f * i / g_sr);
      iw -= g_ol[i] * sin(2.0 * M_PI * f * i / g_sr);
      rd += g_l[i] * cos(2.0 * M_PI * f * i / g_sr);
      id -= g_l[i] * sin(2.0 * M_PI * f * i / g_sr);
    }
    double ph = atan2(id, rd) - atan2(iw, rw);
    const double base = 2.0 * M_PI * f * d / g_sr;
    while (ph - base > M_PI) ph -= 2.0 * M_PI;
    while (ph - base < -M_PI) ph += 2.0 * M_PI;
    const double delay = ph * g_sr / (2.0 * M_PI * f) - d; /* samples past the time */
    lo = fmin(lo, delay);
    hi = fmax(hi, delay);
    /* the times of the delay's minima: a fall that turns into a rise */
    if (o > (uint32_t)(g_sr * 0.5f)) {
      if (delay < prev) falling = 1;
      else if (falling) {
        if (t_lo1 < 0) t_lo1 = (double)o / g_sr;
        else if (t_lo2 < 0) t_lo2 = (double)o / g_sr;
        falling = 0;
      }
    }
    prev = delay;
  }
  /* The tolerances, derived as arm C's Doppler is: the delay swings 0 to depth, so it moves at most
   * smax = pi depth WOW_HZ samples a second. Each measurement is the delay averaged over one 10 ms
   * window, which reads an extreme at most smax T short; the phase read adds 0.02 samples. A minimum's
   * time is known to one window, so two of them, 1 / WOW_HZ apart, to two windows. */
  const double depth = WOW_MS * 0.001 * g_sr, tw = (double)win / g_sr, smax = M_PI * depth * WOW_HZ;
  const double swing_tol = smax * tw + 0.02, period_tol = 2.0 * tw;
  ok(fabs(lo) <= swing_tol && fabs(hi - depth) <= swing_tol,
     "the wow swings the echo from the time to the time plus its depth (pi depth rate over one window)",
     fmax(fabs(lo), fabs(hi - depth)), swing_tol);
  ok(t_lo2 > 0 && fabs((t_lo2 - t_lo1) - 1.0 / WOW_HZ) <= period_tol, "the wow's swings are 1 / its rate apart (two windows)",
     t_lo2 - t_lo1, 1.0 / WOW_HZ);
  clean();
}

/* ---- E: analog --------------------------------------------------------------------------- */
static void arm_analog(void) {
  g_arm = "E";
  double worst = 0.0;
  repeats(OMX_DELAY_MODES_ANALOG, ANALOG_LP_HZ, 1e-4f, &worst);
  printf("  %6.0f Hz: E analog small-signal repeats worst %.4f dB\n", (double)g_sr, worst);
  /* A held DC input regenerates to the loop's fixed point: y = 1 + fb x/(1 + |x|) at y, the root of
   * y^2 - fb y - 1 = 0 for analog, where tape's linear loop reaches 1 / (1 - fb). */
  for (int md = 0; md < 2; md++) {
    struct ctl c = COMEUP;
    c.time = 2.0f;
    c.fb = 0.95f;
    c.mode = md ? OMX_DELAY_MODES_ANALOG : OMX_DELAY_MODES_TAPE;
    setup(&c);
    const uint32_t n = (uint32_t)(g_sr * 1.0f);
    for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = 1.0f;
    run(g_l, g_r, g_ol, g_or, n);
    const double want = md ? 0.5 * (0.95 + sqrt(0.95 * 0.95 + 4.0)) : 1.0 / (1.0 - 0.95);
    ok(fabs(g_ol[n - 1u] - want) <= 1e-3 * want,
       md ? "analog: a held input regenerates to the saturated loop's fixed point (0.1 %)"
          : "tape: a held input regenerates to 1 / (1 - fb) (0.1 %)",
       g_ol[n - 1u], want);
  }
  struct ctl c = COMEUP;
  c.time = 20.0f;
  c.fb = 0.95f;
  c.mode = OMX_DELAY_MODES_ANALOG;
  setup(&c);
  const uint32_t d = g_d.atom.d_l, n = 20u * d;
  memset(g_l, 0, n * sizeof(float));
  g_l[0] = 1.0f;
  g_l[d / 2u] = -1.0f;
  for (uint32_t i = 0; i < n; i++) g_r[i] = -g_l[i];
  run(g_l, g_r, g_ol, g_or, n);
  int odd = 1;
  for (uint32_t i = 0; i < n; i++) odd &= g_or[i] == -g_ol[i];
  ok(odd, "an inverted input is the inverted output, bit for bit", odd, 1);
  clean();
}

/* ---- F: the duck -------------------------------------------------------------------------- */
static void arm_duck(void) {
  g_arm = "F";
  static const double LEVEL_DB[] = {-40.0, -24.0, -12.0, -3.0};
  for (int dp = 0; dp < 2; dp++)
    for (int k = 0; k < 4; k++) {
      struct ctl c = COMEUP;
      c.time = 5.0f;
      c.fb = 0.0f;
      c.duck = dp ? -9.0f : -24.0f;
      c.thr = -30.0f;
      setup(&c);
      const double a = pow(10.0, LEVEL_DB[k] / 20.0);
      const uint32_t n = (uint32_t)(g_sr * 1.0f);
      /* a key of constant magnitude (the sign alternating), so the peak follower settles on it exactly */
      for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)((i & 1u) ? -a : a);
      run(g_l, g_r, g_ol, g_or, n);
      const double got = 20.0 * log10(fabs(g_ol[n - 1u]) / a);
      const double want = fmax(c.duck, fmin(0.0, c.thr - LEVEL_DB[k]));
      ok(fabs(got - want) <= 0.01, "the wet comes back at max(depth, min(0, T - L)) (0.01 dB)", got, want);
    }
  /* The follower's attack: a key step reaches 1 - 1/e of its level at FX_DUCK_ATTACK_MS (0.5 %). */
  struct ctl c = COMEUP;
  c.time = 5.0f;
  c.fb = 0.0f;
  c.duck = -12.0f;
  setup(&c);
  const uint32_t na = (uint32_t)(DUCK_ATTACK_MS * 0.001 * g_sr + 0.5);
  for (uint32_t i = 0; i < na; i++) g_l[i] = g_r[i] = 0.5f;
  run(g_l, g_r, g_ol, g_or, na);
  const double lvl = g_d.cstate.duck.level, want = 0.5 * (1.0 - exp(-1.0));
  ok(fabs(lvl - want) <= 0.005 * want, "the duck's follower reaches 1 - 1/e at its attack time (0.5 %)", lvl, want);
  clean();
}

/* ---- G: hostile words, no allocation ----------------------------------------------------- */
static void arm_hostile(void) {
  g_arm = "G";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f, 1e-40f}; /* the last: a tempo near 0, omx-dsp#250 */
  for (unsigned h = 0; h < sizeof W / sizeof W[0]; h++) {
    omx_delay_instance_init(&g_d, g_sr);
    omx_delay_instance_set_tempo(&g_d, W[h]);
    omx_delay_instance_resolve(&g_d, 0, W[h], W[h], W[h], W[h], 7, 5, 99, -3, W[h], W[h], W[h]);
    drain();
    const struct omx_fx_delay_color *c = &g_d.color;
    ok(c->mode == OMX_DELAY_MODES_DEFAULT && c->duck.depth_db >= OMX_DELAY_DUCK_DB_RANGE_MIN && c->duck.depth_db <= 0.0f &&
           c->duck.thresh_db >= OMX_DELAY_DUCK_THRESHOLD_DB_RANGE_MIN && c->duck.thresh_db <= 0.0f &&
           g_d.atom.d_l < g_d.state.cap && g_d.bpm > 0.0f && g_d.bpm - g_d.bpm == 0.0f &&
           (W[h] > 0.0f && W[h] - W[h] == 0.0f ? g_d.bpm == W[h] : g_d.bpm == (float)FALLBACK_BPM),
       "hostile words: every travel, choice and the tempo at a declared value", h, 0);
    stimulus(g_l, g_r, 4096u, 0x77u, 300u);
    run(g_l, g_r, g_ol, g_or, 4096u);
  }
  /* A synced time past the travel is held at its top. */
  omx_delay_instance_init(&g_d, g_sr);
  omx_delay_instance_set_tempo(&g_d, 20.0f);
  omx_delay_instance_resolve(&g_d, 0, 300.0f, 0.5f, 0.0f, 1.0f, 0, 1, 1, 0, 0.0f, -30.0f, 250.0f);
  drain();
  ok(g_d.atom.d_l == omx_fxdelay_ms_to_samples((float)OMX_FX_DELAY_TIME_RANGE_MAX, g_sr),
     "a synced time past the travel is held at its top", g_d.atom.d_l, 0);
  g_allocs = 0;
  g_counting = 1;
  struct ctl c = COMEUP;
  c.mode = OMX_DELAY_MODES_TAPE;
  c.duck = -12.0f;
  c.sync = 1;
  setup(&c);
  run(g_l, g_r, g_ol, g_or, 4096u);
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
    arm_tempo();
    arm_tape();
    arm_wow();
    arm_analog();
    arm_duck();
    arm_hostile();
  }
  return instance_oracle_end("delay_color");
}
