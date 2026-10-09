// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The strip EQ's instance oracle (omx_eq_instance.h), built at 32 bands so a full bank (32 bands +
// 4 pass sections) is wider than OMX_EQ_MAX_BANDS and the chunked walk is exercised, at every
// declared rate:
//   A  the instance IS the bank: bit-identical to a chunk-free reference that designs each section
//      with omx_eq_design_f and runs it with omx_biquad, in `eqCoeffs` order (bands, HPF, LPF), in
//      odd block sizes, separate and aliased buffers;
//   B  every hostile control word (±Inf, NaN, far outside the travel, an unknown type or slope) is
//      clamped before the design: the output equals the reference at the clamped values, and the
//      ledger is clean;
//   C  EQ off, every band off, and an on band at 0 dB (parked) are the identity byte for byte;
//   D  a bell's gain at its centre within 0.05 dB, a 24 dB/oct HPF's stop band, latency zero;
//   E  the stereo instance face (omx_eq_instance_resolve, the contract's controls in order) IS the
//      bank on both legs: each leg bit-identical to the chunk-free reference, bypass a wire;
//   F  a switch, slope or type outside its ids reads as its declared default, a non-finite word as
//      the declared default: the face equals one resolved at those defaults;
//   H  init, resolve and run allocate nothing: malloc, calloc, realloc and free are wrapped at
//      link time and counted across the instance's whole life.
#define OMX_CONTRACT_STORAGE 1
#define OMX_EQ_LV2_BANDS 32
#include <omxdsp/fx/omx_eq_instance.h>

#include "instance_oracle.h"

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

#define N 16384u
#define NB OMX_EQ_LV2_BANDS

static float in_l[N], in_r[N], out[N], ref[N];

/** One section of the reference bank. */
struct sec { enum omx_eq_kind kind; double freq, q, gain; int live; };

static void reference(const struct sec *b, uint32_t nb) {
  memcpy(ref, in_l, sizeof ref);
  for (uint32_t k = 0; k < nb; k++) {
    if (!b[k].live) continue;
    float c[5], s[4] = {0};
    omx_eq_design_f(b[k].kind, b[k].freq, b[k].q, b[k].gain, g_sr, c);
    for (uint32_t i = 0; i < N; i++) ref[i] = omx_biquad(ref[i], c, s);
  }
}

static void run_blocks(struct omx_eq_lv2 *e, int alias) {
  static const uint32_t sizes[] = {1u, 64u, 127u, 256u, 33u};
  uint32_t o = 0, k = 0;
  if (alias) memcpy(out, in_l, sizeof out);
  while (o < N) {
    uint32_t b = sizes[k++ % 5u];
    if (b > N - o) b = N - o;
    omx_eq_lv2_run(e, alias ? out + o : in_l + o, out + o, b);
    o += b;
  }
}

/** The words of one cycle, owned here: the controls struct points into them. */
static float w_on, w_hpf[3], w_lpf[3], w_band[NB][5];
static struct omx_eq_lv2_controls ctl;

static void wire(void) {
  memset(&ctl, 0, sizeof ctl);
  ctl.on = &w_on;
  ctl.hpf_on = &w_hpf[0], ctl.hpf_freq = &w_hpf[1], ctl.hpf_slope = &w_hpf[2];
  ctl.lpf_on = &w_lpf[0], ctl.lpf_freq = &w_lpf[1], ctl.lpf_slope = &w_lpf[2];
  for (int i = 0; i < NB; i++)
    for (int j = 0; j < 5; j++) ctl.band[i][j] = &w_band[i][j];
}

static const enum omx_eq_kind kinds[6] = {OMX_EQ_PEAKING, OMX_EQ_LOWSHELF, OMX_EQ_HIGHSHELF,
                                          OMX_EQ_NOTCH,   OMX_EQ_ALLPASS1, OMX_EQ_ALLPASS2};

