// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omxdsp_suite.c — the library suite: every primitive's oracle at every declared rate, with
 * contracts ON, ending on an EMPTY ledger (docs/design/specs/2026-09-26-dsp-primitives.md §8b).
 * Pure C, -lm, no other package.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omxdsp.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the harness -------------------------------------------------------------------------- */

static int g_checks = 0, g_failed = 0;
static const char *g_arm = "";

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s — measured %.9g, limit %.9g\n", g_arm, what, measured, limit);
  }
}

/* Every violation the ledger holds, printed, then the ledger is expected empty for this arm. */
static void expect_clean(void) {
  const uint32_t seen = omx_contract_log.count;
  const uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (!omx_contract_record_ready(r)) continue;
    printf("VIOLATION [%s] %s %s (frame %u)\n", r->stage, r->kind, r->token, r->frame);
  }
  ok(seen == 0u, "no contract violation in this arm", (double)seen, 0.0);
  omx_contract_reset();
}

static uint32_t g_seed = 0x2f6e2b1du;
static float rnd(float lo, float hi) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return lo + (hi - lo) * ((float)(g_seed >> 8) / 16777216.0f);
}

/* ---- the contract mechanism ----------------------------------------------------------------- */

#define OMX_CONTRACT_STAGE "suite/contract"
static void arm_ledger(void) {
  g_arm = "ledger";
  omx_contract_reset();
  const uint32_t before = omx_contract_log.checks;
  OMX_PRE(1, "holds");
  ok(omx_contract_log.checks == before + 1u, "an evaluated contract counts once", (double)omx_contract_log.checks, before + 1.0);
  ok(omx_contract_log.count == 0u, "a held contract records nothing", (double)omx_contract_log.count, 0.0);
  OMX_POST_AT(0, "broken", 17u);
  ok(omx_contract_log.count == 1u, "a broken contract counts once", (double)omx_contract_log.count, 1.0);
  ok(omx_contract_record_ready(&omx_contract_log.rec[0]), "the record is published", 0.0, 1.0);
  ok(strcmp(omx_contract_log.rec[0].kind, "post") == 0, "the record carries its kind", 0.0, 0.0);
  ok(strcmp(omx_contract_log.rec[0].token, "broken") == 0, "the record carries its token", 0.0, 0.0);
  ok(strcmp(omx_contract_log.rec[0].stage, OMX_CONTRACT_STAGE) == 0, "the record carries its stage", 0.0, 0.0);
  ok(omx_contract_log.rec[0].frame == 17u, "the record carries its frame", (double)omx_contract_log.rec[0].frame, 17.0);
  for (uint32_t i = 0; i < OMX_CONTRACT_MAX + 10u; i++) OMX_INVARIANT(0, "overflow");
  ok(omx_contract_log.count == OMX_CONTRACT_MAX + 11u, "the count stays exact past the kept records",
     (double)omx_contract_log.count, (double)OMX_CONTRACT_MAX + 11.0);
  ok(omx_contract_record_ready(&omx_contract_log.rec[OMX_CONTRACT_MAX - 1u]), "the last kept slot is published", 0.0, 1.0);
  omx_contract_reset();
  ok(omx_contract_log.count == 0u, "reset forgets the violations", (double)omx_contract_log.count, 0.0);
  ok(!omx_contract_record_ready(&omx_contract_log.rec[0]), "reset unpublishes the records", 0.0, 0.0);
  ok(omx_contract_log.checks > before, "reset keeps the evaluated count", (double)omx_contract_log.checks, (double)before);
  expect_clean();
}
#undef OMX_CONTRACT_STAGE

static void arm_rates(void) {
  g_arm = "rates";
  ok(OMX_DECLARED_RATE_COUNT >= 4u, "the declaration carries at least the four basic rates", (double)OMX_DECLARED_RATE_COUNT, 4.0);
  for (uint32_t i = 0; i < OMX_DECLARED_RATE_COUNT; i++) {
    ok(omx_rate_is_declared(OMX_DECLARED_RATES[i]), "a declared rate is declared", OMX_DECLARED_RATES[i], 1.0);
    ok(OMX_RATE_IS_DECLARED(OMX_DECLARED_RATES[i]), "the macro forwards to the table", OMX_DECLARED_RATES[i], 1.0);
  }
  const float not_declared[] = {0.0f, -48000.0f, 22050.0f, 96001.0f, 384000.0f, NAN};
  for (size_t i = 0; i < sizeof not_declared / sizeof not_declared[0]; i++)
    ok(!omx_rate_is_declared(not_declared[i]), "an undeclared rate is refused", not_declared[i], 0.0);
  expect_clean();
}

static void arm_block_helpers(void) {
  g_arm = "block-helpers";
  float b[8];
  for (int i = 0; i < 8; i++) b[i] = rnd(-1.0f, 1.0f);
  ok(omx_block_finite(b, 8u), "a finite block is finite", 0.0, 1.0);
  ok(omx_block_finite(NULL, 8u), "a NULL block promises nothing and passes", 0.0, 1.0);
  b[3] = NAN;
  ok(!omx_block_finite(b, 8u), "a NaN is seen", 0.0, 0.0);
  b[3] = INFINITY;
  ok(!omx_block_finite(b, 8u), "an infinity is seen", 0.0, 0.0);
  b[3] = -INFINITY;
  ok(!omx_block_finite(b, 8u), "a negative infinity is seen", 0.0, 0.0);
  b[3] = 0.25f;
  ok(omx_lane_finite(b, NULL, 8u), "a mono lane is finite on its one leg", 0.0, 1.0);
  float r[8] = {0};
  r[7] = NAN;
  ok(!omx_lane_finite(b, r, 8u), "a lane's R leg is checked", 0.0, 0.0);
  float m[4] = {0.5f, -0.75f, 0.25f, 0.0f};
  ok(omx_block_absmax(m, 4u) == 0.75f, "absmax is the largest magnitude", (double)omx_block_absmax(m, 4u), 0.75);
  ok(omx_block_absmax(NULL, 4u) == 0.0f, "absmax of NULL is 0", 0.0, 0.0);
  expect_clean();
}

static void arm_version(void) {
  g_arm = "version";
  ok(omxdsp_version() == ((uint32_t)OMXDSP_VERSION_MAJOR << 16 | (uint32_t)OMXDSP_VERSION_MINOR << 8 | (uint32_t)OMXDSP_VERSION_PATCH),
     "the version packs major, minor and patch", (double)omxdsp_version(), 0.0);
  expect_clean();
}

/* ---- denormal ------------------------------------------------------------------------------- */

static void arm_denormal(void) {
  g_arm = "denormal";
  ok(omx_flush(1e-30f) == 0.0f, "a value below the floor flushes to zero", 1e-30, 0.0);
  ok(omx_flush(-1e-30f) == 0.0f, "a negative value below the floor flushes to zero", -1e-30, 0.0);
  ok(omx_flush(1e-19f) == 1e-19f, "a value above the floor is untouched", 1e-19, 1e-19);
  ok(omx_flush(-0.5f) == -0.5f, "an ordinary value is untouched", -0.5, -0.5);
  omx_denormals_off();
#if defined(__x86_64__) || defined(__i386__)
  ok((_mm_getcsr() & 0x8040u) == 0x8040u, "FTZ and DAZ are set for this thread", (double)(_mm_getcsr() & 0x8040u), 0x8040);
#endif
  expect_clean();
}

/* ---- units ---------------------------------------------------------------------------------- */

static void arm_units(void) {
  g_arm = "units";
  ok(omx_db_to_lin(0.0f) == 1.0f, "0 dB is unity", omx_db_to_lin(0.0f), 1.0);
  ok(fabsf(omx_db_to_lin(-6.0f) - 0.501187f) < 1e-5f, "-6 dB is 0.501187", omx_db_to_lin(-6.0f), 0.501187);
  ok(fabsf(omx_db_to_lin(20.0f) - 10.0f) < 1e-5f, "+20 dB is 10", omx_db_to_lin(20.0f), 10.0);
  for (float db = -120.0f; db <= 24.0f; db += 3.7f) {
    const float back = omx_lin_to_db(omx_db_to_lin(db));
    ok(fabsf(back - db) < 1e-3f, "dB -> lin -> dB round trip", back, db);
  }
  ok(omx_lin_to_db(0.0f) == -180.0f, "silence is a finite -180 dB", omx_lin_to_db(0.0f), -180.0);
  ok(omx_lin_to_db(1e-12f) == -180.0f, "below the floor reads as the floor", omx_lin_to_db(1e-12f), -180.0);
  /* The per-sample pair against the libm pair, dense over the declared travel (row 17's oracle).
   * The worst error is PRINTED, so the tolerance the spec declares is read from a run. */
  double worst_db = 0.0, worst_rel = 0.0;
  for (double lg = -9.0; lg <= 4.0; lg += 1.0 / 4096.0) {
    const float x = (float)pow(10.0, lg);
    const double d = fabs((double)omx_lin_to_db_poly(x) - (double)omx_lin_to_db(x));
    if (d > worst_db) worst_db = d;
  }
  for (double db = -300.0; db <= 80.0; db += 1.0 / 512.0) {
    const double ref = (double)omx_db_to_lin((float)db);
    const double rel = fabs((double)omx_db_to_lin_poly((float)db) - ref) / ref;
    if (rel > worst_rel) worst_rel = rel;
  }
  printf("  units: lin_to_db_poly worst %.3g dB over [1e-9, 1e4]; db_to_lin_poly worst %.3g relative over [-300, +80] dB\n",
         worst_db, worst_rel);
  ok(worst_db <= 1e-4, "lin_to_db_poly within 1e-4 dB of lin_to_db", worst_db, 1e-4);
  ok(worst_rel <= 2e-6, "db_to_lin_poly within 2e-6 relative of db_to_lin", worst_rel, 2e-6);
  ok(omx_db_to_lin_poly(0.0f) == 1.0f, "db_to_lin_poly: 0 dB is EXACTLY 1", omx_db_to_lin_poly(0.0f), 1.0);
  ok(omx_lin_to_db_poly(1.0f) == 0.0f, "lin_to_db_poly: unity is EXACTLY 0 dB", omx_lin_to_db_poly(1.0f), 0.0);
  ok(omx_lin_to_db_poly(0.0f) == omx_lin_to_db_poly(1e-9f), "lin_to_db_poly: silence reads as the floor",
     omx_lin_to_db_poly(0.0f), omx_lin_to_db_poly(1e-9f));
  ok(omx_db_to_lin_poly(-1e4f) > 0.0f && omx_db_to_lin_poly(-1e4f) - omx_db_to_lin_poly(-1e4f) == 0.0f,
     "db_to_lin_poly: far below the travel is a finite positive", omx_db_to_lin_poly(-1e4f), 0.0);
  expect_clean();
}

