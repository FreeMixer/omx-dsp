// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_key.h
 * @brief A dynamics slot's key: the detector's own copy of a signal, filtered, never the audio
 *        (struct omx_key, omx_key_build()).
 *
 * A gate normally opens on the signal it gates. A keyed gate opens on another signal (the kick
 * that lets the bass through), or on its own signal seen through a filter, so a tom mic ignores
 * the kick sitting in another band. A keyed compressor (a ducker) reads its key the same way. The
 * key is built from a source block, or from the lane's own legs, then high-passed and low-passed
 * by one biquad section per edge; the filtered copy feeds the detector and the fader still
 * carries the lane's own full-range audio.
 *
 * Moved from OpenMixer's engine (its mix_lane.h) with the same names, fields, arithmetic and
 * contract stage and tokens (`lane/key-build`), bit for bit.
 *
 * Header-only. Not in omxdsp.h: include <omxdsp/mix/omx_key.h>.
 */
#ifndef OMX_KEY_H
#define OMX_KEY_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/mix/omx_rta_tap.h>

/**
 * @brief The key one dynamics slot's detector listens to.
 *
 * `src` is the key source's samples for this block, or NULL for self: the copy is then taken
 * from the samples the slot is about to process, at zero added latency. A cross-channel source's
 * age (how many blocks old it is) is the caller's to decide and to keep uniform.
 *
 * `buf` is where the filtered detector copy is built: scratch the caller owns, written and
 * consumed inside one lane's processing, so the next lane may reuse it. A NULL `buf` is a slot
 * with no key at all: the detector reads the slot's own legs and every sample is bit-identical to
 * the un-keyed path.
 *
 * `hp_c`/`lp_c` are RBJ biquad sections the caller designed, one per edge, 12 dB per octave. They
 * filter the detector's copy and nothing else.
 */
struct omx_key {
  const float *src;  /* the key source's block; NULL = self (this lane's own gate input) */
  float *buf;        /* scratch the filtered detector copy is built in; NULL = no key */
  int hp_on, lp_on;  /* 0 = that edge is off (the row's 0 Hz) */
  float hp_c[5];     /* {b0,b1,b2,a1,a2}, normalised — high-pass */
  float lp_c[5];     /* the same, low-pass */
  /* Whether this key FEEDS THE DETECTOR (a source other than self, or an engaged filter edge)
   * or is only being LISTENED to. A listen-only key still fills `buf` — that is what the
   * operator hears — but the gate detects exactly as it would with no key at all, so latching
   * a monitor can never move the strip's gain reduction. */
  int to_detector;
  /* SET BY THE WALK, read by the caller: 1 once `buf` holds this block's filtered detector
   * copy. The gate STEP is what builds it, so a lane whose published order carries no gate
   * step has no detector this block — and KEY LISTEN must then feed the cue bus nothing rather
   * than whatever the previous strip left in the shared scratch. An out-field, not a control:
   * the caller clears it when it resolves the key. */
  int built;
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "lane/key-build"
/**
 * @brief Build one dynamics slot's key into `k->buf`: the source copied, then the high-pass and
 *        the low-pass sections run over it.
 *
 * The source is `k->src` when set; otherwise self: `l` as it stands, or `0.5·(l + r)` on a stereo
 * lane (omx_rta_tap_mono_sum(), the mono fold a solo takes). The high-pass then the low-pass run
 * over that copy in `k->buf`, each only when its edge is on, persisting in `hp_state` and
 * `lp_state`. Sets `k->built`.
 * @param k The slot's key; NULL, or a NULL `k->buf`, is no key at all (returns NULL, writes
 *          nothing).
 * @param l The lane's left (or only) leg, `n` samples; read, never written.
 * @param r The lane's right leg, `n` samples, or NULL on a mono lane; read, never written.
 * @param n Samples in the block.
 * @param hp_state The high-pass section's 4-word history, caller-owned.
 * @param lp_state The low-pass section's 4-word history, caller-owned.
 * @return `k->buf` when the key feeds the detector (`k->to_detector`), else NULL: a NULL return
 *         is the un-keyed detector, bit for bit.
 * @pre `finite-in-l`: the leg the self key copies is finite.
 * @post `key-finite`: the block the detector reads is finite.
 * @note RT-safe: O(n), no allocation, no lock, no IO. Thread-safe over distinct `k` and state.
 */
static inline const float *omx_key_build(struct omx_key *k, const float *l, const float *r, uint32_t n,
                                         float hp_state[4], float lp_state[4]) {
  if (!k || !k->buf) return NULL;
  OMX_PRE(n == 0u || omx_block_finite(l, n), "finite-in-l");
  float *kb = k->buf;
  if (k->src) {
    memcpy(kb, k->src, n * sizeof(float));
  } else if (r) {
    omx_rta_tap_mono_sum(kb, l, r, n);
  } else {
    memcpy(kb, l, n * sizeof(float));
  }
  if (k->hp_on)
    for (uint32_t i = 0; i < n; i++) kb[i] = omx_biquad(kb[i], k->hp_c, hp_state);
  if (k->lp_on)
    for (uint32_t i = 0; i < n; i++) kb[i] = omx_biquad(kb[i], k->lp_c, lp_state);
  k->built = 1;
  OMX_POST(n == 0u || omx_block_finite(kb, n), "key-finite");
  return k->to_detector ? kb : NULL;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_KEY_H */
