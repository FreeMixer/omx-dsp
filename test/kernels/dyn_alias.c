// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/omx_dyn.h>
#include <omxdsp/omx_fft.h>
#include <omxdsp/omx_oversampler.h>

/* ---- dyn_alias: the comp's in-band alias on a two-tone, and the 4x path's transparency ---------
 * Moved from openmixer's tools/oversample-probe.c (its self-check (1) and its detector GATE), the
 * one place the alias figure the 4x control path exists for was asserted.
 *
 * THE TWO-TONE. 0.35 per tone at bins 683 and 10243 of a 65536-point window (1000.49 Hz and
 * 15004.6 Hz at 96 kHz), both prime and coprime to 2^16 so the folded product lattice does not
 * collapse onto the true one. Bin-exact, so a rectangular window leaks nothing and every product
 * of order m + q <= 15 lands on the exact bin |m*683 + q*10243|, enumerated as INTEGERS:
 *   WANTED  a product whose true bin is at or below Nyquist (or the tones themselves);
 *   ALIAS   a product above Nyquist whose folded bin lands below it on no wanted bin;
 *   a fold landing on a wanted bin stays WANTED, so the alias figure is UNDERSTATED, never inflated.
 * The figure gated is the worst ALIAS bin at or below 20 kHz (the audible band), in dB re the
 * 1 kHz fundamental. The analyser is omx_fft_radix2_d on the float32 output (test/kernels/fft.c).
 *
 * THE COMP. Peak detect, 0.1 ms attack (the fastest COMP_SPECS offers), 50 ms release, -20 dB
 * threshold, 10:1, hard knee, run through omx_dynamics in 256-frame blocks after 32768 frames of
 * settling (341 ms at 96 kHz, the release's steady state). Three rows, two clauses -- a threshold
 * alone cannot tell "the kernel is clean" from "the analyser went blind":
 *   ovs off (the shipped cascade)          worst in-band alias <= -55 dBc
 *   ovs auto, engaged at 0.1 ms (4x path)  worst in-band alias <= -100 dBc (the operator's line)
 *   the PRE-FIX one-pole envelope          worst in-band alias >  -55 dBc (the positive control:
 *                                          the detector omx_dynamics carried until 2026-09-14,
 *                                          spelled out here with the kernel's own gain computer)
 * ONE RATE, 96 kHz, as the probe had it: the classifier's band is the delivery Nyquist, 48 kHz,
 * which is 96 kHz's own (the REAC segment caps the rig at 96 kHz), and the tones are bin-exact at
 * 96 kHz only. The kernel's other inputs are rate-blind: the oversampler takes no rate at all.
 *
 * SENSITIVITY. The probe's self-check (2), the classifier's own control: the two-tone hard-clipped
 * at 0.35 with no oversampling must show its worst alias anywhere below 48 kHz above -60 dBc.
 *
 * TRANSPARENCY. omx_oversampler up then down at x2 and x4 with NO nonlinearity, on the same
 * two-tone: the output is the input delayed by the declared latency, residual < -100 dB re the
 * signal over the 65536-frame window. The probe's own precondition (its self-check (1)): the
 * table's round trip is the identity, so a product it reports is the stage's, not the filter's.
 * The oversampler takes no rate and the two-tone is defined in samples, so this holds at every
 * rate by construction; it is run once. ----------------------------------------------------------- */

#define DA_NTT 65536u
#define DA_PRE 32768u
#define DA_POST 4096u
#define DA_LEN (DA_PRE + DA_NTT + DA_POST)
#define DA_K1 683L
#define DA_K2 10243L
#define DA_ORDER 15
#define DA_AMP 0.35
#define DA_SR 96000.0f

static void da_two_tone(float *x, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    x[i] = (float)(DA_AMP * (sin(2.0 * M_PI * (double)DA_K1 * (double)i / (double)DA_NTT) +
                             sin(2.0 * M_PI * (double)DA_K2 * (double)i / (double)DA_NTT)));
}

/* Amplitude per bin of DA_NTT samples from DA_PRE, rectangular window (the tones are bin-exact). */
static void da_spectrum(const float *x, double *mag) {
  static double re[DA_NTT], im[DA_NTT];
  for (uint32_t i = 0; i < DA_NTT; i++) { re[i] = (double)x[DA_PRE + i]; im[i] = 0.0; }
  omx_fft_radix2_d(re, im, DA_NTT, 0);
  for (uint32_t b = 0; b <= DA_NTT / 2u; b++) {
    double m = sqrt(re[b] * re[b] + im[b] * im[b]) * 2.0 / (double)DA_NTT;
    if (b == 0u || b == DA_NTT / 2u) m *= 0.5;
    mag[b] = m;
  }
}

/* The worst ALIAS bin at or below `hz`, in dB re the fundamental at DA_K1. */
static double da_worst_alias_dbc(const double *mag, double hz) {
  static uint8_t cls[DA_NTT / 2u + 1u];
  const long nyq = (long)(DA_NTT / 2u), top = (long)(hz / (DA_SR / (double)DA_NTT));
  memset(cls, 0, sizeof cls);
  for (int m = -DA_ORDER; m <= DA_ORDER; m++)
    for (int q = -DA_ORDER; q <= DA_ORDER; q++) {
      const int o = abs(m) + abs(q);
      const long b = labs((long)m * DA_K1 + (long)q * DA_K2);
      if (o != 0 && o <= DA_ORDER && b <= nyq) cls[b] = 1u; /* wanted: a true product or a tone */
    }
  for (int m = -DA_ORDER; m <= DA_ORDER; m++)
    for (int q = -DA_ORDER; q <= DA_ORDER; q++) {
      const int o = abs(m) + abs(q);
      const long b = labs((long)m * DA_K1 + (long)q * DA_K2);
      if (o == 0 || o > DA_ORDER || b <= nyq) continue;
      long bf = b % (long)DA_NTT;
      if (bf > nyq) bf = (long)DA_NTT - bf;
      if (cls[bf] == 0u) cls[bf] = 2u; /* a fold onto a wanted bin stays wanted: understated */
    }
  double worst = 0.0;
  for (long b = 8; b <= top && b <= nyq; b++)
    if (cls[b] == 2u && mag[b] > worst) worst = mag[b];
  return worst <= 1e-30 ? -999.0 : 20.0 * log10(worst / mag[DA_K1]);
}

