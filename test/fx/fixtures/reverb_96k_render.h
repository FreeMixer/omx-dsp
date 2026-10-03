// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * reverb_96k_render.h — the excitation and the hash the 96 kHz reference is cut with.
 *
 * ONE definition, included by BOTH sides: OpenMixer's `packages/pipewire-native/tools/reverb-96k-reference.c`, which renders the
 * fixture, and `test/fx/reverb_math.test.c`, which re-renders and compares it. A second copy of the
 * excitation is a fixture that can go stale without a word — the responses would differ because
 * the INPUT differed, and the oracle would report a kernel change that never happened.
 *
 * Neither RT code nor a stage: it is test scaffolding, and lives under test/fixtures with the
 * fixture it defines.
 */
#ifndef OMX_REVERB_96K_RENDER_H
#define OMX_REVERB_96K_RENDER_H

#include <stdint.h>
#include <string.h>

/* An impulse, then a 5 ms burst loud enough to arm GATED's -40 dB key, then silence for the
 * tail: an impulse alone leaves a Schroeder network answering in sparse pulses, so most decimated
 * taps would be exactly zero and could not localise anything. */
static void omx_ref96_excite(float *l, float *r, int n) {
  memset(l, 0, (size_t)n * sizeof *l);
  memset(r, 0, (size_t)n * sizeof *r);
  l[0] = 1.0f; r[0] = 1.0f;
  uint32_t rng = 20260917u;
  for (int i = 960; i < 960 + 480 && i < n; i++) {
    rng = rng * 1103515245u + 12345u;
    float v = 0.5f * (((float)((rng >> 9) & 0x7fffff) / 4194304.0f) - 1.0f);
    l[i] = v; r[i] = v;
  }
}

static uint32_t omx_ref96_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }

/* FNV-1a over the sample BIT PATTERNS — the bit-equality claim over the whole response, where the
 * decimated taps only sample it. */
static uint64_t omx_ref96_hash(const float *x, int n) {
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < n; i++) {
    uint32_t u = omx_ref96_bits(x[i]);
    for (int b = 0; b < 4; b++) {
      h ^= (uint64_t)((u >> (8 * b)) & 0xffu);
      h *= 1099511628211ull;
    }
  }
  return h;
}

#endif /* OMX_REVERB_96K_RENDER_H */
