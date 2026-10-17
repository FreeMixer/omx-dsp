// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_gen.h
 * @brief The test-signal generators: an oscillator (sine, square, saw, triangle), white noise and
 *        pink noise, each behind a click-free level ramp.
 *
 * The noise RNG is a 32-bit xorshift (deterministic per seed, RT-cheap); pink is Paul Kellet's
 * refined 7-state filtered-white approximation (±0.05 dB to ~1/f from 9 Hz up at 44.1k/48k), far
 * cheaper than a Voss-McCartney octave bank and with no per-sample branching. A generator is
 * mono; a host duplicates the block to both legs of a stereo pair.
 *
 * The kinds and waveforms are omx-contract's generator kernel, GENERATOR_KINDS and WAVEFORMS, in
 * their surface order. Moved from the OpenMixer engine (mix_dsp.h) unchanged in behaviour:
 * test/golden/engine_kernels.sha256 holds its output to the engine's, bit for bit.
 */
#ifndef OMX_GEN_H
#define OMX_GEN_H

#include <math.h>
#include <stdint.h>

#include <omxdsp/omxdsp_definitions.h>

#include "omx_contract.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** @brief The oscillator kind, GENERATOR_KINDS' `oscillator`. */
#define OMX_GEN_OSC OMX_GENERATOR_KINDS_OSCILLATOR
/** @brief White noise, GENERATOR_KINDS' `white`. */
#define OMX_GEN_WHITE OMX_GENERATOR_KINDS_WHITE
/** @brief Pink noise, GENERATOR_KINDS' `pink`. */
#define OMX_GEN_PINK OMX_GENERATOR_KINDS_PINK

/** @brief A sine, WAVEFORMS' `sine`. */
#define OMX_WAVE_SINE OMX_WAVEFORMS_SINE
/** @brief A square, WAVEFORMS' `square`. */
#define OMX_WAVE_SQUARE OMX_WAVEFORMS_SQUARE
/** @brief A rising saw, WAVEFORMS' `saw`. */
#define OMX_WAVE_SAW OMX_WAVEFORMS_SAW
/** @brief A triangle, WAVEFORMS' `triangle`. */
#define OMX_WAVE_TRIANGLE OMX_WAVEFORMS_TRIANGLE

/**
 * @brief One generator's state: the oscillator phase (cycles, [0,1)), the noise RNG, the pink
 *        filter's 7 one-pole states, and the level ramp's currently-applied level.
 *
 * Owned by the audio thread; a control thread touches it only through omx_gen_init before the
 * generator runs.
 */
struct omx_gen_state {
  float phase;      /**< Oscillator phase in cycles, always in [0,1). */
  uint32_t rng;     /**< xorshift32 state; never 0 (0 is a fixed point). */
  float pink[7];    /**< Paul Kellet refined pink filter states b0..b6. */
  float cur_level;  /**< The currently-applied level, ramped toward the target per block. */
};

/**
 * @brief Reset a generator: phase 0, pink filter cleared, level ramp at 0 (so the first block fades
 *        in), RNG seeded.
 * @param s The state.
 * @param seed The RNG seed; 0 is coerced to 1, since xorshift's fixed point at 0 would render
 *        silence forever.
 * @note RT-safe. Thread-safe on distinct states.
 */
static inline void omx_gen_init(struct omx_gen_state *s, uint32_t seed) {
  s->phase = 0.0f;
  s->rng = seed ? seed : 1u;
  for (int i = 0; i < 7; i++) s->pink[i] = 0.0f;
  s->cur_level = 0.0f;
}

/**
 * @brief One uniform white-noise sample in [-1, 1) from a xorshift32 step.
 * @param rng The RNG word, advanced.
 * @return The sample.
 * @note RT-safe: three shifts. Thread-safe on distinct words.
 */
