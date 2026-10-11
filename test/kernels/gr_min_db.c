// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/fx/omx_deesser.h>
#include <omxdsp/omx_band_dyn.h>
#include <omxdsp/omx_dyn.h>
#include <omxdsp/omx_eq_design.h>

/* ---- gr_min_db: the deepest gain a dynamics kernel applied inside its last call ----------------
 * At every declared rate a 1 kHz tone at -6 dBFS primes the kernel for 50 ms, then one 10 ms call
 * carries the tone for 1 ms, a burst at -1 dBFS to 3 ms and silence after it; the gate is primed
 * with silence and its call is the tone, which opens it. The reading of that block is held three
 * ways: it is omx_gr_block_db() of the deepest gain the kernel applied, read exactly from the same
 * kernel keyed on the signal with unit legs (whose output is the applied gain itself, sample for
 * sample), so the 4x path's decimated gain and a factor switch's crossfade are covered; it is the
 * pinned figure below within GR_TOL_DB (the pins were rendered with glibc 2.39's log10f and powf;
 * the tolerance covers another libm); and it is deeper than the gain of the block's last sample by
 * more than GR_TOL_DB, so a reading taken from the last sample, the meter's defect, fails the pin
 * (the arm's own sabotage). A band-dynamics run and the de-esser in both modes are held the same
 * way against the band kernel's per-sample gain. A disabled slot leaves the reading untouched;
 * silence reads 0 dB. --------------------------------------------------------------------------- */

#define GR_MAX (192000u / 20u + 192000u / 100u)
#define GR_CALL 256u
#define GR_TOL_DB 0.01f

enum { GR_COMP, GR_COMP_4X, GR_SWITCH, GR_GATE, GR_BAND, GR_DEESS_SPLIT, GR_DEESS_WIDE, GR_CASES };
static const char *const GR_CASE_NAME[GR_CASES] = {"comp 1x", "comp 4x", "comp factor switch", "gate",
                                                   "band-dyn", "de-esser split", "de-esser wideband"};

/* the pinned readings, dB, per declared rate */
static const struct {
  float rate;
  float db[GR_CASES];
} GR_PINS[] = {
    {44100.0f, {-11.630863f, -17.018297f, -12.651721f, -40.000000f, -15.715191f, -15.715191f, -15.715191f}},
    {48000.0f, {-11.624855f, -17.017860f, -12.430568f, -40.000000f, -15.715153f, -15.715153f, -15.715153f}},
    {88200.0f, {-11.633806f, -17.017859f, -13.363934f, -40.000000f, -15.715954f, -15.715954f, -15.715954f}},
    {96000.0f, {-11.631761f, -17.017847f, -14.396821f, -40.000000f, -15.715511f, -15.715511f, -15.715511f}},
    {176400.0f, {-11.634081f, -17.017876f, -16.612999f, -40.000000f, -15.715900f, -15.715900f, -15.715900f}},
    {192000.0f, {-11.633525f, -17.017878f, -16.849129f, -40.000000f, -15.715725f, -15.715725f, -15.715725f}},
};

/* the tone, the burst over [from, to) and silence from `to` on (to == 0: the tone throughout) */
static float gr_signal(uint32_t i, float sr, uint32_t from, uint32_t to) {
  if (to != 0u && i >= to) return 0.0f;
  const float amp = i >= from && to != 0u ? 0.891251f : 0.5f;
  return amp * sinf((float)(2.0 * M_PI * 1000.0 * (double)i / (double)sr));
}

/* the measured block: 10 ms, its burst from 1 ms to 3 ms */
static uint32_t gr_block(float sr) { return (uint32_t)(0.01f * sr); }
static uint32_t gr_from(float sr) { return (uint32_t)(0.001f * sr); }
static uint32_t gr_to(float sr) { return (uint32_t)(0.003f * sr); }

static struct omx_dyn gr_atom(int mode, float att_ms, float sr) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = mode;
  d.gc.thresh_db = mode == OMX_DYN_BELOW ? -30.0f : -24.0f;
  d.gc.ratio = mode == OMX_DYN_BELOW ? 40.0f : 4.0f;
  d.gc.range_db = mode == OMX_DYN_BELOW ? -40.0f : 0.0f;
  d.gc.makeup_lin = 1.0f;
  d.detect = OMX_DETECT_PEAK;
  d.ovs_mode = OMX_DYN_OVS_AUTO;
  d.attack_ms = att_ms;
  d.attack_coeff = omx_pole_from_time_ms(att_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(mode == OMX_DYN_BELOW ? 20.0f : 5.0f, sr);
  return d;
}

