// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The C side of the TypeScript-twin agreement tests (#1823).
 *
 * omx-dsp holds the law for a set of small kernels that the console also carries in TypeScript
 * (each header tags its kernel `@twin <name>`). A twin cannot be bit-identical to its kernel: the
 * kernel runs libm in double or float, the twin runs V8's math in double. So each twin is pinned
 * by a test that feeds both the same inputs and compares the answers within a stated tolerance.
 * This program is the kernel half. It reads requests on stdin, one per line,
 *
 *     <kernel> <number> <number> ...
 *
 * and answers each with one line, `ok <number> <number> ...` or `err <word>`. Numbers are decimal
 * (JavaScript's own shortest round-trip form in, "%.17g" out), so no value is rounded on the way.
 * Each kernel's argument layout is the comment above its handler. Float kernels are called with the
 * inputs narrowed to float and answer the float widened back, exactly as the engine calls them.
 *
 * It links omx-dsp (ts-twin-oracle.mjs says which build), never a copy of any maths of its own: a number
 * here that the library does not produce is a bug in this file.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/analysis/omx_align_fit.h>
#include <omxdsp/analysis/omx_hrp_correct.h>
#include <omxdsp/analysis/omx_hrp_track.h>
#include <omxdsp/analysis/omx_room.h>
#include <omxdsp/analysis/omx_rta.h>
#include <omxdsp/fx/omx_eq_instance.h>
#include <omxdsp/mix/omx_meter_view.h>
#include <omxdsp/mix/omx_pan_law.h>
#include <omxdsp/omx_balance_law.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_curve.h>
#include <omxdsp/omx_dyn.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_magnitude.h>
#include <omxdsp/omx_units.h>

#define MAXV 4096
#define MAXOUT 8192

typedef int (*handler)(const double *in, int n, double *out);

/* eq_design: kind freq q gain_db rate -> b0 b1 b2 a1 a2 */
static int h_eq_design(const double *in, int n, double *out) {
  if (n != 5) return -1;
  omx_eq_design((enum omx_eq_kind)(int)in[0], in[1], in[2], in[3], in[4], out);
  return 5;
}

/* eq_octaves_q: bw_oct -> q */
static int h_eq_octaves_q(const double *in, int n, double *out) {
  if (n != 1) return -1;
  out[0] = omx_eq_octaves_q(in[0]);
  return 1;
}

/* eq_band_is_identity: kind gain_db -> 0|1 */
static int h_eq_band_is_identity(const double *in, int n, double *out) {
  if (n != 2) return -1;
  out[0] = omx_eq_band_is_identity((enum omx_eq_kind)(int)in[0], in[1]);
  return 1;
}

/* eq_lv2_clamp_q: lv2_band_type q -> q (float) */
static int h_eq_lv2_clamp_q(const double *in, int n, double *out) {
  if (n != 2) return -1;
  out[0] = omx_eq_lv2_clamp_q((int)in[0], (float)in[1]);
  return 1;
}

/* eq_bank: eq_on rate nbands { kind freq gain q on }*nbands -> count { b0 b1 b2 a1 a2 enabled }*count */
static int h_eq_bank(const double *in, int n, double *out) {
  if (n < 3) return -1;
  const int nb = (int)in[2];
  if (nb < 0 || nb > 64 || n != 3 + 5 * nb) return -1;
  struct omx_eq_band bands[64];
  double coeffs[64][5];
  uint8_t enabled[64];
  for (int i = 0; i < nb; i++) {
    const double *b = in + 3 + 5 * i;
    bands[i].kind = (enum omx_eq_kind)(int)b[0];
    bands[i].freq_hz = b[1];
    bands[i].gain_db = b[2];
    bands[i].q = b[3];
    bands[i].on = (int)b[4];
  }
  const uint32_t cnt = omx_eq_bank_design((int)in[0], bands, (uint32_t)nb, in[1], coeffs, enabled);
  out[0] = cnt;
  for (uint32_t i = 0; i < cnt; i++) {
    for (int j = 0; j < 5; j++) out[1 + 6 * i + j] = coeffs[i][j];
    out[1 + 6 * i + 5] = enabled[i];
  }
  return 1 + 6 * (int)cnt;
}

/* eq_pass: hpf_on hpf_freq hpf_slope24 lpf_on lpf_freq lpf_slope24 rate -> count { b0 b1 b2 a1 a2 }*count */
static int h_eq_pass(const double *in, int n, double *out) {
  if (n != 7) return -1;
  const struct omx_eq_pass hpf = {(int)in[0], in[1], (int)in[2]};
  const struct omx_eq_pass lpf = {(int)in[3], in[4], (int)in[5]};
  double coeffs[OMX_EQ_PASS_MAX_SECTIONS][5];
  const uint32_t cnt = omx_eq_pass_design(&hpf, &lpf, in[6], coeffs);
  out[0] = cnt;
  for (uint32_t i = 0; i < cnt; i++)
    for (int j = 0; j < 5; j++) out[1 + 5 * i + j] = coeffs[i][j];
  return 1 + 5 * (int)cnt;
}

/* eq_band_magnitude_db: kind freq gain_db q at_hz rate floor_db -> dB */
static int h_eq_band_magnitude_db(const double *in, int n, double *out) {
  if (n != 7) return -1;
  const struct omx_eq_band b = {(enum omx_eq_kind)(int)in[0], in[1], in[2], in[3], 1};
  out[0] = omx_eq_band_magnitude_db(&b, in[4], in[5], in[6]);
  return 1;
}

/* eq_magnitude_sum_db: at_hz rate floor_db nbands { kind freq gain q }*nbands -> dB */
static int h_eq_magnitude_sum_db(const double *in, int n, double *out) {
  if (n < 4) return -1;
  const int nb = (int)in[3];
  if (nb < 0 || nb > 64 || n != 4 + 4 * nb) return -1;
  struct omx_eq_band bands[64];
  for (int i = 0; i < nb; i++) {
    const double *b = in + 4 + 4 * i;
    bands[i].kind = (enum omx_eq_kind)(int)b[0];
    bands[i].freq_hz = b[1];
    bands[i].gain_db = b[2];
    bands[i].q = b[3];
    bands[i].on = 1;
  }
  out[0] = omx_eq_magnitude_sum_db(bands, (uint32_t)nb, in[0], in[1], in[2]);
  return 1;
}

/* curve_points: kind family rate points has_extents x_min x_max nterms { role index value unit shape }*nterms
 *               -> code count { x y }*count (shape -1 for none; the enums are omx_curve.h's, by value) */
static int h_curve_points(const double *in, int n, double *out) {
  if (n < 8) return -1;
  const int nt = (int)in[7];
  if (nt < 0 || nt > 512 || n != 8 + 5 * nt) return -1;
  static struct omx_curve_term terms[512];
  for (int i = 0; i < nt; i++) {
    const double *t = in + 8 + 5 * i;
    terms[i].role = (enum omx_curve_roles)(int)t[0];
    terms[i].index = (uint32_t)t[1];
    terms[i].value = t[2];
    terms[i].unit = (enum omx_curve_units)(int)t[3];
    terms[i].shape = (int)t[4];
    terms[i].origin = OMX_CURVE_ORIGINS_USER;
  }
  const struct omx_curve_request rq = {(enum omx_curve_displays)(int)in[0], (enum omx_curve_families)(int)in[1], in[2],
                                       (uint32_t)in[3], in[4] != 0.0, in[5], in[6], terms, (uint32_t)nt, 0};
  double x[OMX_CURVE_POINTS_MAX], y[OMX_CURVE_POINTS_MAX];
  int code = -1;
  const uint32_t cnt = omx_curve_points(&rq, x, y, OMX_CURVE_POINTS_MAX, &code);
  out[0] = code;
  out[1] = cnt;
  for (uint32_t i = 0; i < cnt; i++) {
    out[2 + 2 * i] = x[i];
    out[3 + 2 * i] = y[i];
  }
  return 2 + 2 * (int)cnt;
}

/* curve_points_marked: kind family rate points has_extents x_min x_max user_only nterms
 *                      { role index value unit shape origin }*nterms
 *                      -> code count { x y }*count nmarks { freq depth_db q origin }*nmarks [{ y_user }*count] */
static int h_curve_points_marked(const double *in, int n, double *out) {
  if (n < 9) return -1;
  const int nt = (int)in[8];
  if (nt < 0 || nt > 512 || n != 9 + 6 * nt) return -1;
  static struct omx_curve_term terms[512];
  for (int i = 0; i < nt; i++) {
    const double *t = in + 9 + 6 * i;
    terms[i].role = (enum omx_curve_roles)(int)t[0];
    terms[i].index = (uint32_t)t[1];
    terms[i].value = t[2];
    terms[i].unit = (enum omx_curve_units)(int)t[3];
    terms[i].shape = (int)t[4];
    terms[i].origin = (enum omx_curve_origins)(int)t[5];
  }
  const struct omx_curve_request rq = {(enum omx_curve_displays)(int)in[0], (enum omx_curve_families)(int)in[1], in[2],
                                       (uint32_t)in[3], in[4] != 0.0, in[5], in[6], terms, (uint32_t)nt, in[7] != 0.0};
  double x[OMX_CURVE_POINTS_MAX], y[OMX_CURVE_POINTS_MAX], yu[OMX_CURVE_POINTS_MAX];
  struct omx_curve_mark marks[OMX_CURVE_BANDS_MAX];
  struct omx_curve_extra ex = {yu, marks, OMX_CURVE_BANDS_MAX, 0u};
  int code = -1;
  const uint32_t cnt = omx_curve_points_marked(&rq, x, y, OMX_CURVE_POINTS_MAX, &ex, &code);
  int k = 0;
  out[k++] = code;
  out[k++] = cnt;
  for (uint32_t i = 0; i < cnt; i++) {
    out[k++] = x[i];
    out[k++] = y[i];
  }
  out[k++] = cnt ? ex.nmarks : 0;
  for (uint32_t i = 0; cnt && i < ex.nmarks; i++) {
    out[k++] = marks[i].freq_hz;
    out[k++] = marks[i].depth_db;
    out[k++] = marks[i].q;
    out[k++] = marks[i].origin;
  }
  if (cnt && rq.user_only)
    for (uint32_t i = 0; i < cnt; i++) out[k++] = yu[i];
  return k;
}

/* gaincomp_db: mode thresh_db ratio knee_db range_db level_db -> dB (float, the per-sample form) */
static int h_gaincomp_db(const double *in, int n, double *out) {
  if (n != 6) return -1;
  const struct omx_gaincomp_params p = {(int)in[0], (float)in[1], (float)in[2], (float)in[3], (float)in[4], 1.0f};
  out[0] = omx_gaincomp_db(&p, (float)in[5]);
  return 1;
}

/* gaincomp_db_shifted: thresh_db ratio knee_db range_db shift_db level_db -> dB (float, BELOW mode) */
static int h_gaincomp_db_shifted(const double *in, int n, double *out) {
  if (n != 6) return -1;
  const struct omx_gaincomp_params p = {OMX_DYN_BELOW, (float)in[0], (float)in[1], (float)in[2], (float)in[3], 1.0f};
  out[0] = omx_gaincomp_db_shifted(&p, (float)in[4], (float)in[5]);
  return 1;
}

/* gate_knee: thresh_db knee_start_db knee_end_db -> knee_db shift_db (float) */
static int h_gate_knee(const double *in, int n, double *out) {
  if (n != 3) return -1;
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.gc.mode = OMX_DYN_BELOW;
  d.gc.thresh_db = (float)in[0];
  omx_gate_knee(&d, (float)in[1], (float)in[2]);
  out[0] = d.gc.knee_db;
  out[1] = d.knee_shift_db;
  return 2;
}

/* hrp_cut_db: excess_db amount max_cut_db -> cut_db (float) */
static int h_hrp_cut_db(const double *in, int n, double *out) {
  if (n != 3) return -1;
  out[0] = omx_hrp_cut_db((float)in[0], (float)in[1], (float)in[2]);
  return 1;
}

/* hrp_resonance_score: cut_db voice_confidence partial_confidence -> score (float) */
static int h_hrp_resonance_score(const double *in, int n, double *out) {
  if (n != 3) return -1;
  out[0] = omx_hrp_resonance_score((float)in[0], (float)in[1], (float)in[2]);
  return 1;
}

/* hrp_cents_between: a_hz b_hz -> cents (float) */
static int h_hrp_cents_between(const double *in, int n, double *out) {
  if (n != 2) return -1;
  out[0] = omx_hrp_cents_between((float)in[0], (float)in[1]);
  return 1;
}

/* pan_stereo: pan -> gain_l gain_r (float): the mono pan law over the stereo layout */
static int h_pan_stereo(const double *in, int n, double *out) {
  if (n != 1) return -1;
  float l = 0.0f, r = 0.0f;
  omx_pan_stereo((float)in[0], &l, &r);
  out[0] = l;
  out[1] = r;
  return 2;
}

/* balance_law: pan -> gain_l gain_r (float): the stereo balance law */
static int h_balance_law(const double *in, int n, double *out) {
  if (n != 1) return -1;
  float l = 0.0f, r = 0.0f;
  omx_balance_law((float)in[0], &l, &r);
  out[0] = l;
  out[1] = r;
  return 2;
}

/* lin_to_db: lin -> dB (float, floored at 1e-9) */
static int h_lin_to_db(const double *in, int n, double *out) {
  if (n != 1) return -1;
  out[0] = omx_lin_to_db((float)in[0]);
  return 1;
}

/* lin_to_db_d: lin floor_lin -> dB (double) */
static int h_lin_to_db_d(const double *in, int n, double *out) {
  if (n != 2) return -1;
  out[0] = omx_lin_to_db_d(in[0], in[1]);
  return 1;
}

/* db_to_lin: dB -> linear (float) */
static int h_db_to_lin(const double *in, int n, double *out) {
  if (n != 1) return -1;
  out[0] = omx_db_to_lin((float)in[0]);
  return 1;
}

/* eq_butterworth_qs: slope_24 -> sections q0 q1 (q1 is 0 for one section) */
static int h_eq_butterworth_qs(const double *in, int n, double *out) {
  if (n != 1) return -1;
  double q[2] = {0.0, 0.0};
  out[0] = omx_eq_butterworth_qs(in[0] != 0.0, q);
  out[1] = q[0];
  out[2] = q[1];
  return 3;
}

/* dyn_oversample_factor: enabled ovs_mode attack_ms topology -> factor (attack narrowed to float, as the
 * engine stores it) */
static int h_dyn_oversample_factor(const double *in, int n, double *out) {
  if (n != 4) return -1;
  struct omx_dyn p;
  memset(&p, 0, sizeof p);
  p.enabled = in[0] != 0.0;
  p.ovs_mode = (int)in[1];
  p.attack_ms = (float)in[2];
  p.topology = (int)in[3];
  out[0] = omx_dyn_oversample_factor(&p);
  return 1;
}

/* rta_fft_size_for: rate_hz -> window length */
static int h_rta_fft_size_for(const double *in, int n, double *out) {
  if (n != 1) return -1;
  out[0] = omx_rta_fft_size_for((uint32_t)in[0]);
  return 1;
}

/* align_correction: fit_delay_ms fit_invert applied_delay_ms applied_inverted travel_min travel_max
 *                   -> delay_ms inverted clamped */
static int h_align_correction(const double *in, int n, double *out) {
  if (n != 6) return -1;
  OmxAlignCorrection c;
  omx_align_correction(in[0], (uint32_t)in[1], in[2], (uint32_t)in[3], in[4], in[5], &c);
  out[0] = c.delay_ms;
  out[1] = c.inverted;
  out[2] = c.clamped;
  return 3;
}

static int read_modes(const double *in, int count, OmxRoomMode *modes) {
  for (int i = 0; i < count; i++) {
    modes[i].hz = in[4 * i];
    modes[i].positions = (uint32_t)in[4 * i + 1];
    modes[i].q = in[4 * i + 2];
    modes[i].above_median_db = in[4 * i + 3];
  }
  return 4 * count;
}

/* room_mode_eq_proposals: min_positions nmodes { hz positions q above_median_db }*nmodes
 *                         -> count { hz gain_db q positions bounded requested_db }*count */
static int h_room_mode_eq_proposals(const double *in, int n, double *out) {
  if (n < 2) return -1;
  const int nm = (int)in[1];
  if (nm < 0 || nm > 64 || n != 2 + 4 * nm) return -1;
  OmxRoomMode modes[64];
  OmxRoomModeEqProposal props[64];
  read_modes(in + 2, nm, modes);
  uint32_t cnt = 0;
  omx_room_mode_eq_proposals(modes, (uint32_t)nm, (uint32_t)in[0], props, 64u, &cnt);
  out[0] = cnt;
  for (uint32_t i = 0; i < cnt; i++) {
    out[1 + 6 * i + 0] = props[i].hz;
    out[1 + 6 * i + 1] = props[i].gain_db;
    out[1 + 6 * i + 2] = props[i].q;
    out[1 + 6 * i + 3] = props[i].positions;
    out[1 + 6 * i + 4] = props[i].bounded;
    out[1 + 6 * i + 5] = props[i].requested_db;
  }
  return 1 + 6 * (int)cnt;
}

/* room_mode_residual_db: target_hz nbins { hz }*nbins { magnitude_db }*nbins -> has_value value */
static int h_room_mode_residual_db(const double *in, int n, double *out) {
  if (n < 2) return -1;
  const int nb = (int)in[1];
  if (nb < 0 || nb > 1024 || n != 2 + 2 * nb) return -1;
  static double scratch[1024];
  double residual = 0.0;
  const int has = omx_room_mode_residual_db(in + 2, in + 2 + nb, (uint32_t)nb, in[0], scratch, &residual);
  out[0] = has;
  out[1] = has ? residual : 0.0;
  return 2;
}

/* room_mode_set_stable: tolerance_hz nbefore nafter { hz }*nbefore { hz }*nafter -> 0|1 */
static int h_room_mode_set_stable(const double *in, int n, double *out) {
  if (n < 3) return -1;
  const int nb = (int)in[1], na = (int)in[2];
  if (nb < 0 || na < 0 || nb > 64 || na > 64 || n != 3 + nb + na) return -1;
  OmxRoomMode before[64], after[64];
  unsigned char used[64];
  for (int i = 0; i < nb; i++) before[i] = (OmxRoomMode){in[3 + i], 2u, 5.0, 6.0};
  for (int i = 0; i < na; i++) after[i] = (OmxRoomMode){in[3 + nb + i], 2u, 5.0, 6.0};
  out[0] = omx_room_mode_set_stable(before, (uint32_t)nb, after, (uint32_t)na, in[0], used);
  return 1;
}

/* meter_view_fill_db: db floor_db -> fill */
static int h_meter_view_fill_db(const double *in, int n, double *out) {
  if (n != 2) return -1;
  out[0] = omx_meter_view_fill_db(in[0], in[1]);
  return 1;
}

/* meter_view_fill: level floor_db -> fill */
static int h_meter_view_fill(const double *in, int n, double *out) {
  if (n != 2) return -1;
  out[0] = omx_meter_view_fill(in[0], in[1]);
  return 1;
}

/* meter_view_step: fall_db_per_s floor_db { fill dt_s }*k -> the marker at each tick, from a fresh view */
static int h_meter_view_step(const double *in, int n, double *out) {
  if (n < 2 || (n - 2) % 2 != 0) return -1;
  struct omx_meter_view v = {0};
  const int k = (n - 2) / 2;
  for (int i = 0; i < k; i++) out[i] = omx_meter_view_step(&v, in[2 + 2 * i], in[3 + 2 * i], in[0], in[1]);
  return k;
}

/* meter_view_clip: { clip clear }*k -> the clip word at each tick (a clear is taken before the tick's clip) */
static int h_meter_view_clip(const double *in, int n, double *out) {
  if (n % 2 != 0) return -1;
  struct omx_meter_view v = {0};
  for (int i = 0; i < n / 2; i++) {
    if (in[2 * i + 1] != 0.0) omx_meter_view_clip_clear(&v);
    out[i] = omx_meter_view_clip(&v, in[2 * i] != 0.0);
  }
  return n / 2;
}

/* meter_view_ladder: steps hysteresis prev fill -> count (prev negative: none) */
static int h_meter_view_ladder(const double *in, int n, double *out) {
  if (n != 4) return -1;
  out[0] = omx_meter_view_ladder((int32_t)in[2], in[3], (uint32_t)in[0], in[1]);
  return 1;
}

/* meter_view_bar_step: steps prev level -> count */
static int h_meter_view_bar_step(const double *in, int n, double *out) {
  if (n != 3) return -1;
  out[0] = omx_meter_view_bar_step((int32_t)in[1], in[2], (uint32_t)in[0]);
  return 1;
}

/* meter_view_decimate: kind kept value -> kept */
static int h_meter_view_decimate(const double *in, int n, double *out) {
  if (n != 3) return -1;
  out[0] = omx_meter_view_decimate((enum omx_meter_kinds)(int)in[0], in[1], in[2]);
  return 1;
}

/* meter_view_gr: rate pole { deepest_db dt_s }*k -> the reading at each tick, from no reduction */
static int h_meter_view_gr(const double *in, int n, double *out) {
  if (n < 2 || (n - 2) % 2 != 0) return -1;
  struct omx_meter_gr m = {0.0f, 0.0f};
  const int k = (n - 2) / 2;
  for (int i = 0; i < k; i++)
    out[i] = (double)omx_meter_view_gr(&m, (float)in[2 + 2 * i], in[3 + 2 * i], (float)in[0], (float)in[1]);
  return k;
}

/* meter_view_unit_of: kind measurement -> unit (METER_UNITS position; the count for none) */
static int h_meter_view_unit_of(const double *in, int n, double *out) {
  if (n != 2) return -1;
  out[0] = (double)omx_meter_view_unit_of((enum omx_meter_kinds)(int)in[0], (enum omx_meter_measurements)(int)in[1]);
  return 1;
}

/* meter_gr_hold_ms: -> OMX_METER_GR_HOLD_MS */
static int h_meter_gr_hold_ms(const double *in, int n, double *out) {
  (void)in;
  if (n != 0) return -1;
  out[0] = (double)OMX_METER_GR_HOLD_MS;
  return 1;
}

static const struct {
  const char *name;
  handler fn;
} TABLE[] = {
    {"eq_design", h_eq_design},
    {"eq_octaves_q", h_eq_octaves_q},
    {"eq_band_is_identity", h_eq_band_is_identity},
    {"eq_lv2_clamp_q", h_eq_lv2_clamp_q},
    {"eq_bank", h_eq_bank},
    {"eq_pass", h_eq_pass},
    {"eq_band_magnitude_db", h_eq_band_magnitude_db},
    {"eq_magnitude_sum_db", h_eq_magnitude_sum_db},
    {"curve_points", h_curve_points},
    {"curve_points_marked", h_curve_points_marked},
    {"gaincomp_db", h_gaincomp_db},
    {"gaincomp_db_shifted", h_gaincomp_db_shifted},
    {"gate_knee", h_gate_knee},
    {"hrp_cut_db", h_hrp_cut_db},
    {"hrp_resonance_score", h_hrp_resonance_score},
    {"hrp_cents_between", h_hrp_cents_between},
    {"pan_stereo", h_pan_stereo},
    {"balance_law", h_balance_law},
    {"lin_to_db", h_lin_to_db},
    {"lin_to_db_d", h_lin_to_db_d},
    {"db_to_lin", h_db_to_lin},
    {"rta_fft_size_for", h_rta_fft_size_for},
    {"eq_butterworth_qs", h_eq_butterworth_qs},
    {"dyn_oversample_factor", h_dyn_oversample_factor},
    {"align_correction", h_align_correction},
    {"room_mode_eq_proposals", h_room_mode_eq_proposals},
    {"room_mode_residual_db", h_room_mode_residual_db},
    {"room_mode_set_stable", h_room_mode_set_stable},
    {"meter_view_fill_db", h_meter_view_fill_db},
    {"meter_view_fill", h_meter_view_fill},
    {"meter_view_step", h_meter_view_step},
    {"meter_view_clip", h_meter_view_clip},
    {"meter_view_ladder", h_meter_view_ladder},
    {"meter_view_bar_step", h_meter_view_bar_step},
    {"meter_view_decimate", h_meter_view_decimate},
    {"meter_view_gr", h_meter_view_gr},
    {"meter_view_unit_of", h_meter_view_unit_of},
    {"meter_gr_hold_ms", h_meter_gr_hold_ms},
};

/* The kernels the table answers, for a test that asks "is the oracle really here". */
static void list_kernels(void) {
  for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) printf("%s\n", TABLE[i].name);
}