/* ---- one-pole ------------------------------------------------------------------------------- */

static void arm_onepole(void) {
  g_arm = "onepole";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    /* time constant: the step response covers 1 - 1/e after tau */
    const float tau_ms = 5.0f;
    const float p = omx_pole_from_time_ms(tau_ms, sr);
    ok(p > 0.0f && p < 1.0f, "a positive time gives a pole inside (0, 1)", p, 1.0);
    float y = 0.0f;
    const uint32_t n_tau = (uint32_t)(tau_ms * 0.001f * sr + 0.5f);
    for (uint32_t i = 0; i < n_tau; i++) omx_onepole(&y, 1.0f, p);
    ok(fabsf(y - (1.0f - expf(-1.0f))) < 2e-3f, "the step response covers 1-1/e after one time constant", y, 1.0 - exp(-1.0));
    /* corner: |H| at fc for the impulse-invariant pole is (1-p)/sqrt(1 - 2p cos w + p^2) */
    const float fc = 1000.0f;
    const float pc = omx_pole_from_cutoff_hz(fc, sr);
    const double w = 2.0 * M_PI * fc / sr;
    const double closed = (1.0 - pc) / sqrt(1.0 - 2.0 * pc * cos(w) + (double)pc * pc);
    float st = 0.0f;
    double re = 0.0, im = 0.0;
    const uint32_t settle = (uint32_t)(sr * 0.2f), meas = (uint32_t)(sr * 0.1f);
    for (uint32_t i = 0; i < settle + meas; i++) {
      const float x = sinf((float)(w * i));
      const float o = omx_onepole(&st, x, pc);
      if (i >= settle) { re += o * cos(w * i); im += o * sin(w * i); }
    }
    const double mag = 2.0 * sqrt(re * re + im * im) / meas;
    ok(fabs(mag - closed) < 2e-3, "the corner's magnitude matches the closed form", mag, closed);
    ok(fabs(20.0 * log10(closed) + 3.0) < 0.2, "the corner sits near -3 dB", 20.0 * log10(closed), -3.0);
    /* wires and exactness */
    ok(omx_pole_from_time_ms(0.0f, sr) == 0.0f, "ms <= 0 is a wire", omx_pole_from_time_ms(0.0f, sr), 0.0);
    ok(omx_pole_from_cutoff_hz(-1.0f, sr) == 0.0f, "hz <= 0 is a wire", omx_pole_from_cutoff_hz(-1.0f, sr), 0.0);
    float wire = 0.25f;
    ok(omx_onepole(&wire, 0.75f, 0.0f) == 0.75f, "pole 0 passes the input through", wire, 0.75);
    float held = 0.3125f;
    omx_onepole_toward(&held, 0.3125f, p);
    ok(held == 0.3125f, "the increment form holds a state equal to its target bit for bit", held, 0.3125);
    float conv = 0.0f, incr = 0.0f;
    for (int i = 0; i < 100; i++) { omx_onepole(&conv, 0.8f, p); omx_onepole_toward(&incr, 0.8f, p); }
    ok(fabsf(conv - incr) < 1e-5f, "both forms are the same filter", conv, incr);
  }
  expect_clean();
}

/* ---- biquad --------------------------------------------------------------------------------- */

static void arm_biquad(void) {
  g_arm = "biquad";
  const float unity[5] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  float s[4] = {0};
  float in[64], out[64];
  for (int i = 0; i < 64; i++) { in[i] = rnd(-1.0f, 1.0f); out[i] = omx_biquad(in[i], unity, s); }
  ok(memcmp(in, out, sizeof in) == 0, "unity coefficients are the identity bit for bit", 0.0, 0.0);
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    /* a cookbook low-pass at 1 kHz, Q 1/sqrt2, designed here in double; the section's impulse
     * response against the same recursion run in double */
    const double w0 = 2.0 * M_PI * 1000.0 / sr, alpha = sin(w0) / (2.0 * M_SQRT1_2), cw = cos(w0), a0 = 1.0 + alpha;
    const float c[5] = {(float)((1.0 - cw) / 2.0 / a0), (float)((1.0 - cw) / a0), (float)((1.0 - cw) / 2.0 / a0),
                        (float)(-2.0 * cw / a0), (float)((1.0 - alpha) / a0)};
    float st[4] = {0};
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0, worst = 0.0;
    for (int i = 0; i < 2000; i++) {
      const float x = i == 0 ? 1.0f : 0.0f;
      const float y = omx_biquad(x, c, st);
      const double yd = c[0] * x + c[1] * x1 + c[2] * x2 - c[3] * y1 - c[4] * y2;
      x2 = x1; x1 = x; y2 = y1; y1 = yd;
      if (fabs(yd - y) > worst) worst = fabs(yd - y);
    }
    ok(worst < 1e-5, "the section's impulse response follows its recursion", worst, 1e-5);
    ok(omx_block_finite(st, 4u), "the state stays finite", 0.0, 1.0);
    /* the cascade equals the sections run in series, and a parked section leaves its state alone */
    float coeffs[3][5]; memcpy(coeffs[0], c, sizeof c); memcpy(coeffs[1], c, sizeof c); memcpy(coeffs[2], c, sizeof c);
    float sa[3][4] = {{0}}, sb1[4] = {0}, sb2[4] = {0};
    const uint8_t on[3] = {1, 0, 1};
    float a[128], b[128];
    for (int i = 0; i < 128; i++) { a[i] = rnd(-0.7f, 0.7f); b[i] = a[i]; }
    omx_biquad_cascade(a, 128u, 3u, coeffs, on, sa);
    for (int i = 0; i < 128; i++) b[i] = omx_biquad(omx_biquad(b[i], c, sb1), c, sb2);
    ok(memcmp(a, b, sizeof a) == 0, "the cascade is its sections in series, bit for bit", 0.0, 0.0);
    ok(sa[1][0] == 0.0f && sa[1][2] == 0.0f, "a parked section's state does not advance", sa[1][0], 0.0);
    ok(memcmp(sa[0], sb1, sizeof sb1) == 0 && memcmp(sa[2], sb2, sizeof sb2) == 0, "each section keeps its own slot", 0.0, 0.0);
    /* a 0 dB cascade is the identity over a block */
    float d[64]; for (int i = 0; i < 64; i++) d[i] = rnd(-1.0f, 1.0f);
    float e[64]; memcpy(e, d, sizeof d);
    float un[2][5] = {{1, 0, 0, 0, 0}, {1, 0, 0, 0, 0}}; float su[2][4] = {{0}};
    omx_biquad_cascade(e, 64u, 2u, un, NULL, su);
    ok(memcmp(d, e, sizeof d) == 0, "a unity cascade is the identity", 0.0, 0.0);
  }
  {
    /* THE COST DOOR'S ORACLE (spec §1 row 2): omx_biquad_cascade_stereo — parked bands dropped
     * once, two sections and both legs per pass, state in locals — is the band-outer loop of
     * omx_biquad over the live bands, BIT FOR BIT, output and state, for every band count to the
     * cap, odd and even live counts, one and two legs, and block lengths 1, 7, 64 and 513. */
    static float cf[OMX_EQ_MAX_BANDS][5];
    for (int b = 0; b < OMX_EQ_MAX_BANDS; b++)
      omx_eq_design_f((enum omx_eq_kind)(b % 6), 40.0 * pow(400.0, b / 23.0), 0.5 + 0.1 * b,
                      (b & 1) ? 6.0 : -9.0, 96000.0, cf[b]);
    static const uint32_t ns[4] = {1u, 7u, 64u, 513u};
    int bad = 0, cases = 0;
    for (uint32_t nb = 0; nb <= OMX_EQ_MAX_BANDS; nb++)
      for (int mask = 0; mask < 4; mask++)
        for (int ni = 0; ni < 4; ni++)
          for (int legs = 1; legs <= 2; legs++) {
            const uint32_t n = ns[ni];
            uint8_t en[OMX_EQ_MAX_BANDS];
            for (uint32_t b = 0; b < OMX_EQ_MAX_BANDS; b++)
              en[b] = mask == 0 ? 1 : mask == 1 ? (b % 3 != 1) : mask == 2 ? (b & 1) : (b * 7 % 5 < 2);
            static float l0[513], r0[513], l1[513], r1[513];
            float sl0[OMX_EQ_MAX_BANDS][4], sr0[OMX_EQ_MAX_BANDS][4], sl1[OMX_EQ_MAX_BANDS][4], sr1[OMX_EQ_MAX_BANDS][4];
            for (uint32_t b = 0; b < OMX_EQ_MAX_BANDS; b++)
              for (int j = 0; j < 4; j++) sl0[b][j] = sl1[b][j] = rnd(-0.3f, 0.3f), sr0[b][j] = sr1[b][j] = rnd(-0.3f, 0.3f);
            for (uint32_t i = 0; i < n; i++) l0[i] = l1[i] = rnd(-1.0f, 1.0f), r0[i] = r1[i] = rnd(-1.0f, 1.0f);
            for (uint32_t b = 0; b < nb; b++) {
              if (mask && !en[b]) continue;
              for (uint32_t i = 0; i < n; i++) l0[i] = omx_biquad(l0[i], cf[b], sl0[b]);
              if (legs == 2) for (uint32_t i = 0; i < n; i++) r0[i] = omx_biquad(r0[i], cf[b], sr0[b]);
            }
            if (legs == 2) omx_biquad_cascade_stereo(l1, r1, n, nb, cf, mask ? en : NULL, sl1, sr1);
            else omx_biquad_cascade(l1, n, nb, cf, mask ? en : NULL, sl1);
            cases++;
            if (memcmp(l0, l1, n * sizeof(float)) || memcmp(sl0, sl1, sizeof sl0) ||
                (legs == 2 && (memcmp(r0, r1, n * sizeof(float)) || memcmp(sr0, sr1, sizeof sr0))))
              bad++;
          }
    ok(bad == 0 && cases == 25 * 4 * 4 * 2,
       "the fused two-leg cascade is the band-outer omx_biquad loop, bit for bit (output and state)", bad, 0.0);
  }
  ok(OMX_EQ_MAX_BANDS == 24, "the cascade cap is the declared joint budget", OMX_EQ_MAX_BANDS, 24.0);
  expect_clean();
}

