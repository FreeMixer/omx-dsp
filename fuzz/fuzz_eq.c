// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: the EQ's design and run path (omx_eq_design.h, omx_biquad.h,
 * omx_magnitude.h): a bank of bands designed at a rate from engineering values, the head
 * pass filters, then a block through the cascade and the bank's magnitude.
 *
 * Input:  [0]     bit 0: the rate is any rate from 8 to 768 kHz (u16 over that span), else one
 *                 of the nine of the oracle grid; bit 1: the EQ is on; bit 2: the run is
 *                 stereo, else one leg
 *         [1..]   the rate (one byte or two), the head filters (on, slope, corner as a raw
 *                 double each), the band count (1..OMX_EQ_MAX_BANDS), each band's kind, switch,
 *                 corner (a raw double), Q (a raw double) and gain (a u16 over +-24 dB), then a
 *                 u8 block length (1..255) and the block's samples (fuzz_block, finite).
 *
 * The corner and the Q are ANY finite double, a negative or an astronomical one included (the
 * design clamps the corner into [1 Hz, 0.999 Nyquist] and reads a Q at or below 0 as 1e-3); a
 * non-finite one is not in the design's contract and is read as 1 kHz and Q 1.
 *
 * Properties, each an abort():
 *   - no contract the library states is broken (the ledger stays empty): every section the bank
 *     designs has finite coefficients and both poles inside the unit circle;
 *   - the same sections pass omx_biquad_stable() as the harness computes it from the
 *     coefficients, and the bank has at most as many sections as bands (a band that is off is
 *     not in it, a band at 0 dB gain is parked);
 *   - the cascade, over finite samples, gives finite samples on both legs, and a bank with no live
 *     section leaves the block as it was, bit for bit;
 *   - the bank's magnitude (dB) is finite at any frequency, and a bank with no live section is
 *     0 dB there;
 *   - the head filters make at most OMX_EQ_PASS_MAX_SECTIONS sections, each stable;
 *   - the width-to-Q conversions give a finite positive Q for any finite width above 0.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_magnitude.h>

#include "fuzz.h"

#define BLOCK_MAX 255u

