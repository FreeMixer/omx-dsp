// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_pan_law.h>

/* ---- the layout-driven pan law (mix/omx_pan_law.h, FreeMixer/omx-dsp#98). At each of the nine
 * RME rates, one second of a pan sweep (pan = 1.25·sin(2π·5·i/sr), the clamp on the path) over
 * every layout: stereo, LCR at three centre widths and four divergences, and two wider axes. The
 * gains' power sums to 1 at every position of every layout; no gain is inverted; only the
 * adjacent pair (and the centre) carry the source. Stereo: −3 dB per leg at centre, the hard legs,
 * and (pan + 1)·π/4 bit for bit. LCR at divergence 1 is stereo exactly; at divergence 0 the centre
 * takes the whole source at pan 0. The LCR fold's centre level is the float nearest 1/√2, −3.01 dB,
 * and a centre folded onto a pair at it carries half its power into each leg. ------------------------ */

static const float MIX_PAN_RATES[OMX_RME_RATES_COUNT] = OMX_RME_RATES_INIT;

struct mix_pan_case {
  struct omx_pan_layout l;
  float divergence;
};

static uint32_t mix_pan_cases(struct mix_pan_case *c) {
  uint32_t n = 0;
  omx_pan_layout_stereo(&c[n].l);
  c[n++].divergence = 0.0f;
  static const float widths[] = {0.5f, 0.25f, 1.0f};
  static const float divs[] = {0.0f, 0.25f, 0.5f, 1.0f};
  for (uint32_t w = 0; w < 3; w++)
    for (uint32_t d = 0; d < 4; d++) {
      omx_pan_layout_lcr(&c[n].l, widths[w]);
      c[n++].divergence = divs[d];
    }
  omx_pan_layout_stereo(&c[n].l); /* three on the axis: L, a phantom-free centre, R */
  c[n].l.n_axis = 3u;
  c[n].l.pos[1] = 0.0f;
  c[n].l.pos[2] = 1.0f;
  c[n++].divergence = 0.0f;
  omx_pan_layout_stereo(&c[n].l); /* five, uneven: a wide front */
  c[n].l.n_axis = 5u;
  c[n].l.pos[1] = -0.6f;
  c[n].l.pos[2] = 0.1f;
  c[n].l.pos[3] = 0.5f;
  c[n].l.pos[4] = 1.0f;
  c[n++].divergence = 0.0f;
  return n;
}

static void arm_mix_pan_law(void) {
  g_arm = "mix_pan_law";
  struct mix_pan_case cases[16];
  const uint32_t n_cases = mix_pan_cases(cases);

  /* Stereo at its fixed points. */
  float gl, gr;
  omx_pan_stereo(0.0f, &gl, &gr);
  ok(gl == gr, "stereo centre: the legs are equal", gl, gr);
  ok(fabs(20.0 * log10(gl) - 20.0 * log10(M_SQRT1_2)) < 1e-5, "stereo centre: -3 dB per leg",
     20.0 * log10(gl), 20.0 * log10(M_SQRT1_2));
  omx_pan_stereo(-1.0f, &gl, &gr);
  ok(gl == 1.0f && gr == 0.0f, "stereo hard left is (1, 0)", gl, gr);
  omx_pan_stereo(1.0f, &gl, &gr);
  ok(gr == 1.0f && fabsf(gl) < 1e-7f, "stereo hard right is (0, 1)", gl, gr);
  omx_pan_stereo(7.0f, &gl, &gr);
  ok(gr == 1.0f && fabsf(gl) < 1e-7f, "a pan past +1 is clamped to hard right", gl, gr);

  /* LCR fixed points. */
  struct omx_pan_layout lcr;
  omx_pan_layout_lcr(&lcr, 0.5f);
  float g3[3];
  omx_pan_gains(&lcr, 0.0f, 0.0f, g3);
  ok(g3[2] == 1.0f && g3[0] == 0.0f && g3[1] == 0.0f, "LCR divergence 0 at centre: C alone", g3[2], 1.0);
  omx_pan_gains(&lcr, 0.75f, 0.0f, g3);
  ok(g3[2] == 0.0f, "LCR: past the centre window C is 0", g3[2], 0.0);

  for (uint32_t ri = 0; ri < OMX_RME_RATES_COUNT; ri++) {
    const float sr = MIX_PAN_RATES[ri];
    const uint32_t n = (uint32_t)sr;
    uint32_t power_bad = 0, inverted = 0, stray = 0, stereo_bits = 0, lcr_div1_bits = 0;
    double worst_power = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      const float pan = 1.25f * sinf((float)(2.0 * M_PI * 5.0 * (double)i / sr));
      for (uint32_t c = 0; c < n_cases; c++) {
        const struct omx_pan_layout *l = &cases[c].l;
        float g[OMX_PAN_SPEAKERS_MAX];
        omx_pan_gains(l, pan, cases[c].divergence, g);
        double power = 0.0;
        uint32_t live = 0;
        for (uint32_t k = 0; k < omx_pan_layout_speakers(l); k++) {
          power += (double)g[k] * g[k];
          if (g[k] < -1e-6f) inverted++;
          if (k < l->n_axis && fabsf(g[k]) > 1e-6f) live++;
        }
        const double err = fabs(power - 1.0);
        if (err > worst_power) worst_power = err;
        if (err > (l->centre ? 1e-4 : 1e-5)) power_bad++;
        if (live > 2u) stray++;
      }
      /* stereo: the engine's closed form, bit for bit */
      const float cp = pan < -1.0f ? -1.0f : (pan > 1.0f ? 1.0f : pan);
      const float theta = (cp + 1.0f) * 0.7853981633974483f;
      omx_pan_stereo(pan, &gl, &gr);
      if (gl != cosf(theta) || gr != sinf(theta)) stereo_bits++;
      /* LCR at divergence 1 is the stereo law */
      omx_pan_gains(&lcr, pan, 1.0f, g3);
      if (g3[0] != gl || g3[1] != gr || g3[2] != 0.0f) lcr_div1_bits++;
    }
    ok(power_bad == 0, "the gains' power sums to 1 at every position of every layout", worst_power, 1e-5);
    ok(inverted == 0, "no gain is inverted at this rate", inverted, 0);
    ok(stray == 0, "only the adjacent pair carries the source on the axis", stray, 0);
    ok(stereo_bits == 0, "stereo is (pan + 1)·π/4, cos and sin, bit for bit", stereo_bits, 0);
    ok(lcr_div1_bits == 0, "LCR at divergence 1 is the stereo law bit for bit", lcr_div1_bits, 0);

    /* the LCR fold's centre level: one second of a 997 Hz centre, folded onto a pair */
    const float clev = OMX_PAN_LCR_FOLD_CLEV;
    double pc = 0.0, pl = 0.0;
    for (uint32_t i = 0; i < n; i++) {
      const float c = 0.5f * sinf((float)(2.0 * M_PI * 997.0 * (double)i / sr));
      const float lo = clev * c;
      pc += (double)c * c;
      pl += (double)lo * lo;
    }
    ok(fabs(pl / pc - 0.5) < 1e-7, "a centre folded at clev carries half its power into each leg", pl / pc, 0.5);
  }
  {
    const float clev = OMX_PAN_LCR_FOLD_CLEV, want = (float)M_SQRT1_2;
    ok(memcmp(&clev, &want, sizeof clev) == 0, "clev is the float nearest 1/sqrt(2)", clev, M_SQRT1_2);
    ok(fabs(20.0 * log10((double)clev) + 3.0103) < 1e-4, "clev is -3.01 dB", 20.0 * log10((double)clev), -3.0103);
  }
  expect_clean();
}
