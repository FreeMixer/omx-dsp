// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * drive_instance.test.c — the drive instance face (omx_drive_instance.h), every arm at every rate
 * in OMX_DECLARED_RATES:
 *   A  a rate the console does not declare is refused and the face is the identity; init resolves
 *      the contract's defaults: engaged, the default factor (4x), the latency the kernel declares;
 *   B  bypassed is the identity byte for byte, in place and out of place, with no latency;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_drive_process on the atom the
 *      console's resolve_fx_drive builds from the same controls and the same designed bank, for
 *      every curve and every band;
 *   D  every travel, at every hostile host value, lands inside its declared travel (a non-finite
 *      word at the declared default); a curve or band outside its ids reads as the first, a switch,
 *      roll-off corner or oversampling factor outside its ids as the declared default; the output
 *      is finite;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves them each the instance alone (no shared state);
 *   H  init, resolve (re-engage and factor changes included) and run allocate nothing: malloc,
 *      calloc, realloc and free are wrapped at link time and counted across the instance's whole life;
 * and, at the nine RME rates (32 to 192 kHz; the face where the console declares the rate, the
 * kernel on the console's atom at the three it refuses):
 *   O  every `oversample` factor (1, 2, 4) publishes its closed-form latency (0, 48, 72) and at mix
 *      0 is its input that many samples late, bit for bit; a factor change mid-run, every ordered
 *      pair, takes the new latency at once and is from the block boundary on a fresh drive at the
 *      new factor, bit for bit (the state rebuilt), with the base-rate coefficient bank unchanged.
 *   make test-fx
 */
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_drive_instance.h>

#include "instance_harness.h"

#define BLK 256u
#define NBLK 12

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

/* The contract's controls, in their declared order. */
typedef struct {
  float amount, character, band_freq, mix, trim;
  int curve, band, auto_gain, stereo_link, hf_rolloff, oversample;
} Knobs;

static const Knobs DEFAULTS = {OMX_DRIVE_INSTANCE_AMOUNT_DEFAULT, OMX_DRIVE_INSTANCE_CHARACTER_DEFAULT,
                               OMX_DRIVE_INSTANCE_BAND_FREQ_DEFAULT, OMX_DRIVE_INSTANCE_MIX_DEFAULT,
                               OMX_DRIVE_INSTANCE_TRIM_DEFAULT, OMX_DRIVE_INSTANCE_CURVE_DEFAULT,
                               OMX_DRIVE_INSTANCE_BAND_DEFAULT, OMX_DRIVE_INSTANCE_AUTO_GAIN_DEFAULT,
                               OMX_DRIVE_INSTANCE_STEREO_LINK_DEFAULT, OMX_DRIVE_INSTANCE_HF_ROLLOFF_DEFAULT,
                               OMX_DRIVE_INSTANCE_OVERSAMPLE_DEFAULT};

static void resolve(OmxDriveInstance *s, int bypass, const Knobs *k) {
  omx_drive_instance_resolve(s, bypass, k->amount, k->character, k->band_freq, k->mix, k->trim, k->curve, k->band,
                             k->auto_gain, k->stereo_link, k->hf_rolloff, k->oversample);
}

/* Heap-held: the state's inline oversampler histories make an instance large. */
static OmxDriveInstance *inst(void) { return calloc(1, sizeof(OmxDriveInstance)); }

/* The console's resolve_fx_drive, from the same units and the same designed bank, at the knobs'
 * factor, for arm C. */
static struct omx_drive console_atom(uint32_t rate, const Knobs *k) {
  struct omx_drive o;
  memset(&o, 0, sizeof(o));
  o.enabled = 1;
  o.curve = k->curve;
  o.band = k->band;
  o.drive_lin = omx_db_to_lin(k->amount);
  o.even_w = 0.5f * (k->character + 1.0f);
  o.mix = 0.01f * k->mix;
  o.trim_lin = omx_db_to_lin(k->trim);
  o.auto_gain = k->auto_gain;
  o.stereo_link = k->stereo_link;
  float corner_hz = 0.0f;
  o.hf_on = omx_drive_hf_rolloffs_value(k->hf_rolloff, &corner_hz) ? 1 : 0;
  o.os_factor = k->oversample;
  omx_drive_design_bank(&o, k->band_freq, o.hf_on ? corner_hz : 20000.0f, rate);
  omx_drive_time_constants(&o, (float)rate);
  return o;
}

static void arm_init(void) {
  OmxDriveInstance *s = inst();
  ok(omx_drive_instance_init(s, 0.0f) == 0, "A: rate 0 refused");
  ok(omx_drive_instance_init(s, NAN) == 0, "A: NaN rate refused");
  ok(omx_drive_instance_init(s, g_sr + 1.0f) == 0, "A: an undeclared rate refused");
  resolve(s, 0, &DEFAULTS);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_drive_instance_run(s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_drive_instance_latency(s) == 0u, "A: a refused instance declares no latency");
  ok(omx_drive_instance_init(s, g_sr) == 1, "A: the declared rate is accepted");
  ok(s->atom.enabled == 1, "A: an instance a host racks is engaged");
  ok(s->atom.os_factor == OMX_DRIVE_OVERSAMPLES_DEFAULT && s->state.factor == s->atom.os_factor,
     "A: the factor is the contract's default, the state built for it");
  ok(omx_drive_instance_latency(s) == (uint32_t)omx_drive_latency(&s->atom) && omx_drive_instance_latency(s) > 0u,
     "A: the latency is the kernel's");
  ok(s->atom.mix == 0.01f * OMX_DRIVE_INSTANCE_MIX_DEFAULT &&
         s->atom.drive_lin == omx_db_to_lin(OMX_DRIVE_INSTANCE_AMOUNT_DEFAULT) &&
         s->atom.curve == OMX_DRIVE_SOFT && s->atom.band == OMX_DRIVE_BAND_FULL && s->atom.auto_gain == 1 &&
         s->atom.stereo_link == 1 && s->atom.hf_on == 0,
     "A: the contract's defaults are what init resolved");
  free(s);
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  OmxDriveInstance *s = inst();
  omx_drive_instance_init(s, g_sr);
  Knobs k = DEFAULTS;
  k.amount = 24.0f;
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l); memcpy(r0, r, sizeof r);
    resolve(s, 1, &k);
    omx_drive_instance_run(s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_drive_instance_run(s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  ok(omx_drive_instance_latency(s) == 0u, "B: a bypassed instance declares no latency");
  free(s);
  drain_violations("B: no contract broken");
}

static void arm_is_the_kernel(void) {
  static const Knobs K[] = {
      {0.0f, 0.0f, 2000.0f, 100.0f, 0.0f, OMX_DRIVE_SOFT, OMX_DRIVE_BAND_FULL, 1, 1, 0, 4},
      {30.0f, 0.6f, 900.0f, 70.0f, -6.0f, OMX_DRIVE_EXCITER, OMX_DRIVE_BAND_TILT, 0, 1, OMX_DRIVE_HF_ROLLOFFS_12000, 2},
      {12.0f, -1.0f, 5000.0f, 100.0f, 0.0f, OMX_DRIVE_TAPE, OMX_DRIVE_BAND_LOW, 1, 0, OMX_DRIVE_HF_ROLLOFFS_16000, 1},
      {20.0f, 1.0f, 300.0f, 45.0f, 6.0f, OMX_DRIVE_TUBE, OMX_DRIVE_BAND_HIGH, 0, 0, 0, 2},
      {36.0f, -0.4f, 20.0f, 100.0f, -24.0f, OMX_DRIVE_SOFT, OMX_DRIVE_BAND_TILT, 1, 1, OMX_DRIVE_HF_ROLLOFFS_16000, 1},
      {6.0f, 0.2f, 20000.0f, 10.0f, 12.0f, OMX_DRIVE_TAPE, OMX_DRIVE_BAND_FULL, 0, 1, OMX_DRIVE_HF_ROLLOFFS_12000, 4}};
  for (int k = 0; k < (int)(sizeof K / sizeof K[0]); k++) {
    OmxDriveInstance *s = inst();
    omx_drive_instance_init(s, g_sr);
    const struct omx_drive a = console_atom((uint32_t)g_sr, &K[k]);
    struct omx_drive_state *ks = calloc(1, sizeof *ks);
    omx_drive_state_init(ks, a.os_factor);
    float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK];
    int same = 1, wet = 0;
    for (int b = 0; b < NBLK; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      memcpy(l0, l, sizeof l);
      resolve(s, 0, &K[k]);
      omx_drive_instance_run(s, l, r, ol, or_, BLK);
      omx_drive_process(l, r, BLK, &a, ks);
      same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
      wet |= !same_bytes(ol, l0, BLK);
    }
    ok(wet, "C: the engaged instance is not a wire (the comparison is not of two passthroughs)");
    ok(same, "C: the engaged instance is omx_drive_process on the console's atom, bit for bit");
    free(s); free(ks);
  }
  drain_violations("C: no contract broken");
}

/* Where a hostile word must land: every non-finite word at the default, else inside [lo, hi]. */
static float land(float x, float lo, float hi, float def) {
  if (x - x != 0.0f) return def;
  return x < lo ? lo : x > hi ? hi : x;
}

static void arm_clamps(void) {
  static const float LO[5] = {(float)OMX_DRIVE_AMOUNT_RANGE_MIN, (float)OMX_DRIVE_CHARACTER_RANGE_MIN,
                              (float)OMX_DRIVE_BAND_FREQ_RANGE_MIN, (float)OMX_DRIVE_MIX_RANGE_MIN,
                              (float)OMX_DRIVE_TRIM_RANGE_MIN};
  static const float HI[5] = {(float)OMX_DRIVE_AMOUNT_RANGE_MAX, (float)OMX_DRIVE_CHARACTER_RANGE_MAX,
                              (float)OMX_DRIVE_BAND_FREQ_RANGE_MAX, (float)OMX_DRIVE_MIX_RANGE_MAX,
                              (float)OMX_DRIVE_TRIM_RANGE_MAX};
  static const float DEF[5] = {OMX_DRIVE_INSTANCE_AMOUNT_DEFAULT, OMX_DRIVE_INSTANCE_CHARACTER_DEFAULT,
                               OMX_DRIVE_INSTANCE_BAND_FREQ_DEFAULT, OMX_DRIVE_INSTANCE_MIX_DEFAULT,
                               OMX_DRIVE_INSTANCE_TRIM_DEFAULT};
  float l[BLK], r[BLK];
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    const float x = HOSTILE[h];
    for (int knob = 0; knob < 5; knob++) {
      OmxDriveInstance *s = inst(), *e = inst();
      omx_drive_instance_init(s, g_sr);
      omx_drive_instance_init(e, g_sr);
      float in[5] = {18.0f, 0.3f, 700.0f, 80.0f, -3.0f};
      float want[5];
      memcpy(want, in, sizeof want);
      in[knob] = x;
      want[knob] = land(x, LO[knob], HI[knob], DEF[knob]);
      omx_drive_instance_resolve(s, 0, in[0], in[1], in[2], in[3], in[4], OMX_DRIVE_TUBE, OMX_DRIVE_BAND_TILT, 1, 1,
                                 OMX_DRIVE_HF_ROLLOFFS_12000, 2);
      omx_drive_instance_resolve(e, 0, want[0], want[1], want[2], want[3], want[4], OMX_DRIVE_TUBE,
                                 OMX_DRIVE_BAND_TILT, 1, 1, OMX_DRIVE_HF_ROLLOFFS_12000, 2);
      char what[128];
      snprintf(what, sizeof what, "D: control %d at %g lands at %g", knob, (double)x, (double)want[knob]);
      ok(memcmp(&s->atom, &e->atom, sizeof s->atom) == 0, what);
      programme(l, r, BLK, (uint32_t)(h * 5 + knob) * BLK);
      omx_drive_instance_run(s, l, r, l, r, BLK);
      ok(all_finite(l, BLK) && all_finite(r, BLK), "D: hostile controls leave the output finite");
      free(s); free(e);
    }
  }
  static const int BAD[] = {-1, 4, 7, 11999, 0x7fffffff};
  /* The oversample ids are factors (1, 2, 4), so its own bad words: 4 is an id there. */
  static const int BAD_FACTOR[] = {0, 3, 8, -4, 0x7fffffff};
  for (int m = 0; m < 5; m++) {
    OmxDriveInstance *s = inst(), *e = inst();
    omx_drive_instance_init(s, g_sr);
    omx_drive_instance_init(e, g_sr);
    /* Both first moved to factor 1, so a bad word must move the factor back to the default. */
    omx_drive_instance_resolve(s, 0, 18.0f, 0.3f, 700.0f, 80.0f, -3.0f, OMX_DRIVE_SOFT, OMX_DRIVE_BAND_FULL, 1, 1,
                               0, 1);
    omx_drive_instance_resolve(e, 0, 18.0f, 0.3f, 700.0f, 80.0f, -3.0f, OMX_DRIVE_SOFT, OMX_DRIVE_BAND_FULL, 1, 1,
                               0, 1);
    omx_drive_instance_resolve(s, 0, 18.0f, 0.3f, 700.0f, 80.0f, -3.0f, BAD[m], BAD[m], BAD[m], BAD[m], BAD[m],
                               BAD_FACTOR[m]);
    omx_drive_instance_resolve(e, 0, 18.0f, 0.3f, 700.0f, 80.0f, -3.0f, OMX_DRIVE_SOFT, OMX_DRIVE_BAND_FULL,
                               OMX_DRIVE_INSTANCE_AUTO_GAIN_DEFAULT, OMX_DRIVE_INSTANCE_STEREO_LINK_DEFAULT,
                               OMX_DRIVE_INSTANCE_HF_ROLLOFF_DEFAULT, OMX_DRIVE_INSTANCE_OVERSAMPLE_DEFAULT);
    ok(memcmp(&s->atom, &e->atom, sizeof s->atom) == 0,
       "D: a choice outside its ids reads as the first (curve, band) or the declared default (the rest)");
    ok(s->atom.os_factor == OMX_DRIVE_OVERSAMPLES_DEFAULT && s->state.factor == s->atom.os_factor,
       "D: an oversample word outside its ids reads as the declared default, the state built for it");
    free(s); free(e);
  }
  drain_violations("D: no contract broken");
}

