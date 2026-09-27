// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_oversampler.c — both directions polyphase: the half-band's even taps are zero except the
 * centre, so interpolation is one multiply for half the outputs and T multiply-adds for the
 * other half, and decimation never computes a sample it discards. Each stage keeps its own
 * input history and emits the output of a sample it saw a whole window ago; that lag is the
 * declared latency. The window is built once per chunk, never shifted per sample.
 */
#include <omxdsp/omx_oversampler.h>

#include <string.h>

#define T OMX_HALFBAND_ODD_TAPS

/* Input samples per pass. Bounds the scratch; the result does not depend on it. */
#define OVS_CHUNK 128u

/* hist: the last OMX_OVS_UP_HIST inputs, oldest first. Output pair i is centred on the input
 * OMX_OVS_UP_DELAY back from the newest of this pass; ×2 restores the amplitude zero-stuffing halves. */
static void up2_stage(float *hist, const float *in, uint32_t n, float *out) {
  uint32_t done = 0u;
  while (done < n) {
    const uint32_t take = (n - done > OVS_CHUNK) ? OVS_CHUNK : (n - done);
    float win[OMX_OVS_UP_HIST + OVS_CHUNK];
    memcpy(win, hist, OMX_OVS_UP_HIST * sizeof *win);
    memcpy(win + OMX_OVS_UP_HIST, in + done, take * sizeof *win);
    for (uint32_t i = 0; i < take; i++) {
      const float *c = win + OMX_OVS_UP_HIST + i - OMX_OVS_UP_DELAY;
      out[2u * (done + i)] = 2.0f * OMX_HALFBAND_CENTER * c[0];
      float acc = 0.0f;
      for (uint32_t j = 0; j < T; j++) acc += OMX_HALFBAND_ODD_COEF[j] * (c[-(int)j] + c[j + 1u]);
      out[2u * (done + i) + 1u] = 2.0f * acc;
    }
    memcpy(hist, win + take, OMX_OVS_UP_HIST * sizeof *hist);
    done += take;
  }
}

/* Two inputs per output; the output is centred OMX_OVS_DOWN_DELAY back so its window is complete. */
static void down2_stage(float *hist, const float *in, uint32_t n_out, float *out) {
  uint32_t done = 0u;
  while (done < n_out) {
    const uint32_t take = (n_out - done > OVS_CHUNK) ? OVS_CHUNK : (n_out - done);
    float win[OMX_OVS_DOWN_HIST + 2u * OVS_CHUNK];
    memcpy(win, hist, OMX_OVS_DOWN_HIST * sizeof *win);
    memcpy(win + OMX_OVS_DOWN_HIST, in + 2u * done, 2u * take * sizeof *win);
    for (uint32_t o = 0; o < take; o++) {
      const float *c = win + OMX_OVS_DOWN_HIST + 2u * o - OMX_OVS_DOWN_DELAY;
      out[done + o] = omx_halfband_dot(c);
    }
    memcpy(hist, win + 2u * take, OMX_OVS_DOWN_HIST * sizeof *hist);
    done += take;
  }
}

void omx_oversampler_init(struct omx_oversampler *o, uint32_t factor) {
  memset(o, 0, sizeof *o);
  o->factor = (factor >= 4u) ? 4u : (factor >= 2u ? 2u : 1u);
  o->stages = (o->factor == 4u) ? 2u : (o->factor == 2u ? 1u : 0u);
}

uint32_t omx_oversampler_latency_for(uint32_t factor) {
  /* 2×: up T @base + down 2T @2·base = 24 + 24; 4×: 24 + 12 + 12 + 24. */
  if (factor >= 4u) return OMX_OVS_LATENCY_4X;
  if (factor >= 2u) return OMX_OVS_UP_DELAY + OMX_OVS_DOWN_DELAY / 2u;
  return 0u;
}

uint32_t omx_oversampler_latency(const struct omx_oversampler *o) {
  return omx_oversampler_latency_for(o->factor);
}

#define OMX_CONTRACT_STAGE "oversampler/up"
void omx_oversampler_up(struct omx_oversampler *o, const float *in, uint32_t n, float *out) {
  if (n == 0u) return;
  OMX_PRE(omx_block_finite(in, n), "finite-in");
  if (o->stages == 0u) {
    memcpy(out, in, n * sizeof *out);
    OMX_POST(omx_block_finite(out, n), "finite-out");
    return;
  }
  if (o->stages == 1u) {
    up2_stage(o->up_hist[0], in, n, out);
    OMX_POST(omx_block_finite(out, 2u * n), "finite-out");
    return;
  }
  /* The first stage writes its 2n into the second half of `out` (4n); the second reads it back
   * into the whole of `out`. The write index always runs ahead of the read. */
  float *mid = out + 2u * n;
  up2_stage(o->up_hist[0], in, n, mid);
  up2_stage(o->up_hist[1], mid, 2u * n, out);
  OMX_POST(omx_block_finite(out, 4u * n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "oversampler/down"
void omx_oversampler_down(struct omx_oversampler *o, const float *in, uint32_t n, float *out) {
  if (n == 0u) return;
  if (o->stages == 0u) {
    memcpy(out, in, n * sizeof *out);
    OMX_POST(omx_block_finite(out, n), "finite-out");
    return;
  }
  if (o->stages == 1u) {
    OMX_PRE(omx_block_finite(in, 2u * n), "finite-in");
    down2_stage(o->down_hist[0], in, n, out);
    OMX_POST(omx_block_finite(out, n), "finite-out");
    return;
  }
  OMX_PRE(omx_block_finite(in, 4u * n), "finite-in");
  /* 4n → 2n → n: `out` holds n, so the intermediate 2n lives in a chunked frame array. */
  uint32_t done = 0u;
  while (done < n) {
    const uint32_t take = (n - done > OVS_CHUNK) ? OVS_CHUNK : (n - done);
    float mid[2u * OVS_CHUNK];
    down2_stage(o->down_hist[0], in + 4u * done, 2u * take, mid);
    down2_stage(o->down_hist[1], mid, take, out + done);
    done += take;
  }
  OMX_POST(omx_block_finite(out, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE
