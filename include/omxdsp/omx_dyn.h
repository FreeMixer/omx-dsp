// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_dyn.h
 * @brief The dynamics atom, its detector parameters and the dynamics slot's kernel: what a comp,
 *        a gate, a band-dyn band or a de-esser resolves its controls to, the one derivation of
 *        the envelope's parameters from it, and `omx_dynamics_keyed`, which applies it.
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/mix_dsp.h (omx-dsp-dev#32, #34):
 * the band-dyn and the de-esser kernels derive their detector from the atom here and nowhere else,
 * so the engine's slot and a kernel cannot read the attack and release poles differently; the
 * strip's gate and comp slots and the keyed gate's plugin run the one kernel here. Pure; the 4x
 * control path calls the archive's omx_oversampler.
 *
 * Not in the omxdsp.h umbrella, as the fx/ headers are not: it depends on omx_envelope.h, which
 * includes the umbrella ahead of its own definitions. Include it by name.
 */
#ifndef OMX_DYN_H
#define OMX_DYN_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "omx_contract.h"
#include "omx_envelope.h"
#include "omx_gaincomp.h"
#include "omx_lookahead.h"
#include "omx_oversampler.h"

/*
 * DETECTOR OVERSAMPLING — the contract's `detectorOversampling`, resolved into the RT atom.
 *
 * The cascade above took the worst in-band aliased product from −49.7 to −59.9 dBc and there it
 * stops: its floor is the attack/release direction switch, which no filtering behind it can
 * pass. The ONE thing measured to cross the −90 dBc line while leaving attack, release and gain
 * reduction bit-identical is running the whole CONTROL chain — rectifier, envelope, log, curve,
 * exp — at 4× and decimating the GAIN back, with the audio never resampled at all
 * (docs/design/notes/2026-09-14-oversampling-vs-192k.md §9a/§10).
 *
 * It is expensive, so it is a per-instance choice rather than a law, and `auto` — the default —
 * spends it only where the measurement says it is needed: below
 * {@link OMX_DYN_OVS_AUTO_MS} of attack. A strip with an ordinary attack pays nothing.
 */
/** @brief `auto`: 4× only while the attack is faster than {@link OMX_DYN_OVS_AUTO_MS}. */
#define OMX_DYN_OVS_AUTO 0
/** @brief `off`: never oversample, whatever the attack — the cascade alone. */
#define OMX_DYN_OVS_OFF 1
/** @brief `4x`: always oversample, even at an attack slow enough not to need it. */
#define OMX_DYN_OVS_X4 2

/** @brief Where `auto` engages. 0.5 ms, operator ruling 2026-09-14: the measured defect is a
 * FAST-attack defect, and by 0.5 ms the control path is already band-limited enough that the
 * fold has nothing loud to fold. */
#define OMX_DYN_OVS_AUTO_MS 0.5f

/* The derivation that reads it, `omx_dyn_oversample_factor`, sits with the struct below. */

/** @brief One resolved dynamics atom: a per-block snapshot of the operator's controls. */
struct omx_dyn {
  int enabled;         /* 0 → the atom is a no-op (the whole slot bypassed) */
  struct omx_gaincomp_params gc; /* the gain computer's mode, threshold, ratio, knee, range, make-up */
  int detect;          /* OMX_DETECT_PEAK | OMX_DETECT_RMS */
  float attack_coeff;  /* one-pole coeff when the detector RISES (0 = instant, →1 = slow) */
  float release_coeff; /* one-pole coeff when the detector FALLS */
  int ovs_mode;        /* OMX_DYN_OVS_AUTO | _OFF | _X4 — the operator's choice, not a factor */
  /* The attack in MILLISECONDS, the operator's own number. `attack_coeff` is the derivation of
   * it (and of the rate); this is the value `auto` compares against a millisecond threshold,
   * which a coefficient cannot answer without knowing the rate it was made at. */
  float attack_ms;
};

/**
 * @brief The detector parameters of a dynamics atom: its attack and release poles and its domain.
 * @param p The atom.
 * @return `{attack_coeff, release_coeff, detect}` as the envelope's parameters, copied unchanged.
 * @note RT-safe: three loads. Thread-safe: pure.
 */
static inline struct omx_env_params omx_dyn_env_params(const struct omx_dyn *p) {
  const struct omx_env_params e = {p->attack_coeff, p->release_coeff, p->detect};
  return e;
}

#define OMX_CONTRACT_STAGE "dsp/dyn-factor"
/**
 * @brief The 4x control path's factor the atom engages: 1 or 4.
 *
 * THE ENGAGED STATE IS DERIVED, NEVER STORED. One function, so the RT loop, the readback the
 * UI renders and anything that reports the stage's latency cannot answer differently — the
 * defect this codebase keeps re-buying is two readers of one fact. A disabled slot is never
 * oversampled: it is a no-op, and a no-op has no detector.
 * @param p The atom.
 * @return 4 while the 4x control path is engaged, else 1.
 * @post `bypass-identity`: the factor is 1 or 4, the two the compensation ring can carry.
 * @note RT-safe: a few compares. Thread-safe: pure.
 */
static inline uint32_t omx_dyn_oversample_factor(const struct omx_dyn *p) {
  uint32_t f;
  if (!p->enabled || p->ovs_mode == OMX_DYN_OVS_OFF) f = 1u;
  else if (p->ovs_mode == OMX_DYN_OVS_X4) f = 4u;
  else f = (p->attack_ms > 0.0f && p->attack_ms < OMX_DYN_OVS_AUTO_MS) ? 4u : 1u;
  /* The compensation delay ring is sized by OMX_OVS_LATENCY_4X, so a factor this derivation can
   * return but the ring cannot carry would be a read tap past the end of the line. Two values is
   * the whole range, and saying so here is what makes that a checked fact rather than a reading
   * of three branches. */
  OMX_POST(f == 1u || f == 4u, "bypass-identity");
  return f;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief ONE dynamics slot's persistent RT state — what the kernel remembers across buffers.
 *
 * It is a struct because the detector holds two things a float cannot: the envelope CASCADE
 * ({@link OMX_DYN_ENV_STAGES}), and — when the 4× control path is engaged — a rate changer with
 * history, plus the delay line that keeps the audio lined up with a gain that arrives
 * {@link OMX_OVS_LATENCY_4X} samples late.
 *
 * PLAIN INLINE FLOATS, nothing to allocate: a lane embeds two of these and the RT callback
 * never touches an allocator. The oversamplers are carried whether or not they run, because
 * the alternative is allocating one the moment an operator drags an attack time past 0.5 ms.
 *
 * THE DELAY LINE IS ALWAYS FED, even at factor 1 where the read tap is 0 and the output is
 * therefore bit-identical to a kernel with no delay line at all. Feeding it costs a store and
 * an index; NOT feeding it would mean engaging the 4× path into 72 samples of silence, which
 * is exactly the seam the crossfade exists to avoid having to hide.
 */
struct omx_dyn_state {
  struct omx_env env;              /* the detector cascade — level, so rate-independent */
  struct omx_oversampler ovs_a;    /* up for the key (or leg L), and down for the GAIN */
  struct omx_oversampler ovs_b;    /* up for leg R — unused on a mono or keyed slot */
  float delay_l[OMX_OVS_LATENCY_4X + 1u];
  float delay_r[OMX_OVS_LATENCY_4X + 1u];
  uint32_t delay_pos;
  uint32_t factor;                 /* the factor that ran LAST buffer, for the switch */
  uint32_t xfade_left;             /* samples of handover still owed */
  uint32_t xfade_tap;              /* the outgoing path's read tap */
  float xfade_gain;                /* the outgoing path's last gain, HELD across the handover */
  float last_gain;                 /* what to hold if the factor changes next buffer */
};

/**
 * @brief Samples of handover when the engaged factor changes.
 *
 * 1024 at 96 kHz is 10.7 ms, and the length was MEASURED, not chosen. Engaging moves the read
 * tap by OMX_OVS_LATENCY_4X, so the handover crossfades between two copies of the audio 72
 * samples apart — a comb, briefly. Worst sample-to-sample step during the switch, against the
 * step the same tone shows with no switch at all (`mix_dyn_ovs.test.c`): 256 → 0.0136 vs
 * 0.0080, 512 → 0.0095, **1024 → 0.0080, exactly the unswitched figure**, 2048 → no further
 * gain. So 1024 is where the discontinuity disappears into the signal and anything longer only
 * lengthens the comb. What it does NOT remove is the brief level dip that crossfading two
 * offset copies produces; that is measured and reported, not hidden.
 */
#define OMX_DYN_XFADE 1024u

/** @brief Base-rate frames the 4× path handles per pass. Bounds its scratch; nothing observable
 * depends on it, which the same oracle proves by running several quanta. */
#define OMX_DYN_OVS_CHUNK 64u

/**
 * @brief Clear one slot's state and arm its rate changers for `factor`.
 * @param st The slot's state.
 * @param factor The factor the slot starts at: 1 (base path) or 4.
 * @note RT-safe: a memset and two oversampler inits, no allocation. Thread-safe on distinct state.
 */
static inline void omx_dyn_state_init(struct omx_dyn_state *st, uint32_t factor) {
  memset(st, 0, sizeof *st);
  omx_oversampler_init(&st->ovs_a, factor);
  omx_oversampler_init(&st->ovs_b, factor);
  st->factor = factor;
  st->last_gain = 1.0f;
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "dynamics"
/*
 * CONTRACT (omx_contract.h). The dynamics slot is a TIME-VARYING GAIN: it multiplies the block
 * by a per-sample coefficient derived from a detector, and nothing else. So its laws are about
 * that coefficient's SIGN and BOUND, not about the waveform:
 *
 *  - BELOW mode (gate/expander) never ADDS gain — `omx_gaincomp_db` is <= 0 there, so the
 *    output's peak can never exceed the input's. A gate that makes a signal louder is the
 *    defect this token names.
 *  - ABOVE mode (comp/limiter) never adds more than the declared make-up.
 *  - The parameters are inside the limits the contract row declares (ratio >= 1, knee >= 0,
 *    range <= 0), which is a claim about the CONTROL thread, checked here because here is where
 *    a violation becomes audible.
 *
 * The peak laws need the input peak, which this function overwrites in place, so they are stated
 * by the contract battery around the call. What is checked HERE is what only here can see.
 */
/**
 * @brief Apply one dynamics slot to a strip's leg(s) IN PLACE, with the detector listening to `key`.
 *
 * `l` is always driven; `r` may be NULL (mono — one leg) or a second leg (stereo). ONE gain is
 * applied to both legs, so a stereo strip's image never shifts under gain reduction.
 *
 * THE KEY (docs/design/specs/2026-09-08-keyed-gates.md). `key` is the MONO signal the detector
 * measures, already filtered by whatever the key filter is set to — the caller's copy, never a
 * leg of the audio. `key == NULL` is the ordinary self-detecting slot and is the ONLY shape the
 * comp slot and an un-keyed gate ever take: the detector then tracks the max across the provided
 * legs, exactly as it always has, sample for sample and bit for bit. A keyed slot gates the
 * channel's OWN audio while opening on somebody else's level, which is the whole feature.
 *
 * `st` is the slot's persistent RT state ({@link omx_dyn_state}): the detector CASCADE, each
 * stage smoothed with the attack coeff while it rises and the release coeff while it falls, so
 * attack/release shape the gain response for BOTH comp (level up → more reduction) and gate
 * (level up → gate opens); plus the rate changers and the compensation delay the 4× control
 * path needs. The detector LEVEL the gain computer reads is the cascade's LAST stage
 * ({@link omx_env_level}), which is also what the GR meter publishes — one derivation, one
 * reader. A disabled slot is a no-op that leaves the signal AND the state untouched.
 * RT-safe: O(n), no allocation, bounded scratch whatever the quantum.
 *
 * NOTE (RT cost, lane kernel-cost-dynamics 2026-09-27): the gain computer is dB-domain and runs
 * the libm pair inside `omx_gaincomp_gain` (measured cheaper than the unvectorised `_poly` pair). Each
 * chunk is three loops — rectify (branch-free per shape), the cascade (the only serial one), the
 * gain — and at factor 1 with no handover owed the ring read is the sample just written, so the
 * chunk applies `x·g` directly and refills only the ring's last `ring` inputs: bit-identical to
 * the per-sample ring walk, which the 4× path and a handover still take.
 * @param l The first leg, processed in place.
 * @param r The second leg, processed in place, or NULL (mono).
 * @param key The detector's mono key, or NULL (the slot detects on its own legs).
 * @param n Frames.
 * @param p The atom.
 * @param st The slot's state.
 * @pre `finite-in-l`, `finite-in-r`, `finite-in-key`: every input block is finite; and once the
 *      slot is enabled, `ratio-at-least-one`, `knee-not-negative`, `range-is-an-attenuation`,
 *      `makeup-positive`.
 * @post `finite-out-l`, `finite-out-r`; `gain-finite-nonneg` and `no-gain-added`: the last gain
 *       applied is finite, non-negative and at most the make-up.
 * @invariant `envelope-finite`: the detector cascade stays finite.
 * @note RT-safe: O(n), no allocation, bounded scratch. Thread-safe on distinct state.
 */
static inline void omx_dynamics_keyed(float *l, float *r, const float *key, uint32_t n,
                                      const struct omx_dyn *p, struct omx_dyn_state *st) {
  OMX_PRE(omx_block_finite(l, n), "finite-in-l");
  OMX_PRE(omx_block_finite(r, n), "finite-in-r");
  /* The KEY is a third input block and it was the one with no precondition until 2026-09-17: it
   * is the caller's own filtered copy of somebody ELSE's audio (docs/design/specs/
   * 2026-09-08-keyed-gates.md), so it arrives from a different strip's scratch through a biquad
   * pair this stage does not own. A NaN there is permanent — it enters the envelope cascade,
   * which is recursive. `omx_block_finite` answers 1 for the NULL an un-keyed slot passes. */
  OMX_PRE(omx_block_finite(key, n), "finite-in-key");
  /* R-056 finding, 2026-09-17 (rt_alloc.test.c's real-render corpus, the first build to run
   * on_process under OMX_CONTRACTS): these four preconditions are about what the stage does
   * WHEN IT RUNS, not about a disabled slot -- resolve_dyn's own `if (!o->enabled) return;`
   * leaves ratio/knee/range/makeup at whatever the struct held before (0 on first use), so a
   * check ahead of the enable gate reads a disabled slot as "ratio-at-least-one"/"makeup-positive"
   * violations — the single most common state a real desk is in. The checks run only once the
   * function is actually going to process, matching omx_drive_process's own
   * enabled-gates-the-config-checks shape below. */
  if (!p->enabled || n == 0) return;
  OMX_PRE(p->gc.ratio >= 1.0f, "ratio-at-least-one");
  OMX_PRE(p->gc.knee_db >= 0.0f, "knee-not-negative");
  OMX_PRE(p->gc.mode != OMX_DYN_BELOW || p->gc.range_db <= 0.0f, "range-is-an-attenuation");
  OMX_PRE(p->gc.makeup_lin > 0.0f, "makeup-positive");

  /* THE SWITCH. `auto` is derived every buffer, so an operator dragging the attack across
   * OMX_DYN_OVS_AUTO_MS lands here. The incoming path's rate changers are re-armed (their
   * history belongs to a rate they are no longer running at) and a handover is owed: the read
   * tap moves by the oversampler's latency, which is a discontinuity in the AUDIO and not
   * merely in the gain. The outgoing gain is HELD for the handover rather than kept running —
   * a second detector chain for 2.7 ms would be a second compressor to keep honest. */
  const uint32_t want = omx_dyn_oversample_factor(p);
  if (want != st->factor) {
    /* factor 0 is COLD, not a change: a lane's RT state is memset to zero at node build, and a
     * fresh slot owes no handover from a path that never ran. Crossfading out of it would fade
     * in from the zeroed `last_gain`, i.e. silence. `mix_lane.test.c`'s shared-kernel vector is
     * what holds this. */
    if (st->factor != 0u) {
      st->xfade_tap = omx_oversampler_latency_for(st->factor);
      st->xfade_gain = st->last_gain;
      st->xfade_left = OMX_DYN_XFADE;
    }
    omx_oversampler_init(&st->ovs_a, want);
    omx_oversampler_init(&st->ovs_b, want);
    st->factor = want;
  }
  const uint32_t tap = omx_oversampler_latency_for(st->factor);
  const uint32_t ring = OMX_OVS_LATENCY_4X + 1u;
  const int rms = (p->detect == OMX_DETECT_RMS);

  const struct omx_env_params ep = omx_dyn_env_params(p);
  float ac, rc;
  omx_env_stage_poles(&ep, st->factor, &ac, &rc);

  uint32_t done = 0u;
  while (done < n) {
    const uint32_t m = (n - done > OMX_DYN_OVS_CHUNK) ? OMX_DYN_OVS_CHUNK : (n - done);
    float gain[OMX_DYN_OVS_CHUNK];

    if (st->factor == 1u) {
      /* THE BASE PATH: the detector reads the legs (or the key) sample for sample at the graph's
       * own rate. The cascade is bit-identical to the one that shipped; rectify, cascade and gain
       * are three loops so only the cascade is serial. */
      float lev[OMX_DYN_OVS_CHUNK];
      const float *kl = key ? key + done : l + done;
      const float *kr = key ? key + done : (r ? r + done : l + done);
      if (rms)
        for (uint32_t i = 0; i < m; i++) {
          const float sl = kl[i] * kl[i], sr = kr[i] * kr[i];
          lev[i] = sl > sr ? sl : sr;
        }
      else
        for (uint32_t i = 0; i < m; i++) {
          const float al = fabsf(kl[i]), ar = fabsf(kr[i]);
          lev[i] = al > ar ? al : ar;
        }
      for (uint32_t i = 0; i < m; i++) lev[i] = omx_env_step(&st->env, &ep, lev[i], ac, rc);
      for (uint32_t i = 0; i < m; i++) gain[i] = omx_gaincomp_gain(&p->gc, lev[i]);
    } else {
      /* THE 4× CONTROL PATH. The audio is NOT resampled — only the signal the detector looks
       * at goes up, and only the GAIN comes back down. A stereo slot sends BOTH legs up and
       * takes the max there, because max() is itself a nonlinearity: taking it at 96 kHz would
       * fold before the oversampling could prevent it, which is the whole point of the row. */
      const uint32_t f = st->factor, mf = m * f;
      float ua[OMX_DYN_OVS_CHUNK * 4u], ub[OMX_DYN_OVS_CHUNK * 4u], g4[OMX_DYN_OVS_CHUNK * 4u];
      const float *src_a = key ? key + done : l + done;
      omx_oversampler_up(&st->ovs_a, src_a, m, ua);
      const int two = (!key && r) ? 1 : 0;
      if (two) omx_oversampler_up(&st->ovs_b, r + done, m, ub);
      /* Rectify, cascade, gain — the same three loops as the base path, over the oversampled
       * block. A mono or keyed slot reads leg A twice, which is the max it always took. */
      const float *vb = two ? ub : ua;
      if (rms)
        for (uint32_t k = 0; k < mf; k++) {
          const float sa = ua[k] * ua[k], sb = vb[k] * vb[k];
          g4[k] = sa > sb ? sa : sb;
        }
      else
        for (uint32_t k = 0; k < mf; k++) {
          const float aa = fabsf(ua[k]), bb = fabsf(vb[k]);
          g4[k] = aa > bb ? aa : bb;
        }
      for (uint32_t k = 0; k < mf; k++) g4[k] = omx_env_step(&st->env, &ep, g4[k], ac, rc);
      for (uint32_t k = 0; k < mf; k++) g4[k] = omx_gaincomp_gain(&p->gc, g4[k]);
      omx_oversampler_down(&st->ovs_a, g4, m, gain);
      /* D-1, 2026-09-17 (docs/design/notes/2026-09-17-dynamics-math-review.md, measured by
       * mix_dyn_math.test.c at all four rates): a half-band RINGS on a step, and the step a hard
       * gate makes when it re-opens is the steepest this stage can produce — four poles of 0.1 ms
       * attack take the gain from the range floor to unity inside a handful of oversampled
       * samples. The gain the COMPUTER produced was in [0, makeup_lin]; the gain the decimator
       * handed back was not. Measured on the unclamped path, gate at ratio 40 / range -80 dB /
       * attack 0.1 ms: the applied gain reached 1.0244 / 1.0748 / 1.0819 / 1.0440 at 44.1 / 48 /
       * 96 / 192 kHz — up to +0.68 dB of gain ADDED by a GATE, which is the defect the stage's
       * own contract token names — and -0.0482 / -0.0686 / -0.0802 / -0.0098, a POLARITY FLIP on
       * the samples either side of the opening.
       *
       * The clamp is not a smoothing: it is `omx_gaincomp_gain`'s own postcondition restated at the
       * one place a filter can break it. It cannot fire at factor 1, which never filters the
       * gain, so the base path stays bit-identical to the cascade that shipped before any of
       * this. Two compares per base sample. */
      for (uint32_t i = 0; i < m; i++) {
        if (!(gain[i] >= 0.0f)) gain[i] = 0.0f; /* `!(x >= 0)` also catches a NaN */
        else if (gain[i] > p->gc.makeup_lin) gain[i] = p->gc.makeup_lin;
      }
    }

    if (tap == 0u && st->xfade_left == 0u) {
      /* Factor 1, no handover owed: every read is the sample just written, so the ring only has
       * to hold the chunk's last `ring` inputs for a later switch — fed BEFORE the legs are
       * overwritten, then the gain applied in place. Bit-identical to the walk below. */
      const uint32_t keep = m < ring ? m : ring;
      uint32_t w = omx_lookahead_fwd(st->delay_pos, (m - keep) % ring, ring);
      for (uint32_t i = m - keep; i < m; i++) {
        st->delay_l[w] = l[done + i];
        st->delay_r[w] = r ? r[done + i] : l[done + i];
        w = omx_lookahead_fwd(w, 1u, ring);
      }
      st->delay_pos = w;
      for (uint32_t i = 0; i < m; i++) l[done + i] *= gain[i];
      if (r)
        for (uint32_t i = 0; i < m; i++) r[done + i] *= gain[i];
    } else {
      for (uint32_t i = 0; i < m; i++) {
        const float xl = l[done + i];
        const float xr = r ? r[done + i] : xl;
        st->delay_l[st->delay_pos] = xl;
        st->delay_r[st->delay_pos] = xr;
        const uint32_t rd = omx_lookahead_back(st->delay_pos, tap, ring);
        float gl = st->delay_l[rd] * gain[i];
        float gr = st->delay_r[rd] * gain[i];
        if (st->xfade_left > 0u) {
          const float a = 1.0f - (float)st->xfade_left / (float)OMX_DYN_XFADE;
          const uint32_t xt = st->xfade_tap;
          const uint32_t ord = omx_lookahead_back(st->delay_pos, xt, ring);
          gl = a * gl + (1.0f - a) * st->delay_l[ord] * st->xfade_gain;
          gr = a * gr + (1.0f - a) * st->delay_r[ord] * st->xfade_gain;
          st->xfade_left--;
        }
        st->delay_pos = omx_lookahead_fwd(st->delay_pos, 1u, ring);
        l[done + i] = gl;
        if (r) r[done + i] = gr;
      }
    }
    st->last_gain = gain[m - 1u];
    done += m;
  }
  OMX_POST(omx_block_finite(l, n), "finite-out-l");
  OMX_POST(omx_block_finite(r, n), "finite-out-r");
  OMX_POST(st->last_gain >= 0.0f && st->last_gain - st->last_gain == 0.0f, "gain-finite-nonneg");
  /* The gain that actually reached the audio is inside the gain computer's own range — the law
   * D-1's clamp restores on the 4x path and that the base path has always kept. */
  OMX_POST(st->last_gain <= p->gc.makeup_lin || p->gc.range_db > 0.0f, "no-gain-added");
  OMX_INVARIANT(omx_block_finite(st->env.stage, (uint32_t)OMX_DYN_ENV_STAGES), "envelope-finite");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The same slot detecting on its OWN signal — the shape every comp slot and every un-keyed
 *        gate takes. ONE kernel, no named variant: a key is a pointer, and its absence is a fact.
 * @param l The first leg, processed in place.
 * @param r The second leg, processed in place, or NULL (mono).
 * @param n Frames.
 * @param p The atom.
 * @param st The slot's state.
 * @note RT-safe and thread-safe as omx_dynamics_keyed().
 */
static inline void omx_dynamics(float *l, float *r, uint32_t n,
                                const struct omx_dyn *p, struct omx_dyn_state *st) {
  omx_dynamics_keyed(l, r, NULL, n, p, st);
}

#endif /* OMX_DYN_H */