/* The rates a rate-dependent twin is checked at: the contract's full oracle grid, as the library was built with it. */
static void list_rates(void) {
  static const float rates[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;
  for (uint32_t i = 0; i < OMX_ORACLE_FULL_RATES_COUNT; i++) printf("%.17g\n", (double)rates[i]);
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--list") == 0) {
    list_kernels();
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "--rates") == 0) {
    list_rates();
    return 0;
  }
  static char line[1 << 20];
  static double in[MAXV], out[MAXOUT];
  while (fgets(line, sizeof line, stdin)) {
    char *save = NULL;
    char *name = strtok_r(line, " \t\r\n", &save);
    if (!name) continue;
    int n = 0;
    for (char *tok; (tok = strtok_r(NULL, " \t\r\n", &save)) != NULL;) {
      if (n >= MAXV) {
        n = -2;
        break;
      }
      char *end = NULL;
      in[n++] = strtod(tok, &end);
      if (*end) {
        n = -3;
        break;
      }
    }
    if (n < 0) {
      printf("err bad-number\n");
      continue;
    }
    handler fn = NULL;
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++)
      if (strcmp(TABLE[i].name, name) == 0) fn = TABLE[i].fn;
    if (!fn) {
      printf("err unknown-kernel\n");
      continue;
    }
    const int k = fn(in, n, out);
    if (k < 0) {
      printf("err bad-arity\n");
      continue;
    }
    printf("ok");
    for (int i = 0; i < k; i++) printf(" %.17g", out[i]);
    printf("\n");
  }
  return 0;
}
