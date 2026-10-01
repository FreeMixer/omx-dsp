// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_param.h
 * @brief The parameter clamp: a port, atom or knob value brought into its declared travel, with
 *        ONE defined answer for NaN and ±Inf per word.
 *
 * A site picks its word by one question: is the floor a safe resting value for this parameter?
 *  - yes: omx_clampf() — a NaN carries no position and reads as the FLOOR; ±Inf are ordered and
 *    saturate (−Inf to the floor, +Inf to the ceiling);
 *  - no:  omx_clamp_or() — every non-finite value reads as the parameter's declared neutral value
 *    (a symmetric feedback travel's floor is its deepest negative feedback, an EQ gain's floor its
 *    deepest cut: "a non-finite feedback is no feedback", "a non-finite gain is no gain").
 * omx_unit() is omx_clampf() over [0, 1]. A finite value inside the travel passes bit for bit.
 *
 * Design: docs/design/specs/2026-09-26-dsp-primitives.md §1 row 23.
 */
#ifndef OMX_PARAM_H
#define OMX_PARAM_H

#include "omx_contract.h"

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "param/clamp"

/**
 * @brief `v` clamped to [lo, hi]; a NaN reads as `lo`, −Inf as `lo`, +Inf as `hi`.
 * @param v The value a host, an atom or a knob carries — any float, NaN and ±Inf included.
 * @param lo The floor of the declared travel, finite.
 * @param hi The ceiling of the declared travel, finite, `>= lo`.
 * @return A value inside [lo, hi]: `v` itself when it is already inside.
 * @pre `travel-is-ordered`.
 * @post `inside-the-travel`.
 * @note RT-safe and thread-safe: two compares, no state. `v >= lo` is false for a NaN, which is
 *       the whole NaN law.
 */
static inline float omx_clampf(float v, float lo, float hi) {
  OMX_PRE(lo <= hi, "travel-is-ordered");
  const float c = v >= lo ? (v <= hi ? v : hi) : lo;
  OMX_POST(c >= lo && c <= hi, "inside-the-travel");
  return c;
}

/**
 * @brief `v` clamped to [lo, hi]; every non-finite value (NaN, ±Inf) reads as `dflt`.
 * @param v The value a host, an atom or a knob carries — any float.
 * @param lo The floor of the declared travel, finite.
 * @param hi The ceiling of the declared travel, finite, `>= lo`.
 * @param dflt The parameter's declared neutral value, inside [lo, hi].
 * @return `dflt` for a non-finite `v`, else `v` clamped to [lo, hi].
 * @pre `default-inside-the-travel`.
 * @post `inside-the-travel`.
 * @note RT-safe and thread-safe: one finiteness test (`v − v == 0` is false for NaN and ±Inf)
 *       and omx_clampf().
 */
static inline float omx_clamp_or(float v, float lo, float hi, float dflt) {
  OMX_PRE(lo <= hi && dflt >= lo && dflt <= hi, "default-inside-the-travel");
  const float c = (v - v == 0.0f) ? omx_clampf(v, lo, hi) : dflt;
  OMX_POST(c >= lo && c <= hi, "inside-the-travel");
  return c;
}

/**
 * @brief A unit knob: omx_clampf() over [0, 1] — NaN and −Inf read as 0, +Inf as 1.
 * @param v The value — any float.
 * @return A value inside [0, 1].
 * @note RT-safe and thread-safe: omx_clampf().
 */
static inline float omx_unit(float v) { return omx_clampf(v, 0.0f, 1.0f); }

#undef OMX_CONTRACT_STAGE

#endif /* OMX_PARAM_H */
