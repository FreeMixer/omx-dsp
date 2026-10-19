// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The graphic EQ's `true` mode oracle (omx_geq_instance_design / omx_geq_true_gains), at the nine
// oracle rates. The measured curve is the CASCADE the kernel runs: the
// sum of the designed sections' magnitudes, from the coefficients in the design, never the
// solve's own matrix.
//   T  at every rate the instance accepts, the designed cascade meets every fader at every ISO
//      centre within 0.5 dB, over six shaped sets and 50 seeded fader sets on the travel's grid;
//      at a rate it refuses (32 kHz: the top centre is past Nyquist) the design is refused too;
//   K  the oracle has teeth: the faders handed through unsolved (standard) miss the alternating
//      ±12 set by more than 0.5 dB at every rate, and the solve lands within 0.2 dB;
//   S  standard designed and applied is bit-identical to resolve() on the same words;
//   F  flat is exact zeros in both modes, and the output is the input byte for byte;
//   C  the clamped case: a target past what the ceiling reaches (alternating ±22 dB, beyond the
//      travel, through omx_geq_true_gains) reports clamped, every gain sits inside ±30 dB and the
//      bands it clamped sit ON it, every gain and the cascade they make keep the target's sign;
//      the travel's own worst set (alternating ±15) is unclamped;
//   A  audio: a sine at three centres through the instance applied in `true` mode comes out at the
//      fader's gain within 0.5 dB;
//   H  design, apply and run allocate nothing (malloc, calloc, realloc and free wrapped at link time).
#define OMX_CONTRACT_STORAGE 1
#include <stdlib.h>

#include <omxdsp/fx/omx_geq_instance.h>

#include "instance_oracle.h"

#define N 32768u
#define TOL_DB 0.5
#define SEEDS 50
#define AUDIO_PASSES 4u

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

static float in_l[N], in_r[N], out_l[N], out_r[N], ref_l[N], ref_r[N];
static const double centres[] = OMX_GEQ_INSTANCE_CENTRES_HZ;

/** The cascade's dB at `f`: the sum of the live sections' magnitudes, from the coefficients. */
static double cascade_db(const double c[][5], const double *section_db, double f, double sr) {
  double db = 0.0;
  for (int k = 0; k < OMX_GEQ_BANDS; k++)
    if (section_db[k] != 0.0) db += omx_biquad_magnitude_db_d(c[k], f, sr, -300.0);
  return db;
}

/** The worst |cascade − target| over the 31 centres. */
static double worst_centre_error(const double c[][5], const double *section_db, const float *target, double sr) {
  double w = 0.0;
  for (int i = 0; i < OMX_GEQ_BANDS; i++) {
    const double e = fabs(cascade_db(c, section_db, centres[i], sr) - (double)target[i]);
    if (e > w) w = e;
  }
  return w;
}

/** A fader set on the travel's 0.5 dB grid from a fixed LCG (a red names its seed). */
static void fader_set(uint32_t seed, float g[OMX_GEQ_BANDS]) {
  uint32_t x = seed;
  const uint32_t steps = (uint32_t)((OMX_GEQ_BAND_RANGE_MAX - OMX_GEQ_BAND_RANGE_MIN) / OMX_GEQ_BAND_RANGE_STEP_DOUBLE);
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    x = x * 1664525u + 1013904223u;
    g[k] = (float)(OMX_GEQ_BAND_RANGE_MIN + (double)(x % (steps + 1u)) * OMX_GEQ_BAND_RANGE_STEP_DOUBLE);
  }
}

static void all(float g[OMX_GEQ_BANDS], float db) {
  for (int k = 0; k < OMX_GEQ_BANDS; k++) g[k] = db;
}
static void alternating(float g[OMX_GEQ_BANDS], float db) {
  for (int k = 0; k < OMX_GEQ_BANDS; k++) g[k] = (k % 2 == 0) ? db : -db;
}
static void one_band(float g[OMX_GEQ_BANDS]) {
  for (int k = 0; k < OMX_GEQ_BANDS; k++) g[k] = centres[k] == 1000.0 ? 12.0f : 0.0f;
}
static void tilt(float g[OMX_GEQ_BANDS]) {
  for (int k = 0; k < OMX_GEQ_BANDS; k++) g[k] = (float)(-12.0 + 24.0 * k / (OMX_GEQ_BANDS - 1));
}

