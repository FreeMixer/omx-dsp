// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: the sample-rate converter (omx_src.h): a rate pair reduced to a plan, the
 * polyphase table designed, a take converted block by block and flushed.
 *
 * Input:  [0..3]  the source rate, u32 LE (any value: 0 and the absurd are refused)
 *         [4..7]  the destination rate, u32 LE
 *         [8]     bit 0: the blocks are cut at the sizes the following bytes give, else the
 *                 take goes through in one call; bit 1: the take is flushed at its end
 *         [9..]   blocks until the input ends or 16 have run: a u16 length (modulo 513, a
 *                 length of 0 is a block of no samples) and that many finite samples
 *                 (fuzz_block, a signed 16-bit word each).
 *
 * The plan refuses what it cannot size (a zero rate, an interpolation factor past OMX_SRC_MAX_L),
 * and the harness holds it to refusing exactly that; a refused plan ends the input.
 *
 * Properties, each an abort():
 *   - the plan is the REDUCED ratio: l/m equals out/in, and gcd(l, m) is 1;
 *   - a call never makes more outputs than omx_src_out_capacity() allows for its input, and a take run
 *     without a flush makes no more than a whole file's omx_src_out_frames() plus the filter's half
 *     length (the outputs its first samples wait for);
 *   - the take is the same however it is cut into blocks: run whole, or in the blocks the input
 *     gives, it yields the same samples bit for bit;
 *   - every output sample is finite, and bounded by the input's peak times the filter's own
 *     worst-case gain (the sum of the absolute coefficients of its worst phase);
 *   - an identity plan (the same rate) returns the input samples unchanged.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_src.h>

#include "fuzz.h"

#define BLOCKS_MAX 16
#define BLOCK_MAX 512u
#define IN_MAX (BLOCKS_MAX * BLOCK_MAX)

/* a take: the input samples as the blocks gave them, and where each block ends */
struct take {
  float x[IN_MAX];
  uint32_t end[BLOCKS_MAX];
  int blocks;
  uint32_t n;
};

static uint32_t convert(const struct omx_src_plan *p, const float *coef, const struct take *t, int whole, int flush,
                        float *out, uint32_t cap) {
  float hist[OMX_SRC_TAPS];
  struct omx_src s;
  omx_src_init(&s, p, coef, hist);
  uint32_t w = 0, from = 0;
  if (whole) {
    w = omx_src_run(&s, t->x, t->n, out, cap, flush);
  } else {
    for (int b = 0; b < t->blocks; b++) {
      const int last = b == t->blocks - 1;
      const uint32_t n = t->end[b] - from;
      w += omx_src_run(&s, t->x + from, n, out + w, cap - w, last && flush);
      from = t->end[b];
    }
  }
  return w;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  struct fuzz_in in = fuzz_in(data, size);
  const uint32_t rin = fuzz_u32(&in), rout = fuzz_u32(&in);
  const uint8_t flags = fuzz_u8(&in);
  fuzz_ledger_reset();

  struct omx_src_plan p;
  if (omx_src_plan_of(rin, rout, &p) != 0) {
    FUZZ_CHECK(rin == 0 || rout == 0 || rout / omx_src_gcd(rout, rin) > OMX_SRC_MAX_L,
               "the plan refused %u -> %u, which it can size", (unsigned)rin, (unsigned)rout);
    return 0;
  }
  FUZZ_CHECK(omx_src_gcd(p.l, p.m) == 1u, "the plan %u/%u of %u -> %u is not reduced", (unsigned)p.l, (unsigned)p.m,
             (unsigned)rin, (unsigned)rout);
  FUZZ_CHECK((uint64_t)p.l * rin == (uint64_t)p.m * rout, "the plan %u/%u is not the ratio of %u -> %u", (unsigned)p.l,
             (unsigned)p.m, (unsigned)rin, (unsigned)rout);
  const size_t ncoef = omx_src_coef_count(&p);
  float *coef = malloc(ncoef * sizeof(float));
  /* not a property: malloc failed */
  if (!coef) abort();
  omx_src_design(&p, coef, OMX_SRC_ROLLOFF_DOUBLE, OMX_SRC_STOPBAND_DB);

  static struct take t;
  t.n = 0;
  t.blocks = 0;
  float peak = 0.0f;
  while (t.blocks < BLOCKS_MAX && in.n > 0) {
    const uint32_t n = fuzz_u16(&in) % (BLOCK_MAX + 1u);
    fuzz_block(&in, t.x + t.n, n, 0);
    for (uint32_t i = 0; i < n; i++) peak = fmaxf(peak, fabsf(t.x[t.n + i]));
    t.n += n;
    t.end[t.blocks++] = t.n;
  }

  if (t.blocks == 0) t.end[t.blocks++] = 0u; /* a take is at least one call, of no samples */

  /* the worst phase's gain: the sum of the absolute coefficients, what no output can exceed per unit of peak */
  double gain = 0.0;
  for (uint32_t ph = 0; ph < p.l; ph++) {
    double g = 0.0;
    for (uint32_t k = 0; k < p.taps; k++) g += fabs((double)coef[(size_t)ph * p.taps + k]);
    if (g > gain) gain = g;
  }

  const uint32_t cap = omx_src_out_capacity(&p, t.n + 2u * p.taps);
  float *a = malloc((size_t)cap * sizeof(float) + 1), *b = malloc((size_t)cap * sizeof(float) + 1);
  /* not a property: malloc failed */
  if (!a || !b) abort();
  const int flush = (flags >> 1) & 1;
  const uint32_t wa = convert(&p, coef, &t, 1, flush, a, cap);
  const uint32_t wb = convert(&p, coef, &t, (flags & 1u) ? 0 : 1, flush, b, cap);

  FUZZ_CHECK(wa == wb && memcmp(a, b, (size_t)wa * sizeof(float)) == 0,
             "%u -> %u: the take cut into %d blocks is not the take run whole (%u and %u samples)", (unsigned)rin,
             (unsigned)rout, t.blocks, (unsigned)wa, (unsigned)wb);
  const uint64_t want = omx_src_out_frames(t.n, &p);
  FUZZ_CHECK(wa <= omx_src_out_capacity(&p, t.n + (flush ? p.taps : 0u)),
             "%u -> %u: %u samples out of %u in exceeds the capacity", (unsigned)rin, (unsigned)rout, (unsigned)wa,
             (unsigned)t.n);
  if (!flush) FUZZ_CHECK(wa <= want + p.taps / 2u + 1u, "%u -> %u: %u samples out of %u in, a whole file is %u", (unsigned)rin,
                         (unsigned)rout, (unsigned)wa, (unsigned)t.n, (unsigned)want);
  for (uint32_t i = 0; i < wa; i++) {
    FUZZ_CHECK(isfinite(a[i]) && fabs((double)a[i]) <= gain * (double)peak * 1.0001 + 1e-30,
               "%u -> %u: output %u is %g, past %g x peak %g", (unsigned)rin, (unsigned)rout, (unsigned)i, (double)a[i], gain,
               (double)peak);
  }
  if (p.l == 1u && p.m == 1u)
    FUZZ_CHECK(wa >= t.n && memcmp(a, t.x, (size_t)t.n * sizeof(float)) == 0, "an identity plan changed the take");
  fuzz_ledger_clean("src");
  free(coef);
  free(a);
  free(b);
  return 0;
}
