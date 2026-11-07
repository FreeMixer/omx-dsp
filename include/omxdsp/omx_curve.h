// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_curve.h
 * @brief One curve function for every surface: the points of a panel description's display from the
 *        description's own role words, the current values and the rate.
 *
 * A request is the display's kind (CURVE_DISPLAYS), the family that picks the model (CURVE_FAMILIES), the
 * sample rate, the grid points, optional extents, and the terms: each a role word (CURVE_ROLES), an index, a
 * plain value, the unit the description gives that role (CURVE_UNITS) and, for an EQ band, the shape the
 * caller resolved (CURVE_SHAPES). The request is already bound: the function parses no label and reads no
 * descriptor. It converts each value once (`coef` to dB through omx_lin_to_db(), `oct` to Q through
 * omx_eq_octaves_q(), `khz` to Hz) and answers ascending points:
 *
 * - `eq_response`: exactly omx_eq_curve_db() over the bands the terms make, by omx_eq_curve_bands()'s rule (a
 *   band switched off is not drawn, the head filters as Butterworth sections), x in Hz, y in dB, the bands'
 *   anchors included;
 * - `transfer_curve`: input dB to output dB over the grid, `x + g(x)` with `g` omx_gaincomp_db(), or
 *   omx_gaincomp_db_shifted() for a gate whose knee range omx_gate_knee() resolves, the make-up added when a
 *   `makeup` term is present.
 *
 * A missing term is a code, never a default (CURVE_CODES): no curve is drawn from a guess. The defaults that
 * are not guesses are the switches (a band or head filter with no `on` term runs) and the corner of a gain
 * computer (no `knee` term, or no gate knee range, is the hard knee the kernel runs at width 0).
 *
 * Not on the audio path: a control thread, a host's verb or a plugin's own window calls it. Design: one panel
 * derivation, §5.1.
 */
#ifndef OMX_CURVE_H
#define OMX_CURVE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "fx/omx_eq_instance.h"
#include "omx_eq_design.h"
#include "omx_gaincomp.h"
#include "omx_gate.h"
#include "omx_magnitude.h"
#include "omx_units.h"
#include "omxdsp_definitions.h"

/** @brief What omx_curve_points() answers in its code: CURVE_CODES, spelt as the design names them. */
enum omx_curve_code {
  OMX_CURVE_OK = OMX_CURVE_CODES_OK,                             /**< A curve. */
  OMX_CURVE_KIND_UNKNOWN = OMX_CURVE_CODES_KIND_UNKNOWN,         /**< A kind, family, shape or slope with no closed form. */
  OMX_CURVE_TERM_MISSING = OMX_CURVE_CODES_TERM_MISSING,         /**< A term the model needs is not in the request. */
  OMX_CURVE_UNIT_REFUSED = OMX_CURVE_CODES_UNIT_REFUSED,         /**< A term in a unit its role does not accept. */
  OMX_CURVE_VALUE_NOT_FINITE = OMX_CURVE_CODES_VALUE_NOT_FINITE, /**< A value, the rate or an extent not finite; a rate not positive. */
  OMX_CURVE_POINTS_RANGE = OMX_CURVE_CODES_POINTS_RANGE,         /**< Grid points, a band index or the extents out of range. */
  OMX_CURVE_ROOM = OMX_CURVE_CODES_ROOM,                         /**< The caller's buffers are too small. */
};

/** @brief The most bands an `eq_response` request can index: each drawn band adds three anchors to at least two
 *         grid points, and the whole is at most CURVE_POINTS_MAX. */
#define OMX_CURVE_BANDS_MAX ((OMX_CURVE_POINTS_MAX - 2u) / 3u)

/** @brief A term's shape when the request gives none: every term but a band's `band_type` or `band_freq`. */
#define OMX_CURVE_NO_SHAPE (-1)

/** @brief One bound term of a curve request: a role of the description, its value now, in its unit. */
struct omx_curve_term {
  enum omx_curve_roles role; /**< The role word. */
  uint32_t index;            /**< The band's index for an indexed role (`band_*`); 0 for any other. */
  double value;              /**< The control's plain value. */
  enum omx_curve_units unit; /**< The unit the description gives the role. */
  int shape;                 /**< An enum omx_curve_shapes, or OMX_CURVE_NO_SHAPE: for `band_type` the shape its
                              *   value selects, resolved by the caller from the role's `shapes`; for `band_freq`
                              *   the fixed shape of a band no control selects. */
  enum omx_curve_origins origin; /**< Where the band comes from (CURVE_ORIGINS): OMX_CURVE_ORIGINS_USER, the zero of
                                  *   an initializer that names none, or a planter's (the feedback corrector's two pools, the HRP, a room mode). A band's origin is
                                  *   its `band_freq` term's; the sum ignores it. */
};

