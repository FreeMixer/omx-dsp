// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * fastmath_drive_render.c — the drive stage's output for all four curves at 1x and 4x, at every declared
 * rate, written raw (`--dump FILE`) for tools/fastmath-diff.sh to set a libmvec build against the libm one.
 * Same stimulus and parameters as drive_golden.test.c (tilt band, +18 dB, character +0.4, mix 0.8, auto-gain,
 * linked, roll-off), the curve and the oversampling factor varying. It carries no digest.
 */
#define OMX_FX_GOLDEN_DUMP_ONLY 1
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/fx/omx_drive_design.h>

#include "golden.h"

#define BLOCK 128
#define CONFIGS 8

static int render(float sr, float *out, size_t *bytes) {
  static const int curves[4] = {OMX_DRIVE_SOFT, OMX_DRIVE_EXCITER, OMX_DRIVE_TUBE, OMX_DRIVE_TAPE};
  static const int factors[2] = {1, 4};
  for (int c = 0; c < CONFIGS; c++) {
    struct omx_drive p;
    memset(&p, 0, sizeof p);
    p.enabled = 1;
    p.curve = curves[c >> 1];
    p.band = OMX_DRIVE_BAND_TILT;
    p.drive_lin = powf(10.0f, 18.0f / 20.0f);
    p.even_w = (0.4f + 1.0f) / 2.0f;
    p.mix = 0.8f;
    p.trim_lin = powf(10.0f, -3.0f / 20.0f);
    p.auto_gain = 1;
    p.stereo_link = 1;
    p.hf_on = 1;
    p.os_factor = factors[c & 1];
    omx_drive_design_bank(&p, 2000.0f, 12000.0f, (uint32_t)sr);
    omx_drive_time_constants(&p, sr);
    struct omx_drive_state *st = calloc(1, sizeof(*st));
    omx_drive_state_init(st, omx_drive_factor_of(p.os_factor));
    uint32_t lcg = 0x1234567u;
    float l[BLOCK], r[BLOCK];
    float *o = out + (size_t)c * 2 * OMX_FX_GOLDEN_FRAMES;
    for (int f = 0; f < OMX_FX_GOLDEN_FRAMES; f += BLOCK) {
      for (int i = 0; i < BLOCK; i++) {
        omx_fx_golden_stimulus(f + i, &lcg, &l[i], &r[i]);
        l[i] *= 2.0f;
        r[i] *= 2.0f;
      }
      omx_drive_process(l, r, BLOCK, &p, st);
      for (int i = 0; i < BLOCK; i++) { o[2 * (f + i)] = l[i]; o[2 * (f + i) + 1] = r[i]; }
    }
    free(st);
  }
  *bytes = (size_t)CONFIGS * 2 * OMX_FX_GOLDEN_FRAMES * sizeof(float);
  return 0;
}

int main(int argc, char **argv) {
  static float out[(size_t)CONFIGS * 2 * OMX_FX_GOLDEN_FRAMES];
  return omx_fx_golden_main_sized(argc, argv, "fastmath_drive", render, out);
}