/** A full bank: 32 bands of every type across the spectrum, both pass filters at 24 dB/oct. */
static uint32_t full_bank(struct sec *b) {
  uint32_t k = 0;
  w_on = 1.0f;
  for (int i = 0; i < NB; i++) {
    const int type = i % 6;
    const double f = 30.0 * pow(2.0, (double)i * 9.0 / NB);
    const double g = (i % 4 == 3) ? 0.0 : (double)((i * 5) % 25 - 12);
    const double q = type == 3 ? 20.0 : 0.5 + 0.2 * (i % 7);
    w_band[i][0] = (float)type, w_band[i][1] = (float)f, w_band[i][2] = (float)g, w_band[i][3] = (float)q;
    w_band[i][4] = 1.0f;
    const int identity = type <= 2 && (float)g == 0.0f;
    b[k++] = (struct sec){kinds[type], (float)f, (float)q, (float)g, !identity};
  }
  w_hpf[0] = 1.0f, w_hpf[1] = 40.0f, w_hpf[2] = 24.0f;
  w_lpf[0] = 1.0f, w_lpf[1] = 16000.0f, w_lpf[2] = 24.0f;
  double qs[2];
  omx_eq_butterworth_qs(1, qs);
  for (int s = 0; s < 2; s++) b[k++] = (struct sec){OMX_EQ_HIGHPASS, 40.0f, qs[s], 0.0, 1};
  for (int s = 0; s < 2; s++) b[k++] = (struct sec){OMX_EQ_LOWPASS, 16000.0f, qs[s], 0.0, 1};
  return k;
}

static void arm_identity(void) {
  g_arm = "A bank identity";
  struct sec b[NB + 4];
  for (int alias = 0; alias < 2; alias++) {
    const uint32_t nb = full_bank(b);
    struct omx_eq_lv2 e;
    omx_eq_lv2_init(&e, g_sr);
    omx_eq_lv2_set_controls(&e, &ctl);
    run_blocks(&e, alias);
    reference(b, nb);
    ok(e.count == nb && nb > OMX_EQ_MAX_BANDS, "the bank holds every section, wider than one cascade", e.count, nb);
    ok(memcmp(out, ref, sizeof out) == 0, alias ? "aliased in/out equals the reference" : "separate in/out equals the reference", 0, 0);
  }
  expect_clean();
}

static void arm_clamps(void) {
  g_arm = "B clamps";
  struct sec b[NB + 4];
  full_bank(b);
  for (int i = 0; i < NB; i++) w_band[i][4] = 0.0f;
  /* {type, freq, gain, Q} words -> what the design must have seen */
  static const struct { float w[4]; int type; double f, g, q; } rows[] = {
      {{0.0f, 1e6f, 99.0f, 0.0f}, 0, OMX_EQ_FREQ_RANGE_MAX, OMX_EQ_GAIN_RANGE_MAX, OMX_EQ_Q_RANGE_MIN},
      {{1.0f, NAN, -1e30f, NAN}, 1, OMX_EQ_FREQ_RANGE_MIN, OMX_EQ_GAIN_RANGE_MIN, OMX_EQ_Q_RANGE_MIN},
      {{3.0f, -INFINITY, NAN, 500.0f}, 3, OMX_EQ_FREQ_RANGE_MIN, 0.0, OMX_EQ_NOTCH_Q_RANGE_MAX},
      {{99.0f, 2000.0f, 6.0f, INFINITY}, 5, 2000.0, 6.0, OMX_EQ_Q_RANGE_MAX},
      {{NAN, 500.0f, INFINITY, 1.0f}, 0, 500.0, 0.0, 1.0},
      {{-7.0f, 300.0f, -3.0f, 116.0f}, 0, 300.0, -3.0, OMX_EQ_Q_RANGE_MAX},
  };
  const uint32_t nr = sizeof rows / sizeof rows[0];
  uint32_t k = 0;
  for (uint32_t r = 0; r < nr; r++) {
    for (int j = 0; j < 4; j++) w_band[r][j] = rows[r].w[j];
    w_band[r][4] = 1.0f;
    const int identity = rows[r].type <= 2 && rows[r].g == 0.0;
    b[k++] = (struct sec){kinds[rows[r].type], (float)rows[r].f, (float)rows[r].q, (float)rows[r].g, !identity};
  }
  w_hpf[0] = 1.0f, w_hpf[1] = 1e9f, w_hpf[2] = 100.0f;
  w_lpf[0] = 1.0f, w_lpf[1] = -5.0f, w_lpf[2] = NAN;
  double qs[2];
  omx_eq_butterworth_qs(1, qs);
  for (int s = 0; s < 2; s++) b[k++] = (struct sec){OMX_EQ_HIGHPASS, OMX_HPF_FREQ_RANGE_MAX, qs[s], 0.0, 1};
  omx_eq_butterworth_qs(0, qs);
  b[k++] = (struct sec){OMX_EQ_LOWPASS, OMX_LPF_FREQ_RANGE_MIN, qs[0], 0.0, 1};
  struct omx_eq_lv2 e;
  omx_eq_lv2_init(&e, g_sr);
  omx_eq_lv2_set_controls(&e, &ctl);
  run_blocks(&e, 0);
  reference(b, k);
  ok(e.count == k, "the bank holds the clamped sections", e.count, k);
  ok(memcmp(out, ref, sizeof out) == 0, "hostile words equal the reference at the clamped values", 0, 0);
  ok(e.band[0].freq_hz == OMX_EQ_FREQ_RANGE_MAX && e.band[0].gain_db == OMX_EQ_GAIN_RANGE_MAX &&
         e.band[2].q == OMX_EQ_NOTCH_Q_RANGE_MAX && e.band[3].type == 5 && e.hpf.freq_hz == OMX_HPF_FREQ_RANGE_MAX &&
         e.lpf.freq_hz == OMX_LPF_FREQ_RANGE_MIN && e.lpf.sections == 1u,
     "the core holds the clamped controls", e.band[0].freq_hz, OMX_EQ_FREQ_RANGE_MAX);
  expect_clean();
}