/** @brief A curve request: a display of the description, already bound to values and a rate. */
struct omx_curve_request {
  enum omx_curve_displays kind;      /**< The display's kind. */
  enum omx_curve_families family;    /**< The family: picks the model and, for a transfer curve, the gain
                                      *   computer's mode. */
  double sample_rate;                /**< The instance's rate, Hz. */
  uint32_t points;                   /**< The grid points, 2 to CURVE_POINTS_MAX (an EQ's anchors on top). */
  int has_extents;                   /**< Non-zero when `x_min` and `x_max` are given; else CURVE_EXTENTS. */
  double x_min;                      /**< The low edge of x: Hz for an EQ, input dB for a transfer curve. */
  double x_max;                      /**< The high edge of x, above `x_min`. */
  const struct omx_curve_term *terms; /**< The terms; a later term of one role and index replaces an earlier. */
  uint32_t nterms;                   /**< Terms in `terms`. */
  int user_only;                     /**< Non-zero: omx_curve_points_marked() also answers the curve over the `user`
                                      *   bands alone, at the same x. */
};

/** @brief A band of an `eq_response` whose origin is not `user`: where a surface marks what the corrector plants. */
struct omx_curve_mark {
  double freq_hz;                /**< The band's centre, Hz. */
  double depth_db;               /**< The band's own magnitude at its centre, dB: a bell's gain, a notch's floor. */
  double q;                      /**< The band's Q. */
  enum omx_curve_origins origin; /**< Its origin, never OMX_CURVE_ORIGINS_USER. */
};

/** @brief The wire words of CURVE_ORIGINS, by value. */
static const char *const OMX_CURVE_ORIGIN_WORDS[OMX_CURVE_ORIGINS_COUNT] = {"user", "fbs-fixed", "fbs-live", "hrp",
                                                                            "room-mode"};

/** @brief What omx_curve_points_marked() answers beside the points; the caller's buffers, any of them NULL when not
 *         asked for. */
struct omx_curve_extra {
  double *y_user;               /**< The curve over the `user` bands alone at each x; room for the points. */
  struct omx_curve_mark *marks; /**< One per drawn band whose origin is not `user`, in band order. */
  uint32_t mark_room;           /**< Room in `marks`. */
  uint32_t nmarks;              /**< Set to the marks written. */
};

/** @brief The wire words of CURVE_DISPLAYS, by value: the description's display words. */
static const char *const OMX_CURVE_DISPLAY_WORDS[OMX_CURVE_DISPLAYS_COUNT] = {"eq_response", "transfer_curve"};
/** @brief The wire words of CURVE_FAMILIES, by value: the description's family words. */
static const char *const OMX_CURVE_FAMILY_WORDS[OMX_CURVE_FAMILIES_COUNT] = {"eq", "dynamics", "limiter", "gate", "deesser"};
/** @brief The wire words of CURVE_ROLES, by value: the description's role words. */
static const char *const OMX_CURVE_ROLE_WORDS[OMX_CURVE_ROLES_COUNT] = {
    "band_type", "band_freq", "band_gain", "band_q",  "band_on", "hpf_freq",   "hpf_slope",  "hpf_on",   "lpf_freq",
    "lpf_slope", "lpf_on",    "threshold", "ratio",   "knee",    "makeup",     "range",      "knee_start", "knee_end"};
/** @brief The wire words of CURVE_UNITS, by value: the param-contract extension's unit tokens. */
static const char *const OMX_CURVE_UNIT_WORDS[OMX_CURVE_UNITS_COUNT] = {"hz",   "khz", "db",   "dbfs", "dbu",
                                                                        "lufs", "coef", "oct", "none", "ratio"};
/** @brief The wire words of CURVE_SHAPES, by value; `unshaped` is spelt `-`. */
static const char *const OMX_CURVE_SHAPE_WORDS[OMX_CURVE_SHAPES_COUNT] = {
    "off", "bell", "lowshelf", "highshelf", "highpass", "lowpass", "notch", "-", "allpass1", "allpass2"};

