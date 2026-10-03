// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The reverb kernel's oracle (omx_reverb.h), every arm at every rate in OMX_DECLARED_RATES. The
 * arms' frame counts are written at 48 kHz and read through at(), so a window is the same stretch
 * of time at every rate:
 *   make test-fx
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_reverb.h>

#include "fx_rates.h"

static int g_fail = 0, g_checks = 0;
static float g_sr = 48000.0f;
/* A frame count written at 48 kHz, at the rate under test. */
static int at(int frames_at_48k) { return (int)((double)frames_at_48k * (double)g_sr / 48000.0); }
static void check(int cond, const char *what) {
  g_checks++; if (!cond) { g_fail++; fprintf(stderr, "FAIL: %s\n", what); }
}

static struct omx_reverb_state *make_state(float sr) {
  struct omx_reverb_state *s = calloc(1, sizeof(*s));
  float *pool = calloc(OMX_REVERB_POOL_FLOATS, sizeof(float));
  omx_reverb_state_layout(s, pool, OMX_REVERB_POOL_FLOATS, sr);
  return s;
}
static float tail_energy(const float *l, const float *r, int a, int b) {
  float e = 0.0f; for (int i = a; i < b; i++) e += l[i] * l[i] + r[i] * r[i]; return e;
}

/* An impulse through one configuration at an EXPLICIT size — every parameter the caller cares
 * about is an argument, because the earlier three-argument form let a caller hand `size` to the
 * `damping` slot and still read a plausible number (the hall/plate golden did exactly that). */
static void run_impulse_sz(int algo, float size, float damping, float predelay_ms, float *l, float *r, int n, float sr) {
  struct omx_reverb_state *s = make_state(sr);
  struct omx_reverb p = { .enabled = 1, .algorithm = algo, .size = size, .damping = damping,
                          .predelay_ms = predelay_ms, .width = 1.0f, .mix = 1.0f,
                          .lowcut = 0.0f, .highcut = 20000.0f, .plate_mod_depth = OMX_REVERB_PLATE_MOD_DEPTH_DEFAULT };
  memset(l, 0, sizeof(float) * n); memset(r, 0, sizeof(float) * n);
  l[0] = 1.0f; r[0] = 1.0f;
  // process in 256-frame blocks
  for (int off = 0; off < n; off += 256) {
    int len = (n - off) < 256 ? (n - off) : 256;
    omx_reverb_process(l + off, r + off, len, &p, s, sr);
  }
  free(s->_pool); free(s);
}

/* The old three-argument spelling, at the size it always used. */
static void run_impulse(int algo, float damping, float predelay_ms, float *l, float *r, int n, float sr) {
  run_impulse_sz(algo, 0.7f, damping, predelay_ms, l, r, n, sr);
}

static void test_decaying_diffuse_tail_both(void) {
  const int N = at(48000); float sr = g_sr;
  float *l = malloc(sizeof(float) * N), *r = malloc(sizeof(float) * N);
  static const char *names[3] = { "room", "plate", "hall" };
  for (int algo = 0; algo <= 2; algo++) {
    run_impulse(algo, 0.5f, 0.0f, l, r, N, sr);
    // after the initial build-up the tail energy must decrease over time (a decaying tail).
    float early = tail_energy(l, r, at(2000), at(6000));
    float late = tail_energy(l, r, at(30000), at(34000));
    char what[32]; snprintf(what, sizeof(what), "%s tail decays", names[algo]);
    check(late < early, what);
    // no NaN/denormal blow-up over the long tail
    int finite = 1; for (int i = 0; i < N; i++) if (!isfinite(l[i]) || !isfinite(r[i])) finite = 0;
    check(finite, "no NaN/inf over a long tail");
  }
  free(l); free(r);
}

/* HALL's whole reason to exist: a longer decay tail than PLATE at the SAME size/mix — not a bigger
 * ROOM, a genuinely longer-ringing space. Property, not a magic buffer: run both algorithms on an
 * identical impulse and compare measured late-tail energy; no hand-computed expected samples. */
static void test_hall_tail_outlasts_plate(void) {
  const int N = at(96000); float sr = g_sr; // 2 s — plate's tank loop alone is tens of ms long
  const float SIZE = 0.3f, DAMPING = 0.5f;
  float *lh = malloc(sizeof(float) * N), *rh = malloc(sizeof(float) * N);
  float *lp = malloc(sizeof(float) * N), *rp = malloc(sizeof(float) * N);
  /* was: run_impulse(algo, SIZE, DAMPING, ...) — SIZE landed in the `damping` slot and DAMPING in
   * `predelay_ms`, so the golden compared both algorithms at size 0.7 while its prose said 0.3.
   * Same-for-both either way, so the property held; the numbers are now the ones named. */
  run_impulse_sz(OMX_REVERB_HALL, SIZE, DAMPING, 0.0f, lh, rh, N, sr);
  run_impulse_sz(OMX_REVERB_PLATE, SIZE, DAMPING, 0.0f, lp, rp, N, sr);
  // a late window, well past both algorithms' early build-up — this is the tail, not the onset.
  float hallTail = tail_energy(lh, rh, at(64000), at(72000));
  float plateTail = tail_energy(lp, rp, at(64000), at(72000));
  check(hallTail > plateTail, "hall's tail outlasts plate's at the same size/mix");
  free(lh); free(rh); free(lp); free(rp);
}

