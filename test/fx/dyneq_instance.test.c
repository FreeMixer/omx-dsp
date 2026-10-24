// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The dynamic EQ's instance oracle (omx_dyneq_instance.h) at each of the nine oracle rates
// (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  with every band's dynamics off, the output is the EQ face's (omx_eq_instance.h) at the same
//      bands, memcmp-equal on both legs, in odd block sizes, separate and aliased buffers, with bells,
//      shelves, parked 0 dB bands and bands that are off;
//   B  the static law: a steady sine at a bell's centre, level L at the band's detector, comes out at
//      `G + clamp((L - T)(1/R - 1), range)` (down) or `G + clamp((R - 1)(L - T), range)` (up), signed by
//      the range, knee BAND_DYN_KNEE_DB, within 0.04 dB; the readout band<n>GainDb is the closed form at
//      the detector's level within 0.01 dB; and under a shelf, the output is `|S(f)|·|1 + (g - 1)B(f)|`
//      at a frequency inside the shelf;
//   C  the band's attack and release: the 4-stage peak cascade reaches 56.65 % of a step at attackMs and
//      falls to 43.35 % at releaseMs (an Erlang-4 at t = tau), within 5 %;
//   D  listen: the output is the band's B over the signal reaching the band, bit for bit against a
//      reference whose B is designed here and run by omx_biquad(); two listening bands give their sum;
//   E  bypass and a refused rate are the identity and move no state word; hostile control words are
//      clamped to the declared travels (the output equals one resolved at the clamped values) and the
//      ledger stays clean;
//   F  mix 0 % is the input bit for bit; 50 % is the convex blend of the input and the 100 % output;
//   H  init, resolve and run allocate nothing (malloc, calloc, realloc, free wrapped and counted).
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_dyneq_instance.h>

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

#define NB OMX_DYNEQ_INSTANCE_BANDS
#define EB OMX_EQ_INSTANCE_BANDS
#define N 20000u
#define MAXN 400000u

/** The band knee, dB, BAND_DYN_KNEE_DB, and the band count, DYNEQ_BANDS, as declared: stated here and
 * not read off the header, so a moved declaration turns arms A and B red (dyneq_instance-perturb.sh). */
#define KNEE_DB 6.0
#define BANDS_DECLARED 6
/** Where the 4-stage peak cascade settles on a sine, dB under its peak (band_dyn.test.c's O3 pins it). */
#define PEAK_SETTLE_DB (-0.742)

/** Violations an arm's rate does not explain, taken off the ledger as the arm runs: at 32, 64 and
 * 128 kHz every design records `rate-is-declared`, and the ledger keeps OMX_CONTRACT_MAX records. */
static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

static float in_l[N], in_r[N], a_l[N], a_r[N], b_l[N], b_r[N];
static float g_x[MAXN], g_y[MAXN], g_z[MAXN];

/** One band's controls. */
struct band {
  int on, type;
  float freq, gain, q;
  int mode;
  float thr, ratio, range, att, rel;
  int listen;
};

struct set {
  struct band b[NB];
  float mix;
};

static struct set quiet(void) {
  struct set s;
  memset(&s, 0, sizeof s);
  static const float centres[BANDS_DECLARED] = {80.0f, 250.0f, 800.0f, 2500.0f, 6000.0f, 12000.0f};
  for (int i = 0; i < NB; i++) {
    s.b[i] = (struct band){0, OMX_DYNEQ_BAND_TYPES_BELL, centres[i], 0.0f, 1.0f, OMX_DYNEQ_DYN_MODES_OFF,
                           -30.0f, 2.0f, -6.0f, 5.0f, 100.0f, 0};
  }
  s.mix = 100.0f;
  return s;
}

