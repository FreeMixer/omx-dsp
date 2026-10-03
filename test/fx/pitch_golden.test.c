// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The pitch shifter's golden digests: one SHA-256 per declared rate over the kernel's output for a
 * fixed stimulus and a fixed fader set, compared with test/golden/pitch.sha256. A patch release may
 * not move one; a release that does is a minor and names the kernel.
 *
 *   make test-fx                    compare
 *   build/fx_pitch_golden --write   print the lines test/golden/pitch.sha256 holds
 *
 * The stimulus is an impulse, then integer-generated noise, so no libm call shapes the input. The
 * shifter runs at −5 semitones +12 cents, 60 % wet, and turns to +7 semitones, 100 % wet,
 * halfway, so the direction hand-over is in the digest.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_pitch.h>

#include "fx_rates.h"
#include "sha256.h"

#define FRAMES 16384
#define BLOCK 128

static void render(float sr, float *out) {
  const uint32_t cap = omx_pitch_cap_for(sr);
  float *ring = calloc(2u * cap, sizeof(float));
  if (ring == NULL) abort();
  struct omx_pitch p;
  struct omx_pitch_state s;
  if (omx_pitch_state_init(&s, ring, ring + cap, cap) != OMX_FDELAY_OK) abort();
  omx_pitch_resolve(&p, 1, -5.0f, 12.0f, 60.0f, sr);
  uint32_t lcg = 0x1234567u;
  float l[BLOCK], r[BLOCK];
  for (int o = 0; o < FRAMES; o += BLOCK) {
    if (o == FRAMES / 2) omx_pitch_resolve(&p, 1, 7.0f, 0.0f, 100.0f, sr);
    for (int i = 0; i < BLOCK; i++) {
      lcg = lcg * 1664525u + 1013904223u;
      float noise = (float)(int32_t)(lcg >> 8) / 16777216.0f - 0.25f;
      l[i] = (o + i == 0) ? 1.0f : (o + i < 3 * FRAMES / 4 ? noise * 0.5f : 0.0f);
      r[i] = (o + i == 0) ? 0.0f : (o + i < 3 * FRAMES / 4 ? -noise * 0.25f : 0.0f);
    }
    omx_pitch_process(l, r, BLOCK, &p, &s);
    for (int i = 0; i < BLOCK; i++) { out[2 * (o + i)] = l[i]; out[2 * (o + i) + 1] = r[i]; }
  }
  free(ring);
}

int main(int argc, char **argv) {
  omx_fx_require_rate_floor();
  char probe[65];
  omx_sha256_hex("abc", 3, probe);
  if (strcmp(probe, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
    fprintf(stderr, "fx/pitch_golden: the SHA-256 self-test failed (%s)\n", probe);
    return 2;
  }
  int write = argc > 1 && strcmp(argv[1], "--write") == 0;
  const char *path = argc > 1 && !write ? argv[1] : "test/golden/pitch.sha256";
  static float out[2 * FRAMES];
  int fail = 0, checked = 0;
  FILE *f = write ? NULL : fopen(path, "r");
  if (!write && !f) { fprintf(stderr, "fx/pitch_golden: cannot read %s\n", path); return 2; }
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
  if (!write) printf("fx/pitch_golden: %d rates, %d moved\n", checked, fail);
  return fail == 0 ? 0 : 1;
}