/* ---- THE DAMPING LAW (spec 2026-07-15-native-delay-reverb.md, Amendment 2026-09-17 §A) --------
 *
 * Damping attenuates HIGHS IN THE FEEDBACK PATH and NEVER the tail's level: a damping one-pole in
 * a reverb loop has UNITY DC GAIN at every damping value. So the LOW band of an impulse response
 * decays at the rate the SIZE alone sets, whatever the damping knob says.
 *
 * The plate's closed form: its longest tank loop is 908 + 2656 = 3564 samples at the Dattorro
 * tuning rate 29 761 Hz (119.75 ms), with per-pass gain 0.5 + 0.49 x size, so
 *     RT60 = loopMs x ln(0.001) / ln(gain).
 */
#define PLATE_LOOP_SAMPLES 3564.0f
#define PLATE_TUNING_RATE 29761.0f

static float plate_rt60_closed_form_ms(float size) {
  float loop_ms = (PLATE_LOOP_SAMPLES / PLATE_TUNING_RATE) * 1000.0f;
  float g = OMX_REVERB_PLATE_DECAY_FLOOR + OMX_REVERB_PLATE_DECAY_SPAN * size;
  return loop_ms * logf(0.001f) / logf(g);
}

/* One-pole low-pass, in place — isolates the band the damping filter leaves alone. */
static void lowpass_inplace(float *x, int n, float sr, float fc) {
  float a = 1.0f - expf(-2.0f * 3.14159265f * fc / sr);
  float y = 0.0f;
  for (int i = 0; i < n; i++) { y += a * (x[i] - y); x[i] = y; }
}

/* The RT60 a measured decay implies: the amplitude dB lost between two windows, extrapolated. */
static float measured_rt60_ms(const float *x, int n, float sr, float t1_ms, float t2_ms, float win_ms) {
  int w = (int)(win_ms * 0.001f * sr);
  int a1 = (int)(t1_ms * 0.001f * sr), a2 = (int)(t2_ms * 0.001f * sr);
  if (a2 + w > n) return -1.0f;
  float e1 = 0.0f, e2 = 0.0f;
  for (int i = 0; i < w; i++) { e1 += x[a1 + i] * x[a1 + i]; e2 += x[a2 + i] * x[a2 + i]; }
  if (e1 <= 0.0f || e2 <= 0.0f) return -1.0f;           /* no tail to measure - NOT a long RT60 */
  float db = 10.0f * log10f(e1 / e2);                    /* amplitude dB lost across the gap */
  if (db <= 0.0f) return -1.0f;
  return 60.0f * (t2_ms - t1_ms) / db;
}

static void test_plate_damping_never_shortens_the_tail(void) {
  const int N = at(144000); float sr = g_sr;            /* 3 s */
  const float SIZE = 0.5f;
  float *l = malloc(sizeof(float) * N), *r = malloc(sizeof(float) * N);
  const float expected = plate_rt60_closed_form_ms(SIZE);
  static const float DAMPINGS[4] = { 0.0f, 0.3f, 0.6f, 0.9f };
  float rt60[4];
  for (int i = 0; i < 4; i++) {
    run_impulse_sz(OMX_REVERB_PLATE, SIZE, DAMPINGS[i], 0.0f, l, r, N, sr);
    /* the probe must be able to see a tail at all before its silence means anything */
    float late = tail_energy(l, r, at(38400), at(43200));        /* 800-900 ms */
    char what[64]; snprintf(what, sizeof(what), "plate at damping %.1f still has a tail", (double)DAMPINGS[i]);
    check(late > 1e-9f, what);
    lowpass_inplace(l, N, sr, 200.0f);                   /* the band damping must not touch */
    rt60[i] = measured_rt60_ms(l, N, sr, 800.0f, 1800.0f, 100.0f);
    snprintf(what, sizeof(what), "plate low-band RT60 is measurable at damping %.1f", (double)DAMPINGS[i]);
    check(rt60[i] > 0.0f, what);
  }
  /* (1) the closed form is a FLOOR, not an equality: counting the tank's allpass at its own
   * length is the SHORTEST path round the loop, and an allpass's own recirculation (coef 0.7)
   * adds longer ones — so the real tail runs longer than `loopMs x ln(0.001)/ln(gain)` and can
   * never run SHORTER, whatever the damping. Measured 3635 ms against a 2810 ms floor at
   * size 0.5, 48 kHz (x1.29). The band is the assertion; the equality across damping below is
   * the law. */
  check(rt60[0] >= expected * 0.95f && rt60[0] <= expected * 2.0f,
        "plate RT60 at damping 0 sits in the band its size's closed form floors");
  /* (2) damping moves the tail's brightness, not its length */
  for (int i = 1; i < 4; i++) {
    char what[64]; snprintf(what, sizeof(what), "plate low-band RT60 at damping %.1f matches damping 0", (double)DAMPINGS[i]);
    check(rt60[i] > 0.0f && fabsf(rt60[i] - rt60[0]) < rt60[0] * 0.10f, what);
  }
  fprintf(stderr, "  plate RT60 (size %.2f): closed form %.0f ms; measured %.0f / %.0f / %.0f / %.0f ms at damping 0/0.3/0.6/0.9\n",
          (double)SIZE, (double)expected, (double)rt60[0], (double)rt60[1], (double)rt60[2], (double)rt60[3]);
  free(l); free(r);
}

