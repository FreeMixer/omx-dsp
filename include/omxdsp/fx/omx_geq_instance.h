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
 *   1. DESIGN. The kernel reads designed sections, never a centre, a Q or a gain (the design of the 31-band graphic EQ):
 *     on the desk the sections are designed on the control thread. A plugin's
 *      controls arrive on its ports, so the instance designs them itself, with the same words the
 *      desk uses: `omx_eq_design` (the channel EQ's matched bell, kept in double), OMX_EQ_PEAKING,
 *      at the ISO 266 nominal third-octave centres ({@link OMX_GEQ_INSTANCE_CENTRES_HZ}, core's
 *      `ISO_THIRD_OCTAVE_CENTRES_HZ` verbatim) and the declared band Q (OMX_GEQ_BAND_Q_DOUBLE). A
 *      section is redesigned only when ITS gain moved; a static instance designs nothing.
 *   2. CLAMP. Every band gain is clamped into the geq kernel's declared `band` travel
 *      (OMX_GEQ_BAND_RANGE_MIN..MAX, omxdsp_definitions.h) and a non-finite word reads as the
 *      declared default, 0 dB, flat (omx_clamp_or: a NaN gain carries no position). A band at
 *      exactly 0 dB is skipped by the kernel, so a flat instance is memcmp-identical to bypass.
 *   3. TRUE MODE. `standard` hands each fader to its section (the classic proportional build-up);
 *      `true` is the two-pass interaction solve of the band interaction: with
 *      `B(γ)_{ik}` band k's dB response at centre i per dB of its own gain, the first pass solves
 *      `B(OMX_GEQ_PROTO_DB)·γ = fader`, the second re-evaluates each column at the gain the first
 *      pass gave it (a band whose first-pass gain is 0 keeps the prototype column) and solves again;
 *      a section gain past OMX_GEQ_DESIGN_GAIN_MAX_DB clamps and the design says so (`clamped`).
 *      The solve is ~1900 magnitude evaluations and two 31 x 31 eliminations, so it is a CONTROL
 *      thread step, {@link omx_geq_instance_design}, never called from the audio callback; the
 *      callback only publishes the finished design, {@link omx_geq_instance_apply} (31 x 5 copies).
 *      A zero target is exact zeros in both modes (`B·0 = 0`), so flat stays the identity.
 *   4. IN -> OUT. The kernel works IN PLACE on the two legs; a host connects separate input and
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
#include <omxdsp/omxdsp_definitions.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_magnitude.h>
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

/** @brief The graphic EQ's design modes: the faders as section gains, or the
 *         two-pass interaction solve. */
enum omx_geq_mode {
  OMX_GEQ_MODE_STANDARD = 0, /**< Each section's gain is its fader: the proportional build-up. */
  OMX_GEQ_MODE_TRUE = 1      /**< The section gains solve the band interaction. */
};

/** @brief One finished design: what {@link omx_geq_instance_design} computes on the control thread
 *         and {@link omx_geq_instance_apply} publishes to the kernel. Plain inline data, no pointer. */
typedef struct {
  int ok;                              /**< 1 when designed; 0 when refused (apply ignores it). */
  int mode;                            /**< The mode it was designed in, an enum omx_geq_mode. */
  int clamped;                         /**< 1 when a `true` section gain hit OMX_GEQ_DESIGN_GAIN_MAX_DB. */
  float band_db[OMX_GEQ_BANDS];        /**< The faders after the travel clamp, dB: the target. */
  double section_db[OMX_GEQ_BANDS];    /**< The gain each section is designed at, dB. */
  float live_db[OMX_GEQ_BANDS];        /**< section_db in float; exactly 0 where the section is skipped. */
  double coeffs[OMX_GEQ_BANDS][5];     /**< `{b0, b1, b2, a1, a2}` per section, in double. */
} OmxGeqDesign;

/**
 * @brief Whether a rate can carry the 31 sections: positive, finite and with the top ISO centre
 *        below Nyquist (below it, the top two centres clamp onto one frequency and the
 *        interaction matrix is singular). The rule {@link omx_geq_instance_init} applies.
 * @param sr The sample rate, Hz.
 * @return 1 when usable, 0 when not.
 * @note Thread-safe: pure.
 */
static inline int omx_geq_instance_rate_ok(double sr) {
  return isfinite(sr) && sr > 2.0 * omx_geq_instance_centres_hz[OMX_GEQ_BANDS - 1];
}

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
  if (!omx_geq_instance_rate_ok(sr)) return 0;
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
 * @brief The bypass->engaged edge shared by resolve and apply: on the edge every history is cleared.
 * @param s The instance, ready.
 * @param bypass Non-zero when the stage is bypassed this cycle.
 * @return 1 when engaged, 0 when bypassed.
 * @note RT-safe: one memset on the edge, nothing else. Not thread-safe on one instance.
 */
static inline int omx_geq_instance_engage(OmxGeqInstance *s, int bypass) {
  const int engaged = bypass ? 0 : 1;
  if (engaged && !s->was_engaged) omx_geq_instance_clear(s);
  s->was_engaged = engaged;
  return engaged;
}

/**
 * Resolve the host's band gains into the kernel's atom for one cycle.
 *
 * `bypass` non-zero -> the atom is disabled and the kernel passes through. The argument is the geq
 * kernel's one contract control, `band` (dB), which the kernel takes once per section: an array of
 * OMX_GEQ_BANDS gains, the contract's own count, in ISO centre order. Each gain is clamped (see
 * the header note); a section is redesigned only when its clamped gain moved, and the atom is
 * rebuilt only when one did.
 */
#define OMX_CONTRACT_STAGE "geq/instance-resolve"
static inline void omx_geq_instance_resolve(OmxGeqInstance *s, int bypass, const float band[OMX_GEQ_BANDS]) {
  if (!s || !s->ready || !band) return;
  /* CONTRACT (omx_contract.h). Every section the atom carries was designed from a gain inside the
   * declared travel, at its ISO centre and the declared Q, whatever the host's ports held. */
  const int engaged = omx_geq_instance_engage(s, bypass);
  int moved = !s->designed;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    const float g = omx_clamp_or(band[k], (float)OMX_GEQ_BAND_RANGE_MIN, (float)OMX_GEQ_BAND_RANGE_MAX,
                                 (float)OMX_GEQ_BAND_RANGE_DEFAULT);
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
    inside &= s->gain_db[k] >= OMX_GEQ_BAND_RANGE_MIN && s->gain_db[k] <= OMX_GEQ_BAND_RANGE_MAX &&
              omx_biquad_stable(s->atom.c[k]);
  OMX_POST(inside, "sections-inside-the-declared-travel-and-stable");
#endif
}
#undef OMX_CONTRACT_STAGE

/** @brief The floor a band column's magnitude is read at, dB: a peaking section has no null, so it
 *         is never reached; it only keeps the evaluator finite. */
#define OMX_GEQ_INSTANCE_MAG_FLOOR_DB (-300.0)

/**
 * @brief Band k's response at every ISO centre per dB of its own gain: column k of `B(γ)`.
 * @param k The band, 0 .. OMX_GEQ_BANDS - 1.
 * @param gain_db The gain the column is evaluated at, dB, non-zero.
 * @param sr The sample rate, Hz, usable.
 * @param col The column, OMX_GEQ_BANDS rows.
 * @note CONTROL thread: one omx_eq_design() and OMX_GEQ_BANDS magnitude evaluations. Thread-safe: pure.
 */
static inline void omx_geq_instance_column(int k, double gain_db, double sr, double col[OMX_GEQ_BANDS]) {
  double c[5];
  omx_eq_design(OMX_EQ_PEAKING, omx_geq_instance_centres_hz[k], OMX_GEQ_BAND_Q_DOUBLE, gain_db, sr, c);
  for (int i = 0; i < OMX_GEQ_BANDS; i++)
    col[i] = omx_biquad_magnitude_db_d(c, omx_geq_instance_centres_hz[i], sr, OMX_GEQ_INSTANCE_MAG_FLOOR_DB) / gain_db;
}

/**
 * @brief One pass of the solve: `B(γ)·x = target`, every column k evaluated at `at_db[k]` (the
 *        prototype gain where that is 0), by Gaussian elimination with partial pivoting.
 * @param at_db The gain each column is evaluated at, dB.
 * @param target_db The right-hand side, dB.
 * @param sr The sample rate, Hz, usable.
 * @param x The solution, dB.
 * @return 1 when solved, 0 on a zero pivot or a non-finite solution (x is then unspecified).
 * @note CONTROL thread: OMX_GEQ_BANDS² magnitude evaluations and one 31 x 32 elimination on the
 *       stack (~8 KiB); no allocation. Thread-safe: pure.
 */
static inline int omx_geq_instance_solve_pass(const double at_db[OMX_GEQ_BANDS], const double target_db[OMX_GEQ_BANDS],
                                              double sr, double x[OMX_GEQ_BANDS]) {
  double m[OMX_GEQ_BANDS][OMX_GEQ_BANDS + 1];
  double col[OMX_GEQ_BANDS];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    omx_geq_instance_column(k, at_db[k] == 0.0 ? (double)OMX_GEQ_PROTO_DB : at_db[k], sr, col);
    for (int i = 0; i < OMX_GEQ_BANDS; i++) m[i][k] = col[i];
  }
  for (int i = 0; i < OMX_GEQ_BANDS; i++) m[i][OMX_GEQ_BANDS] = target_db[i];
  for (int c = 0; c < OMX_GEQ_BANDS; c++) {
    int pivot = c;
    for (int r = c + 1; r < OMX_GEQ_BANDS; r++)
      if (fabs(m[r][c]) > fabs(m[pivot][c])) pivot = r;
    if (pivot != c)
      for (int j = 0; j <= OMX_GEQ_BANDS; j++) {
        const double t = m[c][j];
        m[c][j] = m[pivot][j];
        m[pivot][j] = t;
      }
    const double p = m[c][c];
    if (p == 0.0) return 0;
    for (int r = c + 1; r < OMX_GEQ_BANDS; r++) {
      const double f = m[r][c] / p;
      if (f == 0.0) continue;
      for (int j = c; j <= OMX_GEQ_BANDS; j++) m[r][j] -= f * m[c][j];
    }
  }
  for (int r = OMX_GEQ_BANDS - 1; r >= 0; r--) {
    double acc = m[r][OMX_GEQ_BANDS];
    for (int j = r + 1; j < OMX_GEQ_BANDS; j++) acc -= m[r][j] * x[j];
    x[r] = acc / m[r][r];
    if (!isfinite(x[r])) return 0;
  }
  return 1;
}

