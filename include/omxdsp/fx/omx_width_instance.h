/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_width_instance.h — stereo width and pseudo-stereo as a plugin instance (FX-11): a mono keyboard
 * or playback made wide on the PA, a monitor mix narrowed. It adds no DSP to the omx_width word.
 *
 *   RESOLVE. {@link omx_width_instance_resolve} takes the width kernel's contract controls in their
 *   declared order (data/kernels/width.json), a travel clamped into its range and a non-finite word
 *   reading as its default, a choice outside its set reading as its default. widthPct/100 scales the
 *   side; pseudoMs becomes the delay in samples (omx_width_delay); bassMonoHz is the LR4 corner.
 *   RUN. The word over both legs, in to out. At its come-up values (100 %, no pseudo-stereo, no bass
 *   mono) the face is the input bit for bit.
 *
 * LATENCY IS ZERO: the Haas delay and the comb are the effect, the dry mid is frame-aligned.
 * Nothing is allocated: the word's ring is inline in {@link OmxWidthInstance}.
 */
#ifndef OMX_WIDTH_INSTANCE_H
#define OMX_WIDTH_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_param.h>
#include <omxdsp/omx_width.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_WIDTH_INSTANCE_CHANNELS 2

typedef struct {
  float sr;
  struct omx_width p;
  struct omx_width_state st;
  float wpct;  /**< widthPct as resolved */
  int pmode;   /**< pseudo as resolved, an OMX_WIDTH_PSEUDOS_* member */
  float pms;   /**< pseudoMs as resolved */
  int bmode;   /**< bassMono as resolved, an OMX_WIDTH_BASS_MONOS_* member */
  float bhz;   /**< bassMonoHz as resolved */
  int enabled;
  int ready;
} OmxWidthInstance;

static inline void omx_width_instance_resolve(OmxWidthInstance *s, int bypass, float width_pct, int pseudo, float pseudo_ms,
                                              int bass_mono, float bass_mono_hz);

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. The face comes up at its declared defaults, the identity. Allocates nothing. */
static inline int omx_width_instance_init(OmxWidthInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_oracle_full(sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  omx_width_instance_resolve(s, 0, (float)OMX_WIDTH_WIDTH_PCT_RANGE_DEFAULT, (int)OMX_WIDTH_PSEUDOS_DEFAULT,
                             (float)OMX_WIDTH_PSEUDO_MS_RANGE_DEFAULT, (int)OMX_WIDTH_BASS_MONOS_DEFAULT,
                             (float)OMX_WIDTH_BASS_MONO_HZ_RANGE_DEFAULT);
  return 1;
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the width kernel's contract controls in their declared order: the width (percent),
 * the pseudo-stereo (a WIDTH_PSEUDOS index), its delay (ms), the bass mono (a WIDTH_BASS_MONOS index)
 * and its corner (Hz).
 */
#define OMX_CONTRACT_STAGE "width/instance-resolve"
static inline void omx_width_instance_resolve(OmxWidthInstance *s, int bypass, float width_pct, int pseudo, float pseudo_ms,
                                              int bass_mono, float bass_mono_hz) {
  if (!s || !s->ready) return;
  s->wpct = omx_clamp_or(width_pct, (float)OMX_WIDTH_WIDTH_PCT_RANGE_MIN, (float)OMX_WIDTH_WIDTH_PCT_RANGE_MAX,
                         (float)OMX_WIDTH_WIDTH_PCT_RANGE_DEFAULT);
  s->pmode = pseudo >= 0 && pseudo < (int)OMX_WIDTH_PSEUDOS_COUNT ? pseudo : (int)OMX_WIDTH_PSEUDOS_DEFAULT;
  s->pms = omx_clamp_or(pseudo_ms, (float)OMX_WIDTH_PSEUDO_MS_RANGE_MIN, (float)OMX_WIDTH_PSEUDO_MS_RANGE_MAX,
                        (float)OMX_WIDTH_PSEUDO_MS_RANGE_DEFAULT);
  s->bmode = bass_mono == OMX_WIDTH_BASS_MONOS_ON ? 1 : 0;
  s->bhz = omx_clamp_or(bass_mono_hz, (float)OMX_WIDTH_BASS_MONO_HZ_RANGE_MIN, (float)OMX_WIDTH_BASS_MONO_HZ_RANGE_MAX,
                        (float)OMX_WIDTH_BASS_MONO_HZ_RANGE_DEFAULT);
  const int was_bass = s->p.bass_mono;
  omx_width_design(&s->p, s->wpct * 0.01f, s->pmode, omx_width_delay(s->pms, s->sr), s->bmode, (double)s->bhz, (double)s->sr);
  if (s->p.bass_mono && !was_bass) {
    memset(&s->st.xs, 0, sizeof s->st.xs);
    memset(&s->st.ap, 0, sizeof s->st.ap);
  }
  s->enabled = bypass ? 0 : 1;
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: the word over both legs, in to out. `in_*` and `out_*` may alias.
 * Not ready, bypassed or at the come-up values is the identity and moves no state.
 */
static inline void omx_width_instance_run(OmxWidthInstance *s, const float *in_l, const float *in_r, float *out_l,
                                          float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready || !s->enabled) return;
  omx_width_process(out_l, out_r, n, &s->p, &s->st);
}

/** The frames of latency the instance introduces: none. */
static inline uint32_t omx_width_instance_latency(const OmxWidthInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_WIDTH_INSTANCE_H */
