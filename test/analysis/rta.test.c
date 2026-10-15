// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * rta.test.c — the RTA capture ring and spectrum (<omxdsp/analysis/omx_rta.h>), the oracles that
 * came with them from the engine's mix_dsp.test.c:
 *
 *   1. the ring reads back what was written, oldest first, across a wrap and an oversized write;
 *   2. its fill count saturates at the capacity, a reset starts it over, and the window it gates
 *      is the end of the Hann ramp a part-filled window reads (closed form, to 0.2 dB);
 *   3. the FFT length holds the declared bin width at every declared rate;
 *   4. a bin-aligned full-scale sinusoid lands in its own bin near unity, and silence FFTs to a
 *      silent spectrum.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/analysis/omx_rta.h>

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

static void close_to(float got, float want, const char *what) {
  g_checks++;
  float d = fabsf(got - want);
  if (d > 1e-5f) {
    g_fail++;
    fprintf(stderr, "FAIL: %s — got %.7f want %.7f (|d|=%.2e)\n", what, got, want, d);
  }
}

static float sum_sq(const float *x, uint32_t n) {
  float s = 0.0f;
  for (uint32_t i = 0; i < n; i++) s += x[i] * x[i];
  return s;
}

static void test_ring_write_read_basic(void) {
  struct omx_ring ring;
  memset(&ring, 0, sizeof(ring));
  atomic_store_explicit(&ring.pos, 0, memory_order_relaxed);

  float chunk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  omx_ring_write(&ring, chunk, 8);

  /* reading back exactly what was written, chronological (oldest..newest). */
  float out[8] = {0};
  omx_ring_read_latest(&ring, out, 8);
  for (int i = 0; i < 8; i++) close_to(out[i], chunk[i], "ring basic read == what was written");

  /* reading fewer than were written returns the MOST RECENT tail. */
  float out2[3] = {0};
  omx_ring_read_latest(&ring, out2, 3);
  close_to(out2[0], 6.0f, "ring tail read[0] is the 3rd-from-last sample");
  close_to(out2[1], 7.0f, "ring tail read[1]");
  close_to(out2[2], 8.0f, "ring tail read[2] is the most recent sample");
}

static void test_ring_wraparound(void) {
  struct omx_ring ring;
  memset(&ring, 0, sizeof(ring));
  atomic_store_explicit(&ring.pos, 0, memory_order_relaxed);

  /* Fill the ring exactly, then write a bit more (wrapping): the read-back must be the
   * LATEST OMX_RTA_RING_SIZE samples, chronologically ordered, not the stale head. */
  static float fill[OMX_RTA_RING_SIZE];
  for (uint32_t i = 0; i < OMX_RTA_RING_SIZE; i++) fill[i] = (float)i;
  omx_ring_write(&ring, fill, OMX_RTA_RING_SIZE);

  float extra[5] = {-1, -2, -3, -4, -5};
  omx_ring_write(&ring, extra, 5);

  float out[10] = {0};
  omx_ring_read_latest(&ring, out, 10);
  /* the last 5 of `fill` (RING_SIZE-5 .. RING_SIZE-1) precede the 5 `extra` samples. */
  for (int i = 0; i < 5; i++) {
    close_to(out[i], (float)(OMX_RTA_RING_SIZE - 5 + i), "ring wraparound: tail of the old fill survives");
  }
  close_to(out[5], -1.0f, "ring wraparound: first extra sample lands right after the old tail");
  close_to(out[9], -5.0f, "ring wraparound: most recent sample is last");
}

static void test_ring_oversized_write(void) {
  struct omx_ring ring;
  memset(&ring, 0, sizeof(ring));
  atomic_store_explicit(&ring.pos, 0, memory_order_relaxed);

  /* A single write LARGER than the whole ring: only the tail (the most recent
   * OMX_RTA_RING_SIZE samples of the input) survives — an oversized PipeWire quantum must
   * never corrupt the ring or read stale/garbage data. */
  static float big[OMX_RTA_RING_SIZE + 100];
  for (uint32_t i = 0; i < OMX_RTA_RING_SIZE + 100; i++) big[i] = (float)i;
  omx_ring_write(&ring, big, OMX_RTA_RING_SIZE + 100);

  float out[4] = {0};
  omx_ring_read_latest(&ring, out, 4);
  close_to(out[3], (float)(OMX_RTA_RING_SIZE + 100 - 1), "oversized write: most recent sample survives");
  close_to(out[0], (float)(OMX_RTA_RING_SIZE + 100 - 4), "oversized write: only the tail survives");
}

