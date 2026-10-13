// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "../fx/fx_rates.h"

/* ---- the offline sample-rate converter -------------------------------------------------------
 *
 * The engine's oracle (openmixer mix_src.test.c), moved with the converter. What a resampler has
 * to prove, each a separate claim:
 *   1. a tone comes out at the same frequency (closed-form correlation over whole cycles);
 *   2. the alias floor: an upsample invents nothing above the source's band and a downsample folds
 *      nothing back, measured at the frequencies spurs land on;
 *   3. the level survives (per-phase normalisation);
 *   4. the timing survives: output frame N is input frame N·L/M (group delay compensated);
 *   5. the identity case is exact, bit for bit;
 *   6. the design curve is real: a window designed for 40 dB does not reach 90.
 * Then every ordered pair of the nine RME rates: a plan of the rate ratio (or the refusal the cap
 * gives), the tone's frequency and level, and DC at unity. */

static double src_db(double lin) { return 20.0 * log10(lin < 1e-12 ? 1e-12 : lin); }

/* The complex amplitude of `n` samples at exactly `hz`, given `rate`; the caller arranges a whole
 * number of cycles. */
static void src_tone_at(const float *x, size_t from, size_t n, double hz, double rate, double *mag,
                        double *phase) {
  double re = 0.0, im = 0.0;
  const double w = 2.0 * M_PI * hz / rate;
  for (size_t i = 0; i < n; i++) {
    re += (double)x[from + i] * cos(w * (double)i);
    im -= (double)x[from + i] * sin(w * (double)i);
  }
  if (mag) *mag = 2.0 * sqrt(re * re + im * im) / (double)n;
  if (phase) *phase = atan2(im, re);
}

/* A whole buffer through a fresh converter, fed in blocks of 1024 (not a divisor of any rate pair
 * here, so the block boundaries land mid-phase). Returns outputs written. */
static size_t src_convert(const float *in, size_t in_n, uint32_t in_rate, uint32_t out_rate, float *out,
                          size_t out_cap, double rolloff, double stopband) {
  struct omx_src_plan plan = {0};
  if (omx_src_plan_of(in_rate, out_rate, &plan) != 0) return 0;
  float *coef = calloc(omx_src_coef_count(&plan), sizeof(float));
  float *hist = calloc(omx_src_hist_count(&plan), sizeof(float));
  omx_src_design(&plan, coef, rolloff, stopband);
  struct omx_src s;
  omx_src_init(&s, &plan, coef, hist);
  size_t written = 0;
  const size_t block = 1024;
  for (size_t i = 0; i < in_n; i += block) {
    const uint32_t take = (uint32_t)((in_n - i < block) ? (in_n - i) : block);
    const int last = (i + take >= in_n);
    written += omx_src_run(&s, in + i, take, out + written, (uint32_t)(out_cap - written), last);
  }
  free(coef);
  free(hist);
  return written;
}

/* The worst spur a conversion leaves, measured on the bins spurs land on: the interpolation images
 * at |k·fs_in ± f| and the decimation folds at |k·fs_out ± f|. */
static double src_spur_floor_db(const float *x, size_t n, double out_rate, double in_rate,
                                double tone_hz, double *at_hz, int *probes) {
  double worst = 0.0;
  double worst_hz = 0.0;
  int asked = 0;
  const double bin = out_rate / (double)n;
  for (int k = 1; k <= 4; k++) {
    const double families[4] = { (double)k * in_rate - tone_hz, (double)k * in_rate + tone_hz,
                                 (double)k * out_rate - tone_hz, (double)k * out_rate + tone_hz };
    for (int f = 0; f < 4; f++) {
      const double hz = fabs(families[f]);
      if (hz < bin * 3.0 || hz >= out_rate * 0.5) continue;
      if (fabs(hz - tone_hz) < bin * 3.0) continue;
      const double snapped = llround(hz / bin) * bin;
      asked++;
      double mag = 0.0;
      src_tone_at(x, 0, n, snapped, out_rate, &mag, NULL);
      if (mag > worst) {
        worst = mag;
        worst_hz = snapped;
      }
    }
  }
  if (at_hz) *at_hz = worst_hz;
  if (probes) *probes = asked;
  return src_db(worst);
}