/* (3) damping DOES shorten the high end - the other half of the law. */
static void test_plate_damping_shortens_the_hf_decay(void) {
  const int N = at(96000); float sr = g_sr;
  float *lb = malloc(sizeof(float) * N), *rb = malloc(sizeof(float) * N);
  float *ld = malloc(sizeof(float) * N), *rd = malloc(sizeof(float) * N);
  run_impulse_sz(OMX_REVERB_PLATE, 0.5f, 0.0f, 0.0f, lb, rb, N, sr);
  run_impulse_sz(OMX_REVERB_PLATE, 0.5f, 0.9f, 0.0f, ld, rd, N, sr);
  float hfBright = 0.0f, hfDamped = 0.0f;
  for (int i = at(24000) + 1; i < N; i++) {                      /* past 500 ms: the tail, not the onset */
    hfBright += fabsf(lb[i] - lb[i - 1]);
    hfDamped += fabsf(ld[i] - ld[i - 1]);
  }
  check(hfDamped < hfBright * 0.9f, "plate damping shortens the HF decay");
  free(lb); free(rb); free(ld); free(rd);
}

/* ---- REVERSE and GATED (spec 2026-07-15-native-delay-reverb.md, Amendment 2026-09-17 SSB/C) ---
 *
 * Both are the ROOM comb network handed an OUTPUT STAGE, so ROOM at the same settings is the
 * control arm in every test below: a mirror that did not mirror, or a gate that did not gate,
 * would read exactly like ROOM and these checks would fail.
 */

/* Run an arbitrary input through one configuration, on a fresh state, in 256-frame blocks. */
static void run_signal(const struct omx_reverb *p, const float *in, float *l, float *r, int n, float sr) {
  struct omx_reverb_state *s = make_state(sr);
  for (int i = 0; i < n; i++) { l[i] = in[i]; r[i] = in[i]; }
  for (int off = 0; off < n; off += 256) {
    int len = (n - off) < 256 ? (n - off) : 256;
    omx_reverb_process(l + off, r + off, len, p, s, sr);
  }
  free(s->_pool); free(s);
}

static struct omx_reverb base_params(int algo) {
  struct omx_reverb p;
  memset(&p, 0, sizeof p);
  p.enabled = 1; p.algorithm = algo; p.size = 0.7f; p.damping = 0.3f; p.predelay_ms = 0.0f;
  p.width = 1.0f; p.mix = 1.0f; p.lowcut = 0.0f; p.highcut = 20000.0f;
  p.reverse_ms = 300.0f; p.hold_ms = 120.0f; p.release_ms = 20.0f; p.gate_threshold_db = -40.0f;
  p.plate_mod_depth = OMX_REVERB_PLATE_MOD_DEPTH_DEFAULT;
  return p;
}

/* REVERSE's whole claim: the energy a hit produces RISES to a peak and is then over - the exact
 * inverse of ROOM's decay, measured in the SAME windows on the SAME impulse. */
static void test_reverse_swells_where_room_decays(void) {
  const int N = at(96000); float sr = g_sr;             /* 2 s */
  const float W_MS = 300.0f;
  float *in = calloc(N, sizeof(float));
  float *lv = malloc(sizeof(float) * N), *rv = malloc(sizeof(float) * N);
  float *lo = malloc(sizeof(float) * N), *ro = malloc(sizeof(float) * N);
  in[0] = 1.0f;
  struct omx_reverb pv = base_params(OMX_REVERB_REVERSE); pv.reverse_ms = W_MS;
  struct omx_reverb po = base_params(OMX_REVERB_ROOM);
  run_signal(&pv, in, lv, rv, N, sr);
  run_signal(&po, in, lo, ro, N, sr);
  const int w = (int)(W_MS * 0.001f * sr);              /* one window, in frames */
  float vEarly = tail_energy(lv, rv, w, w + w / 2);     /* [1W, 1.5W) */
  float vLate = tail_energy(lv, rv, w + w / 2, 2 * w);  /* [1.5W, 2W) */
  float vGone = tail_energy(lv, rv, 3 * w, 4 * w);      /* [3W, 4W) - past the swell */
  float oEarly = tail_energy(lo, ro, w, w + w / 2);
  float oLate = tail_energy(lo, ro, w + w / 2, 2 * w);
  check(vLate > vEarly * 2.0f, "reverse swells: the window nearest the peak carries the energy");
  check(oLate < oEarly, "room decays in the same two windows (the control arm)");
  check(vGone < vLate * 0.2f, "reverse's swell is over within two windows of the hit");
  fprintf(stderr, "  reverse (W %.0f ms): energy [1W,1.5W) %.4g -> [1.5W,2W) %.4g, [3W,4W) %.4g; room %.4g -> %.4g\n",
          (double)W_MS, (double)vEarly, (double)vLate, (double)vGone, (double)oEarly, (double)oLate);
  free(in); free(lv); free(rv); free(lo); free(ro);
}

