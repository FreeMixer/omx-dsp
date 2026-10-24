// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The multiband compressor's instance oracle (omx_mbcomp_instance.h) at each of the nine oracle rates
// (OMX_FX_ORACLE_RATES, 32 to 192 kHz), every band count MBC_MIN_BANDS to MBC_MAX_BANDS:
//   A  with every band bypassed the sum of the LR4 tree is an all-pass: unit magnitude at 50 frequencies
//      per rate within 0.01 dB, at the come-up corners and at corners spread to the travel's ends;
//   B  each band's static law: a steady sine two octaves inside band k, level L, comes out at
//      `Σ g_j |B_j(f)|` with `g_k` the band's hard-knee law `(1/R - 1)(L_k - T)` on the band's RMS level,
//      within 0.02 dB, and band<k>GainReductionDb reads that law within 0.02 dB;
//   C  the corner line: a 16 kHz corner at 32 kHz is designed at MBC_CORNER_MAX_FRACTION of the rate,
//      where the split is -6.0206 dB within 0.001 dB;
//   D  solo: band k alone sums to |B_k(f)| within 0.01 dB; every band soloed is no band soloed, bit for bit;
//   E  mix 0 % is the tree at rest bit for bit whatever the bands compress; the output gain is one
//      multiply by its linear gain, bit for bit; bypass is the identity and moves no state word;
//   F  the declarations: the band count travel and the corner count as declared, the come-up corners
//      MBC_CROSSOVER_DEFAULTS_3 as declared, hostile words clamped and the corners made rising, the
//      ledger clean, and init, resolve and run allocate nothing.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_mbcomp_instance.h>

#include "instance_oracle.h"

#include <complex.h>
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

/* The declarations, stated here and not read off the header, so a moved one turns arm F (or C) red
 * (mbcomp_instance-perturb.sh): MBC_MIN_BANDS, MBC_MAX_BANDS, MBC_MAX_XOVERS, MBC_CORNER_MAX_FRACTION and
 * MBC_CROSSOVER_DEFAULTS_3. */
#define MIN_BANDS 2u
#define MAX_BANDS 5u
#define MAX_XOVERS 4u
#define CORNER_FRACTION 0.45
static const float COME_UP_3[2] = {200.0f, 2000.0f};

#define NB OMX_MBC_MAX_BANDS
#define NX OMX_MBC_MAX_XOVERS
#define MAXN 400000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_al[MAXN], g_ar[MAXN];

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

struct set {
  float bands, hz[NX], thr[NB], ratio[NB], att[NB], rel[NB], mk[NB], mix, out;
  int solo[NB], by[NB];
};

static struct set rest(uint32_t bands) {
  struct set s;
  memset(&s, 0, sizeof s);
  s.bands = (float)bands;
  for (uint32_t k = 0; k < NX; k++) s.hz[k] = NAN;
  for (uint32_t k = 0; k < NB; k++) {
    s.thr[k] = -20.0f;
    s.ratio[k] = 4.0f;
    s.att[k] = 50.0f;
    s.rel[k] = 50.0f;
    s.mk[k] = 0.0f;
    s.by[k] = 1;
  }
  s.mix = 100.0f;
  return s;
}

static void apply(OmxMbcompInstance *m, int bypass, const struct set *s) {
  omx_mbcomp_instance_resolve(m, bypass, s->bands, s->hz, s->thr, s->ratio, s->att, s->rel, s->mk, s->solo, s->by, s->mix,
                              s->out);
  drain();
}

static void run(OmxMbcompInstance *m, const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 256u) {
    omx_mbcomp_instance_run(m, il + o, ir + o, ol + o, orr + o, n - o < 256u ? n - o : 256u);
    drain();
  }
}

static double complex section(const double c[5], double w) {
  const double complex z1 = cexp(-I * w), z2 = z1 * z1;
  return (c[0] + c[1] * z1 + c[2] * z2) / (1.0 + c[3] * z1 + c[4] * z2);
}

