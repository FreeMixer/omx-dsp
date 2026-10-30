/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_rta.h — the RTA's capture ring and its magnitude spectrum, the two pieces every analysis
 * engine reads its audio through: the ring an audio callback writes a block into, and the one
 * windowed FFT that turns a window of it into the linear magnitudes FBS and HRP measure.
 *
 * Moved from the OpenMixer engine (mix_dsp.h), unchanged in its code. The ring is a
 * SINGLE-WRITER / SINGLE-READER structure with no lock: `pos` and `filled` are what the reader
 * gates a read on, and `buf` is what the read actually returns — so the samples go in FIRST, and
 * `pos`/`filled` publish with a RELEASE store only once they are true. `omx_ring_has` and the
 * pos load in {@link omx_ring_read_at} pair with an ACQUIRE, so a reader that sees the count or
 * the cursor has also seen the samples they claim. No FFT, window or allocation runs on the
 * writer's side.
 *
 * The analyser's other readings sit here too, also moved from mix_dsp.h: the complex spectrum
 * ({@link omx_rta_spectrum_complex}) and the half-band ÷2 decimator the low-frequency zoom runs
 * ({@link omx_decimate_halfband}), whose table and interior dot product are omx_oversampler.h's.
 * Not in the umbrella header: omx_fft.h includes the umbrella. Include this header by name.
 */
#ifndef OMX_RTA_H
#define OMX_RTA_H

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omxdsp_definitions.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_fft.h>
#include <omxdsp/omx_oversampler.h>

/** The spectrum RESOLUTION the console commits to — the one number every analysis consumer
 * shares (ONE spectrum stream per tap, sized to the most
 * demanding operation, never parallel tiers). ~5.9 Hz/bin is both the display requirement
 * and the measured knee for
 * harmonic detection (trombone corpus, 2026-08-18): twice as coarse mis-fits the comb below
 * C3 and detection drowns in sub-octave phantoms; finer needs a window so long that fast
 * passages smear away (1.46 Hz/bin ⇒ 683 ms ⇒ 34 of 236 note events survive). The FFT
 * LENGTH is therefore derived from the graph rate ({@link omx_rta_fft_size_for}) — pinning
 * the length instead was how moving the rig to 96 kHz silently halved the resolution.
 * OMX_RTA_TARGET_BIN_HZ comes from the generated omxdsp_definitions.h (declared once, in core). */

/** The largest FFT the buffers hold: 192 kHz — the console's declared rate ceiling
 * — at the target resolution. A power of two. OMX_RTA_FFT_SIZE_MAX comes from the generated
 * omxdsp_definitions.h (declared once, in core). */

/** The FFT length for `rate`: the smallest power of two whose bins are at or under
 * {@link OMX_RTA_TARGET_BIN_HZ} (44.1/48 kHz → 8192, 88.2/96 kHz → 16384, 176.4/192 kHz →
 * 32768). Window time stays ~171 ms at every rate — resolution AND responsiveness are both
 * rate-invariant, which is the point of deriving. */
static inline uint32_t omx_rta_fft_size_for(uint32_t rate) {
  uint32_t n = 2048;
  while (n < OMX_RTA_FFT_SIZE_MAX && (float)rate / (float)n > OMX_RTA_TARGET_BIN_HZ) n *= 2;
  return n;
}

/** The display's LOW-frequency ZOOM: a fixed 8192-point FFT over a ÷8-decimated copy of
 * the SAME tap — sub-2 Hz bins at every graph rate (48 kHz → 0.73, 96 kHz → 1.46) exactly
 * where the log display's bottom decades starve on the main spectrum's linear grid. It is
 * DISPLAY-ONLY and rides the same tap value (one stream, one row — the operator's one-
 * spectrum decision holds ON THE WIRE); detection (FBS/HRP) keeps the main spectrum, whose
 * ~171 ms window measured 7× the low tier's event recall. Fixed size on purpose: the zoom
 * is a resolution floor for eyes, not a rate-scaled analysis window, and fixing it keeps
 * the ring and scratch costs rate-independent. */
#define OMX_RTA_LOW_FFT_SIZE 8192
/** How many cascaded ÷2 half-band decimation stages the LOW zoom runs — see
 * {@link omx_decimate_halfband}'s doc for why three CHEAP ÷2 stages, not one ÷8 FIR. */