static void arm_alias_and_independence(void) {
  OmxDriveInstance *a = inst(), *b = inst(), *c = inst(), *alone = inst();
  omx_drive_instance_init(a, g_sr);
  omx_drive_instance_init(b, g_sr);
  omx_drive_instance_init(c, g_sr);
  omx_drive_instance_init(alone, g_sr);
  const Knobs p = {18.0f, 0.3f, 700.0f, 80.0f, 0.0f, OMX_DRIVE_SOFT, OMX_DRIVE_BAND_HIGH, 1, 1, OMX_DRIVE_HF_ROLLOFFS_12000, 2};
  const Knobs q = {33.0f, 0.0f, 2000.0f, 100.0f, -6.0f, OMX_DRIVE_TUBE, OMX_DRIVE_BAND_FULL, 0, 0, 0, 1};
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK], xl[BLK], xr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < NBLK; k++) {
    programme(l, r, BLK, (uint32_t)k * BLK);
    resolve(a, 0, &p);
    resolve(b, 0, &p);
    resolve(alone, 0, &p);
    resolve(c, 0, &q); /* a different one between */
    omx_drive_instance_run(a, l, r, al, ar, BLK);
    programme(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_drive_instance_run(c, cl, cr, cl, cr, BLK);
    memcpy(bl, l, sizeof l); memcpy(br, r, sizeof r);
    omx_drive_instance_run(b, bl, br, bl, br, BLK);
    omx_drive_instance_run(alone, l, r, xl, xr, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
    indep &= same_bytes(al, xl, BLK) && same_bytes(ar, xr, BLK);
  }
  ok(alias, "F: in place is the out-of-place answer");
  ok(indep, "G: an instance between two others leaves them each the instance alone");
  free(a); free(b); free(c); free(alone);
  drain_violations("F/G: no contract broken");
}

static void arm_no_allocation(void) {
  static OmxDriveInstance s;
  float l[BLK], r[BLK];
  Knobs k = DEFAULTS;
  g_allocs = 0;
  g_counting = 1;
  const int ready = omx_drive_instance_init(&s, g_sr);
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    k.amount = b < 6 ? 24.0f : 6.0f;
    k.band_freq = b < 3 ? 400.0f : 3000.0f;
    k.band = b & 3;
    k.hf_rolloff = b % 3; /* off, 12000 Hz, 16000 Hz */
    k.oversample = 1 << (b % 3); /* a factor change every block: the state rebuilt in place */
    resolve(&s, b == 5, &k);
    omx_drive_instance_run(&s, l, r, l, r, BLK);
  }
  g_counting = 0;
  ok(ready, "H: the instance came up");
  ok(g_allocs == 0, "H: init, resolve, a re-engage, a factor change and run allocate nothing");
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

/* ---- O: the oversample control, at the nine RME rates ------------------------------------- */

/* The factors the contract offers (DRIVE_OVERSAMPLES) and the round trip each one costs, in
 * base-rate samples: nothing at 1x; at 2x the up stage's 24 plus the down stage's 48 at twice the
 * rate; at 4x 72. Written as numbers, not read from the oversampler, so the
 * latency the face publishes is checked against the closed form and not against itself. */
static const int FACTORS[3] = {OMX_DRIVE_OVERSAMPLES_1, OMX_DRIVE_OVERSAMPLES_2, OMX_DRIVE_OVERSAMPLES_4};
static const uint32_t FACTOR_LATENCY[3] = {0u, 48u, 72u};

/* One drive under test at g_sr: the face where the console declares the rate, and where it does
 * not (32, 64 and 128 kHz, which the face refuses) the kernel itself on the console's atom, so
 * every factor meets the same closed forms at all nine rates. */
typedef struct {
  int face;
  OmxDriveInstance *s;
  struct omx_drive atom;
  struct omx_drive_state *ks;
} Drv;

static void drv_set(Drv *d, const Knobs *k) {
  if (d->face)
    resolve(d->s, 0, k);
  else
    d->atom = console_atom((uint32_t)g_sr, k);
}

static void drv_open(Drv *d, const Knobs *k) {
  memset(d, 0, sizeof *d);
  d->face = omx_rate_is_declared(g_sr);
  if (d->face) {
    d->s = inst();
    omx_drive_instance_init(d->s, g_sr);
  } else {
    d->ks = calloc(1, sizeof *d->ks);
    omx_drive_state_init(d->ks, k->oversample);
  }
  drv_set(d, k);
}

static void drv_close(Drv *d) {
  free(d->s);
  free(d->ks);
}

static void drv_run(Drv *d, const float *l, const float *r, float *ol, float *or_, uint32_t n) {
  if (d->face) {
    omx_drive_instance_run(d->s, l, r, ol, or_, n);
    return;
  }
  memcpy(ol, l, (size_t)n * sizeof *ol);
  memcpy(or_, r, (size_t)n * sizeof *or_);
  omx_drive_process(ol, or_, n, &d->atom, d->ks);
}

static uint32_t drv_latency(const Drv *d) {
  return d->face ? omx_drive_instance_latency(d->s) : (uint32_t)omx_drive_latency(&d->atom);
}

static int drv_built_for(const Drv *d, int f) {
  return d->face ? d->s->atom.os_factor == f && d->s->state.factor == f : d->ks->factor == f;
}

/* The programme over the whole run, so a delayed copy can be read back from any block. */
#define ORUN (NBLK * BLK)
static float g_xl[ORUN], g_xr[ORUN];

/* out[t] for t in [t0, t0 + n) is x delayed by `lat` from `since`: x[t - lat] where that is at or
 * after `since`, else silence (a state built at `since` holds nothing older). */
static int delayed_dry(const float *out, const float *x, uint32_t t0, uint32_t n, uint32_t lat, uint32_t since) {
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t t = t0 + i;
    const float want = t >= since + lat ? x[t - lat] : 0.0f;
    if (memcmp(&out[i], &want, sizeof want) != 0) return 0;
  }
  return 1;
}