/**
 * @brief The value of `word` in one of the word tables above, the way a host reads a bound line.
 * @param words One of the OMX_CURVE_*_WORDS tables.
 * @param n The table's count (the set's `OMX_CURVE_*_COUNT`).
 * @param word The word, NUL-terminated; NULL is no word.
 * @return The value, or -1 for a word the table does not hold.
 * @note Thread-safe: pure.
 * @thread control
 */
static inline int omx_curve_word(const char *const *words, uint32_t n, const char *word) {
  if (!word) return -1;
  for (uint32_t i = 0; i < n; i++)
    if (strcmp(words[i], word) == 0) return (int)i;
  return -1;
}

/**
 * @brief Whether `role` takes a value in `unit`: the conversions omx_curve_points() makes, and no other.
 *
 * A frequency in `hz` or `khz`; a band's gain in `db` or `coef`; a band's Q in `none` or `coef` (a Q) or `oct`
 * (a bandwidth); a choice or a switch in `none`; a head filter's slope in `none` or `db`, dB per octave; a
 * threshold in any level unit, read as dB; a ratio in `ratio` or `coef`; the other gain computer terms in `db`.
 * @param role The role.
 * @param unit The unit.
 * @return 1 when the role accepts the unit, else 0.
 * @note Thread-safe: a switch, pure.
 * @thread audio
 */
static inline int omx_curve_unit_accepted(enum omx_curve_roles role, enum omx_curve_units unit) {
  switch (role) {
  case OMX_CURVE_ROLES_BAND_FREQ:
  case OMX_CURVE_ROLES_HPF_FREQ:
  case OMX_CURVE_ROLES_LPF_FREQ:
    return unit == OMX_CURVE_UNITS_HZ || unit == OMX_CURVE_UNITS_KHZ;
  case OMX_CURVE_ROLES_BAND_GAIN:
    return unit == OMX_CURVE_UNITS_DB || unit == OMX_CURVE_UNITS_COEF;
  case OMX_CURVE_ROLES_BAND_Q:
    return unit == OMX_CURVE_UNITS_NONE || unit == OMX_CURVE_UNITS_COEF || unit == OMX_CURVE_UNITS_OCT;
  case OMX_CURVE_ROLES_BAND_TYPE:
  case OMX_CURVE_ROLES_BAND_ON:
  case OMX_CURVE_ROLES_HPF_ON:
  case OMX_CURVE_ROLES_LPF_ON:
    return unit == OMX_CURVE_UNITS_NONE;
  case OMX_CURVE_ROLES_HPF_SLOPE:
  case OMX_CURVE_ROLES_LPF_SLOPE:
    return unit == OMX_CURVE_UNITS_NONE || unit == OMX_CURVE_UNITS_DB;
  case OMX_CURVE_ROLES_THRESHOLD:
    return unit == OMX_CURVE_UNITS_DB || unit == OMX_CURVE_UNITS_DBFS || unit == OMX_CURVE_UNITS_DBU ||
           unit == OMX_CURVE_UNITS_LUFS;
  case OMX_CURVE_ROLES_RATIO:
    return unit == OMX_CURVE_UNITS_RATIO || unit == OMX_CURVE_UNITS_COEF;
  case OMX_CURVE_ROLES_KNEE:
  case OMX_CURVE_ROLES_MAKEUP:
  case OMX_CURVE_ROLES_RANGE:
  case OMX_CURVE_ROLES_KNEE_START:
  case OMX_CURVE_ROLES_KNEE_END:
    return unit == OMX_CURVE_UNITS_DB;
  }
  return 0;
}

/**
 * @brief A term's value in the model's unit: Hz for a frequency, dB for a gain, Q for a bandwidth, the plain
 *        value otherwise. The one conversion each value goes through.
 * @param t The term; its unit accepted by its role (omx_curve_unit_accepted()).
 * @return The converted value.
 * @note Thread-safe: pure.
 * @thread control
 */
static inline double omx_curve_term_value(const struct omx_curve_term *t) {
  switch (t->unit) {
  case OMX_CURVE_UNITS_KHZ: return t->value * 1000.0;
  case OMX_CURVE_UNITS_COEF:
    return t->role == OMX_CURVE_ROLES_BAND_GAIN ? (double)omx_lin_to_db((float)t->value) : t->value;
  case OMX_CURVE_UNITS_OCT: return omx_eq_octaves_q(t->value);
  default: return t->value;
  }
}