/* ---- EQ design and the matched pair ---------------------------------------------------------- */

/* The steady-state magnitude of a float section at `hz`, measured by running a sine through it. */
static double section_mag_at(const float c[5], double hz, double sr) {
  float st[4] = {0};
  const double w = 2.0 * M_PI * hz / sr;
  const uint32_t settle = (uint32_t)(sr * 0.25), meas = (uint32_t)(sr * 0.25);
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < settle + meas; i++) {
    const float y = omx_biquad(sinf((float)(w * i)), c, st);
    if (i >= settle) { re += y * cos(w * i); im += y * sin(w * i); }
  }
  return 20.0 * log10(2.0 * sqrt(re * re + im * im) / meas);
}

static void arm_eq_design(void) {
  g_arm = "eq-design";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    float c[5];
    omx_eq_design_f(OMX_EQ_PEAKING, 1000.0, 1.0, 6.0, sr, c);
    ok(fabs(section_mag_at(c, 1000.0, sr) - 6.0) < 0.05, "a +6 dB bell reads +6 dB at its centre", section_mag_at(c, 1000.0, sr), 6.0);
    ok(fabs(section_mag_at(c, 40.0, sr)) < 0.1, "the bell is unity far below its centre", section_mag_at(c, 40.0, sr), 0.0);
    omx_eq_design_f(OMX_EQ_HIGHPASS, 200.0, M_SQRT1_2, 0.0, sr, c);
    ok(fabs(section_mag_at(c, 200.0, sr) + 3.01) < 0.1, "the Butterworth high-pass sits at -3 dB on its corner", section_mag_at(c, 200.0, sr), -3.01);
    ok(section_mag_at(c, 20.0, sr) < -35.0, "the high-pass rejects two decades down", section_mag_at(c, 20.0, sr), -35.0);
    omx_eq_design_f(OMX_EQ_LOWPASS, 2000.0, M_SQRT1_2, 0.0, sr, c);
    ok(fabs(section_mag_at(c, 2000.0, sr) + 3.01) < 0.15, "the fitted low-pass sits at -3 dB on its corner", section_mag_at(c, 2000.0, sr), -3.01);
    ok(fabs(section_mag_at(c, 50.0, sr)) < 0.05, "the low-pass is unity at DC", section_mag_at(c, 50.0, sr), 0.0);
    omx_eq_design_f(OMX_EQ_NOTCH, 1000.0, 4.0, 0.0, sr, c);
    ok(section_mag_at(c, 1000.0, sr) < -40.0, "the notch nulls its centre", section_mag_at(c, 1000.0, sr), -40.0);
    ok(fabs(section_mag_at(c, 100.0, sr)) < 0.05, "the notch is unity a decade below", section_mag_at(c, 100.0, sr), 0.0);
    omx_eq_design_f(OMX_EQ_LOWSHELF, 300.0, M_SQRT1_2, -9.0, sr, c);
    ok(fabs(section_mag_at(c, 20.0, sr) + 9.0) < 0.1, "the low shelf carries its gain at DC", section_mag_at(c, 20.0, sr), -9.0);
    omx_eq_design_f(OMX_EQ_HIGHSHELF, 3000.0, M_SQRT1_2, 4.0, sr, c);
    ok(fabs(section_mag_at(c, 30.0, sr)) < 0.05, "the high shelf is unity at DC", section_mag_at(c, 30.0, sr), 0.0);
    ok(fabs(section_mag_at(c, sr * 0.45, sr) - 4.0) < 0.15, "the high shelf carries its gain near Nyquist", section_mag_at(c, sr * 0.45, sr), 4.0);
    /* the double design and its float narrowing agree; every design keeps its poles inside */
    double d[5];
    omx_eq_design(OMX_EQ_PEAKING, 1000.0, 1.0, 6.0, sr, d);
    omx_eq_design_f(OMX_EQ_PEAKING, 1000.0, 1.0, 6.0, sr, c);
    for (int i = 0; i < 5; i++) ok(c[i] == (float)d[i], "the float design is the narrowed double design", c[i], d[i]);
    for (int k = 0; k <= 5; k++) {
      omx_eq_design((enum omx_eq_kind)k, 15000.0, 8.0, 12.0, sr, d);
      ok(fabs(d[3]) < 1.0 + d[4] && d[4] < 1.0, "an extreme design stays inside the unit circle", d[4], 1.0);
    }
  }
  double qs[2];
  ok(omx_eq_butterworth_qs(0, qs) == 1u && fabs(qs[0] - M_SQRT1_2) < 1e-12, "12 dB/oct is one section at 1/sqrt2", qs[0], M_SQRT1_2);
  ok(omx_eq_butterworth_qs(1, qs) == 2u && fabs(qs[0] * qs[1] - M_SQRT1_2) < 1e-6, "24 dB/oct's two Qs multiply to 1/sqrt2", qs[0] * qs[1], M_SQRT1_2);
  expect_clean();
}

static void arm_matched_pair(void) {
  g_arm = "matched-pair";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    const double w0 = omx_matched_w0(1000.0, sr);
    const OmxMatchedPair pc = omx_matched_pair(w0, 0.3);
    ok(pc.p2 >= 0.0 && pc.p2 < 1.0 && pc.dc > 0.0, "a complex pair sits inside the unit circle", pc.p2, 1.0);
    ok(fabs((1.0 + pc.p1 + pc.p2) - pc.dc) < 1e-9, "the factored dc equals the summed polynomial at z = 1", pc.dc, 1.0 + pc.p1 + pc.p2);
    const OmxMatchedPair pr = omx_matched_pair(w0, 2.5);
    ok(pr.p2 >= 0.0 && pr.p2 < 1.0 && pr.dc > 0.0, "a real-root pair sits inside the unit circle", pr.p2, 1.0);
    ok(fabs((1.0 + pr.p1 + pr.p2) - pr.dc) < 1e-9, "the real-root dc equals the polynomial at z = 1", pr.dc, 1.0 + pr.p1 + pr.p2);
    double e[3];
    omx_eq_matched_pair(w0, 0.3, e);
    ok(e[0] == pc.p1 && e[1] == pc.p2 && e[2] == pc.dc, "both spellings of the pair agree bit for bit", e[2], pc.dc);
    ok(omx_matched_f0(0.0, sr) == 1.0, "the corner clamp floors at 1 Hz", omx_matched_f0(0.0, sr), 1.0);
    ok(omx_matched_f0(sr, sr) == sr * 0.5 * 0.999, "the corner clamp stops under Nyquist", omx_matched_f0(sr, sr), sr * 0.4995);
    float sec[5];
    omx_matched_pair_section(sec, w0, 0.5, w0, 2.0);
    ok(fabs(section_mag_at(sec, 20.0, sr)) < 0.05, "a pair-over-pair section is unity at DC", section_mag_at(sec, 20.0, sr), 0.0);
  }
  expect_clean();
}

/* ---- the LFO -------------------------------------------------------------------------------- */