static struct omx_dyn da_comp(int ovs_mode) {
  struct omx_dyn p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.gc.mode = OMX_DYN_ABOVE;
  p.detect = OMX_DETECT_PEAK;
  p.gc.thresh_db = -20.0f;
  p.gc.ratio = 10.0f;
  p.gc.makeup_lin = 1.0f;
  p.attack_ms = 0.1f;
  p.ovs_mode = ovs_mode;
  p.attack_coeff = expf(-1.0f / (0.1f * 0.001f * DA_SR));
  p.release_coeff = expf(-1.0f / (50.0f * 0.001f * DA_SR));
  return p;
}

static void da_run_comp(const float *in, float *out, int ovs_mode) {
  const struct omx_dyn p = da_comp(ovs_mode);
  static struct omx_dyn_state st;
  omx_dyn_state_init(&st, omx_dyn_oversample_factor(&p));
  memcpy(out, in, DA_LEN * sizeof *out);
  for (uint32_t i = 0; i < DA_LEN; i += 256u) omx_dynamics(out + i, NULL, 256u, &p, &st);
}

/* The pre-fix detector: one pole on |x|, then the kernel's own gain computer and units. */
static void da_run_one_pole(const float *in, float *out) {
  const struct omx_dyn p = da_comp(OMX_DYN_OVS_OFF);
  float e = 0.0f;
  for (uint32_t i = 0; i < DA_LEN; i++) {
    const float x = in[i], d = fabsf(x);
    const float c = d > e ? p.attack_coeff : p.release_coeff;
    e = c * e + (1.0f - c) * d;
    out[i] = x * omx_db_to_lin(omx_gaincomp_db(&p.gc, omx_lin_to_db(e))) * p.gc.makeup_lin;
  }
}

static void arm_dyn_alias(void) {
  g_arm = "dyn_alias";
  static float in[DA_LEN], out[DA_LEN], up[4u * 256u];
  static double mag[DA_NTT / 2u + 1u];
  da_two_tone(in, DA_LEN);

  for (uint32_t factor = 2u; factor <= 4u; factor *= 2u) {
    struct omx_oversampler o;
    omx_oversampler_init(&o, factor);
    for (uint32_t i = 0; i < DA_LEN; i += 256u) {
      omx_oversampler_up(&o, in + i, 256u, up);
      omx_oversampler_down(&o, up, 256u, out + i);
    }
    const uint32_t lat = omx_oversampler_latency(&o);
    double num = 0.0, den = 0.0;
    for (uint32_t i = DA_PRE; i < DA_PRE + DA_NTT; i++) {
      const double e = (double)out[i + lat] - (double)in[i];
      num += e * e;
      den += (double)in[i] * (double)in[i];
    }
    const double d = 10.0 * log10(num / den);
    ok(d < -100.0, "up then down with no nonlinearity is the two-tone, residual dB re signal", d, -100.0);
    if (getenv("OMXDSP_DYN_ALIAS_PRINT")) printf("dyn_alias: x%u transparency %.1f dB\n", factor, d);
  }

  /* the classifier's own sensitivity: plain hard clipping at 0.35 aliases grossly, 0..48 kHz */
  for (uint32_t i = 0; i < DA_LEN; i++) out[i] = in[i] > 0.35f ? 0.35f : (in[i] < -0.35f ? -0.35f : in[i]);
  da_spectrum(out, mag);
  const double clip = da_worst_alias_dbc(mag, 0.5 * DA_SR);
  ok(clip > -60.0, "a plain hard clip at 0.35 shows its alias above -60 dBc (the classifier sees)", clip, -60.0);

  da_run_comp(in, out, OMX_DYN_OVS_OFF);
  da_spectrum(out, mag);
  const double shipped = da_worst_alias_dbc(mag, 20000.0);
  ok(shipped <= -55.0, "comp, ovs off, 0.1 ms: worst alias <= 20 kHz on the two-tone, dBc", shipped, -55.0);

  da_run_comp(in, out, OMX_DYN_OVS_AUTO);
  da_spectrum(out, mag);
  const double engaged = da_worst_alias_dbc(mag, 20000.0);
  ok(engaged <= -100.0, "comp, ovs auto engaged at 0.1 ms: worst alias <= 20 kHz, dBc", engaged, -100.0);

  da_run_one_pole(in, out);
  da_spectrum(out, mag);
  const double control = da_worst_alias_dbc(mag, 20000.0);
  ok(control > -55.0, "the pre-fix one-pole detector still FAILS -55 dBc (the analyser can see)", control, -55.0);

  if (getenv("OMXDSP_DYN_ALIAS_PRINT"))
    printf("dyn_alias: clip %.1f, ovs off %.1f, auto 4x %.1f, pre-fix %.1f dBc\n", clip, shipped, engaged, control);
  expect_clean();
}