/**
 * @brief The design kind a drawn shape is.
 * @param shape An enum omx_curve_shapes.
 * @return The enum omx_eq_kind, or -1 for `off` and `unshaped`, which are not drawn.
 * @note Thread-safe: a switch, pure.
 * @thread audio
 */
static inline int omx_curve_shape_kind(int shape) {
  switch (shape) {
  case OMX_CURVE_SHAPES_BELL: return OMX_EQ_PEAKING;
  case OMX_CURVE_SHAPES_LOWSHELF: return OMX_EQ_LOWSHELF;
  case OMX_CURVE_SHAPES_HIGHSHELF: return OMX_EQ_HIGHSHELF;
  case OMX_CURVE_SHAPES_HIGHPASS: return OMX_EQ_HIGHPASS;
  case OMX_CURVE_SHAPES_LOWPASS: return OMX_EQ_LOWPASS;
  case OMX_CURVE_SHAPES_NOTCH: return OMX_EQ_NOTCH;
  case OMX_CURVE_SHAPES_ALLPASS1: return OMX_EQ_ALLPASS1;
  case OMX_CURVE_SHAPES_ALLPASS2: return OMX_EQ_ALLPASS2;
  default: return -1;
  }
}

/**
 * @brief The term of `role` at `index` in the request: the last, since a later term replaces an earlier.
 * @param rq The request.
 * @param role The role.
 * @param index The band's index, 0 for a role that is not indexed.
 * @return The term, or NULL when the request binds none.
 * @note Thread-safe: a scan, pure.
 * @thread audio
 */
static inline const struct omx_curve_term *omx_curve_find(const struct omx_curve_request *rq,
                                                          enum omx_curve_roles role, uint32_t index) {
  const struct omx_curve_term *found = NULL;
  for (uint32_t i = 0; i < rq->nterms; i++)
    if (rq->terms[i].role == role && rq->terms[i].index == index) found = &rq->terms[i];
  return found;
}

/**
 * @brief One head filter from its three roles: absent when no term of them is bound; else its corner and its
 *        slope are needed, 12 or 24 dB per octave (omx_eq_pass's two), and its switch runs it unless 0.
 * @param rq The request.
 * @param freq_role The filter's corner role.
 * @param slope_role Its slope role.
 * @param on_role Its switch role.
 * @param out The filter, set when the answer is OMX_CURVE_OK.
 * @param present Set to 1 when a term of the filter is bound, else 0.
 * @return OMX_CURVE_OK, OMX_CURVE_TERM_MISSING or OMX_CURVE_KIND_UNKNOWN (a slope the filters do not run).
 * @note Thread-safe: pure.
 * @thread control
 */
static inline int omx_curve_pass(const struct omx_curve_request *rq, enum omx_curve_roles freq_role,
                                 enum omx_curve_roles slope_role, enum omx_curve_roles on_role,
                                 struct omx_eq_pass *out, int *present) {
  const struct omx_curve_term *f = omx_curve_find(rq, freq_role, 0), *s = omx_curve_find(rq, slope_role, 0),
                              *on = omx_curve_find(rq, on_role, 0);
  *present = f || s || on;
  if (!*present) return OMX_CURVE_OK;
  /* the slope in dB per octave: from the role's declared enumeration (the caller resolves a choice to it) or a
   * control in dB; a bare choice value is no slope */
  if (!f || !s || s->unit != OMX_CURVE_UNITS_DB) return OMX_CURVE_TERM_MISSING;
  const double slope = omx_curve_term_value(s);
  if (slope != 12.0 && slope != 24.0) return OMX_CURVE_KIND_UNKNOWN;
  out->on = on ? omx_curve_term_value(on) != 0.0 : 1;
  out->freq_hz = omx_curve_term_value(f);
  out->slope_24 = slope == 24.0;
  return OMX_CURVE_OK;
}

/**
 * @brief An `eq_response`: the bands and head filters the terms make, drawn by omx_eq_curve_bands() and
 *        omx_eq_curve_db() from `x_min` to `x_max`, each band floored at CURVE_EXTENTS `eqFloorDb`.
 * @param rq The request, kind and family checked.
 * @param x_min The low edge, Hz, positive.
 * @param x_max The high edge, Hz, above `x_min`.
 * @param x The frequencies, Hz.
 * @param y The response, dB.
 * @param room Room in `x` and in `y`.
 * @param ex The marks and the user-only curve, or NULL.
 * @param code The answer's code.
 * @return The points written; 0 with a code other than OMX_CURVE_OK.
 * @note Thread-safe on distinct output.
 * @thread control
 */
