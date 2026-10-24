// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* ---- the Doppler read head (omx_doppler.h) at the nine oracle rates ------------------------- */
#include <omxdsp/omx_doppler.h>

/* The frequency of y[from, to) near `f`, Hz: the phase of 5 ms blocks demodulated at `f`, unwrapped
 * and fitted by least squares; the slope is the offset from `f`. */
static double doppler_freq(const float *y, uint32_t from, uint32_t to, double f, double sr) {
  const uint32_t b = (uint32_t)(sr / 200.0);
  double sx = 0, sy = 0, sxx = 0, sxy = 0, prev = 0, unwrap = 0;
  int k = 0;
  for (uint32_t o = from; o + b <= to; o += b, k++) {
    double re = 0, im = 0;
    for (uint32_t i = o; i < o + b; i++) {
      re += y[i] * cos(2.0 * M_PI * f * i / sr);
      im -= y[i] * sin(2.0 * M_PI * f * i / sr);
    }
    double ph = atan2(im, re);
    if (k > 0) {
      while (ph + unwrap - prev > M_PI) unwrap -= 2.0 * M_PI;
      while (ph + unwrap - prev < -M_PI) unwrap += 2.0 * M_PI;
    }
    ph += unwrap;
    prev = ph;
    const double t = (o + 0.5 * b) / sr;
    sx += t;
    sy += ph;
    sxx += t * t;
    sxy += t * ph;
  }
  const double slope = (k * sxy - sx * sy) / (k * sxx - sx * sx);
  return f - slope / (2.0 * M_PI);
}

static void arm_doppler(void) {
  g_arm = "doppler";
  static const float RATES[] = OMX_ORACLE_FULL_RATES_INIT;
  static const double CENTS[] = {-25.0, -8.0, -1.0, 1.0, 8.0, 25.0};
  enum { MAXN = 400000, CAP = 16384 };
  static float x[MAXN], y[MAXN], ring[CAP];
  static uint8_t single[MAXN];
  double worst = 0.0;
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const double sr = RATES[ri];
    const uint32_t n = (uint32_t)(sr * 2.0 > MAXN ? MAXN : sr * 2.0);
    const float window = (float)(0.02 * sr), mean = (float)(0.012 * sr) + 0.5f * window;
    const double f = 997.0;
    for (uint32_t i = 0; i < n; i++) x[i] = (float)(0.5 * sin(2.0 * M_PI * f * i / sr));
    for (uint32_t c = 0; c < 6u; c++) {
      struct omx_fdelay line;
      memset(ring, 0, sizeof ring);
      omx_fdelay_init(&line, ring, CAP, 3);
      struct omx_doppler d;
      struct omx_doppler_state st;
      omx_doppler_design(&d, CENTS[c], window, 0.25f);
      omx_doppler_start(&d, &st);
      float peak = 0.0f;
      for (uint32_t i = 0; i < n; i++) {
        single[i] = st.pos < (1.0f - d.fade) * d.window;
        omx_fdelay_write(&line, x[i]);
        y[i] = omx_doppler_read(&line, &d, &st, mean);
        peak = fabsf(y[i]) > peak ? fabsf(y[i]) : peak;
      }
      /* the first stretch read by one head alone, once the line holds a full window */
      const uint32_t settle = (uint32_t)(mean + window) + 8u;
      uint32_t a = settle, b2;
      while (a < n && !single[a]) a++;
      b2 = a;
      while (b2 < n && single[b2]) b2++;
      const double want = f * pow(2.0, CENTS[c] / 1200.0);
      const double got = doppler_freq(y, a, b2, want, sr);
      const double err = 1200.0 * log2(got / want);
      worst = fmax(worst, fabs(err));
      ok(b2 - a > (uint32_t)(0.2 * sr), "a single-head stretch of at least 200 ms to measure", b2 - a, 0.2 * sr);
      ok(fabs(err) < 0.1, "one head comes out at f 2^(c/1200), within 0.1 cent", err, 0.1);
      ok(peak <= 0.5f * 1.0001f, "the crossfade never rises above the input", peak, 0.5);
      /* the hand-over between heads is a crossfade, not a jump: no step between samples larger than
       * the detuned sine's own steepest one, the fade's own slope included (10 %) */
      double jump = 0.0;
      for (uint32_t i = settle + 1u; i < n; i++) {
        const double e = fabs((double)y[i] - y[i - 1u]);
        if (!(e <= jump)) jump = e;
      }
      const double slope = 0.5 * 2.0 * M_PI * want / sr;
      ok(jump <= 1.1 * slope, "no step between samples larger than the sine's own, through every hand-over", jump, 1.1 * slope);
    }
    /* At 0 cents the head stays at the centre: the line read at the mean delay, bit for bit. */
    struct omx_fdelay a, b;
    static float ring_b[CAP];
    memset(ring, 0, sizeof ring);
    memset(ring_b, 0, sizeof ring_b);
    omx_fdelay_init(&a, ring, CAP, 3);
    omx_fdelay_init(&b, ring_b, CAP, 3);
    struct omx_doppler d;
    struct omx_doppler_state st;
    omx_doppler_design(&d, 0.0, window, 0.25f);
    omx_doppler_start(&d, &st);
    int same = 1;
    for (uint32_t i = 0; i < 20000u; i++) {
      omx_fdelay_write(&a, x[i]);
      omx_fdelay_write(&b, x[i]);
      same &= omx_doppler_read(&a, &d, &st, mean - 0.37f) == omx_fdelay_read(&b, mean - 0.37f);
    }
    ok(same && d.step == 0.0f, "at 0 cents the head is the line read at the mean delay, bit for bit", same, 1);
  }
  printf("  doppler: worst detune error %.4f cents over nine rates\n", worst);
  expect_clean();
}