/* |B_k(f)| from the designs: the LR4 sections squared, highs below band k, the low at band k. */
static double band_mag(const struct omx_mbcomp *m, uint32_t k, double f, double sr) {
  const double w = 2.0 * M_PI * f / sr;
  double g = 1.0;
  for (uint32_t j = 0; j + 1u < m->bands; j++) {
    if (j < k) g *= pow(cabs(section(m->xo[j].hp, w)), 2.0);
    else if (j == k) g *= pow(cabs(section(m->xo[j].lp, w)), 2.0);
  }
  return g;
}

static double dft_db(const float *h, uint32_t len, double f, double sr, double amp) {
  const double w = 2.0 * M_PI * f / sr;
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < len; i++) {
    re += h[i] * cos(w * i);
    im -= h[i] * sin(w * i);
  }
  return 20.0 * log10(sqrt(re * re + im * im) / amp);
}

static double tone_db(const float *x, uint32_t n, double f, double sr, double amp) {
  const double w = 2.0 * M_PI * f / sr;
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    re += x[i] * cos(w * i);
    im -= x[i] * sin(w * i);
  }
  return 20.0 * log10(2.0 * sqrt(re * re + im * im) / n / amp);
}

/* ---- A: the tree at rest is an all-pass ------------------------------------------------- */
static void arm_allpass(void) {
  g_arm = "A";
  const uint32_t len = (uint32_t)g_sr;
  const float amp = 1e-3f;
  static OmxMbcompInstance m;
  for (uint32_t bands = OMX_MBC_MIN_BANDS; bands <= OMX_MBC_MAX_BANDS; bands++)
    for (int spread = 0; spread < 2; spread++) {
      struct set s = rest(bands);
      if (spread)
        for (uint32_t k = 0; k + 1u < bands; k++)
          s.hz[k] = (float)(40.0 * pow(16000.0 / 40.0, (double)k / (double)(bands - 2u ? bands - 2u : 1u)));
      omx_mbcomp_instance_init(&m, g_sr);
      apply(&m, 0, &s);
      memset(g_l, 0, len * sizeof(float));
      g_l[0] = amp;
      memcpy(g_r, g_l, len * sizeof(float));
      run(&m, g_l, g_r, g_ol, g_or, len);
      const uint32_t tap = omx_mbcomp_instance_latency(&m);
      double worst = 0.0;
      const double top = fmin(20000.0, 0.45 * g_sr);
      for (int q = 0; q < 50; q++) {
        const double f = 20.0 * pow(top / 20.0, q / 49.0);
        worst = fmax(worst, fabs(dft_db(g_ol + tap, len - tap, f, g_sr, amp)));
      }
      ok(worst <= 0.01, spread ? "every band bypassed, corners spread: unit magnitude at 50 frequencies (0.01 dB)"
                               : "every band bypassed: unit magnitude at 50 frequencies (0.01 dB)",
         worst, 0.01);
    }
  clean();
}