static inline uint32_t omx_curve_eq(const struct omx_curve_request *rq, double x_min, double x_max, double *x,
                                    double *y, uint32_t room, struct omx_curve_extra *ex, int *code) {
  struct omx_eq_band bands[OMX_CURVE_BANDS_MAX];
  enum omx_curve_origins origin[OMX_CURVE_BANDS_MAX];
  uint32_t nb = 0;
  for (uint32_t i = 0; i < rq->nterms; i++) {
    const enum omx_curve_roles r = rq->terms[i].role;
    if (r > OMX_CURVE_ROLES_BAND_ON) continue;
    if (rq->terms[i].index >= OMX_CURVE_BANDS_MAX) return (*code = OMX_CURVE_POINTS_RANGE), 0u;
    if (rq->terms[i].index + 1u > nb) nb = rq->terms[i].index + 1u;
  }
  uint32_t used = 0;
  for (uint32_t b = 0; b < nb; b++) {
    const struct omx_curve_term *type = omx_curve_find(rq, OMX_CURVE_ROLES_BAND_TYPE, b),
                                *freq = omx_curve_find(rq, OMX_CURVE_ROLES_BAND_FREQ, b),
                                *gain = omx_curve_find(rq, OMX_CURVE_ROLES_BAND_GAIN, b),
                                *q = omx_curve_find(rq, OMX_CURVE_ROLES_BAND_Q, b),
                                *on = omx_curve_find(rq, OMX_CURVE_ROLES_BAND_ON, b);
    if (!type && !freq && !gain && !q && !on) continue; /* an index no term names is no band */
    if (!freq) return (*code = OMX_CURVE_TERM_MISSING), 0u;
    const int shape = type && type->shape != OMX_CURVE_NO_SHAPE ? type->shape : freq->shape;
    if (shape == OMX_CURVE_NO_SHAPE) return (*code = OMX_CURVE_TERM_MISSING), 0u;
    const int kind = omx_curve_shape_kind(shape);
    if (kind < 0) continue; /* off, or a choice with no shape (`-`): not drawn */
    const int gains = kind == OMX_EQ_PEAKING || kind == OMX_EQ_LOWSHELF || kind == OMX_EQ_HIGHSHELF;
    if (gains && !gain) return (*code = OMX_CURVE_TERM_MISSING), 0u;
    bands[used].kind = (enum omx_eq_kind)kind;
    bands[used].freq_hz = omx_curve_term_value(freq);
    bands[used].gain_db = gains ? omx_curve_term_value(gain) : 0.0;
    /* no Q control: the band's declared default (EQ_BAND_DEFAULTS), clamped for its shape as clampEqQFor clamps */
    bands[used].q = q ? omx_curve_term_value(q)
                      : (double)omx_eq_lv2_clamp_q(kind == OMX_EQ_NOTCH ? OMX_EQ_LV2_NOTCH : OMX_EQ_LV2_BELL,
                                                   (float)OMX_EQ_BAND_DEFAULTS_Q);
    bands[used].on = on ? omx_curve_term_value(on) != 0.0 : 1;
    origin[used] = freq->origin;
    used++;
  }
  struct omx_eq_pass hpf, lpf;
  int has_hpf = 0, has_lpf = 0;
  int c = omx_curve_pass(rq, OMX_CURVE_ROLES_HPF_FREQ, OMX_CURVE_ROLES_HPF_SLOPE, OMX_CURVE_ROLES_HPF_ON, &hpf, &has_hpf);
  if (c == OMX_CURVE_OK)
    c = omx_curve_pass(rq, OMX_CURVE_ROLES_LPF_FREQ, OMX_CURVE_ROLES_LPF_SLOPE, OMX_CURVE_ROLES_LPF_ON, &lpf, &has_lpf);
  if (c != OMX_CURVE_OK) return (*code = c), 0u;
  if (nb == 0 && !has_hpf && !has_lpf) return (*code = OMX_CURVE_TERM_MISSING), 0u;
  struct omx_eq_band drawn[OMX_CURVE_BANDS_MAX + OMX_EQ_PASS_MAX_SECTIONS];
  const uint32_t nd = omx_eq_curve_bands(1, bands, used, has_hpf ? &hpf : NULL, has_lpf ? &lpf : NULL, drawn);
  if (OMX_EQ_CURVE_CAP(rq->points, nd) > OMX_CURVE_POINTS_MAX) return (*code = OMX_CURVE_POINTS_RANGE), 0u;
  if (room < OMX_EQ_CURVE_CAP(rq->points, nd)) return (*code = OMX_CURVE_ROOM), 0u;
  if (ex && ex->marks) {
    uint32_t nm = 0;
    for (uint32_t b = 0; b < used; b++)
      if (bands[b].on && origin[b] != OMX_CURVE_ORIGINS_USER) nm++;
    if (nm > ex->mark_room) return (*code = OMX_CURVE_ROOM), 0u;
  }
  *code = OMX_CURVE_OK;
  const uint32_t n =
      omx_eq_curve_db(drawn, nd, x_min, x_max, rq->points, rq->sample_rate, OMX_CURVE_EXTENTS_EQ_FLOOR_DB, x, y);
  if (!ex) return n;
  /* the user bands and the head filters alone, the same rule over fewer bands, at the same x */
  struct omx_eq_band mine[OMX_CURVE_BANDS_MAX];
  uint32_t nmine = 0;
  ex->nmarks = 0;
  for (uint32_t b = 0; b < used; b++) {
    if (origin[b] == OMX_CURVE_ORIGINS_USER) {
      mine[nmine++] = bands[b];
      continue;
    }
    if (!bands[b].on || !ex->marks) continue;
    struct omx_curve_mark *m = &ex->marks[ex->nmarks++];
    m->freq_hz = bands[b].freq_hz;
    m->depth_db = omx_eq_band_magnitude_db(&bands[b], bands[b].freq_hz, rq->sample_rate, OMX_CURVE_EXTENTS_EQ_FLOOR_DB);
    m->q = bands[b].q;
    m->origin = origin[b];
  }
  if (ex->y_user) {
    struct omx_eq_band drawn_user[OMX_CURVE_BANDS_MAX + OMX_EQ_PASS_MAX_SECTIONS];
    const uint32_t nu = omx_eq_curve_bands(1, mine, nmine, has_hpf ? &hpf : NULL, has_lpf ? &lpf : NULL, drawn_user);
    const double hi = rq->sample_rate / 2.0 * 0.999;
    for (uint32_t i = 0; i < n; i++)
      ex->y_user[i] = omx_eq_magnitude_sum_db(drawn_user, nu, x[i] < hi ? x[i] : hi, rq->sample_rate,
                                              OMX_CURVE_EXTENTS_EQ_FLOOR_DB);
  }
  return n;
}