/* The mirror IS a mirror: it reverses the ORDER OF EVENTS. Two bursts a quarter-window apart, at
 * two frequencies a band apart, go IN low-then-high; through REVERSE they must come OUT
 * high-then-low, and through ROOM low-then-high. A swell shape could be faked by an envelope; the
 * order of two distinguishable events cannot. */
static void band_split(const float *x, int n, float sr, float cut_hz, float *out, int high) {
  float a = 1.0f - expf(-2.0f * 3.14159265f * cut_hz / sr);
  float y = 0.0f;
  for (int i = 0; i < n; i++) { y += a * (x[i] - y); out[i] = high ? x[i] - y : y; }
}
/* When a band's energy sits, in ms — the energy-weighted mean time over [a, b). */
static float band_centroid_ms(const float *x, int a, int b, float sr) {
  double num = 0.0, den = 0.0;
  for (int i = a; i < b; i++) { double e = (double)x[i] * (double)x[i]; num += e * (double)i; den += e; }
  return den > 0.0 ? (float)((num / den) / sr * 1000.0) : -1.0f;
}

static void test_reverse_reverses_the_order_of_events(void) {
  const int N = at(96000); float sr = g_sr;
  const float W_MS = 300.0f;
  float *in = calloc(N, sizeof(float));
  float *lv = malloc(sizeof(float) * N), *rv = malloc(sizeof(float) * N);
  float *lo = malloc(sizeof(float) * N), *ro = malloc(sizeof(float) * N);
  float *band = malloc(sizeof(float) * N);
  /* LOW burst at 0 ms, HIGH burst 75 ms later — both inside one 300 ms window. */
  const int b1 = 0, b2 = (int)(0.150f * sr), blen = (int)(0.020f * sr);
  for (int i = 0; i < blen; i++) {
    in[b1 + i] += sinf(2.0f * 3.14159265f * 300.0f * (float)i / sr);
    in[b2 + i] += sinf(2.0f * 3.14159265f * 6000.0f * (float)i / sr);
  }
  struct omx_reverb pv = base_params(OMX_REVERB_REVERSE); pv.reverse_ms = W_MS; pv.damping = 0.0f; pv.size = 0.0f;
  struct omx_reverb po = base_params(OMX_REVERB_ROOM); po.damping = 0.0f; po.size = 0.0f;
  run_signal(&pv, in, lv, rv, N, sr);
  run_signal(&po, in, lo, ro, N, sr);
  const int w = (int)(W_MS * 0.001f * sr);
  /* REVERSE: the mirrored pair lands in the two windows after the hits. ROOM: at the hits. */
  band_split(lv, N, sr, 1000.0f, band, 0); float vLow = band_centroid_ms(band, w, 2 * w, sr);
  band_split(lv, N, sr, 3000.0f, band, 1); float vHigh = band_centroid_ms(band, w, 2 * w, sr);
  band_split(lo, N, sr, 1000.0f, band, 0); float oLow = band_centroid_ms(band, 0, 2 * w, sr);
  band_split(lo, N, sr, 3000.0f, band, 1); float oHigh = band_centroid_ms(band, 0, 2 * w, sr);
  /* a SEPARATION, not just a sign: the two bursts are 150 ms apart going in, and each arm must
   * put that much distance between the bands - a near-tie would be two smeared tails, not order. */
  check(oHigh > oLow + 50.0f, "room keeps the order: the low burst sounds first (the control arm)");
  check(vHigh + 50.0f < vLow, "reverse REVERSES the order: the later burst comes back first");
  fprintf(stderr, "  reverse order: reverse low@%.0f ms high@%.0f ms; room low@%.0f ms high@%.0f ms\n",
          (double)vLow, (double)vHigh, (double)oLow, (double)oHigh);
  free(in); free(lv); free(rv); free(lo); free(ro); free(band);
}

/* GATED's envelope, measured SAMPLE BY SAMPLE against ROOM: the gate is the last thing in the wet
 * chain, so gated(t) / room(t) IS the envelope - 1 through the hold, a straight line down across
 * the release, and 0 after. No proxy, no shape check. */