static void apply(OmxDyneqInstance *d, int bypass, const struct set *s) {
  int on[NB], type[NB], mode[NB], lis[NB];
  float f[NB], g[NB], q[NB], thr[NB], ra[NB], rg[NB], at[NB], re[NB];
  for (int i = 0; i < NB; i++) {
    on[i] = s->b[i].on;
    type[i] = s->b[i].type;
    f[i] = s->b[i].freq;
    g[i] = s->b[i].gain;
    q[i] = s->b[i].q;
    mode[i] = s->b[i].mode;
    thr[i] = s->b[i].thr;
    ra[i] = s->b[i].ratio;
    rg[i] = s->b[i].range;
    at[i] = s->b[i].att;
    re[i] = s->b[i].rel;
    lis[i] = s->b[i].listen;
  }
  omx_dyneq_instance_resolve(d, bypass, on, type, f, g, q, mode, thr, ra, rg, at, re, lis, s->mix);
  drain();
}

/** The EQ face at the same static bands, its other bands and pass filters off. */
static void apply_eq(OmxEqInstance *e, const struct set *s) {
  int type[EB], on[EB];
  float f[EB], g[EB], q[EB];
  for (int i = 0; i < EB; i++) {
    const int mine = i < NB;
    type[i] = mine ? s->b[i].type : 0;
    on[i] = mine ? s->b[i].on : 0;
    f[i] = mine ? s->b[i].freq : 1000.0f;
    g[i] = mine ? s->b[i].gain : 0.0f;
    q[i] = mine ? s->b[i].q : 1.0f;
  }
  omx_eq_instance_resolve(e, 0, 0, 100.0f, OMX_FILTER_SLOPES_12, 0, 10000.0f, OMX_FILTER_SLOPES_12, type, f, g, q, on);
  drain();
}

static void run_blocks_dyneq(OmxDyneqInstance *d, const float *xl, const float *xr, float *yl, float *yr, uint32_t n,
                             int alias) {
  static const uint32_t sizes[] = {1u, 64u, 127u, 256u, 33u, 300u};
  if (alias) {
    memcpy(yl, xl, n * sizeof(float));
    memcpy(yr, xr, n * sizeof(float));
  }
  for (uint32_t o = 0, k = 0; o < n; k++) {
    uint32_t b = sizes[k % 6u];
    if (b > n - o) b = n - o;
    if (alias) omx_dyneq_instance_run(d, yl + o, yr + o, yl + o, yr + o, b);
    else omx_dyneq_instance_run(d, xl + o, xr + o, yl + o, yr + o, b);
    o += b;
  }
}

/* ---- A: the EQ face, bit for bit --------------------------------------------------------- */
static void arm_identity(void) {
  g_arm = "A";
  ok(NB == BANDS_DECLARED, "the face carries DYNEQ_BANDS bands as declared", NB, BANDS_DECLARED);
  static const int types[BANDS_DECLARED] = {1, 0, 0, 0, 0, 2};
  static const float gains[BANDS_DECLARED] = {4.0f, -6.5f, 0.0f, 3.0f, -12.0f, 6.0f};
  static const int ons[BANDS_DECLARED] = {1, 1, 1, 0, 1, 1};
  for (int alias = 0; alias < 2; alias++) {
    struct set s = quiet();
    for (int i = 0; i < NB; i++) {
      s.b[i].on = ons[i];
      s.b[i].type = types[i];
      s.b[i].gain = gains[i];
      s.b[i].q = 0.7f + 0.4f * (float)i;
      s.b[i].thr = -60.0f; /* a threshold the stimulus is far above: dynamics off still means nothing moves */
    }
    static OmxDyneqInstance d;
    static OmxEqInstance e;
    omx_dyneq_instance_init(&d, g_sr);
    omx_eq_instance_init(&e, g_sr);
    apply(&d, 0, &s);
    apply_eq(&e, &s);
    stimulus(in_l, in_r, N, 0x1234567u, 4096u);
    run_blocks_dyneq(&d, in_l, in_r, a_l, a_r, N, alias);
    for (uint32_t o = 0; o < N; o += 512u) omx_eq_instance_run(&e, in_l + o, in_r + o, b_l + o, b_r + o, N - o < 512u ? N - o : 512u);
    ok(memcmp(a_l, b_l, sizeof a_l) == 0, alias ? "aliased: dynamics off, left leg memcmp the EQ face" : "dynamics off, left leg memcmp the EQ face", 0, 0);
    ok(memcmp(a_r, b_r, sizeof a_r) == 0, alias ? "aliased: dynamics off, right leg memcmp the EQ face" : "dynamics off, right leg memcmp the EQ face", 0, 0);
  }
  clean();
}