/**
 * @brief A `transfer_curve`: the gain computer of the family's mode at each grid level, `y = x + g(x)` plus the
 *        make-up, from `x_min` to `x_max` dB in `points` evenly spaced levels, both edges included.
 *
 * ABOVE (dynamics, de-esser) needs a threshold and a ratio, a limiter a threshold (its ratio is infinity); BELOW (gate) a threshold, a range and a
 * ratio, and the knee range both its ends or neither, resolved as the gate resolves it (omx_gate_knee()). A
 * value outside the gain computer's domain is held at its edge, the bound its precondition names: a ratio at
 * least the mode's minimum (OMX_COMP_RATIO_MIN, OMX_GATE_RATIO_MIN), a knee at least OMX_COMP_KNEE_DB_MIN, a
 * gate's range at most OMX_GATE_RANGE_DB_MAX.
 * @param rq The request, kind and family checked.
 * @param x_min The lowest input level, dB.
 * @param x_max The highest input level, dB, above `x_min`.
 * @param x The input levels, dB.
 * @param y The output levels, dB.
 * @param room Room in `x` and in `y`.
 * @param code The answer's code.
 * @return The points written; 0 with a code other than OMX_CURVE_OK.
 * @note Thread-safe on distinct output.
 * @thread control
 */
static inline uint32_t omx_curve_transfer(const struct omx_curve_request *rq, double x_min, double x_max, double *x,
                                          double *y, uint32_t room, int *code) {
  const int below = rq->family == OMX_CURVE_FAMILIES_GATE;
  const struct omx_curve_term *thr = omx_curve_find(rq, OMX_CURVE_ROLES_THRESHOLD, 0),
                              *ratio = omx_curve_find(rq, OMX_CURVE_ROLES_RATIO, 0),
                              *knee = omx_curve_find(rq, OMX_CURVE_ROLES_KNEE, 0),
                              *makeup = omx_curve_find(rq, OMX_CURVE_ROLES_MAKEUP, 0),
                              *range = omx_curve_find(rq, OMX_CURVE_ROLES_RANGE, 0),
                              *ks = omx_curve_find(rq, OMX_CURVE_ROLES_KNEE_START, 0),
                              *ke = omx_curve_find(rq, OMX_CURVE_ROLES_KNEE_END, 0);
  /* a limiter has no ratio role: the dynamics form at ratio infinity, a hard corner at the threshold */
  const int limiter = rq->family == OMX_CURVE_FAMILIES_LIMITER;
  if (!thr || (!ratio && !limiter) || (below && !range) || (below && !ks != !ke))
    return (*code = OMX_CURVE_TERM_MISSING), 0u;
  if (room < rq->points) return (*code = OMX_CURVE_ROOM), 0u;
  const float r = ratio ? (float)omx_curve_term_value(ratio) : INFINITY, k = knee ? (float)omx_curve_term_value(knee) : 0.0f;
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.gc.mode = below ? OMX_DYN_BELOW : OMX_DYN_ABOVE;
  d.gc.thresh_db = (float)omx_curve_term_value(thr);
  const float rmin = below ? OMX_GATE_RATIO_MIN : OMX_COMP_RATIO_MIN;
  d.gc.ratio = r < rmin ? rmin : r;
  d.gc.knee_db = k < OMX_COMP_KNEE_DB_MIN ? OMX_COMP_KNEE_DB_MIN : k;
  d.gc.makeup_lin = 1.0f;
  if (below) {
    const float rg = (float)omx_curve_term_value(range);
    d.gc.range_db = rg > OMX_GATE_RANGE_DB_MAX ? OMX_GATE_RANGE_DB_MAX : rg;
    if (ks) omx_gate_knee(&d, (float)omx_curve_term_value(ks), (float)omx_curve_term_value(ke));
  }
  const double mk = makeup ? omx_curve_term_value(makeup) : 0.0;
  const uint32_t n = rq->points;
  for (uint32_t i = 0; i < n; i++) {
    const double lv = i + 1u == n ? x_max : x_min + (x_max - x_min) * (double)i / (double)(n - 1u);
    const float g = below ? omx_gaincomp_db_shifted(&d.gc, d.knee_shift_db, (float)lv) : omx_gaincomp_db(&d.gc, (float)lv);
    x[i] = lv;
    y[i] = lv + (double)g + mk;
  }
  *code = OMX_CURVE_OK;
  return n;
}