static void arm_target(void) {
  g_arm = "T true meets the target";
  OmxGeqInstance s;
  const int ready = omx_geq_instance_init(&s, g_sr);
  float g[OMX_GEQ_BANDS];
  static OmxGeqDesign d;
  if (!ready) {
    ok(!omx_geq_instance_rate_ok(g_sr), "only a rate whose Nyquist the top centre passes is refused", g_sr, 0);
    alternating(g, 12.0f);
    double t[OMX_GEQ_BANDS], x[OMX_GEQ_BANDS];
    for (int k = 0; k < OMX_GEQ_BANDS; k++) t[k] = g[k];
    ok(omx_geq_true_gains(t, g_sr, x, NULL) == 0, "the solve refuses the rate the instance refuses", 0, 0);
    ok(omx_geq_instance_design(&s, OMX_GEQ_MODE_TRUE, g, &d) == 0 && d.ok == 0, "the design refuses it too", 0, 0);
    printf("geq true @ %.0f Hz: refused (top centre past Nyquist)\n", (double)g_sr);
    return;
  }
  double worst = 0.0;
  for (int set = 0; set < 6 + SEEDS; set++) {
    switch (set) {
      case 0: all(g, 12.0f); break;
      case 1: alternating(g, 12.0f); break;
      case 2: one_band(g); break;
      case 3: tilt(g); break;
      case 4: alternating(g, 15.0f); break;
      case 5: all(g, 15.0f); break;
      default: fader_set((uint32_t)(set - 5), g); break;
    }
    ok(omx_geq_instance_design(&s, OMX_GEQ_MODE_TRUE, g, &d) == 1, "the true design succeeds", set, 0);
    ok(d.clamped == 0, "a set inside the travel is never clamped", set, 0);
    const double e = worst_centre_error((const double(*)[5])d.coeffs, d.section_db, g, g_sr);
    if (e > worst) worst = e;
    ok(e <= TOL_DB, "the cascade meets every fader at every centre", e, TOL_DB);
  }
  printf("geq true @ %.0f Hz: worst centre error %.4f dB over %d sets\n", (double)g_sr, worst, 6 + SEEDS);
}

static void arm_teeth(void) {
  g_arm = "K teeth";
  OmxGeqInstance s;
  if (!omx_geq_instance_init(&s, g_sr)) return;
  float g[OMX_GEQ_BANDS];
  alternating(g, 12.0f);
  static OmxGeqDesign st, tr;
  omx_geq_instance_design(&s, OMX_GEQ_MODE_STANDARD, g, &st);
  omx_geq_instance_design(&s, OMX_GEQ_MODE_TRUE, g, &tr);
  const double es = worst_centre_error((const double(*)[5])st.coeffs, st.section_db, g, g_sr);
  const double et = worst_centre_error((const double(*)[5])tr.coeffs, tr.section_db, g, g_sr);
  ok(es > TOL_DB, "standard (the faders unsolved) misses the alternating set", es, TOL_DB);
  ok(et < 0.2, "the solve lands within 0.2 dB", et, 0.2);
  int same = 1;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) same &= st.section_db[k] == (double)g[k];
  ok(same, "standard hands the faders through", 0, 0);
}

static void run_through(OmxGeqInstance *s, float *ol, float *orr) {
  static const uint32_t sizes[] = {1u, 64u, 127u, 256u, 33u};
  uint32_t o = 0, k = 0;
  while (o < N) {
    uint32_t b = sizes[k++ % 5u];
    if (b > N - o) b = N - o;
    omx_geq_instance_run(s, in_l + o, in_r + o, ol + o, orr + o, b);
    o += b;
  }
}

