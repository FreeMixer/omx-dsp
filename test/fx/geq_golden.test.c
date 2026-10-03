// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The graphic EQ's golden digests: one SHA-256 per declared rate over the kernel's output for a
 * fixed stimulus and a fixed fader set, compared with test/golden/geq.sha256. A patch release may
 * not move one; a release that does is a minor and names the kernel.
 *
 *   make test-fx                    compare
 *   build/fx_geq_golden --write     print the lines test/golden/geq.sha256 holds
 *
 * The stimulus is an impulse, then integer-generated noise, so no libm call shapes the input. The
 * sections are designed by omx_eq_design at the ISO third-octave centres and Q; every third band
 * sits at exactly 0 dB, so the skip-and-prime path runs, and the fader set changes halfway, so an
 * engage edge on a skipped section is in the digest.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_eq_design.h>
#include <omxdsp/fx/omx_geq.h>

#include "fx_rates.h"
#include "sha256.h"

#define FRAMES 16384
#define BLOCK 128

static void design(struct omx_geq *p, int second, double sr) {
  double c[OMX_GEQ_BANDS][5];
  float g[OMX_GEQ_BANDS];
  const double q = pow(2.0, 1.0 / 6.0) / (pow(2.0, 1.0 / 3.0) - 1.0);
  for (int k = 0; k < OMX_GEQ_BANDS; k++) {
    g[k] = (k % 3 == (second ? 1 : 0)) ? 0.0f : 0.5f * (float)((k * 7 + (second ? 11 : 3)) % 61) - 15.0f;
    omx_eq_design(OMX_EQ_PEAKING, 1000.0 * pow(2.0, (double)(k - 17) / 3.0), q, g[k], sr, c[k]);
  }
  omx_geq_set(p, 1, (const double(*)[5])c, g);
}

static void render(float sr, float *out) {
  static struct omx_geq p;
  static struct omx_geq_state s;
  omx_geq_state_init(&s);
  design(&p, 0, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    if (o == FRAMES / 2) design(&p, 1, sr);
    for (int i = 0; i < BLOCK; i++) {
      lcg = lcg * 1664525u + 1013904223u;
      float noise = (float)(int32_t)(lcg >> 8) / 16777216.0f - 0.25f;
      l[i] = (o + i == 0) ? 1.0f : (o + i < 3 * FRAMES / 4 ? noise * 0.5f : 0.0f);
      r[i] = (o + i == 0) ? 0.0f : (o + i < 3 * FRAMES / 4 ? -noise * 0.25f : 0.0f);
    }
    omx_geq_process(l, r, BLOCK, &p, &s);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
}

int main(int argc, char **argv) {
  omx_fx_require_rate_floor();
  char probe[65];
  omx_sha256_hex("abc", 3, probe);
  if (strcmp(probe, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
    fprintf(stderr, "fx/geq_golden: the SHA-256 self-test failed (%s)\n", probe);
    return 2;
  }
  int write = argc > 1 && strcmp(argv[1], "--write") == 0;
  const char *path = argc > 1 && !write ? argv[1] : "test/golden/geq.sha256";
  static float out[2 * FRAMES];
  int fail = 0, checked = 0;
  FILE *f = write ? NULL : fopen(path, "r");
  if (!write && !f) { fprintf(stderr, "fx/geq_golden: cannot read %s\n", path); return 2; }
  for (int k = 0; k < (int)OMX_DECLARED_RATE_COUNT; k++) {
    char hex[65];
    render(OMX_DECLARED_RATES[k], out);
    omx_sha256_hex(out, sizeof out, hex);
    if (write) { printf("%.0f %s\n", (double)OMX_DECLARED_RATES[k], hex); continue; }
    double rate = 0.0;
    char want[65] = {0};
    rewind(f);
    int found = 0;
    while (fscanf(f, "%lf %64s", &rate, want) == 2)
      if (rate == (double)OMX_DECLARED_RATES[k]) { found = 1; break; }
    checked++;
    if (!found) { fprintf(stderr, "FAIL: %.0f Hz has no golden digest\n", (double)OMX_DECLARED_RATES[k]); fail++; }
    else if (strcmp(want, hex) != 0) {
      fprintf(stderr, "FAIL: %.0f Hz output moved: %s, golden %s\n", (double)OMX_DECLARED_RATES[k], hex, want);
      fail++;
    }
  }
  if (f) fclose(f);
  if (!write) printf("fx/geq_golden: %d rates, %d moved\n", checked, fail);
  return fail == 0 ? 0 : 1;
}
