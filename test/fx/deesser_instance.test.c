// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * deesser_instance.test.c — the de-esser instance core (omx_deesser_instance.h), every arm at
 * every rate in OMX_DECLARED_RATES:
 *   A  a refused init (no rate, a rate the console does not declare) is the identity; every
 *      declared rate is accepted; the published latency is zero;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_deess_process on the atom the
 *      console's resolve_fx_deess loads, its band the section core's deEsserBandSection designs
 *      (transcribed here from eq.ts, not read from the face), in both modes;
 *   D  every knob, at every hostile host value, lands inside its declared travel (non-finite at
 *      the declared default), a mode index outside the set reads as split, and the output is
 *      finite;
 *   E  a bypass->engaged edge keeps the state, as the console's stage does: the bypassed blocks
 *      touch no state word;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves each the instance alone (no shared state);
 *   H  init, resolve and run allocate nothing: malloc, calloc, realloc and free are wrapped at
 *      link time and counted across the instance's whole life.
 *   make test-fx
 */
#include <limits.h>
#include <math.h>
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_deesser_instance.h>

#include "instance_harness.h"

#define BLK 256u
#define NBLK 12
#define NKNOBS 7

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

/* The programme with a sibilant on top: 7 and 15 kHz tones gated on and off, so the detector moves. */
static void voice(float *l, float *r, uint32_t n, uint32_t t0) {
  programme(l, r, n, t0);
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t t = t0 + i;
    const float gate = ((t / (uint32_t)(0.004f * g_sr)) & 1u) ? 0.0f : 0.5f;
    const float w = 2.0f * 3.14159265f * (float)t / g_sr;
    const float s = gate * (sinf(7000.0f * w) + 0.5f * sinf(15000.0f * w));
    l[i] += s;
    r[i] += 0.8f * s;
  }
}

/* core's deEsserBandSection (eq.ts: bandwidthOctavesQ then rbjSection('bandpass')), transcribed. */
static void console_band(float freq, float width, float sr, float out[5]) {
  const double nyq = (double)sr * 0.5;
  const double f0 = fmin(fmax((double)freq, 1.0), nyq * 0.999);
  const double w0 = (2.0 * 3.141592653589793 * f0) / (double)sr;
  const double q = 1.0 / (2.0 * sinh(((0.6931471805599453 / 2.0) * (double)width * w0) / sin(w0)));
  const double alpha = sin(w0) / (2.0 * q);
  const double a0 = 1.0 + alpha;
  const double c[5] = {alpha / a0, 0.0, -alpha / a0, -2.0 * cos(w0) / a0, (1.0 - alpha) / a0};
  for (int i = 0; i < 5; i++) out[i] = (float)c[i];
}

/* The console's resolve_fx_deess, from the same knobs (the knee the controller's constant). */
static struct omx_deess console_atom(const float k[NKNOBS], int mode) {
  struct omx_deess o;
  memset(&o, 0, sizeof o);
  o.enabled = 1;
  o.mode = mode;
  o.dyn.enabled = 1;
  o.dyn.gc.mode = OMX_DYN_ABOVE;
  o.dyn.detect = OMX_DETECT_PEAK;
  o.dyn.gc.makeup_lin = 1.0f;
  o.dyn.ovs_mode = OMX_DYN_OVS_OFF;
  o.dyn.gc.thresh_db = k[2];
  o.dyn.gc.ratio = k[3];
  o.dyn.gc.knee_db = (float)OMX_DEESS_KNEE_DB;
  o.dyn.gc.range_db = k[4];
  o.dyn.attack_ms = k[5];
  o.dyn.attack_coeff = omx_pole_from_time_ms(k[5], g_sr);
  o.dyn.release_coeff = omx_pole_from_time_ms(k[6], g_sr);
  console_band(k[0], k[1], g_sr, o.bp_c);
  return o;
}

