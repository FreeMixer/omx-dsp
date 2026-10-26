// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The compressor's key path, hold and ducker (omx_comp_instance.h) at each of the nine oracle rates
// (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  at the come-up values the keyed run is the plain run bit for bit: no key, a key with keySource
//      self, a sidechain with no key connected; and through the engaged key path, the programme as its
//      own sidechain, linked, is the plain run bit for bit;
//   B  a keyed sine pair: the programme's gain follows the static law of the KEY's level, not the
//      programme's, within 0.05 dB, and keyLevelDb reads the key's RMS level within 0.05 dB (attack and
//      release equal, so the RMS cascade is an unbiased mean of the square);
//   C  the key filter's high-pass and low-pass and the tilt are their closed forms at four frequencies
//      per rate (the cookbook high-pass and matched low-pass at Q 1/sqrt 2, and
//      `sqrt((a²Ω² + 1)/(Ω² + a²))`), within 0.01 dB, heard through keyListen;
//   D  keyListen is the key after its filter and tilt, bit for bit against a reference built here, and
//      folded as detectLink says (mid on both legs, side on both, max the mid left and the side right);
//   E  hold: after a key step down the gain stays where it was for holdMs, then releases exactly as the
//      slot without a hold does, holdMs late to the sample;
//   F  mid and side: a pure side signal gives no reduction in mid (the input bit for bit) and the full
//      law in side; max reads the louder of the two;
//   G  duck: the programme's gain is `max(rangeDb, -(K - T)(1 - 1/R))` within 0.05 dB, and a 0 dB range
//      ducks nothing (the input bit for bit);
//   H  hostile control words are clamped into their declared travels and the choices read their
//      defaults, the ledger is clean, and init, resolve and run_keyed allocate nothing.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_comp_instance.h>

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

/** The key tilt's pivot, Hz, KEY_TILT_PIVOT_HZ as declared: stated here so a moved declaration turns
 * arm C red (comp_key-perturb.sh). */
#define PIVOT_HZ 1000.0

#define MAXN 400000u
static float g_pl[MAXN], g_pr[MAXN], g_kl[MAXN], g_kr[MAXN], g_ol[MAXN], g_or[MAXN], g_rl[MAXN], g_rr[MAXN];

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

/** The key path's controls. */
struct key {
  int src;
  float hp, lp, tilt;
  int listen;
  float hold;
  int link, duck;
  float range;
};
static const struct key IDLE = {0, 0.0f, 0.0f, 0.0f, 0, 0.0f, 0, 0, -12.0f};

static void resolve(OmxCompInstance *c, float thr, float ratio, float knee, float att, float rel, struct key k) {
  omx_comp_instance_resolve(c, 0, thr, ratio, knee, att, rel, 0.0f, 100.0f, (int)OMX_COMP_KINDS_COMP, OMX_DYN_OVS_OFF,
                            OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, k.src, k.hp, k.lp, k.tilt, k.listen, k.hold, k.link,
                            k.duck, k.range, (int)OMX_COMP_RELEASE_MODES_FIXED);
  drain();
}

static void run(OmxCompInstance *c, const float *kl, const float *kr, const float *il, const float *ir, float *ol,
                float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 300u) {
    const uint32_t m = n - o < 300u ? n - o : 300u;
    omx_comp_instance_run_keyed(c, kl ? kl + o : NULL, kr ? kr + o : NULL, il + o, ir + o, ol + o, orr + o, m);
    drain();
  }
}

static double db(double x) { return 20.0 * log10(x < 1e-30 ? 1e-30 : x); }

static double tone_mag(const float *x, uint32_t n, double f, double sr) {
  const double w = 2.0 * M_PI * f / sr;
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    re += x[i] * cos(w * i);
    im -= x[i] * sin(w * i);
  }
  return 2.0 * sqrt(re * re + im * im) / n;
}

/** The ABOVE law in double, knee `w` centred on the threshold. */
static double law(double level_db, double thr, double ratio, double w) {
  const double x = level_db - thr, h = 0.5 * w, s = 1.0 / ratio - 1.0;
  if (x <= -h) return 0.0;
  if (w > 0.0 && x < h) return s * (x + h) * (x + h) / (2.0 * w);
  return s * x;
}

