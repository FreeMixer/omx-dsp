// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the one curve function (omx_curve.h): a description's display, bound, to points ------------- */

#include "fixtures/curve_requests.h"
#include "fixtures/curve_golden.h"

static const float CURVE_ORACLE_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;

/* The golden EQ's points may move by the libm of another platform (cos, sin, log10 in double; log10f for the
 * linear-factor gain), measured nowhere above 1e-12 dB; a change of law moves them by far more
 * (test/curve-sabotage.sh). The transfer curves are float arithmetic and one sqrtf, exact. */
#define CURVE_GOLDEN_EQ_TOL 1e-9

static uint32_t curve_run(const struct omx_curve_request *rq, double *x, double *y, int *code) {
  *code = -1;
  return omx_curve_points(rq, x, y, OMX_CURVE_POINTS_MAX, code);
}

/* One request, its terms replaced: the request of a code's arm. */
static int curve_code_of(const struct omx_curve_request *base, const struct omx_curve_term *terms, uint32_t n) {
  struct omx_curve_request rq = *base;
  rq.terms = terms;
  rq.nterms = n;
  double x[OMX_CURVE_POINTS_MAX], y[OMX_CURVE_POINTS_MAX];
  int code = -1;
  const uint32_t got = omx_curve_points(&rq, x, y, OMX_CURVE_POINTS_MAX, &code);
  return got == 0u ? code : OMX_CURVE_OK;
}

