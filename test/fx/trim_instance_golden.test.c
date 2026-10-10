// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The trim face's golden digests (golden.h): the stimulus through the trim face then the eq face
 * with every band off — omx-strip's input stage — driven by test/fx/strip_input_script.h, block by
 * block, host bypass spans included, with separate in/out buffers. The digests were written by
 * tools/strip-input-identity.sh from omx-plugins' plugins/omx-strip/omx_strip.h itself (its gate,
 * EQ and compressor off, order 0) over the same script: a face that is not the strip's input stage
 * bit for bit moves them.
 *
 *   make test-fx                                compare
 *   build/fx_trim_instance_golden --write       print the faces' digests
 */
#include <stdint.h>

#include <omxdsp/fx/omx_eq_instance.h>
#include <omxdsp/fx/omx_trim_instance.h>

#include "golden.h"
#include "strip_input_script.h"

#define BLOCK STRIP_INPUT_BLOCK

static void render(float sr, float *out) {
  OmxTrimInstance trim;
  OmxEqInstance eq;
  omx_trim_instance_init(&trim, sr);
  omx_eq_instance_init(&eq, sr);
  int type[OMX_EQ_INSTANCE_BANDS], on[OMX_EQ_INSTANCE_BANDS];
  float freq[OMX_EQ_INSTANCE_BANDS], gain[OMX_EQ_INSTANCE_BANDS], q[OMX_EQ_INSTANCE_BANDS];
  for (int i = 0; i < OMX_EQ_INSTANCE_BANDS; i++)
    type[i] = (int)OMX_EQ_BAND_TYPES_DEFAULT, freq[i] = omx_eq_lv2_band_freq_default((uint32_t)i), gain[i] = 0.0f,
    q[i] = OMX_EQ_LV2_BAND_Q_DEFAULT, on[i] = 0;
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const struct strip_input_words w = strip_input_words_at(o / BLOCK);
    omx_trim_instance_resolve(&trim, w.bypass, w.trim_db);
    omx_eq_instance_resolve(&eq, w.bypass, w.hpf_on, w.hpf_freq, w.hpf_slope, w.lpf_on, w.lpf_freq, w.lpf_slope, type,
                            freq, gain, q, on);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_trim_instance_run(&trim, l, r, ol, orr, BLOCK);
    omx_eq_instance_run(&eq, ol, orr, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "trim_instance", render); }
