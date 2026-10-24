// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_doppler.h
 * @brief The Doppler read head: a read on a fractional delay line whose delay moves at a constant
 *        rate, so the signal comes out detuned with no pitch shifter, re-centred by a crossfade.
 *
 * A read whose delay changes by `s` samples per sample plays the line back at `1 − s` times its
 * speed: a sine of frequency `f` comes out at `f·(1 − s)`, a detune of `1200·log2(1 − s)` cents. For
 * a detune of `c` cents the slope is `s = 1 − 2^(c/1200)`: negative (the delay shrinks) to tune up,
 * positive to tune down.
 *
 * The head sweeps a window of `W` samples centred on the mean delay `D`, its position `p` running
 * from 0 to `W` at `|s|` per sample, the delay `D + sign(s)·(p − W/2)`. When `p` passes `(1 − F)·W` a
 * second head starts at position 0 and the output crossfades to it linearly over the last `F·W` of
 * the sweep (the weights `1 − q/(F·W)` and `q/(F·W)`, `q` the new head's position, sum to 1); when the
 * old head reaches `W` the new one carries on alone. Outside the fade one head is read alone, at
 * weight 1, so there the output is exactly the delay line read at the moving delay and its frequency
 * exactly `f·2^(c/1200)`. A head at 0 cents does not move: it sits at the window's centre and the
 * output is the line read at `D`, bit for bit.
 *
 * Contract: no allocation, no lock, no libm call per sample; the design's `pow` runs on the control
 * thread. The state is one float.
 */
#ifndef OMX_DOPPLER_H
#define OMX_DOPPLER_H

#include <math.h>
#include <stdint.h>

#include "omx_contract.h"
#include "omx_fdelay.h"

/** @brief A Doppler head's design: its window, fade, step and direction. */
struct omx_doppler {
  float window; /**< The sweep, samples, positive. */
  float fade;   /**< The crossfade's share of the sweep, in (0, 0.5]. */
  float step;   /**< `|s|`, samples of delay per sample, in [0, 1). */
  float sign;   /**< −1 while the delay shrinks (tuned up), +1 while it grows (tuned down). */
};

/** @brief A Doppler head's state: the position of the head being read, samples into the window. */
struct omx_doppler_state {
  double pos; /**< In [0, window). Double: a float position near a window of thousands of samples
               *   rounds a step of a few ten-thousandths to the wrong slope. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "doppler/design"
/**
 * @brief Design a head for a detune of `cents` over a sweep of `window` samples, `fade` of it
 *        crossfaded.
 * @param d Receives the design.
 * @param cents The detune, cents; positive tunes up.
 * @param window The sweep, samples, positive.
 * @param fade The crossfade's share of the sweep, in (0, 0.5].
 * @pre `window-positive`, `fade-in-range`, `detune-under-an-octave`.
 * @post `step-below-one`.
 * @note Control thread: one pow. Thread-safe: writes only `d`.
 */
static inline void omx_doppler_design(struct omx_doppler *d, double cents, float window, float fade) {
  OMX_PRE(window > 0.0f, "window-positive");
  OMX_PRE(fade > 0.0f && fade <= 0.5f, "fade-in-range");
  OMX_PRE(fabs(cents) < 1200.0, "detune-under-an-octave");
  const double s = 1.0 - pow(2.0, cents / 1200.0);
  d->window = window;
  d->fade = fade;
  d->step = (float)fabs(s);
  d->sign = s < 0.0 ? -1.0f : 1.0f;
  OMX_POST(d->step >= 0.0f && d->step < 1.0f, "step-below-one");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief Put a head at the window's centre, where a head that does not move reads the mean delay.
 * @param d The design.
 * @param st The state.
 * @note RT-safe. Thread-safe on distinct state.
 */
static inline void omx_doppler_start(const struct omx_doppler *d, struct omx_doppler_state *st) {
  st->pos = 0.5 * (double)d->window;
}

/**
 * @brief The delay a head at position `p` reads, samples: `D + sign·(p − W/2)`.
 * @param d The design.
 * @param mean The mean delay `D`, samples.
 * @param p The head's position, samples into the window.
 * @return The delay, samples.
 * @note RT-safe. Thread-safe: pure.
 */
static inline float omx_doppler_delay(const struct omx_doppler *d, float mean, double p) {
  return (float)((double)mean + (double)d->sign * (p - 0.5 * (double)d->window));
}

#define OMX_CONTRACT_STAGE "doppler/read"
/**
 * @brief Read one sample through the head and advance it.
 * @param line The delay line, written with the current sample already.
 * @param d The design.
 * @param st The state.
 * @param mean The mean delay `D`, samples; `D ± W/2` must lie inside the line's reach.
 * @return The sample.
 * @pre `delay-window-inside-the-line`.
 * @post `position-inside-the-window`.
 * @note RT-safe: one or two fractional reads, no libm call. Thread-safe on distinct state.
 */
static inline float omx_doppler_read(const struct omx_fdelay *line, const struct omx_doppler *d,
                                     struct omx_doppler_state *st, float mean) {
  OMX_PRE(mean - 0.5f * d->window >= omx_fdelay_min_delay(line->order) &&
              mean + 0.5f * d->window <= omx_fdelay_max_delay(line->cap, line->order),
          "delay-window-inside-the-line");
  const double w = d->window, start = (1.0 - (double)d->fade) * w;
  const double p = st->pos;
  float y = omx_fdelay_read(line, omx_doppler_delay(d, mean, p));
  if (p >= start) {
    const double q = p - start;
    const float g = (float)(q / ((double)d->fade * w));
    const float yn = omx_fdelay_read(line, omx_doppler_delay(d, mean, q));
    y = y + g * (yn - y);
  }
  double np = p + (double)d->step;
  if (np >= w) np -= start;
  st->pos = np;
  OMX_POST(st->pos >= 0.0 && st->pos < w, "position-inside-the-window");
  return y;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_DOPPLER_H */