static void arm_standard_identity(void) {
  g_arm = "S standard apply equals resolve";
  float g[OMX_GEQ_BANDS];
  fader_set(7u, g);
  g[3] = 40.0f;
  g[11] = NAN;
  OmxGeqInstance a, b;
  if (!omx_geq_instance_init(&a, g_sr)) return;
  omx_geq_instance_init(&b, g_sr);
  static OmxGeqDesign d;
  omx_geq_instance_resolve(&a, 0, g);
  omx_geq_instance_design(&b, OMX_GEQ_MODE_STANDARD, g, &d);
  omx_geq_instance_apply(&b, 0, &d);
  stimulus(in_l, in_r, N, 0x1234567u, 0u);
  run_through(&a, ref_l, ref_r);
  run_through(&b, out_l, out_r);
  ok(memcmp(out_l, ref_l, sizeof out_l) == 0 && memcmp(out_r, ref_r, sizeof out_r) == 0,
     "standard designed + applied is bit-identical to resolve", 0, 0);
}

static void arm_flat(void) {
  g_arm = "F flat";
  OmxGeqInstance s;
  if (!omx_geq_instance_init(&s, g_sr)) return;
  float g[OMX_GEQ_BANDS] = {0};
  static OmxGeqDesign d;
  for (int mode = 0; mode < 2; mode++) {
    omx_geq_instance_design(&s, mode, g, &d);
    int zero = d.ok == 1;
    for (int k = 0; k < OMX_GEQ_BANDS; k++) zero &= d.section_db[k] == 0.0 && d.live_db[k] == 0.0f;
    ok(zero, mode ? "true: flat is exact zeros" : "standard: flat is exact zeros", mode, 0);
    omx_geq_instance_apply(&s, 0, &d);
    stimulus(in_l, in_r, N, 0x7654321u, 0u);
    run_through(&s, out_l, out_r);
    ok(memcmp(out_l, in_l, sizeof out_l) == 0 && memcmp(out_r, in_r, sizeof out_r) == 0, "flat is the identity", mode, 0);
  }
}

static void arm_clamped(void) {
  g_arm = "C clamped";
  if (!omx_geq_instance_rate_ok(g_sr)) return;
  double t[OMX_GEQ_BANDS], x[OMX_GEQ_BANDS];
  int clamped = -1;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) t[k] = (k % 2 == 0) ? 22.0 : -22.0;
  ok(omx_geq_true_gains(t, g_sr, x, &clamped) == 1, "a target past the ceiling still designs", 0, 0);
  ok(clamped == 1, "and reports clamped", clamped, 1);
  int inside = 1, on = 0;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    inside &= fabs(x[k]) <= (double)OMX_GEQ_DESIGN_GAIN_MAX_DB;
    on += fabs(x[k]) == (double)OMX_GEQ_DESIGN_GAIN_MAX_DB;
  }
  ok(inside, "every gain inside the ceiling", 0, 0);
  ok(on > 0, "the clamped bands sit on the ceiling", on, 1);
  /* The clamped design still cuts where the target cuts and boosts where it boosts: every section
   * keeps its target's sign, and the cascade they make reads the target's sign at every centre. */
  int signs = 1, cascade_signs = 1;
  double c[OMX_GEQ_BANDS][5];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    signs &= (x[k] > 0.0) == (t[k] > 0.0);
    omx_eq_design(OMX_EQ_PEAKING, centres[k], OMX_GEQ_BAND_Q_DOUBLE, x[k], g_sr, c[k]);
  }
  ok(signs, "every clamped gain keeps its target's sign", 0, 0);
  for (int i = 0; i < OMX_GEQ_BANDS; i++) cascade_signs &= (cascade_db(c, x, centres[i], g_sr) > 0.0) == (t[i] > 0.0);
  ok(cascade_signs, "the clamped cascade reads the target's sign at every centre", 0, 0);
  for (int k = 0; k < OMX_GEQ_BANDS; k++) t[k] = (k % 2 == 0) ? 15.0 : -15.0;
  clamped = -1;
  ok(omx_geq_true_gains(t, g_sr, x, &clamped) == 1 && clamped == 0, "the travel's worst set is unclamped", clamped, 0);
  double big = 0.0;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) big = fabs(x[k]) > big ? fabs(x[k]) : big;
  ok(big <= (double)OMX_GEQ_DESIGN_GAIN_MAX_DB && big > 15.0, "its largest gain is past the travel, under the ceiling",
     big, OMX_GEQ_DESIGN_GAIN_MAX_DB);
  printf("geq true @ %.0f Hz: alternating ±22 clamps %d bands; alternating ±15 peaks at %.2f dB\n", (double)g_sr, on, big);
}