static void arm_identity_paths(void) {
  g_arm = "C identity paths";
  struct sec b[NB + 4];
  struct omx_eq_lv2 e;
  full_bank(b);
  w_on = 0.0f;
  omx_eq_lv2_init(&e, g_sr);
  omx_eq_lv2_set_controls(&e, &ctl);
  run_blocks(&e, 0);
  ok(e.count == 0u && memcmp(out, in_l, sizeof out) == 0, "EQ off is the identity", e.count, 0);
  full_bank(b);
  for (int i = 0; i < NB; i++) w_band[i][4] = 0.0f;
  w_hpf[0] = w_lpf[0] = 0.0f;
  omx_eq_lv2_set_controls(&e, &ctl);
  run_blocks(&e, 0);
  ok(e.count == 0u && memcmp(out, in_l, sizeof out) == 0, "every band off is the identity", e.count, 0);
  w_band[5][0] = 0.0f, w_band[5][2] = 0.0f, w_band[5][4] = 1.0f;
  omx_eq_lv2_set_controls(&e, &ctl);
  run_blocks(&e, 0);
  ok(e.count == 1u && e.enabled[0] == 0u && memcmp(out, in_l, sizeof out) == 0, "a 0 dB bell is parked", e.count, 1);
  expect_clean();
}

static void arm_response(void) {
  g_arm = "D response";
  struct sec b[NB + 4];
  struct omx_eq_lv2 e;
  full_bank(b);
  for (int i = 0; i < NB; i++) w_band[i][4] = 0.0f;
  w_hpf[0] = w_lpf[0] = 0.0f;
  w_band[0][0] = 0.0f, w_band[0][1] = 1000.0f, w_band[0][2] = 6.0f, w_band[0][3] = 1.0f, w_band[0][4] = 1.0f;
  omx_eq_lv2_init(&e, g_sr);
  omx_eq_lv2_set_controls(&e, &ctl);
  sine(in_l, N, 1000.0, g_sr, 0.25);
  omx_eq_lv2_run(&e, in_l, out, N);
  double db = 20.0 * log10(rms(out, N / 2, N) / rms(in_l, N / 2, N));
  ok(fabs(db - 6.0) < 0.05, "bell +6 dB at its centre", db, 6.0);
  w_band[0][4] = 0.0f;
  w_hpf[0] = 1.0f, w_hpf[1] = 1000.0f, w_hpf[2] = 24.0f;
  omx_eq_lv2_set_controls(&e, &ctl);
  omx_eq_lv2_reset_state(&e);
  sine(in_l, N, 100.0, g_sr, 0.25);
  omx_eq_lv2_run(&e, in_l, out, N);
  db = 20.0 * log10(rms(out, N / 2, N) / rms(in_l, N / 2, N));
  ok(db < -70.0, "24 dB/oct HPF at 1 kHz, a decade below", db, -70.0);
  ok(OMX_EQ_LV2_LATENCY_FRAMES == 0, "latency zero", 0, 0);
  expect_clean();
}