static void arm_lfo(void) {
  g_arm = "lfo";
  double worst = 0.0;
  for (int i = 0; i < 1000; i++) {
    const float u = (float)i / 1000.0f;
    const double err = fabs((double)omx_lfo_shape(u) + sin(2.0 * M_PI * u));
    if (err > worst) worst = err;
  }
  ok(worst < 0.0562, "the parabola is -sin(2 pi u) to within 0.0561", worst, 0.0561);
  ok(omx_lfo_shape(0.0f) == 0.0f && omx_lfo_shape(0.5f) == 0.0f, "the shape crosses zero at 0 and half a turn", omx_lfo_shape(0.5f), 0.0);
  ok(omx_lfo_shape(0.25f) == -1.0f && omx_lfo_shape(0.75f) == 1.0f, "the shape peaks at the quarter turns", omx_lfo_shape(0.75f), 1.0);
  ok(omx_lfo_wrap(1.25f) == 0.25f && omx_lfo_wrap(0.9f) == 0.9f, "wrap folds one turn", omx_lfo_wrap(1.25f), 0.25);
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    struct omx_lfo l = {0.0f, omx_lfo_inc(2.0f, sr), 0.0f};
    ok(fabsf(l.inc * sr - 2.0f) < 1e-4f, "the increment is the rate in turns per sample", l.inc * sr, 2.0);
    ok(omx_lfo_inc(0.0f, sr) == 0.0f && omx_lfo_inc(-1.0f, sr) == 0.0f, "a non-positive rate freezes", omx_lfo_inc(-1.0f, sr), 0.0);
    ok(omx_lfo_inc(sr * 0.5f, sr) == 0.0f && omx_lfo_inc(sr, sr) == 0.0f, "a rate at or above Nyquist freezes", omx_lfo_inc(sr, sr), 0.0);
    /* one second of advances lands two turns later, and the phase never leaves [0, 1) */
    int inside = 1;
    for (uint32_t i = 0; i < (uint32_t)sr; i++) { omx_lfo_advance(&l); if (!(l.phase >= 0.0f && l.phase < 1.0f)) inside = 0; }
    ok(inside, "the phase stays inside one turn across a second", l.phase, 1.0);
    ok(fabsf(l.phase) < 1e-2f || fabsf(l.phase - 1.0f) < 1e-2f, "two whole turns return to phase zero within float accumulation", l.phase, 0.0);
    /* N reads of one oscillator at offsets k/N are the shape at those phases */
    l.phase = 0.3f;
    for (int k = 0; k < 4; k++) {
      const float off = (float)k / 4.0f;
      ok(omx_lfo_at(&l, off) == omx_lfo_shape(omx_lfo_wrap(0.3f + off)), "a read at an offset is the shape at that phase", off, 0.0);
    }
    ok(l.phase == 0.3f, "a read does not move the oscillator", l.phase, 0.3);
    ok(omx_lfo_sweep(10.0f, 4.0f, -1.0f) == 10.0f && omx_lfo_sweep(10.0f, 4.0f, 1.0f) == 14.0f, "the sweep runs from base to base + depth", omx_lfo_sweep(10.0f, 4.0f, 1.0f), 14.0);
  }
  /* THE PERIOD IS THE RATE's (dsp-primitives §1 row 12): 1/rate seconds of advances land back
   * on the phase they started from, at the slow end of every consumer's travel, every rate. A bare
   * float add rounds each step to the phase's ulp and missed by up to 0.024 of a turn here. */
  static const float SLOW[4] = {0.05f, 0.1f, 1.0f, 20.0f};
  double worst_turns = 0.0;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++)
    for (int k = 0; k < 4; k++) {
      const float sr = OMX_DECLARED_RATES[ri];
      struct omx_lfo l = {0.0f, omx_lfo_inc(SLOW[k], sr), 0.0f};
      const uint64_t turn = (uint64_t)llround((double)sr / (double)SLOW[k]);
      for (uint64_t i = 0; i < turn; i++) omx_lfo_advance(&l);
      const double off = l.phase > 0.5f ? 1.0 - (double)l.phase : (double)l.phase;
      if (off > worst_turns) worst_turns = off;
    }
  ok(worst_turns < 1e-5, "one period of advances returns to the start, every rate (turns)", worst_turns, 1e-5);
  struct omx_lfo frozen = {0.25f, 0.0f, 0.0f};
  for (int i = 0; i < 1000; i++) omx_lfo_advance(&frozen);
  ok(frozen.phase == 0.25f, "a frozen oscillator does not move", frozen.phase, 0.25);
  ok(omx_lfo_state_size() == sizeof(struct omx_lfo) && omx_lfo_state_align() == _Alignof(struct omx_lfo), "the state layout is exported", (double)omx_lfo_state_size(), sizeof(struct omx_lfo));
  expect_clean();
}

/* ---- the fractional delay line -------------------------------------------------------------- */

static void arm_fdelay(void) {
  g_arm = "fdelay";
  static float ring[4096];
  for (int order = 3; order <= 5; order += 2) {
    /* the kernel is unity at DC at every fraction, and the identity at a zero fraction */
    for (int fi = 0; fi < 50; fi++) {
      float c[OMX_FDELAY_MAX_TAPS];
      omx_fdelay_lagrange(order, (float)fi / 50.0f, c);
      double sum = 0.0;
      for (int k = 0; k <= order; k++) sum += c[k];
      ok(fabs(sum - 1.0) < 1e-5, "the kernel sums to one", sum, 1.0);
      if (fi == 0) ok(c[omx_fdelay_lookbehind(order)] == 1.0f, "a zero fraction is the identity tap", c[omx_fdelay_lookbehind(order)], 1.0);
    }
    /* The kernel IS its documented product, bit for bit, whatever instantiation computes it (the
     * order-3 instantiation a constant order lets the compiler unroll is the chorus lane's cost
     * change, and it must not move one bit): c[k] = (float)(Π(j≠k)(f − (j − off)) / Π(j≠k)(k − j))
     * in double, left to right, over 2^20 grid fractions and 2^16 scattered ones. */
    {
      const int off = (int)omx_fdelay_lookbehind(order);
      long diff = 0, n = 0;
      uint32_t seed = 12345u;
      for (uint32_t i = 0; i < (1u << 20) + (1u << 16); i++) {
        float f;
        if (i < (1u << 20)) f = (float)i / (float)(1u << 20);
        else { seed = seed * 1664525u + 1013904223u; f = (float)(seed >> 8) / 16777216.0f; }
        float c[OMX_FDELAY_MAX_TAPS], want[OMX_FDELAY_MAX_TAPS];
        omx_fdelay_lagrange(order, f, c);
        for (int k = 0; k <= order; k++) {
          double num = 1.0, den = 1.0;
          for (int j = 0; j <= order; j++) {
            if (j == k) continue;
            num *= (double)f - (double)(j - off);
            den *= (double)(k - j);
          }
          want[k] = (float)(num / den);
        }
        n++;
        if (memcmp(c, want, (size_t)(order + 1) * sizeof(float)) != 0) diff++;
      }
      ok(diff == 0, "the kernel is its documented product, bit for bit", (double)diff, 0.0);
      printf("omxdsp_suite fdelay: order %d, %ld/%ld fractions bit-identical to the product\n", order, n - diff, n);
    }
    struct omx_fdelay l;
    ok(omx_fdelay_init(&l, ring, 4096u, order) == OMX_FDELAY_OK, "a legal order arms", order, 0.0);
    ok(l.order == order && l.cap == 4096u, "an armed line carries what it was given", l.order, order);
    /* a whole-sample delay is an exact copy, for any input, at every declared rate's block */
    for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
      const float sr = OMX_DECLARED_RATES[ri];
      memset(ring, 0, sizeof ring);
      l.wpos = 0u;
      const uint32_t n = 256u, d = (uint32_t)(sr * 0.001f); /* 1 ms */
      float x[512], y[512];
      for (uint32_t i = 0; i < 512u; i++) { x[i] = rnd(-1.0f, 1.0f) * (i % 7 == 0 ? 1e-25f : 1.0f); y[i] = x[i]; }
      omx_fdelay_process(y, 512u, &l, (float)d);
      int exact = 1;
      for (uint32_t i = d; i < 512u; i++) if (y[i] != x[i - d]) exact = 0;
      ok(exact, "a whole-sample delay is bit-exact, below the flush floor too", d, 0.0);
      (void)n;
      /* a fractional delay of a tone reads the tone at the delayed phase within the kernel's loss */
      memset(ring, 0, sizeof ring); l.wpos = 0u;
      const float delay = 10.5f;
      const double w = 2.0 * M_PI * 1000.0 / sr;
      double worst = 0.0;
      for (uint32_t i = 0; i < 2000u; i++) {
        const float out = omx_fdelay_tick(&l, sinf((float)(w * i)), delay);
        if (i > 100u) { const double want = sin(w * ((double)i - delay)); if (fabs(out - want) > worst) worst = fabs(out - want); }
      }
      ok(worst < 2e-3, "a fractional read of a 1 kHz tone lands on the delayed phase", worst, 2e-3);
      /* latency: the clamp's answer is what the line delivers */
      ok(omx_fdelay_latency(&l, -3.0f) == 0.0f, "a negative request delivers 0", omx_fdelay_latency(&l, -3.0f), 0.0);
      ok(omx_fdelay_latency(&l, 1e9f) == omx_fdelay_max_delay(4096u, order), "a huge request is clamped to the ring", omx_fdelay_latency(&l, 1e9f), omx_fdelay_max_delay(4096u, order));
      ok(omx_fdelay_latency(&l, 0.3f) == omx_fdelay_min_delay(order), "a fraction below the reach is raised to the shortest whole delay", omx_fdelay_latency(&l, 0.3f), omx_fdelay_min_delay(order));
    }
  }
  ok(omx_fdelay_cap_for(100.0f, 5) == 100u + 2u + 2u, "the cap is the delay plus the reach plus the slot", omx_fdelay_cap_for(100.0f, 5), 104.0);
  ok(omx_fdelay_max_delay(omx_fdelay_cap_for(100.0f, 3), 3) >= 100.0f, "a cap sized for a delay serves it", omx_fdelay_max_delay(omx_fdelay_cap_for(100.0f, 3), 3), 100.0);
  ok(omx_fdelay_state_size() == sizeof(struct omx_fdelay), "the state layout is exported", (double)omx_fdelay_state_size(), sizeof(struct omx_fdelay));
  expect_clean();
}

