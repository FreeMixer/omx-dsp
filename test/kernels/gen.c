// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/omx_rta.h>

#include "../fx/fx_rates.h"

/* ---- the test-signal generators --------------------------------------------------------------
 *
 * The engine's oracle (openmixer mix_dsp.test.c, the generator tests), moved with the generators:
 * the waveforms at their cardinal phases, a bin-aligned sine through the analyser, the level ramp,
 * phase continuity across blocks, white noise bounded, zero-mean and replayable, pink noise
 * bounded and tilted, and the frequency guard. The sine, continuity, guard and noise claims then
 * run again at each of the nine RME rates. */

#define GEN_FFT 8192u

static float gen_block[65536];
static float gen_work_re[GEN_FFT], gen_work_im[GEN_FFT], gen_mags[GEN_FFT / 2u];

static void gen_close_to(float got, float want, const char *what) {
  ok(fabsf(got - want) <= 1e-5f, what, got, want);
}

static void gen_osc_waveforms(void) {
  gen_close_to(omx_gen_osc(OMX_WAVE_SINE, 0.0f), 0.0f, "sine phase 0 = 0");
  gen_close_to(omx_gen_osc(OMX_WAVE_SINE, 0.25f), 1.0f, "sine phase 1/4 = +1");
  gen_close_to(omx_gen_osc(OMX_WAVE_SINE, 0.75f), -1.0f, "sine phase 3/4 = -1");
  gen_close_to(omx_gen_osc(OMX_WAVE_SQUARE, 0.1f), 1.0f, "square first half = +1");
  gen_close_to(omx_gen_osc(OMX_WAVE_SQUARE, 0.6f), -1.0f, "square second half = -1");
  gen_close_to(omx_gen_osc(OMX_WAVE_SAW, 0.0f), -1.0f, "saw phase 0 = -1");
  gen_close_to(omx_gen_osc(OMX_WAVE_SAW, 0.5f), 0.0f, "saw phase 1/2 = 0");
  gen_close_to(omx_gen_osc(OMX_WAVE_SAW, 0.75f), 0.5f, "saw phase 3/4 = +1/2");
  gen_close_to(omx_gen_osc(OMX_WAVE_TRIANGLE, 0.0f), 0.0f, "triangle phase 0 = 0");
  gen_close_to(omx_gen_osc(OMX_WAVE_TRIANGLE, 0.25f), 1.0f, "triangle phase 1/4 = +1");
  gen_close_to(omx_gen_osc(OMX_WAVE_TRIANGLE, 0.5f), 0.0f, "triangle phase 1/2 = 0");
  gen_close_to(omx_gen_osc(OMX_WAVE_TRIANGLE, 0.75f), -1.0f, "triangle phase 3/4 = -1");
  ok(OMX_GEN_OSC == 0 && OMX_GEN_WHITE == 1 && OMX_GEN_PINK == 2, "the kinds are GENERATOR_KINDS' positions", OMX_GEN_PINK, 2.0);
  ok(OMX_WAVE_SINE == 0 && OMX_WAVE_SQUARE == 1 && OMX_WAVE_SAW == 2 && OMX_WAVE_TRIANGLE == 3,
     "the waveforms are WAVEFORMS' positions", OMX_WAVE_TRIANGLE, 3.0);
}

/* A bin-aligned sine out of omx_gen_fill reads back at its own bin near unity. */
static void gen_sine_spectrum(float sr, uint32_t k0) {
  const float f0 = (float)k0 * sr / (float)GEN_FFT;
  struct omx_gen_state st;
  omx_gen_init(&st, 1u);
  st.cur_level = 1.0f; /* skip the fade-in so the whole block is steady-state */
  omx_gen_fill(&st, gen_block, GEN_FFT, OMX_GEN_OSC, OMX_WAVE_SINE, f0, 1.0f, sr);
  omx_rta_spectrum(gen_block, GEN_FFT, gen_work_re, gen_work_im, gen_mags);
  uint32_t peak = 1;
  for (uint32_t k = 1; k < GEN_FFT / 2u; k++)
    if (gen_mags[k] > gen_mags[peak]) peak = k;
  ok(peak == k0, "gen sine: a bin-aligned tone peaks at exactly its own FFT bin", peak, k0);
  ok(gen_mags[k0] > 0.9f && gen_mags[k0] < 1.1f, "gen sine: a full-level tone reads near unity magnitude", gen_mags[k0], 1.0);
}

/* A fresh generator fades in from 0 across its first block, and a level change ramps from the
 * previously applied level. A 10 Hz square is +1 for its first half-cycle, so out[i] IS the ramp. */
static void gen_level_ramp(void) {
  const float sr = 48000.0f;
  struct omx_gen_state st;
  omx_gen_init(&st, 1u);
  ok(st.cur_level == 0.0f, "gen init: the level ramp starts at 0", st.cur_level, 0.0);
  float block[256];
  omx_gen_fill(&st, block, 256, OMX_GEN_OSC, OMX_WAVE_SQUARE, 10.0f, 1.0f, sr);
  ok(fabsf(block[0]) < 0.02f, "gen ramp: the first sample of a fresh generator is ~0", block[0], 0.02);
  ok(block[255] > 0.98f, "gen ramp: the last sample of the first block reaches the target", block[255], 0.98);
  gen_close_to(st.cur_level, 1.0f, "gen ramp: cur_level lands exactly on the target");
  omx_gen_fill(&st, block, 256, OMX_GEN_OSC, OMX_WAVE_SQUARE, 10.0f, 0.25f, sr);
  ok(block[0] > 0.95f, "gen ramp: a level change starts from the previously applied level", block[0], 0.95);
  gen_close_to(st.cur_level, 0.25f, "gen ramp: cur_level follows the new target");
}

