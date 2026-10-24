/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_doubler_instance.h — the vocal doubler as a plugin instance: one to four voices of the input,
 * each a little later, a little detuned and drifting, spread across the stereo field. It mints no
 * DSP: every voice is an omx_doppler read head on one fractional delay line (omx_fdelay, Lagrange
 * order 3) fed the mono sum `(l + r) / 2`, the detune is the Doppler of the head's moving delay (no
 * pitch shifter), the drift is omx_lfo on the head's mean delay, the place is the console's balance
 * law (omx_balance_law) and the blend is omx_wetdry_mix.
 *
 *   1. RESOLVE. {@link omx_doubler_instance_resolve} takes the doubler kernel's contract controls in
 *      their declared order (data/kernels/doubler.json), each clamped into its travel, a non-finite
 *      word reading as its default. Voice n sits at `delayMs x DOUBLER_VOICE_DELAYS[n]`, is tuned by
 *      `detuneCents x DOUBLER_VOICE_DETUNES[n]`, drifts `modDepthMs` either side at `modRateHz` a
 *      further `n / voices` of a turn along, and is panned to `spread x DOUBLER_VOICE_PANS[n]`. Its
 *      head sweeps DOUBLER_WINDOW_MS, or less where its delay cannot hold that sweep and the drift
 *      inside the line: the window is then twice the room the voice has, so the head never reads
 *      ahead of the input.
 *   2. RUN. Per sample, the mono sum into the line, each voice through its head, the voices' sum over
 *      the voice count through their pans, and the blend with the dry legs. A mix of 0 is the input,
 *      bit for bit, and moves no state.
 *
 * LATENCY IS ZERO: the dry path is frame-aligned; the voices' delay is the effect. Nothing is
 * allocated: the line's ring is inline in {@link OmxDoublerInstance}, sized for the longest voice at
 * the highest rate, so an instance is initialised where it lives and is never copied after init.
 */
#ifndef OMX_DOUBLER_INSTANCE_H
#define OMX_DOUBLER_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_balance_law.h>
#include <omxdsp/omx_doppler.h>
#include <omxdsp/omx_fdelay.h>
#include <omxdsp/omx_lfo.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_wetdry.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_DOUBLER_INSTANCE_CHANNELS 2

/** The voices the state holds, DOUBLER_MAX_VOICES. */
#define OMX_DOUBLER_INSTANCE_VOICES OMX_DOUBLER_MAX_VOICES

/** Each voice's detune share, delay share and place: DOUBLER_VOICE_DETUNES, _DELAYS and _PANS. */
static const float OMX_DOUBLER_INSTANCE_DETUNES[OMX_DOUBLER_VOICE_DETUNES_COUNT] = OMX_DOUBLER_VOICE_DETUNES_INIT;
static const float OMX_DOUBLER_INSTANCE_DELAYS[OMX_DOUBLER_VOICE_DELAYS_COUNT] = OMX_DOUBLER_VOICE_DELAYS_INIT;
static const float OMX_DOUBLER_INSTANCE_PANS[OMX_DOUBLER_VOICE_PANS_COUNT] = OMX_DOUBLER_VOICE_PANS_INIT;
_Static_assert(OMX_DOUBLER_VOICE_DETUNES_COUNT == OMX_DOUBLER_MAX_VOICES && OMX_DOUBLER_VOICE_DELAYS_COUNT == OMX_DOUBLER_MAX_VOICES &&
                   OMX_DOUBLER_VOICE_PANS_COUNT == OMX_DOUBLER_MAX_VOICES,
               "one detune, delay and place per voice");

/** The head's sweep and its crossfade share: DOUBLER_WINDOW_MS, DOUBLER_FADE. */
#define OMX_DOUBLER_INSTANCE_WINDOW_MS ((float)OMX_DOUBLER_WINDOW_MS)
#define OMX_DOUBLER_INSTANCE_FADE ((float)OMX_DOUBLER_FADE)

/** The line's Lagrange order. */
#define OMX_DOUBLER_INSTANCE_ORDER 3

/** The line's ring, floats: the longest voice (40 ms x the largest delay share, plus the drift and half
 * the sweep) at 192 kHz, held to that by test/fx/doubler_instance.test.c. */
#define OMX_DOUBLER_INSTANCE_CAP 16384u