#define OMX_RTA_LOW_DECIM_STAGES 3
/** 2^{@link OMX_RTA_LOW_DECIM_STAGES} — the LOW zoom's overall decimation factor. */
#define OMX_RTA_LOW_DECIM_FACTOR 8
/** The RAW (ring-rate) sample window the LOW zoom reads: three ÷2 stages hand
 * `omx_rta_spectrum` back exactly {@link OMX_RTA_LOW_FFT_SIZE} decimated samples. The cost
 * is LATENCY on the zoom's own reading only (~683 ms @96 kHz), never RT load. */
#define OMX_RTA_LOW_WINDOW (OMX_RTA_LOW_FFT_SIZE * OMX_RTA_LOW_DECIM_FACTOR)

/** Ring capacity per leg: 2× the LARGER of the two readers' windows — the LOW zoom's
 * {@link OMX_RTA_LOW_WINDOW} (65536) exceeds even the 192 kHz main window
 * ({@link OMX_RTA_FFT_SIZE_MAX}), the same "a read never races a write all the way around
 * the buffer" margin the ring always carried. */
#define OMX_RTA_RING_SIZE (OMX_RTA_LOW_WINDOW * 2)

/** One leg's capture ring (MAIN gets two instances, L+R; a watched strip likewise). */
struct omx_ring {
  float buf[OMX_RTA_RING_SIZE];
  _Atomic uint32_t pos; /* next write index, mod OMX_RTA_RING_SIZE — RELEASE-stored after buf */
  /* How many samples have been written since {@link omx_ring_reset}, SATURATING at the ring's
   * capacity. It is what tells a reader whether the window it is about to take is audio or a
   * ring that is still part silence — see {@link omx_ring_has}. Written by the RT thread with
   * a RELEASE store, after `buf` and `pos` (single writer, no read-modify-write), read by the
   * JS thread with ACQUIRE so a `filled` a reader trusts has the samples it counts behind it. */
  _Atomic uint32_t filled;
  /* How many samples the writer has CLAIMED since the ring was made — monotonic, wrapping at
   * 2^32, never reset. Advanced by `n` and fenced BEFORE {@link omx_ring_write} touches `buf`
   * (the seqlock writer's "odd first", per ring), so a reader that copied any sample of a write
   * sees that write's claim once it fences after the copy. {@link omx_ring_latch4} latches it
   * beside `pos`; {@link omx_ring_window_intact} re-reads it after the copy — the check that the
   * window the latch proved was not LAPPED while the reader was copying it ("the copy is inside the proof"). */
  _Atomic uint32_t head;
};

/*
 * Start ring `ring` EMPTY without clearing its buffer — the control thread's door, taken when a
 * lane is armed after a spell disarmed. With `pos` and `filled` both at zero, the newest `n`
 * samples are all post-reset audio as soon as `filled` reaches `n`, so the samples the previous
 * arming left behind can never be read: {@link omx_ring_has} refuses the window until then.
 * Call it only while nothing is writing the ring (a disarmed lane), and publish the ring
 * afterwards, so the RT thread sees the zeroed counters before the pointer.
 */
static inline void omx_ring_reset(struct omx_ring *ring) {
  atomic_store_explicit(&ring->pos, 0, memory_order_relaxed);
  atomic_store_explicit(&ring->filled, 0, memory_order_relaxed);
}

/** How many real samples the ring holds since its reset, capped at its capacity. ACQUIRE: a
 * caller that gates a read on this count must also see the samples the count describes. */
static inline uint32_t omx_ring_filled(const struct omx_ring *ring) {
  return atomic_load_explicit(&ring->filled, memory_order_acquire);
}

/** Whether a window of `n` samples read off this ring is entirely audio written since the reset
 * — the fact a reader gates a frame on. */
static inline bool omx_ring_has(const struct omx_ring *ring, uint32_t n) {
  return omx_ring_filled(ring) >= n;
}

/*
 * Append `n` samples to the ring (RT thread, called from on_process). Handles wraparound
 * and — defensively, for an oversized quantum — `n` greater than the ring's capacity (only
 * the most recent OMX_RTA_RING_SIZE samples survive). RT-safe: fixed-work memcpy(s), no
 * allocation, no branch beyond the wrap/oversize checks.
 */