/* Two consecutive fills are one waveform: the step across the boundary is within the sine's own
 * steepest per-sample slope, 2πf/sr. */
static void gen_phase_continuity(float sr) {
  const float f = 1000.0f;
  struct omx_gen_state st;
  omx_gen_init(&st, 1u);
  st.cur_level = 1.0f;
  float a[128], b[128];
  omx_gen_fill(&st, a, 128, OMX_GEN_OSC, OMX_WAVE_SINE, f, 1.0f, sr);
  omx_gen_fill(&st, b, 128, OMX_GEN_OSC, OMX_WAVE_SINE, f, 1.0f, sr);
  const float max_step = 2.0f * (float)M_PI * f / sr;
  ok(fabsf(b[0] - a[127]) <= max_step * 1.01f, "gen phase: the waveform is continuous across a block boundary",
     fabsf(b[0] - a[127]), max_step);
}

static void gen_white(float sr) {
  struct omx_gen_state st;
  omx_gen_init(&st, 42u);
  st.cur_level = 0.5f;
  omx_gen_fill(&st, gen_block, 65536, OMX_GEN_WHITE, 0, 0.0f, 0.5f, sr);
  float peak = 0.0f;
  double sum = 0.0;
  for (uint32_t i = 0; i < 65536; i++) {
    const float a = fabsf(gen_block[i]);
    if (a > peak) peak = a;
    sum += (double)gen_block[i];
  }
  ok(peak <= 0.5f, "gen white: never exceeds the level", peak, 0.5);
  ok(peak > 0.45f, "gen white: reaches near the level", peak, 0.45);
  ok(fabs(sum / 65536.0) < 0.02, "gen white: zero-mean over a long run", sum / 65536.0, 0.02);
  struct omx_gen_state st2;
  omx_gen_init(&st2, 42u);
  st2.cur_level = 0.5f;
  float first[8];
  omx_gen_fill(&st2, first, 8, OMX_GEN_WHITE, 0, 0.0f, 0.5f, sr);
  ok(first[0] == gen_block[0] && first[7] == gen_block[7], "gen white: the same seed replays the same sequence", first[0], gen_block[0]);
}

/* Pink: bounded at full level, and between bin ~16 and bin ~1024 ideal pink drops 8x in amplitude,
 * so the low band carries clearly more than the high. */
static void gen_pink(float sr) {
  struct omx_gen_state st;
  omx_gen_init(&st, 7u);
  st.cur_level = 1.0f;
  omx_gen_fill(&st, gen_block, GEN_FFT, OMX_GEN_PINK, 0, 0.0f, 1.0f, sr);
  float peak = 0.0f;
  for (uint32_t i = 0; i < GEN_FFT; i++)
    if (fabsf(gen_block[i]) > peak) peak = fabsf(gen_block[i]);
  ok(peak <= 1.0f, "gen pink: never exceeds the level", peak, 1.0);
  ok(peak > 0.05f, "gen pink: carries real signal", peak, 0.05);
  omx_rta_spectrum(gen_block, GEN_FFT, gen_work_re, gen_work_im, gen_mags);
  double low = 0.0, high = 0.0;
  for (uint32_t k = 8; k < 32; k++) low += (double)gen_mags[k];
  low /= 24.0;
  for (uint32_t k = 1024; k < 2048; k++) high += (double)gen_mags[k];
  high /= 1024.0;
  ok(low > 2.0 * high, "gen pink: the low band carries clearly more than the high (the tilt)", low, 2.0 * high);
}

/* A frequency at or above Nyquist does not wrap the increment into garbage. */
static void gen_freq_guard(float sr) {
  struct omx_gen_state st;
  omx_gen_init(&st, 1u);
  st.cur_level = 1.0f;
  float block[256];
  omx_gen_fill(&st, block, 256, OMX_GEN_OSC, OMX_WAVE_SINE, 2.0f * sr, 1.0f, sr);
  float peak = 0.0f;
  for (uint32_t i = 0; i < 256; i++)
    if (fabsf(block[i]) > peak) peak = fabsf(block[i]);
  ok(peak <= 1.0f, "gen freq guard: an above-Nyquist frequency stays bounded", peak, 1.0);
  ok(st.phase >= 0.0f && st.phase < 1.0f, "gen freq guard: the phase stays in [0,1)", st.phase, 1.0);
}

static void arm_gen(void) {
  g_arm = "gen";
  gen_osc_waveforms();
  gen_sine_spectrum(48000.0f, 32u); /* 187.5 Hz */
  gen_level_ramp();
  gen_phase_continuity(48000.0f);
  gen_white(48000.0f);
  gen_pink(48000.0f);
  gen_freq_guard(48000.0f);
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    gen_sine_spectrum(sr, 171u);
    gen_phase_continuity(sr);
    gen_white(sr);
    gen_pink(sr);
    gen_freq_guard(sr);
  }
  expect_clean();
}
