// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_matched_pair.h
 * @brief One conjugate pair placed by the matched-Z map `z = exp(sT)`, the primitive every
 *        matched biquad design is built from; the C twin of core's `matchedPair` (eq.ts).
 *
 * Design: docs/design/specs/2026-09-26-dsp-primitives.md §1 row 3 and Appendix A (why the
 * polynomial at `z = 1` is returned factored, and why the real-root branch divides).
 */
#ifndef OMX_MATCHED_PAIR_H
#define OMX_MATCHED_PAIR_H

#include <math.h>

#include "omx_contract.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief A pole or zero pair `1 + p1·z⁻¹ + p2·z⁻²` and its value at `z = 1`.
 */
typedef struct {
  double p1; /**< The z⁻¹ coefficient. */
  double p2; /**< The z⁻² coefficient: |z|² of the pair, inside the unit circle. */
  double dc; /**< `1 + p1 + p2`, computed factored so a low-frequency pair keeps its digits. */
} OmxMatchedPair;

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "matched/pair"
/**
 * @brief Place one pair by the matched-Z map at normalised angular frequency `wn` with damping `zeta`.
 * @param wn Angular frequency, radians per sample, positive; held at or below π.
 * @param zeta Damping ratio, positive; below 1 a complex pair, at or above 1 two real roots.
 * @return The pair and its factored value at `z = 1`.
 * @pre `positive-finite-w`, `positive-finite-damping`.
 * @post `pair-inside-unit-circle`: `0 <= p2 < 1` and `dc > 0`.
 * @note RT-safe: `exp`, `cos`, `sin`, `sqrt` on the RT-safe allowlist. Thread-safe: pure.
 */
