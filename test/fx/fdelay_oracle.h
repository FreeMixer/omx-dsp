// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * fdelay_oracle.h — the fractional delay line's response, derived BY A DIFFERENT ALGORITHM, for
 * the oracles of the stages built on it (mix_chorus.test.c, mix_flanger.test.c).
 *
 * TEST SUPPORT ONLY: no production translation unit includes this, and it is deliberately not a
 * kernel. It exists so the two modulation oracles stand on ONE derivation of the primitive's
 * response instead of two copies that can drift apart — the same reason `mix_fdelay.h` has one
 * read body behind its two doors.
 *
 * `mix_fdelay.h` builds the interpolation as a product of Lagrange basis coefficients. This file
 * builds the same unique polynomial by NEWTON'S DIVIDED DIFFERENCES over the complex exponential
 * samples themselves and never forms a coefficient at all — the derivation
 * `mix_fdelay.test.c`'s own oracle uses, and the reason a stage oracle standing on it is not
 * comparing the kernel with itself.
 */
#ifndef OMX_TEST_FDELAY_ORACLE_H
#define OMX_TEST_FDELAY_ORACLE_H

#include <complex.h>
#include <math.h>

#include <omxdsp/omx_fdelay.h>

/**
 * The line's response to `e^{jwn}` at a delay of `d` samples: the taps of an order-N interpolator
 * sit at distances `id + off + 1 − k` for k = 0..N, so their values under a complex exponential
 * are `e^{-jw·distance}`; Newton's divided differences through those N+1 points, evaluated at the
 * read point, IS the interpolated sample. The ideal answer would be `e^{-jwd}`, and the
 * difference between the two is what the fdelay spec's §2 table measures.
 *
 * `d` is taken as the line's CLAMPED delay — the caller passes what `omx_fdelay_latency` would
 * answer, which for every control range in the modulation specs is the request itself.
 */
static double complex omx_oracle_fdelay(int order, double d, double w) {
  const int off = (order - 1) / 2;
  const long id = (long)floor(d);
  const double fr = d - (double)id;
  double x[OMX_FDELAY_MAX_TAPS];
  double complex v[OMX_FDELAY_MAX_TAPS];
  for (int k = 0; k <= order; k++) {
    x[k] = (double)k;
    v[k] = cexp(-I * w * (double)(id + off + 1 - k));
  }
  for (int j = 1; j <= order; j++)
    for (int k = order; k >= j; k--) v[k] = (v[k] - v[k - 1]) / (x[k] - x[k - j]);
  const double X = (double)off + (1.0 - fr); /* the read point in the kernel's own coordinate */
  double complex acc = v[order];
  for (int k = order - 1; k >= 0; k--) acc = acc * (X - x[k]) + v[k];
  return acc;
}

/** The DFT of a captured impulse response at angular frequency `w` — the transfer function of a
 *  FROZEN (inc = 0) modulation stage, which is linear and time-invariant while its oscillator
 *  does not move. Exact at any frequency, unlike a windowed tone measurement. */
static double complex omx_oracle_dft(const float *h, unsigned n, double w) {
  double complex acc = 0.0;
  for (unsigned i = 0; i < n; i++) acc += (double)h[i] * cexp(-I * w * (double)i);
  return acc;
}

#endif /* OMX_TEST_FDELAY_ORACLE_H */
