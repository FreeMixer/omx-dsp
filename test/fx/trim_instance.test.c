// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * trim_instance.test.c — the input trim's instance core (omx_trim_instance.h), every arm at every
 * rate in OMX_DECLARED_RATES:
 *   A  a refused init (no rate, a non-finite rate) is the identity; the latency is zero;
 *   B  bypassed is the identity byte for byte, in place and out of place;
 *   C  the trim face then the eq face with every band off ARE omx-strip's input stage: bit-identical,
 *      block by block over test/fx/strip_input_script.h, to the arithmetic of omx-plugins
 *      plugins/omx-strip/omx_strip.h (main bc29ac4, omx_strip_resolve's trim and filter words,
 *      omx_strip_stage's OMX_STRIP_INPUT case), host bypass spans included; the reference below is
 *      that code with the strip's other stages and parameter table taken out, and
 *      tools/strip-input-identity.sh holds the golden digests to the plugin's own file;
 *   D  a trim word at every hostile host value lands inside TRIM_RANGE (non-finite at its 0 dB
 *      default), and the output is finite;
 *   E  a fresh instance is unity: a first block at 0 dB is the identity, a first block at +6 dB
 *      ramps from unity and the next block is the plain product;
 *   F  in == out (an aliased port) is the out-of-place answer;
 *   G  an instance run between two others leaves each the instance alone;
 *   H  nothing allocates (the allocators are wrapped and counted).
 *   make test-fx
 */
#include <math.h>
#include <stdlib.h>

#define OMX_CONTRACT_STORAGE 1

#include <omxdsp/fx/omx_eq_instance.h>
#include <omxdsp/fx/omx_trim_instance.h>

#include "instance_harness.h"
#include "strip_input_script.h"

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

#define BLK STRIP_INPUT_BLOCK
#define NBLK 128

/* ---- the reference: omx_strip.h's input stage (omx-plugins main bc29ac4) ---------------------- */

typedef struct {
  float trim_cur, trim_tgt;
  struct omx_eq_lv2 filter[2];
  int bypass;
} StripInput;

static void strip_input_init(StripInput *s, float sr) {
  memset(s, 0, sizeof *s);
  for (int c = 0; c < 2; c++) omx_eq_lv2_init(&s->filter[c], sr);
  s->trim_cur = s->trim_tgt = 1.0f;
}

static void strip_input_resolve(StripInput *s, struct strip_input_words w) {
  const float v[6] = {(float)w.hpf_on, w.hpf_freq, (float)w.hpf_slope, (float)w.lpf_on, w.lpf_freq, (float)w.lpf_slope};
  s->bypass = w.bypass;
  s->trim_tgt = omx_db_to_lin(omx_clampf(w.trim_db, -24.0f, 24.0f)); /* OMX_STRIP_PARAM_TRIM_DB_MIN/MAX */
  for (int c = 0; c < 2; c++) {
    const struct omx_eq_lv2_controls f = {.hpf_on = &v[0], .hpf_freq = &v[1], .hpf_slope = &v[2],
                                          .lpf_on = &v[3], .lpf_freq = &v[4], .lpf_slope = &v[5]};
    omx_eq_lv2_set_controls(&s->filter[c], &f);
  }
}

static void strip_input_run(StripInput *s, float *l, float *r, uint32_t n) {
  if (s->bypass) return;
  const struct omx_ramp g = omx_ramp_begin(&s->trim_cur, s->trim_tgt, n);
  for (uint32_t i = 0; i < n; i++) l[i] *= omx_ramp_at(g, i), r[i] *= omx_ramp_at(g, i);
  omx_ramp_end(g, &s->trim_cur);
  omx_eq_lv2_run(&s->filter[0], l, l, n);
  omx_eq_lv2_run(&s->filter[1], r, r, n);
}

/* ---- the faces ------------------------------------------------------------------------------- */

typedef struct {
  OmxTrimInstance trim;
  OmxEqInstance eq;
} Faces;

static int faces_init(Faces *f, float sr) {
  return omx_trim_instance_init(&f->trim, sr) & omx_eq_instance_init(&f->eq, sr);
}

static void faces_resolve(Faces *f, struct strip_input_words w) {
  int type[OMX_EQ_INSTANCE_BANDS], on[OMX_EQ_INSTANCE_BANDS];
  float freq[OMX_EQ_INSTANCE_BANDS], gain[OMX_EQ_INSTANCE_BANDS], q[OMX_EQ_INSTANCE_BANDS];
  for (int i = 0; i < OMX_EQ_INSTANCE_BANDS; i++)
    type[i] = (int)OMX_EQ_BAND_TYPES_DEFAULT, freq[i] = omx_eq_lv2_band_freq_default((uint32_t)i), gain[i] = 0.0f,
    q[i] = OMX_EQ_LV2_BAND_Q_DEFAULT, on[i] = 0;
  omx_trim_instance_resolve(&f->trim, w.bypass, w.trim_db);
  omx_eq_instance_resolve(&f->eq, w.bypass, w.hpf_on, w.hpf_freq, w.hpf_slope, w.lpf_on, w.lpf_freq, w.lpf_slope, type,
                          freq, gain, q, on);
}

static void faces_run(Faces *f, const float *il, const float *ir, float *ol, float *or_, uint32_t n) {
  omx_trim_instance_run(&f->trim, il, ir, ol, or_, n);
  omx_eq_instance_run(&f->eq, ol, or_, ol, or_, n);
}

/* ---- the arms -------------------------------------------------------------------------------- */

static void arm_refused(void) {
  OmxTrimInstance s;
  ok(omx_trim_instance_init(&s, 0.0f) == 0, "A: rate 0 refused");
  ok(omx_trim_instance_init(&s, NAN) == 0, "A: NaN rate refused");
  ok(omx_trim_instance_init(&s, INFINITY) == 0, "A: infinite rate refused");
  ok(omx_trim_instance_init(&s, -g_sr) == 0, "A: negative rate refused");
  omx_trim_instance_resolve(&s, 0, 12.0f);
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  programme(l, r, BLK, 0);
  omx_trim_instance_run(&s, l, r, ol, or_, BLK);
  ok(same_bytes(l, ol, BLK) && same_bytes(r, or_, BLK), "A: a refused instance is the identity");
  ok(omx_trim_instance_init(&s, g_sr) == 1, "A: the declared rate is accepted");
  ok(omx_trim_instance_latency(&s) == 0u, "A: the published latency is zero frames");
  drain_violations("A: no contract broken");
}

static void arm_bypass(void) {
  OmxTrimInstance s;
  omx_trim_instance_init(&s, g_sr);
  float l[BLK], r[BLK], ol[BLK], or_[BLK], l0[BLK], r0[BLK];
  int same = 1;
  for (int b = 0; b < 12; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    memcpy(l0, l, sizeof l), memcpy(r0, r, sizeof r);
    omx_trim_instance_resolve(&s, 1, -9.0f + (float)b);
    omx_trim_instance_run(&s, l, r, ol, or_, BLK);
    same &= same_bytes(ol, l0, BLK) && same_bytes(or_, r0, BLK);
    omx_trim_instance_run(&s, l, r, l, r, BLK);
    same &= same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK);
  }
  ok(same, "B: bypassed is the identity, in place and out of place");
  drain_violations("B: no contract broken");
}

