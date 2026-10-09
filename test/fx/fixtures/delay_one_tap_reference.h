// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * delay_one_tap_reference.h — the one-tap FX delay block, frozen, never edited.
 *
 * The body of omx_fx_delay_process with one read per leg, exactly as omx-dsp 25300b0 holds it
 * (contracts stripped). test/fx/delay_taps.test.c memcmps omx_fx_delay_process and
 * omx_fx_delay_process_taps at one tap against it (tap-delay L1): a one-tap delay plays the same
 * bits whatever the read taps add to the kernel.
 */
#ifndef OMX_DELAY_ONE_TAP_REFERENCE_H
#define OMX_DELAY_ONE_TAP_REFERENCE_H

#include <omxdsp/fx/omx_delay.h>

static inline void omx_fx_delay_one_tap_reference(float *l, float *r, uint32_t n,
                                                  const struct omx_fx_delay *p,
                                                  struct omx_fx_delay_state *s, float sr) {
  if (!p->enabled || n == 0 || s->ring_l == NULL || s->ring_r == NULL || s->cap == 0) return;
  uint32_t cap = s->cap;
  uint32_t dl = omx_fxdelay_clamp(p->d_l, cap);
  uint32_t dr = omx_fxdelay_clamp(p->d_r, cap);
  float fb = omx_clamp_or(p->feedback, OMX_FX_DELAY_FEEDBACK_RANGE_MIN, OMX_FX_DELAY_FEEDBACK_RANGE_MAX, 0.0f);
  float mix = omx_clamp_or(p->mix, 0.0f, 1.0f, 0.0f);
  float dry = 1.0f - mix;
  float tone = omx_fxdelay_tone_pole(p->tone, sr);
  uint32_t w = s->wpos;
  float dampL = s->damp_l, dampR = s->damp_r;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t rl = omx_lookahead_back(w, dl, cap);
    uint32_t rr = omx_lookahead_back(w, dr, cap);
    float xl = l[i], xr = r[i];
    float tapL = dl == 0 ? xl : s->ring_l[rl];
    float tapR = dr == 0 ? xr : s->ring_r[rr];
    dampL = tapL * (1.0f - tone) + dampL * tone;
    dampR = tapR * (1.0f - tone) + dampR * tone;
    float fbL = p->pingpong ? dampR : dampL;
    float fbR = p->pingpong ? dampL : dampR;
    s->ring_l[w] = xl + fb * fbL;
    s->ring_r[w] = xr + fb * fbR;
    l[i] = dry * xl + mix * tapL;
    r[i] = dry * xr + mix * tapR;
    w = omx_lookahead_fwd(w, 1u, cap);
  }
  s->wpos = w;
  s->damp_l = dampL;
  s->damp_r = dampR;
}

#endif /* OMX_DELAY_ONE_TAP_REFERENCE_H */