static void test_gated_envelope_is_the_ratio_to_room(void) {
  const int N = at(48000); float sr = g_sr;             /* 1 s */
  const float BURST_MS = 50.0f, HOLD_MS = 120.0f, REL_MS = 20.0f;
  float *in = calloc(N, sizeof(float));
  float *lg = malloc(sizeof(float) * N), *rg = malloc(sizeof(float) * N);
  float *lo = malloc(sizeof(float) * N), *ro = malloc(sizeof(float) * N);
  const int burst = (int)(BURST_MS * 0.001f * sr);
  for (int i = 0; i < burst; i++) in[i] = sinf(2.0f * 3.14159265f * 440.0f * (float)i / sr);
  struct omx_reverb pg = base_params(OMX_REVERB_GATED);
  pg.hold_ms = HOLD_MS; pg.release_ms = REL_MS; pg.gate_threshold_db = -40.0f;
  struct omx_reverb po = base_params(OMX_REVERB_ROOM);
  run_signal(&pg, in, lg, rg, N, sr);
  run_signal(&po, in, lo, ro, N, sr);
  /* The measured envelope at a time, over the samples where ROOM is loud enough to divide by. */
  float (*env_at)(const float *, const float *, int, int) = NULL; (void)env_at;
  struct { float ms; float want; } probes[] = {
    { 100.0f, 1.0f },                                   /* inside the hold */
    { 165.0f, 1.0f },                                   /* the last of the hold (50 + 120) */
    { 180.0f, 0.5f },                                   /* halfway down a 20 ms linear release */
    { 188.0f, 0.1f },                                   /* nearly cut */
  };
  for (unsigned k = 0; k < sizeof(probes) / sizeof(probes[0]); k++) {
    int a = (int)(probes[k].ms * 0.001f * sr);
    float num = 0.0f, den = 0.0f;
    for (int i = a; i < a + at(48); i++) { num += fabsf(lg[i]); den += fabsf(lo[i]); }
    float env = den > 0.0f ? num / den : -1.0f;
    char what[80];
    snprintf(what, sizeof(what), "gated envelope at %.0f ms is %.2f", (double)probes[k].ms, (double)probes[k].want);
    check(den > 1e-6f && fabsf(env - probes[k].want) < 0.06f, what);
    fprintf(stderr, "  gated env at %6.1f ms: measured %.3f (want %.2f)\n",
            (double)probes[k].ms, (double)env, (double)probes[k].want);
  }
  /* and the cut is ABSOLUTE: exactly zero past hold + release, while room is still ringing. */
  int cut = (int)((BURST_MS + HOLD_MS + REL_MS + 5.0f) * 0.001f * sr);
  check(tail_energy(lg, rg, cut, N) == 0.0f, "gated is EXACTLY silent past hold + release");
  check(tail_energy(lo, ro, cut, N) > 1e-6f, "room is still ringing there (the control arm)");
  /* The gate's open and close SAMPLES and a hash of every output bit: the threshold word
   * (omx_db_to_lin, R-097) must land the gate on the same samples, bit for bit. */
  int open = -1, close = -1;
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < N; i++) {
    if (lg[i] != 0.0f || rg[i] != 0.0f) { if (open < 0) open = i; close = i; }
    uint32_t b[2]; memcpy(&b[0], &lg[i], 4); memcpy(&b[1], &rg[i], 4);
    for (int j = 0; j < 2; j++) { h ^= b[j]; h *= 1099511628211ull; }
  }
  check(open >= 0 && close > open && close < cut, "the gate opens and closes inside the run");
  fprintf(stderr, "  gated at -40 dB: opens at sample %d, closes at sample %d, output fnv %016llx\n",
          open, close, (unsigned long long)h);
  free(in); free(lg); free(rg); free(lo); free(ro);
}

static void test_mix_zero_bit_identical(void) {
  const int N = 512; float sr = g_sr;
  float l[512], r[512], l0[512];
  struct omx_reverb_state *s = make_state(sr);
  struct omx_reverb p = { .enabled = 1, .algorithm = OMX_REVERB_PLATE, .size = 0.8f, .damping = 0.4f,
                          .predelay_ms = 10.0f, .width = 1.0f, .mix = 0.0f, .lowcut = 0.0f, .highcut = 20000.0f, .plate_mod_depth = OMX_REVERB_PLATE_MOD_DEPTH_DEFAULT };
  for (int i = 0; i < N; i++) { l[i] = sinf(i * 0.11f); r[i] = cosf(i * 0.07f); l0[i] = l[i]; }
  omx_reverb_process(l, r, N, &p, s, sr);
  for (int i = 0; i < N; i++) check(l[i] == l0[i], "mix=0 is bit-identical dry");
  free(s->_pool); free(s);
}

static void test_damping_reduces_hf_tail(void) {
  const int N = at(24000); float sr = g_sr;
  float *l = malloc(sizeof(float) * N), *r = malloc(sizeof(float) * N);
  float *l2 = malloc(sizeof(float) * N), *r2 = malloc(sizeof(float) * N);
  run_impulse(OMX_REVERB_ROOM, 0.1f, 0.0f, l, r, N, sr);   // bright
  run_impulse(OMX_REVERB_ROOM, 0.9f, 0.0f, l2, r2, N, sr); // damped
  // crude HF proxy: sum of |first difference| (more HF -> larger). Damped < bright in the tail.
  float hfBright = 0.0f, hfDamp = 0.0f;
  for (int i = at(12000) + 1; i < N; i++) { hfBright += fabsf(l[i] - l[i - 1]); hfDamp += fabsf(l2[i] - l2[i - 1]); }
  check(hfDamp < hfBright, "damping reduces HF energy in the tail");
  free(l); free(r); free(l2); free(r2);
}