/* ---- RTA: the first-frame gate ------------ */

static void test_ring_fill_count_saturates_and_resets(void) {
  static struct omx_ring ring;
  memset(&ring, 0, sizeof(ring));

  check(omx_ring_filled(&ring) == 0, "fill count: a fresh ring holds nothing");
  check(!omx_ring_has(&ring, 1), "fill count: a fresh ring cannot answer a one-sample window");

  static float chunk[100];
  for (uint32_t i = 0; i < 100; i++) chunk[i] = 1.0f;
  omx_ring_write(&ring, chunk, 100);
  check(omx_ring_filled(&ring) == 100, "fill count: counts exactly what was written");
  check(omx_ring_has(&ring, 100), "fill count: 100 samples answer a 100-sample window");
  check(!omx_ring_has(&ring, 101), "fill count: 100 samples do not answer a 101-sample window");

  /* SATURATION at the ring's capacity: a ring that has been running for an hour holds one
   * ring of audio, not an hour of it, and no reader asks for more than one window. */
  static float fill[OMX_RTA_RING_SIZE];
  for (uint32_t i = 0; i < OMX_RTA_RING_SIZE; i++) fill[i] = 2.0f;
  omx_ring_write(&ring, fill, OMX_RTA_RING_SIZE);
  check(omx_ring_filled(&ring) == OMX_RTA_RING_SIZE, "fill count: saturates at the capacity");
  omx_ring_write(&ring, fill, OMX_RTA_RING_SIZE);
  check(omx_ring_filled(&ring) == OMX_RTA_RING_SIZE, "fill count: stays at the capacity");

  /* RESET leaves the buffer alone and still cannot hand back a pre-reset sample: `pos` starts
   * over with the count, so the newest window is post-reset audio the moment the count allows
   * it to be read at all. */
  omx_ring_reset(&ring);
  check(omx_ring_filled(&ring) == 0, "reset: the count starts over");
  check(!omx_ring_has(&ring, 1), "reset: no window is available until samples arrive");
  static float after[64];
  for (uint32_t i = 0; i < 64; i++) after[i] = 3.0f;
  omx_ring_write(&ring, after, 64);
  check(omx_ring_has(&ring, 64), "reset: 64 new samples answer a 64-sample window");
  check(!omx_ring_has(&ring, 65), "reset: the pre-reset samples do not count toward a window");
  float out[64] = {0};
  omx_ring_read_latest(&ring, out, 64);
  close_to(out[0], 3.0f, "reset: the readable window is post-reset audio, oldest sample");
  close_to(out[63], 3.0f, "reset: the readable window is post-reset audio, newest sample");
}

/*
 * THE RAMP, AND WHY THE GATE IS THE COUNT. A ring that starts empty and is fed a steady tone
 * reads LOW until it has held one whole window, because the FFT's Hann window weights a block
 * that is still part silence. How low is a CLOSED FORM: the reading is the fraction of the
 * window's Hann mass that the tone covers, and the tone always covers the NEWEST end.
 *
 *   window filled | Hann mass covered | reading against settled
 *   n/8           | 0.01246           | -38.10 dB
 *   n/4           | 0.09083           | -20.84 dB
 *   n/2           | 0.50000           |  -6.02 dB
 *   3n/4          | 0.90847           |  -0.83 dB
 *   n             | 1.00000           |   0.00 dB
 *
 * The -6.02 dB at a half-filled window is the figure a full server run once produced as a
 * MAIN baseline and nearly filed as a doubled leg. It is a window, not the audio.
 *
 * omx_ring_has is false at every one of those partial fills and true at the last, so a reader
 * that gates on it never publishes a point on this ramp.
 */