/* ---- B: each band's static law and readout ----------------------------------------------- */
static void arm_static(void) {
  g_arm = "B";
  const uint32_t n = (uint32_t)(g_sr * 1.0f);
  static OmxMbcompInstance m;
  double worst = 0.0;
  for (uint32_t bands = OMX_MBC_MIN_BANDS; bands <= OMX_MBC_MAX_BANDS; bands++) {
    const float *hz = omx_mbcomp_come_up_hz(bands);
    for (uint32_t k = 0; k < bands; k++) {
      /* two octaves inside band k */
      const double f = k == 0 ? hz[0] / 4.0 : k + 1u == bands ? fmin(hz[k - 1u] * 4.0, 0.4 * g_sr) : sqrt(hz[k - 1u] * hz[k]);
      struct set s = rest(bands);
      for (uint32_t j = 0; j < bands; j++) s.by[j] = 0;
      s.thr[k] = -30.0f;
      s.ratio[k] = 3.0f;
      s.mk[k] = 2.0f;
      const double amp_db = -10.0, amp = sqrt(2.0) * pow(10.0, amp_db / 20.0);
      omx_mbcomp_instance_init(&m, g_sr);
      apply(&m, 0, &s);
      for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)(amp * sin(2.0 * M_PI * f * i / g_sr));
      run(&m, g_l, g_r, g_ol, g_or, n);
      double want_lin = 0.0, gr_k = 0.0;
      for (uint32_t j = 0; j < bands; j++) {
        const double bm = band_mag(&m.atom, j, f, g_sr);
        const double lvl = amp_db + 20.0 * log10(bm);
        const double x = lvl - s.thr[j];
        const double grj = x > 0.0 ? (1.0 / s.ratio[j] - 1.0) * x : 0.0;
        if (j == k) gr_k = grj;
        want_lin += pow(10.0, (grj + s.mk[j]) / 20.0) * bm;
      }
      /* the last whole number of periods inside the last 0.4 s, so the measure has no leakage */
      const uint32_t len = (uint32_t)(floor(0.4 * f) * g_sr / f + 0.5), skip = n - len;
      const double got = tone_db(g_ol + skip, len, f, g_sr, amp), want = 20.0 * log10(want_lin);
      worst = fmax(worst, fabs(got - want));
      ok(fabs(got - want) <= 0.02, "a sine inside band k: the sum of g_j |B_j(f)| with the band's law (0.02 dB)", got, want);
      const double ro = omx_mbcomp_instance_band_gr_db(&m, k);
      ok(fabs(ro - gr_k) <= 0.02, "band<k>GainReductionDb reads the band's law (0.02 dB)", ro, gr_k);
      ok(ro <= 0.0 && ro >= OMX_MBCOMP_READOUT_BAND1_GAIN_REDUCTION_DB_MIN, "the readout inside its range", ro, 0);
      ok(omx_mbcomp_instance_band_gr_db(&m, bands) == 0.0f, "a band past the count reads 0", 0, 0);
    }
  }
  printf("  %6.0f Hz: B worst static-law gap %.4f dB\n", (double)g_sr, worst);
  clean();
}

/* ---- C: the corner line ------------------------------------------------------------------ */
static void arm_corner_line(void) {
  g_arm = "C";
  if (g_sr != 32000.0f) return;
  static OmxMbcompInstance m;
  struct set s = rest(2);
  s.hz[0] = 16000.0f;
  omx_mbcomp_instance_init(&m, g_sr);
  apply(&m, 0, &s);
  const double line = CORNER_FRACTION * g_sr, w = 2.0 * M_PI * line / g_sr;
  const double lo = 20.0 * log10(pow(cabs(section(m.atom.xo[0].lp, w)), 2.0));
  const double hi = 20.0 * log10(pow(cabs(section(m.atom.xo[0].hp, w)), 2.0));
  ok(fabs(lo + 6.0206) <= 0.001 && fabs(hi + 6.0206) <= 0.001,
     "a 16 kHz corner at 32 kHz is designed at the corner line: -6.0206 dB both sides there", lo, hi);
  clean();
}