static void test_predelay_shifts_onset(void) {
  const int N = at(8000); float sr = g_sr;
  float *l = malloc(sizeof(float) * N), *r = malloc(sizeof(float) * N);
  run_impulse(OMX_REVERB_PLATE, 0.5f, 20.0f, l, r, N, sr); // 20 ms = 960 frames pre-delay
  // energy before the pre-delay onset should be ~silent (only the pre-delay line, no tank yet).
  float pre = tail_energy(l, r, 1, at(900));
  float post = tail_energy(l, r, at(1000), at(3000));
  check(pre < post * 0.05f, "pre-delay holds the onset back ~960 frames");
  free(l); free(r);
}

static void test_plate_denser_early_diffusion_than_room(void) {
  const int N = at(4000); float sr = g_sr;
  float *lr = malloc(sizeof(float) * N), *rr = malloc(sizeof(float) * N);
  float *lp = malloc(sizeof(float) * N), *rp = malloc(sizeof(float) * N);
  run_impulse(OMX_REVERB_ROOM, 0.5f, 0.0f, lr, rr, N, sr);
  run_impulse(OMX_REVERB_PLATE, 0.5f, 0.0f, lp, rp, N, sr);
  // count non-trivial samples in the first 40 ms (1920 frames): the plate diffuses denser.
  int roomHits = 0, plateHits = 0;
  for (int i = 0; i < at(1920); i++) { if (fabsf(lr[i]) > 1e-3f) roomHits++; if (fabsf(lp[i]) > 1e-3f) plateHits++; }
  check(plateHits > roomHits, "plate builds denser early diffusion than the room algorithm");
  free(lr); free(rr); free(lp); free(rp);
}

/* Drive a continuous sine of frequency `freq` Hz through the reverb (mix=1 wet-only) and return the
 * steady-state output RMS measured over the second half of the buffer (past the tail build-up). */
static float sine_out_rms(float freq, float lowcut, float sr, int n) {
  struct omx_reverb_state *s = make_state(sr);
  struct omx_reverb p = { .enabled = 1, .algorithm = OMX_REVERB_ROOM, .size = 0.7f, .damping = 0.3f,
                          .predelay_ms = 0.0f, .width = 1.0f, .mix = 1.0f,
                          .lowcut = lowcut, .highcut = 20000.0f };
  float *l = malloc(sizeof(float) * n), *r = malloc(sizeof(float) * n);
  const float w = 2.0f * 3.14159265f * freq / sr;
  for (int i = 0; i < n; i++) { l[i] = sinf(w * i); r[i] = sinf(w * i); }
  for (int off = 0; off < n; off += 256) {
    int len = (n - off) < 256 ? (n - off) : 256;
    omx_reverb_process(l + off, r + off, len, &p, s, sr);
  }
  float e = 0.0f; int a = n / 2;
  for (int i = a; i < n; i++) e += l[i] * l[i];
  float rms = sqrtf(e / (float)(n - a));
  free(l); free(r); free(s->_pool); free(s);
  return rms;
}

static void test_lowcut_attenuates_low_leaves_mid(void) {
  const int N = at(48000); float sr = g_sr;
  const float LOWCUT = 800.0f;
  float lowNo = sine_out_rms(40.0f, 0.0f, sr, N);     // 40 Hz, low-cut open
  float lowCut = sine_out_rms(40.0f, LOWCUT, sr, N);  // 40 Hz, low-cut at 800 Hz
  float midNo = sine_out_rms(3000.0f, 0.0f, sr, N);   // 3 kHz, low-cut open
  float midCut = sine_out_rms(3000.0f, LOWCUT, sr, N);// 3 kHz, low-cut at 800 Hz
  // the low tone is strongly attenuated (well below half its uncut level)...
  check(lowCut < lowNo * 0.5f, "lowcut attenuates a 40 Hz component");
  // ...while the mid tone (well above the corner) is essentially unchanged.
  check(midCut > midNo * 0.9f, "lowcut leaves a 3 kHz component ~unchanged");
  // and the low tone is far more affected than the mid tone.
  check((lowCut / lowNo) < (midCut / midNo) * 0.5f, "lowcut is frequency-selective (low << mid)");
}

/* A bypassed-then-re-enabled reverb must start clean. The mixer's re-enable path zeroes the pool and
 * re-lays it out at the pool's sample rate; this validates that reset primitive: after a tail is
 * built, that clear makes a silence-fed reverb emit nothing (vs. a non-cleared state that bleeds the
 * buffered reverberation). */
