// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The vocal doubler's instance oracle (omx_doubler_instance.h) at each of the nine oracle rates
// (OMX_FX_ORACLE_RATES, 32 to 192 kHz):
//   A  a mix of 0 is the input, bit for bit, every setting;
//   B  the detune: one voice, no drift, a sine read by one head comes out at
//      f x 2^(detuneCents x DOUBLER_VOICE_DETUNES[0] / 1200) within 0.1 cent;
//   C  the delay: each voice's mean delay is delayMs x DOUBLER_VOICE_DELAYS[n]; an undetuned, undrifting
//      voice's impulse response has its centroid there (0.05 samples); the ring holds the longest voice
//      with its drift and sweep at 192 kHz;
//   D  the drift: an undetuned voice's delay swings modDepthMs either side of its mean, within the
//      delay's top slope 2 pi modRateHz modDepthMs over one 5 ms measuring window, its swings
//      1 / modRateHz apart within two windows; of two voices the second drifts half a turn along;
//   E  the place: still voices' impulses land on the legs by the linear balance law of
//      spread x DOUBLER_VOICE_PANS[n] over the voice count, written out here (8 ulp);
//   F  hostile words land inside the declared travels, the ledger is clean, nothing allocates;
//   G  a sweep of delayMs and modDepthMs across the room limit, resolved every 64 samples, steps no
//      sample by more than its bound: each read's own steepest step A w (1 + s + r), w the sine's
//      radian frequency, s the head's slope and r the drift's, plus each crossfade's, 2A over its
//      length, for the head's fade (DOUBLER_FADE x its window) and the hand-over (DOUBLER_FADE x
//      DOUBLER_WINDOW_MS), summed over the voices at their gains;
//   H  through that sweep every voice's lowest read stays one sample clear of the kernel's reach;
//   I  on a ramp, which the line reads exactly, the voice's crossfade is DOUBLER_FADE of
//      DOUBLER_WINDOW_MS, linear, and a changed voice hands over linearly across DOUBLER_FADE x
//      DOUBLER_WINDOW_MS from its held read, both in closed form within a derived rounding bound.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_doubler_instance.h>

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

/* The declarations, stated here so a moved one turns its arm red (doubler_instance-perturb.sh). */
static const double DETUNES[4] = {1, -1, 0.5, -0.5};
static const double DELAYS[4] = {1, 1.31, 0.78, 1.17};
static const double PANS[4] = {-1, 1, -0.5, 0.5};
#define WINDOW_MS 20.0
#define FADE 0.25

#define MAXN 1200000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN];
static uint8_t g_single[MAXN];

static uint32_t g_unexplained = 0;
static void drain(void) { g_unexplained += omx_fx_drain_ledger(g_sr); }
static void clean(void) {
  drain();
  ok(g_unexplained == 0u, "no contract violation in this arm", (double)g_unexplained, 0.0);
  g_unexplained = 0u;
}

static OmxDoublerInstance g_d;

static void setup(float voices, float dms, float cents, float rate, float depth, float spread, float mix) {
  omx_doubler_instance_init(&g_d, g_sr);
  omx_doubler_instance_resolve(&g_d, 0, voices, dms, cents, rate, depth, spread, mix);
  drain();
}

static void run(const float *il, const float *ir, float *ol, float *orr, uint32_t n) {
  for (uint32_t o = 0; o < n; o += 256u) {
    omx_doubler_instance_run(&g_d, il + o, ir + o, ol + o, orr + o, n - o < 256u ? n - o : 256u);
    drain();
  }
}

/* ---- A: mix 0 ------------------------------------------------------------------------------- */
static void arm_mix0(void) {
  g_arm = "A";
  const uint32_t n = (uint32_t)(g_sr * 0.3f);
  stimulus(g_l, g_r, n, 0x1234u, 2000u);
  setup(4.0f, 9.0f, 20.0f, 1.3f, 2.0f, 100.0f, 0.0f);
  run(g_l, g_r, g_ol, g_or, n);
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0,
     "mix 0 is the input, bit for bit", 0, 0);
  clean();
}

