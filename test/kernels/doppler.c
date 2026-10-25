// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* ---- the Doppler read head (omx_doppler.h) at the nine oracle rates ------------------------- */
#include <omxdsp/omx_doppler.h>

#include <float.h>

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

/* The fade law and the hand-over law in closed form. The line carries a ramp x[i] = i / 4096, every
 * sample an exact float, which order-3 Lagrange reads exactly, so a read at delay d at sample n is
 * (n - d) / 4096 and the head's output is a closed form of its delays and weights. The position runs
 * the header's law in double here: from W/2, + step a sample, - (1 - F) W at W; the delays are
 * D + sign (p - W/2) and D + sign (q - W/2), q = p - (1 - F) W, the weight q / (F W). The bound is
 * derived: a read's rounding, four products and three sums over coefficients each a few ulp off,
 * within 24 ulp of the largest sample read; the delay's float rounding, 1 ulp of D + W, times the
 * ramp's slope; the fade's product and sum, 3 ulp; 32 ulp of the largest sample in all, plus the
 * delay term. */
#define DOPPLER_RAMP (1.0 / 4096.0)
struct doppler_law {
  double pos, w, fade, step, sign, mean;
};
static void doppler_law_delays(const struct doppler_law *h, double *d0, double *d1, double *g) {
  const double start = (1.0 - h->fade) * h->w;
  *d0 = h->mean + h->sign * (h->pos - 0.5 * h->w);
  *d1 = *d0;
  *g = 0.0;
  if (h->pos >= start) {
    const double q = h->pos - start;
    *d1 = h->mean + h->sign * (q - 0.5 * h->w);
    *g = q / (h->fade * h->w);
  }
}
static void doppler_law_step(struct doppler_law *h) {
  h->pos += h->step;
  if (h->pos >= h->w) h->pos -= (1.0 - h->fade) * h->w;
}
static double doppler_ramp_at(double n, double d) { return (n - d) * DOPPLER_RAMP; }

static void doppler_laws(double sr, float *ring, uint32_t cap) {
  enum { N = 200000 };
  const float window = (float)(0.02 * sr), mean = (float)(0.012 * sr) + 0.5f * window;
  const uint32_t n = (uint32_t)(sr * 1.0 > N ? N : sr * 1.0), settle = (uint32_t)(mean + window) + 8u;
  struct omx_fdelay line;
  memset(ring, 0, cap * sizeof(float));
  omx_fdelay_init(&line, ring, cap, 3);
  struct omx_doppler d;
  struct omx_doppler_state st;
  omx_doppler_design(&d, 25.0, window, 0.25f);
  omx_doppler_start(&d, &st);
  struct doppler_law h = {0.5 * window, window, d.fade, d.step, d.sign, mean};
  /* the hand-over: at n / 2 the head moves to -8 cents over a 1.4x window at a mean 4 ms longer, and
   * fades from the held read over 0.3 of the new window */
  const uint32_t at = n / 2u;
  const float window2 = 1.4f * window, mean2 = mean + (float)(0.004 * sr), len = 0.3f * window2;
  struct omx_doppler d2;
  omx_doppler_design(&d2, -8.0, window2, 0.25f);
  struct doppler_law h2 = {0.5 * window2, window2, d2.fade, d2.step, d2.sign, mean2};
  double held0 = 0, held1 = 0, heldg = 0, prev0 = 0, prev1 = 0, prevg = 0, worst_fade = 0, worst_hand = 0;
  uint32_t fades = 0u, hands = 0u, bad = 0u;
  for (uint32_t i = 0; i < n; i++) {
    omx_fdelay_write(&line, (float)(i * DOPPLER_RAMP));
    if (i == at) {
      held0 = prev0; /* the last read, one sample back */
      held1 = prev1;
      heldg = prevg;
      omx_doppler_hand_over(&d2, &st, len);
    }
    const int handing = i >= at && omx_doppler_handing_over(&st);
    const float y = omx_doppler_read(&line, i < at ? &d : &d2, &st, i < at ? mean : mean2);
    struct doppler_law *law = i < at ? &h : &h2;
    double d0, d1, g;
    doppler_law_delays(law, &d0, &d1, &g);
    prev0 = d0;
    prev1 = d1;
    prevg = g;
    double want = (1.0 - g) * doppler_ramp_at(i, d0) + g * doppler_ramp_at(i, d1);
    if (handing) {
      const double hw = 1.0 - (double)(i - at) / (double)len;
      const double yh = (1.0 - heldg) * doppler_ramp_at(i, held0) + heldg * doppler_ramp_at(i, held1);
      want = (1.0 - hw) * want + hw * yh;
    }
    if (i < at) doppler_law_step(&h);
    else doppler_law_step(&h2);
    if (i < settle) continue;
    const double big = i * DOPPLER_RAMP;
    const double tol = 32.0 * FLT_EPSILON * big + DOPPLER_RAMP * FLT_EPSILON * (mean2 + window2);
    const double e = fabs((double)y - want);
    if (handing) {
      hands++;
      worst_hand = fmax(worst_hand, e / tol);
    } else if (g > 0.0) {
      fades++;
      worst_fade = fmax(worst_fade, e / tol);
    }
    bad += !(e <= tol);
  }
  printf("  doppler %6.0f Hz: fade law worst %.3f of its bound over %u samples, hand-over law worst %.3f over %u\n", sr,
         worst_fade, fades, worst_hand, hands);
  ok(bad == 0u && fades > 0u, "a head's crossfade follows its declared linear law (derived rounding bound)", bad, 0);
  ok(hands + 1u >= (uint32_t)len && hands <= (uint32_t)len + 1u, "a hand-over fades over the length it was given", hands, len);
  /* a crossfade of two reads at the same delay is the identity: a still head handed over to itself */
  struct omx_fdelay b;
  static float ring_b[16384];
  memset(ring, 0, cap * sizeof(float));
  memset(ring_b, 0, sizeof ring_b);
  omx_fdelay_init(&line, ring, cap, 3);
  omx_fdelay_init(&b, ring_b, 16384u, 3);
  struct omx_doppler z;
  omx_doppler_design(&z, 0.0, window, 0.25f);
  omx_doppler_start(&z, &st);
  int same = 1;
  for (uint32_t i = 0; i < 20000u; i++) {
    const float x = (float)sin(0.01 * i);
    omx_fdelay_write(&line, x);
    omx_fdelay_write(&b, x);
    if (i == 9000u) omx_doppler_hand_over(&z, &st, 500.0f);
    same &= omx_doppler_read(&line, &z, &st, mean - 0.37f) == omx_fdelay_read(&b, mean - 0.37f);
  }
  ok(same, "a crossfade of two reads at the same delay is the identity, bit for bit", same, 1);
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
       * its bound, a read's own steepest step A w (1 + s), w the sine's radian frequency and s the
       * head's slope, plus the fade's, its weight moving 1 / (F W) a sample across two reads at most
       * 2A apart */
      double jump = 0.0;
      for (uint32_t i = settle + 1u; i < n; i++) {
        const double e = fabs((double)y[i] - y[i - 1u]);
        if (!(e <= jump)) jump = e;
      }
      const double bound = 0.5 * 2.0 * M_PI * f * (1.0 + d.step) / sr + 2.0 * 0.5 / (d.fade * d.window);
      ok(jump <= bound, "no step between samples past A w (1 + s) + 2A / (F W), through every fade", jump, bound);
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
    doppler_laws(sr, ring, CAP);
  }
  printf("  doppler: worst detune error %.4f cents over nine rates\n", worst);
  expect_clean();
}
