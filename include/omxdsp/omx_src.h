// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_src.h
 * @brief Sample-rate conversion, offline only: a Kaiser-windowed-sinc polyphase resampler.
 *
 * Not for a process callback: it allocates nothing, but its work per call is unbounded and its
 * cost per output sample depends on the rate pair. Its one job is converting a file captured at
 * another rate into one at the graph's rate before the transport plays it.
 *
 * The rate pair reduces to L/M (48k -> 192k is 4/1; 192k -> 48k is 1/4; 44.1k -> 48k is 160/147).
 * Conceptually the signal is upsampled by L (zeros inserted), lowpassed, and decimated by M. The
 * polyphase decomposition never computes the zeros: for output n the phase is p = (n*M) mod L and
 * the input index is i = floor(n*M / L), so each output is one dot product of `taps` coefficients
 * against the last `taps` input samples:
 *
 *   y[n] = SUM_k h[p + k*L] * x[i - k]
 *
 * The prototype h is a sinc multiplied by a Kaiser window, whose beta trades stopband attenuation
 * against transition width on a known curve: beta = 0.1102*(A - 8.7) puts the stopband at A dB.
 * The cutoff is 0.5/max(L,M) of the interpolated rate, the lower of the two Nyquists, so an
 * upsample invents nothing above the source's band and a downsample folds nothing back.
 *
 * Each phase's coefficients are scaled to sum to exactly 1, so DC passes at unity through every
 * phase and a converted file comes back at its level. The group delay is compensated: the
 * accumulator starts at `(taps/2)*L`, so output frame 0 corresponds to input frame 0 whatever the
 * rate pair, and the tracks of a multitrack stay aligned.
 *
 * The console's design is omx-contract's src kernel: OMX_SRC_TAPS taps per phase, a window for
 * OMX_SRC_STOPBAND_DB, a passband of OMX_SRC_ROLLOFF_DOUBLE of the shared Nyquist, and at most
 * OMX_SRC_MAX_L phases. Moved from the OpenMixer engine (mix_src.h) unchanged in behaviour:
 * test/golden/engine_kernels.sha256 holds its output to the engine's, bit for bit.
 */
#ifndef OMX_SRC_H
#define OMX_SRC_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <omxcontract/omx_contract_limits.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/**
 * @brief Greatest common divisor, for reducing a rate pair to L/M.
 * @param a One rate.
 * @param b The other.
 * @return gcd(a, b); `a` when `b` is 0.
 * @note RT-safe and thread-safe: pure.
 */