/* ---- the oversampler ------------------------------------------------------------------------ */

static void arm_oversampler(void) {
  g_arm = "oversampler";
  /* the table: the centre and the odd taps sum to unity at DC; the dot is that sum on a DC window */
  double dc = OMX_HALFBAND_CENTER;
  for (int i = 0; i < OMX_HALFBAND_ODD_TAPS; i++) dc += 2.0 * OMX_HALFBAND_ODD_COEF[i];
  ok(fabs(dc - 1.0) < 1e-6, "the half-band is unity at DC", dc, 1.0);
  float win[4 * OMX_HALFBAND_ODD_TAPS - 1];
  for (int i = 0; i < 4 * OMX_HALFBAND_ODD_TAPS - 1; i++) win[i] = 0.5f;
  ok(fabsf(omx_halfband_dot(win + 2 * OMX_HALFBAND_ODD_TAPS - 1) - 0.5f) < 1e-6f, "the dot over a DC window is the level", omx_halfband_dot(win + 2 * OMX_HALFBAND_ODD_TAPS - 1), 0.5);
  ok(omx_oversampler_latency_for(1u) == 0u && omx_oversampler_latency_for(2u) == 48u && omx_oversampler_latency_for(4u) == 72u, "the declared latencies", omx_oversampler_latency_for(4u), 72.0);
  ok(omx_oversampler_latency_for(4u) == OMX_OVS_LATENCY_4X, "the macro and the function agree", OMX_OVS_LATENCY_4X, 72.0);
  ok(omx_oversampler_state_size() == sizeof(struct omx_oversampler), "the state layout is exported", (double)omx_oversampler_state_size(), sizeof(struct omx_oversampler));
  for (uint32_t factor = 1u; factor <= 4u; factor *= 2u) {
    struct omx_oversampler o;
    omx_oversampler_init(&o, factor);
    ok(o.factor == factor && omx_oversampler_latency(&o) == omx_oversampler_latency_for(factor), "init arms the factor", o.factor, factor);
    for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
      const float sr = OMX_DECLARED_RATES[ri];
      /* round trip of a tone: the output is the input delayed by the declared latency, within
       * the half-band's passband ripple; measured over a second of audio in uneven blocks */
      const uint32_t total = 4096u, lat = omx_oversampler_latency_for(factor);
      static float x[4096], up[4096 * 4], back[4096];
      const double w = 2.0 * M_PI * 1000.0 / sr;
      for (uint32_t i = 0; i < total; i++) x[i] = 0.5f * sinf((float)(w * i));
      omx_oversampler_init(&o, factor);
      uint32_t done = 0u;
      const uint32_t blocks[] = {64u, 37u, 128u, 1u, 256u, 100u};
      uint32_t bi = 0u;
      while (done < total) {
        uint32_t n = blocks[bi++ % 6u];
        if (n > total - done) n = total - done;
        omx_oversampler_up(&o, x + done, n, up);
        omx_oversampler_down(&o, up, n, back + done);
        done += n;
      }
      double worst = 0.0;
      for (uint32_t i = lat + 512u; i < total; i++) { const double e = fabs((double)back[i] - (double)x[i - lat]); if (e > worst) worst = e; }
      ok(worst < 0.5 * 0.0025, "up then down is the input delayed by the declared latency within the ripple", worst, 0.00125);
      /* quantum invariance: the same signal in one block equals the uneven blocks bit for bit */
      static float once[4096];
      omx_oversampler_init(&o, factor);
      omx_oversampler_up(&o, x, total, up);
      omx_oversampler_down(&o, up, total, once);
      ok(memcmp(once, back, sizeof once) == 0, "the block size does not touch a bit of the output", 0.0, 0.0);
      if (factor == 1u) ok(memcmp(once, x, sizeof once) == 0, "factor 1 is a copy", 0.0, 0.0);
      /* the interpolated tone is the tone: its level at the higher rate is the level in */
      if (factor > 1u) {
        omx_oversampler_init(&o, factor);
        omx_oversampler_up(&o, x, total, up);
        float peak = 0.0f;
        for (uint32_t i = 1024u; i < total * factor; i++) if (fabsf(up[i]) > peak) peak = fabsf(up[i]);
        ok(fabsf(peak - 0.5f) < 0.5f * 0.003f, "the interpolated tone keeps its level", peak, 0.5);
      }
    }
  }
  expect_clean();
}

/* ---- envelope ------------------------------------------------------------------------------- */

/* The N-stage cascade of identical one-poles q from rest: P(Bin(n+N, 1-q) >= N). */
static double cascade_step(uint32_t n, double q) {
  double below = 0.0, c = 1.0;
  for (int k = 0; k < OMX_DYN_ENV_STAGES; k++) {
    if (k > 0) c *= (double)(n + OMX_DYN_ENV_STAGES + 1u - (uint32_t)k) / (double)k;
    below += c * pow(1.0 - q, k) * pow(q, (double)(n + OMX_DYN_ENV_STAGES) - k);
  }
  return 1.0 - below;
}

static void arm_envelope(void) {
  g_arm = "envelope";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    const struct omx_env_params e = {omx_pole_from_time_ms(5.0f, sr), omx_pole_from_time_ms(120.0f, sr), OMX_DETECT_PEAK};
    float ac, rc;
    omx_env_stage_poles(&e, 1u, &ac, &rc);
    const double qa = pow((double)e.attack_pole, OMX_DYN_ENV_STAGES), qr = pow((double)e.release_pole, OMX_DYN_ENV_STAGES);
    ok(fabs(ac - qa) <= 1e-6 * qa, "the base-rate attack stage pole is pole^N", ac, qa);
    ok(fabs(rc - qr) <= 1e-6 * qr, "the base-rate release stage pole is pole^N", rc, qr);
    float a4, r4;
    omx_env_stage_poles(&e, 4u, &a4, &r4);
    const double q4 = pow((double)e.attack_pole, OMX_DYN_ENV_STAGES / 4.0);
    ok(fabs(a4 - q4) <= 1e-6 * q4, "at 4x the stage pole is pole^(N/4)", a4, q4);
    struct omx_env env;
    memset(&env, 0, sizeof env);
    const uint32_t len = (uint32_t)(0.03f * sr);
    double worst = 0.0;
    for (uint32_t i = 0; i < len; i++) {
      const float y = omx_env_step(&env, &e, 1.0f, ac, rc);
      const double d = fabs((double)y - cascade_step(i, ac));
      if (d > worst) worst = d;
    }
    ok(worst <= 1e-4, "the attack step response is the N-stage closed form (float state)", worst, 1e-4);
    ok(omx_env_level(&env, &e) == env.stage[OMX_DYN_ENV_STAGES - 1], "the peak level is the last stage", omx_env_level(&env, &e), env.stage[OMX_DYN_ENV_STAGES - 1]);
    for (int s = 0; s < OMX_DYN_ENV_STAGES; s++) env.stage[s] = 1.0f;
    worst = 0.0;
    for (uint32_t i = 0; i < len; i++) {
      const float y = omx_env_step(&env, &e, 0.0f, ac, rc);
      const double d = fabs((double)y - (1.0 - cascade_step(i, rc)));
      if (d > worst) worst = d;
    }
    ok(worst <= 1e-4, "the release decay is the N-stage closed form (float state)", worst, 1e-4);
    const struct omx_env_params rms = {e.attack_pole, e.release_pole, OMX_DETECT_RMS};
    memset(&env, 0, sizeof env);
    float lev = 0.0f;
    for (uint32_t i = 0; i < (uint32_t)(0.2f * sr); i++) lev = omx_env_step(&env, &rms, 0.25f * 0.25f, ac, rc);
    ok(fabsf(lev - 0.25f) < 1e-4f, "RMS of a settled 0.25 square is 0.25", lev, 0.25);
    ok(lev == sqrtf(env.stage[OMX_DYN_ENV_STAGES - 1]) && lev == omx_env_level(&env, &rms), "the RMS level is the root of the last stage", lev, 0.0);
    const struct omx_env_params inst = {0.0f, 0.0f, OMX_DETECT_PEAK};
    float a0, r0;
    omx_env_stage_poles(&inst, 1u, &a0, &r0);
    memset(&env, 0, sizeof env);
    ok(omx_env_step(&env, &inst, 0.7f, a0, r0) == 0.7f, "pole 0 is instant", env.stage[OMX_DYN_ENV_STAGES - 1], 0.7);
  }
  ok(omx_env_state_size() == sizeof(struct omx_env) && omx_env_state_align() == _Alignof(struct omx_env), "the state layout is exported", (double)omx_env_state_size(), sizeof(struct omx_env));
  expect_clean();
}


/* ---- envdiff (row 8) ----------------------------------------------------------------------- */

/* The transient spec §2 peak contrasts, dB, ΔS then ΔA: the same at every rate (L9). */
static const double ENVDIFF_PEAK_DB[2] = {19.753, 19.784};