#define OMX_CONTRACT_STAGE "curve/points"
/**
 * @brief The points of one display of a panel description, and the marks of the bands a feedback corrector planted:
 *        the one curve function every surface calls, a host's `curve_points` verb, a plugin's own window and the twin
 *        `curvePoints` alike (omx_curve_points() is it with no marks).
 *
 * Checked in this order, the first failure answered: the kind and family (OMX_CURVE_KIND_UNKNOWN: an
 * `eq_response` of the `eq` family, a `transfer_curve` of `dynamics`, `limiter`, `gate` or `deesser`); the grid
 * points, 2 to CURVE_POINTS_MAX (OMX_CURVE_POINTS_RANGE); the rate, the extents and every value finite and the
 * rate positive (OMX_CURVE_VALUE_NOT_FINITE); every unit accepted by its role (OMX_CURVE_UNIT_REFUSED); the
 * extents bounding a range, an EQ's above 0 Hz (OMX_CURVE_POINTS_RANGE); then the model's own terms
 * (OMX_CURVE_TERM_MISSING, OMX_CURVE_KIND_UNKNOWN), an EQ's anchors within CURVE_POINTS_MAX
 * (OMX_CURVE_POINTS_RANGE) and the room (OMX_CURVE_ROOM). With no extents, an EQ runs from CURVE_EXTENTS
 * `freqMinHz` to the lower of `freqMaxHz` and `nyquistFraction` of Nyquist, a transfer curve from `levelMinDb`
 * to `levelMaxDb`.
 * @param rq The request.
 * @param x The x of each point, ascending: Hz, or input dB.
 * @param y The y of each point: dB, or output dB.
 * @param room Room in `x` and in `y`: CURVE_POINTS_MAX always suffices.
 * @param ex NULL, or the marks of the bands whose origin is not `user` and, when the request sets `user_only`, the
 *        curve over the `user` bands alone at the same x (OMX_CURVE_ROOM when `user_only` asks for it and `y_user` is
 *        NULL, or the marks outnumber `mark_room`). A transfer curve has no mark, and its user-only curve is itself.
 * @param code Set to an enum omx_curve_code.
 * @return The points written; 0 when `*code` is not OMX_CURVE_OK.
 * @post `ascending`.
 * @note Thread-safe on distinct output, reentrant.
 * @twin curvePoints
 * @thread control
 */