/* ---- B: the static law ------------------------------------------------------------------ */
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

/** The characteristic in double: `sign(range)·min(|gr|, |range|)`, the band knee. */
static double closed_d(int down, double thr, double ratio, double range, double level_db) {
  const double x = level_db - thr, h = 0.5 * KNEE_DB;
  double gr;
  if (down) gr = x <= -h ? 0.0 : (x < h ? (1.0 / ratio - 1.0) * (x + h) * (x + h) / (2.0 * KNEE_DB) : (1.0 / ratio - 1.0) * x);
  else gr = x >= h ? 0.0 : (x > -h ? (1.0 - ratio) * (x - h) * (x - h) / (2.0 * KNEE_DB) : (ratio - 1.0) * x);
  const double m = fabs(range), a = fabs(gr) < m ? fabs(gr) : m;
  return range > 0.0 ? a : -a;
}

static double complex_mag(const float c[5], double f, double sr, double *re_out, double *im_out) {
  const double w = 2.0 * M_PI * f / sr;
  const double cr1 = cos(w), ci1 = -sin(w), cr2 = cos(2 * w), ci2 = -sin(2 * w);
  const double nr = c[0] + c[1] * cr1 + c[2] * cr2, ni = c[1] * ci1 + c[2] * ci2;
  const double dr = 1.0 + c[3] * cr1 + c[4] * cr2, di = c[3] * ci1 + c[4] * ci2;
  const double den = dr * dr + di * di;
  *re_out = (nr * dr + ni * di) / den;
  *im_out = (ni * dr - nr * di) / den;
  return sqrt(*re_out * *re_out + *im_out * *im_out);
}

/** `B` designed here, independently of omx_band_dyn_design(): the bell's cookbook bandpass, or the
 * shelf half `½(1 ± A₁)` of the first-order allpass at the corner. */
static void design_b(int type, double f, double q, double sr, float c[5]) {
  if (type == OMX_DYNEQ_BAND_TYPES_BELL) {
    omx_eq_design_f(OMX_EQ_BANDPASS, f, q, 0.0, sr, c);
    return;
  }
  double a[5];
  omx_eq_design(OMX_EQ_ALLPASS1, f, q, 0.0, sr, a);
  const int low = type == OMX_DYNEQ_BAND_TYPES_LOW_SHELF;
  const double h = low ? 0.5 * (1.0 + a[0]) : 0.5 * (1.0 - a[0]);
  c[0] = (float)h;
  c[1] = (float)(low ? h : -h);
  c[2] = 0.0f;
  c[3] = (float)a[0];
  c[4] = 0.0f;
}

/** One steady sine through band 3 (index 2) alone; returns the measured output gain in dB and writes
 * the expected closed form, the readout and the readout's closed form at the detector's level. */
