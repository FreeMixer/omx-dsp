// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The offline pre-hit reverse render's golden digests (golden.h), at the nine RME rates.
 *
 * One digest per rate covers four renders of a 0.5 s stereo region laid end to end, each followed
 * by the render's return code: an impulse, noise over the first quarter, clicks at 0.25 s and on
 * the region's last sample, rendered as
 *   - REVERSE (ROOM's network), 300 ms window, mix 0.5;
 *   - PLATE, 120 ms window, 20 ms pre-delay, wet only;
 *   - HALL, the window's ceiling (500 ms, as long as the region), mix 0.3;
 *   - GATED (ROOM's network), a window under the floor (clamped to 50 ms), mix 0.7.
 *
 * The digests in test/golden/reverse_render.sha256 were written by the console engine's own copy
 * of this kernel (the engine's mix_reverse_render.h, against
 * omx-dsp 0.6.1 and omx-contract 2.3.0), by building this file with
 * -DOMX_REVERSE_RENDER_ENGINE_COPY and -I<that src>:
 *
 *   cc $(CFLAGS) -DOMX_REVERSE_RENDER_ENGINE_COPY -I<engine src> \
 *      -Iinclude -isystem $(CONTRACT_INC) -Itest/fx -include test/support/log10f_subst.h \
 *      -o build/fx_reverse_render_golden_engine test/fx/reverse_render_golden.test.c build/libomxdsp.a -lm
 *   build/fx_reverse_render_golden_engine --write > test/golden/reverse_render.sha256
 *
 *   make test-fx                     compare fx/omx_reverse_render.h with them
 *
 * The 32, 64 and 128 kHz lines were added later by fx/omx_reverse_render.h itself (`--write`), with
 * the six the engine wrote unchanged (#75).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef OMX_REVERSE_RENDER_ENGINE_COPY
#include "mix_reverse_render.h"
#else
#include <omxdsp/fx/omx_reverse_render.h>
#endif

#define OMX_FX_GOLDEN_RME 1 /* held at the nine RME rates */
#include "golden.h"

#define MAX_N 96000u /* 0.5 s at 192 kHz */
#define NCASE 4

struct rcase {
  int algorithm;
  float reverse_ms, predelay_ms, mix;
};

static const struct rcase CASES[NCASE] = {
    {OMX_REVERB_REVERSE, 300.0f, 0.0f, 0.5f},
    {OMX_REVERB_PLATE, 120.0f, 20.0f, 1.0f},
    {OMX_REVERB_HALL, 500.0f, 0.0f, 0.3f},
    {OMX_REVERB_GATED, 10.0f, 0.0f, 0.7f},
};

static float g_out[NCASE * (2u * MAX_N + 1u)];
static float g_l[MAX_N], g_r[MAX_N];

static int render(float sr, float *out, size_t *bytes) {
  const uint32_t n = (uint32_t)(sr * 0.5f);
  const uint32_t wmax = omx_reverse_render_window((float)OMX_REVERSE_RENDER_MAX_MS, sr);
  struct omx_reverse_render_scratch sc;
  sc.st = calloc(1, sizeof(struct omx_reverb_state));
  sc.pool = calloc(OMX_REVERB_POOL_FLOATS, sizeof(float));
  sc.pool_len = OMX_REVERB_POOL_FLOATS;
  sc.tmp_l = calloc(wmax, sizeof(float));
  sc.tmp_r = calloc(wmax, sizeof(float));
  sc.tmp_cap = wmax;
  sc.wet_l = calloc(n, sizeof(float));
  sc.wet_r = calloc(n, sizeof(float));
  sc.wet_cap = n;
  float *o = out;
  int fail = 0;
  for (int c = 0; c < NCASE; c++) {
    uint32_t lcg = 0x1234567u;
    for (uint32_t i = 0; i < n; i++) {
      const float x = omx_fx_golden_noise(&lcg);
      g_l[i] = i == 0 ? 1.0f : (i < n / 4u ? x * 0.5f : 0.0f);
      g_r[i] = i == 0 ? 0.0f : (i < n / 4u ? -x * 0.25f : 0.0f);
    }
    g_l[n / 2u] = 1.0f;
    g_r[n / 2u] = -1.0f;
    g_l[n - 1u] = 0.5f;
    g_r[n - 1u] = 0.5f;
    struct omx_reverb p;
    memset(&p, 0, sizeof p);
    p.enabled = 1;
    p.algorithm = CASES[c].algorithm;
    p.size = 0.7f;
    p.damping = 0.5f;
    p.predelay_ms = CASES[c].predelay_ms;
    p.width = 1.0f;
    p.mix = CASES[c].mix;
    p.lowcut = 0.0f;
    p.highcut = 20000.0f;
    p.reverse_ms = CASES[c].reverse_ms;
    const int rc = omx_reverse_render(g_l, g_r, n, &p, sr, &sc);
    if (rc != 0) {
      fprintf(stderr, "FAIL: %.0f Hz case %d: the render refused\n", (double)sr, c);
      fail++;
    }
    for (uint32_t i = 0; i < n; i++) { o[2 * i] = g_l[i]; o[2 * i + 1] = g_r[i]; }
    o[2u * n] = (float)rc;
    o += 2u * n + 1u;
  }
  *bytes = (size_t)(o - out) * sizeof(float);
  free(sc.st);
  free(sc.pool);
  free(sc.tmp_l);
  free(sc.tmp_r);
  free(sc.wet_l);
  free(sc.wet_r);
  return fail;
}

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "reverse_render", render, g_out); }