typedef struct {
  float sr;
  struct omx_fdelay line;
  struct omx_doppler head[OMX_DOUBLER_INSTANCE_VOICES];
  struct omx_doppler_state hs[OMX_DOUBLER_INSTANCE_VOICES];
  struct omx_lfo lfo;
  float mean[OMX_DOUBLER_INSTANCE_VOICES]; /**< each voice's mean delay, samples */
  float gl[OMX_DOUBLER_INSTANCE_VOICES];   /**< each voice's left gain, its pan over the voice count */
  float gr[OMX_DOUBLER_INSTANCE_VOICES];   /**< each voice's right gain */
  float depth;                             /**< the drift, samples either side */
  uint32_t nv;                             /**< voices sounding */
  float mix;
  int enabled;
  int ready;
  float ring[OMX_DOUBLER_INSTANCE_CAP];
} OmxDoublerInstance;

/**
 * Bind an instance to its rate, over its own ring. Returns 1 when usable, 0 when not: the rate must be
 * one of the oracle set. The doubler comes up at its declared defaults. Allocates nothing.
 */
static inline void omx_doubler_instance_resolve(OmxDoublerInstance *s, int bypass, float voices, float delay_ms,
                                                float detune_cents, float mod_rate_hz, float mod_depth_ms,
                                                float spread_pct, float mix_pct);

static inline int omx_doubler_instance_init(OmxDoublerInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_oracle_full(sr)) return 0;
  if (omx_fdelay_init(&s->line, s->ring, OMX_DOUBLER_INSTANCE_CAP, OMX_DOUBLER_INSTANCE_ORDER) != OMX_FDELAY_OK) return 0;
  omx_lfo_start(&s->lfo);
  s->sr = sr;
  s->ready = 1;
  omx_doubler_instance_resolve(s, 0, (float)OMX_DOUBLER_VOICES_RANGE_DEFAULT, (float)OMX_DOUBLER_DELAY_MS_RANGE_DEFAULT,
                               (float)OMX_DOUBLER_DETUNE_CENTS_RANGE_DEFAULT, (float)OMX_DOUBLER_MOD_RATE_HZ_RANGE_DEFAULT,
                               (float)OMX_DOUBLER_MOD_DEPTH_MS_RANGE_DEFAULT, (float)OMX_DOUBLER_SPREAD_PCT_RANGE_DEFAULT,
                               (float)OMX_DOUBLER_MIX_PCT_RANGE_DEFAULT);
  for (uint32_t k = 0; k < OMX_DOUBLER_INSTANCE_VOICES; k++) omx_doppler_start(&s->head[k], &s->hs[k]);
  return 1;
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the doubler kernel's contract controls in their declared order: the voice count,
 * the mean delay (ms), the detune (cents), the drift's rate (Hz) and depth (ms), the spread and the
 * mix (percent).
 */