/* ---- A: the come-up values ------------------------------------------------------------- */
static void arm_identity(void) {
  g_arm = "A";
  const uint32_t n = 20000u;
  stimulus(g_pl, g_pr, n, 0x1234567u, 3000u);
  stimulus(g_kl, g_kr, n, 0x7654321u, 1000u);
  static OmxCompInstance a, b;
  for (int c = 0; c < 3; c++) {
    omx_comp_instance_init(&a, g_sr);
    omx_comp_instance_init(&b, g_sr);
    struct key k = IDLE;
    k.src = c == 2 ? (int)OMX_COMP_KEY_SOURCES_SIDECHAIN : (int)OMX_COMP_KEY_SOURCES_SELF;
    resolve(&a, -30.0f, 4.0f, 6.0f, 5.0f, 200.0f, k);
    resolve(&b, -30.0f, 4.0f, 6.0f, 5.0f, 200.0f, k);
    run(&a, c == 0 ? NULL : (c == 1 ? g_kl : NULL), c == 1 ? g_kr : NULL, g_pl, g_pr, g_ol, g_or, n);
    for (uint32_t o = 0; o < n; o += 300u) {
      omx_comp_instance_run(&b, g_pl + o, g_pr + o, g_rl + o, g_rr + o, n - o < 300u ? n - o : 300u);
      drain();
    }
    ok(memcmp(g_ol, g_rl, n * sizeof(float)) == 0 && memcmp(g_or, g_rr, n * sizeof(float)) == 0,
       c == 0 ? "no key: run_keyed is run, bit for bit"
              : c == 1 ? "keySource self with a key handed: run_keyed is run, bit for bit"
                       : "sidechain with no key connected: run_keyed is run, bit for bit",
       c, 0);
  }
  /* Through the engaged key path: the sidechain is the programme itself, no edge, flat, linked, so
   * the fold is the louder leg the self detector reads. */
  memcpy(g_kl, g_pl, n * sizeof(float));
  memcpy(g_kr, g_pr, n * sizeof(float));
  omx_comp_instance_init(&a, g_sr);
  omx_comp_instance_init(&b, g_sr);
  struct key k = IDLE;
  k.src = (int)OMX_COMP_KEY_SOURCES_SIDECHAIN;
  resolve(&a, -30.0f, 4.0f, 6.0f, 5.0f, 200.0f, k);
  resolve(&b, -30.0f, 4.0f, 6.0f, 5.0f, 200.0f, IDLE);
  ok(omx_comp_instance_keyed(&a, g_kl), "the sidechain engages the key path", 0, 1);
  run(&a, g_kl, g_kr, g_pl, g_pr, g_ol, g_or, n);
  for (uint32_t o = 0; o < n; o += 300u) {
    omx_comp_instance_run(&b, g_pl + o, g_pr + o, g_rl + o, g_rr + o, n - o < 300u ? n - o : 300u);
    drain();
  }
  ok(memcmp(g_ol, g_rl, n * sizeof(float)) == 0 && memcmp(g_or, g_rr, n * sizeof(float)) == 0,
     "the engaged key path, keyed by the programme itself, linked: the plain run, bit for bit", 0, 0);
  clean();
}

/* ---- B: the law follows the key ---------------------------------------------------------- */
static void arm_keyed_law(void) {
  g_arm = "B";
  static const double KEY_DB[] = {-40.0, -24.0, -12.0, -6.0};
  const uint32_t n = (uint32_t)(g_sr * 1.0f);
  for (unsigned c = 0; c < 4; c++) {
    const double ka = sqrt(2.0) * pow(10.0, KEY_DB[c] / 20.0); /* a sine of RMS level KEY_DB */
    for (uint32_t i = 0; i < n; i++) {
      g_kl[i] = g_kr[i] = (float)(ka * sin(2.0 * M_PI * 1000.0 * i / g_sr));
      g_pl[i] = g_pr[i] = (float)(0.01 * sin(2.0 * M_PI * 250.0 * i / g_sr));
    }
    static OmxCompInstance s;
    omx_comp_instance_init(&s, g_sr);
    struct key k = IDLE;
    k.src = (int)OMX_COMP_KEY_SOURCES_SIDECHAIN;
    resolve(&s, -30.0f, 4.0f, c == 1 ? 6.0f : 0.0f, 50.0f, 50.0f, k);
    run(&s, g_kl, g_kr, g_pl, g_pr, g_ol, g_or, n);
    const uint32_t skip = n / 2u;
    const double got = db(tone_mag(g_ol + skip, n - skip, 250.0, g_sr) / 0.01);
    const double want = law(KEY_DB[c], -30.0, 4.0, c == 1 ? 6.0 : 0.0);
    ok(fabs(got - want) <= 0.05, "the programme's gain is the law at the key's level (0.05 dB)", got, want);
    const double lvl = omx_comp_instance_key_level_db(&s);
    ok(fabs(lvl - KEY_DB[c]) <= 0.05, "keyLevelDb reads the key's RMS level (0.05 dB)", lvl, KEY_DB[c]);
  }
  clean();
}