#define OMX_CONTRACT_STAGE "geq/true-gains"
/**
 * @brief The `true` section gains for a target: the two-pass solve, then the ceiling.
 *
 * The target is NOT clamped to the fader travel here (the instance's design clamps first), so a
 * caller can ask for a curve the ceiling cannot reach: every gain past ±OMX_GEQ_DESIGN_GAIN_MAX_DB
 * is set to the ceiling with its sign and `*clamped` is 1, never silently.
 * @param target_db The curve at the ISO centres, dB, finite.
 * @param sr The sample rate, Hz.
 * @param gains_db The section gains, dB; all zero when refused.
 * @param clamped Set to 1 when a gain hit the ceiling, else 0; may be NULL.
 * @return 1 when solved; 0 when refused (a rate {@link omx_geq_instance_rate_ok} refuses, a
 *         non-finite target, a singular pass).
 * @post `gains-inside-the-ceiling`.
 * @note CONTROL thread only: two passes of omx_geq_instance_solve_pass(); never on the audio
 *       thread. No allocation. Thread-safe: pure.
 */
static inline int omx_geq_true_gains(const double target_db[OMX_GEQ_BANDS], double sr, double gains_db[OMX_GEQ_BANDS],
                                     int *clamped) {
  int over = 0, finite = 1;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    gains_db[k] = 0.0;
    finite &= isfinite(target_db[k]) ? 1 : 0;
  }
  if (clamped) *clamped = 0;
  if (!finite || !omx_geq_instance_rate_ok(sr)) return 0;
  double proto[OMX_GEQ_BANDS], first[OMX_GEQ_BANDS], second[OMX_GEQ_BANDS];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) proto[k] = (double)OMX_GEQ_PROTO_DB;
  if (!omx_geq_instance_solve_pass(proto, target_db, sr, first)) return 0;
  if (!omx_geq_instance_solve_pass(first, target_db, sr, second)) return 0;
  const double ceiling = (double)OMX_GEQ_DESIGN_GAIN_MAX_DB;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    double g = second[k];
    if (fabs(g) > ceiling) {
      g = copysign(ceiling, g);
      over = 1;
    }
    gains_db[k] = g;
    OMX_POST(fabs(gains_db[k]) <= ceiling, "gains-inside-the-ceiling");
  }
  if (clamped) *clamped = over;
  return 1;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "geq/instance-design"
