// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The integer delay line's golden digests (golden.h), at the nine RME rates.
 *
 * One digest per rate covers three renders laid end to end:
 *   - a stereo pair: the stimulus (an impulse, then noise for the whole run) through the pair in
 *     uneven blocks, each leg's delay re-pointed every 1024 frames through its own list of
 *     requests (0, whole moves, moves within one block, the ceiling, past the ring), so steady
 *     reads and cross-fades are both held;
 *   - a mono pair (r = NULL) through the same script;
 *   - the ms -> samples conversion over a list of requests, hostile ones included.
 *
 * The digests in test/golden/idelay.sha256 were written by the console engine's own copy of this
 * line (omx_delay_samples, omx_delay_apply and omx_bus_out_delay in openmixer
 * packages/pipewire-native/src/mix_dsp.h at 6ee8a9bd, against omx-dsp 0.6.1 and omx-contract
 * 2.3.0), by building this file with -DOMX_IDELAY_ENGINE_COPY and -I<that src>:
 *
 *   cc $(CFLAGS) -DOMX_IDELAY_ENGINE_COPY -I<openmixer>/packages/pipewire-native/src -Iinclude \
 *      -isystem $(CONTRACT_INC) -Itest/fx -include test/support/log10f_subst.h \
 *      -o build/fx_idelay_golden_engine test/fx/idelay_golden.test.c build/libomxdsp.a -lm
 *   build/fx_idelay_golden_engine --write > test/golden/idelay.sha256
 *
 *   make test-fx                     compare omx_idelay.h with them
 *
 * The 32, 64 and 128 kHz lines were added later by omx_idelay.h itself (`--write`), with the six
 * the engine wrote unchanged (#81).
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef OMX_IDELAY_ENGINE_COPY
#include "mix_dsp.h"
#define PAIR struct omx_out_delay
#define SAMPLES omx_delay_samples
#define PAIR_APPLY omx_bus_out_delay
#else
#include <omxdsp/omx_idelay.h>
#define PAIR struct omx_idelay_pair
#define SAMPLES omx_idelay_samples
#define PAIR_APPLY omx_idelay_pair_apply
#endif

#define OMX_FX_GOLDEN_RME 1 /* held at the nine RME rates */
#include "golden.h"

/* The ring: 50 ms at the highest graph rate, +1. Short enough that the ceiling request below
 * saturates at every rate and the stimulus wraps the ring many times. */
#define RING (((OMX_RT_HARD_TARGET_RATE / 1000) * 50) + 1)
#define SEG 1024
#define NREQ 24

/* The requests, ms, one per segment and leg. */
static const float MS_L[] = {0.0f, 1.0f, 1.0f, 0.02f, 5.0f, 0.0f, 23.5f, 7.0f, 49.99f, 50.0f, 1000.0f, 0.5f, 0.0f, 12.0f, 3.3f, 0.0f};
static const float MS_R[] = {0.0f, 0.0f, 3.0f, 3.0f, 0.04f, 31.0f, 0.0f, 7.0f, 2.0f, 200.0f, 0.1f, 0.5f, 9.0f, 0.0f, 3.3f, 1.0f};
#define NMS (sizeof MS_L / sizeof MS_L[0])

static const uint32_t BLOCKS[] = {64u, 37u, 128u, 1u, 256u, 100u, 7u, 1024u};

static float g_rl[RING], g_rr[RING];

static void pair(float sr, float *out, int stereo) {
  memset(g_rl, 0, sizeof g_rl);
  memset(g_rr, 0, sizeof g_rr);
  PAIR d;
  memset(&d, 0, sizeof d);
  d.ring_l = g_rl;
  d.ring_r = g_rr;
  d.cap = RING;
  static float l[OMX_FX_GOLDEN_FRAMES], r[OMX_FX_GOLDEN_FRAMES];
  uint32_t lcg = 0x1234567u;
  for (int i = 0; i < OMX_FX_GOLDEN_FRAMES; i++) {
    const float x = omx_fx_golden_noise(&lcg);
    l[i] = i == 0 ? 1.0f : x * 0.5f;
    r[i] = i == 0 ? 0.0f : -x * 0.25f;
  }
  uint32_t done = 0, bi = 0;
  while (done < (uint32_t)OMX_FX_GOLDEN_FRAMES) {
    const uint32_t s = (done / SEG) % NMS;
    const uint32_t tl = SAMPLES(MS_L[s], sr, d.cap);
    const uint32_t tr = SAMPLES(MS_R[s], sr, d.cap);
    uint32_t k = BLOCKS[bi++ % (sizeof BLOCKS / sizeof BLOCKS[0])];
    const uint32_t seg_end = (done / SEG + 1u) * SEG;
    if (k > seg_end - done) k = seg_end - done;
    PAIR_APPLY(l + done, stereo ? r + done : NULL, k, &d, tl, tr);
    done += k;
  }
  if (stereo) {
    for (int i = 0; i < OMX_FX_GOLDEN_FRAMES; i++) { out[2 * i] = l[i]; out[2 * i + 1] = r[i]; }
  } else {
    memcpy(out, l, sizeof(float) * OMX_FX_GOLDEN_FRAMES);
  }
}

static int render(float sr, float *out, size_t *bytes) {
  pair(sr, out, 1);
  pair(sr, out + 2 * OMX_FX_GOLDEN_FRAMES, 0);
  uint32_t t[3 * NREQ];
  const float req[NREQ] = {0.0f, -1.0f, -0.0f, 1e-6f, 0.0104167f, 0.0113378f, 0.0208333f, 0.5f,
                           1.0f / 3.0f, 1.0f, 3.2f, 7.0f, 20.408f, 23.5f, 49.99f, 50.0f,
                           50.01f, 86.0f, 99.9f, 120.0f, 750.0f, 1000.0f, 1.0e9f, 0.0226757f};
  for (int i = 0; i < NREQ; i++) {
    t[3 * i] = SAMPLES(req[i], sr, RING);
    t[3 * i + 1] = SAMPLES(req[i], sr, 1u);
    t[3 * i + 2] = SAMPLES(req[i], sr, 0u);
  }
  memcpy(out + 3 * OMX_FX_GOLDEN_FRAMES, t, sizeof t);
  *bytes = 3u * OMX_FX_GOLDEN_FRAMES * sizeof(float) + 3u * NREQ * sizeof(uint32_t);
  return 0;
}

static float g_out[3 * OMX_FX_GOLDEN_FRAMES + 3 * NREQ];

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "idelay", render, g_out); }
