// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: the two delay lines, the fractional one (omx_fdelay.h, a Lagrange kernel
 * over a caller-owned ring) and the integer one (omx_idelay.h, a ring with a cross-fade between
 * two delays).
 *
 * Input:  [0]     bit 0: the line is fractional, else integer; bit 1: the order is 5, else 3
 *                 (fractional); bit 2: the blocks' samples are mono, else the integer pair runs
 *                 both legs
 *         [1..2]  u16 LE: the ring's size in samples (1..4096; refused sizes are part of the story)
 *         [3..]   blocks until the input ends or 12 have run: a u8 block length (1..128), the
 *                 delay for the block (the fractional line: any float, NaN and the infinities
 *                 included; the integer line: a u16 of tenths of a millisecond at a rate the
 *                 next byte picks off the grid) and the block's samples (fuzz_block, finite).
 *
 * The ring starts zeroed, so the line is silent before the take starts, and the harness keeps the
 * whole take itself, which is the independent oracle: the line's own ring is never read back.
 *
 * Properties, each an abort():
 *   - a refused order or ring (init) leaves the line unarmed, and an unarmed line leaves the block
 *     as it was; an armed line's delay lands in [0, the ring's longest], a non-finite request
 *     reads as 0;
 *   - a WHOLE-sample delay is exact: the block's output is the take delayed by that many samples,
 *     bit for bit, zero before the start of the take;
 *   - a fractional delay stays finite and inside the interpolation's own bound: the sum of the
 *     absolute Lagrange weights at that fraction times the take's peak;
 *   - a steady integer delay (the same one two blocks running) is the take delayed, bit for bit;
 *     a delay change (the cross-fade) stays between the two taps it fades;
 *   - omx_idelay_samples() lands in [0, cap) for any finite ms and any rate off the declared grid
 *     or on it, and is 0 for a delay of no length.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_fdelay.h>
#include <omxdsp/omx_idelay.h>

#include "fuzz.h"

#define BLOCKS_MAX 12
#define BLOCK_MAX 128u
#define TAKE_MAX (BLOCKS_MAX * BLOCK_MAX)
#define RING_MAX 4096u

static float take_at(const float *take, long i) { return i < 0 ? 0.0f : take[i]; }

/* the sum of the absolute Lagrange weights at a fraction: what a sample can grow by */
static double lebesgue(int order, float kf) {
  float c[OMX_FDELAY_MAX_TAPS];
  omx_fdelay_lagrange(order, kf, c);
  double s = 0.0;
  for (int k = 0; k <= order; k++) s += fabs((double)c[k]);
  return s;
}

static void fractional(struct fuzz_in *in, int order, uint32_t cap) {
  float *ring = calloc(cap ? cap : 1u, sizeof(float));
  /* not a property: calloc failed */
  if (!ring) abort();
  struct omx_fdelay l;
  const int fits = omx_fdelay_max_delay(cap, order) > 0.0f;
  const enum omx_fdelay_code code = omx_fdelay_init(&l, ring, cap, order);
  const int armed = code == OMX_FDELAY_OK;
  FUZZ_CHECK(armed == fits, "init answered %d for a ring of %u at order %d", (int)code, (unsigned)cap, order);
  /* a ring that cannot hold the kernel is refused by a code, and the call records its PRE `the-ring-holds-the-kernels-reach` */
  if (!fits) {
    FUZZ_CHECK(code == OMX_FDELAY_RING_TOO_SMALL, "a ring of %u at order %d was answered %d", (unsigned)cap, order, (int)code);
    fuzz_ledger_reset();
  }
  static float take[TAKE_MAX];
  uint32_t done = 0;
  float peak = 0.0f;
  for (int b = 0; b < BLOCKS_MAX && in->n > 0; b++) {
    const uint32_t n = 1u + fuzz_u8(in) % BLOCK_MAX;
    const float delay = fuzz_f32(in);
    float buf[BLOCK_MAX], dry[BLOCK_MAX];
    fuzz_block(in, buf, n, 0);
    memcpy(dry, buf, n * sizeof(float));
    for (uint32_t i = 0; i < n; i++) {
      take[done + i] = buf[i];
      peak = fmaxf(peak, fabsf(buf[i]));
    }
    if (!armed) {
      /* an unarmed line is not run: omx_fdelay_process requires a line that is armed (its PRE) */
      FUZZ_CHECK(omx_fdelay_latency(&l, delay) == 0.0f, "an unarmed line has a latency");
      done += n;
      continue;
    }
    const float d = omx_fdelay_latency(&l, delay);
    FUZZ_CHECK(d >= 0.0f && d <= omx_fdelay_max_delay(cap, order) && d - d == 0.0f,
               "a request of %g was delivered as %g, in a ring that serves %g", (double)delay, (double)d,
               (double)omx_fdelay_max_delay(cap, order));
    if (!(delay - delay == 0.0f)) FUZZ_CHECK(d == 0.0f, "a non-finite request was delivered as %g", (double)d);
    omx_fdelay_process(buf, n, &l, delay);
    uint32_t id;
    float kf;
    const int whole = omx_fdelay_split(&l, delay, &id, &kf);
    const double bound = whole ? 1.0 : lebesgue(order, kf);
    for (uint32_t i = 0; i < n; i++) {
      FUZZ_CHECK(buf[i] - buf[i] == 0.0f, "fractional delay %g gave a non-finite sample at %u", (double)d, (unsigned)i);
      FUZZ_CHECK(fabs((double)buf[i]) <= bound * (double)peak * 1.0001 + 1e-30,
                 "delay %g (%g weights): sample %u is %g, past %g x peak %g", (double)d, bound, (unsigned)i, (double)buf[i],
                 bound, (double)peak);
      if (whole) {
        const float want = take_at(take, (long)(done + i) - (long)id);
        FUZZ_CHECK(memcmp(&want, &buf[i], sizeof want) == 0 || (want == 0.0f && buf[i] == 0.0f),
                   "whole delay %u: sample %u is %g, the take says %g", (unsigned)id, (unsigned)(done + i), (double)buf[i],
                   (double)want);
      }
    }
    done += n;
  }
  free(ring);
}