static double steady(int type, double f0, double ftest, float gain, int mode, float thr, float ratio, float range,
                     double amp_db, double *want, double *readout, double *readout_want) {
  struct set s = quiet();
  struct band *b = &s.b[2];
  b->on = 1;
  b->type = type;
  b->freq = (float)f0;
  b->gain = gain;
  b->q = 2.0f;
  b->mode = mode;
  b->thr = thr;
  b->ratio = ratio;
  b->range = range;
  b->att = 5.0f;
  b->rel = 100.0f;
  static OmxDyneqInstance d;
  omx_dyneq_instance_init(&d, g_sr);
  apply(&d, 0, &s);
  const uint32_t n = (uint32_t)(g_sr * 0.8f);
  const double amp = pow(10.0, amp_db / 20.0);
  for (uint32_t i = 0; i < n; i++) g_x[i] = (float)(amp * sin(2.0 * M_PI * ftest * i / g_sr));
  for (uint32_t o = 0; o < n; o += 256u) omx_dyneq_instance_run(&d, g_x + o, g_x + o, g_y + o, g_z + o, n - o < 256u ? n - o : 256u);
  const uint32_t skip = n / 2u;
  drain();
  const double got = db(tone_mag(g_y + skip, n - skip, ftest, g_sr) / amp);
  /* The closed form: the static section S, then y = u + (g - 1)B(u), the detector on B(u). */
  const struct omx_eq_lv2_band *eb = &d.eq.leg[0].band[2];
  float sc[5];
  omx_eq_design_f(omx_eq_lv2_kind(eb->type), eb->freq_hz, eb->q, eb->gain_db, g_sr, sc);
  drain();
  double sre, sim, bre, bim;
  const double smag = gain == 0.0f ? 1.0 : complex_mag(sc, ftest, g_sr, &sre, &sim);
  float bc[5];
  design_b(type, eb->freq_hz, eb->q, g_sr, bc);
  const double bmag = complex_mag(bc, ftest, g_sr, &bre, &bim);
  const double level_db = amp_db + db(smag) + db(bmag) + PEAK_SETTLE_DB;
  const double dd = closed_d(mode == OMX_DYNEQ_DYN_MODES_DOWN, thr, ratio, range, level_db);
  const double g = pow(10.0, dd / 20.0);
  const double hr = 1.0 + (g - 1.0) * bre, hi = (g - 1.0) * bim;
  *want = db(smag) + db(sqrt(hr * hr + hi * hi));
  *readout = omx_dyneq_instance_band_gain_db(&d, 2);
  const struct omx_band_dyn_block bk = omx_band_dyn_begin(&d.band[2].dyn);
  drain();
  *readout_want = closed_d(mode == OMX_DYNEQ_DYN_MODES_DOWN, thr, ratio, range, db(omx_env_level(&d.st[2].env, &bk.ep)));
  return got;
}

