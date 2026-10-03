// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The golden-digest runner every kernel's <kernel>_golden.test.c calls: one SHA-256 per declared
 * rate over the kernel's interleaved stereo output for a fixed stimulus and parameter set,
 * compared with test/golden/<kernel>.sha256, or printed as those lines with --write. A patch
 * release may not move one; a release that does is a minor and names the kernel.
 */
#ifndef OMXDSP_TEST_FX_GOLDEN_H
#define OMXDSP_TEST_FX_GOLDEN_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fx_rates.h"
#include "sha256.h"

/** Frames each digest covers; `render` writes 2 * OMX_FX_GOLDEN_FRAMES interleaved floats. */
#define OMX_FX_GOLDEN_FRAMES 16384

/** One step of the stimulus's generator: the LCG every golden render draws its noise from, for a
 * kernel whose stimulus keeps the noise on for a different span than omx_fx_golden_stimulus. */
static inline float omx_fx_golden_noise(uint32_t *lcg) {
  *lcg = *lcg * 1664525u + 1013904223u;
  return (float)(int32_t)(*lcg >> 8) / 16777216.0f - 0.25f;
}

/** The stimulus's next sample pair: an impulse on L at frame 0, then integer-generated noise for
 * the first half (L at half scale, R inverted at quarter scale), then silence, so no libm call
 * shapes the input. `lcg` is the caller's generator word, seeded 0x1234567. */
static inline void omx_fx_golden_stimulus(int frame, uint32_t *lcg, float *l, float *r) {
  float noise = omx_fx_golden_noise(lcg);
  *l = (frame == 0) ? 1.0f : (frame < OMX_FX_GOLDEN_FRAMES / 2 ? noise * 0.5f : 0.0f);
  *r = (frame == 0) ? 0.0f : (frame < OMX_FX_GOLDEN_FRAMES / 2 ? -noise * 0.25f : 0.0f);
}

/** A render whose output length depends on the rate: it fills `out`, sets `*bytes` to the bytes the
 * digest covers and returns how many of its own checks failed (counted with the moved digests). */
typedef int (*omx_fx_golden_render_sized_fn)(float sr, float *out, size_t *bytes);

/** The driver: `--write` prints `<rate> <sha256>` per declared rate, otherwise compares with the
 * digest file (argv[1], default test/golden/<name>.sha256). `out` is the caller's buffer, large
 * enough for the longest render. Exit 0 green, 1 a digest moved or is missing or a render check
 * failed, 2 the measurement could not be taken. */
static inline int omx_fx_golden_main_sized(int argc, char **argv, const char *name,
                                           omx_fx_golden_render_sized_fn render, float *out) {
  omx_fx_require_rate_floor();
  char probe[65];
  omx_sha256_hex("abc", 3, probe);
  if (strcmp(probe, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
    fprintf(stderr, "fx/%s_golden: the SHA-256 self-test failed (%s)\n", name, probe);
    return 2;
  }
  int write = argc > 1 && strcmp(argv[1], "--write") == 0;
  char fallback[256];
  snprintf(fallback, sizeof fallback, "test/golden/%s.sha256", name);
  const char *path = argc > 1 && !write ? argv[1] : fallback;
  int fail = 0, checked = 0;
  FILE *f = write ? NULL : fopen(path, "r");
  if (!write && !f) { fprintf(stderr, "fx/%s_golden: cannot read %s\n", name, path); return 2; }
  for (int k = 0; k < (int)OMX_DECLARED_RATE_COUNT; k++) {
    char hex[65];
    size_t bytes = 0;
    fail += render(OMX_DECLARED_RATES[k], out, &bytes);
    omx_sha256_hex(out, bytes, hex);
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
  if (!write) printf("fx/%s_golden: %d rates, %d moved\n", name, checked, fail);
  return fail == 0 ? 0 : 1;
}

/* The fixed-length render omx_fx_golden_main adapts to the sized driver; one program, one kernel. */
static void (*omx_fx_golden_fixed_render)(float sr, float *out);

static inline int omx_fx_golden_fixed(float sr, float *out, size_t *bytes) {
  *bytes = 2u * OMX_FX_GOLDEN_FRAMES * sizeof(float);
  memset(out, 0, *bytes);
  omx_fx_golden_fixed_render(sr, out);
  return 0;
}

/** The driver for a render of 2 * OMX_FX_GOLDEN_FRAMES interleaved floats at every rate. */
static inline int omx_fx_golden_main(int argc, char **argv, const char *name, void (*render)(float sr, float *out)) {
  static float out[2 * OMX_FX_GOLDEN_FRAMES];
  omx_fx_golden_fixed_render = render;
  return omx_fx_golden_main_sized(argc, argv, name, omx_fx_golden_fixed, out);
}

#endif