#define OMX_CONTRACT_STAGE "doubler/instance-resolve"
static inline void omx_doubler_instance_resolve(OmxDoublerInstance *s, int bypass, float voices, float delay_ms,
                                                float detune_cents, float mod_rate_hz, float mod_depth_ms,
                                                float spread_pct, float mix_pct) {
  if (!s || !s->ready) return;
  const float voice_count = omx_clamp_or(voices, (float)OMX_DOUBLER_VOICES_RANGE_MIN, (float)OMX_DOUBLER_VOICES_RANGE_MAX,
                                (float)OMX_DOUBLER_VOICES_RANGE_DEFAULT);
  const float dms = omx_clamp_or(delay_ms, (float)OMX_DOUBLER_DELAY_MS_RANGE_MIN, (float)OMX_DOUBLER_DELAY_MS_RANGE_MAX,
                                 (float)OMX_DOUBLER_DELAY_MS_RANGE_DEFAULT);
  const float cents = omx_clamp_or(detune_cents, (float)OMX_DOUBLER_DETUNE_CENTS_RANGE_MIN,
                                   (float)OMX_DOUBLER_DETUNE_CENTS_RANGE_MAX, (float)OMX_DOUBLER_DETUNE_CENTS_RANGE_DEFAULT);
  const float mrate = omx_clamp_or(mod_rate_hz, (float)OMX_DOUBLER_MOD_RATE_HZ_RANGE_MIN,
                                   (float)OMX_DOUBLER_MOD_RATE_HZ_RANGE_MAX, (float)OMX_DOUBLER_MOD_RATE_HZ_RANGE_DEFAULT);
  const float mdepth = omx_clamp_or(mod_depth_ms, (float)OMX_DOUBLER_MOD_DEPTH_MS_RANGE_MIN,
                                    (float)OMX_DOUBLER_MOD_DEPTH_MS_RANGE_MAX, (float)OMX_DOUBLER_MOD_DEPTH_MS_RANGE_DEFAULT);
  const float spread = omx_clamp_or(spread_pct, (float)OMX_DOUBLER_SPREAD_PCT_RANGE_MIN,
                                    (float)OMX_DOUBLER_SPREAD_PCT_RANGE_MAX, (float)OMX_DOUBLER_SPREAD_PCT_RANGE_DEFAULT);
  const float mix_share = omx_clamp_or(mix_pct, (float)OMX_DOUBLER_MIX_PCT_RANGE_MIN, (float)OMX_DOUBLER_MIX_PCT_RANGE_MAX,
                                       (float)OMX_DOUBLER_MIX_PCT_RANGE_DEFAULT) /
                          100.0f;
  s->nv = (uint32_t)(voice_count + 0.5f);
  s->depth = mdepth * 0.001f * s->sr;
  s->lfo.inc = omx_lfo_inc(mrate, s->sr);
  const float reach = omx_fdelay_min_delay(OMX_DOUBLER_INSTANCE_ORDER);
  for (uint32_t k = 0; k < OMX_DOUBLER_INSTANCE_VOICES; k++) {
    s->mean[k] = dms * OMX_DOUBLER_INSTANCE_DELAYS[k] * 0.001f * s->sr;
    /* The sweep the voice has room for: half of it either side of the mean, past the drift, clear of
     * the kernel's reach. */
    const float room = s->mean[k] - s->depth - reach;
    const float full = OMX_DOUBLER_INSTANCE_WINDOW_MS * 0.001f * s->sr;
    const float window = 2.0f * room < full ? 2.0f * room : full;
    const struct omx_doppler was = s->head[k];
    omx_doppler_design(&s->head[k], (double)cents * (double)OMX_DOUBLER_INSTANCE_DETUNES[k], window > 1.0f ? window : 1.0f,
                       OMX_DOUBLER_INSTANCE_FADE);
    if (s->head[k].window != was.window || !(s->hs[k].pos < (double)s->head[k].window)) omx_doppler_start(&s->head[k], &s->hs[k]);
    float bl, br;
    omx_balance_law(spread * 0.01f * OMX_DOUBLER_INSTANCE_PANS[k], &bl, &br);
    s->gl[k] = bl / (float)s->nv;
    s->gr[k] = br / (float)s->nv;
  }
  s->mix = mix_share;
  s->enabled = bypass ? 0 : 1;
  OMX_POST(s->nv >= 1u && s->nv <= OMX_DOUBLER_INSTANCE_VOICES && s->mix >= 0.0f && s->mix <= 1.0f, "voices-and-mix-declared");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: the dry legs blended with the voices, in to out. `in_*` and
 * `out_*` may alias. Not ready, bypassed or a mix of 0 is the identity and moves no state.
 */
#define OMX_CONTRACT_STAGE "doubler/instance-run"
static inline void omx_doubler_instance_run(OmxDoublerInstance *s, const float *in_l, const float *in_r, float *out_l,
                                            float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready || !s->enabled || !(s->mix > 0.0f)) return;
  OMX_PRE(omx_block_finite(out_l, n) && omx_block_finite(out_r, n), "finite-in");
  const float dry = 1.0f - s->mix;
  for (uint32_t i = 0; i < n; i++) {
    const float xl = out_l[i], xr = out_r[i];
    omx_fdelay_write(&s->line, 0.5f * (xl + xr));
    float wl = 0.0f, wr = 0.0f;
    for (uint32_t k = 0; k < s->nv; k++) {
      const float drift = s->depth * omx_lfo_at(&s->lfo, (float)k / (float)s->nv);
      const float v = omx_doppler_read(&s->line, &s->head[k], &s->hs[k], s->mean[k] + drift);
      wl += s->gl[k] * v;
      wr += s->gr[k] * v;
    }
    omx_lfo_advance(&s->lfo);
    omx_wetdry_mix(&out_l[i], &out_r[i], xl, xr, wl, wr, dry, s->mix);
  }
  OMX_POST(omx_block_finite(out_l, n) && omx_block_finite(out_r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/** The frames of latency the instance introduces: none. */
static inline uint32_t omx_doubler_instance_latency(const OmxDoublerInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_DOUBLER_INSTANCE_H */