struct envdiff_run { double worst, bound, peak_w, peak_c, q, slope_from, slope_to; uint32_t at_c, from_w, from_c, to_w, to_c; };

/* The 0.1 -> 1.0 step (rise = 1, ΔA) or 1.0 -> 0.1 (rise = 0, ΔS) at the declared defaults, the
 * word against the closed form on the float32 per-stage poles it runs. */
static struct envdiff_run envdiff_step_run(float sr, int rise) {
  const float slow_ms = rise ? OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT : OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT;
  const struct omx_env_params fe = {omx_pole_from_time_ms(OMX_TRANSIENT_FAST_ATTACK_MS, sr),
                                    omx_pole_from_time_ms(OMX_TRANSIENT_FAST_RELEASE_MS, sr), OMX_DETECT_PEAK};
  const struct omx_env_params se = {rise ? omx_pole_from_time_ms(slow_ms, sr) : fe.attack_pole,
                                    rise ? fe.release_pole : omx_pole_from_time_ms(slow_ms, sr), OMX_DETECT_PEAK};
  struct omx_envdiff_poles p;
  omx_env_stage_poles(&fe, 1u, &p.fast_attack, &p.fast_release);
  omx_env_stage_poles(&se, 1u, &p.slow_attack, &p.slow_release);
  const double a = rise ? 0.1 : 1.0, b = rise ? 1.0 : 0.1;
  const double qf = rise ? p.fast_attack : p.fast_release, qs = rise ? p.slow_attack : p.slow_release;
  struct envdiff_run r = {0.0, 0.0, -1.0, -1.0, qs, 0.0, 0.0, 0u, 0u, 0u, 0u, 0u};
  double dc_prev = 0.0;
  r.bound = 20.0 * log10(1.0 + OMX_DYN_ENV_STAGES * 3.0 * ldexp(1.0, -24) / (1.0 - qs)) + 2e-4;
  struct omx_env fast, slow;
  for (int s = 0; s < OMX_DYN_ENV_STAGES; s++) fast.stage[s] = slow.stage[s] = (float)a;
  const uint32_t len = (uint32_t)((rise ? 0.02f : 0.5f) * sr);
  for (uint32_t i = 0; i < len; i++) {
    const float fl = omx_env_step(&fast, &fe, (float)b, p.fast_attack, p.fast_release);
    const float dw = omx_envdiff_step(&slow, &fe, (float)b, fl, &p, OMX_TRANSIENT_FLOOR_LIN);
    const double ef = a + (b - a) * cascade_step(i, qf), es = a + (b - a) * cascade_step(i, qs);
    const double dc = rise ? 20.0 * log10(ef / es) : 20.0 * log10(es / ef);
    if (fabs(dw - dc) > r.worst) r.worst = fabs(dw - dc);
    if (dw > r.peak_w) r.peak_w = dw;
    if (dc > r.peak_c) { r.peak_c = dc; r.at_c = i; }
    if (dw >= OMX_TRANSIENT_REF_DB) { if (!r.from_w) r.from_w = i; r.to_w = i; }
    if (dc >= OMX_TRANSIENT_REF_DB) { if (!r.from_c) { r.from_c = i; r.slope_from = fabs(dc - dc_prev); } r.to_c = i; }
    else if (r.from_c && !r.slope_to) r.slope_to = fabs(dc - dc_prev);
    dc_prev = dc;
  }
  return r;
}


static void arm_envdiff(void) {
  g_arm = "envdiff";
  uint32_t tabled = 0u;
  double worst_ratio = 0.0;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    for (int rise = 1; rise >= 0; rise--) {
      const struct envdiff_run r = envdiff_step_run(sr, rise);
      if (r.worst / r.bound > worst_ratio) worst_ratio = r.worst / r.bound;
      ok(r.worst <= r.bound, rise ? "ΔA on the rising step is the closed form" : "ΔS on the falling step is the closed form", r.worst, r.bound);
      ok(fabs(r.peak_w - r.peak_c) <= 0.01, "the word's peak contrast is the closed form's", r.peak_w, r.peak_c);
      const double tol_from = 1.0 + r.bound / r.slope_from, tol_to = 1.0 + r.bound / r.slope_to;
      ok(fabs((double)r.from_w - r.from_c) <= tol_from, "the ≥ REF window opens within 1 + bound/slope samples of the closed form", fabs((double)r.from_w - r.from_c), tol_from);
      ok(fabs((double)r.to_w - r.to_c) <= tol_to, "the ≥ REF window closes within 1 + bound/slope samples of the closed form", fabs((double)r.to_w - r.to_c), tol_to);
      ok(fabs(r.peak_c - ENVDIFF_PEAK_DB[rise]) <= 0.001, "the closed-form peak is the spec §2 table's, at every rate", r.peak_c, ENVDIFF_PEAK_DB[rise]);
      tabled++;
    }
    /* L2 and L3 over gated noise: both contrasts non-negative at every sample; identical at every
     * power-of-two level, within the arm A bound at the decimal ones. */
    const float k[7] = {1.0f, 0x1p-7f, 0x1p-3f, 0x1p3f, 1e-2f, 1e-1f, 10.0f};
    const struct omx_env_params fe = {omx_pole_from_time_ms(OMX_TRANSIENT_FAST_ATTACK_MS, sr),
                                      omx_pole_from_time_ms(OMX_TRANSIENT_FAST_RELEASE_MS, sr), OMX_DETECT_PEAK};
    const struct omx_env_params ae = {omx_pole_from_time_ms(OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT, sr), fe.release_pole, OMX_DETECT_PEAK};
    const struct omx_env_params re = {fe.attack_pole, omx_pole_from_time_ms(OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT, sr), OMX_DETECT_PEAK};
    struct omx_envdiff_poles pa, pr;
    omx_env_stage_poles(&fe, 1u, &pa.fast_attack, &pa.fast_release);
    pr.fast_attack = pa.fast_attack; pr.fast_release = pa.fast_release;
    omx_env_stage_poles(&ae, 1u, &pa.slow_attack, &pa.slow_release);
    omx_env_stage_poles(&re, 1u, &pr.slow_attack, &pr.slow_release);
    const double bound = 20.0 * log10(1.0 + OMX_DYN_ENV_STAGES * 3.0 * ldexp(1.0, -24) / (1.0 - pr.slow_release)) + 2e-4;
    const uint32_t len = (uint32_t)(1.0f * sr), gate = (uint32_t)(sr / 8.0f);
    static float ref_a[192000], ref_s[192000];
    uint32_t negative = 0u, pow2_diff = 0u;
    double worst_level = 0.0;
    for (int ki = 0; ki < 7; ki++) {
      struct omx_env f, sa, ss;
      memset(&f, 0, sizeof f); memset(&sa, 0, sizeof sa); memset(&ss, 0, sizeof ss);
      g_seed = 0x2f6e2b1du;
      for (uint32_t i = 0; i < len; i++) {
        const float amp = ((i / gate) & 1u) ? 0.1f : 0.9f;
        const float d = k[ki] * (amp * (0.5f + 0.5f * fabsf(rnd(-1.0f, 1.0f))));
        const float fl = omx_env_step(&f, &fe, d, pa.fast_attack, pa.fast_release);
        const float da = omx_envdiff_step(&sa, &fe, d, fl, &pa, OMX_TRANSIENT_FLOOR_LIN);
        const float ds = omx_envdiff_step(&ss, &fe, d, fl, &pr, OMX_TRANSIENT_FLOOR_LIN);
        negative += (da < 0.0f) + (ds < 0.0f);
        if (ki == 0) { ref_a[i] = da; ref_s[i] = ds; continue; }
        if (i < (uint32_t)(0.01f * sr)) continue;
        if (ki < 4) { pow2_diff += (da != ref_a[i]) + (ds != ref_s[i]); continue; }
        const double e = fmax(fabs(da - ref_a[i]), fabs(ds - ref_s[i]));
        if (e > worst_level) worst_level = e;
      }
    }
    ok(negative == 0u, "L2: both contrasts are non-negative at every sample (gated noise)", negative, 0.0);
    ok(pow2_diff == 0u, "L3: at x2^-7, x2^-3, x2^3 the contrasts are bit-identical", pow2_diff, 0.0);
    ok(worst_level <= bound, "L3: at x0.01, x0.1, x10 the contrasts agree within the arm A bound", worst_level, bound);
    struct omx_env f, s;
    memset(&f, 0, sizeof f); memset(&s, 0, sizeof s);
    float silent = 0.0f;
    for (uint32_t i = 0; i < 1000u; i++) {
      const float fl = omx_env_step(&f, &fe, 0.0f, pa.fast_attack, pa.fast_release);
      silent = fmaxf(silent, omx_envdiff_step(&s, &fe, 0.0f, fl, &pa, OMX_TRANSIENT_FLOOR_LIN));
    }
    ok(silent == 0.0f, "below the floor the contrast is exactly 0", silent, 0.0);
  }
  ok(tabled == 2u * OMX_DECLARED_RATE_COUNT, "every declared rate was checked against the §2 peaks, both steps", tabled, 2.0 * OMX_DECLARED_RATE_COUNT);
  printf("envdiff: worst step error %.3g of its bound, %u declared rates\n", worst_ratio, OMX_DECLARED_RATE_COUNT);
  expect_clean();
}

/* ---- gain computer -------------------------------------------------------------------------- */