/* ---- B: the detune -------------------------------------------------------------------------- */
static double freq_of(const float *y, uint32_t from, uint32_t to, double f) {
  const uint32_t b = (uint32_t)(g_sr / 200.0f);
  double sx = 0, sy = 0, sxx = 0, sxy = 0, prev = 0, unwrap = 0;
  int k = 0;
  for (uint32_t o = from; o + b <= to; o += b, k++) {
    double re = 0, im = 0;
    for (uint32_t i = o; i < o + b; i++) {
      re += y[i] * cos(2.0 * M_PI * f * i / g_sr);
      im -= y[i] * sin(2.0 * M_PI * f * i / g_sr);
    }
    double ph = atan2(im, re);
    if (k > 0) {
      while (ph + unwrap - prev > M_PI) unwrap -= 2.0 * M_PI;
      while (ph + unwrap - prev < -M_PI) unwrap += 2.0 * M_PI;
    }
    ph += unwrap;
    prev = ph;
    const double t = (o + 0.5 * b) / g_sr;
    sx += t;
    sy += ph;
    sxx += t * t;
    sxy += t * ph;
  }
  return f - ((k * sxy - sx * sy) / (k * sxx - sx * sx)) / (2.0 * M_PI);
}

static void arm_detune(void) {
  g_arm = "B";
  static const float CENTS[3] = {3.0f, 8.0f, 25.0f};
  const double f = 997.0;
  const uint32_t n = (uint32_t)(g_sr * 2.0f);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)(0.5 * sin(2.0 * M_PI * f * i / g_sr));
  for (int c = 0; c < 3; c++) {
    setup(1.0f, 40.0f, CENTS[c], 0.5f, 0.0f, 0.0f, 100.0f);
    for (uint32_t o = 0; o < n; o++) {
      g_single[o] = g_d.hs[0].pos < (1.0 - FADE) * g_d.head[0].window;
      omx_doubler_instance_run(&g_d, g_l + o, g_r + o, g_ol + o, g_or + o, 1u);
      if ((o & 1023u) == 0u) drain();
    }
    drain();
    uint32_t a = (uint32_t)(g_sr * 0.1f), b;
    while (a < n && !g_single[a]) a++;
    b = a;
    while (b < n && g_single[b]) b++;
    const double want = f * pow(2.0, CENTS[c] * DETUNES[0] / 1200.0);
    const double err = 1200.0 * log2(freq_of(g_ol, a, b, want) / want);
    ok(b - a > (uint32_t)(0.1f * g_sr) && fabs(err) < 0.1, "a voice comes out at f 2^(cents x its share / 1200) (0.1 cent)",
       err, 0.1);
    ok(fabs(g_d.head[0].window - WINDOW_MS * 0.001 * g_sr) <= 1e-3 * g_sr,
       "a voice with room sweeps DOUBLER_WINDOW_MS", g_d.head[0].window, WINDOW_MS * 0.001 * g_sr);
  }
  clean();
}

/* ---- C: the delay --------------------------------------------------------------------------- */
static void arm_delay(void) {
  g_arm = "C";
  for (int k = 0; k < 4; k++) {
    setup(4.0f, 12.0f, 0.0f, 0.5f, 0.0f, 0.0f, 100.0f);
    ok(fabs(g_d.mean[k] - 12.0 * DELAYS[k] * 0.001 * g_sr) <= 1e-3, "voice n's mean delay is delayMs x its share", g_d.mean[k],
       12.0 * DELAYS[k] * 0.001 * g_sr);
  }
  setup(1.0f, 12.0f, 0.0f, 0.5f, 0.0f, 0.0f, 100.0f);
  const uint32_t n = (uint32_t)(g_sr * 0.05f);
  memset(g_l, 0, n * sizeof(float));
  memset(g_r, 0, n * sizeof(float));
  g_l[0] = g_r[0] = 1.0f;
  run(g_l, g_r, g_ol, g_or, n);
  double s0 = 0.0, s1 = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    s0 += g_ol[i];
    s1 += (double)i * g_ol[i];
  }
  const double want = 12.0 * DELAYS[0] * 0.001 * g_sr;
  ok(fabs(s1 / s0 - want) <= 0.05, "an undetuned voice's impulse response centres on its mean delay (0.05 samples)", s1 / s0,
     want);
  const double longest = 40.0 * 1.31 * 0.001 * 192000.0 + 3.0 * 0.001 * 192000.0 + 0.5 * WINDOW_MS * 0.001 * 192000.0;
  ok(longest <= omx_fdelay_max_delay(OMX_DOUBLER_INSTANCE_CAP, OMX_DOUBLER_INSTANCE_ORDER),
     "the ring holds the longest voice with its drift and sweep at 192 kHz", longest, 0);
  clean();
}