static inline uint32_t omx_src_gcd(uint32_t a, uint32_t b) {
  while (b != 0u) {
    const uint32_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/** @brief One conversion's shape: the reduced ratio and the filter length. */
struct omx_src_plan {
  uint32_t l; /* interpolation factor */
  uint32_t m; /* decimation factor */
  uint32_t taps;
};

/**
 * @brief Reduce a rate pair into a plan.
 *
 * An identity plan (l == m == 1) is a legal answer: it says the rates are the same, and a caller
 * that converts anyway gets its samples back unchanged (the prototype for L=M=1 is a single unit
 * tap), so a conversion path is written once rather than once per case.
 * @param in_rate The source rate, Hz.
 * @param out_rate The destination rate, Hz.
 * @param out The plan, written on success.
 * @return 0 on success; -1 for a zero rate, a NULL plan or an interpolation factor past
 *         OMX_SRC_MAX_L (refused before anything is sized for it).
 * @note Thread-safe: pure over `out`.
 */
static inline int omx_src_plan_of(uint32_t in_rate, uint32_t out_rate, struct omx_src_plan *out) {
  if (in_rate == 0u || out_rate == 0u || out == NULL) return -1;
  const uint32_t g = omx_src_gcd(out_rate, in_rate);
  const uint32_t l = out_rate / g;
  const uint32_t m = in_rate / g;
  if (l > OMX_SRC_MAX_L) return -1;
  out->l = l;
  out->m = m;
  out->taps = (l == 1u && m == 1u) ? 1u : OMX_SRC_TAPS;
  return 0;
}

/**
 * @brief Floats the coefficient table needs: one phase of `taps` per interpolation step.
 * @param p The plan.
 * @return `taps * l`.
 * @note Thread-safe: pure.
 */
static inline size_t omx_src_coef_count(const struct omx_src_plan *p) {
  return (size_t)p->taps * (size_t)p->l;
}

/**
 * @brief Floats the history needs: the last `taps` input samples.
 * @param p The plan.
 * @return `taps`.
 * @note Thread-safe: pure.
 */
static inline size_t omx_src_hist_count(const struct omx_src_plan *p) {
  return (size_t)p->taps;
}

/**
 * @brief Modified Bessel function of the first kind, order 0: the Kaiser window's kernel.
 *
 * The series converges fast for the arguments a window uses (beta <= ~10); 40 terms is far past
 * float precision, and this runs once per coefficient at design time, never per sample.
 * @param x The argument.
 * @return I0(x).
 * @note Thread-safe: pure.
 */
static inline double omx_src_bessel_i0(double x) {
  double sum = 1.0;
  double term = 1.0;
  const double half = x * 0.5;
  for (int k = 1; k < 40; k++) {
    term *= (half / (double)k) * (half / (double)k);
    sum += term;
    if (term < sum * 1e-17) break;
  }
  return sum;
}

/**
 * @brief sin(pi x)/(pi x), the removable singularity at 0 answered rather than divided.
 * @param x The argument.
 * @return sinc(x); 1 for |x| < 1e-12.
 * @note Thread-safe: pure.
 */
static inline double omx_src_sinc(double x) {
  if (x > -1e-12 && x < 1e-12) return 1.0;
  const double pix = M_PI * x;
  return sin(pix) / pix;
}

/**
 * @brief Design the polyphase table into `coef`.
 *
 * Phase-major: `coef[p*taps + k]` is h[p + k*L], so a conversion's inner loop walks one
 * contiguous run of `taps` floats per output. `rolloff` and `stopband_db` are arguments so an
 * oracle can measure the design curve itself (a window designed for 60 dB must not reach 96); the
 * console's one setting is OMX_SRC_ROLLOFF_DOUBLE and OMX_SRC_STOPBAND_DB.
 * @param p The plan.
 * @param coef The table, omx_src_coef_count(p) floats.
 * @param rolloff The passband's share of the shared Nyquist.
 * @param stopband_db The stopband attenuation the window is designed for, dB.
 * @note Not RT: O(taps * l) Bessel series. Thread-safe on distinct tables.
 */
static inline void omx_src_design(const struct omx_src_plan *p, float *coef, double rolloff,
                                  double stopband_db) {
  const uint32_t l = p->l;
  const uint32_t taps = p->taps;
  const size_t n_total = (size_t)taps * (size_t)l;
  if (n_total == 1u) { /* the identity plan: one unit tap, and no window to speak of */
    coef[0] = 1.0f;
    return;
  }
  /* Kaiser's own rule for beta from the stopband attenuation. Below 21 dB the window is a
   * rectangle and beta is 0 — an academic branch here, kept so the function is total. */
  double beta = 0.0;
  if (stopband_db > 50.0) beta = 0.1102 * (stopband_db - 8.7);
  else if (stopband_db >= 21.0) beta = 0.5842 * pow(stopband_db - 21.0, 0.4) + 0.07886 * (stopband_db - 21.0);
  const double denom = omx_src_bessel_i0(beta);
  /* The shared Nyquist, as a fraction of the INTERPOLATED sample rate: an upsample must not
   * invent energy above the source's band, and a downsample must not fold the source's top into
   * the destination's. Both are `0.5 / max(L, M)`. */
  const uint32_t wide = p->l > p->m ? p->l : p->m;
  const double fc = rolloff * 0.5 / (double)wide;
  const double centre = ((double)n_total - 1.0) * 0.5;
  /* The prototype, then the phase-major shuffle. Written straight into `coef` at its final
   * position, so no scratch array is needed and the caller's one buffer is the whole cost. */
  for (uint32_t k = 0; k < taps; k++) {
    for (uint32_t ph = 0; ph < l; ph++) {
      const size_t j = (size_t)ph + (size_t)k * (size_t)l; /* index into the prototype */
      const double d = (double)j - centre;
      const double window_arg = 2.0 * (double)j / ((double)n_total - 1.0) - 1.0;
      const double w = omx_src_bessel_i0(beta * sqrt(fmax(0.0, 1.0 - window_arg * window_arg))) / denom;
      coef[(size_t)ph * (size_t)taps + (size_t)k] = (float)(2.0 * fc * omx_src_sinc(2.0 * fc * d) * w);
    }
  }
  /* PER-PHASE NORMALISATION — see the header. Each phase passes DC at exactly unity, so a
   * converted take comes back at the level it went in at. */
  for (uint32_t ph = 0; ph < l; ph++) {
    double sum = 0.0;
    for (uint32_t k = 0; k < taps; k++) sum += (double)coef[(size_t)ph * (size_t)taps + (size_t)k];
    if (sum > 1e-12 || sum < -1e-12) {
      const float g = (float)(1.0 / sum);
      for (uint32_t k = 0; k < taps; k++) coef[(size_t)ph * (size_t)taps + (size_t)k] *= g;
    }
  }
}

/**
 * @brief One running conversion.
 *
 * The caller owns `coef` (shared across every track of a take: the design depends only on the
 * rate pair) and `hist` (per track: it is the signal's own state).
 */
struct omx_src {
  uint32_t l;
  uint32_t m;
  uint32_t taps;
  const float *coef;
  /* The last `taps` input samples, newest FIRST — so a dot product walks `coef` forward and
   * `hist` forward together, which is what the phase-major layout was chosen for. */
  float *hist;
  /* n*M + (taps/2)*L, where n is the output index. The offset IS the group-delay compensation:
   * it starts the output at the input sample the filter's centre is over. */
  uint64_t acc;
  /* Input samples pushed so far. `acc / l` is the input index the next output needs. */
  uint64_t consumed;
};

/**
 * @brief Start a conversion. `hist` is zeroed here: the signal is silent before it starts.
 * @param s The conversion.
 * @param p The plan.
 * @param coef The designed table, kept by pointer.
 * @param hist The history, omx_src_hist_count(p) floats, kept by pointer.
 * @note Thread-safe on distinct states.
 */
static inline void omx_src_init(struct omx_src *s, const struct omx_src_plan *p, const float *coef,
                                float *hist) {
  s->l = p->l;
  s->m = p->m;
  s->taps = p->taps;
  s->coef = coef;
  s->hist = hist;
  memset(hist, 0, (size_t)p->taps * sizeof(float));
  s->acc = (uint64_t)(p->taps / 2u) * (uint64_t)p->l;
  s->consumed = 0u;
}

/**
 * @brief Output frames a whole file of `in_frames` produces: exact, so a caller sizes an output
 *        file's header before writing a sample.
 * @param in_frames The input length.
 * @param p The plan.
 * @return `in_frames * l / m`.
 * @note Thread-safe: pure.
 */
static inline uint64_t omx_src_out_frames(uint64_t in_frames, const struct omx_src_plan *p) {
  return (in_frames * (uint64_t)p->l) / (uint64_t)p->m;
}

/**
 * @brief Output frames `in_n` inputs can produce in the worst case: what `out` must have room for.
 * @param p The plan.
 * @param in_n The inputs of one call.
 * @return `in_n * l / m + 2`.
 * @note Thread-safe: pure.
 */
static inline uint32_t omx_src_out_capacity(const struct omx_src_plan *p, uint32_t in_n) {
  return (uint32_t)(((uint64_t)in_n * (uint64_t)p->l) / (uint64_t)p->m + 2u);
}

/**
 * @brief One output sample: the dot product of phase `phase`'s coefficients against the history.
 * @param s The conversion.
 * @param phase The phase, below `l`.
 * @return The output sample.
 * @note Thread-safe: reads `s` only.
 */
static inline float omx_src_dot(const struct omx_src *s, uint32_t phase) {
  const float *c = s->coef + (size_t)phase * (size_t)s->taps;
  const float *h = s->hist;
  float acc = 0.0f;
  for (uint32_t k = 0; k < s->taps; k++) acc += c[k] * h[k];
  return acc;
}

/**
 * @brief Push one input sample, newest first.
 * @param s The conversion.
 * @param x The sample.
 * @note O(taps). Thread-safe on distinct states.
 */
static inline void omx_src_push(struct omx_src *s, float x) {
  memmove(s->hist + 1, s->hist, (size_t)(s->taps - 1u) * sizeof(float));
  s->hist[0] = x;
  s->consumed++;
}

/**
 * @brief Emit every output whose input index is now available.
 * @param s The conversion.
 * @param out The output buffer.
 * @param out_cap Its capacity.
 * @param written Outputs already in `out`.
 * @return Outputs in `out` after the call.
 * @note Thread-safe on distinct states.
 */
static inline uint32_t omx_src_drain(struct omx_src *s, float *out, uint32_t out_cap, uint32_t written) {
  while (written < out_cap && (s->acc / (uint64_t)s->l) < s->consumed) {
    out[written++] = omx_src_dot(s, (uint32_t)(s->acc % (uint64_t)s->l));
    s->acc += (uint64_t)s->m;
  }
  return written;
}

/**
 * @brief Convert `in_n` input samples, writing at most `out_cap` outputs.
 *
 * `flush` non-zero says the input has ended: the filter's tail is walked out with zeros, so the
 * last `taps/2` input samples reach the output instead of being stranded in the history.
 * @param s The conversion.
 * @param in The input samples.
 * @param in_n How many.
 * @param out The output buffer.
 * @param out_cap Its capacity.
 * @param flush Non-zero on the last call of a file.
 * @return Outputs written.
 * @note Not RT: O(in_n * taps) plus a memmove per input. Thread-safe on distinct states.
 */
static inline uint32_t omx_src_run(struct omx_src *s, const float *in, uint32_t in_n, float *out,
                                   uint32_t out_cap, int flush) {
  uint32_t written = 0u;
  for (uint32_t i = 0; i < in_n; i++) {
    omx_src_push(s, in[i]);
    written = omx_src_drain(s, out, out_cap, written);
  }
  if (flush) {
    for (uint32_t i = 0; i < s->taps && written < out_cap; i++) {
      omx_src_push(s, 0.0f);
      written = omx_src_drain(s, out, out_cap, written);
    }
  }
  return written;
}

#endif /* OMX_SRC_H */