static void resolve(OmxDeesserInstance *s, int bypass, const float k[NKNOBS], int mode) {
  omx_deesser_instance_resolve(s, bypass, k[0], k[1], k[2], k[3], k[4], k[5], k[6], mode);
}

static const float LO[NKNOBS] = {OMX_DEESS_FREQ_RANGE_MIN,  OMX_DEESS_WIDTH_RANGE_MIN, OMX_DEESS_THRESHOLD_RANGE_MIN,
                                 OMX_DEESS_RATIO_RANGE_MIN, OMX_DEESS_RANGE_RANGE_MIN, OMX_DEESS_ATTACK_RANGE_MIN,
                                 OMX_DEESS_RELEASE_RANGE_MIN};
static const float HI[NKNOBS] = {OMX_DEESS_FREQ_RANGE_MAX,  OMX_DEESS_WIDTH_RANGE_MAX, OMX_DEESS_THRESHOLD_RANGE_MAX,
                                 OMX_DEESS_RATIO_RANGE_MAX, OMX_DEESS_RANGE_RANGE_MAX, OMX_DEESS_ATTACK_RANGE_MAX,
                                 OMX_DEESS_RELEASE_RANGE_MAX};
static const float DEF[NKNOBS] = {OMX_DEESS_FREQ_RANGE_DEFAULT,  OMX_DEESS_WIDTH_RANGE_DEFAULT,
                                  OMX_DEESS_THRESHOLD_RANGE_DEFAULT, OMX_DEESS_RATIO_RANGE_DEFAULT,
                                  OMX_DEESS_RANGE_RANGE_DEFAULT,  OMX_DEESS_ATTACK_RANGE_DEFAULT,
                                  OMX_DEESS_RELEASE_RANGE_DEFAULT};

static float land(float x, float lo, float hi, float def) {
  if (x - x != 0.0f) return def;
  return x < lo ? lo : x > hi ? hi : x;
}

static int same_atom(const struct omx_deess *a, const struct omx_deess *b) {
  return a->enabled == b->enabled && a->mode == b->mode && a->dyn.enabled == b->dyn.enabled &&
         a->dyn.gc.mode == b->dyn.gc.mode && a->dyn.detect == b->dyn.detect &&
         a->dyn.gc.makeup_lin == b->dyn.gc.makeup_lin && a->dyn.ovs_mode == b->dyn.ovs_mode &&
         a->dyn.gc.thresh_db == b->dyn.gc.thresh_db && a->dyn.gc.ratio == b->dyn.gc.ratio &&
         a->dyn.gc.knee_db == b->dyn.gc.knee_db && a->dyn.gc.range_db == b->dyn.gc.range_db &&
         a->dyn.attack_ms == b->dyn.attack_ms && a->dyn.attack_coeff == b->dyn.attack_coeff &&
         a->dyn.release_coeff == b->dyn.release_coeff && memcmp(a->bp_c, b->bp_c, sizeof a->bp_c) == 0;
}

static const float KNOBS[NKNOBS] = {7000.0f, 1.0f, -40.0f, 6.0f, -18.0f, 0.5f, 40.0f};

