// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_gate.h
 * @brief The keyed gate's DSP: its controls resolved to the dynamics atom inside the declared
 *        travel, its latency, and a host's block run through fixed scratch by the one dynamics
 *        kernel.
 *
 * Moved unchanged from the OpenMixer keyed gate plugin:
 * the processing half of the LV2 face, without the face. The port enum, the descriptor and the
 * stereo I/O binding stay with the plugin; a plugin hands this header the pointers its ports were
 * connected to, NULL for an unconnected one.
 *
 * Every sample of gain is computed by `omx_dynamics_keyed` (omx_dyn.h) — the SAME kernel the
 * console's gate slot runs, with the SAME resolved parameters the console pushes: ACT-BELOW mode,
 * peak detector, unity make-up, detector oversampling on `auto`. A second transcription of the
 * gain computer is how two gates would quietly stop being the same gate, so there is none.
 *
 * ## The key filter
 *
 * The contract's gate declares `hpHz` and `lpHz` (GATE_KEY_LIMITS, omx-contract 2.7.0): the two
 * edges of the detector's key filter, each 0 (off) up to 20 kHz. They filter what the detector
 * listens to and nothing else; the audio the gate attenuates never passes through them. The
 * filter is the console's: one section per edge, 12 dB per octave, Butterworth (Q = 1/sqrt 2),
 * designed for the gate's rate by omx_eq_design (the cookbook high-pass, the matched-Z low-pass,
 * a corner above 0.999 Nyquist held there), the high-pass first. Each section runs in double
 * (omx_biquad_d): a 20 Hz corner at 192 kHz puts the poles within 5e-4 of the unit circle, where
 * a float section misses its closed form by most of a dB. An edge at 0 is out; with both out the
 * gate is the gate that shipped before them, bit for bit.
 *
 * The key the filter runs on is the external key when the gate listens to one, and otherwise
 * the gate's own input as the console's self key builds it, the mono sum `(l + r) / 2`. A gate
 * with both edges out and no external key keeps the kernel's own self detection (the peak of
 * either leg), as it always has.
 *
 * `omx_gate_resolve` leaves HOLD, HYSTERESIS and the KNEE at their neutral values (0, 0, a hard
 * knee); the kernel runs all three (`omx_dyn.hold_frames`, `hyst_db`, and `gc.knee_db` with
 * `knee_shift_db` set by {@link omx_gate_knee}), and fx/omx_gate_instance.h is the face that sets
 * them from the contract's controls (omx-contract#21).
 *
 * ## The knee range
 *
 * The contract's gate declares `kneeStartDb` and `kneeEndDb` beside `thresholdDb`: the desk draws
 * the gate's transfer curve with the gain fully closed at one edge, fully open at the other and a
 * ramp between, and the threshold is a handle of its own. In the kernel the threshold stays the
 * corner of the expander's two lines (`(ratio - 1) * (level - threshold)` below, 0 dB above) and
 * the knee range is the stretch of input level over which that corner is rounded: a quadratic
 * Bezier from the lower edge to the upper one whose control point is the corner, C1 with both
 * lines (omx_gaincomp_db_shifted). The edges are read low to high whatever their order. A
 * threshold inside the range keeps the range where the operator put it; one outside it moves the
 * range, whole, until the threshold sits on its nearer edge, which rounds that side not at all. A
 * zero-width range (start == end, wherever it sits) is the hard knee, the gate that shipped, bit
 * for bit. A range centred on the threshold (the desk's default, -43..-37 around -40) is
 * omx_gaincomp_db's own centred knee of that width.
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
 * memcpy/memset/expf. The key filter's design (omx_eq_design's cos, sin, exp, sqrt) runs in
 * `omx_gate_resolve`, and only for an edge that moved. Chunking the host's block through fixed scratch changes nothing observable:
 * the kernel's state is continuous across calls.
 *
 * Not in the omxdsp.h umbrella, as omx_dyn.h is not. Include it by name.
 */
#ifndef OMX_GATE_H
#define OMX_GATE_H

#include <stdint.h>
#include <string.h>

#include "omx_biquad.h"
#include "omx_contract.h"
#include <omxcontract/omx_contract_limits.h>
#include "omx_dyn.h"
#include "omx_eq_design.h"
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
  const float *hp_hz;        /**< the key filter's high-pass corner, Hz; 0 (and unconnected) is out */
  const float *lp_hz;        /**< the key filter's low-pass corner, Hz; 0 (and unconnected) is out */
};

