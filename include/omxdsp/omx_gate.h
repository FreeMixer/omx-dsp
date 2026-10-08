// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_gate.h
 * @brief The keyed gate's DSP: its controls resolved to the dynamics atom inside the declared
 *        travel, its latency, and a host's block run through fixed scratch by the one dynamics
 *        kernel.
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/keyed_gate_lv2.c (omx-dsp-dev#34):
 * the processing half of the LV2 face, without the face. The port enum, the descriptor and the
 * stereo I/O binding stay with the plugin; a plugin hands this header the pointers its ports were
 * connected to, NULL for an unconnected one.
 *
 * Every sample of gain is computed by `omx_dynamics_keyed` (omx_dyn.h) — the SAME kernel the
 * console's gate slot runs, with the SAME resolved parameters the console pushes: ACT-BELOW mode,
 * peak detector, hard knee, unity make-up, detector oversampling on `auto`. A second
 * transcription of the gain computer is how two gates would quietly stop being the same gate, so
 * there is none.
 *
 * What the console's gate has that this gate does NOT, and why:
 *  - the KEY FILTER (HP/LP edges): an external host filters its own sidechain send.
 *  - HOLD / HYSTERESIS: the native slot's resolved parameters carry neither, so exposing them here
 *    would promise behaviour the kernel does not have.
 *
 * ## Aliasing
 *
 * A host may hand an input and an output that are the same buffer. The kernel works in place, and
 * a sidechain block that aliases an output would be overwritten by the copy that precedes it. So
 * each block is copied into the instance's scratch first — inputs AND key — processed there, and
 * copied out. Every aliasing pattern is then safe, and the cost is three memcpys.
 *
 * ## RT contract
 *
 * `omx_gate_run` allocates nothing, takes no lock, does no I/O and calls no libc beyond
 * memcpy/memset/expf. Chunking the host's block through fixed scratch changes nothing observable:
 * the kernel's state is continuous across calls.
 *
 * Not in the omxdsp.h umbrella, as omx_dyn.h is not. Include it by name.
 */
#ifndef OMX_GATE_H
#define OMX_GATE_H

#include <stdint.h>
#include <string.h>

#include "omx_contract.h"
#include "omx_contract_limits.h"
#include "omx_dyn.h"
#include "omx_param.h"

/** @brief Frames per pass through the scratch. Nothing observable depends on it (see above). */
#define OMX_GATE_CHUNK 256u

/** @brief The gate's controls: each points at its value, NULL for unconnected (the default). */
struct omx_gate_controls {
  const float *enabled;      /**< 0 bypasses the gate */
  const float *key_external; /**< non-zero keys the detector from the external input */
  const float *threshold;    /**< the level below which the gate closes */
  const float *ratio;        /**< the expansion ratio below the threshold */
  const float *range;        /**< the deepest attenuation the gate applies */
  const float *attack;       /**< the attack time */
  const float *release;      /**< the release time */
};

/** @brief One gate instance: its rate, the dynamics slot's state and the block scratch. */
struct omx_gate {
  float rate;                 /**< the sample rate the gate is armed at */
  struct omx_dyn_state st;    /**< the dynamics slot's state */
  float sl[OMX_GATE_CHUNK];   /**< the left scratch */
  float sr[OMX_GATE_CHUNK];   /**< the right scratch */
  float sk[OMX_GATE_CHUNK];   /**< the key scratch */
};

/**
 * @brief Arm a gate at `rate` (instantiate) or re-arm it (activate): the slot's state cleared, base path.
 * @param g The instance.
 * @param rate The host's sample rate, Hz.
 * @note Not RT-safe only in that it clears the whole instance's state; no allocation.
 */
static inline void omx_gate_init(struct omx_gate *g, float rate) {
  g->rate = rate;
  omx_dyn_state_init(&g->st, 1u);
}

/**
 * @brief One control read inside its declared travel: GATE_LIMITS through the generated OMX_GATE_*
 *        macros — the same numbers the generated TTL states. An unconnected control is the
 *        default; a NaN falls to `lo` (omx_clampf).
 * @param port The control's value, or NULL (unconnected).
 * @param lo The travel's floor.
 * @param hi The travel's roof.
 * @param dflt The default.
 * @return The value inside [lo, hi], or `dflt`.
 * @note RT-safe. Thread-safe: pure.
 */
static inline float omx_gate_clampf(const float *port, float lo, float hi, float dflt) {
  return port ? omx_clampf(*port, lo, hi) : dflt;
}

/**
 * @brief The console's gateStateToNativeDyn, restated as control reads — the ONE resolution a gate
 *        slot gets. The `threshold` control is the declared gate threshold travel: an external
 *        host's gate has one open point, not the desk's knee pair.
 * @param g The instance (its rate).
 * @param c The controls.
 * @param p The atom, written whole.
 * @note RT-safe. Thread-safe: pure in `g` and `c`.
 */
static inline void omx_gate_resolve(const struct omx_gate *g, const struct omx_gate_controls *c, struct omx_dyn *p) {
  memset(p, 0, sizeof *p);
  p->enabled = omx_gate_clampf(c->enabled, 0.0f, 1.0f, 1.0f) >= 0.5f;
  p->gc.mode = OMX_DYN_BELOW;
  p->detect = OMX_DETECT_PEAK;
  p->gc.thresh_db = omx_gate_clampf(c->threshold, OMX_GATE_THRESHOLD_DB_MIN, OMX_GATE_THRESHOLD_DB_MAX, OMX_GATE_THRESHOLD_DB_DEFAULT);
  p->gc.ratio = omx_gate_clampf(c->ratio, OMX_GATE_RATIO_MIN, OMX_GATE_RATIO_MAX, OMX_GATE_RATIO_DEFAULT);
  p->gc.knee_db = 0.0f;
  p->gc.range_db = omx_gate_clampf(c->range, OMX_GATE_RANGE_DB_MIN, OMX_GATE_RANGE_DB_MAX, OMX_GATE_RANGE_DB_DEFAULT);
  p->gc.makeup_lin = 1.0f;
  p->ovs_mode = OMX_DYN_OVS_AUTO;
  float a_ms = omx_gate_clampf(c->attack, OMX_GATE_ATTACK_MS_MIN, OMX_GATE_ATTACK_MS_MAX, OMX_GATE_ATTACK_MS_DEFAULT);
  float r_ms = omx_gate_clampf(c->release, OMX_GATE_RELEASE_MS_MIN, OMX_GATE_RELEASE_MS_MAX, OMX_GATE_RELEASE_MS_DEFAULT);
  p->attack_ms = a_ms;
  p->attack_coeff = omx_pole_from_time_ms(a_ms, g->rate);
  p->release_coeff = omx_pole_from_time_ms(r_ms, g->rate);
}

/* The latency the kernel's compensation delay adds IS the engaged factor's — derived by the
 * kernel's own omx_dyn_oversample_factor, never restated. A disabled slot is factor 1. */
#define OMX_CONTRACT_STAGE "keyed-gate"
/**
 * @brief The frames of latency the gate reports for `p`: OMX_OVS_LATENCY_4X while 4x is engaged, else 0.
 * @param p The resolved atom.
 * @return The latency, frames.
 * @post `latency-is-the-factor-s`: 0 or OMX_OVS_LATENCY_4X, nothing else.
 * @note RT-safe. Thread-safe: pure.
 */
static inline float omx_gate_latency(const struct omx_dyn *p) {
  const float frames = omx_dyn_oversample_factor(p) == 4u ? (float)OMX_OVS_LATENCY_4X : 0.0f;
  OMX_POST(frames == 0.0f || frames == (float)OMX_OVS_LATENCY_4X, "latency-is-the-factor-s");
  return frames;
}

/**
 * @brief The core: the host's block through fixed scratch, the kernel over each chunk. The key is
 *        optional: a NULL key, or `key_external` choosing SELF, is the ordinary self-detecting
 *        gate — the kernel's NULL key, bit for bit.
 * @param g The instance.
 * @param p The resolved atom.
 * @param key The sidechain block, or NULL (unconnected).
 * @param key_external The key-source control: >= 0.5 (or NULL) listens to `key`, else SELF.
 * @param in_l The left input block.
 * @param in_r The right input block.
 * @param out_l The left output block; may alias any input.
 * @param out_r The right output block; may alias any input.
 * @param n Frames.
 * @post `bypass-identity-l`, `bypass-identity-r`: a disabled gate is the identity.
 * @note RT-safe: no allocation, no lock. Thread-safe on distinct instances.
 */
static inline void omx_gate_run(struct omx_gate *g, const struct omx_dyn *p, const float *key,
                                const float *key_external, const float *in_l, const float *in_r,
                                float *out_l, float *out_r, uint32_t n) {
  const int keyed = key && omx_gate_clampf(key_external, 0.0f, 1.0f, 1.0f) >= 0.5f;
  for (uint32_t off = 0; off < n;) {
    uint32_t m = n - off < OMX_GATE_CHUNK ? n - off : OMX_GATE_CHUNK;
    memcpy(g->sl, in_l + off, m * sizeof(float));
    memcpy(g->sr, in_r + off, m * sizeof(float));
    if (keyed) memcpy(g->sk, key + off, m * sizeof(float));
    omx_dynamics_keyed(g->sl, g->sr, keyed ? g->sk : NULL, m, p, &g->st);
    /* Disabled is the identity. Checked on the scratch against the input BEFORE the copy-out,
     * so it still means something when the host aliases an output onto its input. */
    OMX_POST(p->enabled || !memcmp(g->sl, in_l + off, m * sizeof(float)), "bypass-identity-l");
    OMX_POST(p->enabled || !memcmp(g->sr, in_r + off, m * sizeof(float)), "bypass-identity-r");
    memcpy(out_l + off, g->sl, m * sizeof(float));
    memcpy(out_r + off, g->sr, m * sizeof(float));
    off += m;
  }
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_GATE_H */