static inline uint32_t omx_curve_points_marked(const struct omx_curve_request *rq, double *x, double *y,
                                               uint32_t room, struct omx_curve_extra *ex, int *code) {
  const int eq = rq->kind == OMX_CURVE_DISPLAYS_EQ_RESPONSE;
  if (!(eq ? rq->family == OMX_CURVE_FAMILIES_EQ
           : rq->kind == OMX_CURVE_DISPLAYS_TRANSFER_CURVE && rq->family != OMX_CURVE_FAMILIES_EQ &&
                 (unsigned)rq->family < OMX_CURVE_FAMILIES_COUNT))
    return (*code = OMX_CURVE_KIND_UNKNOWN), 0u;
  if (rq->points < 2u || rq->points > OMX_CURVE_POINTS_MAX) return (*code = OMX_CURVE_POINTS_RANGE), 0u;
  if (!isfinite(rq->sample_rate) || !(rq->sample_rate > 0.0) ||
      (rq->has_extents && !(isfinite(rq->x_min) && isfinite(rq->x_max))))
    return (*code = OMX_CURVE_VALUE_NOT_FINITE), 0u;
  for (uint32_t i = 0; i < rq->nterms; i++)
    if (!isfinite(rq->terms[i].value)) return (*code = OMX_CURVE_VALUE_NOT_FINITE), 0u;
  for (uint32_t i = 0; i < rq->nterms; i++)
    if ((unsigned)rq->terms[i].role >= OMX_CURVE_ROLES_COUNT || (unsigned)rq->terms[i].origin >= OMX_CURVE_ORIGINS_COUNT ||
        !omx_curve_unit_accepted(rq->terms[i].role, rq->terms[i].unit))
      return (*code = OMX_CURVE_UNIT_REFUSED), 0u;
  double lo, hi;
  if (rq->has_extents) {
    lo = rq->x_min;
    hi = rq->x_max;
  } else if (eq) {
    const double nyq = OMX_CURVE_EXTENTS_NYQUIST_FRACTION_DOUBLE * rq->sample_rate / 2.0;
    lo = OMX_CURVE_EXTENTS_FREQ_MIN_HZ;
    hi = nyq < OMX_CURVE_EXTENTS_FREQ_MAX_HZ ? nyq : OMX_CURVE_EXTENTS_FREQ_MAX_HZ;
  } else {
    lo = OMX_CURVE_EXTENTS_LEVEL_MIN_DB;
    hi = OMX_CURVE_EXTENTS_LEVEL_MAX_DB;
  }
  if (!(hi > lo) || (eq && !(lo > 0.0))) return (*code = OMX_CURVE_POINTS_RANGE), 0u;
  if (ex && rq->user_only && !ex->y_user) return (*code = OMX_CURVE_ROOM), 0u;
  const uint32_t n = eq ? omx_curve_eq(rq, lo, hi, x, y, room, ex, code) : omx_curve_transfer(rq, lo, hi, x, y, room, code);
  if (ex && !eq && n) {
    ex->nmarks = 0;
    if (ex->y_user)
      for (uint32_t i = 0; i < n; i++) ex->y_user[i] = y[i]; /* no band of a transfer curve has an origin */
  }
  int ascending = 1;
  for (uint32_t i = 1; i < n; i++) ascending &= x[i - 1] <= x[i];
  OMX_POST(ascending, "ascending");
  (void)ascending; /* read only by the post-condition, compiled out in a release build */
  return n;
}

/**
 * @brief omx_curve_points_marked() with no marks and no user-only curve: the points alone.
 * @param rq The request.
 * @param x The x of each point, ascending.
 * @param y The y of each point.
 * @param room Room in `x` and in `y`.
 * @param code Set to an enum omx_curve_code.
 * @return The points written; 0 when `*code` is not OMX_CURVE_OK.
 * @note Thread-safe on distinct output, reentrant.
 * @thread control
 */
static inline uint32_t omx_curve_points(const struct omx_curve_request *rq, double *x, double *y, uint32_t room,
                                        int *code) {
  return omx_curve_points_marked(rq, x, y, room, NULL, code);
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_CURVE_H */
