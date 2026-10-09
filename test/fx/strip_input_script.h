// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The control script the trim face's oracles and its golden digests drive, block by block: the
 * words of omx-strip's input stage (trim, then the HPF and the LPF) and the host's bypass. One
 * script, read by test/fx/trim_instance.test.c (arm C), test/fx/trim_instance_golden.test.c (the
 * faces) and tools/strip-input-identity.c (omx-plugins' omx_strip.h itself), so the three render the
 * same thing. Every value is inside its declared travel: the strip clamps with omx_clampf, the face
 * with omx_clamp_or, and the two agree on every finite word.
 */
#ifndef OMXDSP_TEST_FX_STRIP_INPUT_SCRIPT_H
#define OMXDSP_TEST_FX_STRIP_INPUT_SCRIPT_H

/** The frames of one block of the script. */
#define STRIP_INPUT_BLOCK 128

struct strip_input_words {
  float trim_db;
  int hpf_on;
  float hpf_freq;
  int hpf_slope; /* 12 or 24, dB/oct: the FILTER_SLOPES id and the strip's slope word alike */
  int lpf_on;
  float lpf_freq;
  int lpf_slope;
  int bypass; /* the host's */
};

/** The words for block `b`: the trim walks the whole travel in uneven steps (its ends and 0 dB
 * included) and holds for a few blocks at a time; each pass filter goes on, moves, changes slope and
 * goes off; the host bypasses blocks 40 to 47 (the ramp holds) and 90 to 91. */
static inline struct strip_input_words strip_input_words_at(int b) {
  static const float TRIM[] = {0.0f, 6.0f, 6.0f, -24.0f, 24.0f, 24.0f, 0.0f, -3.7f, 11.1f, -0.1f, 0.1f, -12.5f, 3.0f};
  struct strip_input_words w;
  w.trim_db = TRIM[(b / 3) % (int)(sizeof TRIM / sizeof TRIM[0])];
  w.hpf_on = (b / 8) % 3 != 0;
  w.hpf_freq = 20.0f + 70.0f * (float)((b / 5) % 15); /* 20 .. 1000 Hz */
  w.hpf_slope = (b / 16) % 2 ? 24 : 12;
  w.lpf_on = (b / 6) % 4 != 1;
  w.lpf_freq = 1000.0f + 1900.0f * (float)((b / 7) % 11); /* 1 .. 20 kHz */
  w.lpf_slope = (b / 12) % 2 ? 12 : 24;
  w.bypass = (b >= 40 && b < 48) || (b >= 90 && b < 92);
  return w;
}

#endif