static void test_ring_fill_gate_is_the_end_of_the_ramp(void) {
  static struct omx_ring ring;
  memset(&ring, 0, sizeof(ring));

  const uint32_t rate = 48000;
  const uint32_t n = omx_rta_fft_size_for(rate);
  check(n == 8192, "ramp oracle: 48 kHz derives an 8192-point window");

  /* A BIN-ALIGNED unit sine, so the settled reading is one magnitude bin at ~1.0 and the
   * comparison is a pure ratio with no scalloping to explain away. */
  const uint32_t bin = 171; /* 171 * 48000 / 8192 = 1002 Hz */
  const float step = 2.0f * (float)M_PI * (float)bin / (float)n;

  static float block[OMX_RTA_FFT_SIZE_MAX];
  static float work_re[OMX_RTA_FFT_SIZE_MAX];
  static float work_im[OMX_RTA_FFT_SIZE_MAX];
  static float mags[OMX_RTA_FFT_SIZE_MAX / 2];
  static float chunk[256];

  const uint32_t marks[5] = {8192 / 8, 8192 / 4, 8192 / 2, 3 * 8192 / 4, 8192};
  const float want_db[5] = {-38.099f, -20.837f, -6.021f, -0.827f, 0.0f};
  const char *names[5] = {"an eighth-filled window reads 38.1 dB low",
                          "a quarter-filled window reads 20.8 dB low",
                          "a half-filled window reads 6.02 dB low",
                          "a three-quarter-filled window reads 0.83 dB low",
                          "a whole window reads its settled value"};
  float got[5] = {0};

  float phase = 0.0f;
  uint32_t written = 0;
  uint32_t mark = 0;
  float settled = 0.0f;
  while (written < 2 * n) {
    for (uint32_t i = 0; i < 256; i++) {
      chunk[i] = sinf(phase);
      phase += step;
    }
    omx_ring_write(&ring, chunk, 256);
    written += 256;
    if (mark < 5 && written == marks[mark]) {
      omx_ring_read_latest(&ring, block, n);
      omx_rta_spectrum(block, n, work_re, work_im, mags);
      got[mark] = mags[bin];
      /* THE GATE: false for every partial window, true exactly when the window is whole. */
      check(omx_ring_has(&ring, n) == (marks[mark] == n), names[mark]);
      mark++;
    }
  }
  check(mark == 5, "ramp oracle: every checkpoint was measured");
  settled = got[4];
  check(settled > 0.5f, "ramp oracle: the settled reading is a SIGNAL, not a floor");

  for (uint32_t k = 0; k < 5; k++) {
    float db = 20.0f * log10f(got[k] / settled);
    g_checks++;
    if (fabsf(db - want_db[k]) > 0.2f) {
      g_fail++;
      fprintf(stderr, "FAIL: %s — got %.3f dB want %.3f dB\n", names[k], (double)db,
              (double)want_db[k]);
    }
  }

  /* And the frame a gated reader DOES publish is the settled one: the whole window and the
   * window two rings later agree to within 0.01 dB. */
  omx_ring_read_latest(&ring, block, n);
  omx_rta_spectrum(block, n, work_re, work_im, mags);
  float drift = 20.0f * log10f(mags[bin] / settled);
  /* 0.05 dB, not zero: the two windows start at different samples, so their leakage floors
   * differ in the last few float digits (measured 0.025 dB). The ramp it stands against is
   * 38 dB tall. */
  check(fabsf(drift) < 0.05f, "ramp oracle: past the gate the reading holds within 0.05 dB");
}