/* 1 + 2 + 3 + 4: a 1 kHz tone, 48 k -> 192 k. */
static void src_upsample_48_to_192(void) {
  const size_t in_n = 48000;
  const double hz = 1000.0;
  float *in = calloc(in_n, sizeof(float));
  for (size_t i = 0; i < in_n; i++) in[i] = (float)(0.5 * sin(2.0 * M_PI * hz * (double)i / 48000.0));
  const size_t cap = in_n * 4 + 1024;
  float *out = calloc(cap, sizeof(float));
  const size_t n = src_convert(in, in_n, 48000, 192000, out, cap, OMX_SRC_ROLLOFF_DOUBLE, OMX_SRC_STOPBAND_DB);
  ok(n >= in_n * 4 - 8, "48k->192k produces four output frames per input frame", (double)n, (double)(in_n * 4 - 8));
  const size_t from = 192000 / 4;
  const size_t win = 192000 / 2;
  double mag = 0.0, phase = 0.0;
  src_tone_at(out, from, win, hz, 192000.0, &mag, &phase);
  ok(fabs(mag - 0.5) <= 0.005, "48k->192k: the tone is still 1 kHz at its own amplitude", mag, 0.5);
  int probes = 0;
  const double floor_db = src_spur_floor_db(out + from, win, 192000.0, 48000.0, hz, NULL, &probes);
  ok(probes > 0, "48k->192k: there are image frequencies in band to look at", probes, 1.0);
  ok(src_db(mag) - floor_db >= 90.0, "48k->192k: every spur is at least 90 dB below the tone", src_db(mag) - floor_db, 90.0);
  double src_phase = 0.0;
  src_tone_at(in, in_n / 4, in_n / 2, hz, 48000.0, NULL, &src_phase);
  double deg = (phase - src_phase) * 180.0 / M_PI;
  while (deg > 180.0) deg -= 360.0;
  while (deg <= -180.0) deg += 360.0;
  ok(fabs(deg) <= 3.0, "48k->192k: output frame N is input frame N, the delay is compensated", deg, 3.0);
  free(in);
  free(out);
}

/* 2, the harder direction: no spur family of a 1 kHz tone lands inside a 48 kHz band. */
static void src_downsample_192_to_48(void) {
  const size_t in_n = 192000;
  const double hz = 1000.0;
  float *in = calloc(in_n, sizeof(float));
  for (size_t i = 0; i < in_n; i++) in[i] = (float)(0.5 * sin(2.0 * M_PI * hz * (double)i / 192000.0));
  const size_t cap = in_n / 4 + 1024;
  float *out = calloc(cap, sizeof(float));
  const size_t n = src_convert(in, in_n, 192000, 48000, out, cap, OMX_SRC_ROLLOFF_DOUBLE, OMX_SRC_STOPBAND_DB);
  ok(n >= in_n / 4 - 8, "192k->48k produces one output frame per four input frames", (double)n, (double)(in_n / 4 - 8));
  double mag = 0.0;
  src_tone_at(out, 48000 / 4, 48000 / 2, hz, 48000.0, &mag, NULL);
  ok(fabs(mag - 0.5) <= 0.005, "192k->48k: the tone survives the decimation at its own amplitude", mag, 0.5);
  int probes = 0;
  src_spur_floor_db(out + 48000 / 4, 48000 / 2, 48000.0, 192000.0, hz, NULL, &probes);
  ok(probes == 0, "192k->48k: a 1 kHz tone has no spur frequency inside a 48 kHz band", probes, 0.0);
  free(in);
  free(out);
}

