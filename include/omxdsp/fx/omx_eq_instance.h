/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_eq_instance.h — the console's channel EQ as a plugin instance: ONE shim over the native EQ
 * stage (`OMX_STAGE_EQ`, mix_lane.h), with the band count a COMPILE-TIME constant, and no format
 * in it.
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/eq_lv2.h (integration/waves-2026-10
 * eb13725ea), the strip EQ anchor of the instance headers (omx_delay_instance.h's shape): the
 * includes now name the library's own headers (`omx_biquad_cascade` is omx_biquad.h's, the travels
 * omx_contract_limits.h's), and the control-word processing eq_lv2.c's run() did is here as
 * {@link omx_eq_lv2_set_controls}, so a shell carries ports across its host boundary and nothing
 * more. The `omx_eq_lv2_*` names are kept, as omx_delay_instance.h keeps `omx_delay_lv2_*`.
 *
 * Ruling (ops briefs/2026-09-23-lane-omx-lv2-pack-scoping.md, "EQ RULED"): LV2 has no variable
 * port count, so the EQ ships as THREE bundles — omx-eq8, omx-eq16, omx-eq32 — from this one
 * file, `-DOMX_EQ_LV2_BANDS=8|16|32` at the three `cc` lines in package.json. There are no three
 * copies of the code; there are three numbers.
 *
 * ## What it mirrors, exactly
 *
 * On the desk the EQ is not a struct of its own: `eqCoeffs` (core/src/eq.ts) turns the strip's
 * `EqState` into an ordered coefficient bank and mix_lane.h runs `omx_biquad_cascade` over it
 * with a per-leg state whose slot i belongs to bank entry i. This core rebuilds that bank the
 * same way, from the same model, in the same order:
 *
 *   - N parametric BANDS, each {type, freq, gain, Q, on} — the `EqBand` quintet, with the type
 *     enum in `EQ_BAND_TYPES` order (bell, lowShelf, highShelf, notch, allpass1, allpass2) and the ranges of
 *     `EQ_FREQ_RANGE` / `EQ_GAIN_RANGE` / `EQ_Q_RANGE` (a notch's Q reaches `EQ_NOTCH_Q_RANGE`,
 *     as `clampEqQFor` allows). A band that is `on: false` is NOT in the bank (the desk drops
 *     it and the slots close up, exactly as here); a band that is on but IDENTITY (a
 *     gain-carrying shape at exactly 0 dB, `eqBandIsIdentity`) is in the bank at its position
 *     and PARKED (`enabled = 0`) — the cascade skips it, bit for bit as if it never existed.
 *   - then the HPF and the LPF — `PassFilter` {on, freq, slope 12|24}, one or two Butterworth
 *     sections each (`butterworthSlopeQs`), always enabled when on, appended after the bands.
 *   - the whole-EQ `on`: off means an EMPTY bank (`eq_on = 0` on the desk), a wire.
 *
 * The section design is mix_eq_design.h, the C twin of `rbjSection`; the applier is
 * mix_dsp.h's `omx_biquad_cascade`, the very function the desk runs — this file adds a bank
 * builder and nothing that touches a sample. eq_lv2.test.c holds the oracle: the audio out of
 * this core is BIT-IDENTICAL to the native lane (`omx_lane_stages`, OMX_STAGE_EQ) over the same
 * bank at 192 kHz, for each of the three band counts.
 *
 * ## The one place the shim is wider than a strip
 *
 * The desk's cascade caps at `OMX_EQ_MAX_BANDS` (24) sections per strip; omx-eq32 carries up to
 * 32 + 4 pass sections. `omx_biquad_cascade` is band-OUTER, so running it over the bank in
 * chunks of at most 24 sections is the same arithmetic in the same order as one call would be —
 * the chunking is a loop around the applier, not a second applier, and the oracle proves it
 * against a chunk-free reference.
 *
 * ## Where the maths runs
 *
 * A section is redesigned only when one of its parameters MOVED since the last design (a
 * dragged knob redesigns one section per cycle; a static plugin redesigns nothing). The design
 * is a handful of `exp`/`cos`/`sqrt` — pure, allocation-free, no lock, no I/O — which is what
 * `lv2:hardRTCapable` asks for. The desk designs on its control thread because it has one; a
 * plugin's control words arrive on its ports.
 */
#ifndef OMX_EQ_LV2_H
#define OMX_EQ_LV2_H

#include <string.h>

#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_contract_limits.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_param.h>

#ifndef OMX_EQ_LV2_BANDS
#define OMX_EQ_LV2_BANDS 8
#endif

/** The band type enum, in `EQ_BAND_TYPES` order: the integer a host sees on the type port. */
enum omx_eq_lv2_type {
  OMX_EQ_LV2_BELL = 0,
  OMX_EQ_LV2_LOWSHELF = 1,
  OMX_EQ_LV2_HIGHSHELF = 2,
  OMX_EQ_LV2_NOTCH = 3,
  OMX_EQ_LV2_ALLPASS1 = 4,
  OMX_EQ_LV2_ALLPASS2 = 5,
};
#define OMX_EQ_LV2_TYPE_COUNT 6

/** The pass-filter slopes, dB/oct, in core's `FILTER_SLOPES` order: the two members a slope port
 * offers. 24 selects the two-section Butterworth. */
enum omx_eq_lv2_slope {
  OMX_EQ_LV2_SLOPE_12 = 12,
  OMX_EQ_LV2_SLOPE_24 = 24,
};

/* The declared travels are `@freemixer/declarations`' EQ_FREQ_RANGE, EQ_GAIN_RANGE, EQ_Q_RANGE,
 * EQ_NOTCH_Q_RANGE, HPF_FREQ_RANGE and LPF_FREQ_RANGE, read as the generated
 * OMX_<NAME>_MIN/_MAX of omx_contract_limits.h (dsp-primitives §7's scalar door) — this face types
 * none of them (audit 2026-09-29 C-1). */

/** The bank's capacity: every band plus two Butterworth sections per pass filter. */
#define OMX_EQ_LV2_PASS_SECTIONS 4u
#define OMX_EQ_LV2_SECTIONS ((uint32_t)OMX_EQ_LV2_BANDS + OMX_EQ_LV2_PASS_SECTIONS)

/** Zero intentional latency: a biquad cascade has group delay and no delay line. */
#define OMX_EQ_LV2_LATENCY_FRAMES 0

/** One band as the host last set it — the desk's `EqBand`, minus the id and the label. */
struct omx_eq_lv2_band {
  int type;
  float freq_hz;
  float gain_db;
  float q;
  int on;
  float coeffs[5]; /* designed for the four above, at the core's rate */
  int designed;    /* 0 until the first design */
};

struct omx_eq_lv2_pass {
  int on;
  float freq_hz;
  int slope_24;
  float coeffs[2][5];
  uint32_t sections; /* 1 or 2 */
  int designed;
};

struct omx_eq_lv2 {
  double rate;
  int on;
  struct omx_eq_lv2_band band[OMX_EQ_LV2_BANDS];
  struct omx_eq_lv2_pass hpf, lpf;

  /* The PUBLISHED bank — what the desk's control thread hands mix_lane.h. */
  uint32_t count;
  float coeffs[OMX_EQ_LV2_SECTIONS][5];
  uint8_t enabled[OMX_EQ_LV2_SECTIONS];
  int dirty; /* a control moved since the bank was last assembled */

  /* The RT state, one 4-word slot per bank entry, aligned with `coeffs` by index. */
  float state[OMX_EQ_LV2_SECTIONS][4];
};

/** `clampEqQFor`: a notch may go ultra-narrow; every other shape keeps `EQ_Q_RANGE`. */
static inline float omx_eq_lv2_clamp_q(int type, float q) {
  return type == OMX_EQ_LV2_NOTCH ? omx_clampf(q, OMX_EQ_NOTCH_Q_RANGE_MIN, OMX_EQ_NOTCH_Q_RANGE_MAX)
                                  : omx_clampf(q, OMX_EQ_Q_RANGE_MIN, OMX_EQ_Q_RANGE_MAX);
}

/** `bandHasGain` ∧ gain == 0 → `eqBandIsIdentity`: present in the bank, parked. */
static inline int omx_eq_lv2_band_is_identity(const struct omx_eq_lv2_band *b) {
  return b->type != OMX_EQ_LV2_NOTCH && b->type != OMX_EQ_LV2_ALLPASS1 && b->type != OMX_EQ_LV2_ALLPASS2 &&
         b->gain_db == 0.0f;
}

static inline enum omx_eq_kind omx_eq_lv2_kind(int type) {
  switch (type) {
    case OMX_EQ_LV2_LOWSHELF: return OMX_EQ_LOWSHELF;
    case OMX_EQ_LV2_HIGHSHELF: return OMX_EQ_HIGHSHELF;
    case OMX_EQ_LV2_NOTCH: return OMX_EQ_NOTCH;
    case OMX_EQ_LV2_ALLPASS1: return OMX_EQ_ALLPASS1;
    case OMX_EQ_LV2_ALLPASS2: return OMX_EQ_ALLPASS2;
    case OMX_EQ_LV2_BELL:
    default: return OMX_EQ_PEAKING;
  }
}

/** A fresh core at `rate`: the desk's flat default is not assumed — every band is OFF until the
 * host sets it (the TTL's defaults are what a host racks it with), the pass filters are off,
 * and the whole EQ is on. A freshly racked instance is therefore a wire. */
static inline void omx_eq_lv2_init(struct omx_eq_lv2 *e, double rate) {
  memset(e, 0, sizeof *e);
  e->rate = rate;
  e->on = 1;
  e->dirty = 1;
}

/** `activate`: the history is cleared, the design is kept. */
static inline void omx_eq_lv2_reset_state(struct omx_eq_lv2 *e) {
  memset(e->state, 0, sizeof e->state);
}

/** Set band `i` from the host's four words plus its switch. Clamps to the declared ranges
 * (`clampEqFreq`/`clampEqGain`/`clampEqQFor`) and redesigns the section only if something
 * moved. Returns 1 when the bank must be rebuilt. A foreign host may write NaN or ±Inf: freq
 * and Q read a NaN as the floor (omx_clampf), the gain reads any non-finite word as 0 dB
 * (omx_clamp_or) — primitives spec §1 row 23. */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "eq-lv2/set-band"
static inline int omx_eq_lv2_set_band(struct omx_eq_lv2 *e, uint32_t i, int type, float freq_hz,
                                      float gain_db, float q, int on) {
  OMX_PRE(e->rate > 0.0, "rate-positive");
  if (i >= (uint32_t)OMX_EQ_LV2_BANDS) return 0;
  struct omx_eq_lv2_band *b = &e->band[i];
  if (type < 0 || type >= OMX_EQ_LV2_TYPE_COUNT) type = OMX_EQ_LV2_BELL;
  freq_hz = omx_clampf(freq_hz, OMX_EQ_FREQ_RANGE_MIN, OMX_EQ_FREQ_RANGE_MAX);
  gain_db = omx_clamp_or(gain_db, OMX_EQ_GAIN_RANGE_MIN, OMX_EQ_GAIN_RANGE_MAX, 0.0f);
  q = omx_eq_lv2_clamp_q(type, q);
  on = on ? 1 : 0;
  const int moved = !b->designed || b->type != type || b->freq_hz != freq_hz ||
                    b->gain_db != gain_db || b->q != q;
  const int switched = b->on != on;
  if (!moved && !switched) return 0;
  b->type = type;
  b->freq_hz = freq_hz;
  b->gain_db = gain_db;
  b->q = q;
  b->on = on;
  if (moved) {
    omx_eq_design_f(omx_eq_lv2_kind(type), freq_hz, q, gain_db, e->rate, b->coeffs);
    b->designed = 1;
  }
  e->dirty = 1;
  OMX_POST(b->freq_hz >= OMX_EQ_FREQ_RANGE_MIN && b->freq_hz <= OMX_EQ_FREQ_RANGE_MAX,
           "freq-inside-declared-range");
  OMX_POST(b->gain_db >= OMX_EQ_GAIN_RANGE_MIN && b->gain_db <= OMX_EQ_GAIN_RANGE_MAX,
           "gain-inside-declared-range");
  OMX_POST(b->q >= OMX_EQ_Q_RANGE_MIN && b->q <= OMX_EQ_NOTCH_Q_RANGE_MAX, "q-inside-declared-range");
  OMX_POST(omx_block_finite(b->coeffs, 5u), "finite-coeffs");
  return 1;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "eq-lv2/set-pass"
static inline int omx_eq_lv2_set_pass(struct omx_eq_lv2 *e, struct omx_eq_lv2_pass *pf,
                                      enum omx_eq_kind kind, float lo, float hi, int on,
                                      float freq_hz, int slope_24) {
  OMX_PRE(e->rate > 0.0 && lo < hi, "rate-positive-and-range-ordered");
  freq_hz = omx_clampf(freq_hz, lo, hi);
  on = on ? 1 : 0;
  slope_24 = slope_24 ? 1 : 0;
  const int moved = !pf->designed || pf->freq_hz != freq_hz || pf->slope_24 != slope_24;
  const int switched = pf->on != on;
  if (!moved && !switched) return 0;
  pf->on = on;
  pf->freq_hz = freq_hz;
  pf->slope_24 = slope_24;
  if (moved) {
    double qs[2];
    pf->sections = omx_eq_butterworth_qs(slope_24, qs);
    for (uint32_t s = 0; s < pf->sections; s++)
      omx_eq_design_f(kind, freq_hz, qs[s], 0.0, e->rate, pf->coeffs[s]);
    pf->designed = 1;
  }
  e->dirty = 1;
  OMX_POST(pf->freq_hz >= lo && pf->freq_hz <= hi, "cutoff-inside-declared-range");
  OMX_POST(pf->sections == 1u || pf->sections == 2u, "one-or-two-sections");
  OMX_POST(omx_block_finite(pf->coeffs[0], 5u * pf->sections), "finite-coeffs");
  return 1;
}
#undef OMX_CONTRACT_STAGE

/** The HPF: `clampFilterFreq('hpf')`, `HPF_FREQ_RANGE`. */
static inline int omx_eq_lv2_set_hpf(struct omx_eq_lv2 *e, int on, float freq_hz, int slope_24) {
  return omx_eq_lv2_set_pass(e, &e->hpf, OMX_EQ_HIGHPASS, OMX_HPF_FREQ_RANGE_MIN, OMX_HPF_FREQ_RANGE_MAX,
                             on, freq_hz, slope_24);
}

/** The LPF: `clampFilterFreq('lpf')`, `LPF_FREQ_RANGE`. */
static inline int omx_eq_lv2_set_lpf(struct omx_eq_lv2 *e, int on, float freq_hz, int slope_24) {
  return omx_eq_lv2_set_pass(e, &e->lpf, OMX_EQ_LOWPASS, OMX_LPF_FREQ_RANGE_MIN, OMX_LPF_FREQ_RANGE_MAX,
                             on, freq_hz, slope_24);
}

/** The whole-EQ switch (`EqState.on`). */
static inline int omx_eq_lv2_set_on(struct omx_eq_lv2 *e, int on) {
  on = on ? 1 : 0;
  if (e->on == on) return 0;
  e->on = on;
  e->dirty = 1;
  return 1;
}

/** `eqCoeffs`: assemble the bank from the bands and the pass filters, in that order, dropping
 * `on: false` bands and parking identity ones. Idempotent; a no-op unless a control moved. */
#define OMX_CONTRACT_STAGE "eq-lv2/publish"
static inline void omx_eq_lv2_publish(struct omx_eq_lv2 *e) {
  if (!e->dirty) return;
  e->dirty = 0;
  uint32_t k = 0;
  if (!e->on) {
    e->count = 0;
    OMX_POST(e->count == 0u, "off-is-an-empty-bank");
    return;
  }
  for (uint32_t i = 0; i < (uint32_t)OMX_EQ_LV2_BANDS; i++) {
    const struct omx_eq_lv2_band *b = &e->band[i];
    if (!b->on || !b->designed) continue;
    memcpy(e->coeffs[k], b->coeffs, sizeof b->coeffs);
    e->enabled[k] = omx_eq_lv2_band_is_identity(b) ? 0u : 1u;
    k++;
  }
  const struct omx_eq_lv2_pass *pfs[2] = {&e->hpf, &e->lpf};
  for (int p = 0; p < 2; p++) {
    if (!pfs[p]->on || !pfs[p]->designed) continue;
    for (uint32_t s = 0; s < pfs[p]->sections; s++) {
      memcpy(e->coeffs[k], pfs[p]->coeffs[s], sizeof pfs[p]->coeffs[s]);
      e->enabled[k] = 1u;
      k++;
    }
  }
  e->count = k;
  OMX_POST(e->count <= OMX_EQ_LV2_SECTIONS, "bank-within-capacity");
  OMX_POST(omx_block_finite(e->coeffs[0], 5u * e->count), "finite-bank");
}
#undef OMX_CONTRACT_STAGE

/**
 * ONE block, in place over `buf`: the published bank through `omx_biquad_cascade`, the desk's
 * applier, in chunks of at most `OMX_EQ_MAX_BANDS` sections (see the header note — band-outer,
 * so a chunked walk IS the unchunked walk). An empty bank leaves every sample untouched.
 */
#define OMX_CONTRACT_STAGE "eq-lv2/process"
static inline void omx_eq_lv2_process(struct omx_eq_lv2 *e, float *buf, uint32_t n) {
  OMX_PRE(omx_block_finite(buf, n), "finite-in");
  omx_eq_lv2_publish(e);
  OMX_PRE(e->count <= OMX_EQ_LV2_SECTIONS, "bank-within-capacity");
  for (uint32_t b0 = 0; b0 < e->count; b0 += OMX_EQ_MAX_BANDS) {
    const uint32_t nb = e->count - b0 > OMX_EQ_MAX_BANDS ? OMX_EQ_MAX_BANDS : e->count - b0;
    omx_biquad_cascade(buf, n, nb, (const float (*)[5])&e->coeffs[b0], &e->enabled[b0],
                       &e->state[b0]);
  }
  OMX_POST(omx_block_finite(buf, n), "finite-out");
  OMX_POST(omx_block_finite(e->state[0], 4u * e->count), "finite-state");
}
#undef OMX_CONTRACT_STAGE

/** The LV2 face: `in` to `out` (they may alias — an in-place host is fine). */
#define OMX_CONTRACT_STAGE "eq-lv2/run"
static inline void omx_eq_lv2_run(struct omx_eq_lv2 *e, const float *in, float *out, uint32_t n) {
  OMX_PRE(in != NULL && out != NULL, "ports-connected");
  if (out != in) memmove(out, in, n * sizeof(float));
  omx_eq_lv2_process(e, out, n);
  OMX_POST(omx_block_finite(out, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/* ---- the control words: eq_lv2.c's run(), without the LV2 face -------------------------------- */

/** The defaults a word reads when its port is not connected (eq_lv2.c's `ctl(…, dflt)` arguments):
 * the HPF at 80 Hz, the LPF at 18 kHz, a band a 1 kHz bell at 0 dB, Q 1, all off. */
#define OMX_EQ_LV2_HPF_FREQ_DEFAULT 80.0f
#define OMX_EQ_LV2_LPF_FREQ_DEFAULT 18000.0f
#define OMX_EQ_LV2_BAND_FREQ_DEFAULT 1000.0f
#define OMX_EQ_LV2_BAND_GAIN_DEFAULT 0.0f
#define OMX_EQ_LV2_BAND_Q_DEFAULT 1.0f

/** The words of one cycle, in the port layout's order (eq_lv2.c): the whole-EQ switch, the HPF
 * triple, the LPF triple, then N band quintets in `EqBand` order. A NULL word is an unconnected
 * port and reads its default. */
struct omx_eq_lv2_controls {
  const float *on;
  const float *hpf_on, *hpf_freq, *hpf_slope;
  const float *lpf_on, *lpf_freq, *lpf_slope;
  const float *band[OMX_EQ_LV2_BANDS][5]; /* type, freq, gain, Q, on */
};

/** A float word, or its default when unconnected. */
static inline float omx_eq_lv2_word(const float *w, float dflt) { return w ? *w : dflt; }

/** A switch word: on above one half. */
static inline int omx_eq_lv2_word_on(const float *w, int dflt) { return w ? (*w > 0.5f) : dflt; }

/** An integer word (a slope, a band type): held inside [lo, hi], every non-finite word the declared
 * default, before it is rounded half away from zero (omx_port_int.h's `omx_port_int`, dsp-primitives
 * row 23). */
#define OMX_CONTRACT_STAGE "port/int"
static inline int omx_eq_lv2_word_int(const float *w, int lo, int hi, int dflt) {
  if (!w) return dflt;
  const float c = omx_clamp_or(*w, (float)lo, (float)hi, (float)dflt);
  const int i = (int)(c + (c < 0.0f ? -0.5f : 0.5f));
  OMX_POST(i >= lo && i <= hi, "inside-the-travel");
  return i;
}
#undef OMX_CONTRACT_STAGE

/** eq_lv2.c's run() up to the cascade: every control word handed to the setters, which clamp and
 * redesign only what moved. No allocation, no lock, no I/O. */
static inline void omx_eq_lv2_set_controls(struct omx_eq_lv2 *e, const struct omx_eq_lv2_controls *c) {
  omx_eq_lv2_set_on(e, omx_eq_lv2_word_on(c->on, 1));
  omx_eq_lv2_set_hpf(e, omx_eq_lv2_word_on(c->hpf_on, 0), omx_eq_lv2_word(c->hpf_freq, OMX_EQ_LV2_HPF_FREQ_DEFAULT),
                     omx_eq_lv2_word_int(c->hpf_slope, OMX_EQ_LV2_SLOPE_12, OMX_EQ_LV2_SLOPE_24,
                                         OMX_EQ_LV2_SLOPE_12) >= OMX_EQ_LV2_SLOPE_24);
  omx_eq_lv2_set_lpf(e, omx_eq_lv2_word_on(c->lpf_on, 0), omx_eq_lv2_word(c->lpf_freq, OMX_EQ_LV2_LPF_FREQ_DEFAULT),
                     omx_eq_lv2_word_int(c->lpf_slope, OMX_EQ_LV2_SLOPE_12, OMX_EQ_LV2_SLOPE_24,
                                         OMX_EQ_LV2_SLOPE_12) >= OMX_EQ_LV2_SLOPE_24);
  for (uint32_t i = 0; i < (uint32_t)OMX_EQ_LV2_BANDS; i++) {
    const float *const *b = c->band[i];
    omx_eq_lv2_set_band(e, i,
                        omx_eq_lv2_word_int(b[0], OMX_EQ_LV2_BELL, OMX_EQ_LV2_TYPE_COUNT - 1, OMX_EQ_LV2_BELL),
                        omx_eq_lv2_word(b[1], OMX_EQ_LV2_BAND_FREQ_DEFAULT),
                        omx_eq_lv2_word(b[2], OMX_EQ_LV2_BAND_GAIN_DEFAULT),
                        omx_eq_lv2_word(b[3], OMX_EQ_LV2_BAND_Q_DEFAULT), omx_eq_lv2_word_on(b[4], 0));
  }
}

#endif /* OMX_EQ_LV2_H */