static inline OmxMatchedPair omx_matched_pair(double wn, double zeta) {
  OMX_PRE(wn > 0.0 && wn - wn == 0.0, "positive-finite-w");
  OMX_PRE(zeta > 0.0 && zeta - zeta == 0.0, "positive-finite-damping");
  const double w = wn > M_PI ? M_PI : wn;
  const double e = exp(-zeta * w);
  OmxMatchedPair out;
  if (zeta < 1.0) {
    const double th = sqrt(1.0 - zeta * zeta) * w;
    const double ec = e * cos(th);
    const double es = e * sin(th);
    out.p1 = -2.0 * ec;
    out.p2 = e * e;
    out.dc = (1.0 - ec) * (1.0 - ec) + es * es;
    OMX_POST(out.p2 >= 0.0 && out.p2 < 1.0 && out.dc > 0.0, "pair-inside-unit-circle");
    return out;
  }
  {
    const double sq = sqrt(zeta * zeta - 1.0);
    const double z1 = exp(-w / (zeta + sq));
    const double z2 = exp(-w * (zeta + sq));
    out.p1 = -(z1 + z2);
    out.p2 = z1 * z2;
    out.dc = (1.0 - z1) * (1.0 - z2);
    OMX_POST(out.p2 >= 0.0 && out.p2 < 1.0 && out.dc > 0.0, "pair-inside-unit-circle");
    return out;
  }
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The corner clamp every matched design shares: `[1 Hz, 0.999·Nyquist]`.
 * @param freq_hz Requested corner, Hz.
 * @param sr Sample rate, Hz.
 * @return The clamped corner, Hz.
 * @note RT-safe and thread-safe: two compares.
 */
static inline double omx_matched_f0(double freq_hz, double sr) {
  const double nyquist = sr * 0.5;
  double f0 = freq_hz;
  if (!(f0 > 1.0)) f0 = 1.0;
  if (f0 > nyquist * 0.999) f0 = nyquist * 0.999;
  return f0;
}

/**
 * @brief The clamped corner as a normalised angular frequency, `2π·f0/sr`.
 * @param freq_hz Requested corner, Hz.
 * @param sr Sample rate, Hz.
 * @return Radians per sample.
 * @note RT-safe and thread-safe: pure.
 */
static inline double omx_matched_w0(double freq_hz, double sr) {
  return 2.0 * M_PI * omx_matched_f0(freq_hz, sr) / sr;
}

/**
 * @brief Write a section as the `{b0, b1, b2, a1, a2}` tuple omx_biquad() runs.
 * @param out The five floats.
 * @param b0 Numerator coefficient.
 * @param b1 Numerator coefficient.
 * @param b2 Numerator coefficient.
 * @param p The pole pair.
 * @note RT-safe and thread-safe: five narrowing stores.
 */
static inline void omx_matched_section(float out[5], double b0, double b1, double b2,
                                       OmxMatchedPair p) {
  out[0] = (float)b0;
  out[1] = (float)b1;
  out[2] = (float)b2;
  out[3] = (float)p.p1;
  out[4] = (float)p.p2;
}

/** @brief A fitted numerator `b0 + b1·z⁻¹ + b2·z⁻²`, before it is narrowed into a section. */
typedef struct {
  double b0; /**< Numerator coefficient. */
  double b1; /**< Numerator coefficient. */
  double b2; /**< Numerator coefficient. */
} OmxMatchedNumerator;

#define OMX_CONTRACT_STAGE "matched/fit-numerator"
/**
 * @brief Fit the numerator of a design whose prototype zeros sit at `s = ∞` (Vicanek).
 *
 * The squared magnitude of the numerator equals the target magnitude-squared times the pole
 * pair's squared magnitude at DC, the corner and Nyquist. The operations and their order are
 * the contract: both callers are bit-identical to the lowpass design in core through it.
 * @param u The pole pair's squared magnitude at `z = 1`, positive.
 * @param n_nyq The target squared magnitude at Nyquist, finite.
 * @param n_f0 The target squared magnitude at the corner, finite.
 * @param s0 sin-squared of half the corner, inside (0, 1).
 * @return The three numerator coefficients.
 * @pre `positive-finite-dc`, `finite-targets`, `corner-inside-the-band`.
 * @post `finite-numerator`.
 * @note RT-safe: `sqrt` on the RT-safe allowlist. Thread-safe: pure.
 */
static inline OmxMatchedNumerator omx_matched_fit_numerator(double u, double n_nyq, double n_f0,
                                                            double s0) {
  OMX_PRE(u > 0.0 && u - u == 0.0, "positive-finite-dc");
  OMX_PRE(n_nyq - n_nyq == 0.0 && n_f0 - n_f0 == 0.0, "finite-targets");
  OMX_PRE(s0 > 0.0 && s0 < 1.0, "corner-inside-the-band");
  const double r1 = n_nyq - u;
  const double r2 = n_f0 - u;
  const double w = (r2 - s0 * r1) / (16.0 * s0 * (s0 - 1.0));
  const double n1 = sqrt(u);
  const double nm = sqrt(u + r1 > 0.0 ? u + r1 : 0.0);
  const double sum = (n1 + nm) / 2.0;
  OmxMatchedNumerator out;
  out.b1 = (n1 - nm) / 2.0;
  const double disc = sum * sum - 4.0 * w;
  out.b0 = (sum + sqrt(disc > 0.0 ? disc : 0.0)) / 2.0;
  out.b2 = sum - out.b0;
  OMX_POST(out.b0 - out.b0 == 0.0 && out.b1 - out.b1 == 0.0 && out.b2 - out.b2 == 0.0,
           "finite-numerator");
  return out;
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief A matched pole pair over a matched zero pair, unity at DC: the shape a peaking bell and
 *        both shelves share.
 * @param out The five floats omx_biquad() runs.
 * @param wp Pole angular frequency, radians per sample.
 * @param zp Pole damping, positive.
 * @param wz Zero angular frequency, radians per sample.
 * @param zz Zero damping, positive.
 * @note RT-safe and thread-safe: two omx_matched_pair() calls and a division.
 */
static inline void omx_matched_pair_section(float out[5], double wp, double zp, double wz,
                                            double zz) {
  const OmxMatchedPair p = omx_matched_pair(wp, zp);
  const OmxMatchedPair z = omx_matched_pair(wz, zz);
  const double k = p.dc / z.dc;
  omx_matched_section(out, k, k * z.p1, k * z.p2, p);
}

#endif /* OMX_MATCHED_PAIR_H */