/* ---- D: the drift --------------------------------------------------------------------------- */
/* The delay of y against the dry x over [o, o + win), samples past `mean`, from their phases at f. */
static double delay_at(const float *y, const float *x, uint32_t o, uint32_t win, double f, double mean) {
  double rw = 0, iw = 0, rd = 0, id = 0;
  for (uint32_t i = o; i < o + win; i++) {
    rw += y[i] * cos(2.0 * M_PI * f * i / g_sr);
    iw -= y[i] * sin(2.0 * M_PI * f * i / g_sr);
    rd += x[i] * cos(2.0 * M_PI * f * i / g_sr);
    id -= x[i] * sin(2.0 * M_PI * f * i / g_sr);
  }
  const double period = g_sr / f, base = 2.0 * M_PI * mean / period;
  double ph = atan2(id, rd) - atan2(iw, rw);
  while (ph - base > M_PI) ph -= 2.0 * M_PI;
  while (ph - base < -M_PI) ph += 2.0 * M_PI;
  return ph * period / (2.0 * M_PI) - mean;
}

static void arm_drift(void) {
  g_arm = "D";
  const double f = 300.0, rate = 0.8, depth_ms = 1.5;
  setup(1.0f, 30.0f, 0.0f, (float)rate, (float)depth_ms, 0.0f, 100.0f);
  const uint32_t n = (uint32_t)(g_sr * 4.0f);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)(0.5 * sin(2.0 * M_PI * f * i / g_sr));
  run(g_l, g_r, g_ol, g_or, n);
  const uint32_t win = (uint32_t)(g_sr / 200.0f);
  const double mean = 30.0 * DELAYS[0] * 0.001 * g_sr;
  double lo = 1e9, hi = -1e9, prev = 0, t1 = -1, t2 = -1;
  int falling = 0;
  for (uint32_t o = (uint32_t)(g_sr * 0.2f); o + win <= n; o += win) {
    const double d = delay_at(g_ol, g_l, o, win, f, mean);
    lo = fmin(lo, d);
    hi = fmax(hi, d);
    if (o > (uint32_t)(g_sr * 0.2f)) {
      if (d < prev) falling = 1;
      else if (falling) {
        if (t1 < 0) t1 = (double)o / g_sr;
        else if (t2 < 0) t2 = (double)o / g_sr;
        falling = 0;
      }
    }
    prev = d;
  }
  /* The tolerances, derived. The delay d(t) = depth sin(2 pi rate t) moves at most smax = 2 pi rate
   * depth samples a second. Each measurement is the delay averaged over one window of T = 5 ms, and
   * the window that holds an extreme reads it at most smax T short; the phase read adds 0.02 samples.
   * An extreme's time is known to one window, so two of them, 1 / rate apart, to two windows. */
  const double depth = depth_ms * 0.001 * g_sr, tw = (double)win / g_sr, smax = 2.0 * M_PI * rate * depth;
  const double swing_tol = smax * tw + 0.02, period_tol = 2.0 * tw;
  ok(fabs(hi - depth) <= swing_tol && fabs(lo + depth) <= swing_tol,
     "the drift swings modDepthMs either side (2 pi rate depth over one window)", fmax(fabs(hi - depth), fabs(lo + depth)),
     swing_tol);
  ok(t2 > 0 && fabs((t2 - t1) - 1.0 / rate) <= period_tol, "the drift's swings are 1 / modRateHz apart (two windows)", t2 - t1,
     1.0 / rate);
  /* Two voices, panned hard apart so each leg carries one: voice 1 drifts half a turn along, so its
   * delay's offset from its mean is voice 0's turned over, and their sum stays inside two swings'
   * tolerance while either one swings the full depth. */
  setup(2.0f, 30.0f, 0.0f, (float)rate, (float)depth_ms, 100.0f, 100.0f);
  run(g_l, g_r, g_ol, g_or, n);
  const double mean1 = 30.0 * DELAYS[1] * 0.001 * g_sr;
  double sum = 0.0, swing = 0.0;
  for (uint32_t o = (uint32_t)(g_sr * 0.2f); o + win <= n; o += win) {
    const double d0 = delay_at(g_ol, g_l, o, win, f, mean), d1 = delay_at(g_or, g_r, o, win, f, mean1);
    sum = fmax(sum, fabs(d0 + d1));
    swing = fmax(swing, fabs(d0));
  }
  ok(sum <= 2.0 * swing_tol && swing >= depth - swing_tol, "the second voice drifts half a turn along: its offset is the first's turned over",
     sum, 2.0 * swing_tol);
  clean();
}

