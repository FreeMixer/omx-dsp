// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_deesser.h — the native DE-ESSER stage: a band-limited detector driving the COMPRESSOR's own
 * gain computer.
 *
 * Spec: docs/design/specs/2026-09-22-native-deesser-stage.md (issue #891).
 *
 * THIS FILE MINTS NO DSP MECHANISM, and that is its whole design. Its detect/gain/split step is
 * the band-dynamics word, mix_band_dyn.h (docs/design/specs/2026-09-26-native-dynamic-eq.md §2). The detector is
 * {@link omx_env_step} — the envelope cascade the gate and the comp already run. The gain computer
 * is {@link omx_gaincomp_db} in OMX_DYN_ABOVE mode — the same soft-knee characteristic the comp
 * slot runs, with ONE extra clamp (§4c: the reduction is floored at `range_db`). The band is ONE
 * {@link omx_biquad} section designed in TypeScript (`deEsserBandSection`, the cookbook bandpass),
 * like every other coefficient this node runs. What is new here is the WIRING: the detector
 * listens to the band and the gain is applied either to the whole strip or to the band alone.
 *
 * ## The shape, in one line each
 *
 *   B = BP(x)                           the detector band, one cookbook bandpass, per leg
 *   N = x - B                           its complement, the cookbook notch: |N| <= 1 everywhere
 *   level = envelope cascade of |B|     PEAK, one detector for both legs (folded 0.5/0.5)
 *   gdb   = max(range_db, ABOVE(level)) the shared characteristic, floored
 *   split     y = x + (g - 1)*B         = g*x + (1 - g)*N: at g = 1 this is x EXACTLY, sample for
 *                                       sample, and for g in (0, 1] it is a convex combination of
 *                                       two signals no louder than the input — no gain added at
 *                                       ANY frequency (spec §6; operator ruling, §1 gate line 3)
 *   wideband  y = g*x                   the whole strip ducks while the "s" lasts
 *
 * ## Why the band is a bandpass SECTION and not a highpass·lowpass PAIR
 *
 * The pair this stage first shipped with is not complementary: with H = LP·HP, split's output is
 * x·(1 - (1 - g)·H) and |1 - (1 - g)·H| > 1 wherever Re(H) < 0 — measured +1.0 dB at 2.5 kHz,
 * +0.8 at 3.5 kHz, +0.9 at 16 kHz, +0.3 at 1 kHz at all four rates with a 7 kHz sibilant at the
 * -24 dB floor. The cookbook bandpass and the cookbook notch share a denominator and their
 * numerators sum to it, so B + N = 1 exactly and |B|² + |N|² = 1 on the unit circle; the bound
 * is arithmetic. The C only RUNS the section it is handed — the bound is a property of the DESIGN
 * (core's `bandpass` kind keeps the cookbook denominator for exactly this reason: the identity
 * holds for the cookbook pair and is not proved for any other), and mix_deesser.test.c measures
 * it: a probe tone swept over the audio band never rises above the input, and the sabotage that
 * hands the kernel the old pair turns that arm red.
 *
 * ## Why the FLOOR is here and not in omx_gaincomp_db
 *
 * `range_db` is a live control of the COMP slot as well (mixer_rt.c's resolve_dyn loads it for both
 * modes; the BELOW branch floors on it and the ABOVE branch ignores it). Flooring the ABOVE branch
 * inside the shared computer would change what every compressor on this desk does. So the floor is
 * applied HERE, over the shared computer's output, and is declared as this stage's own bound.
 *
 * ## Why there is no stereoLink, no look-ahead and no oversampler
 *
 * ONE gain serves both legs by construction — the detector reads the two legs' band folded at 0.5
 * apiece, exactly as omx_dynamics links its own — so a stereo strip's image can never shift under
 * de-essing and there is no second thing to link. A look-ahead would put latency on EVERY vocal
 * channel (the operator's ask is a de-esser everywhere) and is refused in the spec's §5 rather than
 * left as an unbuilt option. An oversampler would buy nothing: this stage's non-linearity is a
 * GAIN, not a waveshaper, and a gain generates no harmonics to alias.
 *
 * Same contract as mix_dsp.h: NO PipeWire, NO napi, NO allocation on the RT thread, NO libc beyond
 * <math.h>/<string.h>. The controls ({@link omx_deess}) are a word-atomic snapshot the RT reads
 * once per block; the working state ({@link omx_deess_state}) is PLAIN INLINE FLOATS — this kernel
 * has no buffers, so unlike mix_drive.h there is nothing to calloc and nothing that can be NULL.
 */
#ifndef OMX_MIX_DEESSER_H
#define OMX_MIX_DEESSER_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_band_dyn.h>
#include <omxdsp/omx_contract.h>

/*
 * Where the reduction lands. Members, never merged — the same law mix_reverb.h's algorithms and
 * mix_drive.h's curves carry.
 *
 * SPLIT subtracts the reduced band from the signal: `y = x + (g-1)*B`. It reconstructs EXACTLY at
 * g = 1 (the term is zero), which is what makes the identity a piece of arithmetic rather than a
 * claim about a filter pair. That the result never exceeds the input for ANY g needs one more
 * fact, and it is the band's: its complement x - B is a section of magnitude <= 1 everywhere
 * (the header note above), so what split leaves is a convex mix of x and that complement.
 */
enum omx_deess_mode {
  OMX_DEESS_SPLIT = 0,
  OMX_DEESS_WIDEBAND = 1,
};

/*
 * The resolved control atom — built once per block by the caller from the word-atomic controls
 * (mixer_rt.c's resolve_fx_deess), exactly as resolve_dyn builds the comp's.
 *
 * `dyn` IS the comp's own resolved atom, carried whole rather than copied field by field: mode
 * (always OMX_DYN_ABOVE), detect (always OMX_DETECT_PEAK), thresh_db, ratio, knee_db, range_db
 * (this stage's FLOOR), makeup_lin (always 1 — a de-esser only ever attenuates) and the two poles
 * the operator's milliseconds produced. One struct, one set of field names, one gain computer.
 *
 * The BIQUAD coefficients are designed in TypeScript (`@freemixer/core`'s deEsserBandSection at
 * the live rate) — the C only RUNS them. `{b0,b1,b2,a1,a2}`, normalised, a-terms subtracted.
 */
struct omx_deess {
  int enabled; /* 0 -> the whole stage is a no-op: no sample and no state word is touched */
  int mode;    /* enum omx_deess_mode */
  struct omx_dyn dyn;
  float bp_c[5]; /* the detector band: one cookbook bandpass at freqHz, its Q from widthOct */
};

/**
 * @brief ONE lane's de-esser state: the band-dynamics word's own state (mix_band_dyn.h), whole.
 */
struct omx_deess_state {
  struct omx_band_dyn_state band; /**< The bell's history per leg and the ONE detector cascade. */
};

/* Allocation-freeness by construction and enforced at COMPILE time, the same guard
 * `struct omx_lane_rt` carries: adding a pointer (a ring, a look-ahead line) changes the size and
 * breaks this build — which is the point, because such a member would have to be allocated
 * somewhere, and the only somewhere left would be the RT callback. */
_Static_assert(sizeof(struct omx_deess_state) == sizeof(float) * (OMX_DYN_ENV_STAGES + 8u),
               "de-esser state must stay plain inline floats - nothing to allocate");

/** Clear one lane's de-esser state. CONTROL thread only. */
static inline void omx_deess_state_init(struct omx_deess_state *st) { memset(st, 0, sizeof(*st)); }

/*
 * THE STAGE'S LATENCY, in base-rate samples: ZERO, at every rate and in both modes, engaged or not.
 *
 * It is a function rather than a constant so that every reader asks the same question of the same
 * place — the drive's `omx_drive_latency` is a function for the opposite reason (its answer moves)
 * and the console's latency basis must not learn to treat the two differently. The value is proved
 * from an IMPULSE in mix_deesser.test.c, never read off this line.
 */
static inline int omx_deess_latency(const struct omx_deess *p) {
  (void)p;
  return 0;
}

/*
 * The GAIN (dB, <= 0) this stage applies at detector level `level` (linear, the cascade's last
 * stage in the detector's own domain — {@link omx_env_level}).
 *
 * The shared ABOVE characteristic, FLOORED at `range_db` and capped at 0 dB. Pulled out so the RT
 * loop and the GR readout (`publish_lane_gr`, mixer_rt.c) cannot compute it differently: the
 * meter's whole claim is that it re-runs the SAME gain computer on the SAME level the last sample
 * used, and a second transcription is how a meter and a stage quietly stop agreeing.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "deess/gain-db"
static inline float omx_deess_gain_db(const struct omx_deess *p, float level) {
  float g = omx_band_dyn_offset_db(&p->dyn.gc, level);
  g = g > 0.0f ? 0.0f : g;
  OMX_POST(g <= 0.0f, "no-gain-added");
  OMX_POST(g - g == 0.0f, "finite");
  return g;
}
#undef OMX_CONTRACT_STAGE

/*
 * Apply one lane's de-esser to its leg(s) IN PLACE.
 *
 * `l` is always driven; `r` may be NULL (mono) or the second leg. ONE detector, ONE gain, both
 * legs — a stereo strip's image never shifts under de-essing.
 *
 * A disabled stage is a no-op that leaves the signal AND the state untouched: the biquad histories
 * and the envelope cascade do not advance, so a lane whose de-esser is off is bit-identical to a
 * lane that has none. "Nothing computes unwatched" applies to a bypassed stage as much as to an
 * unwatched meter.
 *
 * RT-safe: O(n), no allocation, no lock, no IO, bounded work whatever the quantum.
 *
 * CONTRACT (omx_contract.h). The de-esser is a TIME-VARYING ATTENUATION derived from a band: it
 * multiplies the block by a per-sample coefficient in (0, 1] (wideband) or crossfades the block
 * toward its band-notched complement by that much (split), and nothing else. So its laws are about
 * the coefficient's BOUND and about the state staying finite — the peak law needs the input peak,
 * which this function overwrites in place, so it is stated by the test battery around the call.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "deesser"
static inline void omx_deess_process(float *l, float *r, uint32_t n, const struct omx_deess *p,
                                     struct omx_deess_state *st) {
  /* The config preconditions sit BEHIND the enable gate for the reason R-056 found on the drive
   * and the dynamics: a disabled slot's atom holds whatever the struct held before. */
  OMX_PRE(!p->enabled || p->dyn.gc.range_db <= 0.0f, "range-is-an-attenuation");
  OMX_PRE(!p->enabled || p->dyn.gc.mode == OMX_DYN_ABOVE, "above-mode");
  omx_band_dyn_run(l, r, n, &p->dyn, p->bp_c, &st->band, p->mode == OMX_DEESS_WIDEBAND, p->enabled);
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_DEESSER_H */