static float gr_pin(float sr, int c) {
  for (size_t k = 0; k < sizeof GR_PINS / sizeof GR_PINS[0]; k++)
    if (GR_PINS[k].rate == sr) return GR_PINS[k].db[c];
  return NAN;
}

static void gr_hold(float sr, int c, float reading, float exact, float last_db) {
  const float pin = gr_pin(sr, c);
  char what[160];
  snprintf(what, sizeof what, "%s at %.0f Hz: the reading is the deepest applied gain, bit for bit", GR_CASE_NAME[c],
           (double)sr);
  ok(reading == exact, what, reading, exact);
  snprintf(what, sizeof what, "%s at %.0f Hz: the reading is the pinned figure", GR_CASE_NAME[c], (double)sr);
  ok(fabsf(reading - pin) <= GR_TOL_DB, what, reading, pin);
  snprintf(what, sizeof what, "%s at %.0f Hz: a last-sample reading misses the pin (sabotage)", GR_CASE_NAME[c],
           (double)sr);
  ok(!(fabsf(last_db - pin) <= GR_TOL_DB), what, last_db, pin);
  if (getenv("OMXDSP_GR_PRINT")) printf("GR %.0f %d %.6f\n", (double)sr, c, (double)reading);
}

/* the dynamics slot: one call on the signal and the same call keyed on it with unit legs */
static void gr_dyn_case(float sr, int c) {
  const uint32_t prime = (uint32_t)(0.05f * sr);
  const struct omx_dyn first = gr_atom(c == GR_GATE ? OMX_DYN_BELOW : OMX_DYN_ABOVE,
                                       c == GR_COMP_4X ? 0.2f : (c == GR_GATE ? 1.0f : 5.0f), sr);
  const struct omx_dyn last = c == GR_SWITCH ? gr_atom(OMX_DYN_ABOVE, 0.2f, sr) : first;
  static float sig[GR_MAX], ones[GR_MAX], l[GR_MAX];
  const uint32_t n = gr_block(sr);
  const uint32_t from = prime + gr_from(sr), to = prime + gr_to(sr);
  for (uint32_t i = 0; i < prime + n; i++) {
    /* the gate's block is the tone after silence: the closed gate opens inside it */
    sig[i] = c == GR_GATE ? (i < prime ? 0.0f : gr_signal(i, sr, 0u, 0u)) : gr_signal(i, sr, from, to);
    ones[i] = 1.0f;
    l[i] = sig[i];
  }
  struct omx_dyn_state a, b;
  omx_dyn_state_init(&a, 1u);
  omx_dyn_state_init(&b, 1u);
  for (uint32_t off = 0; off < prime; off += GR_CALL) {
    const uint32_t m = prime - off < GR_CALL ? prime - off : GR_CALL;
    /* the switch lands on the last priming call, so the measured call runs inside its crossfade */
    const struct omx_dyn *p = off + GR_CALL >= prime ? &last : &first;
    omx_dynamics(l + off, NULL, m, p, &a);
    omx_dynamics_keyed(ones + off, NULL, sig + off, m, p, &b);
  }
  const uint32_t owed = a.xfade_left;
  omx_dynamics(l + prime, NULL, n, &last, &a);
  omx_dynamics_keyed(ones + prime, NULL, sig + prime, n, &last, &b);
  float gmin = 1.0f;
  for (uint32_t i = 0; i < n; i++) gmin = ones[prime + i] < gmin ? ones[prime + i] : gmin;
  if (c == GR_SWITCH) ok(owed > 0u, "the measured call starts inside the crossfade", owed, 0.0);
  ok(a.gr_min_db == b.gr_min_db, "the self-detecting and the keyed slot read alike", a.gr_min_db, b.gr_min_db);
  gr_hold(sr, c, a.gr_min_db, omx_gr_block_db(gmin), omx_gr_block_db(ones[prime + n - 1u]));
}