static double fin_or(double x, double d) { return isfinite(x) ? x : d; }

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  struct fuzz_in in = fuzz_in(data, size);
  const uint8_t flags = fuzz_u8(&in);
  const double rate = (double)((flags & 1u) ? fuzz_range(&in, 8000.0f, 768000.0f) : fuzz_rate(&in));
  fuzz_ledger_reset();

  /* a braced initialiser reads its words in no fixed order: each field is its own statement */
  struct omx_eq_pass hpf, lpf;
  hpf.on = fuzz_u8(&in) & 1;
  hpf.freq_hz = fin_or(fuzz_f64(&in), 1000.0);
  hpf.slope_24 = fuzz_u8(&in) & 1;
  lpf.on = fuzz_u8(&in) & 1;
  lpf.freq_hz = fin_or(fuzz_f64(&in), 1000.0);
  lpf.slope_24 = fuzz_u8(&in) & 1;
  double pass[OMX_EQ_PASS_MAX_SECTIONS][5];
  const uint32_t np = omx_eq_pass_design(&hpf, &lpf, rate, pass);
  FUZZ_CHECK(np <= OMX_EQ_PASS_MAX_SECTIONS, "the head filters wrote %u sections", (unsigned)np);
  for (uint32_t i = 0; i < np; i++)
    FUZZ_CHECK(omx_biquad_stable(pass[i]) && isfinite(pass[i][0]) && isfinite(pass[i][1]) && isfinite(pass[i][2]),
               "head section %u is unstable or not finite", (unsigned)i);
  fuzz_ledger_check("eq pass", omx_rate_is_declared((float)rate), 0);

  const uint32_t nb = 1u + fuzz_below(&in, OMX_EQ_MAX_BANDS);
  struct omx_eq_band bands[OMX_EQ_MAX_BANDS];
  uint32_t on_count = 0;
  for (uint32_t i = 0; i < nb; i++) {
    bands[i].kind = (enum omx_eq_kind)fuzz_below(&in, OMX_EQ_ALLPASS2 + 1);
    bands[i].on = fuzz_u8(&in) & 1;
    bands[i].freq_hz = fin_or(fuzz_f64(&in), 1000.0);
    bands[i].q = fin_or(fuzz_f64(&in), 1.0);
    bands[i].gain_db = (double)fuzz_range(&in, -24.0f, 24.0f);
    if (fuzz_u8(&in) & 1) bands[i].gain_db = 0.0; /* a parked band is a case of its own */
    on_count += (uint32_t)bands[i].on;
  }
  double dcoef[OMX_EQ_MAX_BANDS][5];
  uint8_t enabled[OMX_EQ_MAX_BANDS];
  const uint32_t ns = omx_eq_bank_design((flags >> 1) & 1, bands, nb, rate, dcoef, enabled);
  FUZZ_CHECK(ns <= nb, "the bank has %u sections for %u bands", (unsigned)ns, (unsigned)nb);
  FUZZ_CHECK(((flags >> 1) & 1) ? ns == on_count : ns == 0, "the bank has %u sections for %u bands that are on",
             (unsigned)ns, (unsigned)on_count);
  float coef[OMX_EQ_MAX_BANDS][5];
  uint32_t live = 0;
  for (uint32_t i = 0; i < ns; i++) {
    FUZZ_CHECK(omx_biquad_stable(dcoef[i]), "section %u is not stable: %g %g %g %g %g", (unsigned)i, dcoef[i][0],
               dcoef[i][1], dcoef[i][2], dcoef[i][3], dcoef[i][4]);
    for (int k = 0; k < 5; k++) {
      FUZZ_CHECK(isfinite(dcoef[i][k]), "section %u coefficient %d is not finite", (unsigned)i, k);
      coef[i][k] = (float)dcoef[i][k];
    }
    live += enabled[i];
  }
  fuzz_ledger_check("eq bank", omx_rate_is_declared((float)rate), 0);

  /* the magnitude of the bank: finite at any frequency, and 0 dB with nothing live */
  double f[4] = {fin_or(fuzz_f64(&in), 1000.0), 20.0, 1000.0, rate * 0.5}, db[4];
  omx_biquad_cascade_magnitude_db_d(dcoef, enabled, ns, f, 4, rate, -120.0, db);
  for (int i = 0; i < 4; i++) {
    FUZZ_CHECK(isfinite(db[i]), "the bank's magnitude at %g Hz is %g", f[i], db[i]);
    if (live == 0) FUZZ_CHECK(db[i] == 0.0, "a bank with no live section is %g dB at %g Hz", db[i], f[i]);
  }
  fuzz_ledger_check("eq magnitude", omx_rate_is_declared((float)rate), 0);

  /* the cascade over a finite block */
  const uint32_t n = 1u + fuzz_below(&in, BLOCK_MAX);
  float l[BLOCK_MAX], r[BLOCK_MAX], l0[BLOCK_MAX];
  fuzz_block(&in, l, n, 0);
  fuzz_block(&in, r, n, 0);
  memcpy(l0, l, n * sizeof(float));
  float sl[OMX_EQ_MAX_BANDS][4], sr[OMX_EQ_MAX_BANDS][4];
  memset(sl, 0, sizeof sl);
  memset(sr, 0, sizeof sr);
  if ((flags >> 2) & 1) omx_biquad_cascade_stereo(l, r, n, ns, coef, enabled, sl, sr);
  else omx_biquad_cascade(l, n, ns, coef, enabled, sl);
  FUZZ_CHECK(omx_block_finite(l, n) && omx_block_finite(r, n), "the cascade gave a non-finite sample");
  if (live == 0) FUZZ_CHECK(memcmp(l, l0, n * sizeof(float)) == 0, "a bank with no live section changed the block");
  fuzz_ledger_check("eq cascade", omx_rate_is_declared((float)rate), 0);

  /* the width conversions */
  const double bw = fin_or(fuzz_f64(&in), 1.0);
  if (bw > 0.0) {
    const double q = omx_eq_bandwidth_q(fin_or(fuzz_f64(&in), 1000.0), bw, rate);
    FUZZ_CHECK(isfinite(q) && q > 0.0, "omx_eq_bandwidth_q gave %g", q);
  }
  const double q2 = omx_eq_octaves_q(bw);
  FUZZ_CHECK(isfinite(q2) && q2 > 0.0, "omx_eq_octaves_q(%g) gave %g", bw, q2);
  fuzz_ledger_check("eq widths", omx_rate_is_declared((float)rate), 0);
  return 0;
}