static void arm_static(void) {
  g_arm = "B";
  double worst = 0.0;
  static const struct {
    int mode;
    float gain, range;
    double amp_db;
  } CASE[] = {
      {OMX_DYNEQ_DYN_MODES_DOWN, 0.0f, -12.0f, -6.0},  {OMX_DYNEQ_DYN_MODES_DOWN, 4.0f, -12.0f, -12.0},
      {OMX_DYNEQ_DYN_MODES_DOWN, -6.0f, -24.0f, 0.0},  {OMX_DYNEQ_DYN_MODES_DOWN, 0.0f, 9.0f, -10.0},
      {OMX_DYNEQ_DYN_MODES_DOWN, 0.0f, -3.0f, -6.0},   {OMX_DYNEQ_DYN_MODES_DOWN, 0.0f, -12.0f, -45.0},
      {OMX_DYNEQ_DYN_MODES_UP, 0.0f, 6.0f, -50.0},     {OMX_DYNEQ_DYN_MODES_UP, 3.0f, -12.0f, -48.0},
      {OMX_DYNEQ_DYN_MODES_UP, 0.0f, 12.0f, -20.0},
      /* inside the knee: the detector 1 dB either side of the threshold */
      {OMX_DYNEQ_DYN_MODES_DOWN, 0.0f, -12.0f, -30.0 - PEAK_SETTLE_DB + 1.0},
      {OMX_DYNEQ_DYN_MODES_UP, 0.0f, -12.0f, -30.0 - PEAK_SETTLE_DB - 1.0},
  };
  for (unsigned c = 0; c < sizeof CASE / sizeof CASE[0]; c++) {
    double want, ro, ro_want;
    const double got = steady(OMX_DYNEQ_BAND_TYPES_BELL, 1000.0, 1000.0, CASE[c].gain, CASE[c].mode, -30.0f, 4.0f,
                              CASE[c].range, CASE[c].amp_db, &want, &ro, &ro_want);
    worst = fmax(worst, fabs(got - want));
    ok(fabs(got - want) <= 0.04, "bell at its centre: G + the band's offset (0.04 dB)", got, want);
    ok(fabs(ro - ro_want) <= 0.01, "band<n>GainDb is the closed form at the detector's level (0.01 dB)", ro, ro_want);
    ok(CASE[c].range < 0 ? (ro <= 0.0 && ro >= CASE[c].range - 1e-4) : (ro >= 0.0 && ro <= CASE[c].range + 1e-4),
       "band<n>GainDb is signed and bounded by the range", ro, CASE[c].range);
  }
  /* Shelves: a frequency deep inside the shelf. */
  double want, ro, ro_want, got;
  got = steady(OMX_DYNEQ_BAND_TYPES_LOW_SHELF, 400.0, 100.0, 3.0f, OMX_DYNEQ_DYN_MODES_DOWN, -30.0f, 3.0f, -9.0f, -8.0,
               &want, &ro, &ro_want);
  worst = fmax(worst, fabs(got - want));
  ok(fabs(got - want) <= 0.04, "low shelf: |S(f)|·|1 + (g - 1)B(f)| inside the shelf (0.04 dB)", got, want);
  ok(fabs(ro - ro_want) <= 0.01, "low shelf: the readout at the detector's level (0.01 dB)", ro, ro_want);
  got = steady(OMX_DYNEQ_BAND_TYPES_HIGH_SHELF, 3000.0, 12000.0, -2.0f, OMX_DYNEQ_DYN_MODES_DOWN, -30.0f, 3.0f, -9.0f,
               -8.0, &want, &ro, &ro_want);
  worst = fmax(worst, fabs(got - want));
  ok(fabs(got - want) <= 0.04, "high shelf: |S(f)|·|1 + (g - 1)B(f)| inside the shelf (0.04 dB)", got, want);
  ok(fabs(ro - ro_want) <= 0.01, "high shelf: the readout at the detector's level (0.01 dB)", ro, ro_want);
  printf("  %6.0f Hz: B worst static-law gap %.4f dB\n", (double)g_sr, worst);
  clean();
}

/* ---- C: attack and release ------------------------------------------------------------- */
static void arm_times(void) {
  g_arm = "C";
  struct set s = quiet();
  struct band *b = &s.b[0];
  b->on = 1;
  b->type = OMX_DYNEQ_BAND_TYPES_LOW_SHELF;
  b->freq = 5000.0f;
  b->mode = OMX_DYNEQ_DYN_MODES_DOWN;
  b->thr = -30.0f;
  b->att = 5.0f;
  b->rel = 100.0f;
  static OmxDyneqInstance d;
  omx_dyneq_instance_init(&d, g_sr);
  apply(&d, 0, &s);
  const struct omx_band_dyn_block bk = omx_band_dyn_begin(&d.band[0].dyn);
  const uint32_t n = (uint32_t)(g_sr * 1.5f);
  const float hi = 0.316227766f;
  double t_up = -1.0, t_dn = -1.0;
  for (uint32_t i = 0; i < n; i++) {
    float x = hi, y = hi;
    omx_dyneq_instance_run(&d, &x, &y, &x, &y, 1u);
    if (t_up < 0 && omx_env_level(&d.st[0].env, &bk.ep) >= 0.566530f * hi) t_up = 1000.0 * i / g_sr;
  }
  const float top = omx_env_level(&d.st[0].env, &bk.ep);
  for (uint32_t i = 0; i < n; i++) {
    float x = 0.0f, y = 0.0f;
    omx_dyneq_instance_run(&d, &x, &y, &x, &y, 1u);
    if (t_dn < 0 && omx_env_level(&d.st[0].env, &bk.ep) <= 0.433470f * top) t_dn = 1000.0 * i / g_sr;
  }
  ok(fabs(t_up - 5.0) <= 0.25, "the band's detector reaches 56.65 % of a step at attackMs (5 %)", t_up, 5.0);
  ok(fabs(t_dn - 100.0) <= 5.0, "the band's detector falls to 43.35 % at releaseMs (5 %)", t_dn, 100.0);
  clean();
}