/* ---- C: the key filter's and the tilt's closed forms ----------------------------------- */
static double section_mag(const double c[5], double f, double sr) {
  const double w = 2.0 * M_PI * f / sr;
  const double nr = c[0] + c[1] * cos(w) + c[2] * cos(2 * w), ni = -c[1] * sin(w) - c[2] * sin(2 * w);
  const double dr = 1.0 + c[3] * cos(w) + c[4] * cos(2 * w), di = -c[3] * sin(w) - c[4] * sin(2 * w);
  return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

static void arm_filters(void) {
  g_arm = "C";
  static const struct {
    float hp, lp, tilt;
  } CASE[] = {{120.0f, 0.0f, 0.0f}, {0.0f, 4000.0f, 0.0f}, {300.0f, 6000.0f, 0.0f}, {0.0f, 0.0f, 4.5f}, {0.0f, 0.0f, -3.0f},
              {80.0f, 9000.0f, 2.0f}};
  static const double FREQ[] = {60.0, 500.0, 2500.0, 8000.0};
  const uint32_t n = (uint32_t)(g_sr * 0.5f);
  double worst = 0.0;
  for (unsigned c = 0; c < sizeof CASE / sizeof CASE[0]; c++)
    for (unsigned q = 0; q < 4; q++) {
      for (uint32_t i = 0; i < n; i++) {
        g_kl[i] = g_kr[i] = (float)(0.25 * sin(2.0 * M_PI * FREQ[q] * i / g_sr));
        g_pl[i] = g_pr[i] = 0.0f;
      }
      static OmxCompInstance s;
      omx_comp_instance_init(&s, g_sr);
      struct key k = IDLE;
      k.src = (int)OMX_COMP_KEY_SOURCES_SIDECHAIN;
      k.hp = CASE[c].hp;
      k.lp = CASE[c].lp;
      k.tilt = CASE[c].tilt;
      k.listen = (int)OMX_COMP_KEY_LISTENS_ON;
      resolve(&s, -30.0f, 4.0f, 6.0f, 5.0f, 200.0f, k);
      run(&s, g_kl, g_kr, g_pl, g_pr, g_ol, g_or, n);
      const uint32_t skip = n / 2u;
      const double got = db(tone_mag(g_ol + skip, n - skip, FREQ[q], g_sr) / 0.25);
      double want = 0.0, hc[5], lc[5];
      if (CASE[c].hp > 0.0f) {
        omx_eq_design(OMX_EQ_HIGHPASS, CASE[c].hp, M_SQRT1_2, 0.0, g_sr, hc);
        want += db(section_mag(hc, FREQ[q], g_sr));
      }
      if (CASE[c].lp > 0.0f) {
        omx_eq_design(OMX_EQ_LOWPASS, CASE[c].lp, M_SQRT1_2, 0.0, g_sr, lc);
        want += db(section_mag(lc, FREQ[q], g_sr));
      }
      if (CASE[c].tilt != 0.0f) {
        const double sg = CASE[c].tilt / (20.0 * log10(2.0)), a2 = (1.0 + sg) / (1.0 - sg);
        const double o = tan(M_PI * FREQ[q] / g_sr) / tan(M_PI * PIVOT_HZ / g_sr);
        want += 10.0 * log10((a2 * o * o + 1.0) / (o * o + a2));
      }
      worst = fmax(worst, fabs(got - want));
      ok(fabs(got - want) <= 0.01, "the key's filter and tilt are their closed forms (0.01 dB)", got, want);
    }
  printf("  %6.0f Hz: C worst key-path gap %.5f dB\n", (double)g_sr, worst);
  clean();
}

/* ---- D: listen, bit for bit ----------------------------------------------------------- */
static void arm_listen(void) {
  g_arm = "D";
  const uint32_t n = 12000u;
  stimulus(g_kl, g_kr, n, 0x2468aceu, 700u);
  stimulus(g_pl, g_pr, n, 0x1357bdfu, 1300u);
  for (int fold = 0; fold < (int)OMX_COMP_DETECT_LINKS_COUNT; fold++) {
    static OmxCompInstance s;
    omx_comp_instance_init(&s, g_sr);
    struct key k = IDLE;
    k.src = (int)OMX_COMP_KEY_SOURCES_SIDECHAIN;
    k.hp = 150.0f;
    k.lp = 7000.0f;
    k.tilt = -2.5f;
    k.listen = (int)OMX_COMP_KEY_LISTENS_ON;
    k.link = fold;
    resolve(&s, -30.0f, 4.0f, 6.0f, 5.0f, 200.0f, k);
    run(&s, g_kl, g_kr, g_pl, g_pr, g_ol, g_or, n);
    double hc[5], lc[5], tc[5] = {0};
    omx_eq_design(OMX_EQ_HIGHPASS, 150.0, M_SQRT1_2, 0.0, g_sr, hc);
    omx_eq_design(OMX_EQ_LOWPASS, 7000.0, M_SQRT1_2, 0.0, g_sr, lc);
    const double sg = -2.5 / OMX_TILT_OCTAVE_DB, a = sqrt((1.0 + sg) / (1.0 - sg)), w = tan(M_PI * PIVOT_HZ / g_sr);
    tc[0] = (a + w) / (1.0 + a * w);
    tc[1] = (w - a) / (1.0 + a * w);
    tc[3] = (a * w - 1.0) / (1.0 + a * w);
    double s1[2][4] = {{0}}, s2[2][4] = {{0}}, s3[2][4] = {{0}};
    int same = 1;
    for (uint32_t i = 0; i < n; i++) {
      const float x = omx_biquad_d(omx_biquad_d(omx_biquad_d(g_kl[i], hc, s1[0]), lc, s2[0]), tc, s3[0]);
      const float y = omx_biquad_d(omx_biquad_d(omx_biquad_d(g_kr[i], hc, s1[1]), lc, s2[1]), tc, s3[1]);
      const float mid = 0.5f * (x + y), side = 0.5f * (x - y);
      const float wl = fold == 0 ? x : fold == 2 ? side : mid;
      const float wr = fold == 0 ? y : fold == 1 ? mid : side;
      same &= g_ol[i] == wl && g_or[i] == wr;
    }
    ok(same, fold == 0 ? "listen, linked: the filtered and tilted key legs, bit for bit"
             : fold == 1 ? "listen, mid: the mid on both legs, bit for bit"
             : fold == 2 ? "listen, side: the side on both legs, bit for bit"
                         : "listen, max: the mid left and the side right, bit for bit",
       fold, 0);
  }
  clean();
}

/* ---- E: hold -------------------------------------------------------------------------- */
static void arm_hold(void) {
  g_arm = "E";
  static const float HOLD_MS[] = {10.0f, 50.0f, 300.0f};
  const uint32_t drop = (uint32_t)(g_sr * 0.4f), n = drop + (uint32_t)(g_sr * 0.8f);
  for (uint32_t i = 0; i < n; i++) {
    g_kl[i] = g_kr[i] = i < drop ? 0.5f : 0.0f;
    g_pl[i] = g_pr[i] = 0.1f;
  }
  static OmxCompInstance r;
  omx_comp_instance_init(&r, g_sr);
  struct key k = IDLE;
  k.src = (int)OMX_COMP_KEY_SOURCES_SIDECHAIN;
  resolve(&r, -30.0f, 4.0f, 0.0f, 5.0f, 100.0f, k);
  run(&r, g_kl, g_kr, g_pl, g_pr, g_rl, g_rr, n);
  for (unsigned h = 0; h < 3; h++) {
    static OmxCompInstance s;
    omx_comp_instance_init(&s, g_sr);
    k.hold = HOLD_MS[h];
    resolve(&s, -30.0f, 4.0f, 0.0f, 5.0f, 100.0f, k);
    run(&s, g_kl, g_kr, g_pl, g_pr, g_ol, g_or, n);
    const uint32_t frames = (uint32_t)(HOLD_MS[h] * g_sr * 0.001f + 0.5f);
    const float held = g_ol[drop - 1u];
    int flat = 1, late = 1;
    for (uint32_t i = drop; i < drop + frames; i++) flat &= g_ol[i] == held;
    for (uint32_t i = drop + frames; i < n; i++) late &= g_ol[i] == g_rl[i - frames];
    ok(memcmp(g_ol, g_rl, drop * sizeof(float)) == 0, "before the drop the held slot is the slot without a hold", HOLD_MS[h], 0);
    ok(flat, "the gain holds, unchanged to the bit, for holdMs after the key drops", HOLD_MS[h], frames);
    ok(late, "after the hold the release is the unheld release, holdMs late to the sample", HOLD_MS[h], frames);
    ok(held < 0.05f, "the hold holds a reduction", held, 0.1);
  }
  clean();
}

/* ---- F: mid, side, max ----------------------------------------------------------------- */
static void arm_mid_side(void) {
  g_arm = "F";
  const uint32_t n = (uint32_t)(g_sr * 0.6f);
  const double amp = sqrt(2.0) * pow(10.0, -10.0 / 20.0);
  for (uint32_t i = 0; i < n; i++) {
    g_pl[i] = (float)(amp * sin(2.0 * M_PI * 1000.0 * i / g_sr));
    g_pr[i] = -g_pl[i];
  }
  for (int fold = 1; fold < (int)OMX_COMP_DETECT_LINKS_COUNT; fold++) {
    static OmxCompInstance s;
    omx_comp_instance_init(&s, g_sr);
    struct key k = IDLE;
    k.link = fold;
    resolve(&s, -30.0f, 4.0f, 0.0f, 50.0f, 50.0f, k);
    run(&s, NULL, NULL, g_pl, g_pr, g_ol, g_or, n);
    if (fold == (int)OMX_COMP_DETECT_LINKS_MID) {
      ok(memcmp(g_ol, g_pl, n * sizeof(float)) == 0 && memcmp(g_or, g_pr, n * sizeof(float)) == 0,
         "mid: a pure side signal is not reduced, the input bit for bit", 0, 0);
    } else {
      const uint32_t skip = n / 2u;
      const double got = db(tone_mag(g_ol + skip, n - skip, 1000.0, g_sr) / amp);
      const double want = law(-10.0, -30.0, 4.0, 0.0);
      ok(fabs(got - want) <= 0.05, fold == (int)OMX_COMP_DETECT_LINKS_SIDE ? "side: the full law on a pure side signal (0.05 dB)"
                                                                          : "max: the louder of mid and side, the side here (0.05 dB)",
         got, want);
    }
  }
  clean();
}

/* ---- G: duck --------------------------------------------------------------------------- */
static void arm_duck(void) {
  g_arm = "G";
  static const double KEY_DB[] = {-26.0, -18.0, -6.0, 0.0};
  static const float RANGE[] = {-12.0f, -24.0f};
  const uint32_t n = (uint32_t)(g_sr * 1.0f);
  for (unsigned r = 0; r < 2; r++)
    for (unsigned c = 0; c < 4; c++) {
      const double ka = sqrt(2.0) * pow(10.0, KEY_DB[c] / 20.0);
      for (uint32_t i = 0; i < n; i++) {
        g_kl[i] = g_kr[i] = (float)(ka * sin(2.0 * M_PI * 1000.0 * i / g_sr));
        g_pl[i] = g_pr[i] = (float)(0.01 * sin(2.0 * M_PI * 250.0 * i / g_sr));
      }
      static OmxCompInstance s;
      omx_comp_instance_init(&s, g_sr);
      struct key k = IDLE;
      k.src = (int)OMX_COMP_KEY_SOURCES_SIDECHAIN;
      k.duck = (int)OMX_COMP_DUCKS_ON;
      k.range = RANGE[r];
      resolve(&s, -30.0f, 4.0f, 0.0f, 50.0f, 50.0f, k);
      run(&s, g_kl, g_kr, g_pl, g_pr, g_ol, g_or, n);
      const uint32_t skip = n / 2u;
      const double got = db(tone_mag(g_ol + skip, n - skip, 250.0, g_sr) / 0.01);
      const double want = fmax((double)RANGE[r], -(KEY_DB[c] + 30.0) * (1.0 - 1.0 / 4.0));
      ok(fabs(got - want) <= 0.05, "duck: the programme's gain is max(range, -(K - T)(1 - 1/R)) (0.05 dB)", got, want);
    }
  static OmxCompInstance z;
  omx_comp_instance_init(&z, g_sr);
  struct key k = IDLE;
  k.src = (int)OMX_COMP_KEY_SOURCES_SIDECHAIN;
  k.duck = (int)OMX_COMP_DUCKS_ON;
  k.range = 0.0f;
  resolve(&z, -30.0f, 4.0f, 0.0f, 20.0f, 400.0f, k);
  run(&z, g_kl, g_kr, g_pl, g_pr, g_ol, g_or, n);
  ok(memcmp(g_ol, g_pl, n * sizeof(float)) == 0, "duck with a 0 dB range ducks nothing, the input bit for bit", 0, 0);
  clean();
}

/* ---- H: hostile words, no allocation ---------------------------------------------------- */
static void arm_hostile(void) {
  g_arm = "H";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  const uint32_t n = 4096u;
  stimulus(g_pl, g_pr, n, 0x99u, 500u);
  stimulus(g_kl, g_kr, n, 0x77u, 300u);
  for (unsigned h = 0; h < sizeof W / sizeof W[0]; h++) {
    static OmxCompInstance s;
    omx_comp_instance_init(&s, g_sr);
    const struct key k = {7, W[h], W[h], W[h], 9, W[h], -4, 5, W[h]};
    resolve(&s, -30.0f, 4.0f, 6.0f, 5.0f, 200.0f, k);
    ok(s.src == 0 && s.listen == 0 && s.fold == (int)OMX_COMP_DETECT_LINKS_DEFAULT && s.atom.floor_db == 0.0f,
       "a choice outside its ids reads as its default", h, 0);
    ok(s.filt[0].hp_hz >= OMX_COMP_KEY_HP_HZ_RANGE_MIN && s.filt[0].hp_hz <= OMX_COMP_KEY_HP_HZ_RANGE_MAX &&
           s.filt[0].lp_hz >= OMX_COMP_KEY_LP_HZ_RANGE_MIN && s.filt[0].lp_hz <= OMX_COMP_KEY_LP_HZ_RANGE_MAX &&
           s.tilt.a > 0.0 && s.atom.hold_frames <= (uint32_t)(OMX_COMP_HOLD_MS_RANGE_MAX * g_sr * 0.001f + 1.0f),
       "every key travel is clamped into its declared range", h, 0);
    run(&s, g_kl, g_kr, g_pl, g_pr, g_ol, g_or, n);
  }
  g_allocs = 0;
  g_counting = 1;
  static OmxCompInstance s;
  omx_comp_instance_init(&s, g_sr);
  struct key k = IDLE;
  k.src = 1;
  k.hp = 100.0f;
  k.lp = 5000.0f;
  k.tilt = 3.0f;
  k.hold = 20.0f;
  k.link = 3;
  k.duck = 1;
  k.listen = 1;
  resolve(&s, -30.0f, 4.0f, 6.0f, 5.0f, 200.0f, k);
  run(&s, g_kl, g_kr, g_pl, g_pr, g_ol, g_or, n);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run_keyed allocate nothing", (double)g_allocs, 0);
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
    arm_keyed_law();
    arm_filters();
    arm_listen();
    arm_hold();
    arm_mid_side();
    arm_duck();
    arm_hostile();
  }
  return instance_oracle_end("comp_key");
}