/* ---- E: the place --------------------------------------------------------------------------- */
/* Still voices (0 cents, no drift) each read the line at its own mean delay, so an impulse comes back
 * once per voice, centred on its delay, and the Lagrange kernel's taps sum to 1: the left leg's samples
 * around voice n's delay sum to its left gain. The closed form is the console's linear balance law
 * written out here, b_L = 1 - p for p > 0 and 1 otherwise (b_R the mirror), p = spread x
 * DOUBLER_VOICE_PANS[n], over the voice count. Each window's sum is within 8 ulp of its gain: the
 * kernel's four taps each a few ulp off. */
static void arm_place(void) {
  g_arm = "E";
  const uint32_t n = (uint32_t)(g_sr * 0.05f);
  const double spread = 0.7;
  setup(4.0f, 12.0f, 0.0f, 0.7f, 0.0f, (float)(spread * 100.0), 100.0f);
  memset(g_l, 0, n * sizeof(float));
  memset(g_r, 0, n * sizeof(float));
  g_l[0] = g_r[0] = 1.0f;
  run(g_l, g_r, g_ol, g_or, n);
  int laws = 1;
  double worst = 0.0;
  for (int k = 0; k < 4; k++) {
    const double at = 12.0 * DELAYS[k] * 0.001 * g_sr, p = spread * PANS[k];
    const double wl = (p > 0.0 ? 1.0 - p : 1.0) / 4.0, wr = (p < 0.0 ? 1.0 + p : 1.0) / 4.0;
    double sl = 0.0, sr = 0.0;
    for (uint32_t i = (uint32_t)(at - 4.0); i <= (uint32_t)(at + 4.0); i++) {
      sl += g_ol[i];
      sr += g_or[i];
    }
    worst = fmax(worst, fmax(fabs(sl - wl), fabs(sr - wr)));
    laws &= fabs(sl - wl) <= 8.0 * FLT_EPSILON && fabs(sr - wr) <= 8.0 * FLT_EPSILON;
  }
  ok(laws, "each voice lands on the legs by the balance law of spread x its place over the voice count (8 ulp)", worst,
     8.0 * FLT_EPSILON);
  clean();
}

/* ---- I: the face's fade and hand-over laws -------------------------------------------------- */
/* One voice, 25 cents, no drift, no spread, a ramp x[i] = i / 4096 that order-3 Lagrange reads
 * exactly: the wet is b_L(0) times the head's closed form (test/kernels/doppler.c's law, here with the
 * face's own window, DOUBLER_WINDOW_MS, and fade, DOUBLER_FADE). At the run's middle delayMs moves
 * from 40 to 30 ms and the voice hands over: the held read at the last delays, the new head from its
 * centre, the weight of the held read falling from 1 by 1 / (DOUBLER_FADE x DOUBLER_WINDOW_MS) a
 * sample. Bound: 32 ulp of the largest sample read plus 1 ulp of the delay times the ramp's slope. */
