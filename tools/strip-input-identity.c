// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * strip-input-identity.c — omx-plugins' plugins/omx-strip/omx_strip.h, the file itself, over the
 * golden stimulus and test/fx/strip_input_script.h: every parameter at its declared default but the
 * script's trim and pass-filter words, the gate, the EQ and the compressor off, order 0, the host's
 * bypass the script's. Its digests are the ones test/fx/trim_instance_golden.test.c renders through
 * the trim and eq faces, so the faces are the strip's input stage bit for bit or the comparison
 * says which rate moved. Built and run by tools/strip-input-identity.sh against an omx-plugins
 * checkout; a desk check like tools/engine-identity.sh.
 */
#include <stdint.h>

#include "omx_strip.h"

#include "golden.h"
#include "strip_input_script.h"

#define BLOCK STRIP_INPUT_BLOCK

static void render(float sr, float *out) {
  static OmxStrip s;
  float v[OMX_STRIP_PARAM_COUNT];
  for (int k = 0; k < OMX_STRIP_PARAM_COUNT; k++) v[k] = OMX_STRIP_PARAMS[k].def;
  v[OMX_STRIP_PARAM_GATE_ON] = 0.0f, v[OMX_STRIP_PARAM_EQ_ON] = 0.0f, v[OMX_STRIP_PARAM_COMP_ON] = 0.0f;
  v[OMX_STRIP_PARAM_ORDER] = 0.0f;
  omx_strip_init(&s, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK], ol[BLOCK], orr[BLOCK];
  for (int o = 0; o < OMX_FX_GOLDEN_FRAMES; o += BLOCK) {
    const struct strip_input_words w = strip_input_words_at(o / BLOCK);
    v[OMX_STRIP_PARAM_TRIM_DB] = w.trim_db;
    v[OMX_STRIP_PARAM_HPF_ON] = (float)w.hpf_on, v[OMX_STRIP_PARAM_HPF_FREQ] = w.hpf_freq;
    v[OMX_STRIP_PARAM_HPF_SLOPE] = (float)w.hpf_slope;
    v[OMX_STRIP_PARAM_LPF_ON] = (float)w.lpf_on, v[OMX_STRIP_PARAM_LPF_FREQ] = w.lpf_freq;
    v[OMX_STRIP_PARAM_LPF_SLOPE] = (float)w.lpf_slope;
    omx_strip_resolve(&s, w.bypass, v);
    for (int i = 0; i < BLOCK; i++) omx_fx_golden_stimulus(o + i, &lcg, &l[i], &r[i]);
    omx_strip_run(&s, l, r, ol, orr, BLOCK);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = ol[i]; out[2 * (o + i) + 1] = orr[i]; }
  }
}

int main(int argc, char **argv) { return omx_fx_golden_main(argc, argv, "trim_instance", render); }