static void arm_audio(void) {
  g_arm = "A audio";
  OmxGeqInstance s;
  if (!omx_geq_instance_init(&s, g_sr)) return;
  static const int bands[] = {7, 17, 25}; /* 100 Hz, 1 kHz, 6.3 kHz */
  float g[OMX_GEQ_BANDS];
  static OmxGeqDesign d;
  for (int set = 0; set < 2; set++) {
    if (set == 0) alternating(g, 12.0f); else tilt(g);
    omx_geq_instance_design(&s, OMX_GEQ_MODE_TRUE, g, &d);
    for (unsigned j = 0; j < sizeof bands / sizeof bands[0]; j++) {
      omx_geq_instance_init(&s, g_sr);
      omx_geq_instance_apply(&s, 0, &d);
      /* Four phase-continuous buffers, measured on the last: a 100 Hz section designed past
       * +-20 dB rings for longer than one buffer at 176.4 and 192 kHz. */
      for (uint32_t pass = 0; pass < AUDIO_PASSES; pass++) {
        for (uint32_t i = 0; i < N; i++)
          in_l[i] = (float)(0.1 * sin(2.0 * M_PI * centres[bands[j]] * (double)(pass * N + i) / g_sr));
        memcpy(in_r, in_l, sizeof in_r);
        omx_geq_instance_run(&s, in_l, in_r, out_l, out_r, N);
      }
      const double db = 20.0 * log10(rms(out_l, 0, N) / rms(in_l, 0, N));
      ok(fabs(db - g[bands[j]]) < TOL_DB, "a centre's measured gain is its fader", db, g[bands[j]]);
    }
  }
}

static void arm_no_alloc(void) {
  g_arm = "H no allocation";
  void *(*volatile alloc)(size_t) = malloc;
  void (*volatile release)(void *) = free;
  g_allocs = 0;
  g_counting = 1;
  release(alloc(16));
  g_counting = 0;
  ok(g_allocs == 2, "the allocation counter counts", (double)g_allocs, 2);
  static OmxGeqInstance s;
  static OmxGeqDesign d;
  float g[OMX_GEQ_BANDS];
  tilt(g);
  g_allocs = 0;
  g_counting = 1;
  omx_geq_instance_init(&s, g_sr);
  omx_geq_instance_design(&s, OMX_GEQ_MODE_TRUE, g, &d);
  omx_geq_instance_apply(&s, 0, &d);
  omx_geq_instance_run(&s, in_l, in_r, out_l, out_r, 512u);
  g_counting = 0;
  ok(g_allocs == 0, "design, apply and run allocate nothing", (double)g_allocs, 0);
}

int main(void) {
  (void)expect_clean;        /* the ledger is drained per rate below: 32, 64 and 128 kHz are undeclared */
  (void)sine;
  (void)instance_oracle_end; /* the summary names the oracle rates, not the declared ones */
  omx_fx_require_rate_floor();
  omx_contract_reset();
  uint32_t unexplained = 0;
  for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    g_sr = OMX_FX_ORACLE_RATES[ri];
    arm_target();
    arm_teeth();
    arm_standard_identity();
    arm_flat();
    arm_clamped();
    arm_audio();
    stimulus(in_l, in_r, N, 0x1234567u, 0u);
    arm_no_alloc();
    unexplained += omx_fx_drain_ledger(g_sr);
  }
  g_arm = "ledger";
  ok(unexplained == 0u, "no unexplained contract violation at any rate", unexplained, 0);
  printf("fx/geq_true: %d checks, %d failed, %u oracle rates\n", g_checks, g_failed, OMX_FX_ORACLE_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