/** @brief The key filter: each edge's corner as resolved (0 is out), its section and its history. */
struct omx_gate_key {
  float hp_hz;    /**< the high-pass corner the section was designed at, Hz; 0 is out */
  float lp_hz;    /**< the low-pass corner the section was designed at, Hz; 0 is out */
  double hp_c[5]; /**< the high-pass section, `{b0, b1, b2, a1, a2}` */
  double lp_c[5]; /**< the low-pass section, `{b0, b1, b2, a1, a2}` */
  double hp_s[4]; /**< the high-pass history */
  double lp_s[4]; /**< the low-pass history */
};

/** @brief One gate instance: its rate, the dynamics slot's state and the block scratch. */
struct omx_gate {
  float rate;                 /**< the sample rate the gate is armed at */
  struct omx_dyn_state st;    /**< the dynamics slot's state */
  struct omx_gate_key key;    /**< the detector's key filter */
  float sl[OMX_GATE_CHUNK];   /**< the left scratch */
  float sr[OMX_GATE_CHUNK];   /**< the right scratch */
  float sk[OMX_GATE_CHUNK];   /**< the key scratch */
};

/**
 * @brief Arm a gate at `rate` (instantiate) or re-arm it (activate): the slot's state cleared, base path,
 *        the key filter out until a resolve designs it at this rate.
 * @param g The instance.
 * @param rate The host's sample rate, Hz.
 * @note Not RT-safe only in that it clears the whole instance's state; no allocation.
 */
