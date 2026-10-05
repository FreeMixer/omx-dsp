// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_fader_law.h
 * @brief THE fader law: fader dB, pan, send, mute and DCA resolve to ONE linear coefficient — the
 *        engine, the fader plugin and the channel-strip plugin share this and nothing else.
 *
 * Operator decision 2026-10-04: the fader, the pan law, a send and a DCA never touch audio on
 * their own; each only sets one entry of the summing matrix G (omx_mixmatrix.h). This header is
 * that entry's one definition: `g = fader · pan · send · dca · mute · dca_mute`. Stateless, pure.
 * Design: docs/design/specs/2026-10-04-mix-matrix.md.
 */
#ifndef OMX_FADER_LAW_H
#define OMX_FADER_LAW_H

#include <math.h>

#include "omx_contract.h"
#include "omx_units.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** @brief Pan law leg selector: no pan — a mono output, unity gain whatever `pan` holds. */
#define OMX_PAN_LEG_NONE 0
/** @brief Pan law leg selector: the left leg of a stereo output. */
#define OMX_PAN_LEG_L 1
/** @brief Pan law leg selector: the right leg of a stereo output. */
#define OMX_PAN_LEG_R 2

/** @brief One G entry's resolved inputs: a strip feeding one output of the summing matrix. */
struct omx_fader_law_params {
  float fader_db; /**< The channel fader, dB; finite. */
  float pan;      /**< The pan law input, in [-1, 1]; read only when pan_leg is L or R. */
  int pan_leg;    /**< OMX_PAN_LEG_NONE, _L or _R: which leg of the output this entry is. */
  float send_lin; /**< The send/bus level, linear amplitude, not negative; 1 for a tap that carries the fader as is. */
  int mute;       /**< Non-zero mutes the strip into this output. */
  float dca_lin;  /**< The assigned DCA's own linear gain; 1 when no DCA is assigned. */
  int dca_mute;   /**< Non-zero when the assigned DCA is muted; 0 when none is assigned. */
};

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "fader_law/pan"
/**
 * @brief The constant-power pan law's gain for one leg.
 * @param pan The pan input, in [-1, 1]: -1 is hard left, +1 is hard right.
 * @param leg OMX_PAN_LEG_L or OMX_PAN_LEG_R.
 * @return `cos(theta)` for the L leg, `sin(theta)` for the R leg, `theta = (pan+1)·pi/4`: `gL²+gR²
 *         = 1` at every pan, so panning never changes the signal's total power.
 * @pre `pan-in-range`, `leg-is-stereo`.
 * @post `finite`.
 * @note RT-safe: one `cosf` or `sinf`, both on the RT-safe allowlist. Thread-safe: pure.
 */
static inline float omx_pan_law_gain(float pan, int leg) {
  OMX_PRE(pan >= -1.0f && pan <= 1.0f, "pan-in-range");
  OMX_PRE(leg == OMX_PAN_LEG_L || leg == OMX_PAN_LEG_R, "leg-is-stereo");
  const float theta = (pan + 1.0f) * 0.25f * (float)M_PI;
  const float g = leg == OMX_PAN_LEG_L ? cosf(theta) : sinf(theta);
  OMX_POST(g - g == 0.0f, "finite");
  return g;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "fader_law/coeff"
/**
 * @brief THE fader law: one G entry, `fader · pan · send · dca · mute`, nothing else touches audio.
 * @param p The entry's resolved inputs.
 * @return The entry's linear coefficient, not negative.
 * @pre `fader-finite`, `send-not-negative`, `dca-not-negative`.
 * @post `finite`, `not-negative`.
 * @note RT-safe: omx_db_to_lin() (one `powf`) and at most one `cosf`/`sinf` through
 *       omx_pan_law_gain(), both on the RT-safe allowlist; no call otherwise. Thread-safe: pure.
 */
static inline float omx_fader_law_coeff(const struct omx_fader_law_params *p) {
  OMX_PRE(p->fader_db - p->fader_db == 0.0f, "fader-finite");
  OMX_PRE(p->send_lin >= 0.0f, "send-not-negative");
  OMX_PRE(p->dca_lin >= 0.0f, "dca-not-negative");
  const float fader = omx_db_to_lin(p->fader_db);
  const float pan = p->pan_leg == OMX_PAN_LEG_NONE ? 1.0f : omx_pan_law_gain(p->pan, p->pan_leg);
  const float mute = p->mute ? 0.0f : 1.0f;
  const float dca_mute = p->dca_mute ? 0.0f : 1.0f;
  const float g = fader * pan * p->send_lin * p->dca_lin * mute * dca_mute;
  OMX_POST(g - g == 0.0f, "finite");
  OMX_POST(g >= 0.0f, "not-negative");
  return g;
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_FADER_LAW_H */