#define OMX_CONTRACT_STAGE "dsp/ring-write"
static inline void omx_ring_write(struct omx_ring *ring, const float *in, uint32_t n) {
  if (n == 0) return;
  /* THE CLAIM BEFORE THE SAMPLES — the one store that has to lead `buf` rather than follow it:
   * a reader whose copy overlaps these samples must be able to tell, after its copy, that the
   * writer got there ({@link omx_ring_window_intact}). The release FENCE orders the claim ahead
   * of every store below it (a release store alone orders only what came BEFORE it). */
  atomic_store_explicit(&ring->head, atomic_load_explicit(&ring->head, memory_order_relaxed) + n,
                        memory_order_relaxed);
  atomic_thread_fence(memory_order_release);
  /* THE SAMPLES FIRST — `buf` is plain memory, no atomic touches it, so nothing here
   * synchronises with a reader unless the counters published AFTER it say so. A reader that trusts `pos`/`filled` before the
   * memcpy they describe has landed would read whatever `buf` held from the PREVIOUS write —
   * the ordering the fix corrects, not a cosmetic tear. */
  uint32_t filled = atomic_load_explicit(&ring->filled, memory_order_relaxed);
  uint32_t pos = atomic_load_explicit(&ring->pos, memory_order_relaxed);
  if (n >= OMX_RTA_RING_SIZE) {
    memcpy(ring->buf, in + (n - OMX_RTA_RING_SIZE), OMX_RTA_RING_SIZE * sizeof(float));
    /* `pos` before `filled`: both are RELEASE, so either order a reader could observe them in
     * is safe, but a reader that sees `filled` reach `n` must already be able to read a whole
     * window at `pos`, and `pos` is what that window is anchored on. */
    atomic_store_explicit(&ring->pos, 0, memory_order_release);
    atomic_store_explicit(&ring->filled, OMX_RTA_RING_SIZE, memory_order_release);
    /* invariant: the ring's own cursors never leave [0, OMX_RTA_RING_SIZE] -- read back the
     * stores, not the constants, so a future edit that stores the wrong value is caught. */
    OMX_INVARIANT(atomic_load_explicit(&ring->pos, memory_order_relaxed) < OMX_RTA_RING_SIZE &&
                  atomic_load_explicit(&ring->filled, memory_order_relaxed) <= OMX_RTA_RING_SIZE,
                  "ring-cursors-in-bounds");
    return;
  }
  uint32_t first = OMX_RTA_RING_SIZE - pos;
  if (first > n) first = n;
  memcpy(ring->buf + pos, in, first * sizeof(float));
  uint32_t rem = n - first;
  if (rem > 0) memcpy(ring->buf, in + first, rem * sizeof(float));
  /* THE COUNTERS, RELEASE, ONLY NOW THAT THE SAMPLES ARE IN PLACE. */
  atomic_store_explicit(&ring->pos, (pos + n) % OMX_RTA_RING_SIZE, memory_order_release);
  if (filled < OMX_RTA_RING_SIZE) {
    uint32_t room = OMX_RTA_RING_SIZE - filled;
    atomic_store_explicit(&ring->filled, n >= room ? OMX_RTA_RING_SIZE : filled + n,
                          memory_order_release);
  }
  OMX_INVARIANT(atomic_load_explicit(&ring->pos, memory_order_relaxed) < OMX_RTA_RING_SIZE &&
                atomic_load_explicit(&ring->filled, memory_order_relaxed) <= OMX_RTA_RING_SIZE,
                "ring-cursors-in-bounds");
}
#undef OMX_CONTRACT_STAGE

/*
 * Read `n` samples ending `back` samples BEFORE the newest one, in chronological order
 * (oldest first) into `out` — the JS-thread side, called from mixerRta and mixerAlignPair,
 * never from on_process. A silent (never-written) ring reads back all zero, which FFTs to a
 * silent spectrum.
 *
 * WHY A READER WOULD WANT THE PAST. Two lanes measured
 * against each other are snapshotted at the same instant, so a signal that reaches one of
 * them LATE puts only `n - delay` samples of shared content in the two windows — and the
 * coherence a perfect measurement can reach is capped by that overlap alone, to 0.5 at a
 * delay of 0.228 windows and to nothing at a whole one. Reading the EARLY lane `back = delay`
 * samples into its own past restores the overlap, and the fit then measures a residual
 * instead of the delay. The ring is deliberately larger than any one window
 * ({@link OMX_RTA_RING_SIZE} is 131072 against a 16384 window at 96 kHz), so this is history
 * the ring already holds, not a new buffer.
 *
 * `n + back` past the ring's capacity would read samples the writer has already overwritten,
 * so it is REFUSED — `out` is zeroed rather than filled with whatever is there. Silence
 * reads as an absent spectrum, which the fit refuses by name; plausible garbage would not.
 */