static void test_fft_size_for_rate_ladder(void) {
  /* The ONE resolution commitment, pinned at every rate the console admits: bins at or
   * under OMX_RTA_TARGET_BIN_HZ, window unchanged in TIME (~171 ms) because the length
   * doubles with the rate. Pinning the LENGTH instead is the defect this forbids: the
   * rig moving 48 kHz -> 96 kHz silently halved the resolution and harmonic detection
   * drowned (measured 2026-08-18, trombone corpus). */
  /* The pinned length per declared rate, in OMX_DECLARED_RATES order (the fastest lands on the
   * buffer ceiling); the table's length is checked against the declared count. */
  static const uint32_t PINNED_FFT[] = {8192u, 8192u, 16384u, 16384u, 32768u, 32768u};
  _Static_assert(sizeof PINNED_FFT / sizeof PINNED_FFT[0] == OMX_DECLARED_RATE_COUNT,
                 "one pinned fft length per declared rate");
  for (uint32_t i = 0; i < OMX_DECLARED_RATE_COUNT; i++) {
    const uint32_t rate = (uint32_t)OMX_DECLARED_RATES[i];
    const uint32_t n = omx_rta_fft_size_for(rate);
    char what[96];
    snprintf(what, sizeof what, "fft length @%u Hz = %u", rate, PINNED_FFT[i]);
    check(n == PINNED_FFT[i], what);
    check(n <= OMX_RTA_FFT_SIZE_MAX, "fft length never exceeds the buffers");
    check((float)rate / (float)n <= OMX_RTA_TARGET_BIN_HZ + 1e-3f,
          "bins at or under the target resolution at every admitted rate");
  }
}

static void test_fft_known_sinusoid(void) {
  /* A BIN-ALIGNED 1500 Hz tone at 48 kHz / the rate-derived N=8192 (k0 = 1500*8192/48000 =
   * 256 exactly), full scale (amplitude 1.0). Bin alignment means no spectral leakage
   * smearing the peak, so the test can assert BOTH the exact band AND the level tightly —
   * "a known sinusoid lands in the right band at the right level". */
  const float sr = 48000.0f;
  const uint32_t n = omx_rta_fft_size_for((uint32_t)sr);
  const uint32_t k0 = 256;
  const float f0 = (float)k0 * sr / (float)n; /* exactly 1500 Hz */

  static float block[OMX_RTA_FFT_SIZE_MAX];
  for (uint32_t i = 0; i < n; i++) block[i] = sinf(2.0f * (float)M_PI * f0 * (float)i / sr);

  static float work_re[OMX_RTA_FFT_SIZE_MAX];
  static float work_im[OMX_RTA_FFT_SIZE_MAX];
  static float mags[OMX_RTA_FFT_SIZE_MAX / 2];
  omx_rta_spectrum(block, n, work_re, work_im, mags);

  check(sum_sq(mags, n / 2) > 0.0f, "the spectrum is not degenerate/all-zero");

  /* the peak AC bin (skip DC) is exactly k0 — "lands in the right band". */
  uint32_t peak = 1;
  for (uint32_t k = 1; k < n / 2; k++) {
    if (mags[k] > mags[peak]) peak = k;
  }
  check(peak == k0, "a bin-aligned 1500 Hz tone peaks at exactly its own FFT bin");

  /* the level at that bin recovers the input amplitude (1.0 = 0 dBFS) — "at the right
   * level": the Hann coherent-gain correction (4/n) makes a full-scale bin-aligned tone
   * read back near unity, well inside a generous tolerance for float32 + a finite window. */
  check(mags[k0] > 0.85f && mags[k0] < 1.15f,
        "a full-scale bin-aligned tone reads back near unity magnitude (~0 dBFS)");

  /* a bin far from the tone carries only the window's sidelobe leakage, an order of
   * magnitude below the peak — "the right band" also means NOT smeared across the plot. */
  check(mags[k0 + 200] < mags[k0] * 0.05f, "a distant bin carries only sidelobe leakage, not signal");

  /* silence in, silence out. */
  static float silence[OMX_RTA_FFT_SIZE_MAX] = {0};
  static float smags[OMX_RTA_FFT_SIZE_MAX / 2];
  omx_rta_spectrum(silence, n, work_re, work_im, smags);
  for (uint32_t k = 0; k < n / 2; k++) close_to(smags[k], 0.0f, "silence FFTs to a silent spectrum");
}

/* ---- talkback / test-signal generators --------------------------- */

int main(void) {
  test_ring_write_read_basic();
  test_ring_wraparound();
  test_ring_oversized_write();
  test_ring_fill_count_saturates_and_resets();
  test_ring_fill_gate_is_the_end_of_the_ramp();
  test_fft_size_for_rate_ladder();
  test_fft_known_sinusoid();
  printf("rta: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
