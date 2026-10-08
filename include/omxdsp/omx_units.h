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
#include <stdint.h>
#include <string.h>

#include "omx_contract.h"

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
 * @note RT-safe: one `log10f`, on the RT-safe allowlist (a pure function of its argument: no allocation,
 *       lock or errno write on a finite positive input). Thread-safe: pure.
 */
static inline float omx_lin_to_db(float lin) {
  const float floor_lin = 1e-9f;
  return 20.0f * log10f(lin < floor_lin ? floor_lin : lin);
}

/**
 * @brief Linear magnitude to decibels in double precision, floored at a caller-named level.
 *
 * The analysis-tier spelling: a detector that compares ratios of summed spectrum magnitudes
 * against dB gates reads them in double, and its floor is part of its decision law, so the floor
 * is a parameter rather than omx_lin_to_db()'s fixed 1e-9.
 * @param lin Magnitude, linear; values below `floor_lin` read as `floor_lin`.
 * @param floor_lin The smallest magnitude read, linear, > 0.
 * @return `20·log10(max(lin, floor_lin))`.
 * @note Not for a per-sample loop: one `log10`. Thread-safe: pure.
 */
static inline double omx_lin_to_db_d(double lin, double floor_lin) {
  return 20.0 * log10(lin > floor_lin ? lin : floor_lin);
}

/*
 * THE PER-SAMPLE PAIR (row 17, lane kernel-cost-dynamics, 2026-09-27). The same two maps with no
 * libm call, for a loop that converts every sample: the dynamics gain computer ran `log10f` and
 * `powf` per sample (per OVERSAMPLED sample at 4x), which made the dB round trip the most
 * expensive line of the stage. Every coefficient below is a closed form — the atanh series'
 * `2/(2k+1)`, the exponential's `ln2^k/k!` — so nothing here was fitted and nothing can drift
 * from a table somebody regenerated. Straight-line code: a compiler may vectorise a loop of them.
 */

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "units/lin-to-db-poly"
/**
 * @brief Linear amplitude to decibels without a libm call, floored like omx_lin_to_db().
 *
 * The float splits into `2^e · m` with `m` in [√½, √2); `ln m = 2·atanh(t)`, `t = (m−1)/(m+1)`,
 * `|t| ≤ 0.1716`, summed to `t⁹/9` (the first term left out is below 4e-10). Unity is exactly
 * 0 dB. Against omx_lin_to_db(): within 1e-4 dB over [1e-9, 1e4] (the omxdsp suite's units arm
 * measures it).
 * @param lin Amplitude, linear, finite; values below 1e-9 read as 1e-9.
 * @return `20·log10(max(lin, 1e-9))` to the declared tolerance: silence is −180 dB.
 * @post `finite`.
 * @note RT-safe: one divide, no call, no branch but the floor. Thread-safe: pure.
 */
static inline float omx_lin_to_db_poly(float lin) {
  const float x = lin < 1e-9f ? 1e-9f : lin;
  uint32_t u;
  memcpy(&u, &x, sizeof u);
  u += 0x3f800000u - 0x3f3504f3u; /* re-centre so the mantissa lands in [√½, √2) */
  const int32_t e = (int32_t)(u >> 23) - 127;
  u = (u & 0x007fffffu) + 0x3f3504f3u;
  float m;
  memcpy(&m, &u, sizeof m);
  const float t = (m - 1.0f) / (m + 1.0f), t2 = t * t;
  const float ln_m =
      t * (2.0f + t2 * (2.0f / 3.0f + t2 * (2.0f / 5.0f + t2 * (2.0f / 7.0f + t2 * (2.0f / 9.0f)))));
  const float db = 8.685889638065037f /* 20/ln 10 */ * ((float)e * 0.6931471805599453f + ln_m);
  OMX_POST(db - db == 0.0f, "finite");
  return db;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "units/db-to-lin-poly"
/**
 * @brief Decibels to linear amplitude without a libm call, `10^(dB/20)`.
 *
 * `y = dB·log2(10)/20` rounds to `n + f`, `|f| ≤ ½`; `2^f` is the degree-7 Taylor polynomial of
 * `e^(f·ln2)` (the first term left out is below 2e-8 relative) and `2^n` goes into the exponent
 * bits. `y` is held inside [−125, 127], so the result is always a normal float (−752 dB is the
 * quietest answer; a gain there is silence either way). 0 dB is EXACTLY 1 — a gain computer's
 * unity region stays bit-exact. Against omx_db_to_lin(): within 2e-6 relative over
 * [−300, +80] dB (the omxdsp suite's units arm measures it).
 * @param db Level, dB, finite.
 * @return The linear amplitude, to the declared tolerance.
 * @post `finite`.
 * @note RT-safe: no call, one float-to-int conversion. Thread-safe: pure.
 */
static inline float omx_db_to_lin_poly(float db) {
  /* The product in DOUBLE: rounded to float at −300 dB it alone would cost 1.3e-6 of the result
   * (measured 2.8e-6 in all, over the declared bound); `f` is taken before narrowing. */
  double y = (double)db * 0.16609640474436813; /* log2(10)/20 */
  y = y < -125.0 ? -125.0 : (y > 127.0 ? 127.0 : y);
  const int32_t n = (int32_t)(y + (y >= 0.0 ? 0.5 : -0.5)); /* nearest, half away from 0 */
  const float z = (float)(y - (double)n) * 0.6931471805599453f; /* f·ln2, |z| ≤ 0.347 */
  const float p =
      1.0f + z * (1.0f + z * (1.0f / 2.0f + z * (1.0f / 6.0f + z * (1.0f / 24.0f +
      z * (1.0f / 120.0f + z * (1.0f / 720.0f + z * (1.0f / 5040.0f)))))));
  const uint32_t bits = (uint32_t)(n + 127) << 23;
  float scale;
  memcpy(&scale, &bits, sizeof scale);
  const float g = p * scale;
  OMX_POST(g - g == 0.0f && g > 0.0f, "finite");
  return g;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_UNITS_H */