/* ---- D: solo ----------------------------------------------------------------------------- */
static void arm_solo(void) {
  g_arm = "D";
  const uint32_t len = (uint32_t)g_sr;
  const float amp = 1e-3f;
  static OmxMbcompInstance m, a;
  for (uint32_t k = 0; k < 3u; k++) {
    struct set s = rest(3);
    s.solo[k] = 1;
    omx_mbcomp_instance_init(&m, g_sr);
    apply(&m, 0, &s);
    memset(g_l, 0, len * sizeof(float));
    g_l[0] = amp;
    memcpy(g_r, g_l, len * sizeof(float));
    run(&m, g_l, g_r, g_ol, g_or, len);
    double worst = 0.0;
    for (int q = 0; q < 20; q++) {
      const double f = 30.0 * pow(15000.0 / 30.0, q / 19.0);
      const double want = 20.0 * log10(band_mag(&m.atom, k, f, g_sr));
      if (want > -40.0) worst = fmax(worst, fabs(dft_db(g_ol, len, f, g_sr, amp) - want));
    }
    ok(worst <= 0.01, "solo band k: the sum is |B_k(f)| (0.01 dB)", worst, 0.01);
  }
  stimulus(g_l, g_r, 20000u, 0x1234u, 2000u);
  struct set s = rest(4);
  for (uint32_t k = 0; k < NB; k++) {
    s.by[k] = 0;
    s.thr[k] = -40.0f;
  }
  omx_mbcomp_instance_init(&m, g_sr);
  apply(&m, 0, &s);
  run(&m, g_l, g_r, g_ol, g_or, 20000u);
  for (uint32_t k = 0; k < NB; k++) s.solo[k] = 1;
  omx_mbcomp_instance_init(&a, g_sr);
  apply(&a, 0, &s);
  run(&a, g_l, g_r, g_al, g_ar, 20000u);
  ok(memcmp(g_ol, g_al, 20000u * sizeof(float)) == 0 && memcmp(g_or, g_ar, 20000u * sizeof(float)) == 0,
     "every band soloed is no band soloed, bit for bit", 0, 0);
  clean();
}

/* ---- E: mix, output gain, bypass --------------------------------------------------------- */
static void arm_mix_out_bypass(void) {
  g_arm = "E";
  const uint32_t n = 20000u;
  stimulus(g_l, g_r, n, 0x4321u, 1500u);
  static OmxMbcompInstance m, a, snap;
  struct set s = rest(3);
  omx_mbcomp_instance_init(&a, g_sr);
  apply(&a, 0, &s);
  run(&a, g_l, g_r, g_al, g_ar, n);
  for (uint32_t k = 0; k < NB; k++) {
    s.by[k] = 0;
    s.thr[k] = -50.0f;
    s.ratio[k] = 10.0f;
    s.mk[k] = 6.0f;
  }
  s.mix = 0.0f;
  omx_mbcomp_instance_init(&m, g_sr);
  apply(&m, 0, &s);
  run(&m, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_al, n * sizeof(float)) == 0 && memcmp(g_or, g_ar, n * sizeof(float)) == 0,
     "mix 0 %: the tree at rest, bit for bit, whatever the bands compress", 0, 0);
  struct set o = rest(3);
  o.out = 6.0f;
  omx_mbcomp_instance_init(&m, g_sr);
  apply(&m, 0, &o);
  run(&m, g_l, g_r, g_ol, g_or, n);
  const float g = omx_db_to_lin(6.0f);
  int same = 1;
  for (uint32_t i = 0; i < n; i++) same &= g_ol[i] == g_al[i] * g && g_or[i] == g_ar[i] * g;
  ok(same, "the output gain is one multiply by its linear gain, bit for bit", 0, 0);
  apply(&m, 1, &o);
  memcpy(&snap, &m, sizeof snap);
  run(&m, g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0 &&
         memcmp(&snap, &m, sizeof m) == 0,
     "bypass: the identity, no state word moves", 0, 0);
  clean();
}

