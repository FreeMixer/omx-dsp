// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The loudness kernels' golden digests (golden.h), at the nine oracle rates.
 *
 * One digest per rate covers, as doubles laid end to end:
 *   - the two K-weighting sections omx_loudness_kweight_design() gives for the rate;
 *   - every 100 ms of an 8 s stereo stimulus fed in uneven blocks: the momentary, short-term and
 *     integrated energies, the loudness range's two ends and the true peak.
 *
 * The stimulus is integer-generated noise (no libm call shapes it) whose level steps every second
 * through a list that crosses both relative gates and the absolute one, with the right leg
 * inverted at a quarter of the left's level and a full-scale click every 1.3 s. The readings are
 * hashed in energy, never in LUFS, so no libm log10 enters the digests.
 *
 *   make test-fx       compare omx_loudness.h with test/golden/loudness.sha256
 *   make golden-write  rewrite it (only in a commit that bumps the minor version)
 */
#define OMX_FX_GOLDEN_ORACLE_RATES 1
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_loudness.h>

#include "golden.h"

#define SECONDS 8u
#define READS (SECONDS * 10u)
#define PER_READ 7u
#define DOUBLES (10u + READS * PER_READ)

/* Each second's level, linear: loud, quiet (−20 dB), under the gates (−80 dB), back. */
static const float LEVEL[SECONDS] = {0.5f, 0.05f, 0.5f, 0.0001f, 0.25f, 0.5f, 0.005f, 0.4f};

static const uint32_t BLOCKS[] = {64u, 37u, 128u, 1u, 256u, 100u, 7u, 1024u};

static struct omx_loudness g_s;

static int render(float sr, float *out, size_t *bytes) {
  double d[DOUBLES];
  uint32_t at = 0;
  omx_loudness_init(&g_s, sr, 2u, NULL);
  for (int k = 0; k < 2; k++)
    for (int i = 0; i < 5; i++) d[at++] = g_s.kw[k][i];
  const uint32_t tenth = (uint32_t)sr / 10u;
  static float l[192000u / 10u], r[192000u / 10u];
  uint32_t lcg = 0x1234567u, bi = 0u;
  uint64_t frame = 0u;
  const uint64_t click = (uint64_t)((double)sr * 1.3);
  for (uint32_t t = 0; t < READS; t++) {
    const float a = LEVEL[t / 10u];
    for (uint32_t i = 0; i < tenth; i++, frame++) {
      const float x = omx_fx_golden_noise(&lcg);
      l[i] = frame % click == click - 1u ? 1.0f : x * a;
      r[i] = -x * a * 0.25f;
    }
    for (uint32_t done = 0; done < tenth;) {
      uint32_t k = BLOCKS[bi++ % (sizeof BLOCKS / sizeof BLOCKS[0])];
      if (k > tenth - done) k = tenth - done;
      const float *legs[2] = {l + done, r + done};
      omx_loudness_process(&g_s, legs, k);
      done += k;
    }
    d[at++] = omx_loudness_momentary_ms(&g_s);
    d[at++] = omx_loudness_short_term_ms(&g_s);
    d[at++] = omx_loudness_integrated_ms(&g_s);
    omx_loudness_range_ms(&g_s, &d[at], &d[at + 1]);
    at += 2;
    d[at++] = (double)omx_loudness_true_peak(&g_s);
    d[at++] = (double)g_s.subs;
  }
  memcpy(out, d, sizeof d);
  *bytes = sizeof d;
  return at == DOUBLES ? 0 : 1;
}

static float g_out[2u * DOUBLES];

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "loudness", render, g_out); }
