// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * sweep.test.c — the Farina sweep and its deconvolution (<omxdsp/analysis/omx_sweep.h>) against
 * closed forms, at the nine RME rates, with core's default fade fraction from the parity table.
 * A half-second sweep from 40 Hz to 0.45 of the rate at every rate:
 *
 *   1. the length is round(T·rate); L = T/ln(f1/f0) for the realised T; between the fades every
 *      sample is sin(K·(e^{t/L} − 1)), K = 2π·f0·L, recomputed here (1e-9); both ends are 0;
 *   2. the identity: the sweep deconvolved by its own inverse is exactly 1 at the lag N − 1, the
 *      normalisation's closed form (1e-12), and the largest sample more than a millisecond from
 *      it stays under −40 dB of it;
 *   3. a room `δ(t) + 0.5·δ(t − 5 ms) − 0.25·δ(t − 12 ms)` recorded through the sweep comes back
 *      out of the deconvolution at the pre-roll with those three levels at those three lags
 *      (within 0.02);
 *   4. the Farina property: a memoryless `y = x + 0.3·x²` puts its second harmonic L·ln 2 samples
 *      before the linear response; the strongest sample in that window lies within 0.5 % of it;
 *      the delay of order 1 is 0 and of an order under 1 is not a number;
 *   5. the FFT convolution equals the direct sum, `[1, 2, 3] ⊛ [1, 1] = [1, 3, 5, 3]` and two seeded
 *      signals of 300 and 77 samples, within 1e-12; an empty operand is empty;
 *   6. a sweep that cannot exist (f1 at Nyquist, f1 under f0, a length that is not the rate's)
 *      refuses and writes nothing.
 *
 * The kernels run between room_count_begin() and room_count_end(): they allocate nothing.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/analysis/omx_sweep.h>

#include "analysis_rates.h"
#include "fixtures/room_ts.h"
#include "room_oracle.h"

static void arm_rate(double rate) {
  const double f0 = 40.0, f1 = 0.45 * rate, dur = 0.5;
  const uint32_t n = omx_sweep_length(rate, dur);
  g_arm = "sweep";
  check(n == (uint32_t)round(dur * rate), "the length is round(T·rate)", n, round(dur * rate));
  const double realised = (double)n / rate;
  const double fade = TS_DEFAULT_FADE_FRACTION * realised;
  double *stim = room_alloc(n * sizeof(double)), *inv = room_alloc(n * sizeof(double)), L = 0.0;
  room_count_begin();
  const int ok = omx_sweep_generate(rate, f0, f1, dur, fade, fade, stim, inv, n, &L);
  room_count_end();
  check(ok == 1, "a sweep is generated", ok, 1);
  check(fabs(L - realised / log(f1 / f0)) < 1e-15, "L = T / ln(f1/f0)", L, realised / log(f1 / f0));
  const double K = 2.0 * M_PI * f0 * L;
  const uint32_t nf = (uint32_t)round(fade * rate);
  double worst = 0.0;
  for (uint32_t i = nf; i < n - nf; i++) worst = fmax(worst, fabs(stim[i] - sin(K * (exp(((double)i / rate) / L) - 1.0))));
  check(worst < 1e-9, "between the fades, sin(K·(e^{t/L} − 1))", worst, 0);
  check(stim[0] == 0.0 && fabs(stim[n - 1u]) < 1e-15, "both ends at zero", stim[n - 1u], 0);

  /* The identity. */
  g_arm = "identity";
  const uint32_t len = n + 2u;
  double *scratch = room_alloc(4u * (size_t)omx_sweep_convolve_size(len + 1200u, n) * sizeof(double));
  const uint32_t d1 = (uint32_t)round(0.005 * rate), d2 = (uint32_t)round(0.012 * rate);
  const uint32_t irn = 2048u + d2;
  double *ir = room_alloc(irn * sizeof(double));
  room_count_begin();
  omx_sweep_recover_ir(stim, n, inv, n, 1024u, ir, 2048u, scratch);
  room_count_end();
  check(fabs(ir[1024] - 1.0) < 1e-12, "1 at the lag N − 1", ir[1024], 1.0);
  const uint32_t ms = (uint32_t)ceil(rate / 1000.0);
  double side = 0.0;
  for (uint32_t i = 0; i < 2048u; i++)
    if (i + ms < 1024u || i > 1024u + ms) side = fmax(side, fabs(ir[i]));
  check(20.0 * log10(side) < -40.0, "a millisecond off it, under −40 dB", 20.0 * log10(side), -40.0);

  /* A room of three arrivals. */
  g_arm = "room";
  const uint32_t rl = n + d2;
  double *resp = room_alloc(rl * sizeof(double));
  for (uint32_t i = 0; i < n; i++) {
    resp[i] += stim[i];
    resp[i + d1] += 0.5 * stim[i];
    resp[i + d2] -= 0.25 * stim[i];
  }
  free(scratch);
  scratch = room_alloc(4u * (size_t)omx_sweep_convolve_size(rl, n) * sizeof(double));
  const uint32_t pre = 64u;
  omx_sweep_recover_ir(resp, rl, inv, n, pre, ir, irn, scratch);
  check(fabs(ir[pre] - 1.0) < 0.02, "the direct arrival at the pre-roll, 1", ir[pre], 1.0);
  check(fabs(ir[pre + d1] - 0.5) < 0.02, "+5 ms, 0.5", ir[pre + d1], 0.5);
  check(fabs(ir[pre + d2] + 0.25) < 0.02, "+12 ms, −0.25", ir[pre + d2], -0.25);
  check(fabs(ir[0]) < 0.02, "nothing before the pre-roll's start", ir[0], 0.0);

  /* The Farina property. */
  g_arm = "harmonic";
  for (uint32_t i = 0; i < n; i++) resp[i] = stim[i] + 0.3 * stim[i] * stim[i];
  for (uint32_t i = n; i < rl; i++) resp[i] = 0.0;
  const double dt2 = omx_sweep_harmonic_delay(L, rate, 2.0);
  check(fabs(dt2 - L * log(2.0) * rate) < 1e-9, "L·ln 2·rate", dt2, L * log(2.0) * rate);
  check(omx_sweep_harmonic_delay(L, rate, 1.0) == 0.0, "order 1 is the linear response", 0, 0);
  check(isnan(omx_sweep_harmonic_delay(L, rate, 0.5)), "an order under 1 is not a number", 0, 0);
  const uint32_t hpre = (uint32_t)ceil(1.6 * dt2) + 64u, span = hpre + 64u;
  double *h = room_alloc(span * sizeof(double));
  omx_sweep_recover_ir(resp, rl, inv, n, hpre, h, span, scratch);
  /* h[i] sits at lag N − 1 − hpre + i: the linear response at i0 = hpre, the harmonic dt2 before
   * it. The strongest sample from 1.5·dt2 to 0.5·dt2 before the linear response is the harmonic. */
  const double i0 = (double)hpre;
  double best = 0.0, at = 0.0;
  for (uint32_t i = (uint32_t)(i0 - 1.5 * dt2); i < (uint32_t)(i0 - 0.5 * dt2); i++)
    if (fabs(h[i]) > best) {
      best = fabs(h[i]);
      at = (double)i;
    }
  check(fabs((i0 - at) - dt2) / dt2 < 0.005, "the harmonic L·ln 2 before the linear response", i0 - at, dt2);
  check(best < fabs(h[(uint32_t)i0]), "and under it", best, fabs(h[(uint32_t)i0]));
  free(h);

  /* A sweep that cannot exist. */
  g_arm = "refusals";
  stim[5] = 42.0;
  check(omx_sweep_generate(rate, f0, rate / 2.0, dur, fade, fade, stim, inv, n, &L) == 0, "f1 at Nyquist", 0, 0);
  check(omx_sweep_generate(rate, f0, f0, dur, fade, fade, stim, inv, n, &L) == 0, "f1 at f0", 0, 0);
  check(omx_sweep_generate(rate, f0, f1, dur, fade, fade, stim, inv, n - 1u, &L) == 0, "a length not the rate's", 0, 0);
  check(stim[5] == 42.0, "and nothing is written", stim[5], 42.0);
  check(omx_sweep_length(rate, -1.0) == 0u && omx_sweep_length(rate, NAN) == 0u, "no length for a negative duration", 0, 0);

  free(stim); free(inv); free(scratch); free(ir); free(resp);
}

static void arm_convolve(void) {
  g_rate = 0.0;
  g_arm = "convolve";
  const double a[3] = {1.0, 2.0, 3.0}, b[2] = {1.0, 1.0}, want[4] = {1.0, 3.0, 5.0, 3.0};
  double scratch[4u * 4u];
  room_count_begin();
  const uint32_t len = omx_sweep_convolve(a, 3u, b, 2u, scratch);
  room_count_end();
  check(len == 4u, "length 3 + 2 − 1", len, 4);
  for (uint32_t i = 0; i < 4u; i++) check(fabs(scratch[i] - want[i]) < 1e-12, "[1, 3, 5, 3]", scratch[i], want[i]);
  check(omx_sweep_convolve(a, 0u, b, 2u, scratch) == 0u, "an empty operand is empty", 0, 0);

  RoomRng rng = {5u};
  double x[300], y[77], direct[376];
  for (int i = 0; i < 300; i++) x[i] = room_noise(&rng);
  for (int i = 0; i < 77; i++) y[i] = room_noise(&rng);
  memset(direct, 0, sizeof direct);
  for (int i = 0; i < 300; i++)
    for (int j = 0; j < 77; j++) direct[i + j] += x[i] * y[j];
  double *s = room_alloc(4u * (size_t)omx_sweep_convolve_size(300u, 77u) * sizeof(double));
  check(omx_sweep_convolve(x, 300u, y, 77u, s) == 376u, "length 376", 0, 0);
  double worst = 0.0;
  for (int i = 0; i < 376; i++) worst = fmax(worst, fabs(s[i] - direct[i]));
  check(worst < 1e-12, "the FFT convolution is the direct sum", worst, 0);
  free(s);
}

int main(void) {
  omx_analysis_require_rates();
  for (int r = 0; r < OMX_ANALYSIS_RATE_COUNT; r++) {
    g_rate = OMX_ANALYSIS_RATES[r];
    arm_rate(g_rate);
  }
  arm_convolve();
  return room_report("sweep");
}
