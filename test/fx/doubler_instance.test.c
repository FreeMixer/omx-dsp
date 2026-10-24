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
//   D  the drift: an undetuned voice's delay swings modDepthMs either side of its mean (3 %), its
//      swings 1 / modRateHz apart (5 %);
//   E  the place: the voices' sum lands on the legs by the balance law of spread x DOUBLER_VOICE_PANS[n]
//      over the voice count, bit for bit against the kernel words run by hand;
//   F  hostile words land inside the declared travels, the ledger is clean, nothing allocates.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_doubler_instance.h>

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
static void arm_drift(void) {
  g_arm = "D";
  const double f = 300.0, rate = 0.8, depth_ms = 1.5;
  setup(1.0f, 30.0f, 0.0f, (float)rate, (float)depth_ms, 0.0f, 100.0f);
  const uint32_t n = (uint32_t)(g_sr * 4.0f);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = (float)(0.5 * sin(2.0 * M_PI * f * i / g_sr));
  run(g_l, g_r, g_ol, g_or, n);
  const uint32_t win = (uint32_t)(g_sr / 200.0f);
  const double mean = 30.0 * DELAYS[0] * 0.001 * g_sr, period = g_sr / f;
  double lo = 1e9, hi = -1e9, prev = 0, t1 = -1, t2 = -1;
  int falling = 0;
  for (uint32_t o = (uint32_t)(g_sr * 0.2f); o + win <= n; o += win) {
    double rw = 0, iw = 0, rd = 0, id = 0;
    for (uint32_t i = o; i < o + win; i++) {
      rw += g_ol[i] * cos(2.0 * M_PI * f * i / g_sr);
      iw -= g_ol[i] * sin(2.0 * M_PI * f * i / g_sr);
      rd += g_l[i] * cos(2.0 * M_PI * f * i / g_sr);
      id -= g_l[i] * sin(2.0 * M_PI * f * i / g_sr);
    }
    double ph = atan2(id, rd) - atan2(iw, rw), base = 2.0 * M_PI * mean / period;
    while (ph - base > M_PI) ph -= 2.0 * M_PI;
    while (ph - base < -M_PI) ph += 2.0 * M_PI;
    const double d = ph * period / (2.0 * M_PI) - mean;
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
  const double depth = depth_ms * 0.001 * g_sr;
  ok(fabs(hi - depth) <= 0.03 * depth && fabs(lo + depth) <= 0.03 * depth, "the drift swings modDepthMs either side (3 %)",
     hi - lo, 2.0 * depth);
  ok(t2 > 0 && fabs((t2 - t1) - 1.0 / rate) <= 0.05 / rate, "the drift's swings are 1 / modRateHz apart (5 %)", t2 - t1,
     1.0 / rate);
  clean();
}

/* ---- E: the place --------------------------------------------------------------------------- */
static void arm_place(void) {
  g_arm = "E";
  /* a short delay and a wide detune, so the heads cross their fades several times in the second */
  const uint32_t n = (uint32_t)(g_sr * 1.0f);
  stimulus(g_l, g_r, n, 0x4242u, 1500u);
  setup(4.0f, 5.0f, 25.0f, 0.7f, 0.8f, 70.0f, 100.0f);
  run(g_l, g_r, g_ol, g_or, n);
  /* the same voices by hand: the line, the heads, the drift, the law */
  static float ring[OMX_DOUBLER_INSTANCE_CAP];
  memset(ring, 0, sizeof ring);
  struct omx_fdelay line;
  omx_fdelay_init(&line, ring, OMX_DOUBLER_INSTANCE_CAP, OMX_DOUBLER_INSTANCE_ORDER);
  struct omx_doppler h[4];
  struct omx_doppler_state hs[4];
  struct omx_lfo lfo;
  omx_lfo_start(&lfo);
  lfo.inc = omx_lfo_inc(0.7f, g_sr);
  float gl[4], gr[4], mean[4];
  const float depth = 0.8f * 0.001f * g_sr;
  for (int k = 0; k < 4; k++) {
    mean[k] = 5.0f * (float)DELAYS[k] * 0.001f * g_sr;
    const float room = mean[k] - depth - omx_fdelay_min_delay(3), full = (float)WINDOW_MS * 0.001f * g_sr;
    omx_doppler_design(&h[k], 25.0 * DETUNES[k], 2.0f * room < full ? 2.0f * room : full, (float)FADE);
    omx_doppler_start(&h[k], &hs[k]);
    float bl, br;
    omx_balance_law(0.7f * (float)PANS[k], &bl, &br);
    gl[k] = bl / 4.0f;
    gr[k] = br / 4.0f;
  }
  int same = 1;
  for (uint32_t i = 0; i < n; i++) {
    omx_fdelay_write(&line, 0.5f * (g_l[i] + g_r[i]));
    float wl = 0.0f, wr = 0.0f;
    for (int k = 0; k < 4; k++) {
      const float v = omx_doppler_read(&line, &h[k], &hs[k], mean[k] + depth * omx_lfo_at(&lfo, (float)k / 4.0f));
      wl += gl[k] * v;
      wr += gr[k] * v;
    }
    omx_lfo_advance(&lfo);
    same &= g_ol[i] == wl && g_or[i] == wr;
    if ((i & 1023u) == 0u) drain();
  }
  ok(same, "the voices land on the legs by the balance law over the voice count, bit for bit", same, 1);
  ok((double)n > (double)g_d.head[0].window / g_d.head[0].step, "the second spans a full sweep of the fastest head, its fade included",
     g_d.head[0].window / g_d.head[0].step, n);
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
  }
  return instance_oracle_end("doubler_instance");
}