static void arm_refused(void) {
  OmxDeesserInstance s;
  ok(omx_deesser_instance_init(&s, 0.0f) == 0, "A: rate 0 refused");
  ok(omx_deesser_instance_init(&s, NAN) == 0, "A: NaN rate refused");
  ok(omx_deesser_instance_init(&s, g_sr + 1.0f) == 0, "A: an undeclared rate refused");
  resolve(&s, 0, KNOBS, OMX_DEESS_WIDEBAND);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  voice(l, r, BLK, 0);
  omx_deesser_instance_run(&s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_deesser_instance_init(&s, g_sr) == 1, "A: the declared rate is accepted");
  ok(omx_deesser_instance_latency(&s) == 0u, "A: the published latency is zero frames");
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  OmxDeesserInstance s;
  omx_deesser_instance_init(&s, g_sr);
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    voice(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    resolve(&s, 1, KNOBS, OMX_DEESS_SPLIT);
    omx_deesser_instance_run(&s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_deesser_instance_run(&s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const float K[4][NKNOBS] = {{7000.0f, 1.0f, -40.0f, 6.0f, -18.0f, 0.5f, 40.0f},
                                     {2000.0f, 4.0f, -60.0f, 20.0f, -24.0f, 0.1f, 5.0f},
                                     {16000.0f, 0.25f, -30.0f, 4.0f, -12.0f, 1.0f, 60.0f},
                                     {5500.0f, 2.0f, -50.0f, 1.0f, 0.0f, 50.0f, 500.0f}};
  for (int k = 0; k < 4; k++) {
    for (int mode = OMX_DEESS_SPLIT; mode <= OMX_DEESS_WIDEBAND; mode++) {
      OmxDeesserInstance s;
      omx_deesser_instance_init(&s, g_sr);
      struct omx_deess_state ks;
      omx_deess_state_init(&ks);
      const struct omx_deess a = console_atom(K[k], mode);
      float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
      int same = 1, wet = 0;
      for (int b = 0; b < NBLK; b++) {
        voice(l, r, BLK, (uint32_t)b * BLK);
        memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
        resolve(&s, 0, K[k], mode);
        omx_deesser_instance_run(&s, l, r, ol, or_, BLK);
        omx_deess_process(l, r, BLK, &a, &ks);
        same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
        wet |= !same_bytes(ol, l0, BLK) || !same_bytes(or_, r0, BLK);
      }
      char what[160];
      snprintf(what, sizeof what, "C: setting %d, mode %d: the engaged instance is not a wire", k, mode);
      ok(k == 3 ? 1 : wet, what); /* setting 3 is ratio 1 and range 0: the identity by construction */
      snprintf(what, sizeof what,
               "C: setting %d, mode %d: the engaged instance is omx_deess_process on the console's atom, bit for bit",
               k, mode);
      ok(same, what);
    }
  }
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  OmxDeesserInstance s;
  omx_deesser_instance_init(&s, g_sr);
  float l[BLK], r[BLK];
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    for (int knob = 0; knob < NKNOBS; knob++) {
      float in[NKNOBS], want[NKNOBS];
      memcpy(in, KNOBS, sizeof in);
      in[knob] = x;
      memcpy(want, in, sizeof want);
      want[knob] = land(x, LO[knob], HI[knob], DEF[knob]);
      resolve(&s, 0, in, OMX_DEESS_SPLIT);
      const struct omx_deess e = console_atom(want, OMX_DEESS_SPLIT);
      char what[128];
      snprintf(what, sizeof what, "D: knob %d at %g lands at %g", knob, (double)x, (double)want[knob]);
      ok(same_atom(&s.atom, &e), what);
      voice(l, r, BLK, (uint32_t)(h * NKNOBS + knob) * BLK);
      omx_deesser_instance_run(&s, l, r, l, r, BLK);
      ok(all_finite(l, BLK) && all_finite(r, BLK), "D: hostile knobs leave the output finite");
    }
  }
  static const int IDX[] = {INT_MIN, -1, OMX_DEESS_SPLIT, OMX_DEESS_WIDEBAND, OMX_DEESS_WIDEBAND + 1, INT_MAX};
  for (int i = 0; i < (int)(sizeof IDX / sizeof IDX[0]); i++) {
    resolve(&s, 0, KNOBS, IDX[i]);
    const int want = IDX[i] == OMX_DEESS_WIDEBAND ? OMX_DEESS_WIDEBAND : OMX_DEESS_SPLIT;
    char what[128];
    snprintf(what, sizeof what, "D: mode %d reads as %d", IDX[i], want);
    ok(s.atom.mode == want, what);
  }
  drain_violations("D: no contract broken");
}

static void arm_reengage_keeps(void) {
  OmxDeesserInstance s;
  omx_deesser_instance_init(&s, g_sr);
  float l[BLK], r[BLK];
  for (int b = 0; b < 4; b++) {
    voice(l, r, BLK, (uint32_t)b * BLK);
    resolve(&s, 0, KNOBS, OMX_DEESS_SPLIT);
    omx_deesser_instance_run(&s, l, r, l, r, BLK);
  }
  const struct omx_deess_state before = s.state;
  for (int b = 4; b < 8; b++) {
    voice(l, r, BLK, (uint32_t)b * BLK);
    resolve(&s, 1, KNOBS, OMX_DEESS_SPLIT);
    omx_deesser_instance_run(&s, l, r, l, r, BLK);
  }
  resolve(&s, 0, KNOBS, OMX_DEESS_SPLIT);
  ok(memcmp(&before, &s.state, sizeof before) == 0, "E: the bypassed blocks and the re-engage touch no state word");
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  OmxDeesserInstance a, b, c, alone;
  omx_deesser_instance_init(&a, g_sr);
  omx_deesser_instance_init(&b, g_sr);
  omx_deesser_instance_init(&c, g_sr);
  omx_deesser_instance_init(&alone, g_sr);
  static const float Q[NKNOBS] = {4000.0f, 3.0f, -55.0f, 10.0f, -24.0f, 0.2f, 20.0f};
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK], xl[BLK], xr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < NBLK; k++) {
    voice(l, r, BLK, (uint32_t)k * BLK);
    resolve(&a, 0, KNOBS, OMX_DEESS_SPLIT);
    resolve(&b, 0, KNOBS, OMX_DEESS_SPLIT);
    resolve(&alone, 0, KNOBS, OMX_DEESS_SPLIT);
    resolve(&c, 0, Q, OMX_DEESS_WIDEBAND);
    omx_deesser_instance_run(&a, l, r, al, ar, BLK);
    voice(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_deesser_instance_run(&c, cl, cr, cl, cr, BLK); /* a different one between */
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_deesser_instance_run(&b, bl, br, bl, br, BLK);
    omx_deesser_instance_run(&alone, l, r, xl, xr, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
    indep &= same_bytes(al, xl, BLK) && same_bytes(ar, xr, BLK);
  }
  ok(alias, "F: in place is the out-of-place answer");
  ok(indep, "G: an instance between two others leaves them each the instance alone");
  drain_violations("F/G: no contract broken");
}

static void arm_no_allocation(void) {
  static OmxDeesserInstance s;
  float l[BLK], r[BLK];
  g_allocs = 0;
  g_counting = 1;
  const int ready = omx_deesser_instance_init(&s, g_sr);
  for (int b = 0; b < NBLK; b++) {
    float k[NKNOBS];
    memcpy(k, KNOBS, sizeof k);
    k[0] = b < 6 ? 7000.0f : 9000.0f;
    voice(l, r, BLK, (uint32_t)b * BLK);
    resolve(&s, b == 5, k, b & 1);
    omx_deesser_instance_run(&s, l, r, l, r, BLK);
  }
  g_counting = 0;
  ok(ready, "H: the instance came up");
  ok(g_allocs == 0, "H: init, resolve, a re-engage and run allocate nothing");
  /* The counter is live: an allocation inside the window is counted. Called through volatile
   * pointers, so the compiler cannot fold the pair away. */
  void *(*volatile alloc)(size_t) = malloc;
  void (*volatile release)(void *) = free;
  g_counting = 1;
  release(alloc(16));
  g_counting = 0;
  ok(g_allocs == 2, "H: the allocation counter counts (a malloc and a free inside the window)");
  drain_violations("H: no contract broken");
}

int main(void) {
  omx_fx_require_rate_floor();
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = OMX_DECLARED_RATES[ri];
    arm_refused();
    arm_bypass();
    arm_is_the_kernel();
    arm_clamps();
    arm_reengage_keeps();
    arm_alias_and_independence();
    arm_no_allocation();
  }
  return finish("deesser_instance");
}
