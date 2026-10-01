// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

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
