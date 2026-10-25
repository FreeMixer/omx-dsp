// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* ---- the tilt's span (omx_tilt.h): a = 10^(span/40), -span/2 at DC, +span/2 at Nyquist ------ */

static void arm_tilt_span(void) {
  g_arm = "tilt_span";
  static const float RATES[] = OMX_ORACLE_FULL_RATES_INIT;
  static const double SPANS[] = {-6.0, -3.5, -0.5, 0.5, 2.0, 6.0, 12.0};
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const double sr = RATES[ri];
    for (uint32_t si = 0; si < 7u; si++) {
      const double a = omx_tilt_a_of_span(SPANS[si]);
      ok(fabs(a / pow(10.0, SPANS[si] / 40.0) - 1.0) < 1e-14, "a = 10^(span/40)", a, pow(10.0, SPANS[si] / 40.0));
      struct omx_tilt t;
      omx_tilt_design(&t, a, 1000.0, sr);
      /* DC and Nyquist straight off the section: -span/2 and +span/2. */
      const double dc = (t.c[0] + t.c[1]) / (1.0 + t.c[3]), ny = (t.c[0] - t.c[1]) / (1.0 - t.c[3]);
      ok(fabs(20.0 * log10(dc) + 0.5 * SPANS[si]) < 1e-9, "-span/2 at DC", 20.0 * log10(dc), -0.5 * SPANS[si]);
      ok(fabs(20.0 * log10(ny) - 0.5 * SPANS[si]) < 1e-9, "+span/2 at Nyquist", 20.0 * log10(ny), 0.5 * SPANS[si]);
      ok(fabs(20.0 * log10(omx_tilt_magnitude(a, 1000.0, 1000.0, sr))) < 1e-12, "0 dB at the pivot",
         omx_tilt_magnitude(a, 1000.0, 1000.0, sr), 1.0);
    }
  }
  /* A 6 dB span is a +-3 dB pair, about 2 dB per octave at the pivot: one section, not the slope's. */
  const double a6 = omx_tilt_a_of_span(6.0);
  const double slope6 = OMX_TILT_OCTAVE_DB * (a6 * a6 - 1.0) / (a6 * a6 + 1.0);
  ok(fabs(slope6 - 2.0005) < 1e-3, "a 6 dB span slopes about 2 dB per octave at the pivot", slope6, 2.0005);
  ok(omx_tilt_a_of_span(0.0) == 1.0 && omx_tilt_a_of_span(NAN) == 1.0, "a span of 0 or NaN is flat, exactly",
     omx_tilt_a_of_span(NAN), 1.0);
  ok(omx_tilt_a_of_span(1e9) == omx_tilt_a_of_span(OMX_TILT_SPAN_MAX_DB) &&
         omx_tilt_a_of_span(-1e9) == omx_tilt_a_of_span(-OMX_TILT_SPAN_MAX_DB),
     "a span past OMX_TILT_SPAN_MAX_DB is held there", omx_tilt_a_of_span(1e9), 0.0);
  expect_clean();
}
