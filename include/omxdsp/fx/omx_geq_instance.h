/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_geq_instance.h — the native 31-band graphic EQ as a plugin instance: the shell's core, with
 * no format in it.
 *
 * The shape is omx-plugins' omx_delay_instance.h's, under the same HRP shells rule (one C core, N shells). The
 * core here is `omx_geq.h`'s `omx_geq_process` — THE SAME INLINE the console's graphic EQ stage
 * runs — and this file adds no sample arithmetic to it. What it adds is what a host's port model
 * needs and the kernel's atom does not carry:
 *
 *   1. DESIGN. The kernel reads designed sections, never a centre, a Q or a gain (graphic-eq-31
 *      spec §4 L4): on the desk the sections are designed on the control thread. A plugin's
 *      controls arrive on its ports, so the instance designs them itself, with the same words the
 *      desk uses: `omx_eq_design` (the channel EQ's matched bell, kept in double), OMX_EQ_PEAKING,
 *      at the ISO 266 nominal third-octave centres ({@link OMX_GEQ_INSTANCE_CENTRES_HZ}, core's
 *      `ISO_THIRD_OCTAVE_CENTRES_HZ` verbatim) and the declared band Q (OMX_GEQ_BAND_Q_DOUBLE). A
 *      section is redesigned only when ITS gain moved; a static instance designs nothing.
 *   2. CLAMP. Every band gain is clamped into the declared EQ gain travel (OMX_EQ_GAIN_RANGE_MIN..
 *      MAX — the graphic EQ declares no travel of its own; its `geq31` preset is EQ bells) and a
 *      non-finite word reads as 0 dB, flat (omx_clamp_or: a NaN gain carries no position). A band
 *      at exactly 0 dB is skipped by the kernel, so a flat instance is memcmp-identical to bypass.
 *   3. IN -> OUT. The kernel works IN PLACE on the two legs; a host connects separate input and
 *      output buffers (and may connect them to the same memory). run() copies in to out when they
 *      differ and runs the kernel on out. Identity when bypassed.
 *
 * Re-engaging from bypass CLEARS the section histories, as the delay instance clears its rings,
 * so a re-enabled EQ does not ring out history buffered before the bypass.
 *
 * LATENCY IS ZERO, AND PUBLISHED (omx_geq_latency()): a biquad cascade has group delay and no
 * delay line, and a host cannot tell "zero" from "never said".
 */
#ifndef OMX_GEQ_INSTANCE_H
#define OMX_GEQ_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_geq.h>
#include <omxcontract/omx_contract_limits.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_param.h>

/** Audio legs in and out: TWO, one coefficient set shared, one history per leg. */
#define OMX_GEQ_INSTANCE_CHANNELS 2

/** Frames of latency this plugin introduces: NONE — see the header note. */
#define OMX_GEQ_INSTANCE_LATENCY_FRAMES 0.0f

/** The ISO 266 nominal one-third-octave centres, 20 Hz–20 kHz: core/src/eq.ts
 * `ISO_THIRD_OCTAVE_CENTRES_HZ`, whose length the declarations generate as OMX_GEQ_BANDS. */
#define OMX_GEQ_INSTANCE_CENTRES_HZ OMX_ISO_THIRD_OCTAVE_CENTRES_HZ_INIT

static const double omx_geq_instance_centres_hz[] = OMX_GEQ_INSTANCE_CENTRES_HZ;
_Static_assert(sizeof omx_geq_instance_centres_hz / sizeof omx_geq_instance_centres_hz[0] == OMX_GEQ_BANDS,
               "the ISO centre table is the declared band count long");

/** One instance: the atom, the state, the gains the sections were designed at. */
typedef struct {
  double sr;
  struct omx_geq atom;
  struct omx_geq_state state;
  /** The clamped gain each section was last designed at, dB; `designed` 0 until the first. */
  float gain_db[OMX_GEQ_BANDS];
  double coeffs[OMX_GEQ_BANDS][5];
  int designed;
  /** The previous cycle's engaged flag, so a bypass->engaged edge can clear the histories. */
  int was_engaged;
  int ready;
} OmxGeqInstance;

/**
 * Bind an instance to its rate. Returns 1 when usable, 0 when not (a rate that is not positive and
 * finite, or one whose Nyquist does not clear the top centre); a refused init leaves `ready` clear
 * and {@link omx_geq_instance_run} is then the identity: absence is a wire, not a crash.
 */
static inline int omx_geq_instance_init(OmxGeqInstance *s, double sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!(sr > 2.0 * omx_geq_instance_centres_hz[OMX_GEQ_BANDS - 1]) || !isfinite(sr)) return 0;
  s->sr = sr;
  omx_geq_state_init(&s->state);
  s->ready = 1;
  return 1;
}