static inline void omx_ring_read_from(const struct omx_ring *ring, float *out, uint32_t n,
                                     uint32_t pos, uint32_t back) {
  if (n > OMX_RTA_RING_SIZE) n = OMX_RTA_RING_SIZE;
  if (back > OMX_RTA_RING_SIZE - n) {
    memset(out, 0, n * sizeof(float));
    return;
  }
  uint32_t start = (pos + OMX_RTA_RING_SIZE - n - back) % OMX_RTA_RING_SIZE;
  uint32_t first = OMX_RTA_RING_SIZE - start;
  if (first > n) first = n;
  memcpy(out, ring->buf + start, first * sizeof(float));
  uint32_t rem = n - first;
  if (rem > 0) memcpy(out + first, ring->buf, rem * sizeof(float));
}

/* {@link omx_ring_read_from} anchored on this ring's own cursor, read now. ACQUIRE: pairs with
 * the RELEASE store in {@link omx_ring_write} so the samples this cursor points at are visible
 * before the read that follows it. */
static inline void omx_ring_read_at(const struct omx_ring *ring, float *out, uint32_t n,
                                    uint32_t back) {
  omx_ring_read_from(ring, out, n, atomic_load_explicit(&ring->pos, memory_order_acquire), back);
}

/* The newest `n` samples — {@link omx_ring_read_at} with no history asked for. */
static inline void omx_ring_read_latest(const struct omx_ring *ring, float *out, uint32_t n) {
  omx_ring_read_at(ring, out, n, 0);
}

/* The Hann window and the real/complex f32 FFTs the RTA readings below run on are the library's
 * (<omxdsp/omx_fft.h>: omx_hann_window, omx_fft_radix2, omx_fft_real_half). */

/*
 * Linear-magnitude spectrum of a REAL block: windows a COPY into `work_re`/`work_im`
 * (n-length scratch, caller-owned — no allocation here), FFTs it, and writes n/2 magnitude
 * bins to `mags[k]` = amplitude at `k · sampleRate / n` Hz. The `4/n` scale corrects for
 * the Hann window's 0.5 coherent gain (amplitude = |bin| · 2/(n·gain) = |bin| · 4/n) so a
 * full-scale, bin-aligned sinusoid reads back at ~unity magnitude (0 dBFS) — see
 * {@link mix_dsp.test.c}'s known-sinusoid test. `n` must be a power of two ≥ 2. From n ≥ 4
 * the transform is the real-input half-length one ({@link omx_fft_real_half}); n = 2 keeps
 * the plain complex transform.
 */
/* Window + transform into `work_re`/`work_im`, unscaled. The ONE window and the ONE FFT
 * both readings below share: a magnitude spectrum and a complex spectrum differ only in
 * what they keep afterwards, and computing the transform twice would be a second answer to
 * the same question. */
static inline void omx_rta_transform(const float *block, uint32_t n, float *work_re,
                                     float *work_im) {
  for (uint32_t i = 0; i < n; i++) work_re[i] = block[i];
  omx_hann_window(work_re, n);
  if (n >= 4) {
    omx_fft_real_half(work_re, work_im, n);
  } else {
    for (uint32_t i = 0; i < n; i++) work_im[i] = 0.0f;
    omx_fft_radix2(work_re, work_im, n);
  }
}

static inline void omx_rta_spectrum(const float *block, uint32_t n, float *work_re,
                                    float *work_im, float *mags) {
  omx_rta_transform(block, n, work_re, work_im);
  const float scale = 4.0f / (float)n;
  for (uint32_t k = 0; k < n / 2; k++) {
    float re = work_re[k], im = work_im[k];
    mags[k] = sqrtf(re * re + im * im) * scale;
  }
}