static inline float omx_gen_white(uint32_t *rng) {
  uint32_t x = *rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *rng = x;
  return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

/**
 * @brief One pink-noise sample from a white sample: Paul Kellet's refined 7-state filter.
 *
 * The raw filter output spans roughly ±5 for white in ±1, so it is scaled by 0.125 to stay inside
 * [-1, 1]: a test generator must never clip a strip on its own at level 1.
 * @param w The white sample.
 * @param b The filter's seven states, advanced.
 * @return The sample.
 * @note RT-safe: seven one-poles. Thread-safe on distinct states.
 */
static inline float omx_gen_pink(float w, float b[7]) {
  b[0] = 0.99886f * b[0] + w * 0.0555179f;
  b[1] = 0.99332f * b[1] + w * 0.0750759f;
  b[2] = 0.96900f * b[2] + w * 0.1538520f;
  b[3] = 0.86650f * b[3] + w * 0.3104856f;
  b[4] = 0.55000f * b[4] + w * 0.5329522f;
  b[5] = -0.7616f * b[5] - w * 0.0168980f;
  float pink = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
  b[6] = w * 0.115926f;
  return pink * 0.125f;
}

/**
 * @brief The oscillator's raw sample (amplitude [-1,1]) at `phase` for one of the OMX_WAVE_*
 *        waveforms.
 *
 * Phase-aligned so every waveform starts at a rising zero crossing like the sine (the triangle
 * rises to +1 at 1/4; the saw sweeps -1 -> +1 across the cycle).
 * @param waveform An OMX_WAVE_* index; any other value is the sine.
 * @param phase The phase, cycles, in [0, 1).
 * @return The sample.
 * @note RT-safe: one sinf at most. Thread-safe: pure.
 */
static inline float omx_gen_osc(int waveform, float phase) {
  switch (waveform) {
    case OMX_WAVE_SQUARE:
      return phase < 0.5f ? 1.0f : -1.0f;
    case OMX_WAVE_SAW:
      return 2.0f * phase - 1.0f;
    case OMX_WAVE_TRIANGLE:
      if (phase < 0.25f) return 4.0f * phase;
      if (phase < 0.75f) return 2.0f - 4.0f * phase;
      return 4.0f * phase - 4.0f;
    default: /* OMX_WAVE_SINE */
      return sinf(2.0f * (float)M_PI * phase);
  }
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "dsp/gen-fill"
/**
 * @brief Fill one mono block of generator output.
 *
 * The level is ramped linearly from `s->cur_level` to `level` across the block, so start, stop and
 * a live re-level never step the envelope. Frequency changes are phase-continuous by construction
 * (the phase accumulates, never resets).
 * @param s The state.
 * @param out The block, `n` samples.
 * @param n Its length; 0 writes nothing.
 * @param kind An OMX_GEN_* kind; any other value is the oscillator.
 * @param waveform The oscillator's OMX_WAVE_* waveform.
 * @param freq_hz The oscillator's frequency, clamped to [0, 0.45·sr]; NaN is 0.
 * @param level The target level, linear amplitude, clamped to [0, 1]; NaN is 0.
 * @param sr The sample rate, Hz.
 * @post `finite-out`: every kind is a bounded closed form times a ramped level in [0, 1].
 * @note RT-safe: O(n), no allocation. Thread-safe on distinct states.
 */
static inline void omx_gen_fill(struct omx_gen_state *s, float *out, uint32_t n, int kind,
                                int waveform, float freq_hz, float level, float sr) {
  if (n == 0) return;
  if (!(level > 0.0f)) level = 0.0f; /* also catches NaN */
  if (level > 1.0f) level = 1.0f;
  float lvl = s->cur_level;
  float step = (level - lvl) / (float)n;
  if (kind == OMX_GEN_WHITE) {
    for (uint32_t i = 0; i < n; i++) {
      out[i] = omx_gen_white(&s->rng) * lvl;
      lvl += step;
    }
  } else if (kind == OMX_GEN_PINK) {
    for (uint32_t i = 0; i < n; i++) {
      out[i] = omx_gen_pink(omx_gen_white(&s->rng), s->pink) * lvl;
      lvl += step;
    }
  } else {
    float inc = (sr > 0.0f && freq_hz > 0.0f) ? freq_hz / sr : 0.0f; /* NaN freq → 0 */
    if (inc > 0.45f) inc = 0.45f; /* keep well under Nyquist whatever the control said */
    float phase = s->phase;
    for (uint32_t i = 0; i < n; i++) {
      out[i] = omx_gen_osc(waveform, phase) * lvl;
      phase += inc;
      if (phase >= 1.0f) phase -= 1.0f;
      lvl += step;
    }
    s->phase = phase;
  }
  s->cur_level = level;
  OMX_POST(omx_block_finite(out, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_GEN_H */
