// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The reverb's golden digests: one SHA-256 per declared rate over the kernel's output for a fixed
 * stimulus, run through all five configurations (room, plate, hall, reverse, gated) in turn, each
 * on a fresh pool, compared with test/golden/reverb.sha256 by the driver in golden.h. A patch
 * release may not move one; a release that does is a minor and names the kernel.
 *
 *   make test-fx                       compare
 *   build/fx_reverb_golden --write     print the lines test/golden/reverb.sha256 holds
 *
 * The stimulus is an impulse, then integer-generated noise loud enough to arm GATED's key, then
 * silence for the tail, so no libm call shapes the input. The run lasts 0.75 s at every rate, which
 * holds two of REVERSE's 150 ms windows after the noise stops and GATED's whole hold and release.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_reverb.h>

#include "golden.h"

#define BLOCK 128
#define NALGO 5

static const int ALGOS[NALGO] = {OMX_REVERB_ROOM, OMX_REVERB_PLATE, OMX_REVERB_HALL, OMX_REVERB_REVERSE,
                                 OMX_REVERB_GATED};

static int frames_at(float sr) { return ((int)(0.75f * sr) / BLOCK) * BLOCK; }

/* The pool every configuration lays its state over, allocated once in main. */
static float *g_pool;

/* `out` holds NALGO runs back to back, each 2 x frames_at(sr) interleaved floats, `*bytes` their
 * size. Returns how many configurations rang no tail in the 50 ms after the noise stops: a digest
 * of a passthrough (an exhausted pool, a rate the layout refused) holds nothing a reverb made. */
static int render(float sr, float *out, size_t *bytes) {
  float *pool = g_pool;
  int silent = 0;
  const int frames = frames_at(sr);
  for (int a = 0; a < NALGO; a++) {
    struct omx_reverb_state s;
    memset(&s, 0, sizeof s);
    memset(pool, 0, (size_t)OMX_REVERB_POOL_FLOATS * sizeof(float));
    omx_reverb_state_layout(&s, pool, OMX_REVERB_POOL_FLOATS, sr);
    struct omx_reverb p;
    memset(&p, 0, sizeof p);
    p.enabled = 1; p.algorithm = ALGOS[a]; p.size = 0.6f; p.damping = 0.4f; p.predelay_ms = 7.0f;
    p.width = 0.8f; p.mix = 0.7f; p.lowcut = 120.0f; p.highcut = 9000.0f;
    p.reverse_ms = 150.0f; p.hold_ms = 60.0f; p.release_ms = 20.0f; p.gate_threshold_db = -30.0f;
    p.plate_mod_depth = OMX_REVERB_PLATE_MOD_DEPTH_DEFAULT;
    uint32_t lcg = 0x1234567u;
    float l[BLOCK], r[BLOCK];
    float *o = out + (size_t)a * 2u * (size_t)frames;
    for (int b = 0; b < frames; b += BLOCK) {
      for (int i = 0; i < BLOCK; i++) {
        float noise = omx_fx_golden_noise(&lcg);
        l[i] = (b + i == 0) ? 1.0f : (b + i < frames / 3 ? noise * 0.5f : 0.0f);
        r[i] = (b + i == 0) ? 0.0f : (b + i < frames / 3 ? -noise * 0.25f : 0.0f);
      }
      omx_reverb_process(l, r, BLOCK, &p, &s, sr);
      for (int i = 0; i < BLOCK; i++) { o[2 * (b + i)] = l[i]; o[2 * (b + i) + 1] = r[i]; }
    }
    double tail = 0.0;
    for (int i = frames / 3; i < frames / 3 + (int)(0.05f * sr); i++)
      tail += (double)o[2 * i] * o[2 * i] + (double)o[2 * i + 1] * o[2 * i + 1];
    if (!(tail > 1e-6)) {
      fprintf(stderr, "FAIL: %.0f Hz configuration %d rings no tail (energy %.3g)\n", (double)sr, ALGOS[a], tail);
      silent++;
    }
  }
  *bytes = (size_t)NALGO * 2u * (size_t)frames * sizeof(float);
  return silent;
}

int main(int argc, char **argv) {
  g_pool = calloc(OMX_REVERB_POOL_FLOATS, sizeof(float));
  float *out = calloc((size_t)NALGO * 2u * (size_t)frames_at(OMX_REVERB_MAX_RATE), sizeof(float));
  if (!g_pool || !out) { fprintf(stderr, "fx/reverb_golden: out of memory\n"); return 2; }
  const int rc = omx_fx_golden_main_sized(argc, argv, "reverb", render, out);
  free(g_pool);
  free(out);
  return rc;
}