/** The engage-edge clear: every section's history on both legs back to silence. RT-safe: one
 * memset over the inline state. */
#define OMX_CONTRACT_STAGE "geq/instance-clear"
static inline void omx_geq_instance_clear(OmxGeqInstance *s) {
  if (!s || !s->ready) return;
  omx_geq_state_init(&s->state);
  OMX_POST(s->state.l[0][0] == 0.0 && s->state.r[OMX_GEQ_BANDS - 1][3] == 0.0, "cleared-state-is-silent");
}
#undef OMX_CONTRACT_STAGE

/**
 * Resolve the host's 31 band gains into the kernel's atom for one cycle.
 *
 * `bypass` non-zero -> the atom is disabled and the kernel passes through. Each gain is clamped
 * (see the header note); a section is redesigned only when its clamped gain moved, and the atom is
 * rebuilt only when one did.
 */
#define OMX_CONTRACT_STAGE "geq/instance-resolve"
static inline void omx_geq_instance_resolve(OmxGeqInstance *s, int bypass, const float gain_db[OMX_GEQ_BANDS]) {
  if (!s || !s->ready || !gain_db) return;
  /* CONTRACT (omx_contract.h). Every section the atom carries was designed from a gain inside the
   * declared travel, at its ISO centre and the declared Q, whatever the host's ports held. */
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_geq_instance_clear(s);
  s->was_engaged = engaged;
  int moved = !s->designed;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    const float g = omx_clamp_or(gain_db[k], OMX_EQ_GAIN_RANGE_MIN, OMX_EQ_GAIN_RANGE_MAX, 0.0f);
    if (s->designed && g == s->gain_db[k]) continue;
    s->gain_db[k] = g;
    omx_eq_design(OMX_EQ_PEAKING, omx_geq_instance_centres_hz[k], OMX_GEQ_BAND_Q_DOUBLE, g, s->sr, s->coeffs[k]);
    moved = 1;
  }
  s->designed = 1;
  if (moved) omx_geq_set(&s->atom, 1, (const double(*)[5])s->coeffs, s->gain_db);
  s->atom.enabled = engaged;
#ifdef OMX_CONTRACTS
  int inside = 1;
  for (int k = 0; k < OMX_GEQ_BANDS; k++)
    inside &= s->gain_db[k] >= OMX_EQ_GAIN_RANGE_MIN && s->gain_db[k] <= OMX_EQ_GAIN_RANGE_MAX &&
              omx_biquad_stable(s->atom.c[k]);
  OMX_POST(inside, "sections-inside-the-declared-travel-and-stable");
#endif
}
#undef OMX_CONTRACT_STAGE

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: copy in to out where they differ, run the console's kernel in
 * place on out. `in_*` and `out_*` may alias. Not ready, bypassed or flat is the identity byte for
 * byte.
 */
#define OMX_CONTRACT_STAGE "geq/instance-run"
static inline void omx_geq_instance_run(OmxGeqInstance *s, const float *in_l, const float *in_r, float *out_l,
                                        float *out_r, uint32_t n) {
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_geq_process(out_l, out_r, n, &s->atom, &s->state);
  OMX_POST(s->atom.enabled || (memcmp(out_l, in_l, (size_t)n * sizeof(float)) == 0 &&
                               memcmp(out_r, in_r, (size_t)n * sizeof(float)) == 0),
           "bypass-identity");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_GEQ_INSTANCE_H */