static void test_reenable_clears_stale_tail(void) {
  const int N = at(4000); float sr = g_sr;
  struct omx_reverb p = { .enabled = 1, .algorithm = OMX_REVERB_ROOM, .size = 0.8f, .damping = 0.3f,
                          .predelay_ms = 0.0f, .width = 1.0f, .mix = 1.0f,
                          .lowcut = 0.0f, .highcut = 20000.0f, .plate_mod_depth = OMX_REVERB_PLATE_MOD_DEPTH_DEFAULT };
  float *l = malloc(sizeof(float) * N), *r = malloc(sizeof(float) * N);
  /* dirty reference: build a tail, then feed silence WITHOUT clearing -> the buffered tail bleeds. */
  struct omx_reverb_state *dirty = make_state(sr);
  memset(l, 0, sizeof(float) * N); memset(r, 0, sizeof(float) * N); l[0] = 1.0f; r[0] = 1.0f;
  for (int off = 0; off < N; off += 256) { int len = (N - off) < 256 ? (N - off) : 256; omx_reverb_process(l + off, r + off, len, &p, dirty, sr); }
  memset(l, 0, sizeof(float) * N); memset(r, 0, sizeof(float) * N); /* silence in */
  for (int off = 0; off < N; off += 256) { int len = (N - off) < 256 ? (N - off) : 256; omx_reverb_process(l + off, r + off, len, &p, dirty, sr); }
  check(tail_energy(l, r, 0, N) > 1e-6f, "without clearing, a re-fed reverb still emits the buffered tail");
  free(dirty->_pool); free(dirty);
  /* clean: build a tail, then apply the mixer's clear (memset pool + re-layout at the pool's sr). */
  struct omx_reverb_state *s = make_state(sr);
  memset(l, 0, sizeof(float) * N); memset(r, 0, sizeof(float) * N); l[0] = 1.0f; r[0] = 1.0f;
  for (int off = 0; off < N; off += 256) { int len = (N - off) < 256 ? (N - off) : 256; omx_reverb_process(l + off, r + off, len, &p, s, sr); }
  memset(s->_pool, 0, (size_t)s->pool_len * sizeof(float));
  omx_reverb_state_layout(s, s->_pool, s->pool_len, s->sr);
  memset(l, 0, sizeof(float) * N); memset(r, 0, sizeof(float) * N); /* silence in */
  for (int off = 0; off < N; off += 256) { int len = (N - off) < 256 ? (N - off) : 256; omx_reverb_process(l + off, r + off, len, &p, s, sr); }
  check(tail_energy(l, r, 0, N) == 0.0f, "clearing the pool + re-layout makes a re-enabled reverb start silent");
  free(l); free(r); free(s->_pool); free(s);
}


/* A pool too small to lay out must leave the reverb INERT — `_pool` NULL, so omx_reverb_process
 * returns immediately — rather than storing NULLs in the sub-buffers for the RT thread to
 * dereference. REACHABLE: mix_reverb.h's per-rate table shows x3.71 headroom at 96 kHz and the
 * pool EXHAUSTING by 384 kHz — a rate this console declares. This test pins the failure mode:
 * an inert reverb, never a NULL deref on the RT thread. */
static void test_exhausted_pool_is_inert_not_a_crash(void) {
  static float tiny[64];
  struct omx_reverb_state st;
  memset(&st, 0, sizeof st);
  omx_reverb_state_layout(&st, tiny, 64, g_sr); /* nowhere near enough */
  check(st._pool == NULL, "an exhausted pool leaves the reverb inert (no NULL line to deref)");
  float l[8] = {0}, r[8] = {0};
  struct omx_reverb p;
  memset(&p, 0, sizeof p);
  p.enabled = 1; p.mix = 1.0f; p.size = 0.5f;
  omx_reverb_process(l, r, 8, &p, &st, g_sr); /* must be a no-op, not a segfault */
  int untouched = 1;
  for (int i = 0; i < 8; i++) if (l[i] != 0.0f || r[i] != 0.0f) untouched = 0;
  check(untouched, "an inert reverb writes nothing");
}


/* The arms moved here from mix_fx_rt_review.test.c (2026-09-25-native-fx-rt-review.md) report a
 * measurement beside the verdict. */
static void review_ok(int cond, const char *what, double measured, double limit) {
  char b[320];
  snprintf(b, sizeof b, "%s — measured %.9g, limit %.9g", what, measured, limit);
  check(cond, b);
}
static const float REVIEW_RATES[4] = {44100.0f, 48000.0f, 96000.0f, 192000.0f};

/* ---- F1 --------------------------------------------------------------------------------------- */

static float *new_pool(struct omx_reverb_state *s, float sr) {
  float *pool = calloc(OMX_REVERB_POOL_FLOATS, sizeof(float));
  memset(s, 0, sizeof(*s));
  omx_reverb_state_layout(s, pool, OMX_REVERB_POOL_FLOATS, sr);
  return pool;
}

/*
 * A live pace write (96 -> 48 kHz, the console's normal rate change) leaves every ENGAGED reverb
 * holding the pool it was laid out with: the setter only re-lays out on a disabled -> enabled
 * edge (mixer_strip.c) and the rate poll re-pushes the EQ alone (console-rig.ts pollSampleRate).
 * The RT then hands omx_reverb_process the NEW rate every block. The kernel's own precondition
 * names this ("rate-matches-layout") and the release build runs straight past it.
 *
 * The law: the output of a mismatched call is EITHER bit-identical to the same call on a pool
 * laid out at the rate it runs at (the room the operator dialled), OR bit-identical to the input
 * (an announced passthrough, the drive's own `state-built-for-the-factor` guard). Anything else is
 * a room of one size wearing another's times.
 */