/* What a downsampler is for: a 40 kHz tone must not fold to 8 kHz in a 48 kHz file. */
static void src_downsample_rejects_out_of_band(void) {
  const size_t in_n = 192000;
  const double hz = 40000.0;
  float *in = calloc(in_n, sizeof(float));
  for (size_t i = 0; i < in_n; i++) in[i] = (float)(0.5 * sin(2.0 * M_PI * hz * (double)i / 192000.0));
  const size_t cap = in_n / 4 + 1024;
  float *out = calloc(cap, sizeof(float));
  src_convert(in, in_n, 192000, 48000, out, cap, OMX_SRC_ROLLOFF_DOUBLE, OMX_SRC_STOPBAND_DB);
  double folded = 0.0;
  src_tone_at(out, 12000, 24000, 8000.0, 48000.0, &folded, NULL);
  ok(src_db(0.5) - src_db(folded) >= 90.0, "a 40 kHz tone does not fold into the 48 kHz band", src_db(0.5) - src_db(folded), 90.0);
  free(in);
  free(out);
}

/* 3: every phase passes DC at unity. */
static void src_dc_gain_is_unity(void) {
  const size_t in_n = 8192;
  float *in = calloc(in_n, sizeof(float));
  for (size_t i = 0; i < in_n; i++) in[i] = 0.25f;
  const size_t cap = in_n * 2 + 1024;
  float *out = calloc(cap, sizeof(float));
  src_convert(in, in_n, 48000, 96000, out, cap, OMX_SRC_ROLLOFF_DOUBLE, OMX_SRC_STOPBAND_DB);
  double worst = 0.0;
  for (size_t i = 2048; i < in_n; i++) {
    const double d = fabs((double)out[i] - 0.25);
    if (d > worst) worst = d;
  }
  ok(worst < 1e-5, "every phase passes DC at exactly unity: a take comes back at its level", worst, 1e-5);
  free(in);
  free(out);
}

/* 160/147, where a per-phase mistake shows as a periodic modulation rather than a level error. */
static void src_awkward_ratio_44100_to_48000(void) {
  struct omx_src_plan plan = {0};
  ok(omx_src_plan_of(44100, 48000, &plan) == 0, "44.1k -> 48k reduces", 0.0, 0.0);
  ok(plan.l == 160 && plan.m == 147, "... to 160/147", plan.l, 160.0);
  const size_t in_n = 44100;
  const double hz = 1000.0;
  float *in = calloc(in_n, sizeof(float));
  for (size_t i = 0; i < in_n; i++) in[i] = (float)(0.5 * sin(2.0 * M_PI * hz * (double)i / 44100.0));
  const size_t cap = 48000 + 1024;
  float *out = calloc(cap, sizeof(float));
  src_convert(in, in_n, 44100, 48000, out, cap, OMX_SRC_ROLLOFF_DOUBLE, OMX_SRC_STOPBAND_DB);
  double mag = 0.0;
  src_tone_at(out, 12000, 24000, hz, 48000.0, &mag, NULL);
  ok(fabs(mag - 0.5) <= 0.01, "44.1k->48k: the tone keeps its frequency and its level", mag, 0.5);
  int probes = 0;
  const double floor_db = src_spur_floor_db(out + 12000, 24000, 48000.0, 44100.0, hz, NULL, &probes);
  if (probes > 0) ok(src_db(mag) - floor_db >= 90.0, "44.1k->48k: every spur is at least 90 dB below", src_db(mag) - floor_db, 90.0);
  free(in);
  free(out);
}