/* ---- the instance face --------------------------------------------------------------------- */

/* The face's controls from the words full_bank() wrote, in the contract's order. */
static int f_type[NB], f_on[NB];
static float f_freq[NB], f_gain[NB], f_q[NB];
static float out_r[N];

static void face_args(void) {
  for (int i = 0; i < NB; i++)
    f_type[i] = (int)w_band[i][0], f_freq[i] = w_band[i][1], f_gain[i] = w_band[i][2], f_q[i] = w_band[i][3],
    f_on[i] = (int)w_band[i][4];
}

static void face_resolve(OmxEqInstance *s, int bypass) {
  omx_eq_instance_resolve(s, bypass, (int)w_hpf[0], w_hpf[1], (int)w_hpf[2], (int)w_lpf[0], w_lpf[1], (int)w_lpf[2],
                          f_type, f_freq, f_gain, f_q, f_on);
}

static void face_blocks(OmxEqInstance *s, const float *il, const float *ir) {
  static const uint32_t sizes[] = {1u, 64u, 127u, 256u, 33u};
  uint32_t o = 0, k = 0;
  while (o < N) {
    uint32_t b = sizes[k++ % 5u];
    if (b > N - o) b = N - o;
    omx_eq_instance_run(s, il + o, ir + o, out + o, out_r + o, b);
    o += b;
  }
}

static void arm_face(void) {
  g_arm = "E instance face";
  struct sec b[NB + 4];
  const uint32_t nb = full_bank(b);
  face_args();
  static OmxEqInstance s;
  ok(omx_eq_instance_init(&s, g_sr) == 1, "init at a declared rate", 0, 1);
  face_resolve(&s, 0);
  face_blocks(&s, in_l, in_r);
  reference(b, nb);
  ok(memcmp(out, ref, sizeof out) == 0, "the left leg equals the reference bank", 0, 0);
  /* the right leg: the same bank over the right input */
  float keep[N];
  memcpy(keep, in_l, sizeof keep);
  memcpy(in_l, in_r, sizeof in_l);
  reference(b, nb);
  memcpy(in_l, keep, sizeof in_l);
  ok(memcmp(out_r, ref, sizeof out_r) == 0, "the right leg equals the reference bank", 0, 0);
  ok(memcmp(out, in_l, sizeof out) != 0, "the bank shapes the signal", 0, 0);
  static OmxEqInstance by;
  omx_eq_instance_init(&by, g_sr);
  face_resolve(&by, 1);
  face_blocks(&by, in_l, in_r);
  ok(memcmp(out, in_l, sizeof out) == 0 && memcmp(out_r, in_r, sizeof out_r) == 0, "bypassed is a wire", 0, 0);
  static OmxEqInstance bad;
  ok(omx_eq_instance_init(&bad, g_sr + 1.0f) == 0, "an undeclared rate is refused", 0, 0);
  face_blocks(&bad, in_l, in_r);
  ok(memcmp(out, in_l, sizeof out) == 0 && memcmp(out_r, in_r, sizeof out_r) == 0, "a refused instance is a wire", 0, 0);
  ok(omx_eq_instance_latency(&s) == 0u && OMX_EQ_INSTANCE_LATENCY_FRAMES == 0.0f, "latency zero", 0, 0);
  expect_clean();
}