/* the band kernel: the run's reading against its own per-sample gain on a copy of the state */
static void gr_band_case(float sr, int c) {
  const uint32_t prime = (uint32_t)(0.05f * sr);
  struct omx_dyn d = gr_atom(OMX_DYN_ABOVE, 1.0f, sr);
  d.gc.range_db = -18.0f;
  float bc[5];
  omx_eq_design_f(OMX_EQ_BANDPASS, 1000.0, 1.0, 0.0, sr, bc);
  static float l[GR_MAX];
  const uint32_t n = gr_block(sr);
  const uint32_t from = prime + gr_from(sr), to = prime + gr_to(sr);
  for (uint32_t i = 0; i < prime + n; i++) l[i] = gr_signal(i, sr, from, to);
  struct omx_deess de;
  memset(&de, 0, sizeof de);
  de.enabled = 1;
  de.mode = c == GR_DEESS_WIDE ? OMX_DEESS_WIDEBAND : OMX_DEESS_SPLIT;
  de.dyn = d;
  memcpy(de.bp_c, bc, sizeof bc);
  struct omx_deess_state st;
  omx_deess_state_init(&st);
  for (uint32_t off = 0; off < prime; off += GR_CALL) {
    const uint32_t m = prime - off < GR_CALL ? prime - off : GR_CALL;
    if (c == GR_BAND) omx_band_dyn_run(l + off, NULL, m, &d, bc, &st.band, 0, 1);
    else omx_deess_process(l + off, NULL, m, &de, &st);
  }
  struct omx_band_dyn_state copy = st.band;
  const struct omx_band_dyn_block bk = omx_band_dyn_begin(&d);
  float gmin = 1.0f, g = 1.0f;
  for (uint32_t i = 0; i < n; i++) {
    float bl, br;
    g = omx_band_dyn_detect(bc, &copy, &d.gc, &bk, l[prime + i], 0.0f, 0, &bl, &br);
    gmin = g < gmin ? g : gmin;
  }
  if (c == GR_BAND) omx_band_dyn_run(l + prime, NULL, n, &d, bc, &st.band, 0, 1);
  else omx_deess_process(l + prime, NULL, n, &de, &st);
  gr_hold(sr, c, st.band.gr_min_db, omx_gr_block_db(gmin), omx_gr_block_db(g));
}

static void arm_gr_min_db(void) {
  g_arm = "gr_min_db";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    ok(gr_pin(sr, 0) == gr_pin(sr, 0), "every declared rate has pinned readings", sr, 0.0);
    for (int c = GR_COMP; c <= GR_GATE; c++) gr_dyn_case(sr, c);
    for (int c = GR_BAND; c < GR_CASES; c++) gr_band_case(sr, c);

    /* a disabled slot leaves the reading alone; silence reads no reduction */
    struct omx_dyn d = gr_atom(OMX_DYN_ABOVE, 5.0f, sr);
    struct omx_dyn_state st;
    omx_dyn_state_init(&st, 1u);
    static float z[GR_CALL];
    memset(z, 0, sizeof z);
    st.gr_min_db = -7.0f;
    d.enabled = 0;
    omx_dynamics(z, NULL, GR_CALL, &d, &st);
    ok(st.gr_min_db == -7.0f, "a disabled dynamics slot leaves gr_min_db untouched", st.gr_min_db, -7.0);
    d.enabled = 1;
    omx_dynamics(z, NULL, GR_CALL, &d, &st);
    ok(st.gr_min_db == 0.0f, "silence through a comp reads 0 dB", st.gr_min_db, 0.0);
    struct omx_band_dyn_state bs;
    omx_band_dyn_state_init(&bs);
    bs.gr_min_db = -7.0f;
    float bc[5];
    omx_eq_design_f(OMX_EQ_BANDPASS, 1000.0, 1.0, 0.0, sr, bc);
    d.gc.range_db = -18.0f;
    omx_band_dyn_run(z, NULL, GR_CALL, &d, bc, &bs, 0, 0);
    ok(bs.gr_min_db == -7.0f, "a disabled band leaves gr_min_db untouched", bs.gr_min_db, -7.0);
    omx_band_dyn_run(z, NULL, GR_CALL, &d, bc, &bs, 0, 1);
    ok(bs.gr_min_db == 0.0f, "silence through a band reads 0 dB", bs.gr_min_db, 0.0);
  }
  ok(omx_gr_block_db(1.0f) == 0.0f, "unity gain reads 0 dB", omx_gr_block_db(1.0f), 0.0);
  ok(omx_gr_block_db(1.5f) == 0.0f, "a gain above unity reads 0 dB", omx_gr_block_db(1.5f), 0.0);
  ok(omx_gr_block_db(0.0f) == omx_lin_to_db(1e-6f), "a closed gate floors at -120 dB", omx_gr_block_db(0.0f), -120.0);
  ok(omx_gr_block_db(NAN) == omx_lin_to_db(1e-6f), "a NaN gain floors at -120 dB", omx_gr_block_db(NAN), -120.0);
  ok(fabsf(omx_gr_block_db(0.5f) + 6.0206f) < 1e-3f, "half gain reads -6.02 dB", omx_gr_block_db(0.5f), -6.0206);
  expect_clean();
}
