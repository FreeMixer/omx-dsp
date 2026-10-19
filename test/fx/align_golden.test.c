// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The input alignment delay's golden digests (golden.h), at the nine oracle rates.
 *
 * One digest per rate covers three renders laid end to end:
 *   - a stereo strip: the stimulus (an impulse, then noise for the whole run) through
 *     omx_align_apply in uneven blocks, its target re-pointed every 1024 frames through a list
 *     of requests (whole, fractional, under the kernel's reach, back to 0) so the settled reads
 *     and the slews are both held;
 *   - a mono strip (r = NULL) through the same script;
 *   - omx_align_target and omx_align_deliverable over a list of requests, the ceiling and hostile
 *     ones included.
 *
 * The digests in test/golden/align.sha256 were written by the console engine's own copy of this
 * stage (mix_dsp.h, against omx-dsp 0.6.1 and
 * omx-contract 2.3.0), by building this file with -DOMX_ALIGN_ENGINE_COPY and -I<that src>:
 *
 *   cc $(CFLAGS) -DOMX_ALIGN_ENGINE_COPY -I<engine src> -Iinclude \
 *      -isystem $(CONTRACT_INC) -Itest/fx -include test/support/log10f_subst.h \
 *      -o build/fx_align_golden_engine test/fx/align_golden.test.c build/libomxdsp.a -lm
 *   build/fx_align_golden_engine --write > test/golden/align.sha256
 *
 *   make test-fx                     compare omx_align.h with them
 *
 * The 32, 64 and 128 kHz lines were added later by omx_align.h itself (`--write`), with the six
 * the engine wrote unchanged.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef OMX_ALIGN_ENGINE_COPY
#include "mix_dsp.h"
#else
#include <omxdsp/omx_align.h>
#endif

#define OMX_FX_GOLDEN_ORACLE_RATES 1 /* held at the nine oracle rates */
#include "golden.h"

#define SEG 1024
#define NREQ 24

/* The requests, ms, one per segment: short enough that a slew of 1/32 sample per sample lands
 * inside its 1024 frames at every rate, so each segment holds a move and a settled read. */
static const float MS[] = {0.0f, 0.05f, 0.0137f, 0.032f, 0.0001f, 0.0f, 0.07777f, 0.1f, 0.09999f, 0.02f, 0.003f, 0.0f, 0.125f, 0.000021f, 0.041f, 0.0f};
#define NMS (sizeof MS / sizeof MS[0])

static const uint32_t BLOCKS[] = {64u, 37u, 128u, 1u, 256u, 100u, 7u, 1024u};

static float g_rl[OMX_ALIGN_CAP], g_rr[OMX_ALIGN_CAP];

static void strip(float sr, float *out, int stereo) {
  memset(g_rl, 0, sizeof g_rl);
  memset(g_rr, 0, sizeof g_rr);
  struct omx_align_state a;
  memset(&a, 0, sizeof a);
  omx_align_arm(&a, g_rl, g_rr);
  static float l[OMX_FX_GOLDEN_FRAMES], r[OMX_FX_GOLDEN_FRAMES];
  uint32_t lcg = 0x1234567u;
  for (int i = 0; i < OMX_FX_GOLDEN_FRAMES; i++) {
    const float x = omx_fx_golden_noise(&lcg);
    l[i] = i == 0 ? 1.0f : x * 0.5f;
    r[i] = i == 0 ? 0.0f : -x * 0.25f;
  }
  uint32_t done = 0, bi = 0;
  while (done < (uint32_t)OMX_FX_GOLDEN_FRAMES) {
    const float tgt = omx_align_target(MS[(done / SEG) % NMS], sr);
    uint32_t k = BLOCKS[bi++ % (sizeof BLOCKS / sizeof BLOCKS[0])];
    const uint32_t seg_end = (done / SEG + 1u) * SEG;
    if (k > seg_end - done) k = seg_end - done;
    omx_align_apply(&a, l + done, stereo ? r + done : NULL, k, tgt);
    done += k;
  }
  if (stereo) {
    for (int i = 0; i < OMX_FX_GOLDEN_FRAMES; i++) { out[2 * i] = l[i]; out[2 * i + 1] = r[i]; }
  } else {
    memcpy(out, l, sizeof(float) * OMX_FX_GOLDEN_FRAMES);
  }
}

static int render(float sr, float *out, size_t *bytes) {
  strip(sr, out, 1);
  strip(sr, out + 2 * OMX_FX_GOLDEN_FRAMES, 0);
  float *t = out + 3 * OMX_FX_GOLDEN_FRAMES;
  const float req[NREQ] = {0.0f, -1.0f, NAN, INFINITY, -INFINITY, 1e-6f, 0.01f, 0.02f, 0.0104167f, 0.5f,
                           1.0f / 3.0f, 3.2f, 50.0f, 99.999f, 100.0f, 100.001f, 1000.0f, 0.0226757f, 7.0f, 0.75f,
                           2.5f, 0.0416667f, 33.3333f, 64.0f};
  for (int i = 0; i < NREQ; i++) {
    t[2 * i] = omx_align_target(req[i], sr);
    t[2 * i + 1] = omx_align_deliverable(fabsf((float)i * 0.37f));
  }
  *bytes = (3u * OMX_FX_GOLDEN_FRAMES + 2u * NREQ) * sizeof(float);
  return 0;
}

static float g_out[3 * OMX_FX_GOLDEN_FRAMES + 2 * NREQ];

int main(int argc, char **argv) { return omx_fx_golden_main_sized(argc, argv, "align", render, g_out); }