static inline void omx_gate_init(struct omx_gate *g, float rate) {
  g->rate = rate;
  omx_dyn_state_init(&g->st, 1u);
  memset(&g->key, 0, sizeof g->key);
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

#define OMX_CONTRACT_STAGE "keyed-gate/key-filter"
/**
 * @brief The key filter's edges designed for `rate`: each edge whose corner moved is redesigned,
 *        the console's section (omx_eq_design at Q 1/sqrt 2, the cookbook high-pass and the
 *        matched-Z low-pass); an edge at 0 is out. An edge that comes in, or goes out, starts its
 *        history from silence; a corner that moves keeps it, as the console's pushed coefficients do.
 * @param k The key filter.
 * @param rate The gate's rate, Hz, positive while an edge is in.
 * @param hp_hz The high-pass corner, Hz, inside GATE_KEY_LIMITS.hpHz; 0 is out.
 * @param lp_hz The low-pass corner, Hz, inside GATE_KEY_LIMITS.lpHz; 0 is out.
 * @post `edges-inside-the-travel`.
 * @note RT-safe: `cos`, `sin`, `exp`, `sqrt` (omx_eq_design) only for an edge that moved.
 *       Thread-safe on distinct filters.
 */
static inline void omx_gate_key_design(struct omx_gate_key *k, float rate, float hp_hz, float lp_hz) {
  if (hp_hz != k->hp_hz) {
    if (hp_hz > 0.0f) omx_eq_design(OMX_EQ_HIGHPASS, hp_hz, M_SQRT1_2, 0.0, rate, k->hp_c);
    if (!(hp_hz > 0.0f) || !(k->hp_hz > 0.0f)) memset(k->hp_s, 0, sizeof k->hp_s);
    k->hp_hz = hp_hz;
  }
  if (lp_hz != k->lp_hz) {
    if (lp_hz > 0.0f) omx_eq_design(OMX_EQ_LOWPASS, lp_hz, M_SQRT1_2, 0.0, rate, k->lp_c);
    if (!(lp_hz > 0.0f) || !(k->lp_hz > 0.0f)) memset(k->lp_s, 0, sizeof k->lp_s);
    k->lp_hz = lp_hz;
  }
  OMX_POST(k->hp_hz >= OMX_GATE_KEY_HP_HZ_MIN && k->hp_hz <= OMX_GATE_KEY_HP_HZ_MAX &&
               k->lp_hz >= OMX_GATE_KEY_LP_HZ_MIN && k->lp_hz <= OMX_GATE_KEY_LP_HZ_MAX,
           "edges-inside-the-travel");
}

/**
 * @brief Whether the key filter has an edge in.
 * @param k The key filter.
 * @return 1 while either corner is above 0, else 0.
 * @note RT-safe. Thread-safe: pure.
 */
static inline int omx_gate_key_active(const struct omx_gate_key *k) {
  return k->hp_hz > 0.0f || k->lp_hz > 0.0f;
}

/**
 * @brief A key block through the filter IN PLACE: the high-pass, then the low-pass, each only
 *        while it is in. With both out the block is untouched.
 * @param k The key filter; its history advances.
 * @param buf The key block, filtered in place.
 * @param n Frames.
 * @pre `finite-key`.
 * @post `finite-filtered-key`.
 * @note RT-safe: O(n), no allocation, no call but omx_biquad_d(). Thread-safe on distinct filters.
 */
static inline void omx_gate_key_run(struct omx_gate_key *k, float *buf, uint32_t n) {
  OMX_PRE(omx_block_finite(buf, n), "finite-key");
  if (k->hp_hz > 0.0f)
    for (uint32_t i = 0; i < n; i++) buf[i] = omx_biquad_d(buf[i], k->hp_c, k->hp_s);
  if (k->lp_hz > 0.0f)
    for (uint32_t i = 0; i < n; i++) buf[i] = omx_biquad_d(buf[i], k->lp_c, k->lp_s);
  OMX_POST(omx_block_finite(buf, n), "finite-filtered-key");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The console's gateStateToNativeDyn, restated as control reads — the ONE resolution a gate
 *        slot gets. The `threshold` control is the declared gate threshold travel; the knee is
 *        hard here, and {@link omx_gate_knee} sets the contract's knee range on the result. The
 *        key filter's edges, read inside GATE_KEY_LIMITS (unconnected or NaN is out, no come-up
 *        value being declared), are designed into the instance by {@link omx_gate_key_design}.
 * @param g The instance (its rate read, its key filter written).
 * @param c The controls.
 * @param p The atom, written whole.
 * @note RT-safe: the key filter is designed only when an edge moves. Thread-safe on distinct
 *       instances.
 */
static inline void omx_gate_resolve(struct omx_gate *g, const struct omx_gate_controls *c, struct omx_dyn *p) {
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
  omx_gate_key_design(&g->key, g->rate,
                      omx_gate_clampf(c->hp_hz, OMX_GATE_KEY_HP_HZ_MIN, OMX_GATE_KEY_HP_HZ_MAX, OMX_GATE_KEY_HP_HZ_MIN),
                      omx_gate_clampf(c->lp_hz, OMX_GATE_KEY_LP_HZ_MIN, OMX_GATE_KEY_LP_HZ_MAX, OMX_GATE_KEY_LP_HZ_MIN));
}

#define OMX_CONTRACT_STAGE "keyed-gate/knee"
/**
 * @brief The knee range into a resolved atom: `gc.knee_db` the range's width and `knee_shift_db`
 *        its centre's offset from `gc.thresh_db`, moved whole so the threshold stays inside it.
 *        `knee_start_db == knee_end_db` is the hard knee (width 0, offset 0) wherever they sit.
 * @param p The atom, resolved by omx_gate_resolve (its threshold read here).
 * @param knee_start_db One edge of the range, dB, finite.
 * @param knee_end_db The other edge, dB, finite.
 * @post `knee-not-negative`, `shift-inside-the-knee`.
 * @note RT-safe. Thread-safe: pure in its arguments.
 */
static inline void omx_gate_knee(struct omx_dyn *p, float knee_start_db, float knee_end_db) {
  const float lo = knee_start_db < knee_end_db ? knee_start_db : knee_end_db;
  const float hi = knee_start_db < knee_end_db ? knee_end_db : knee_start_db;
  const float width = hi - lo;
  const float half = 0.5f * width;
  float shift = width > 0.0f ? (lo + half) - p->gc.thresh_db : 0.0f;
  shift = shift < -half ? -half : shift > half ? half : shift;
  p->gc.knee_db = width;
  p->knee_shift_db = shift;
  OMX_POST(p->gc.knee_db >= 0.0f, "knee-not-negative");
  OMX_POST(p->knee_shift_db >= -0.5f * p->gc.knee_db && p->knee_shift_db <= 0.5f * p->gc.knee_db,
           "shift-inside-the-knee");
}
#undef OMX_CONTRACT_STAGE

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
 *        gate — the kernel's NULL key, bit for bit, while the key filter is out. With an edge in,
 *        the detector listens to the filtered key: the external one, or the self key `(l + r) / 2`.
 *        A disabled gate leaves the key filter's history as it was, as it leaves the slot's.
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
  const int filtered = omx_gate_key_active(&g->key);
  for (uint32_t off = 0; off < n;) {
    uint32_t m = n - off < OMX_GATE_CHUNK ? n - off : OMX_GATE_CHUNK;
    memcpy(g->sl, in_l + off, m * sizeof(float));
    memcpy(g->sr, in_r + off, m * sizeof(float));
    if (keyed) memcpy(g->sk, key + off, m * sizeof(float));
    else if (filtered)
      for (uint32_t i = 0; i < m; i++) g->sk[i] = 0.5f * (g->sl[i] + g->sr[i]);
    if (filtered && p->enabled) omx_gate_key_run(&g->key, g->sk, m);
    omx_dynamics_keyed(g->sl, g->sr, keyed || filtered ? g->sk : NULL, m, p, &g->st);
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