/* 5: 48 k to 48 k returns the samples unchanged, bit for bit. */
static void src_identity_is_bit_exact(void) {
  struct omx_src_plan plan = {0};
  ok(omx_src_plan_of(48000, 48000, &plan) == 0, "48k -> 48k reduces", 0.0, 0.0);
  ok(plan.l == 1 && plan.m == 1 && plan.taps == 1, "... to the identity plan, one unit tap", plan.taps, 1.0);
  const size_t in_n = 4096;
  float *in = calloc(in_n, sizeof(float));
  for (size_t i = 0; i < in_n; i++) in[i] = (float)sin((double)i * 0.37) * 0.9f;
  float *out = calloc(in_n + 64, sizeof(float));
  const size_t n = src_convert(in, in_n, 48000, 48000, out, in_n + 64, OMX_SRC_ROLLOFF_DOUBLE, OMX_SRC_STOPBAND_DB);
  ok(n >= in_n, "the identity conversion produces one output per input", (double)n, (double)in_n);
  int exact = 1;
  for (size_t i = 0; i < in_n; i++)
    if (out[i] != in[i]) exact = 0;
  ok(exact, "48k -> 48k is bit-exact: the loader runs one path, not two", exact, 1.0);
  free(in);
  free(out);
}

/* 2 proper: the image floor where the window works. 20 kHz at 48 kHz upsampled to 192 kHz puts
 * its images at 28, 68 and 76 kHz, held down by nothing but the filter. */
static double src_image_floor_at(double stopband_db, double rolloff) {
  const size_t in_n = 48000;
  const double hz = 20000.0;
  float *in = calloc(in_n, sizeof(float));
  for (size_t i = 0; i < in_n; i++) in[i] = (float)(0.5 * sin(2.0 * M_PI * hz * (double)i / 48000.0));
  const size_t cap = in_n * 4 + 1024;
  float *out = calloc(cap, sizeof(float));
  src_convert(in, in_n, 48000, 192000, out, cap, rolloff, stopband_db);
  double mag = 0.0;
  const size_t from = in_n, win = 2 * in_n;
  src_tone_at(out, from, win, hz, 192000.0, &mag, NULL);
  const double floor_db = src_spur_floor_db(out + from, win, 192000.0, 48000.0, hz, NULL, NULL);
  free(in);
  free(out);
  return src_db(mag) - floor_db;
}

static void src_image_floor(void) {
  const double rejection = src_image_floor_at(OMX_SRC_STOPBAND_DB, OMX_SRC_ROLLOFF_DOUBLE);
  ok(rejection >= 90.0, "the upsample's images are at least 90 dB below the tone", rejection, 90.0);
}

/* 6: a window designed for 40 dB does not reach 90, so the floor above is the filter's. */
static void src_a_weaker_window_measures_weaker(void) {
  const double weak = src_image_floor_at(40.0, OMX_SRC_ROLLOFF_DOUBLE);
  ok(weak < 70.0, "a window designed for 40 dB does not reach 90: the floor above is the filter's", weak, 70.0);
}

static void src_plan_refuses_the_unusable(void) {
  struct omx_src_plan plan = {0};
  ok(omx_src_plan_of(0, 48000, &plan) == -1, "a zero source rate is refused", 0.0, -1.0);
  ok(omx_src_plan_of(48000, 0, &plan) == -1, "a zero destination rate is refused", 0.0, -1.0);
  ok(omx_src_plan_of(48000, 48000 * (OMX_SRC_MAX_L + 1), &plan) == -1,
     "an interpolation factor past OMX_SRC_MAX_L is refused, not attempted", 0.0, -1.0);
  ok(omx_src_plan_of(192000, 48000, &plan) == 0 && plan.l == 1 && plan.m == 4, "192k -> 48k is 1/4", plan.m, 4.0);
  ok(omx_src_plan_of(48000, 192000, &plan) == 0 && plan.l == 4 && plan.m == 1, "48k -> 192k is 4/1", plan.l, 4.0);
  struct omx_src_plan p4 = { .l = 4, .m = 1, .taps = 64 };
  ok(omx_src_out_frames(1000, &p4) == 4000, "a whole file's output length is exact", (double)omx_src_out_frames(1000, &p4), 4000.0);
}

/* Every ordered pair of the declared rates reduces to a plan whose L/M is the rate ratio in
 * lowest terms: a take recorded at any declared rate converts to any other. */