static double gaincomp_closed(int mode, double t, double r, double k, double range, double l) {
  const double x = l - t, h = 0.5 * k;
  if (mode == OMX_DYN_ABOVE) {
    if (x <= -h) return 0.0;
    if (k > 0.0 && x < h) return (1.0 / r - 1.0) * (x + h) * (x + h) / (2.0 * k);
    return (1.0 / r - 1.0) * x;
  }
  double g;
  if (x >= h) g = 0.0;
  else if (k > 0.0 && x > -h) g = (1.0 - r) * (x - h) * (x - h) / (2.0 * k);
  else g = (r - 1.0) * x;
  return g < range ? range : g;
}

static void arm_gaincomp(void) {
  g_arm = "gaincomp";
  static const struct omx_gaincomp_params cases[] = {
      {OMX_DYN_ABOVE, -20.0f, 4.0f, 6.0f, 0.0f, 1.0f},
      {OMX_DYN_ABOVE, -12.0f, 20.0f, 0.0f, 0.0f, 2.0f},
      {OMX_DYN_ABOVE, -30.0f, 1.0f, 12.0f, 0.0f, 1.0f},
      {OMX_DYN_BELOW, -40.0f, 16.0f, 6.0f, -60.0f, 1.0f},
      {OMX_DYN_BELOW, -50.0f, 100.0f, 0.0f, -90.0f, 1.0f},
      {OMX_DYN_BELOW, -35.0f, 2.0f, 10.0f, -20.0f, 1.5f},
  };
  for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    const struct omx_gaincomp_params *p = &cases[c];
    double worst = 0.0, lo = 1e9, hi = -1e9;
    for (int i = 0; i < 200; i++) {
      const float l = -100.0f + 0.5f * (float)i;
      const double d = fabs((double)omx_gaincomp_db(p, l) - gaincomp_closed(p->mode, p->thresh_db, p->ratio, p->knee_db, p->range_db, l));
      if (d > worst) worst = d;
      const float g = omx_gaincomp_gain(p, omx_db_to_lin(l));
      if (g < lo) lo = g;
      if (g > hi) hi = g;
    }
    ok(worst <= 1e-4, "the characteristic is the closed form at 200 levels", worst, 1e-4);
    ok(lo >= 0.0 && hi <= p->makeup_lin * (1.0 + 1e-6), "the gain lies in [0, makeup]", hi, p->makeup_lin);
    if (p->knee_db > 0.0f) {
      const float h = 0.5f * p->knee_db, eps = 1e-3f;
      for (int side = -1; side <= 1; side += 2) {
        const float edge = p->thresh_db + (float)side * h;
        const float in = omx_gaincomp_db(p, edge - (float)side * eps), out = omx_gaincomp_db(p, edge + (float)side * eps);
        ok(fabsf(in - out) <= 2.0f * eps * p->ratio + 1e-4f, "the knee joins its neighbour without a step", fabsf(in - out), 2.0f * eps * p->ratio + 1e-4f);
      }
    }
  }
  const struct omx_gaincomp_params unity = {OMX_DYN_ABOVE, 0.0f, 4.0f, 0.0f, 0.0f, 1.0f};
  ok(omx_gaincomp_gain(&unity, 0.5f) == 1.0f, "below the threshold the comp is unity bit for bit", omx_gaincomp_gain(&unity, 0.5f), 1.0);
  expect_clean();
}

/* ---- the zero-crossing divider ------------------------------------------------------------- */

/* |DTFT| of y[n0 .. n0+N) at hz, scaled so a sine of amplitude A reads A. */
static double line_at(const float *y, uint32_t n0, uint32_t N, double hz, double sr) {
  double re = 0.0, im = 0.0;
  const double w = 2.0 * M_PI * hz / sr;
  for (uint32_t n = 0; n < N; n++) {
    re += (double)y[n0 + n] * cos(w * (double)(n0 + n));
    im -= (double)y[n0 + n] * sin(w * (double)(n0 + n));
  }
  return 2.0 * sqrt(re * re + im * im) / (double)N;
}

/* The series of the sub-octaver spec §2: |D(A sin)| at (2j+1)f/2. */
static double divider_series(int j, double a) {
  return j == 0 ? a * 8.0 / (3.0 * M_PI) : a * 8.0 / (M_PI * (2.0 * j + 3.0) * (2.0 * j - 1.0));
}

/* A Schmitt trigger that toggles AT +h: the form the spec refuses, the positive control of arm E. */
static float schmitt_at_threshold(float *q, int *hi, float v, float h) {
  if (!*hi && v > h) { *q = -*q; *hi = 1; }
  if (v < -h) *hi = 0;
  return *q * v;
}

static void arm_divider(void) {
  g_arm = "divider";
  enum { MAXS = 192000 * 3 };
  static float x[MAXS], y[MAXS];
  static const double tones[] = {41.2, 110.0, 440.0};
  const double a = 0.5;
  double worst_line = 0.0, worst_f = 0.0;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    /* A: the lines of D(A sin) at (2j+1)f/2 are the series, and the line at f is zero. */
    for (int t = 0; t < 3; t++) {
      const double f = tones[t];
      const uint32_t n0 = (uint32_t)ceil(4.0 * sr / f);
      const uint32_t N = (uint32_t)llround(floor(sr / (2.0 * sr / f)) * (2.0 * sr / f));
      const uint32_t total = n0 + N;
      for (uint32_t n = 0; n < total; n++) x[n] = (float)(a * sin(2.0 * M_PI * f * (double)n / sr));
      struct omx_divider d;
      omx_divider_init(&d);
      omx_divider_block(&d, x, y, total, (float)(0.25 * a));
      for (int j = 0; j <= 4; j++) {
        const double got = line_at(y, n0, N, (2.0 * j + 1.0) * f / 2.0, sr);
        const double err = fabs(got - divider_series(j, a)) / a;
        if (err > worst_line) worst_line = err;
        ok(err < 1e-4, "a line of D(A sin) is the closed-form series to 1e-4 of A", err, 1e-4);
      }
      const double at_f = line_at(y, n0, N, f, sr) / a;
      if (at_f > worst_f) worst_f = at_f;
      ok(at_f < 1e-5, "the line at the input's own f is below -100 dB re A", at_f, 1e-5);
      uint32_t crossings = 0u;
      for (uint32_t k = 1u; ceil((double)k * sr / f) <= (double)(total - 1u); k++) crossings++;
      ok(d.toggles == crossings, "one toggle per input period (L2)", (double)d.toggles, (double)crossings);
    }
    /* C: noise with peak under h/2 never adds a toggle; a harmonic-rich note toggles once a cycle. */
    {
      const double f = 110.0;
      const uint32_t total = (uint32_t)(2.0 * sr + 0.5 * sr / f);
      const double h = 0.25 * a;
      const uint32_t periods = 220u;
      static const double under[] = {-20.0, -18.5};
      for (int k = 0; k < 2; k++) {
        const double peak = a * pow(10.0, under[k] / 20.0);
        for (uint32_t n = 0; n < total; n++)
          x[n] = (float)(a * sin(2.0 * M_PI * f * (double)n / sr + 0.1) + (double)rnd((float)-peak, (float)peak));
        struct omx_divider d;
        omx_divider_init(&d);
        omx_divider_block(&d, x, y, total, (float)h);
        ok(d.toggles == periods, "noise under h/2 adds no toggle", (double)d.toggles, (double)periods);
      }
      {
        const double peak = a * pow(10.0, -12.5 / 20.0);
        for (uint32_t n = 0; n < total; n++)
          x[n] = (float)(a * sin(2.0 * M_PI * f * (double)n / sr + 0.1) + (double)rnd((float)-peak, (float)peak));
        struct omx_divider d;
        omx_divider_init(&d);
        omx_divider_block(&d, x, y, total, (float)h);
        ok(d.toggles > periods, "positive control: noise at -12.5 dB (above h/2) adds a toggle the count sees",
           (double)d.toggles, (double)periods);
      }
      for (int ph = 0; ph < 16; ph++) {
        const double phi = 2.0 * M_PI * ph / 16.0;
        for (uint32_t n = 0; n < total; n++) {
          const double w = 2.0 * M_PI * f * (double)n / sr;
          x[n] = (float)(a * (sin(w + 0.1) + 0.25 * sin(2.0 * w + phi)));
        }
        struct omx_divider d;
        omx_divider_init(&d);
        omx_divider_block(&d, x, y, total, (float)h);
        ok(d.toggles == periods, "H2 at -12 dB at every phase toggles once a cycle", (double)d.toggles, (double)periods);
      }
    }
    /* E: the largest output step never exceeds the input's; the +h Schmitt breaks the bound. */
    {
      const double f = 560.0;
      const uint32_t total = (uint32_t)(sr / 2.0);
      for (uint32_t n = 0; n < total; n++) x[n] = (float)sin(2.0 * M_PI * f * (double)n / sr);
      struct omx_divider d;
      omx_divider_init(&d);
      omx_divider_block(&d, x, y, total, 0.25f);
      float q = 1.0f;
      int hi = 0;
      double din = 0.0, dout = 0.0, dsch = 0.0, prev_s = 0.0;
      for (uint32_t n = 1; n < total; n++) {
        const double s = schmitt_at_threshold(&q, &hi, x[n], 0.25f);
        din = fmax(din, fabs((double)x[n] - x[n - 1]));
        dout = fmax(dout, fabs((double)y[n] - y[n - 1]));
        dsch = fmax(dsch, fabs(s - prev_s));
        prev_s = s;
      }
      ok(dout <= din, "the divider's largest step is within the input's", dout, din);
      ok(dsch > 2.0 * din, "positive control: a toggle at +h steps past twice the input's step", dsch, 2.0 * din);
    }
  }
  printf("divider: worst line error %.3g of A, worst line at f %.3g of A (%.1f dB)\n", worst_line, worst_f,
         20.0 * log10(worst_f + 1e-30));
  ok(omx_divider_state_size() == sizeof(struct omx_divider) && omx_divider_state_align() == _Alignof(struct omx_divider),
     "the state layout is exported", (double)omx_divider_state_size(), sizeof(struct omx_divider));
  expect_clean();
}