/* ---- D: listen ------------------------------------------------------------------------- */
static void arm_listen(void) {
  g_arm = "D";
  struct set s = quiet();
  s.b[1] = (struct band){1, OMX_DYNEQ_BAND_TYPES_BELL, 300.0f, 5.0f, 1.5f, OMX_DYNEQ_DYN_MODES_DOWN, -40.0f, 4.0f, -12.0f, 2.0f, 80.0f, 1};
  s.b[3] = (struct band){1, OMX_DYNEQ_BAND_TYPES_HIGH_SHELF, 4000.0f, -3.0f, 0.7f, OMX_DYNEQ_DYN_MODES_OFF, -40.0f, 2.0f, -6.0f, 5.0f, 100.0f, 0};
  s.b[4] = (struct band){1, OMX_DYNEQ_BAND_TYPES_BELL, 7000.0f, 0.0f, 3.0f, OMX_DYNEQ_DYN_MODES_UP, -50.0f, 2.0f, 6.0f, 5.0f, 100.0f, 0};
  for (int two = 0; two < 2; two++) {
    s.b[4].listen = two;
    static OmxDyneqInstance d;
    omx_dyneq_instance_init(&d, g_sr);
    apply(&d, 0, &s);
    stimulus(in_l, in_r, N, 0x2468ace, 2048u);
    for (uint32_t o = 0; o < N; o += 200u) omx_dyneq_instance_run(&d, in_l + o, in_r + o, a_l + o, a_r + o, N - o < 200u ? N - o : 200u);
    /* The reference: the same bands without listening, band by band, B tapped where each band sits. */
    struct set q = s;
    for (int i = 0; i < NB; i++) q.b[i].listen = 0;
    static OmxDyneqInstance r;
    omx_dyneq_instance_init(&r, g_sr);
    apply(&r, 0, &q);
    float hl[NB][4] = {{0}}, hr[NB][4] = {{0}};
    for (uint32_t o = 0; o < N; o += 200u) {
      const uint32_t m = N - o < 200u ? N - o : 200u;
      memcpy(b_l + o, in_l + o, m * sizeof(float));
      memcpy(b_r + o, in_r + o, m * sizeof(float));
      float sl[200] = {0}, sr[200] = {0};
      for (int i = 0; i < NB; i++) {
        if (!r.on[i]) continue;
        const uint32_t k = r.entry[i];
        omx_biquad_cascade(b_l + o, m, 1u, (const float(*)[5]) & r.eq.leg[0].coeffs[k], &r.eq.leg[0].enabled[k], &r.eq.leg[0].state[k]);
        omx_biquad_cascade(b_r + o, m, 1u, (const float(*)[5]) & r.eq.leg[1].coeffs[k], &r.eq.leg[1].enabled[k], &r.eq.leg[1].state[k]);
        if (s.b[i].listen) {
          float c[5];
          design_b(s.b[i].type, r.eq.leg[0].band[i].freq_hz, r.eq.leg[0].band[i].q, g_sr, c);
          for (uint32_t j = 0; j < m; j++) {
            sl[j] += omx_biquad(b_l[o + j], c, hl[i]);
            sr[j] += omx_biquad(b_r[o + j], c, hr[i]);
          }
        }
        omx_band_dyn_process(b_l + o, b_r + o, m, &r.band[i], &r.st[i]);
      }
      memcpy(b_l + o, sl, m * sizeof(float));
      memcpy(b_r + o, sr, m * sizeof(float));
    }
    ok(memcmp(a_l, b_l, sizeof a_l) == 0 && memcmp(a_r, b_r, sizeof a_r) == 0,
       two ? "two listening bands: the sum of their B, bit for bit" : "listen: the band's B over the signal reaching it, bit for bit", 0, 0);
  }
  clean();
}