static void reverb_runs_no_room_cut_at_another_rate(void) {
  static const int ALGOS[5] = {OMX_REVERB_ROOM, OMX_REVERB_PLATE, OMX_REVERB_HALL,
                               OMX_REVERB_REVERSE, OMX_REVERB_GATED};
  for (int from = 0; from < 4; from++) {
    for (int to = 0; to < 4; to++) {
      /* from == to is the PRESENCE CONTROL: the same comparison must pass on a matched layout,
       * or a red below would only prove the comparison cannot pass. */
      const float sr = REVIEW_RATES[to];
      const uint32_t n = (uint32_t)(0.25f * sr);
      for (int a = 0; a < 5; a++) {
        struct omx_reverb_state stale, fresh;
        float *p1 = new_pool(&stale, REVIEW_RATES[from]);
        float *p2 = new_pool(&fresh, sr);
        struct omx_reverb p;
        memset(&p, 0, sizeof p);
        p.enabled = 1;
        p.algorithm = ALGOS[a];
        p.plate_mod_depth = OMX_REVERB_PLATE_MOD_DEPTH_DEFAULT;
        p.size = 0.7f;
        p.damping = 0.3f;
        p.width = 1.0f;
        p.mix = 1.0f;
        p.reverse_ms = 300.0f;
        p.hold_ms = 120.0f;
        p.release_ms = 20.0f;
        p.gate_threshold_db = -40.0f;
        p.plate_mod_depth = OMX_REVERB_PLATE_MOD_DEPTH_DEFAULT;
        float *il = calloc(n, sizeof(float)), *ir = calloc(n, sizeof(float));
        float *sl = calloc(n, sizeof(float)), *sr_ = calloc(n, sizeof(float));
        float *fl = calloc(n, sizeof(float)), *fr = calloc(n, sizeof(float));
        il[0] = ir[0] = 1.0f;
        memcpy(sl, il, n * sizeof(float));
        memcpy(sr_, ir, n * sizeof(float));
        memcpy(fl, il, n * sizeof(float));
        memcpy(fr, ir, n * sizeof(float));
        omx_reverb_process(sl, sr_, n, &p, &stale, sr);
        omx_reverb_process(fl, fr, n, &p, &fresh, sr);
        const int as_live_rate = memcmp(sl, fl, n * sizeof(float)) == 0 &&
                                 memcmp(sr_, fr, n * sizeof(float)) == 0;
        const int passthrough = memcmp(sl, il, n * sizeof(float)) == 0 &&
                                memcmp(sr_, ir, n * sizeof(float)) == 0;
        /* the first wet sample, for the failure line: where the stale room answers vs the live one */
        int first_stale = -1, first_fresh = -1;
        for (uint32_t i = 1; i < n && (first_stale < 0 || first_fresh < 0); i++) {
          if (first_stale < 0 && fabsf(sl[i]) > 1e-9f) first_stale = (int)i;
          if (first_fresh < 0 && fabsf(fl[i]) > 1e-9f) first_fresh = (int)i;
        }
        char what[192];
        snprintf(what, sizeof what,
                 "F1%s reverb algo %d laid out at %.0f Hz, run at %.0f Hz: is the live rate's room "
                 "or a passthrough (first wet sample stale vs live)",
                 from == to ? " CONTROL" : "", ALGOS[a], (double)REVIEW_RATES[from], (double)sr);
        review_ok(as_live_rate || passthrough, what, (double)first_stale, (double)first_fresh);
        free(il); free(ir); free(sl); free(sr_); free(fl); free(fr); free(p1); free(p2);
      }
    }
  }
}

int main(void) {
  omx_fx_require_rate_floor();
  reverb_runs_no_room_cut_at_another_rate();
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = OMX_DECLARED_RATES[ri];
    fprintf(stderr, "fx/reverb at %.0f Hz\n", (double)g_sr);
    test_decaying_diffuse_tail_both();
    test_plate_damping_never_shortens_the_tail();
    test_plate_damping_shortens_the_hf_decay();
    test_reverse_swells_where_room_decays();
    test_reverse_reverses_the_order_of_events();
    test_gated_envelope_is_the_ratio_to_room();
    test_mix_zero_bit_identical();
    test_damping_reduces_hf_tail();
    test_predelay_shifts_onset();
    test_plate_denser_early_diffusion_than_room();
    test_hall_tail_outlasts_plate();
    test_lowcut_attenuates_low_leaves_mid();
    test_reenable_clears_stale_tail();
    test_exhausted_pool_is_inert_not_a_crash();
  }
  printf("fx/reverb: %d checks, %d failures (%d rates)\n", g_checks, g_fail, (int)OMX_DECLARED_RATE_COUNT);
  return g_fail == 0 ? 0 : 1;
}
