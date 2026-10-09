// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * drive_instance.test.c — the drive instance face (omx_drive_instance.h), every arm at every rate
 * in OMX_DECLARED_RATES:
 *   A  a rate the console does not declare is refused and the face is the identity; init resolves
 *      the contract's defaults: engaged, the face's factor, the latency the kernel declares;
 *   B  bypassed is the identity byte for byte, in place and out of place, with no latency;
 *   C  engaged, the instance IS the kernel: bit-identical to omx_drive_process on the atom the
 *      console's resolve_fx_drive builds from the same controls and the same designed bank, for
 *      every curve and every band;
 *   D  every travel, at every hostile host value, lands inside its declared travel (a non-finite
 *      word at the declared default); a choice outside its ids reads as the first; the output is
 *      finite;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves them each the instance alone (no shared state);
 *   H  init, resolve (re-engage included) and run allocate nothing: malloc, calloc, realloc and
 *      free are wrapped at link time and counted across the instance's whole life.
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
  int curve, band;
} Knobs;

static const Knobs DEFAULTS = {OMX_DRIVE_INSTANCE_AMOUNT_DEFAULT, OMX_DRIVE_INSTANCE_CHARACTER_DEFAULT,
                               OMX_DRIVE_INSTANCE_BAND_FREQ_DEFAULT, OMX_DRIVE_INSTANCE_MIX_DEFAULT,
                               OMX_DRIVE_INSTANCE_TRIM_DEFAULT, OMX_DRIVE_INSTANCE_CURVE_DEFAULT,
                               OMX_DRIVE_INSTANCE_BAND_DEFAULT};

static void resolve(OmxDriveInstance *s, int bypass, const Knobs *k) {
  omx_drive_instance_resolve(s, bypass, k->amount, k->character, k->band_freq, k->mix, k->trim, k->curve, k->band);
}

/* Heap-held: the state's inline oversampler histories make an instance large. */
static OmxDriveInstance *inst(void) { return calloc(1, sizeof(OmxDriveInstance)); }

/* The console's resolve_fx_drive, from the same units and the same designed bank, at the face's
 * come-up values for what the contract does not declare, for arm C. */
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
  o.auto_gain = 1;
  o.stereo_link = 1;
  o.hf_on = 0;
  o.os_factor = OMX_DRIVE_INSTANCE_FACTOR;
  omx_drive_design_bank(&o, k->band_freq, 20000.0f, rate);
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
  ok(s->atom.os_factor == OMX_DRIVE_INSTANCE_FACTOR && s->state.factor == s->atom.os_factor,
     "A: the factor is the face's, the state built for it");
  ok(omx_drive_instance_latency(s) == (uint32_t)omx_drive_latency(&s->atom) && omx_drive_instance_latency(s) > 0u,
     "A: the latency is the kernel's");
  ok(s->atom.mix == 0.01f * OMX_DRIVE_INSTANCE_MIX_DEFAULT &&
         s->atom.drive_lin == omx_db_to_lin(OMX_DRIVE_INSTANCE_AMOUNT_DEFAULT) &&
         s->atom.curve == OMX_DRIVE_SOFT && s->atom.band == OMX_DRIVE_BAND_FULL,
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
      {0.0f, 0.0f, 2000.0f, 100.0f, 0.0f, OMX_DRIVE_SOFT, OMX_DRIVE_BAND_FULL},
      {30.0f, 0.6f, 900.0f, 70.0f, -6.0f, OMX_DRIVE_EXCITER, OMX_DRIVE_BAND_TILT},
      {12.0f, -1.0f, 5000.0f, 100.0f, 0.0f, OMX_DRIVE_TAPE, OMX_DRIVE_BAND_LOW},
      {20.0f, 1.0f, 300.0f, 45.0f, 6.0f, OMX_DRIVE_TUBE, OMX_DRIVE_BAND_HIGH},
      {36.0f, -0.4f, 20.0f, 100.0f, -24.0f, OMX_DRIVE_SOFT, OMX_DRIVE_BAND_TILT},
      {6.0f, 0.2f, 20000.0f, 10.0f, 12.0f, OMX_DRIVE_TAPE, OMX_DRIVE_BAND_FULL}};
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
      omx_drive_instance_resolve(s, 0, in[0], in[1], in[2], in[3], in[4], OMX_DRIVE_TUBE, OMX_DRIVE_BAND_TILT);
      omx_drive_instance_resolve(e, 0, want[0], want[1], want[2], want[3], want[4], OMX_DRIVE_TUBE,
                                 OMX_DRIVE_BAND_TILT);
      char what[128];
      snprintf(what, sizeof what, "D: control %d at %g lands at %g", knob, (double)x, (double)want[knob]);
      ok(memcmp(&s->atom, &e->atom, sizeof s->atom) == 0, what);
      programme(l, r, BLK, (uint32_t)(h * 5 + knob) * BLK);
      omx_drive_instance_run(s, l, r, l, r, BLK);
      ok(all_finite(l, BLK) && all_finite(r, BLK), "D: hostile controls leave the output finite");
      free(s); free(e);
    }
  }
  static const int BAD[] = {-1, 4, 7, 0x7fffffff};
  for (int m = 0; m < 4; m++) {
    OmxDriveInstance *s = inst(), *e = inst();
    omx_drive_instance_init(s, g_sr);
    omx_drive_instance_init(e, g_sr);
    omx_drive_instance_resolve(s, 0, 18.0f, 0.3f, 700.0f, 80.0f, -3.0f, BAD[m], BAD[m]);
    omx_drive_instance_resolve(e, 0, 18.0f, 0.3f, 700.0f, 80.0f, -3.0f, OMX_DRIVE_SOFT, OMX_DRIVE_BAND_FULL);
    ok(memcmp(&s->atom, &e->atom, sizeof s->atom) == 0, "D: a curve or band outside its ids reads as the first");
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
  const Knobs p = {18.0f, 0.3f, 700.0f, 80.0f, 0.0f, OMX_DRIVE_SOFT, OMX_DRIVE_BAND_HIGH};
  const Knobs q = {33.0f, 0.0f, 2000.0f, 100.0f, -6.0f, OMX_DRIVE_TUBE, OMX_DRIVE_BAND_FULL};
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
    resolve(&s, b == 5, &k);
    omx_drive_instance_run(&s, l, r, l, r, BLK);
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
    arm_init();
    arm_bypass();
    arm_is_the_kernel();
    arm_clamps();
    arm_alias_and_independence();
    arm_no_allocation();
  }
  return finish("drive_instance");
}