static int all_finite(const float *a, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    if (a[i] - a[i] != 0.0f) return 0;
  return 1;
}

static int same_bytes_all(void) {
  return memcmp(a_l, in_l, sizeof a_l) == 0 && memcmp(a_r, in_r, sizeof a_r) == 0;
}

/* ---- E: bypass, a refused rate, hostile words ------------------------------------------ */
static void arm_bypass_hostile(void) {
  g_arm = "E";
  struct set s = quiet();
  s.b[2] = (struct band){1, OMX_DYNEQ_BAND_TYPES_BELL, 1000.0f, 6.0f, 2.0f, OMX_DYNEQ_DYN_MODES_DOWN, -40.0f, 4.0f, -12.0f, 2.0f, 80.0f, 0};
  static OmxDyneqInstance d, snap;
  omx_dyneq_instance_init(&d, g_sr);
  apply(&d, 0, &s);
  stimulus(in_l, in_r, N, 0x13579bdu, 1024u);
  omx_dyneq_instance_run(&d, in_l, in_r, a_l, a_r, N / 2u);
  apply(&d, 1, &s);
  memcpy(&snap, &d, sizeof snap);
  omx_dyneq_instance_run(&d, in_l, in_r, a_l, a_r, N);
  ok(same_bytes_all(), "bypass: out memcmp in", 0, 0);
  ok(memcmp(&snap, &d, sizeof d) == 0, "bypass: no state word moves", 0, 0);
  static OmxDyneqInstance refused;
  ok(omx_dyneq_instance_init(&refused, 22050.0f) == 0, "a rate outside the oracle set is refused", 0, 0);
  omx_dyneq_instance_run(&refused, in_l, in_r, a_l, a_r, N);
  ok(memcmp(a_l, in_l, sizeof a_l) == 0 && memcmp(a_r, in_r, sizeof a_r) == 0, "a refused instance is the identity", 0, 0);
  for (int h = 0; h < 7; h++) {
    static const float HOST[7] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f, 0.0f};
    const float w = HOST[h];
    struct set x = s;
    for (int i = 0; i < NB; i++) {
      x.b[i].on = i == 2 ? 1 : (h == 0 ? 7 : 0);
      x.b[i].type = h - 3;
      x.b[i].mode = i == 2 ? OMX_DYNEQ_DYN_MODES_DOWN : 9;
      x.b[i].freq = w;
      x.b[i].gain = w;
      x.b[i].q = w;
      x.b[i].thr = w;
      x.b[i].ratio = w;
      x.b[i].range = w;
      x.b[i].att = w;
      x.b[i].rel = w;
      x.b[i].listen = -2;
    }
    x.mix = w;
    static OmxDyneqInstance hd;
    omx_dyneq_instance_init(&hd, g_sr);
    apply(&hd, 0, &x);
    omx_dyneq_instance_run(&hd, in_l, in_r, a_l, a_r, N);
    drain();
    ok(all_finite(a_l, N) && all_finite(a_r, N), "hostile words: the output is finite", h, 0);
    for (int i = 0; i < NB; i++) {
      const struct omx_band_dyn *b = &hd.band[i];
      ok(b->dyn.gc.thresh_db >= OMX_DYNEQ_THRESHOLD_RANGE_MIN && b->dyn.gc.thresh_db <= OMX_DYNEQ_THRESHOLD_RANGE_MAX &&
             b->dyn.gc.ratio >= OMX_DYNEQ_RATIO_RANGE_MIN && b->dyn.gc.ratio <= OMX_DYNEQ_RATIO_RANGE_MAX &&
             b->dyn.gc.range_db >= OMX_DYNEQ_RANGE_RANGE_MIN && b->dyn.gc.range_db <= OMX_DYNEQ_RANGE_RANGE_MAX &&
             b->dyn.attack_ms >= OMX_DYNEQ_ATTACK_RANGE_MIN && b->dyn.attack_ms <= OMX_DYNEQ_ATTACK_RANGE_MAX &&
             hd.listen[i] == 0 && (i == 2 || hd.on[i] == 0),
         "hostile words: every band's atom inside its declared travel, choices at their defaults", h, 0);
    }
    ok(hd.mix >= 0.0f && hd.mix <= 1.0f, "hostile words: the mix inside its travel", hd.mix, 1.0);
  }
  clean();
}

