// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The precision limiter's golden digests (golden.h): the stimulus at +12 dB through the limiter at
 * a -1 dBFS ceiling, 1.5 ms look-ahead, 50 ms release, in 128-frame blocks.
 *
 *   make test-fx                        compare
 *   build/fx_limiter_golden --write     print the lines test/golden/limiter.sha256 holds
 */
#include <stdint.h>
#include <stdlib.h>

#include <omxdsp/fx/omx_limiter.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  const uint32_t cap = omx_limiter_cap(sr);
  float *mem = calloc(omx_limiter_mem_floats(cap), sizeof(float));
  uint32_t *idx = calloc(cap, sizeof(uint32_t));
  struct omx_limiter_state st;
  omx_limiter_init(&st, OMX_LIMITER_LOOKAHEAD_MS_DEFAULT, sr, mem, idx, cap);
  const struct omx_limiter p = {1, OMX_LIMITER_CEILING_DB_DEFAULT, OMX_LIMITER_RELEASE_MS_DEFAULT};
  const float gain = omx_db_to_lin(12.0f);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) {
      omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
      l[i] *= gain;
      r[i] *= gain;
    }
    omx_limiter_process(l, r, BLOCK, &p, &st);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
  free(mem);
  free(idx);
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "limiter", render); }
