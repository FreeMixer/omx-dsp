// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The drive's golden digests (golden.h): the stimulus through the tape curve on the tilt band at
 * 2 kHz, drive +18 dB, character +0.4, mix 0.8, trim -3 dB, measured auto-gain, stereo-linked,
 * the 12 kHz roll-off on, at 4x, with the four sections from omx_drive_design_bank at each rate,
 * in 128-frame blocks — the design and the kernel held together.
 *
 *   make test-fx                      compare
 *   build/fx_drive_golden --write     print the lines test/golden/drive.sha256 holds
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/fx/omx_drive_design.h>

#include "golden.h"

#define BLOCK 128

static void render(float sr, float *out) {
  struct omx_drive p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.curve = OMX_DRIVE_TAPE;
  p.band = OMX_DRIVE_BAND_TILT;
  p.drive_lin = powf(10.0f, 18.0f / 20.0f);
  p.even_w = (0.4f + 1.0f) / 2.0f;
  p.mix = 0.8f;
  p.trim_lin = powf(10.0f, -3.0f / 20.0f);
  p.auto_gain = 1;
  p.stereo_link = 1;
  p.hf_on = 1;
  p.os_factor = 4;
  omx_drive_design_bank(&p, 2000.0f, 12000.0f, (uint32_t)sr);
  omx_drive_time_constants(&p, sr);
  struct omx_drive_state *st = calloc(1, sizeof(*st));
  omx_drive_state_init(st, omx_drive_factor_of(p.os_factor));
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    for (int i = 0; i < BLOCK; i++) {
      omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
      l[i] *= 2.0f;
      r[i] *= 2.0f;
    }
    omx_drive_process(l, r, BLOCK, &p, st);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
  free(st);
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "drive", render); }
