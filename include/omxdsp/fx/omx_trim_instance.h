/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_trim_instance.h — the console's input trim as a plugin instance: the lane's head gain, a
 * de-zippered ramp from the gain the last block ended on to the one resolved now, both legs from
 * the same start. No format in it.
 *
 * ## What it mirrors, and what it does not
 *
 * On the desk a lane is "trim-gain fill -> [ordered stages] -> level/pan/mute" (mix_lane.h): the
 * fill is openmixer's `omx_fill_gain` (pipewire-native mix_dsp.h), `out[i] = in[i] *
 * omx_ramp_at(r, i)` over omx_ramp.h's ramp, run once per leg from the same start gain
 * (mixer_rt.c, the trim-gain block). This face is that arithmetic, in place on the plugin's
 * output: `omx_ramp_begin` / `omx_ramp_at` / `omx_ramp_end`, the library's one ramp.
 *
 * The HPF and the LPF are NOT here. On the desk they are sections of the channel EQ's cascade
 * (`eqCoeffs` appends them after the bands; the contract declares hpfOn..lpfSlope as the eq
 * kernel's controls), so a plugin that wants them before its other stages runs the eq face
 * (omx_eq_instance.h) with every band off: one home per fact. Trim and that eq face, in that
 * order, are omx-strip's input stage bit for bit (test/fx/trim_instance.test.c, arm C, and the
 * golden digests).
 *
 * The desk folds the channel's polarity into this gain as a sign; the contract's trim kernel
 * declares no polarity control, so this face has none.
 *
 * ## The control
 *
 * `trim_db`, the trim kernel's one control `trimDb` (TRIM_RANGE, omx_contract_limits.h), in dB:
 * clamped into its declared travel, a non-finite word reading as its declared default (0 dB), and
 * turned into a linear gain with omx_db_to_lin. Bypass is the identity and holds the ramp where it
 * was, so re-engaging glides from the gain the last engaged block ended on. A fresh instance's
 * gain is unity: a first block at 0 dB is the identity, a first block elsewhere ramps from unity.
 * LATENCY IS ZERO. No allocation, no lock; every word of state is in the caller's
 * {@link OmxTrimInstance}.
 */
#ifndef OMX_TRIM_INSTANCE_H
#define OMX_TRIM_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxcontract/omx_contract_limits.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_ramp.h>
#include <omxdsp/omx_units.h>

#define OMX_TRIM_INSTANCE_CHANNELS 2
#define OMX_TRIM_INSTANCE_LATENCY_FRAMES 0.0f

/** The port default: the declaration's (TRIM_RANGE.default, dB). */
#define OMX_TRIM_INSTANCE_TRIM_DB_DEFAULT ((float)OMX_TRIM_RANGE_DEFAULT)

/** One instance: the linear gain the last block ended on and the one the next block ramps to. */
typedef struct {
  float cur, tgt;
  int bypass;
  int ready;
} OmxTrimInstance;

/** Bind an instance to its rate: unity gain, engaged. Returns 1 when usable, 0 when the rate is
 * not positive and finite (run() is then the identity). The gain does not depend on the rate; the
 * rate is checked so a host's refused instance behaves as every other face's. Allocates nothing. */
static inline int omx_trim_instance_init(OmxTrimInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!(sr > 0.0f) || sr - sr != 0.0f) return 0;
  s->cur = s->tgt = 1.0f;
  s->ready = 1;
  return 1;
}

/**
 * Resolve the host's control value for one cycle. `bypass` non-zero makes run() the identity.
 * The argument is the trim kernel's contract control, in its user unit: `trim_db` (dB), clamped
 * into TRIM_RANGE, a non-finite word reading as its default.
 */
#define OMX_CONTRACT_STAGE "trim-instance/resolve"
static inline void omx_trim_instance_resolve(OmxTrimInstance *s, int bypass, float trim_db) {
  if (!s || !s->ready) return;
  s->bypass = bypass ? 1 : 0;
  s->tgt = omx_db_to_lin(omx_clamp_or(trim_db, (float)OMX_TRIM_RANGE_MIN, (float)OMX_TRIM_RANGE_MAX,
                                      OMX_TRIM_INSTANCE_TRIM_DB_DEFAULT));
  OMX_POST(s->tgt > 0.0f && s->tgt - s->tgt == 0.0f, "a-finite-positive-gain");
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, then ramp both legs from
 * the same start gain to the resolved one, in place on out. `in_*` and `out_*` may alias. Not
 * ready, or bypassed, is the identity.
 */
static inline void omx_trim_instance_run(OmxTrimInstance *s, const float *in_l, const float *in_r, float *out_l,
                                         float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready || s->bypass) return;
  const struct omx_ramp g = omx_ramp_begin(&s->cur, s->tgt, n);
  for (uint32_t i = 0; i < n; i++) out_l[i] *= omx_ramp_at(g, i), out_r[i] *= omx_ramp_at(g, i);
  omx_ramp_end(g, &s->cur);
}

/** The frames of latency the instance introduces: none (a gain has no delay line). */
static inline uint32_t omx_trim_instance_latency(const OmxTrimInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_TRIM_INSTANCE_H */
