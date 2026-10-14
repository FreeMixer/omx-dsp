// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * eq_bandwidth_q_table.c — omx_eq_bandwidth_q(), the one bandwidth-to-Q law, as a table
 * (FreeMixer/omx-dsp#41): every row `[sample_rate, freq_hz, bw_oct, q]` at the nine RME rates,
 * centres from below the clamp's floor to past Nyquist, widths from a tenth of an octave to four.
 * openmixer's TypeScript copy (core's bandwidthOctavesQ) is held to it until it is deleted
 * (openmixer #1601); tools/ts-golden.mjs bandwidth-q runs that check from here.
 *
 * Prints the JSON; `make bandwidth-q-table` holds test/golden/eq_bandwidth_q.json equal to it, and
 * `make golden-write` rewrites it. %.17g reads back as the same double in C and in JavaScript.
 */
#include <omxdsp/omxdsp.h>

#include <stdio.h>

int main(void) {
  static const float rates[OMX_RME_RATES_COUNT] = OMX_RME_RATES_INIT;
  static const double freqs[] = {0.5, 20.0, 100.0, 1000.0, 4000.0, 8000.0, 12000.0, 15000.0, 20000.0, 30000.0, 96000.0};
  static const double widths[] = {0.1, 0.5, 1.0, 2.0, 4.0};
  const char *sep = "";
  printf("{\n  \"law\": \"omx_eq_bandwidth_q\",\n  \"source\": \"include/omxdsp/omx_eq_design.h\",\n");
  printf("  \"columns\": [\"sampleRate\", \"freqHz\", \"bwOct\", \"q\"],\n  \"rows\": [");
  for (unsigned r = 0; r < OMX_RME_RATES_COUNT; r++)
    for (unsigned f = 0; f < sizeof freqs / sizeof freqs[0]; f++)
      for (unsigned w = 0; w < sizeof widths / sizeof widths[0]; w++) {
        const double sr = rates[r], q = omx_eq_bandwidth_q(freqs[f], widths[w], sr);
        printf("%s\n    [%.17g, %.17g, %.17g, %.17g]", sep, sr, freqs[f], widths[w], q);
        sep = ",";
      }
  printf("\n  ]\n}\n");
  return 0;
}
