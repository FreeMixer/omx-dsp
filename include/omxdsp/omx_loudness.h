// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_loudness.h
 * @brief EBU R 128 loudness, as ITU-R BS.1770-4 defines it: the K-weighting, the momentary,
 *        short-term and gated integrated loudness, the loudness range of EBU Tech 3342 and the
 *        ×4 true peak.
 *
 * One caller-owned state measures up to OMX_LOUDNESS_MAX_CHANNELS legs. omx_loudness_init()
 * designs the K-weighting for the rate and lays out the gating histograms; omx_loudness_process()
 * takes planar blocks of any length; the readings may be taken between any two blocks. The
 * readings come in energy (the K-weighted, leg-weighted mean square: `_ms`) and in LUFS, where
 * `L = −0.691 + 10·log10(ms)`.
 *
 * The K-weighted squares are summed in 10 ms sub-blocks (`rate / 100` samples). Momentary is the
 * last 40 of them (400 ms), short-term the last 300 (3 s). Every 100 ms the full 400 ms block
 * joins the integrated histogram and, once 3 s have passed, the short-term value joins the range
 * histogram. A histogram bin is 0.1 LU wide, from −70 LUFS (the absolute gate, a bin edge) up, and
 * holds its count and the exact sum of its energies, so a programme of any length takes the same
 * state. The relative gates are placed in energy; the bin a gate falls in is kept when its own
 * mean is above the gate. Design: BUILDING.md, "Loudness" (FreeMixer/omx-dsp#64).
 *
 * Contract: NO allocation, NO lock, NO libc on the process path.
 */
#ifndef OMX_LOUDNESS_H
#define OMX_LOUDNESS_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "omx_biquad.h"
#include "omx_contract.h"
#include "omx_truepeak.h"
#include "omxdsp.h"

/** @brief The most legs one state measures: 7.1. */
#define OMX_LOUDNESS_MAX_CHANNELS 8u
/** @brief BS.1770's loudness offset: `L = −0.691 + 10·log10(ms)`. */
#define OMX_LOUDNESS_OFFSET_LU (-0.691)
/** @brief The absolute gate, LUFS: a block at or below it is silence to the gated measures. */
#define OMX_LOUDNESS_ABSOLUTE_GATE_LUFS (-70.0)
/** @brief The integrated loudness's relative gate, LU below the absolute-gated mean. */
#define OMX_LOUDNESS_RELATIVE_GATE_LU (-10.0)
/** @brief The loudness range's relative gate, LU below the absolute-gated mean (Tech 3342). */
#define OMX_LOUDNESS_RANGE_GATE_LU (-20.0)
/** @brief The range's low percentile (Tech 3342). */
#define OMX_LOUDNESS_RANGE_LOW 0.10
/** @brief The range's high percentile (Tech 3342). */
#define OMX_LOUDNESS_RANGE_HIGH 0.95
/** @brief The sub-blocks per second: a sub-block is 10 ms, `rate / 100` samples. */
#define OMX_LOUDNESS_SUBBLOCKS_PER_S 100u
/** @brief The momentary window in sub-blocks: 400 ms. */
#define OMX_LOUDNESS_MOMENTARY 40u
/** @brief The short-term window in sub-blocks: 3 s; also the ring's length. */
#define OMX_LOUDNESS_SHORT_TERM 300u
/** @brief The gating step in sub-blocks: 100 ms, a block every 75 % overlap. */
#define OMX_LOUDNESS_STEP 10u
/** @brief A histogram bin's width, LU. */
#define OMX_LOUDNESS_BIN_LU 0.1
/** @brief The histograms' bins: 0.1 LU from −70 LUFS to +30 LUFS; the top one also takes any louder block. */
#define OMX_LOUDNESS_BINS 1000u
/** @brief The reading of a measure that has nothing to read: no block above the gate, no signal. */
#define OMX_LOUDNESS_SILENCE (-HUGE_VAL)

/** @brief One gating histogram: per 0.1 LU bin, the blocks it holds and the sum of their energies. */
struct omx_loudness_hist {
  double energy[OMX_LOUDNESS_BINS]; /**< Each bin's sum of block energies (mean squares). */
  uint32_t count[OMX_LOUDNESS_BINS]; /**< Each bin's block count. */
};

/** @brief The measurement's state: the K-weighting, the sub-block ring, both histograms and the true-peak detectors. */
struct omx_loudness {
  double kw[2][5];                                     /**< The K-weighting: the shelf, then the RLB high-pass. */
  double ks[OMX_LOUDNESS_MAX_CHANNELS][2][4];          /**< Each leg's history of both sections. */
  double weight[OMX_LOUDNESS_MAX_CHANNELS];            /**< Each leg's BS.1770 weight; 0 drops the leg. */
  double edge[OMX_LOUDNESS_BINS];                      /**< Each bin's lower edge, in energy. */
  double ring[OMX_LOUDNESS_SHORT_TERM];                /**< The last 300 sub-blocks' weighted sums of squares. */
  double acc[OMX_LOUDNESS_MAX_CHANNELS];               /**< Each leg's sum of squares in the open sub-block. */
  struct omx_loudness_hist gated;                      /**< The 400 ms blocks, for the integrated loudness. */
  struct omx_loudness_hist range;                      /**< The short-term values, for the loudness range. */
  struct omx_truepeak tp[OMX_LOUDNESS_MAX_CHANNELS];   /**< Each leg's ×4 true-peak detector. */
  float peak;                                          /**< The largest true-peak reading since init or reset. */
  uint32_t channels;                                   /**< The legs measured. */
  uint32_t sub_len;                                    /**< Samples per sub-block: `rate / 100`. */
  uint32_t fill;                                       /**< Samples in the open sub-block. */
  uint32_t pos;                                        /**< The ring slot the next sub-block lands in. */
  uint64_t subs;                                       /**< Sub-blocks closed since init or reset. */
};

OMXDSP_STATE_LAYOUT(loudness, struct omx_loudness)

#define OMX_CONTRACT_STAGE "loudness/design"
/**
 * @brief BS.1770-4's K-weighting for `sr`: the head's high shelf, then the RLB high-pass, each a
 *        normalised `{b0, b1, b2, a1, a2}`.
 *
 * The analogue prototype the standard's 48 kHz table was drawn from (shelf 1681.97 Hz, +4.0 dB,
 * Q 0.7072; high-pass 38.135 Hz, Q 0.5003), through the bilinear transform with `tan`
 * prewarping, so every rate gets the same filter and 48 kHz gets the printed coefficients.
 * @param sr The rate, Hz; 100 times a whole number of samples per 10 ms.
 * @param c The two sections out, in double.
 * @pre `rate-is-whole-centiseconds`.
 * @post `both-sections-stable`.
 * @note CONTROL thread: calls `tan` and `pow`. Thread-safe: writes only `c`.
 */
static inline void omx_loudness_kweight_design(double sr, double c[2][5]) {
  OMX_PRE(sr >= 8000.0 && sr <= 768000.0 && sr / 100.0 == floor(sr / 100.0), "rate-is-whole-centiseconds");
  const double f1 = 1681.9744509555319, g1 = 3.99984385397, q1 = 0.7071752369554193;
  const double k1 = tan(M_PI * f1 / sr);
  const double vh = pow(10.0, g1 / 20.0);
  const double vb = pow(vh, 0.499666774155);
  const double d1 = 1.0 + k1 / q1 + k1 * k1;
  c[0][0] = (vh + vb * k1 / q1 + k1 * k1) / d1;
  c[0][1] = 2.0 * (k1 * k1 - vh) / d1;
  c[0][2] = (vh - vb * k1 / q1 + k1 * k1) / d1;
  c[0][3] = 2.0 * (k1 * k1 - 1.0) / d1;
  c[0][4] = (1.0 - k1 / q1 + k1 * k1) / d1;
  const double f2 = 38.13547087602444, q2 = 0.5003270373238773;
  const double k2 = tan(M_PI * f2 / sr);
  const double d2 = 1.0 + k2 / q2 + k2 * k2;
  c[1][0] = 1.0;
  c[1][1] = -2.0;
  c[1][2] = 1.0;
  c[1][3] = 2.0 * (k2 * k2 - 1.0) / d2;
  c[1][4] = (1.0 - k2 / q2 + k2 * k2) / d2;
  OMX_POST(omx_biquad_stable(c[0]) && omx_biquad_stable(c[1]), "both-sections-stable");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief Loudness from a K-weighted mean square: `−0.691 + 10·log10(ms)`.
 * @param ms The mean square, leg weights applied and legs summed.
 * @return LUFS; OMX_LOUDNESS_SILENCE for `ms <= 0` (or NaN).
 * @note RT-safe: one `log10`. Thread-safe: pure.
 */
static inline double omx_loudness_lufs(double ms) {
  return ms > 0.0 ? OMX_LOUDNESS_OFFSET_LU + 10.0 * log10(ms) : OMX_LOUDNESS_SILENCE;
}

#define OMX_CONTRACT_STAGE "loudness/reset"
/**
 * @brief Start a new measurement on the same design: every window, histogram, peak and detector cleared.
 * @param s The state, initialised.
 * @post `windows-empty`.
 * @note RT-safe: no allocation, a fixed amount of clearing. Thread-safe on distinct state.
 */
static inline void omx_loudness_reset(struct omx_loudness *s) {
  memset(s->ks, 0, sizeof s->ks);
  memset(s->ring, 0, sizeof s->ring);
  memset(&s->gated, 0, sizeof s->gated);
  memset(&s->range, 0, sizeof s->range);
  for (uint32_t c = 0; c < OMX_LOUDNESS_MAX_CHANNELS; c++) omx_truepeak_init(&s->tp[c]);
  memset(s->acc, 0, sizeof s->acc);
  s->peak = 0.0f;
  s->fill = 0u;
  s->pos = 0u;
  s->subs = 0u;
  OMX_POST(s->subs == 0u && s->fill == 0u, "windows-empty");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "loudness/init"
/**
 * @brief Arm a measurement of `channels` legs at `sr`: the K-weighting designed, the bin edges laid
 *        out, the weights taken, everything else cleared.
 * @param s The state.
 * @param sr The rate, Hz; 100 times a whole number of samples per 10 ms (every oracle rate).
 * @param channels The legs, 1 to OMX_LOUDNESS_MAX_CHANNELS; more are clamped.
 * @param weight Each leg's weight (BS.1770: 1.0 for L, R and C, 1.41 for the surrounds, 0 for
 *        the LFE); NULL for 1.0 on every leg.
 * @pre `rate-is-whole-centiseconds`, `channels-within-cap`, `weights-finite-and-non-negative`.
 * @post `windows-empty`.
 * @note CONTROL thread: calls `tan` and `pow`. Thread-safe on distinct state.
 */
static inline void omx_loudness_init(struct omx_loudness *s, float sr, uint32_t channels, const float *weight) {
  OMX_PRE(channels >= 1u && channels <= OMX_LOUDNESS_MAX_CHANNELS, "channels-within-cap");
  if (channels > OMX_LOUDNESS_MAX_CHANNELS) channels = OMX_LOUDNESS_MAX_CHANNELS;
  OMX_PRE(weight == NULL || omx_block_finite(weight, channels), "weights-finite-and-non-negative");
  omx_loudness_kweight_design((double)sr, s->kw);
  s->channels = channels;
  s->sub_len = (uint32_t)((double)sr / OMX_LOUDNESS_SUBBLOCKS_PER_S + 0.5);
  if (s->sub_len == 0u) s->sub_len = 1u;
  for (uint32_t c = 0; c < OMX_LOUDNESS_MAX_CHANNELS; c++) {
    const double w = c < channels ? (weight ? (double)weight[c] : 1.0) : 0.0;
    OMX_PRE(w >= 0.0, "weights-finite-and-non-negative");
    s->weight[c] = w > 0.0 ? w : 0.0;
  }
  for (uint32_t b = 0; b < OMX_LOUDNESS_BINS; b++)
    s->edge[b] = pow(10.0, (OMX_LOUDNESS_ABSOLUTE_GATE_LUFS + OMX_LOUDNESS_BIN_LU * (double)b - OMX_LOUDNESS_OFFSET_LU) / 10.0);
  omx_loudness_reset(s);
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The bin a block's energy lands in: the last edge at or below `ms`, by bisection.
 * @param s The state.
 * @param ms The block's energy.
 * @return The bin, or -1 when `ms` is at or below the absolute gate.
 * @note RT-safe: ten comparisons. Thread-safe: reads only.
 */
static inline int omx_loudness_bin(const struct omx_loudness *s, double ms) {
  if (!(ms > s->edge[0])) return -1;
  uint32_t lo = 0u, hi = OMX_LOUDNESS_BINS;
  while (hi - lo > 1u) {
    const uint32_t mid = (lo + hi) / 2u;
    if (ms >= s->edge[mid]) lo = mid;
    else hi = mid;
  }
  return (int)lo;
}

/**
 * @brief The mean of the last `k` sub-blocks' sums, over `k` whole sub-blocks: a window's energy.
 * @param s The state.
 * @param k The window, in sub-blocks, at most OMX_LOUDNESS_SHORT_TERM.
 * @return The window's mean square; a window not yet filled counts the missing time as silence.
 * @note RT-safe: `k` additions. Thread-safe: reads only.
 */
static inline double omx_loudness_window_ms(const struct omx_loudness *s, uint32_t k) {
  double sum = 0.0;
  uint32_t at = s->pos;
  for (uint32_t i = 0; i < k; i++) {
    at = at == 0u ? OMX_LOUDNESS_SHORT_TERM - 1u : at - 1u;
    sum += s->ring[at];
  }
  return sum / ((double)k * (double)s->sub_len);
}

/**
 * @brief Put one block's energy into a histogram (nothing at or below the absolute gate).
 * @param s The state.
 * @param h The histogram.
 * @param ms The block's energy.
 * @note RT-safe: one bisection. Thread-safe on distinct state.
 */
static inline void omx_loudness_hist_add(const struct omx_loudness *s, struct omx_loudness_hist *h, double ms) {
  const int b = omx_loudness_bin(s, ms);
  if (b < 0) return;
  h->energy[b] += ms;
  h->count[b] += 1u;
}

/**
 * @brief Close the open sub-block: into the ring, and every 100 ms the 400 ms block and the 3 s
 *        value into their histograms.
 * @param s The state.
 * @note RT-safe: at most 340 additions. Thread-safe on distinct state.
 */
static inline void omx_loudness_close_sub(struct omx_loudness *s) {
  double sum = 0.0;
  for (uint32_t c = 0; c < s->channels; c++) sum += s->weight[c] * s->acc[c];
  s->ring[s->pos] = sum;
  s->pos = s->pos + 1u == OMX_LOUDNESS_SHORT_TERM ? 0u : s->pos + 1u;
  memset(s->acc, 0, sizeof s->acc);
  s->fill = 0u;
  s->subs++;
  if (s->subs % OMX_LOUDNESS_STEP != 0u) return;
  if (s->subs >= OMX_LOUDNESS_MOMENTARY) omx_loudness_hist_add(s, &s->gated, omx_loudness_window_ms(s, OMX_LOUDNESS_MOMENTARY));
  if (s->subs >= OMX_LOUDNESS_SHORT_TERM) omx_loudness_hist_add(s, &s->range, omx_loudness_window_ms(s, OMX_LOUDNESS_SHORT_TERM));
}

#define OMX_CONTRACT_STAGE "loudness/process"
/**
 * @brief Measure `n` frames of every leg: K-weight, square, weight and sum them into the windows,
 *        and run each leg's true-peak detector.
 *
 * The blocks may be of any length; a 10 ms sub-block closes wherever it falls, so the readings
 * do not depend on how the input was cut.
 * @param s The state.
 * @param in `s->channels` planar legs of `n` samples each; finite. A NULL leg is skipped: it adds
 *        nothing and its filters and detector do not advance.
 * @param n Frames.
 * @pre `finite-in`.
 * @post `finite-windows`.
 * @note RT-safe: no allocation, no libm, `OMX_TRUEPEAK_CHUNK` floats of stack plus the detector's;
 *       O(channels·n). Thread-safe on distinct state.
 */
static inline void omx_loudness_process(struct omx_loudness *s, const float *const *in, uint32_t n) {
  for (uint32_t c = 0; c < s->channels; c++) OMX_PRE(in[c] == NULL || omx_block_finite(in[c], n), "finite-in");
  float pk[OMX_TRUEPEAK_CHUNK];
  for (uint32_t c = 0; c < s->channels; c++) {
    if (in[c] == NULL) continue;
    for (uint32_t done = 0; done < n;) {
      const uint32_t k = n - done < OMX_TRUEPEAK_CHUNK ? n - done : OMX_TRUEPEAK_CHUNK;
      const float p = omx_truepeak_block(&s->tp[c], in[c] + done, k, pk);
      if (p > s->peak) s->peak = p;
      done += k;
    }
  }
  for (uint32_t done = 0; done < n;) {
    const uint32_t k = n - done < s->sub_len - s->fill ? n - done : s->sub_len - s->fill;
    for (uint32_t c = 0; c < s->channels; c++) {
      if (in[c] == NULL) continue;
      const float *x = in[c] + done;
      double(*st)[4] = s->ks[c];
      double sq = s->acc[c]; /* one running sum per leg, sample by sample: the cut does not reorder it */
      for (uint32_t i = 0; i < k; i++) {
        const double y = (double)omx_biquad_d(omx_biquad_d(x[i], s->kw[0], st[0]), s->kw[1], st[1]);
        sq += y * y;
      }
      s->acc[c] = sq;
    }
    s->fill += k;
    done += k;
    if (s->fill == s->sub_len) omx_loudness_close_sub(s);
  }
  OMX_POST(omx_block_finite_d(s->acc, OMX_LOUDNESS_MAX_CHANNELS) && s->peak - s->peak == 0.0f, "finite-windows");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The momentary energy: the mean square of the last 400 ms (closed sub-blocks).
 * @param s The state.
 * @return The mean square; before 400 ms the missing time counts as silence.
 * @note RT-safe: 40 additions. Thread-safe: reads only.
 */
static inline double omx_loudness_momentary_ms(const struct omx_loudness *s) {
  return omx_loudness_window_ms(s, OMX_LOUDNESS_MOMENTARY);
}

/**
 * @brief The short-term energy: the mean square of the last 3 s (closed sub-blocks).
 * @param s The state.
 * @return The mean square; before 3 s the missing time counts as silence.
 * @note RT-safe: 300 additions. Thread-safe: reads only.
 */
static inline double omx_loudness_short_term_ms(const struct omx_loudness *s) {
  return omx_loudness_window_ms(s, OMX_LOUDNESS_SHORT_TERM);
}

/**
 * @brief The energy mean of a histogram's blocks above a gate placed `rel_lu` below their own
 *        energy mean (the absolute gate already applied).
 * @param h The histogram.
 * @param edge The state's bin edges.
 * @param rel_lu The relative gate, LU (negative).
 * @param first The first bin kept, out; OMX_LOUDNESS_BINS when nothing is.
 * @return The gated energy mean; 0 when no block passes.
 * @note RT-safe: two passes over the bins, one `pow`. Thread-safe: reads only.
 */
static inline double omx_loudness_hist_gated(const struct omx_loudness_hist *h, const double *edge, double rel_lu, uint32_t *first) {
  double e = 0.0, n = 0.0;
  for (uint32_t b = 0; b < OMX_LOUDNESS_BINS; b++) { e += h->energy[b]; n += (double)h->count[b]; }
  *first = OMX_LOUDNESS_BINS;
  if (!(n > 0.0)) return 0.0;
  const double gate = e / n * pow(10.0, rel_lu / 10.0);
  double ge = 0.0, gn = 0.0;
  for (uint32_t b = 0; b < OMX_LOUDNESS_BINS; b++) {
    if (h->count[b] == 0u) continue;
    const int above = (b + 1u < OMX_LOUDNESS_BINS && edge[b + 1u] <= gate) ? 0 : h->energy[b] / (double)h->count[b] > gate;
    if (!above) continue;
    if (*first == OMX_LOUDNESS_BINS) *first = b;
    ge += h->energy[b];
    gn += (double)h->count[b];
  }
  return gn > 0.0 ? ge / gn : 0.0;
}

/**
 * @brief The integrated energy: the mean of every 400 ms block above the absolute gate and above
 *        the relative gate, −10 LU below the absolute-gated mean.
 * @param s The state.
 * @return The gated mean square; 0 when no block passes both gates.
 * @note RT-safe: two passes over 1000 bins, one `pow`. Thread-safe: reads only.
 */
static inline double omx_loudness_integrated_ms(const struct omx_loudness *s) {
  uint32_t first;
  return omx_loudness_hist_gated(&s->gated, s->edge, OMX_LOUDNESS_RELATIVE_GATE_LU, &first);
}

#define OMX_CONTRACT_STAGE "loudness/range"
/**
 * @brief The loudness range's two ends, in energy (EBU Tech 3342): the 10th and the 95th
 *        percentile of the short-term values above the absolute gate and above the relative gate,
 *        −20 LU below their mean.
 *
 * A percentile is the value of rank `round((N − 1)·p)` of the `N` values kept, read as the energy
 * mean of the bin that holds it.
 * @param s The state.
 * @param lo The 10th percentile's energy, out; 0 when no value passes.
 * @param hi The 95th percentile's energy, out; 0 when no value passes.
 * @post `low-end-not-above-high-end`.
 * @note RT-safe: three passes over 1000 bins, one `pow`. Thread-safe: reads only.
 */
static inline void omx_loudness_range_ms(const struct omx_loudness *s, double *lo, double *hi) {
  const struct omx_loudness_hist *h = &s->range;
  uint32_t first;
  *lo = *hi = 0.0;
  (void)omx_loudness_hist_gated(h, s->edge, OMX_LOUDNESS_RANGE_GATE_LU, &first);
  if (first == OMX_LOUDNESS_BINS) { OMX_POST(1, "low-end-not-above-high-end"); return; }
  /* the bins kept are `first` and every one after it: the gate keeps a whole tail */
  uint64_t n = 0u;
  for (uint32_t b = first; b < OMX_LOUDNESS_BINS; b++) n += h->count[b];
  const uint64_t rlo = (uint64_t)((double)(n - 1u) * OMX_LOUDNESS_RANGE_LOW + 0.5);
  const uint64_t rhi = (uint64_t)((double)(n - 1u) * OMX_LOUDNESS_RANGE_HIGH + 0.5);
  uint64_t seen = 0u;
  int got_lo = 0;
  for (uint32_t b = first; b < OMX_LOUDNESS_BINS; b++) {
    if (h->count[b] == 0u) continue;
    seen += h->count[b];
    const double mean = h->energy[b] / (double)h->count[b];
    if (!got_lo && rlo < seen) { *lo = mean; got_lo = 1; }
    if (rhi < seen) { *hi = mean; break; }
  }
  OMX_POST(*lo <= *hi, "low-end-not-above-high-end");
}
#undef OMX_CONTRACT_STAGE

/**
 * @brief The momentary loudness, LUFS.
 * @param s The state.
 * @return omx_loudness_lufs() of omx_loudness_momentary_ms().
 * @note RT-safe: one `log10`. Thread-safe: reads only.
 */
static inline double omx_loudness_momentary(const struct omx_loudness *s) { return omx_loudness_lufs(omx_loudness_momentary_ms(s)); }

/**
 * @brief The short-term loudness, LUFS.
 * @param s The state.
 * @return omx_loudness_lufs() of omx_loudness_short_term_ms().
 * @note RT-safe: one `log10`. Thread-safe: reads only.
 */
static inline double omx_loudness_short_term(const struct omx_loudness *s) { return omx_loudness_lufs(omx_loudness_short_term_ms(s)); }

/**
 * @brief The integrated loudness, LUFS.
 * @param s The state.
 * @return omx_loudness_lufs() of omx_loudness_integrated_ms(); OMX_LOUDNESS_SILENCE when every block is gated.
 * @note RT-safe: one `log10`, one `pow`. Thread-safe: reads only.
 */
static inline double omx_loudness_integrated(const struct omx_loudness *s) { return omx_loudness_lufs(omx_loudness_integrated_ms(s)); }

/**
 * @brief The loudness range, LU: the high end's loudness less the low end's.
 * @param s The state.
 * @return LRA, LU; 0 when no short-term value passes the gates.
 * @note RT-safe: two `log10`, one `pow`. Thread-safe: reads only.
 */
static inline double omx_loudness_range(const struct omx_loudness *s) {
  double lo, hi;
  omx_loudness_range_ms(s, &lo, &hi);
  return lo > 0.0 && hi > 0.0 ? 10.0 * log10(hi / lo) : 0.0;
}

/**
 * @brief The largest true peak since init or reset, linear (1.0 is full scale).
 * @param s The state.
 * @return The largest ×4 reading over every leg.
 * @note RT-safe and thread-safe: reads one word.
 */
static inline float omx_loudness_true_peak(const struct omx_loudness *s) { return s->peak; }

/**
 * @brief The largest true peak since init or reset, dBTP.
 * @param s The state.
 * @return `20·log10(peak)`; OMX_LOUDNESS_SILENCE for no signal.
 * @note RT-safe: one `log10`. Thread-safe: reads only.
 */
static inline double omx_loudness_true_peak_db(const struct omx_loudness *s) {
  return s->peak > 0.0f ? 20.0 * log10((double)s->peak) : OMX_LOUDNESS_SILENCE;
}

#endif /* OMX_LOUDNESS_H */
