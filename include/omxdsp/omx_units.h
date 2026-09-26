// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_units.h
 * @brief Decibel and linear-amplitude conversions.
 *
 * Design: docs/design/specs/2026-09-26-dsp-primitives.md §1 row 17.
 */
#ifndef OMX_UNITS_H
#define OMX_UNITS_H

#include <math.h>

/**
 * @brief Decibels to linear amplitude, `10^(dB/20)`.
 * @param db Level, dB; any finite value.
 * @return The linear amplitude: 0 dB is 1, −6 dB is about 0.501.
 * @note RT-safe: one `powf`, on the RT-safe allowlist. Thread-safe: pure.
 */
static inline float omx_db_to_lin(float db) {
  return powf(10.0f, db * 0.05f);
}

/**
 * @brief Linear amplitude to decibels, floored so silence stays finite.
 * @param lin Amplitude, linear; values below 1e-9 read as 1e-9.
 * @return `20·log10(max(lin, 1e-9))`: silence is −180 dB, never −inf or NaN.
 * @note RT-safe: one `log10f`, on the RT-safe allowlist. Thread-safe: pure.
 */
static inline float omx_lin_to_db(float lin) {
  const float floor_lin = 1e-9f;
  return 20.0f * log10f(lin < floor_lin ? floor_lin : lin);
}

#endif /* OMX_UNITS_H */