static void arm_oversample(void) {
  char what[160];
  programme(g_xl, g_xr, ORUN, 0);
  /* mix 0: the stage is its own input delayed by its latency, bit for bit (drive.test.c arm 2). */
  Knobs dry = DEFAULTS;
  dry.amount = 24.0f;
  dry.mix = 0.0f;
  /* engaged and wet, every control off its default */
  const Knobs wet = {18.0f, 0.3f, 700.0f, 80.0f, -3.0f, OMX_DRIVE_TUBE, OMX_DRIVE_BAND_TILT, 1, 1, OMX_DRIVE_HF_ROLLOFFS_12000, 4};
  float ol[BLK], or_[BLK], el[BLK], er[BLK];

  for (int fi = 0; fi < 3; fi++) {
    const int f = FACTORS[fi];
    ok((uint32_t)omx_oversampler_latency_for((uint32_t)f) == FACTOR_LATENCY[fi],
       "O: the oversampler's round trip is the closed form at every factor");
    Drv d;
    Knobs k = dry;
    k.oversample = f;
    drv_open(&d, &k);
    snprintf(what, sizeof what, "O: at %dx the latency is %u samples, the state built for %dx", f,
             FACTOR_LATENCY[fi], f);
    ok(drv_latency(&d) == FACTOR_LATENCY[fi] && drv_built_for(&d, f), what);
    int same = 1;
    for (int b = 0; b < NBLK; b++) {
      drv_run(&d, g_xl + b * BLK, g_xr + b * BLK, ol, or_, BLK);
      same &= delayed_dry(ol, g_xl, (uint32_t)b * BLK, BLK, FACTOR_LATENCY[fi], 0u) &&
              delayed_dry(or_, g_xr, (uint32_t)b * BLK, BLK, FACTOR_LATENCY[fi], 0u);
    }
    snprintf(what, sizeof what, "O: at %dx and mix 0 the output is the input %u samples late, bit for bit", f,
             FACTOR_LATENCY[fi]);
    ok(same, what);
    if (d.face) {
      resolve(d.s, 1, &k);
      ok(omx_drive_instance_latency(d.s) == 0u, "O: bypassed, no latency at any factor");
    }
    drv_close(&d);
  }

  /* A factor change mid-run, every ordered pair: from the block boundary on, the drive is a fresh
   * one at the new factor fed the same input (the state rebuilt, nothing of the old factor's
   * history carried), its latency is the new factor's, and the coefficient bank, which runs at the
   * base rate on both sides of the oversampler, is the one already designed. */
  const uint32_t at = (NBLK / 2) * BLK;
  for (int ai = 0; ai < 3; ai++) {
    for (int bi = 0; bi < 3; bi++) {
      if (ai == bi) continue;
      const int fa = FACTORS[ai], fb = FACTORS[bi];
      for (int m = 0; m < 2; m++) {
        Knobs ka = m ? wet : dry, kb = ka;
        ka.oversample = fa;
        kb.oversample = fb;
        Drv d, fresh;
        drv_open(&d, &ka);
        for (int b = 0; b < NBLK / 2; b++) drv_run(&d, g_xl + b * BLK, g_xr + b * BLK, ol, or_, BLK);
        struct omx_drive before;
        memset(&before, 0, sizeof before);
        if (d.face) before = d.s->atom;
        drv_set(&d, &kb);
        drv_open(&fresh, &kb);
        snprintf(what, sizeof what, "O: %dx to %dx mid-run: the latency is %dx's at once", fa, fb, fb);
        ok(drv_latency(&d) == FACTOR_LATENCY[bi], what);
        if (d.face) {
          snprintf(what, sizeof what, "O: %dx to %dx mid-run: resolve rebuilds the state for %dx", fa, fb, fb);
          ok(drv_built_for(&d, fb), what);
          ok(memcmp(before.band_c, d.s->atom.band_c, sizeof before.band_c) == 0 &&
                 memcmp(before.tilt_c, d.s->atom.tilt_c, sizeof before.tilt_c) == 0 &&
                 memcmp(before.tilt_inv_c, d.s->atom.tilt_inv_c, sizeof before.tilt_inv_c) == 0 &&
                 memcmp(before.hf_c, d.s->atom.hf_c, sizeof before.hf_c) == 0 &&
                 memcmp(d.s->atom.band_c, fresh.s->atom.band_c, sizeof before.band_c) == 0 &&
                 memcmp(d.s->atom.hf_c, fresh.s->atom.hf_c, sizeof before.hf_c) == 0,
             "O: a factor change leaves the base-rate coefficient bank as designed");
        }
        int same = 1, closed = 1;
        for (int b = NBLK / 2; b < NBLK; b++) {
          drv_run(&d, g_xl + b * BLK, g_xr + b * BLK, ol, or_, BLK);
          drv_run(&fresh, g_xl + b * BLK, g_xr + b * BLK, el, er, BLK);
          same &= same_bytes(ol, el, BLK) && same_bytes(or_, er, BLK);
          if (!m)
            closed &= delayed_dry(ol, g_xl, (uint32_t)b * BLK, BLK, FACTOR_LATENCY[bi], at) &&
                      delayed_dry(or_, g_xr, (uint32_t)b * BLK, BLK, FACTOR_LATENCY[bi], at);
        }
        ok(drv_built_for(&d, fb), "O: after a factor change the state runs built for the new factor");
        snprintf(what, sizeof what, "O: %dx to %dx mid-run (%s): from the boundary on, a fresh %dx drive, bit for bit",
                 fa, fb, m ? "wet" : "mix 0", fb);
        ok(same, what);
        if (!m) {
          snprintf(what, sizeof what,
                   "O: %dx to %dx mid-run at mix 0: the input %u samples late from the boundary, silence before", fa,
                   fb, FACTOR_LATENCY[bi]);
          ok(closed, what);
        }
        drv_close(&d);
        drv_close(&fresh);
      }
    }
  }
  ok(omx_fx_drain_ledger(g_sr) == 0u, "O: no contract broken (a refused rate's rate-is-declared aside)");
}

int main(void) {
  omx_fx_require_rate_floor();
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = OMX_DECLARED_RATES[ri];
    arm_init();
    arm_bypass();
    arm_is_the_kernel();
    arm_clamps();
    arm_alias_and_independence();
    arm_no_allocation();
  }
  for (int ri = 0; ri < (int)OMX_FX_RME_RATE_COUNT; ri++) {
    g_sr = OMX_FX_RME_RATES[ri];
    arm_oversample();
  }
  printf("fx/drive_instance: arm O at the %d RME rates\n", (int)OMX_FX_RME_RATE_COUNT);
  return finish("drive_instance");
}