#define RAMP (1.0 / 4096.0)
static void law_delays(double pos, double w, double fade, double sign, double mean, double *d0, double *d1, double *g) {
  const double start = (1.0 - fade) * w;
  *d0 = mean + sign * (pos - 0.5 * w);
  *d1 = *d0;
  *g = 0.0;
  if (pos >= start) {
    *d1 = mean + sign * (pos - start - 0.5 * w);
    *g = (pos - start) / (fade * w);
  }
}
static void arm_laws(void) {
  g_arm = "I";
  const uint32_t n = (uint32_t)(g_sr * 1.6f), at = (uint32_t)(g_sr * 1.0f);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)(i * RAMP);
  setup(1.0f, 40.0f, 25.0f, 0.5f, 0.0f, 0.0f, 100.0f);
  const double w = WINDOW_MS * 0.001 * g_sr, len = FADE * WINDOW_MS * 0.001 * g_sr;
  const double s = pow(2.0, 25.0 / 1200.0) - 1.0; /* the delay shrinks: tuned up */
  double mean = 40.0 * DELAYS[0] * 0.001 * g_sr, pos = 0.5 * w, held[3] = {0, 0, 0}, prev[3] = {0, 0, 0};
  double worst_fade = 0.0, worst_hand = 0.0;
  uint32_t bad = 0u, fades = 0u, hands = 0u;
  for (uint32_t i = 0; i < n; i++) {
    if (i == at) {
      omx_doubler_instance_resolve(&g_d, 0, 1.0f, 30.0f, 25.0f, 0.5f, 0.0f, 0.0f, 100.0f);
      memcpy(held, prev, sizeof held);
      mean = 30.0 * DELAYS[0] * 0.001 * g_sr;
      pos = 0.5 * w;
    }
    const int handing = omx_doppler_handing_over(&g_d.hs[0]);
    omx_doubler_instance_run(&g_d, g_l + i, g_r + i, g_ol + i, g_or + i, 1u);
    if ((i & 1023u) == 0u) drain();
    double d0, d1, g;
    law_delays(pos, w, FADE, -1.0, mean, &d0, &d1, &g);
    prev[0] = d0;
    prev[1] = d1;
    prev[2] = g;
    double want = (1.0 - g) * (i - d0) * RAMP + g * (i - d1) * RAMP;
    if (i >= at && (double)(i - at) < len) {
      const double h = 1.0 - (double)(i - at) / len;
      want = (1.0 - h) * want + h * ((1.0 - held[2]) * (i - held[0]) + held[2] * (i - held[1])) * RAMP;
    }
    pos += (double)(float)fabs(s);
    if (pos >= w) pos -= (1.0 - FADE) * w;
    if ((double)i < mean + w + 8.0) continue;
    const double tol = 32.0 * FLT_EPSILON * i * RAMP + RAMP * FLT_EPSILON * (mean + w);
    const double e = fabs((double)g_ol[i] - want);
    bad += !(e <= tol);
    if (handing) {
      hands++;
      worst_hand = fmax(worst_hand, e / tol);
    } else if (g > 0.0) {
      fades++;
      worst_fade = fmax(worst_fade, e / tol);
    }
  }
  drain();
  printf("  %6.0f Hz: I fade law worst %.3f of its bound over %u samples, hand-over worst %.3f over %u\n", (double)g_sr,
         worst_fade, fades, worst_hand, hands);
  ok(bad == 0u && fades > 0u, "the voice's crossfade follows DOUBLER_FADE of DOUBLER_WINDOW_MS, linear (derived bound)", bad, 0);
  ok(fabs((double)hands - len) <= 1.0, "a changed voice hands over across DOUBLER_FADE x DOUBLER_WINDOW_MS", hands, len);
  clean();
}

