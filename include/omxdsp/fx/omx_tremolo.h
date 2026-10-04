// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_tremolo.h
 * @brief The native TREMOLO / AUTO-PAN kernel: one `omx_lfo` read per sample, turned into a gain.
 *
 * Spec: docs/design/specs/2026-09-26-native-fx-catalogue.md §2 (row "M32 Tremolo / Panner") and
 * docs/design/specs/2026-09-26-dsp-primitives.md §2 — a CONSUMER of primitive 12 (`omx_lfo.h`),
 * no new primitive. With `s = omx_lfo_at(lfo, 0)`:
 *
 *     tremolo  both legs × (1 − mix·depth·(1 − s)/2)
 *     pan      (bL, bR) = omx_balance_law(depth·s);  leg × (1 − mix·(1 − b))
 *
 * The wet/dry sum `(1 − mix)·x + mix·g·x` is folded into ONE gain, so `depth = 0` or `mix = 0`
 * is the gain `1.0f` exactly and the stage is memcmp-identical to its input with no branch. The
 * pan reads MAIN's one stereo law (`omx_balance_law`, `mix_dsp.h`), never a second one. Every
 * gain lies in [1 − mix·depth, 1]: the stage never adds level.
 *
 * Same contract as mix_flanger.h: NO PipeWire, NO napi, NO allocation; the controls are a
 * word-atomic snapshot resolved once per block.
 */
#ifndef OMX_MIX_TREMOLO_H
#define OMX_MIX_TREMOLO_H

#include <stdint.h>

#include <omxdsp/omx_balance.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_lfo.h>

/** @brief What the one oscillator modulates. */
enum omx_tremolo_mode {
  OMX_TREMOLO_MODE_TREMOLO = 0, /**< Amplitude, both legs in phase. */
  OMX_TREMOLO_MODE_PAN = 1,     /**< The balance law's position, the legs in opposition. */
};

/** @brief The resolved control atom. */
struct omx_tremolo {
  int enabled;  /**< 0 → passthrough, before a sample or a state word is touched. */
  int mode;     /**< An `enum omx_tremolo_mode` member; any other value reads as tremolo. */
  float lfo_inc; /**< Turns per sample (`omx_lfo_inc`), in [0, 0.5). */
  float depth;  /**< The modulation's reach, in [0, 1]. */
  float mix;    /**< 0 = dry (bit-identical), 1 = the modulated signal only. */
};

/** @brief RT-owned state: the oscillator alone. */
struct omx_tremolo_state {
  struct omx_lfo lfo; /**< The one oscillator both legs read. */
};

/**
 * @brief Start the oscillator at phase zero, frozen.
 * @param s The state.
 * @note RT-safe: `omx_lfo_start`.
 */
static inline void omx_tremolo_state_init(struct omx_tremolo_state *s) {
  omx_lfo_start(&s->lfo);
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "tremolo/gains"
/**
 * @brief The per-leg gains at one shape value.
 * @param p The atom; `depth` and `mix` inside [0, 1].
 * @param s A shape value, in [−1, 1].
 * @param gl Left gain, out.
 * @param gr Right gain, out.
 * @pre `a-shape-is-inside-unity`.
 * @post `a-tremolo-gain-never-adds-level`.
 * @note RT-safe and thread-safe: at most four multiplies.
 */
static inline void omx_tremolo_gains(const struct omx_tremolo *p, float s, float *gl, float *gr) {
  OMX_PRE(s >= -1.0f && s <= 1.0f, "a-shape-is-inside-unity");
  if (p->mode == OMX_TREMOLO_MODE_PAN) {
    float bl, br;
    omx_balance_law(p->depth * s, &bl, &br);
    *gl = 1.0f - p->mix * (1.0f - bl);
    *gr = 1.0f - p->mix * (1.0f - br);
  } else {
    const float g = 1.0f - p->mix * p->depth * 0.5f * (1.0f - s);
    *gl = g;
    *gr = g;
  }
  OMX_POST(*gl >= 0.0f && *gl <= 1.0f && *gr >= 0.0f && *gr <= 1.0f,
           "a-tremolo-gain-never-adds-level");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "tremolo"
/**
 * @brief Process one block IN PLACE on the strip's two legs.
 * @param l Left leg.
 * @param r Right leg.
 * @param n Frames.
 * @param p The atom.
 * @param s The state; its oscillator advances `n` samples.
 * @pre `finite-in`, `depth-in-unit-range`, `mix-in-unit-range`, `an-increment-is-inside-one-turn`.
 * @post `finite-out`; the oscillator's turn is `omx_lfo_advance`'s own postcondition.
 * @note RT-safe: one oscillator read and two multiplies per frame, no allocation, no libm.
 */
static inline void omx_tremolo_process(float *l, float *r, uint32_t n, const struct omx_tremolo *p,
                                       struct omx_tremolo_state *s) {
  OMX_PRE(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-in");
  OMX_PRE(p->depth >= 0.0f && p->depth <= 1.0f, "depth-in-unit-range");
  OMX_PRE(p->mix >= 0.0f && p->mix <= 1.0f, "mix-in-unit-range");
  OMX_PRE(p->lfo_inc >= 0.0f && p->lfo_inc < 1.0f, "an-increment-is-inside-one-turn");
  if (!p->enabled || n == 0u) return;
  s->lfo.inc = p->lfo_inc;
  for (uint32_t i = 0; i < n; i++) {
    float gl, gr;
    omx_tremolo_gains(p, omx_lfo_at(&s->lfo, 0.0f), &gl, &gr);
    omx_lfo_advance(&s->lfo);
    l[i] *= gl;
    r[i] *= gr;
  }
  OMX_POST(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_TREMOLO_H */