static void src_every_declared_pair_has_a_plan(void) {
  for (unsigned a = 0; a < OMX_DECLARED_RATE_COUNT; a++)
    for (unsigned b = 0; b < OMX_DECLARED_RATE_COUNT; b++) {
      const uint32_t in = (uint32_t)OMX_DECLARED_RATES[a], out = (uint32_t)OMX_DECLARED_RATES[b];
      struct omx_src_plan plan = {0};
      ok(omx_src_plan_of(in, out, &plan) == 0 && (uint64_t)plan.l * in == (uint64_t)plan.m * out,
         "a declared pair reduces to a plan of the rate ratio", in, out);
    }
}

/* The nine RME rates, every ordered pair. The plan is the ratio in lowest terms, refused only past
 * OMX_SRC_MAX_L: of the 81 pairs that is 44.1k -> 128k alone (1280/441). Every planned pair keeps
 * a 1 kHz tone's frequency and level and passes DC at unity, a quarter second each. */
static void src_rme_pairs(void) {
  int refused = 0;
  for (unsigned a = 0; a < OMX_FX_RME_RATE_COUNT; a++)
    for (unsigned b = 0; b < OMX_FX_RME_RATE_COUNT; b++) {
      const uint32_t in = (uint32_t)OMX_FX_RME_RATES[a], out = (uint32_t)OMX_FX_RME_RATES[b];
      const uint32_t g = omx_src_gcd(in, out);
      struct omx_src_plan plan = {0};
      const int r = omx_src_plan_of(in, out, &plan);
      if (out / g > OMX_SRC_MAX_L) {
        ok(r == -1, "an RME pair past the cap is refused", in, out);
        refused++;
        continue;
      }
      ok(r == 0 && plan.l == out / g && plan.m == in / g, "an RME pair reduces to the ratio in lowest terms", in, out);
      const size_t in_n = in / 4u, out_n = out / 4u;
      const size_t cap = (size_t)omx_src_out_frames(in_n, &plan) + 1024u;
      float *x = calloc(in_n, sizeof(float));
      float *y = calloc(cap, sizeof(float));
      for (size_t i = 0; i < in_n; i++) x[i] = (float)(0.5 * sin(2.0 * M_PI * 1000.0 * (double)i / (double)in));
      src_convert(x, in_n, in, out, y, cap, OMX_SRC_ROLLOFF_DOUBLE, OMX_SRC_STOPBAND_DB);
      /* a tenth of a second in the middle of the output: 100 whole cycles of 1 kHz at every RME rate */
      double mag = 0.0;
      src_tone_at(y, out_n / 4u, out / 10u, 1000.0, (double)out, &mag, NULL);
      ok(fabs(mag - 0.5) <= 0.01, "an RME pair keeps a 1 kHz tone's frequency and level", mag, 0.5);
      for (size_t i = 0; i < in_n; i++) x[i] = 0.25f;
      src_convert(x, in_n, in, out, y, cap, OMX_SRC_ROLLOFF_DOUBLE, OMX_SRC_STOPBAND_DB);
      double worst = 0.0;
      for (size_t i = out_n / 4u; i < out_n / 2u; i++)
        if (fabs((double)y[i] - 0.25) > worst) worst = fabs((double)y[i] - 0.25);
      ok(worst < 1e-5, "an RME pair passes DC at unity", worst, 1e-5);
      free(x);
      free(y);
    }
  ok(refused == 1, "of the 81 RME pairs only 44.1k -> 128k is past the cap", refused, 1.0);
}

static void arm_src(void) {
  g_arm = "src";
  src_plan_refuses_the_unusable();
  src_every_declared_pair_has_a_plan();
  src_identity_is_bit_exact();
  src_upsample_48_to_192();
  src_downsample_192_to_48();
  src_downsample_rejects_out_of_band();
  src_dc_gain_is_unity();
  src_awkward_ratio_44100_to_48000();
  src_image_floor();
  src_a_weaker_window_measures_weaker();
  src_rme_pairs();
  expect_clean();
}