static void arm_face_defaults(void) {
  g_arm = "F face defaults";
  struct sec b[NB + 4];
  full_bank(b);
  face_args();
  static OmxEqInstance s, e;
  omx_eq_instance_init(&s, g_sr);
  omx_eq_instance_init(&e, g_sr);
  int bt[NB], bo[NB], wt[NB], wo[NB];
  float bf[NB], bg[NB], bq[NB], wf[NB], wg[NB], wq[NB];
  float centres[NB];
  omx_eq_default_centres((unsigned)NB, centres);
  for (int i = 0; i < NB; i++) {
    bt[i] = i % 3 ? f_type[i] : 99, wt[i] = i % 3 ? f_type[i] : (int)OMX_EQ_BAND_TYPES_DEFAULT;
    bo[i] = i % 4 ? f_on[i] : -3, wo[i] = i % 4 ? f_on[i] : 0;
    bf[i] = i % 5 ? f_freq[i] : NAN, wf[i] = i % 5 ? f_freq[i] : centres[i];
    bg[i] = i % 6 ? f_gain[i] : INFINITY, wg[i] = i % 6 ? f_gain[i] : (float)OMX_EQ_BAND_DEFAULTS_GAIN_DB;
    bq[i] = i % 7 ? f_q[i] : NAN, wq[i] = i % 7 ? f_q[i] : (float)OMX_EQ_BAND_DEFAULTS_Q;
  }
  omx_eq_instance_resolve(&s, 0, 7, NAN, 18, 2, INFINITY, -1, bt, bf, bg, bq, bo);
  omx_eq_instance_resolve(&e, 0, (int)OMX_EQ_HPF_ONS_DEFAULT, (float)OMX_EQ_PASS_FILTER_DEFAULTS_HPF_FREQ_HZ,
                          (int)OMX_FILTER_SLOPES_DEFAULT, (int)OMX_EQ_LPF_ONS_DEFAULT,
                          (float)OMX_EQ_PASS_FILTER_DEFAULTS_LPF_FREQ_HZ, (int)OMX_FILTER_SLOPES_DEFAULT, wt, wf, wg, wq, wo);
  face_blocks(&s, in_l, in_r);
  memcpy(ref, out, sizeof ref);
  face_blocks(&e, in_l, in_r);
  ok(memcmp(out, ref, sizeof out) == 0, "words outside their ids or non-finite read as the declared defaults", 0, 0);
  expect_clean();
}

static void arm_face_no_alloc(void) {
  g_arm = "H no allocation";
  struct sec b[NB + 4];
  full_bank(b);
  face_args();
  static OmxEqInstance s;
  g_allocs = 0;
  g_counting = 1;
  const int ready = omx_eq_instance_init(&s, g_sr);
  face_resolve(&s, 0);
  omx_eq_instance_run(&s, in_l, in_r, out, out_r, 512u);
  face_resolve(&s, 1);
  omx_eq_instance_run(&s, in_l, in_r, out, out_r, 512u);
  f_gain[2] = 6.0f;
  face_resolve(&s, 0);
  omx_eq_instance_run(&s, in_l, in_r, out, out_r, 512u);
  g_counting = 0;
  ok(ready && g_allocs == 0, "init, resolve, a re-engage and run allocate nothing", (double)g_allocs, 0);
  void *(*volatile alloc)(size_t) = malloc;
  void (*volatile release)(void *) = free;
  g_allocs = 0;
  g_counting = 1;
  release(alloc(16));
  g_counting = 0;
  ok(g_allocs == 2, "the allocation counter counts (a malloc and a free inside the window)", (double)g_allocs, 2);
  expect_clean();
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  wire();
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = OMX_DECLARED_RATES[ri];
    stimulus(in_l, in_r, N, 0x1234567u, 0u);
    arm_identity();
    arm_clamps();
    arm_identity_paths();
    stimulus(in_l, in_r, N, 0x1234567u, 0u);
    arm_response();
    stimulus(in_l, in_r, N, 0x1234567u, 0u);
    arm_face();
    arm_face_defaults();
    arm_face_no_alloc();
  }
  return instance_oracle_end("eq_instance");
}