/* ---- state relocation ----------------------------------------------------------------------- */

/* A block sized and aligned as the primitive exports, the size rounded up to the alignment. */
static void *state_block(size_t size, size_t align) {
  const size_t rounded = (size + align - 1u) / align * align;
  return aligned_alloc(align, rounded);
}

/* Every stateful primitive: a reference run in one state, then a second run whose state is
 * memcpy'd to a fresh aligned block halfway (the old block poisoned, so nothing reads it) —
 * the two outputs bit-identical. The line's ring is caller memory the state points at: it
 * stays where it is and only the state moves. */
static void arm_relocation(void) {
  g_arm = "relocation";
  enum { N = 512, HALF = 256, BLOCK = 64 };
  static float x[N], ref[N], out[N], up[BLOCK * 4];
  for (int i = 0; i < N; i++) x[i] = rnd(-0.5f, 0.5f);
  ok(omx_lfo_state_align() > 0u && (omx_lfo_state_align() & (omx_lfo_state_align() - 1u)) == 0u, "the LFO's alignment is a power of two", (double)omx_lfo_state_align(), 0.0);
  ok(omx_fdelay_state_align() > 0u && (omx_fdelay_state_align() & (omx_fdelay_state_align() - 1u)) == 0u, "the line's alignment is a power of two", (double)omx_fdelay_state_align(), 0.0);
  ok(omx_env_state_align() > 0u && (omx_env_state_align() & (omx_env_state_align() - 1u)) == 0u, "the envelope's alignment is a power of two", (double)omx_env_state_align(), 0.0);
  ok(omx_oversampler_state_align() > 0u && (omx_oversampler_state_align() & (omx_oversampler_state_align() - 1u)) == 0u, "the oversampler's alignment is a power of two", (double)omx_oversampler_state_align(), 0.0);
  {
    struct omx_lfo l = {0.0f, omx_lfo_inc(3.0f, 48000.0f), 0.0f};
    for (int i = 0; i < N; i++) { ref[i] = omx_lfo_at(&l, 0.25f); omx_lfo_advance(&l); }
    struct omx_lfo *a = state_block(omx_lfo_state_size(), omx_lfo_state_align());
    struct omx_lfo *b = state_block(omx_lfo_state_size(), omx_lfo_state_align());
    ok(a != 0 && b != 0, "two aligned blocks for the LFO", 0.0, 0.0);
    a->phase = 0.0f; a->inc = omx_lfo_inc(3.0f, 48000.0f);
    for (int i = 0; i < HALF; i++) { out[i] = omx_lfo_at(a, 0.25f); omx_lfo_advance(a); }
    memcpy(b, a, omx_lfo_state_size());
    memset(a, 0xAA, omx_lfo_state_size());
    for (int i = HALF; i < N; i++) { out[i] = omx_lfo_at(b, 0.25f); omx_lfo_advance(b); }
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated LFO continues bit for bit", 0.0, 0.0);
    free(a); free(b);
  }
  {
    static float ring[1024];
    struct omx_fdelay l;
    memset(ring, 0, sizeof ring);
    ok(omx_fdelay_init(&l, ring, 1024u, 5) == OMX_FDELAY_OK, "the reference line arms", 0.0, 0.0);
    for (int i = 0; i < N; i++) ref[i] = omx_fdelay_tick(&l, x[i], 7.25f + 3.0f * (float)(i % 5));
    memset(ring, 0, sizeof ring);
    struct omx_fdelay *a = state_block(omx_fdelay_state_size(), omx_fdelay_state_align());
    struct omx_fdelay *b = state_block(omx_fdelay_state_size(), omx_fdelay_state_align());
    ok(a != 0 && b != 0, "two aligned blocks for the line", 0.0, 0.0);
    ok(omx_fdelay_init(a, ring, 1024u, 5) == OMX_FDELAY_OK, "the relocating line arms", 0.0, 0.0);
    for (int i = 0; i < HALF; i++) out[i] = omx_fdelay_tick(a, x[i], 7.25f + 3.0f * (float)(i % 5));
    memcpy(b, a, omx_fdelay_state_size());
    memset(a, 0xAA, omx_fdelay_state_size());
    for (int i = HALF; i < N; i++) out[i] = omx_fdelay_tick(b, x[i], 7.25f + 3.0f * (float)(i % 5));
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated line over the same ring continues bit for bit", 0.0, 0.0);
    ok(b->ring == ring, "the relocated line still points at the caller's ring", 0.0, 0.0);
    free(a); free(b);
  }
  {
    const struct omx_env_params e = {omx_pole_from_time_ms(1.0f, 48000.0f), omx_pole_from_time_ms(30.0f, 48000.0f), OMX_DETECT_RMS};
    float ac, rc;
    omx_env_stage_poles(&e, 1u, &ac, &rc);
    struct omx_env ref_env;
    memset(&ref_env, 0, sizeof ref_env);
    for (int i = 0; i < N; i++) ref[i] = omx_env_step(&ref_env, &e, x[i] * x[i], ac, rc);
    struct omx_env *a = state_block(omx_env_state_size(), omx_env_state_align());
    struct omx_env *b = state_block(omx_env_state_size(), omx_env_state_align());
    ok(a != 0 && b != 0, "two aligned blocks for the envelope", 0.0, 0.0);
    memset(a, 0, omx_env_state_size());
    for (int i = 0; i < HALF; i++) out[i] = omx_env_step(a, &e, x[i] * x[i], ac, rc);
    memcpy(b, a, omx_env_state_size());
    memset(a, 0xAA, omx_env_state_size());
    for (int i = HALF; i < N; i++) out[i] = omx_env_step(b, &e, x[i] * x[i], ac, rc);
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated envelope continues bit for bit", 0.0, 0.0);
    free(a); free(b);
  }
  {
    struct omx_oversampler o;
    omx_oversampler_init(&o, 4u);
    for (int i = 0; i < N; i += BLOCK) { omx_oversampler_up(&o, x + i, BLOCK, up); omx_oversampler_down(&o, up, BLOCK, ref + i); }
    struct omx_oversampler *a = state_block(omx_oversampler_state_size(), omx_oversampler_state_align());
    struct omx_oversampler *b = state_block(omx_oversampler_state_size(), omx_oversampler_state_align());
    ok(a != 0 && b != 0, "two aligned blocks for the oversampler", 0.0, 0.0);
    omx_oversampler_init(a, 4u);
    for (int i = 0; i < HALF; i += BLOCK) { omx_oversampler_up(a, x + i, BLOCK, up); omx_oversampler_down(a, up, BLOCK, out + i); }
    memcpy(b, a, omx_oversampler_state_size());
    memset(a, 0xAA, omx_oversampler_state_size());
    for (int i = HALF; i < N; i += BLOCK) { omx_oversampler_up(b, x + i, BLOCK, up); omx_oversampler_down(b, up, BLOCK, out + i); }
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated oversampler continues bit for bit", 0.0, 0.0);
    free(a); free(b);
  }
  {
    struct omx_divider ref_d;
    omx_divider_init(&ref_d);
    omx_divider_block(&ref_d, x, ref, N, 0.1f);
    struct omx_divider *a = state_block(omx_divider_state_size(), omx_divider_state_align());
    struct omx_divider *b = state_block(omx_divider_state_size(), omx_divider_state_align());
    ok(a != 0 && b != 0, "two aligned blocks for the divider", 0.0, 0.0);
    omx_divider_init(a);
    omx_divider_block(a, x, out, HALF, 0.1f);
    memcpy(b, a, omx_divider_state_size());
    memset(a, 0xAA, omx_divider_state_size());
    omx_divider_block(b, x + HALF, out + HALF, N - HALF, 0.1f);
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated divider continues bit for bit", 0.0, 0.0);
    free(a); free(b);
  }
  expect_clean();
}

int main(void) {
  omx_contract_reset();
  arm_ledger();
  arm_rates();
  arm_block_helpers();
  arm_version();
  arm_denormal();
  arm_units();
  arm_onepole();
  arm_biquad();
  arm_eq_design();
  arm_matched_pair();
  arm_lfo();
  arm_fdelay();
  arm_oversampler();
  arm_envelope();
  arm_envdiff();
  arm_gaincomp();
  arm_divider();
  arm_relocation();
  const uint32_t violations = omx_contract_log.count;
  const uint32_t evaluated = omx_contract_log.checks;
  printf("omxdsp_suite: %d checks, %d failed; %u contracts evaluated, %u violations left\n", g_checks,
         g_failed, evaluated, violations);
  if (evaluated < 1000u) {
    printf("FAIL the suite evaluated %u contracts — floor is 1000\n", evaluated);
    return 1;
  }
  if (violations != 0u) return 1;
  return g_failed == 0 ? 0 : 1;
}