static void integer(struct fuzz_in *in, uint32_t cap, int mono) {
  float *rl = calloc(cap + 1u, sizeof(float)), *rr = calloc(cap + 1u, sizeof(float));
  /* not a property: calloc failed */
  if (!rl || !rr) abort();
  struct omx_idelay_pair d = {rl, rr, cap, 0, 0, 0, 0};
  static float takel[TAKE_MAX], taker[TAKE_MAX];
  uint32_t done = 0;
  uint32_t last_l = 0, last_r = 0;
  for (int b = 0; b < BLOCKS_MAX && in->n > 0; b++) {
    const uint32_t n = 1u + fuzz_u8(in) % BLOCK_MAX;
    const float sr = OMX_DECLARED_RATES[fuzz_below(in, OMX_DECLARED_RATE_COUNT)];
    const float ms_l = (float)fuzz_u16(in) * 0.1f, ms_r = (float)fuzz_u16(in) * 0.1f;
    const uint32_t tl = omx_idelay_samples(ms_l, sr, cap), tr = omx_idelay_samples(ms_r, sr, cap);
    FUZZ_CHECK(tl < cap && tr < cap, "omx_idelay_samples(%g ms, %g, %u) = %u / %u is outside the ring", (double)ms_l,
               (double)sr, (unsigned)cap, (unsigned)tl, (unsigned)tr);
    if (ms_l == 0.0f) FUZZ_CHECK(tl == 0u, "a delay of no length is %u samples", (unsigned)tl);
    float l[BLOCK_MAX], r[BLOCK_MAX];
    fuzz_block(in, l, n, 0);
    fuzz_block(in, r, n, 0);
    for (uint32_t i = 0; i < n; i++) {
      takel[done + i] = l[i];
      taker[done + i] = r[i];
    }
    /* a steady delay is exact; one that moves is a cross-fade between the old tap and the new one */
    const int steady_l = d.cur_l == tl, steady_r = d.cur_r == tr;
    const uint32_t from_l = d.cur_l, from_r = d.cur_r;
    omx_idelay_pair_apply(l, mono ? NULL : r, n, &d, tl, tr);
    for (uint32_t i = 0; i < n; i++) {
      const float wl = take_at(takel, (long)(done + i) - (long)tl);
      const float ol = take_at(takel, (long)(done + i) - (long)from_l);
      if (steady_l) FUZZ_CHECK(memcmp(&wl, &l[i], sizeof wl) == 0 || (wl == 0.0f && l[i] == 0.0f),
                               "steady delay %u: left sample %u is %g, the take says %g", (unsigned)tl, (unsigned)(done + i),
                               (double)l[i], (double)wl);
      else FUZZ_CHECK(l[i] >= fminf(wl, ol) - 1e-6f && l[i] <= fmaxf(wl, ol) + 1e-6f,
                      "fade %u -> %u: left sample %u is %g, outside %g and %g", (unsigned)from_l, (unsigned)tl, (unsigned)(done + i),
                      (double)l[i], (double)ol, (double)wl);
      if (!mono) {
        const float wr = take_at(taker, (long)(done + i) - (long)tr);
        const float orr = take_at(taker, (long)(done + i) - (long)from_r);
        if (steady_r) FUZZ_CHECK(memcmp(&wr, &r[i], sizeof wr) == 0 || (wr == 0.0f && r[i] == 0.0f),
                                 "steady delay %u: right sample %u is %g, the take says %g", (unsigned)tr, (unsigned)(done + i),
                                 (double)r[i], (double)wr);
        else FUZZ_CHECK(r[i] >= fminf(wr, orr) - 1e-6f && r[i] <= fmaxf(wr, orr) + 1e-6f,
                        "fade %u -> %u: right sample %u is %g, outside %g and %g", (unsigned)from_r, (unsigned)tr,
                        (unsigned)(done + i), (double)r[i], (double)orr, (double)wr);
      }
    }
    last_l = tl;
    last_r = tr;
    done += n;
  }
  (void)last_l;
  (void)last_r;
  free(rl);
  free(rr);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  struct fuzz_in in = fuzz_in(data, size);
  const uint8_t flags = fuzz_u8(&in);
  const uint32_t cap = 1u + fuzz_u16(&in) % RING_MAX;
  fuzz_ledger_reset();
  if (flags & 1u) fractional(&in, (flags & 2u) ? 5 : 3, cap);
  else integer(&in, cap, (flags >> 2) & 1);
  fuzz_ledger_clean("delay");
  return 0;
}
