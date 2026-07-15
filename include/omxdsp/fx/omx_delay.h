// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * mix_delay.h — native stereo FX delay line (send/return or channel insert), pure-C, RT-safe.
 *
 * Same contract as mix_dsp.h: NO PipeWire, NO allocation, NO libc beyond <math.h>. The ring buffers
 * live in `struct omx_fx_delay_state` and are allocated by the CALLER off the RT thread (on insert),
 * never here. Controls (`struct omx_fx_delay`) are word-atomic snapshots the RT thread reads once per
 * block. A delay-TIME change may click (the read pointer jumps) — standard for a delay, and documented
 * rather than cross-faded; the wet gain (mix) and feedback are smooth per-sample.
 *
 * Musical shape: input -> [wet = ringRead*mix, dry = in*(1-mix)] out; feedback writes (in + fbTap*fb)
 * back into the ring, with a one-pole low-pass (tone) in the feedback path so repeats darken. Ping-pong
 * cross-feeds the two legs' feedback so echoes bounce L<->R.
 */
#ifndef OMX_MIX_DELAY_H
#define OMX_MIX_DELAY_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/** Max FX delay time (ms). ~2 s covers slow ambient repeats. */
#define OMX_FXDELAY_MAX_MS 2000
/** Highest graph rate the ring is sized for, so MAX_MS is reachable at 44.1/48/96/192 kHz. */
#define OMX_FXDELAY_MAX_RATE 192000
/** Per-leg ring capacity, samples: MAX_MS at MAX_RATE, +1 so the full max delay is usable. */
#define OMX_FXDELAY_CAP (((OMX_FXDELAY_MAX_RATE / 1000) * OMX_FXDELAY_MAX_MS) + 1)

/** Control atom (word-atomic snapshot). `d_l`/`d_r` are per-leg delays in SAMPLES (caller converts
 * ms/tempo -> samples via omx_bpm_division_ms + rate). `feedback`/`mix`/`tone` in [0,1]. */
struct omx_fx_delay {
  int enabled;      /* 0 -> passthrough (a disabled/absent delay) */
  uint32_t d_l;     /* left-leg delay, samples (clamped to cap-1 here) */
  uint32_t d_r;     /* right-leg delay, samples */
  float feedback;   /* 0..<1 regeneration; hard-clamped below 1 to prevent runaway */
  float mix;        /* 0 = dry (bit-identical), 1 = wet-only (FX send/return default) */
  float tone;       /* feedback-path one-pole damping 0..1 (0 = bright, ->1 = dark repeats) */
  int pingpong;     /* non-zero -> cross-feed the legs (echoes bounce L<->R) */
};

/** Caller-owned rings + RT-owned state. `ring_l`/`ring_r` are `cap` floats each (allocated on insert). */
struct omx_fx_delay_state {
  float *ring_l;
  float *ring_r;
  uint32_t cap;
  uint32_t wpos;     /* shared write cursor (mod cap) */
  float damp_l;      /* one-pole tone state per leg (feedback-path LP) */
  float damp_r;
};

/** Tempo-synced delay time in ms: `division_beats * 60000 / bpm` (1/4=1.0, 1/8=0.5, dotted-1/8=0.75, 1/8T=1/3). */
static inline float omx_bpm_division_ms(float bpm, float division_beats) {
  if (bpm <= 0.0f) return 0.0f;
  return division_beats * 60000.0f / bpm;
}

/** Clamp a per-leg delay to [0, cap-1]. */
static inline uint32_t omx_fxdelay_clamp(uint32_t d, uint32_t cap) {
  if (cap == 0) return 0;
  return d >= cap ? cap - 1 : d;
}

/**
 * Process one block IN PLACE. `l`/`r` are the strip's two legs (distinct buffers). A disabled atom /
 * NULL ring is a passthrough. RT-safe: fixed work per sample, no allocation.
 */
static inline void omx_fx_delay_process(float *l, float *r, uint32_t n,
                                        const struct omx_fx_delay *p, struct omx_fx_delay_state *s) {
  if (!p->enabled || n == 0 || s->ring_l == NULL || s->ring_r == NULL || s->cap == 0) return;
  uint32_t cap = s->cap;
  uint32_t dl = omx_fxdelay_clamp(p->d_l, cap);
  uint32_t dr = omx_fxdelay_clamp(p->d_r, cap);
  float fb = p->feedback;
  if (fb < 0.0f) fb = 0.0f;
  if (fb > 0.99f) fb = 0.99f; /* hard clamp < 1: repeats can never grow without bound */
  float mix = p->mix < 0.0f ? 0.0f : (p->mix > 1.0f ? 1.0f : p->mix);
  float dry = 1.0f - mix;
  float tone = p->tone < 0.0f ? 0.0f : (p->tone > 1.0f ? 1.0f : p->tone);
  uint32_t w = s->wpos;
  float dampL = s->damp_l, dampR = s->damp_r;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t rl = w >= dl ? w - dl : w + cap - dl;
    uint32_t rr = w >= dr ? w - dr : w + cap - dr;
    float xl = l[i], xr = r[i];
    /* D == 0 reads the sample ARRIVING this frame, not the w-slot that still holds the value from a
     * full ring ago (which would echo `cap` samples ≈ 2 s of stale audio). Reading the input makes a
     * 0 ms delay an exact passthrough, matching omx_delay_apply's write-then-read at tgt=0. */
    float tapL = dl == 0 ? xl : s->ring_l[rl];
    float tapR = dr == 0 ? xr : s->ring_r[rr];
    /* one-pole low-pass in the feedback path (tone, Freeverb-style damping): tone=0 passes the tap
     * through (bright), tone->1 freezes it toward its running average (dark repeats). */
    dampL = tapL * (1.0f - tone) + dampL * tone;
    dampR = tapR * (1.0f - tone) + dampR * tone;
    /* ping-pong: each leg's feedback comes from the OTHER leg's damped tap */
    float fbL = p->pingpong ? dampR : dampL;
    float fbR = p->pingpong ? dampL : dampR;
    s->ring_l[w] = xl + fb * fbL;
    s->ring_r[w] = xr + fb * fbR;
    l[i] = dry * xl + mix * tapL;
    r[i] = dry * xr + mix * tapR;
    w = (w + 1 == cap) ? 0 : w + 1;
  }
  s->wpos = w;
  s->damp_l = dampL;
  s->damp_r = dampR;
}

#endif /* OMX_MIX_DELAY_H */
