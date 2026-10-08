// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * log10f_cr.h — omx_log10f(), a correctly rounded float log10 in IEEE double arithmetic, for the
 * tests only. Production calls the libm's log10f; the golden builds substitute this function for
 * it (log10f_subst.h) so a golden digest is exact on every glibc (BUILDING.md, "Golden digests and
 * libm"). Nothing under include/ or src/ may define or call it (tools/log10f-guard.sh).
 */
#ifndef OMXDSP_TEST_LOG10F_CR_H
#define OMXDSP_TEST_LOG10F_CR_H

#include <math.h>
#include <stdint.h>
#include <string.h>

/**
 * @brief `log10(x)` correctly rounded, in IEEE double arithmetic only: the same bits on every libm.
 *
 * A libm's `log10f` is not one function: glibc 2.36's misrounds about a third of the limiter
 * golden's arguments by up to 2 ulp, glibc 2.41's and 2.43's round them all correctly (FreeMixer/omx-dsp#21).
 * Here the float splits into `2^e · m` with `m` in [√½, √2); `ln m = 2·atanh(t)`, `t = (m−1)/(m+1)`,
 * `|t| ≤ 0.1716`, summed to `t²⁵/25`; `log10 x = e·log10 2 + ln m · (1/ln 10)` with `log10 2` split so
 * `e·hi` is exact; one rounding to float at the end. Checked against glibc 2.43's correctly rounded
 * `log10f` on all 2³¹ non-negative floats (`make check-log10f`): one argument, `0x0efeee7a`, lies
 * 7.8e-10 ulp from a float midpoint, past what double arithmetic resolves, and is answered from its
 * exact value. Needs `FLT_EVAL_METHOD == 0` and no contraction (the build's `-ffp-contract=off`).
 * @param x Any float.
 * @return `log10(x)`: −inf at ±0, NaN below 0 or at NaN, +inf at +inf.
 * @note Test support only: production calls the libm's `log10f` (BUILDING.md, "Golden digests and libm").
 */
static inline float omx_log10f(float x) {
  uint32_t u;
  memcpy(&u, &x, sizeof u);
  if (u == 0x0efeee7au) return -0x1.d33a46p+4f; /* the hard case: exact −29.20172595977783351650… */
  if (!(x > 0.0f) || x == INFINITY) return x == 0.0f ? -INFINITY : x < 0.0f ? NAN : x + x;
  const double xd = (double)x;
  uint64_t b;
  memcpy(&b, &xd, sizeof b);
  int e = (int)(b >> 52) - 1023;
  b = (b & 0x000fffffffffffffull) | 0x3ff0000000000000ull;
  double m;
  memcpy(&m, &b, sizeof m);
  if (m > 0x1.6a09e667f3bcdp+0) {
    m *= 0.5;
    e++;
  }
  const double f = m - 1.0, t = f / (2.0 + f), z = t * t;
  double q = 1.0 / 25.0;
  for (int k = 11; k >= 0; k--) q = q * z + 1.0 / (double)(2 * k + 1);
  const double ln_m = 2.0 * t * q;
  return (float)((double)e * 0x1.3441350000000p-2 + ((double)e * 0x1.3ef3fde623e25p-31 + ln_m * 0x1.bcb7b1526e50ep-2));
}

#endif
