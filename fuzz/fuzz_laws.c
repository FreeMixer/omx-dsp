// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: the scalar laws every control port goes through before a kernel reads it:
 * omx_clampf / omx_clamp_or / omx_unit (omx_param.h), the dB and linear conversions
 * (omx_units.h), the pan, balance and fader laws, the ramp a block applies a changed value
 * through, the wet/dry mix and cross-fade, and the one-pole constructors (omx_onepole.h).
 *
 * Input: a sequence of calls, each a u8 selector and the words its call reads (raw floats: any
 * value a host, an atom or a knob can carry, NaN and the infinities included, wherever the
 * function's own contract takes any value, and finite or in-travel words where it states a PRE).
 * The sequence ends with the input.
 *
 * Properties, each an abort():
 *   - a clamp lands inside its travel, whatever the word (NaN included), and a word already inside
 *     is returned unchanged; omx_clamp_or turns a non-finite word into the declared default;
 *   - dB to linear is never negative and, inside +-150 dB, finite and positive, and linear to dB
 *     brings it back to 0.01 dB; linear to dB never leaves its floor and never goes non-finite;
 *   - the pan law is constant power, cos^2 + sin^2 = 1 (1e-5), on [-1, 1]; the balance law
 *     never boosts: both legs in [0, 1], at least one of them 1, any pan;
 *   - the fader law's coefficient is finite, never negative, and zero when the strip or the DCA
 *     is muted;
 *   - a ramp over n samples starts at the applied value (0 for a non-finite one), reaches no
 *     sample outside the span of its start and target, and ends on the target; nothing in it is
 *     non-finite whatever the caller held;
 *   - the wet/dry mix of finite samples lies between the dry and the wet one, and the
 *     cross-fade's every sample lies between the outgoing and the incoming body's;
 *   - the one-pole constructors give a pole in [0, 1) for any finite time or corner (the
 *     fix of omx-dsp#250), a rescaled pole stays in [0, 1], and one step of the filter lies between
 *     the input and the state.
 */
#define OMX_CONTRACT_STORAGE 1
#include <float.h>

#include <omxdsp/omx_balance_law.h>
#include <omxdsp/omx_fader_law.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_ramp.h>
#include <omxdsp/omx_units.h>
#include <omxdsp/omx_wetdry.h>

#include "fuzz.h"

#define CALLS_MAX 64
#define BLOCK_MAX 64u

static int is_fin(float x) { return x - x == 0.0f; }

static float fin_or(float x, float d) { return is_fin(x) ? x : d; }

static void call_clamp(struct fuzz_in *in) {
  const float v = fuzz_f32(in);
  float lo = fin_or(fuzz_f32(in), -1.0f), hi = fin_or(fuzz_f32(in), 1.0f);
  if (lo > hi) {
    const float t = lo;
    lo = hi;
    hi = t;
  }
  const float c = omx_clampf(v, lo, hi);
  FUZZ_CHECK(c >= lo && c <= hi, "omx_clampf(%g, %g, %g) = %g is outside its travel", (double)v, (double)lo, (double)hi,
             (double)c);
  if (v >= lo && v <= hi) FUZZ_CHECK(c == v, "omx_clampf moved %g, which is inside [%g, %g]", (double)v, (double)lo, (double)hi);
  float d = fin_or(fuzz_f32(in), lo);
  d = d < lo ? lo : d > hi ? hi : d;
  const float o = omx_clamp_or(v, lo, hi, d);
  FUZZ_CHECK(o >= lo && o <= hi, "omx_clamp_or(%g, %g, %g, %g) = %g is outside its travel", (double)v, (double)lo,
             (double)hi, (double)d, (double)o);
  if (!is_fin(v)) FUZZ_CHECK(o == d, "omx_clamp_or(%g) = %g is not the default %g", (double)v, (double)o, (double)d);
  const float u = omx_unit(v);
  FUZZ_CHECK(u >= 0.0f && u <= 1.0f, "omx_unit(%g) = %g", (double)v, (double)u);
}

static void call_units(struct fuzz_in *in) {
  const float db = fuzz_f32(in);
  const float g = omx_db_to_lin(fin_or(db, 0.0f));
  FUZZ_CHECK(!(g < 0.0f), "omx_db_to_lin(%g) = %g is negative", (double)db, (double)g);
  const float dbc = fin_or(db, 0.0f);
  if (dbc >= -150.0f && dbc <= 150.0f) {
    FUZZ_CHECK(is_fin(g) && g > 0.0f, "omx_db_to_lin(%g) = %g is not finite and positive", (double)dbc, (double)g);
    const float back = omx_lin_to_db(g);
    FUZZ_CHECK(fabsf(back - dbc) <= 0.01f, "dB %g -> linear %g -> dB %g", (double)dbc, (double)g, (double)back);
    const float gp = omx_db_to_lin_poly(dbc);
    FUZZ_CHECK(is_fin(gp) && gp > 0.0f, "omx_db_to_lin_poly(%g) = %g", (double)dbc, (double)gp);
  }
  const float lin = fuzz_f32(in);
  if (is_fin(lin)) { /* +Inf is +Inf dB: the floor is the law for what is small, not for what is not finite */
    const float l = omx_lin_to_db(lin);
    FUZZ_CHECK(is_fin(l) && l >= OMX_LIN_TO_DB_FLOOR_DB - 0.001f, "omx_lin_to_db(%g) = %g", (double)lin, (double)l);
  }
  if (is_fin(lin)) {
    const float p = omx_lin_to_db_poly(lin);
    FUZZ_CHECK(is_fin(p), "omx_lin_to_db_poly(%g) = %g", (double)lin, (double)p);
  }
}

static void call_pan_fader(struct fuzz_in *in) {
  const float pan = fuzz_range(in, -1.0f, 1.0f);
  const float gl = omx_pan_law_gain(pan, OMX_PAN_LEG_L), gr = omx_pan_law_gain(pan, OMX_PAN_LEG_R);
  FUZZ_CHECK(fabsf(gl * gl + gr * gr - 1.0f) <= 1e-5f, "the pan law at %g: %g^2 + %g^2 is not 1", (double)pan,
             (double)gl, (double)gr);
  const float any = fuzz_f32(in);
  float bl, br;
  omx_balance_law(any, &bl, &br);
  FUZZ_CHECK(bl >= 0.0f && bl <= 1.0f && br >= 0.0f && br <= 1.0f && (bl == 1.0f || br == 1.0f),
             "the balance law at %g gives (%g, %g)", (double)any, (double)bl, (double)br);
  struct omx_fader_law_params p;
  p.fader_db = fuzz_range(in, -200.0f, 60.0f);
  p.pan = pan;
  p.pan_leg = (int)fuzz_below(in, 3);
  p.send_lin = fabsf(fin_or(fuzz_f32(in), 1.0f));
  if (p.send_lin > 1e6f) p.send_lin = 1e6f;
  p.mute = fuzz_u8(in) & 1;
  p.dca_lin = fabsf(fin_or(fuzz_f32(in), 1.0f));
  if (p.dca_lin > 1e6f) p.dca_lin = 1e6f;
  p.dca_mute = fuzz_u8(in) & 1;
  const float g = omx_fader_law_coeff(&p);
  FUZZ_CHECK(is_fin(g) && g >= 0.0f, "the fader law gives %g", (double)g);
  if (p.mute || p.dca_mute) FUZZ_CHECK(g == 0.0f, "a muted entry's coefficient is %g", (double)g);
}

static void call_ramp(struct fuzz_in *in) {
  /* Any word, as far as a ramp's own arithmetic goes: a non-finite one reads as 0; past 1e30 the
   * difference of the two ends, and cur + step * i, leave a float, which no gain or level reaches. */
  float cur = fuzz_f32(in), tgt = fuzz_f32(in);
  if (is_fin(cur) && fabsf(cur) > 1e30f) cur = copysignf(1e30f, cur);
  if (is_fin(tgt) && fabsf(tgt) > 1e30f) tgt = copysignf(1e30f, tgt);
  const uint32_t n = 1u + fuzz_below(in, BLOCK_MAX);
  float held = cur;
  const struct omx_ramp r = omx_ramp_begin(&held, tgt, n);
  const float start = is_fin(cur) ? cur : 0.0f, end = is_fin(tgt) ? tgt : 0.0f;
  const float lo = fminf(start, end), hi = fmaxf(start, end);
  const float slack = 1e-5f * fmaxf(fabsf(lo), fabsf(hi)) + FLT_MIN; /* the rounding of cur + step*i, a denormal one included */
  FUZZ_CHECK(omx_ramp_at(r, 0) == start, "a ramp from %g does not start there (%g)", (double)cur, (double)omx_ramp_at(r, 0));
  for (uint32_t i = 0; i < n; i++) {
    const float x = omx_ramp_at(r, i);
    FUZZ_CHECK(is_fin(x) && x >= lo - slack && x <= hi + slack, "ramp %g -> %g over %u: sample %u is %g", (double)cur,
               (double)tgt, (unsigned)n, (unsigned)i, (double)x);
  }
  omx_ramp_end(r, &held);
  FUZZ_CHECK(held == end, "a ramp ends on %g, not %g", (double)held, (double)end);
}

static void call_wetdry(struct fuzz_in *in) {
  const float mix = fuzz_range(in, 0.0f, 1.0f), dry = 1.0f - mix;
  const float xl = fuzz_range(in, -8.0f, 8.0f), xr = fuzz_range(in, -8.0f, 8.0f);
  const float wl = fuzz_range(in, -8.0f, 8.0f), wr = fuzz_range(in, -8.0f, 8.0f);
  float l, r;
  omx_wetdry_mix(&l, &r, xl, xr, wl, wr, dry, mix);
  const float tol = 1e-5f;
  FUZZ_CHECK(l >= fminf(xl, wl) - tol && l <= fmaxf(xl, wl) + tol && r >= fminf(xr, wr) - tol &&
                 r <= fmaxf(xr, wr) + tol,
             "the wet/dry mix at %g is outside its two inputs", (double)mix);
  float a[BLOCK_MAX], b[BLOCK_MAX], d[BLOCK_MAX];
  const uint32_t n = 1u + fuzz_below(in, BLOCK_MAX);
  for (uint32_t i = 0; i < n; i++) {
    a[i] = fuzz_range(in, -4.0f, 4.0f);
    b[i] = fuzz_range(in, -4.0f, 4.0f);
  }
  omx_wetdry_xfade_block(d, a, b, n);
  for (uint32_t i = 0; i < n; i++)
    FUZZ_CHECK(d[i] >= fminf(a[i], b[i]) - tol && d[i] <= fmaxf(a[i], b[i]) + tol,
               "the cross-fade's sample %u (%g) is outside %g and %g", (unsigned)i, (double)d[i], (double)a[i], (double)b[i]);
}

static void call_onepole(struct fuzz_in *in) {
  const float sr = OMX_DECLARED_RATES[fuzz_below(in, OMX_DECLARED_RATE_COUNT)];
  const float ms = fin_or(fuzz_f32(in), 0.0f), hz = fin_or(fuzz_f32(in), 0.0f);
  const float pt = omx_pole_from_time_ms(ms, sr), pc = omx_pole_from_cutoff_hz(hz, sr);
  FUZZ_CHECK(pt >= 0.0f && pt < 1.0f, "omx_pole_from_time_ms(%g, %g) = %g", (double)ms, (double)sr, (double)pt);
  FUZZ_CHECK(pc >= 0.0f && pc < 1.0f, "omx_pole_from_cutoff_hz(%g, %g) = %g", (double)hz, (double)sr, (double)pc);
  const float p = fuzz_range(in, 0.0f, 1.0f), ref = fuzz_f32(in), run = fuzz_f32(in);
  const float q = omx_pole_rescale(p, ref, run);
  FUZZ_CHECK(q >= 0.0f && q <= 1.0f, "omx_pole_rescale(%g, %g, %g) = %g", (double)p, (double)ref, (double)run, (double)q);
  float s = fuzz_range(in, -4.0f, 4.0f);
  const float x = fuzz_range(in, -4.0f, 4.0f), s0 = s;
  const float y = omx_onepole(&s, x, pt);
  FUZZ_CHECK(y >= fminf(x, s0) - 1e-5f && y <= fmaxf(x, s0) + 1e-5f && y == s,
             "a one-pole step from %g toward %g at pole %g gave %g", (double)s0, (double)x, (double)pt, (double)y);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  struct fuzz_in in = fuzz_in(data, size);
  fuzz_ledger_reset();
  for (int i = 0; i < CALLS_MAX && in.n > 0; i++) {
    switch (fuzz_below(&in, 6)) {
      case 0: call_clamp(&in); break;
      case 1: call_units(&in); break;
      case 2: call_pan_fader(&in); break;
      case 3: call_ramp(&in); break;
      case 4: call_wetdry(&in); break;
      default: call_onepole(&in); break;
    }
    fuzz_ledger_clean("laws");
  }
  return 0;
}