static void arm_is_the_strip_input(void) {
  StripInput ref;
  Faces f;
  strip_input_init(&ref, g_sr);
  ok(faces_init(&f, g_sr) == 1, "C: both faces accept the declared rate");
  float l[BLK], r[BLK], ol[BLK], or_[BLK];
  int same = 1, moved = 0;
  for (int b = 0; b < NBLK; b++) {
    const struct strip_input_words w = strip_input_words_at(b);
    programme(l, r, BLK, (uint32_t)b * BLK);
    strip_input_resolve(&ref, w);
    faces_resolve(&f, w);
    faces_run(&f, l, r, ol, or_, BLK);
    float l0[BLK];
    memcpy(l0, l, sizeof l);
    strip_input_run(&ref, l, r, BLK);
    same &= same_bytes(ol, l, BLK) && same_bytes(or_, r, BLK);
    moved |= !same_bytes(l0, l, BLK);
  }
  ok(moved, "C: the script moves the signal (a reference that does nothing proves nothing)");
  ok(same, "C: trim face then eq face (bands off) are omx_strip.h's input stage, bit for bit");
  drain_violations("C: no contract broken");
}

static void arm_clamps(void) {
  int inside = 1, finite = 1;
  for (int h = 0; h < HOSTILE_COUNT; h++) {
    OmxTrimInstance s;
    omx_trim_instance_init(&s, g_sr);
    omx_trim_instance_resolve(&s, 0, HOSTILE[h]);
    const float want = HOSTILE[h] - HOSTILE[h] == 0.0f ? omx_clampf(HOSTILE[h], -24.0f, 24.0f) : 0.0f;
    inside &= s.tgt == omx_db_to_lin(want);
    float l[BLK], r[BLK];
    for (int b = 0; b < 3; b++) {
      programme(l, r, BLK, (uint32_t)b * BLK);
      omx_trim_instance_run(&s, l, r, l, r, BLK);
      finite &= all_finite(l, BLK) && all_finite(r, BLK);
    }
  }
  ok(inside, "D: every hostile trim word lands inside TRIM_RANGE, a non-finite one at 0 dB");
  ok(finite, "D: the output stays finite");
  ok((float)OMX_TRIM_RANGE_MIN == -24.0f && (float)OMX_TRIM_RANGE_MAX == 24.0f && (float)OMX_TRIM_RANGE_DEFAULT == 0.0f,
     "D: TRIM_RANGE is the travel omx-strip's trim declares (-24..+24 dB, 0 dB)");
  drain_violations("D: no contract broken");
}