static void arm_curve(void) {
  g_arm = "curve";
  double x[OMX_CURVE_POINTS_MAX], y[OMX_CURVE_POINTS_MAX], dx[OMX_CURVE_POINTS_MAX], dy[OMX_CURVE_POINTS_MAX];
  int code;

  /* The wire words: one per value, each read back to its value, `-` the unshaped band. */
  struct { const char *const *w; uint32_t n; } tables[] = {
      {OMX_CURVE_DISPLAY_WORDS, OMX_CURVE_DISPLAYS_COUNT}, {OMX_CURVE_FAMILY_WORDS, OMX_CURVE_FAMILIES_COUNT},
      {OMX_CURVE_ROLE_WORDS, OMX_CURVE_ROLES_COUNT},       {OMX_CURVE_UNIT_WORDS, OMX_CURVE_UNITS_COUNT},
      {OMX_CURVE_SHAPE_WORDS, OMX_CURVE_SHAPES_COUNT}};
  for (uint32_t t = 0; t < 5u; t++)
    for (uint32_t i = 0; i < tables[t].n; i++)
      ok(omx_curve_word(tables[t].w, tables[t].n, tables[t].w[i]) == (int)i, "a wire word reads back to its value", t, i);
  ok(omx_curve_word(OMX_CURVE_SHAPE_WORDS, OMX_CURVE_SHAPES_COUNT, "-") == OMX_CURVE_SHAPES_UNSHAPED, "- is the unshaped band", 0, 0);
  ok(omx_curve_word(OMX_CURVE_ROLE_WORDS, OMX_CURVE_ROLES_COUNT, "attack") == -1 &&
     omx_curve_word(OMX_CURVE_ROLE_WORDS, OMX_CURVE_ROLES_COUNT, NULL) == -1, "a word the table does not hold is -1", 0, 0);
  ok(omx_curve_word(OMX_CURVE_ROLE_WORDS, OMX_CURVE_ROLES_COUNT, "knee_start") == OMX_CURVE_ROLES_KNEE_START &&
     omx_curve_word(OMX_CURVE_DISPLAY_WORDS, OMX_CURVE_DISPLAYS_COUNT, "transfer_curve") == OMX_CURVE_DISPLAYS_TRANSFER_CURVE,
     "the words are the description's snake-case words", 0, 0);

  /* The golden vector of each model, read back, with its marks and its user-only curve. */
  for (uint32_t k = 0; k < CURVE_GOLDEN_COUNT; k++) {
    double yu[OMX_CURVE_POINTS_MAX];
    struct omx_curve_mark mk[8];
    struct omx_curve_extra ex = {yu, mk, 8u, 0u};
    code = -1;
    const uint32_t n = omx_curve_points_marked(CURVE_GOLDEN_REQUESTS[k].rq, x, y, OMX_CURVE_POINTS_MAX, &ex, &code);
    ok(ex.nmarks == CURVE_GOLDEN[k].nmarks, "a golden request answers its marks", ex.nmarks, CURVE_GOLDEN[k].nmarks);
    for (uint32_t m = 0; m < ex.nmarks && m < CURVE_GOLDEN[k].nmarks; m++)
      ok(mk[m].freq_hz == CURVE_GOLDEN[k].marks[m].freq_hz && mk[m].q == CURVE_GOLDEN[k].marks[m].q &&
             mk[m].origin == CURVE_GOLDEN[k].marks[m].origin &&
             fabs(mk[m].depth_db - CURVE_GOLDEN[k].marks[m].depth_db) <= CURVE_GOLDEN_EQ_TOL,
         "a golden mark: frequency, Q and origin bit for bit, depth within the bound", mk[m].depth_db,
         CURVE_GOLDEN[k].marks[m].depth_db);
    for (uint32_t i = 0; i < n && i < CURVE_GOLDEN[k].n; i++)
      ok(fabs(yu[i] - CURVE_GOLDEN[k].y_user[i]) <= CURVE_GOLDEN_EQ_TOL, "a golden user-only y within its bound", yu[i],
         CURVE_GOLDEN[k].y_user[i]);
    ok(code == OMX_CURVE_OK && n == CURVE_GOLDEN[k].n, "a golden request answers its point count", n, CURVE_GOLDEN[k].n);
    const double tol = CURVE_GOLDEN_REQUESTS[k].rq->kind == OMX_CURVE_DISPLAYS_EQ_RESPONSE ? CURVE_GOLDEN_EQ_TOL : 0.0;
    double worst = 0.0;
    for (uint32_t i = 0; i < n && i < CURVE_GOLDEN[k].n; i++) {
      ok(x[i] == CURVE_GOLDEN[k].x[i], "a golden x, bit for bit", x[i], CURVE_GOLDEN[k].x[i]);
      const double gap = fabs(y[i] - CURVE_GOLDEN[k].y[i]);
      if (gap > worst) worst = gap;
      ok(gap <= tol, "a golden y within its bound", y[i], CURVE_GOLDEN[k].y[i]);
    }
    printf("curve: golden %s, %u points, largest gap %.3g (bound %.0e)\n", CURVE_GOLDEN_REQUESTS[k].name, n, worst, tol);
  }

  /* The EQ is omx_eq_curve_db over omx_eq_curve_bands, bit for bit, at every oracle rate: the bands built by hand
   * from the golden EQ's terms, each unit converted the one way the header names. */
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const double sr = CURVE_ORACLE_RATES[ri];
    struct omx_curve_request rq = CURVE_GOLDEN_EQ;
    rq.sample_rate = sr;
    rq.points = 64u;
    const uint32_t n = curve_run(&rq, x, y, &code);
    const struct omx_eq_band bands[4] = {
        {OMX_EQ_PEAKING, 1000.0, 6.0, omx_eq_octaves_q(1.0), 1},
        {OMX_EQ_LOWSHELF, 100.0, (double)omx_lin_to_db(2.0f), 0.7071, 1},
        {OMX_EQ_HIGHSHELF, 8000.0, -4.0, 0.7071, 1},
        {OMX_EQ_NOTCH, 3000.0, 0.0, 8.0, 1}};
    const struct omx_eq_pass hpf = {1, 40.0, 1}, lpf = {1, 16000.0, 0};
    struct omx_eq_band drawn[4 + OMX_EQ_PASS_MAX_SECTIONS];
    const uint32_t nd = omx_eq_curve_bands(1, bands, 4u, &hpf, &lpf, drawn);
    const double hi = 0.999 * sr / 2.0 < 20000.0 ? 0.999 * sr / 2.0 : 20000.0;
    const uint32_t m = omx_eq_curve_db(drawn, nd, 20.0, hi, 64u, sr, -120.0, dx, dy);
    ok(code == OMX_CURVE_OK && n == m, "the EQ answers omx_eq_curve_db's count", n, m);
    uint32_t same = 0;
    for (uint32_t i = 0; i < n && i < m; i++) same += x[i] == dx[i] && y[i] == dy[i];
    ok(same == m, "the EQ is omx_eq_curve_db, bit for bit", same, m);
    ok(x[0] == 20.0 && x[n - 1u] == hi, "the default extents: 20 Hz to the lower of 20 kHz and 0.999 Nyquist", x[n - 1u], hi);
  }

  /* Two FBS notches and an HRP cut: the sum is the curve over every band, the user-only curve is the curve over the user bands and the
   * head filter alone at every point, and each notch is a mark of its origin. Without origins, no mark. */
  {
    double yu[OMX_CURVE_POINTS_MAX], all[OMX_CURVE_POINTS_MAX], sum[OMX_CURVE_POINTS_MAX];
    struct omx_curve_mark mk[4];
    struct omx_curve_extra ex = {yu, mk, 4u, 0u}; /* three marks: the two FBS pools and the HRP */
    const uint32_t n = omx_curve_points_marked(&CURVE_GOLDEN_FBS, x, y, OMX_CURVE_POINTS_MAX, &ex, &code);
    const struct omx_eq_band user[2] = {{OMX_EQ_PEAKING, 120.0, 3.0, 0.8, 1}, {OMX_EQ_HIGHSHELF, 4000.0, 2.0, 0.7071, 1}};
    const struct omx_eq_band every[5] = {user[0], user[1], {OMX_EQ_NOTCH, 2500.0, 0.0, 30.0, 1},
                                         {OMX_EQ_PEAKING, 630.0, -9.0, 16.0, 1}, {OMX_EQ_PEAKING, 1800.0, -4.5, 24.0, 1}};
    const struct omx_eq_pass hpf = {1, 60.0, 0};
    struct omx_eq_band du[2 + OMX_EQ_PASS_MAX_SECTIONS], da[5 + OMX_EQ_PASS_MAX_SECTIONS];
    const uint32_t nu = omx_eq_curve_bands(1, user, 2u, &hpf, NULL, du), na = omx_eq_curve_bands(1, every, 5u, &hpf, NULL, da);
    uint32_t same_user = 0, same_all = 0, below = 0;
    for (uint32_t i = 0; i < n; i++) {
      const double xi = x[i] < 0.999 * 24000.0 ? x[i] : 0.999 * 24000.0;
      same_user += yu[i] == omx_eq_magnitude_sum_db(du, nu, xi, 48000.0, -120.0);
      all[i] = omx_eq_magnitude_sum_db(da, na, xi, 48000.0, -120.0);
      same_all += y[i] == all[i];
      below += y[i] <= yu[i] + 1e-12;
    }
    memcpy(sum, y, n * sizeof(double));
    ok(code == OMX_CURVE_OK && n > 0 && same_user == n, "the user-only curve is the user bands' at every point", same_user, n);
    ok(same_all == n, "the sum keeps the notches: the curve over every band", same_all, n);
    ok(below == n, "the notches only take away: the sum is at or under the user-only curve", below, n);
    ok(ex.nmarks == 3u && mk[0].origin == OMX_CURVE_ORIGINS_FBS_FIXED && mk[0].freq_hz == 2500.0 && mk[0].q == 30.0 &&
           mk[0].depth_db == -120.0 && mk[1].origin == OMX_CURVE_ORIGINS_FBS_LIVE && mk[1].freq_hz == 630.0 &&
           mk[2].origin == OMX_CURVE_ORIGINS_HRP && mk[2].freq_hz == 1800.0 && mk[2].q == 24.0,
       "a mark per planted band (both FBS pools and the HRP), its origin, frequency, Q and depth", ex.nmarks, 3.0);
    ex.nmarks = 9u;
    omx_curve_points_marked(&CURVE_GOLDEN_EQ, x, y, OMX_CURVE_POINTS_MAX, &ex, &code);
    ok(code == OMX_CURVE_OK && ex.nmarks == 0u, "a request with no origin draws no mark", ex.nmarks, 0.0);
    struct omx_curve_extra small = {yu, mk, 2u, 0u};
    ok(omx_curve_points_marked(&CURVE_GOLDEN_FBS, x, y, OMX_CURVE_POINTS_MAX, &small, &code) == 0u && code == OMX_CURVE_ROOM,
       "three marks for room of two: room", code, 0);
    struct omx_curve_extra nouser = {NULL, mk, 4u, 0u};
    ok(omx_curve_points_marked(&CURVE_GOLDEN_FBS, x, y, OMX_CURVE_POINTS_MAX, &nouser, &code) == 0u && code == OMX_CURVE_ROOM,
       "user_only with no buffer for it: room", code, 0);
    ok(omx_curve_points(&CURVE_GOLDEN_FBS, x, all, OMX_CURVE_POINTS_MAX, &code) == n && memcmp(all, sum, n * sizeof(double)) == 0,
       "omx_curve_points is the same sum, marks or none", code, 0);
    struct omx_curve_term bad[] = {CT(BAND_FREQ, 0, 100.0, HZ, OMX_CURVE_SHAPES_NOTCH), CT(BAND_Q, 0, 4.0, NONE, CT_NS)};
    bad[0].origin = (enum omx_curve_origins)OMX_CURVE_ORIGINS_COUNT;
    ok(curve_code_of(&CURVE_GOLDEN_EQ, bad, 2u) == OMX_CURVE_UNIT_REFUSED, "an origin past the set: refused", 0, 0);
  }

  /* A transfer curve is x + the gain computer, bit for bit, in each mode. */
  {
    const uint32_t n = curve_run(&CURVE_GOLDEN_COMP, x, y, &code);
    const struct omx_gaincomp_params p = {OMX_DYN_ABOVE, -20.0f, 4.0f, 6.0f, 0.0f, 1.0f};
    uint32_t same = 0;
    for (uint32_t i = 0; i < n; i++) same += y[i] == x[i] + (double)omx_gaincomp_db(&p, (float)x[i]) + 3.0;
    ok(code == OMX_CURVE_OK && n == 41u && same == n, "the compressor is x + omx_gaincomp_db + make-up", same, n);
    ok(x[0] == -80.0 && x[n - 1u] == 0.0, "the default extents: -80 to 0 dB", x[0], x[n - 1u]);
  }
  {
    const uint32_t n = curve_run(&CURVE_GOLDEN_GATE, x, y, &code);
    struct omx_dyn d;
    memset(&d, 0, sizeof d);
    d.gc = (struct omx_gaincomp_params){OMX_DYN_BELOW, -40.0f, 4.0f, 0.0f, -60.0f, 1.0f};
    omx_gate_knee(&d, -46.0f, -38.0f);
    uint32_t same = 0;
    for (uint32_t i = 0; i < n; i++) same += y[i] == x[i] + (double)omx_gaincomp_db_shifted(&d.gc, d.knee_shift_db, (float)x[i]);
    ok(code == OMX_CURVE_OK && same == n, "the gate is x + omx_gaincomp_db_shifted over omx_gate_knee", same, n);
    ok(d.knee_shift_db != 0.0f, "the golden gate's knee range sits off the threshold", d.knee_shift_db, 0.0);
  }

  /* Given extents are read; a later term replaces an earlier one. */
  {
    struct omx_curve_request rq = CURVE_GOLDEN_COMP;
    rq.has_extents = 1;
    rq.x_min = -60.0;
    rq.x_max = -10.0;
    const uint32_t n = curve_run(&rq, x, y, &code);
    ok(code == OMX_CURVE_OK && x[0] == -60.0 && x[n - 1u] == -10.0, "given extents bound the grid", x[0], x[n - 1u]);
    const struct omx_curve_term twice[] = {CT(THRESHOLD, 0, -10.0, DB, CT_NS), CT(RATIO, 0, 2.0, RATIO, CT_NS),
                                           CT(THRESHOLD, 0, -30.0, DB, CT_NS)};
    rq = CURVE_GOLDEN_COMP;
    rq.terms = twice;
    rq.nterms = 3u;
    curve_run(&rq, x, y, &code);
    ok(y[30] == -20.0 + -5.0, "the later threshold of two is the one drawn", y[30], -25.0);
  }

  /* Every code, each by the request that earns it. */
  const struct omx_curve_term eq1[] = {CT(BAND_FREQ, 0, 1000.0, HZ, OMX_CURVE_SHAPES_BELL), CT(BAND_GAIN, 0, 3.0, DB, CT_NS),
                                       CT(BAND_Q, 0, 1.0, NONE, CT_NS)};
  ok(curve_code_of(&CURVE_GOLDEN_EQ, eq1, 3u) == OMX_CURVE_OK, "a one-band EQ draws", 0, 0);
  {
    /* A band switched off is not drawn, nor a head filter switched off: the curve is the flat line. */
    const struct omx_curve_term parked[] = {CT(BAND_FREQ, 0, 1000.0, HZ, OMX_CURVE_SHAPES_BELL), CT(BAND_GAIN, 0, 9.0, DB, CT_NS),
                                            CT(BAND_Q, 0, 1.0, NONE, CT_NS), CT(BAND_ON, 0, 0.0, NONE, CT_NS),
                                            CT(HPF_FREQ, 0, 200.0, HZ, CT_NS), CT(HPF_SLOPE, 0, 12.0, DB, CT_NS),
                                            CT(HPF_ON, 0, 0.0, NONE, CT_NS)};
    struct omx_curve_request rq = CURVE_GOLDEN_EQ;
    rq.terms = parked;
    rq.nterms = 7u;
    const uint32_t n = curve_run(&rq, x, y, &code);
    uint32_t flat = 0;
    for (uint32_t i = 0; i < n; i++) flat += y[i] == 0.0;
    ok(code == OMX_CURVE_OK && n == rq.points && flat == n, "a band and a head filter switched off are not drawn", flat, n);
    rq.nterms = 6u; /* the head filter's switch unbound: it runs */
    curve_run(&rq, x, y, &code);
    ok(code == OMX_CURVE_OK && y[0] < -20.0, "a head filter with no switch term runs", y[0], -20.0);
  }
  {
    struct omx_curve_request rq = CURVE_GOLDEN_EQ;
    rq.family = OMX_CURVE_FAMILIES_GATE;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_KIND_UNKNOWN, "an eq_response of a gate: kind unknown", code, 0);
    rq = CURVE_GOLDEN_COMP;
    rq.family = OMX_CURVE_FAMILIES_EQ;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_KIND_UNKNOWN, "a transfer_curve of an EQ: kind unknown", code, 0);
    rq.family = (enum omx_curve_families)OMX_CURVE_FAMILIES_COUNT;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_KIND_UNKNOWN, "a family past the set: kind unknown", code, 0);
    rq = CURVE_GOLDEN_COMP;
    rq.kind = (enum omx_curve_displays)OMX_CURVE_DISPLAYS_COUNT;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_KIND_UNKNOWN, "a kind past the set: kind unknown", code, 0);
  }
  const struct omx_curve_term unshaped[] = {CT(BAND_TYPE, 0, 5.0, NONE, OMX_CURVE_SHAPES_UNSHAPED),
                                            CT(BAND_FREQ, 0, 1000.0, HZ, CT_NS), CT(BAND_Q, 0, 1.0, NONE, CT_NS)};
  {
    /* a choice value with no shape (`-`) is off: not drawn, the curve flat */
    struct omx_curve_request ru = CURVE_GOLDEN_EQ;
    ru.terms = unshaped, ru.nterms = 3u;
    const uint32_t nu = omx_curve_points(&ru, x, y, OMX_CURVE_POINTS_MAX, &code);
    uint32_t flat = 0;
    for (uint32_t i = 0; i < nu; i++) flat += y[i] == 0.0;
    ok(code == OMX_CURVE_OK && nu > 0u && flat == nu, "an unshaped band is not drawn: the curve is flat", flat, nu);
  }
  const struct omx_curve_term slope18[] = {CT(HPF_FREQ, 0, 80.0, HZ, CT_NS), CT(HPF_SLOPE, 0, 18.0, DB, CT_NS)};
  ok(curve_code_of(&CURVE_GOLDEN_EQ, slope18, 2u) == OMX_CURVE_KIND_UNKNOWN, "an 18 dB/oct head filter: kind unknown", 0, 0);
  const struct omx_curve_term off[] = {CT(BAND_TYPE, 0, 0.0, NONE, OMX_CURVE_SHAPES_OFF), CT(BAND_FREQ, 0, 1000.0, HZ, CT_NS)};
  ok(curve_code_of(&CURVE_GOLDEN_EQ, off, 2u) == OMX_CURVE_OK, "an off band needs no Q and is not drawn", 0, 0);

  const struct omx_curve_term nofreq[] = {CT(BAND_GAIN, 0, 3.0, DB, CT_NS), CT(BAND_Q, 0, 1.0, NONE, CT_NS)};
  const struct omx_curve_term noshape[] = {CT(BAND_FREQ, 0, 1000.0, HZ, CT_NS), CT(BAND_GAIN, 0, 3.0, DB, CT_NS),
                                           CT(BAND_Q, 0, 1.0, NONE, CT_NS)};
  const struct omx_curve_term noq[] = {CT(BAND_FREQ, 0, 1000.0, HZ, OMX_CURVE_SHAPES_BELL), CT(BAND_GAIN, 0, 3.0, DB, CT_NS)};
  const struct omx_curve_term nogain[] = {CT(BAND_FREQ, 0, 1000.0, HZ, OMX_CURVE_SHAPES_LOWSHELF), CT(BAND_Q, 0, 1.0, NONE, CT_NS)};
  const struct omx_curve_term nopassslope[] = {CT(LPF_FREQ, 0, 9000.0, HZ, CT_NS)};
  const struct omx_curve_term hpnogain[] = {CT(BAND_FREQ, 0, 100.0, HZ, OMX_CURVE_SHAPES_HIGHPASS), CT(BAND_Q, 0, 0.7, NONE, CT_NS)};
  ok(curve_code_of(&CURVE_GOLDEN_EQ, nofreq, 2u) == OMX_CURVE_TERM_MISSING, "a band with no band_freq: term missing", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_EQ, noshape, 3u) == OMX_CURVE_TERM_MISSING, "a band with no shape: term missing", 0, 0);
  {
    /* no Q control: the declared default EQ_BAND_DEFAULTS q, clamped for the shape */
    const struct omx_curve_term withq[] = {CT(BAND_FREQ, 0, 1000.0, HZ, OMX_CURVE_SHAPES_BELL), CT(BAND_GAIN, 0, 3.0, DB, CT_NS),
                                           CT(BAND_Q, 0, (double)OMX_EQ_BAND_DEFAULTS_Q, NONE, CT_NS)};
    double ya[OMX_CURVE_POINTS_MAX], yb[OMX_CURVE_POINTS_MAX];
    struct omx_curve_request ra = CURVE_GOLDEN_EQ, rb = CURVE_GOLDEN_EQ;
    ra.terms = noq, ra.nterms = 2u, rb.terms = withq, rb.nterms = 3u;
    const uint32_t na = omx_curve_points(&ra, x, ya, OMX_CURVE_POINTS_MAX, &code);
    const uint32_t nb2 = omx_curve_points(&rb, x, yb, OMX_CURVE_POINTS_MAX, &code);
    ok(na > 0u && na == nb2 && memcmp(ya, yb, na * sizeof(double)) == 0,
       "a band with no Q draws at the declared default Q, bit for bit", na, nb2);
    /* the allpasses draw what they run: 0 dB */
    const struct omx_curve_term ap[] = {CT(BAND_FREQ, 0, 800.0, HZ, OMX_CURVE_SHAPES_ALLPASS1), CT(BAND_FREQ, 1, 3000.0, HZ, OMX_CURVE_SHAPES_ALLPASS2)};
    ra.terms = ap, ra.nterms = 2u;
    const uint32_t nap = omx_curve_points(&ra, x, ya, OMX_CURVE_POINTS_MAX, &code);
    double worst = 0.0;
    for (uint32_t i = 0; i < nap; i++) worst = fmax(worst, fabs(ya[i]));
    ok(code == OMX_CURVE_OK && nap > 0u && worst < 1e-9, "two allpass bands draw 0 dB", worst, 1e-9);
    /* a slope that is a bare choice value, with no enumeration resolving it: term missing */
    const struct omx_curve_term bare[] = {CT(HPF_FREQ, 0, 80.0, HZ, CT_NS), CT(HPF_SLOPE, 0, 1.0, NONE, CT_NS)};
    ok(curve_code_of(&CURVE_GOLDEN_EQ, bare, 2u) == OMX_CURVE_TERM_MISSING, "a slope from no enumeration: term missing", 0, 0);
    /* a limiter: threshold alone, ratio infinity, a hard corner */
    const struct omx_curve_term lim[] = {CT(THRESHOLD, 0, -6.0, DBFS, CT_NS)};
    struct omx_curve_request rl = CURVE_GOLDEN_COMP;
    rl.family = OMX_CURVE_FAMILIES_LIMITER, rl.terms = lim, rl.nterms = 1u;
    const uint32_t nl = omx_curve_points(&rl, x, ya, OMX_CURVE_POINTS_MAX, &code);
    uint32_t corner = 0;
    for (uint32_t i = 0; i < nl; i++) corner += ya[i] == (x[i] <= -6.0 ? x[i] : -6.0);
    ok(code == OMX_CURVE_OK && nl > 0u && corner == nl, "a limiter is a hard corner at its threshold", corner, nl);
    const struct omx_curve_term noratio_c[] = {CT(THRESHOLD, 0, -6.0, DB, CT_NS)};
    ok(curve_code_of(&CURVE_GOLDEN_COMP, noratio_c, 1u) == OMX_CURVE_TERM_MISSING, "a compressor still needs its ratio", 0, 0);
  }
  ok(curve_code_of(&CURVE_GOLDEN_EQ, nogain, 2u) == OMX_CURVE_TERM_MISSING, "a shelf with no gain: term missing", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_EQ, nopassslope, 1u) == OMX_CURVE_TERM_MISSING, "a head filter with no slope: term missing", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_EQ, hpnogain, 2u) == OMX_CURVE_OK, "a high-pass band needs no gain", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_EQ, NULL, 0u) == OMX_CURVE_TERM_MISSING, "an EQ with no term: term missing", 0, 0);
  const struct omx_curve_term noratio[] = {CT(THRESHOLD, 0, -20.0, DB, CT_NS)};
  const struct omx_curve_term nothr[] = {CT(RATIO, 0, 4.0, RATIO, CT_NS)};
  const struct omx_curve_term gate_norange[] = {CT(THRESHOLD, 0, -40.0, DB, CT_NS), CT(RATIO, 0, 4.0, RATIO, CT_NS)};
  const struct omx_curve_term gate_half[] = {CT(THRESHOLD, 0, -40.0, DB, CT_NS), CT(RATIO, 0, 4.0, RATIO, CT_NS),
                                             CT(RANGE, 0, -60.0, DB, CT_NS), CT(KNEE_START, 0, -45.0, DB, CT_NS)};
  ok(curve_code_of(&CURVE_GOLDEN_COMP, noratio, 1u) == OMX_CURVE_TERM_MISSING, "a compressor with no ratio: term missing", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_COMP, nothr, 1u) == OMX_CURVE_TERM_MISSING, "a compressor with no threshold: term missing", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_GATE, gate_norange, 2u) == OMX_CURVE_TERM_MISSING, "a gate with no range: term missing", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_GATE, gate_half, 4u) == OMX_CURVE_TERM_MISSING, "half a knee range: term missing", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_COMP, gate_norange, 2u) == OMX_CURVE_OK, "a compressor needs no range", 0, 0);

  const struct omx_curve_term freq_db[] = {CT(BAND_FREQ, 0, 1000.0, DB, OMX_CURVE_SHAPES_BELL)};
  const struct omx_curve_term thr_hz[] = {CT(THRESHOLD, 0, -20.0, HZ, CT_NS), CT(RATIO, 0, 4.0, RATIO, CT_NS)};
  const struct omx_curve_term makeup_coef[] = {CT(THRESHOLD, 0, -20.0, DB, CT_NS), CT(RATIO, 0, 4.0, RATIO, CT_NS),
                                               CT(MAKEUP, 0, 2.0, COEF, CT_NS)};
  ok(curve_code_of(&CURVE_GOLDEN_EQ, freq_db, 1u) == OMX_CURVE_UNIT_REFUSED, "a frequency in dB: unit refused", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_COMP, thr_hz, 2u) == OMX_CURVE_UNIT_REFUSED, "a threshold in Hz: unit refused", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_COMP, makeup_coef, 3u) == OMX_CURVE_UNIT_REFUSED, "a make-up as a factor: unit refused", 0, 0);

  const struct omx_curve_term nan_gain[] = {CT(BAND_FREQ, 0, 1000.0, HZ, OMX_CURVE_SHAPES_BELL), CT(BAND_GAIN, 0, NAN, DB, CT_NS),
                                            CT(BAND_Q, 0, 1.0, NONE, CT_NS)};
  const struct omx_curve_term inf_thr[] = {CT(THRESHOLD, 0, -INFINITY, DB, CT_NS), CT(RATIO, 0, 4.0, RATIO, CT_NS)};
  ok(curve_code_of(&CURVE_GOLDEN_EQ, nan_gain, 3u) == OMX_CURVE_VALUE_NOT_FINITE, "a NaN gain: not finite", 0, 0);
  ok(curve_code_of(&CURVE_GOLDEN_COMP, inf_thr, 2u) == OMX_CURVE_VALUE_NOT_FINITE, "a -inf threshold: not finite", 0, 0);
  {
    struct omx_curve_request rq = CURVE_GOLDEN_COMP;
    rq.sample_rate = 0.0;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_VALUE_NOT_FINITE, "a zero rate: not finite", code, 0);
    rq = CURVE_GOLDEN_COMP;
    rq.has_extents = 1;
    rq.x_min = -80.0;
    rq.x_max = INFINITY;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_VALUE_NOT_FINITE, "an infinite extent: not finite", code, 0);
  }

  {
    struct omx_curve_request rq = CURVE_GOLDEN_COMP;
    rq.points = 1u;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_POINTS_RANGE, "one point: points range", code, 0);
    rq.points = OMX_CURVE_POINTS_MAX + 1u;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_POINTS_RANGE, "CURVE_POINTS_MAX + 1: points range", code, 0);
    rq.points = OMX_CURVE_POINTS_MAX;
    ok(curve_run(&rq, x, y, &code) == OMX_CURVE_POINTS_MAX && code == OMX_CURVE_OK, "CURVE_POINTS_MAX points draw", code, 0);
    rq = CURVE_GOLDEN_COMP;
    rq.has_extents = 1;
    rq.x_min = -10.0;
    rq.x_max = -10.0;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_POINTS_RANGE, "extents that bound nothing: points range", code, 0);
    rq = CURVE_GOLDEN_EQ;
    rq.has_extents = 1;
    rq.x_min = 0.0;
    rq.x_max = 20000.0;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_POINTS_RANGE, "an EQ from 0 Hz: points range", code, 0);
    /* The golden EQ draws 7 sections: 256 - 3 x 7 grid points fit, one more does not. */
    rq = CURVE_GOLDEN_EQ;
    rq.points = OMX_CURVE_POINTS_MAX - 21u;
    ok(curve_run(&rq, x, y, &code) > 0u && code == OMX_CURVE_OK, "256 - 3 x 7 grid points fit seven sections", code, 0);
    rq.points++;
    ok(curve_run(&rq, x, y, &code) == 0u && code == OMX_CURVE_POINTS_RANGE, "one grid point more: points range", code, 0);
    const struct omx_curve_term far[] = {CT(BAND_FREQ, OMX_CURVE_BANDS_MAX, 1000.0, HZ, OMX_CURVE_SHAPES_BELL)};
    ok(curve_code_of(&CURVE_GOLDEN_EQ, far, 1u) == OMX_CURVE_POINTS_RANGE, "a band index past what fits: points range", 0, 0);
  }
  {
    int c = -1;
    ok(omx_curve_points(&CURVE_GOLDEN_COMP, x, y, 40u, &c) == 0u && c == OMX_CURVE_ROOM, "40 places for 41 points: room", c, 0);
    ok(omx_curve_points(&CURVE_GOLDEN_COMP, x, y, 41u, &c) == 41u && c == OMX_CURVE_OK, "41 places for 41 points draw", c, 0);
    ok(omx_curve_points(&CURVE_GOLDEN_EQ, x, y, 32u + 3u * 7u - 1u, &c) == 0u && c == OMX_CURVE_ROOM,
       "an EQ's room counts its anchors", c, 0);
  }
  /* A value outside the gain computer's domain is held at its edge, as the kernel's clamp holds it. */
  {
    const struct omx_curve_term wild[] = {CT(THRESHOLD, 0, -40.0, DB, CT_NS), CT(RATIO, 0, 0.5, RATIO, CT_NS),
                                          CT(RANGE, 0, 12.0, DB, CT_NS), CT(KNEE, 0, -3.0, DB, CT_NS)};
    ok(curve_code_of(&CURVE_GOLDEN_GATE, wild, 4u) == OMX_CURVE_OK, "a ratio under 1, a positive range, a negative knee draw", 0, 0);
  }
  expect_clean();
}