/**
 * @brief Design one instance's sections in a mode, on the CONTROL thread, into a design the audio
 *        callback publishes with {@link omx_geq_instance_apply}.
 *
 * Each band word is clamped into the declared travel as resolve clamps it (a non-finite word is
 * the default, 0 dB). `standard`: the section gains are those faders, designed term for term as
 * resolve designs them, so applying a standard design is bit-identical to resolving the same words.
 * `true`: {@link omx_geq_true_gains} on the clamped faders.
 * @param s The instance, ready (its rate is read; nothing in it is written).
 * @param mode An enum omx_geq_mode; any other value is `standard`.
 * @param band The host's band words, dB, ISO centre order.
 * @param d The design, written whole; `d->ok` 0 when refused.
 * @return 1 when designed, 0 when refused (instance not ready, a refused solve).
 * @post `sections-finite-and-stable`.
 * @note CONTROL thread only: in `true` mode ~1900 magnitude evaluations and two eliminations.
 *       No allocation, no lock. Thread-safe on distinct designs; reads `s->sr` only.
 */
static inline int omx_geq_instance_design(const OmxGeqInstance *s, int mode, const float band[OMX_GEQ_BANDS],
                                          OmxGeqDesign *d) {
  if (!d) return 0;
  memset(d, 0, sizeof(*d));
  if (!s || !s->ready || !band) return 0;
  d->mode = mode == OMX_GEQ_MODE_TRUE ? OMX_GEQ_MODE_TRUE : OMX_GEQ_MODE_STANDARD;
  double target[OMX_GEQ_BANDS];
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    d->band_db[k] = omx_clamp_or(band[k], (float)OMX_GEQ_BAND_RANGE_MIN, (float)OMX_GEQ_BAND_RANGE_MAX,
                                 (float)OMX_GEQ_BAND_RANGE_DEFAULT);
    target[k] = d->band_db[k];
  }
  if (d->mode == OMX_GEQ_MODE_TRUE) {
    if (!omx_geq_true_gains(target, s->sr, d->section_db, &d->clamped)) return 0;
  } else {
    for (int k = 0; k < OMX_GEQ_BANDS; k++) d->section_db[k] = target[k];
  }
  int sound = 1;
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    /* A gain too small for a float is a skipped section, designed at exactly 0 dB: live and
     * designed never disagree. */
    d->live_db[k] = (float)d->section_db[k];
    if (d->live_db[k] == 0.0f) d->section_db[k] = 0.0;
    if (d->mode == OMX_GEQ_MODE_STANDARD)
      omx_eq_design(OMX_EQ_PEAKING, omx_geq_instance_centres_hz[k], OMX_GEQ_BAND_Q_DOUBLE, d->band_db[k], s->sr,
                    d->coeffs[k]);
    else
      omx_eq_design(OMX_EQ_PEAKING, omx_geq_instance_centres_hz[k], OMX_GEQ_BAND_Q_DOUBLE, d->section_db[k], s->sr,
                    d->coeffs[k]);
    sound &= omx_biquad_stable(d->coeffs[k]);
  }
  OMX_POST(sound, "sections-finite-and-stable");
  d->ok = sound;
  return d->ok;
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "geq/instance-apply"
/**
 * @brief Publish a finished design to the kernel for one cycle: the audio callback's share of a
 *        designed mode.
 *
 * `bypass` and the engage edge behave as in {@link omx_geq_instance_resolve}. A refused design
 * (`ok` 0) changes no section. After an apply, the next resolve redesigns every section from its
 * own words.
 * @param s The instance.
 * @param bypass Non-zero to bypass the stage this cycle.
 * @param d The design, from {@link omx_geq_instance_design} at this instance's rate.
 * @note RT-safe: OMX_GEQ_BANDS x 5 copies and omx_geq_set(); no libm design call, no allocation,
 *       no lock. Not thread-safe on one instance; `d` must not be written while this reads it.
 */
static inline void omx_geq_instance_apply(OmxGeqInstance *s, int bypass, const OmxGeqDesign *d) {
  if (!s || !s->ready || !d) return;
  const int engaged = omx_geq_instance_engage(s, bypass);
  if (d->ok) {
    memcpy(s->coeffs, d->coeffs, sizeof s->coeffs);
    memcpy(s->gain_db, d->band_db, sizeof s->gain_db);
    s->designed = 0;
    omx_geq_set(&s->atom, 1, (const double(*)[5])s->coeffs, d->live_db);
  }
  s->atom.enabled = engaged;
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