/* ---- F: the declarations, hostile words, no allocation ------------------------------------ */
static void arm_declared(void) {
  g_arm = "F";
  ok(OMX_MBCOMP_BANDS_RANGE_MIN == MIN_BANDS && OMX_MBCOMP_BANDS_RANGE_MAX == MAX_BANDS && OMX_MBC_MIN_BANDS == MIN_BANDS &&
         OMX_MBC_MAX_BANDS == MAX_BANDS,
     "the band count travel is MBC_MIN_BANDS to MBC_MAX_BANDS as declared", OMX_MBC_MAX_BANDS, MAX_BANDS);
  ok(OMX_MBC_MAX_XOVERS == MAX_XOVERS && OMX_MBC_MAX_XOVERS + 1u == OMX_MBC_MAX_BANDS,
     "MBC_MAX_XOVERS is one fewer than MBC_MAX_BANDS, as declared", OMX_MBC_MAX_XOVERS, MAX_XOVERS);
  static OmxMbcompInstance m;
  omx_mbcomp_instance_init(&m, g_sr);
  ok(m.atom.bands == 3u && m.hz[0] == COME_UP_3[0] && m.hz[1] == COME_UP_3[1],
     "a fresh stage comes up at three bands on MBC_CROSSOVER_DEFAULTS_3", m.hz[0], COME_UP_3[0]);
  ok(fabs(OMX_MBC_CORNER_MAX_FRACTION_DOUBLE - CORNER_FRACTION) < 1e-12, "MBC_CORNER_MAX_FRACTION as declared",
     OMX_MBC_CORNER_MAX_FRACTION_DOUBLE, CORNER_FRACTION);
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  for (unsigned h = 0; h < sizeof W / sizeof W[0]; h++) {
    struct set s;
    s.bands = W[h];
    for (uint32_t k = 0; k < NX; k++) s.hz[k] = h == 5 ? 1000.0f - 100.0f * (float)k : W[h];
    for (uint32_t k = 0; k < NB; k++) {
      s.thr[k] = s.ratio[k] = s.att[k] = s.rel[k] = s.mk[k] = W[h];
      s.solo[k] = 3;
      s.by[k] = -1;
    }
    s.mix = s.out = W[h];
    omx_mbcomp_instance_init(&m, g_sr);
    apply(&m, 0, &s);
    int rising = 1, inside = 1;
    for (uint32_t k = 1; k + 1u < m.atom.bands; k++) rising &= m.hz[k] > m.hz[k - 1u];
    for (uint32_t k = 0; k < NB; k++) {
      const struct omx_dyn *d = &m.atom.band[k];
      inside &= d->enabled == 1 && d->gc.thresh_db >= OMX_MBCOMP_THRESHOLD_DB_RANGE_MIN &&
                d->gc.thresh_db <= OMX_MBCOMP_THRESHOLD_DB_RANGE_MAX && d->gc.ratio >= OMX_MBCOMP_RATIO_RANGE_MIN &&
                d->gc.ratio <= OMX_MBCOMP_RATIO_RANGE_MAX && d->attack_ms >= OMX_MBCOMP_ATTACK_MS_RANGE_MIN &&
                d->attack_ms <= OMX_MBCOMP_ATTACK_MS_RANGE_MAX && d->dry >= 0.0f && d->dry <= 1.0f;
    }
    ok(m.atom.bands >= OMX_MBC_MIN_BANDS && m.atom.bands <= OMX_MBC_MAX_BANDS && rising && inside && m.atom.muted == 0u &&
           m.out_lin > 0.0f && m.out_lin - m.out_lin == 0.0f,
       "hostile words: every travel clamped, the corners rising, the choices at their defaults", h, 0);
    stimulus(g_l, g_r, 4096u, 0x55u, 512u);
    run(&m, g_l, g_r, g_ol, g_or, 4096u);
  }
  g_allocs = 0;
  g_counting = 1;
  struct set s = rest(5);
  for (uint32_t k = 0; k < NB; k++) {
    s.by[k] = 0;
    s.att[k] = 0.2f;
  }
  s.solo[2] = 1;
  s.mix = 60.0f;
  s.out = -3.0f;
  omx_mbcomp_instance_init(&m, g_sr);
  apply(&m, 0, &s);
  run(&m, g_l, g_r, g_ol, g_or, 4096u);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0);
  ok(omx_mbcomp_instance_latency(&m) == OMX_OVS_LATENCY_4X, "a 0.2 ms band attack runs the stage at 4x and says so", 0, 0);
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
    arm_allpass();
    arm_static();
    arm_corner_line();
    arm_solo();
    arm_mix_out_bypass();
    arm_declared();
  }
  return instance_oracle_end("mbcomp_instance");
}