static void arm_fresh_is_unity(void) {
  OmxTrimInstance s;
  omx_trim_instance_init(&s, g_sr);
  float l[BLK], r[BLK], l0[BLK], r0[BLK];
  programme(l, r, BLK, 0);
  memcpy(l0, l, sizeof l), memcpy(r0, r, sizeof r);
  omx_trim_instance_resolve(&s, 0, 0.0f);
  omx_trim_instance_run(&s, l, r, l, r, BLK);
  ok(same_bytes(l, l0, BLK) && same_bytes(r, r0, BLK), "E: a fresh instance at 0 dB is the identity");
  omx_trim_instance_resolve(&s, 0, 6.0f);
  omx_trim_instance_run(&s, l, r, l, r, BLK);
  ok(l[0] == l0[0] && r[0] == r0[0], "E: a move ramps from the gain the last block ended on");
  programme(l, r, BLK, BLK);
  memcpy(l0, l, sizeof l), memcpy(r0, r, sizeof r);
  omx_trim_instance_run(&s, l, r, l, r, BLK);
  const float g = omx_db_to_lin(6.0f);
  int product = 1;
  for (int i = 0; i < BLK; i++) product &= l[i] == l0[i] * g && r[i] == r0[i] * g;
  ok(product, "E: once arrived, the block is the plain product with the resolved gain");
  drain_violations("E: no contract broken");
}

static void arm_alias_and_independence(void) {
  OmxTrimInstance a, b, c, alone;
  omx_trim_instance_init(&a, g_sr), omx_trim_instance_init(&b, g_sr);
  omx_trim_instance_init(&c, g_sr), omx_trim_instance_init(&alone, g_sr);
  float l[BLK], r[BLK], al[BLK], ar[BLK], bl[BLK], br[BLK], cl[BLK], cr[BLK], xl[BLK], xr[BLK];
  int alias = 1, indep = 1;
  for (int k = 0; k < 12; k++) {
    const float db = (float)(k % 5) * 4.0f - 8.0f;
    programme(l, r, BLK, (uint32_t)k * BLK);
    omx_trim_instance_resolve(&a, 0, db), omx_trim_instance_resolve(&b, 0, db);
    omx_trim_instance_resolve(&alone, 0, db), omx_trim_instance_resolve(&c, 0, 20.0f - db);
    omx_trim_instance_run(&a, l, r, al, ar, BLK);
    programme(cl, cr, BLK, (uint32_t)(k + 77) * BLK);
    omx_trim_instance_run(&c, cl, cr, cl, cr, BLK); /* a different one between */
    memcpy(bl, l, sizeof l), memcpy(br, r, sizeof r);
    omx_trim_instance_run(&b, bl, br, bl, br, BLK);
    omx_trim_instance_run(&alone, l, r, xl, xr, BLK);
    alias &= same_bytes(al, bl, BLK) && same_bytes(ar, br, BLK);
    indep &= same_bytes(al, xl, BLK) && same_bytes(ar, xr, BLK);
  }
  ok(alias, "F: in place is the out-of-place answer");
  ok(indep, "G: an instance between two others leaves them each the instance alone");
  drain_violations("F/G: no contract broken");
}

static void arm_no_allocation(void) {
  g_allocs = 0;
  g_counting = 1;
  Faces f;
  faces_init(&f, g_sr);
  float l[BLK], r[BLK];
  for (int b = 0; b < NBLK; b++) {
    programme(l, r, BLK, (uint32_t)b * BLK);
    faces_resolve(&f, strip_input_words_at(b));
    faces_run(&f, l, r, l, r, BLK);
  }
  g_counting = 0;
  ok(g_allocs == 0, "H: init, resolve and run allocate nothing");
  drain_violations("H: no contract broken");
}

int main(void) {
  omx_fx_require_rate_floor();
  for (int ri = 0; ri < (int)OMX_DECLARED_RATE_COUNT; ri++) {
    g_sr = OMX_DECLARED_RATES[ri];
    arm_refused();
    arm_bypass();
    arm_is_the_strip_input();
    arm_clamps();
    arm_fresh_is_unity();
    arm_alias_and_independence();
    arm_no_allocation();
  }
  return finish("trim_instance");
}