/* ---- F: hostile words, no allocation -------------------------------------------------------- */
static void arm_hostile(void) {
  g_arm = "F";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  for (unsigned h = 0; h < sizeof W / sizeof W[0]; h++) {
    setup(W[h], W[h], W[h], W[h], W[h], W[h], W[h]);
    int inside = g_d.nv >= 1u && g_d.nv <= 4u && g_d.mix >= 0.0f && g_d.mix <= 1.0f && g_d.depth >= 0.0f &&
                 g_d.depth <= 3.0f * 0.001f * g_sr;
    for (int k = 0; k < 4; k++) inside &= g_d.head[k].window >= 1.0f && g_d.head[k].step < 1.0f;
    ok(inside, "hostile words: every travel at a declared value", h, 0);
    stimulus(g_l, g_r, 4096u, 0x5u, 300u);
    run(g_l, g_r, g_ol, g_or, 4096u);
  }
  g_allocs = 0;
  g_counting = 1;
  setup(4.0f, 5.0f, 25.0f, 2.0f, 3.0f, 100.0f, 80.0f);
  run(g_l, g_r, g_ol, g_or, 4096u);
  g_counting = 0;
  ok(g_allocs == 0, "init, resolve and run allocate nothing", (double)g_allocs, 0);
  clean();
}

/* ---- G: a sweep across the room limit never jumps; H: the margin inside the reach ------------- */
static void arm_sweep(void) {
  g_arm = "G";
  const double f = 220.0, amp = 0.5, w = 2.0 * M_PI * f / g_sr;
  const uint32_t n = (uint32_t)(g_sr * 1.5f), blk = 64u;
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)(amp * sin(w * i));
  setup(4.0f, 25.0f, 25.0f, 2.0f, 0.0f, 0.0f, 100.0f);
  const float reach = omx_fdelay_min_delay(OMX_DOUBLER_INSTANCE_ORDER);
  const double hand = FADE * WINDOW_MS * 0.001 * g_sr;
  double worst = 0.0, worst_ratio = 0.0, low = 1e9;
  uint32_t over = 0u, hands = 0u;
  for (uint32_t o = 0; o < n; o += blk) {
    /* delayMs falls from 25 to 5 and back, modDepthMs rises 0 to 3 and back, a turn each over the run */
    const double t = (double)o / n, tri = t < 0.5 ? 2.0 * t : 2.0 - 2.0 * t;
    omx_doubler_instance_resolve(&g_d, 0, 4.0f, (float)(25.0 - 20.0 * tri), 25.0f, 2.0f, (float)(3.0 * tri), 0.0f, 100.0f);
    for (uint32_t i = o; i < o + blk && i < n; i++) {
      double bound = 0.0;
      for (uint32_t k = 0; k < 4u; k++) {
        const double r = g_d.vdepth[k] * 2.0 * M_PI * 2.0 / g_sr;
        bound += fabs(g_d.gl[k]) * amp *
                 (w * (1.0 + g_d.head[k].step + r) + 2.0 / (FADE * g_d.head[k].window) +
                  (omx_doppler_handing_over(&g_d.hs[k]) ? 2.0 / hand : 0.0));
        hands += omx_doppler_handing_over(&g_d.hs[k]) ? 1u : 0u;
        const double lo = g_d.mean[k] - g_d.vdepth[k] - 0.5 * g_d.head[k].window;
        low = fmin(low, lo - reach);
      }
      omx_doubler_instance_run(&g_d, g_l + i, g_r + i, g_ol + i, g_or + i, 1u);
      if (i > 0u) {
        const double e = fabs((double)g_ol[i] - g_ol[i - 1u]);
        if (!(e <= worst)) worst = e;
        if (!(e <= bound)) over++;
        if (!(e / bound <= worst_ratio)) worst_ratio = e / bound;
      }
    }
    drain();
  }
  printf("  %6.0f Hz: G worst step %.5f, worst step over its bound %.3f, %u hand-over samples; H lowest read %.3f past the reach\n",
         (double)g_sr, worst, worst_ratio, hands, low);
  ok(over == 0u, "a sweep across the room limit steps no sample past its bound", over, 0);
  ok(hands > 0u, "the sweep hands over", hands, 1);
  g_arm = "H";
  ok(low >= 1.0 - 1e-3, "every voice's lowest read stays one sample clear of the kernel's reach", low, 1.0);
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
    arm_mix0();
    arm_detune();
    arm_delay();
    arm_drift();
    arm_place();
    arm_hostile();
    arm_sweep();
    arm_laws();
  }
  return instance_oracle_end("doubler_instance");
}