/* ---- F: the mix ------------------------------------------------------------------------ */
static void arm_mix(void) {
  g_arm = "F";
  struct set s = quiet();
  s.b[2] = (struct band){1, OMX_DYNEQ_BAND_TYPES_BELL, 1000.0f, 6.0f, 2.0f, OMX_DYNEQ_DYN_MODES_DOWN, -40.0f, 4.0f, -12.0f, 2.0f, 80.0f, 0};
  stimulus(in_l, in_r, N, 0x55aa55u, 1500u);
  static OmxDyneqInstance d, w;
  s.mix = 0.0f;
  omx_dyneq_instance_init(&d, g_sr);
  apply(&d, 0, &s);
  omx_dyneq_instance_run(&d, in_l, in_r, a_l, a_r, N);
  ok(same_bytes_all(), "mix 0 %: the input bit for bit", 0, 0);
  s.mix = 50.0f;
  omx_dyneq_instance_init(&d, g_sr);
  apply(&d, 0, &s);
  omx_dyneq_instance_run(&d, in_l, in_r, a_l, a_r, N);
  s.mix = 100.0f;
  omx_dyneq_instance_init(&w, g_sr);
  apply(&w, 0, &s);
  omx_dyneq_instance_run(&w, in_l, in_r, b_l, b_r, N);
  int same = 1;
  for (uint32_t i = 0; i < N; i++) {
    same &= a_l[i] == 0.5f * in_l[i] + 0.5f * b_l[i];
    same &= a_r[i] == 0.5f * in_r[i] + 0.5f * b_r[i];
  }
  ok(same, "mix 50 %: half the input plus half the 100 % output, bit for bit", 0, 0);
  clean();
}

/* ---- H: no allocation ------------------------------------------------------------------ */
static void arm_alloc(void) {
  g_arm = "H";
  struct set s = quiet();
  for (int i = 0; i < NB; i++) {
    s.b[i].on = 1;
    s.b[i].mode = 1 + i % 2;
    s.b[i].listen = i == 5;
  }
  g_allocs = 0;
  g_counting = 1;
  static OmxDyneqInstance d;
  omx_dyneq_instance_init(&d, g_sr);
  apply(&d, 0, &s);
  omx_dyneq_instance_run(&d, in_l, in_r, a_l, a_r, N);
  s.b[0].freq = 333.0f;
  s.mix = 40.0f;
  apply(&d, 0, &s);
  omx_dyneq_instance_run(&d, in_l, in_r, a_l, a_r, N);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0);
  ok(omx_dyneq_instance_latency(&d) == 0u, "latency is zero", 0, 0);
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
    arm_static();
    arm_times();
    arm_listen();
    arm_bypass_hostile();
    arm_mix();
    arm_alloc();
  }
  return instance_oracle_end("dyneq_instance");
}