/**
 * @brief The same transform keeping phase: `out_re[k]`/`out_im[k]` for k < n/2, scaled as
 *        omx_rta_spectrum scales magnitude, so `hypot(out_re[k], out_im[k]) == mags[k]`.
 *
 * A pure delay leaves every magnitude unchanged and shows only as a phase ramp, -2πfτ, so
 * measuring a delay between two signals needs this reading, and averaging it across frames lets
 * a stable phase relationship add and an unstable one cancel (coherence).
 * @param block The block, `n` samples, read only.
 * @param n Its length, a power of two ≥ 2.
 * @param work_re Scratch, `n` floats.
 * @param work_im Scratch, `n` floats.
 * @param out_re The real parts out, `n/2` floats; may alias `work_re`.
 * @param out_im The imaginary parts out, `n/2` floats; may alias `work_im`.
 * @note Allocation-free, O(n log n). Thread-safe on distinct scratch.
 */
static inline void omx_rta_spectrum_complex(const float *block, uint32_t n, float *work_re,
                                            float *work_im, float *out_re, float *out_im) {
  omx_rta_transform(block, n, work_re, work_im);
  const float scale = 4.0f / (float)n;
  for (uint32_t k = 0; k < n / 2; k++) {
    out_re[k] = work_re[k] * scale;
    out_im[k] = work_im[k] * scale;
  }
}

/**
 * @brief One decimated output whose half-band kernel reaches outside `[0, last]`: every tap's
 *        index clamped to the block.
 * @param in The block.
 * @param c The output's centre sample, an even index.
 * @param last The block's last index.
 * @return The filtered sample.
 * @note Allocation-free: OMX_HALFBAND_ODD_TAPS clamped tap pairs. Thread-safe: pure.
 */
static inline float omx_decimate_halfband_edge(const float *in, int32_t c, int32_t last) {
  float acc = OMX_HALFBAND_CENTER * in[c];
  for (uint32_t i = 0; i < OMX_HALFBAND_ODD_TAPS; i++) {
    int32_t k = (int32_t)(2 * i + 1);
    int32_t lo = c - k;
    if (lo < 0) lo = 0;
    int32_t hi = c + k;
    if (hi > last) hi = last;
    acc += OMX_HALFBAND_ODD_COEF[i] * (in[lo] + in[hi]);
  }
  return acc;
}

/**
 * @brief One half-band ÷2 decimation stage of a block: the FIR fused with the downsample, so only
 *        every other filtered sample is computed.
 *
 * `n_in` samples of `in` give `n_in/2` samples of `out` (a trailing odd sample is dropped). Where
 * the kernel would reach outside the block, the edge sample repeats rather than zero-padding: the
 * block is a snapshot of recent history with no true silence at its edges, and the clamp touches
 * only the ~24 taps' worth of samples nearest each edge. The interior is omx_halfband_dot, the one
 * kernel that reads the table, which the compiler can vectorise across outputs.
 * @param in The block, `n_in` samples.
 * @param n_in Its length; below 2 nothing is written.
 * @param out `n_in/2` outputs; must not overlap `in`.
 * @return Outputs written, `n_in/2`.
 * @note Allocation-free, O(n_in), no state. Thread-safe on distinct buffers.
 */
static inline uint32_t omx_decimate_halfband(const float *in, uint32_t n_in, float *out) {
  if (n_in < 2) return 0;
  const uint32_t n_out = n_in / 2;
  const int32_t last = (int32_t)n_in - 1;
  /* The kernel's reach: 2·OMX_HALFBAND_ODD_TAPS − 1 samples either side of the centre. */
  const int32_t reach = 2 * OMX_HALFBAND_ODD_TAPS - 1;
  /* Outputs whose kernel stays inside the block: centre c = 2o with c − reach ≥ 0 and
   * c + reach ≤ last. Everything else takes the clamped edge path. */
  uint32_t o_lo = (uint32_t)((reach + 1) / 2);
  uint32_t o_hi = (last - reach) >= 0 ? (uint32_t)((last - reach) / 2) + 1 : 0;
  if (o_hi < o_lo) o_hi = o_lo;
  if (o_lo > n_out) o_lo = n_out;
  if (o_hi > n_out) o_hi = n_out;
  for (uint32_t o = 0; o < o_lo; o++) out[o] = omx_decimate_halfband_edge(in, (int32_t)(o * 2), last);
  for (uint32_t o = o_lo; o < o_hi; o++) out[o] = omx_halfband_dot(in + o * 2);
  for (uint32_t o = o_hi; o < n_out; o++) out[o] = omx_decimate_halfband_edge(in, (int32_t)(o * 2), last);
  return n_out;
}

#endif /* OMX_RTA_H */
