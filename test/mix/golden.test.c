// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The golden digests of the mix module (include/omxdsp/mix/, FreeMixer/omx-dsp#98): hashed per
 * kernel and per rate at the nine RME rates and compared with test/golden/mix.sha256.
 *
 *   make test-mix                     compare
 *   build/mix_golden --write          print the lines the digest file holds
 *
 * The committed digests were written by this program compiled against the engine's own copies
 * (openmixer ae922d2e7d74, packages/pipewire-native/src: mix_dsp.h, mix_lcr.h, mix_bus.h) by
 * tools/mix-engine-identity.sh, so the moved code is held to the engine's output bit for bit
 * (openmixer docs/design/specs/2026-10-14-lane-output-path.md §6). -DOMX_GOLDEN_ENGINE is that
 * build: each kernel below names the library's call and the engine's.
 *
 * The stimulus makes no libm call (an integer generator, rational pan sweeps); the kernels call
 * cosf, sinf and sqrtf, the engine's own arithmetic.
 *
 *   pan      the stereo pan law over a pan sweep past both ends
 *   lcr      the LCR layout at four divergences over the same sweep
 *   strip    a strip's gains into a stereo and an LCR bus: mono and stereo strips, assigned or
 *            not, muted or not, at four levels
 *   fold     the mono fold's gain at levels and coefficients past the finite (NaN, ±inf)
 *   bus_out  a run of blocks through a stereo and a mono bus's out stage, the controls moving
 *            between blocks: the bus, the cursors and the measured peaks
 *   peak     the block peak over one, two and aliased legs, and the per-leg peaks
 *   clip     the sustained-over run on blocks with runs of 0 to 5 full-scale samples
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxcontract/omx_contract_limits.h>

#ifdef OMX_GOLDEN_ENGINE
#include "mix_bus.h"
#include "mix_dsp.h"
#include "mix_lcr.h"
#else
#include <omxdsp/mix/omx_bus_out.h>
#include <omxdsp/mix/omx_meter.h>
#include <omxdsp/mix/omx_mix_coef.h>
#include <omxdsp/mix/omx_pan_law.h>
#endif

#include "../fx/sha256.h"

static const float GOLDEN_RATES[OMX_RME_RATES_COUNT] = OMX_RME_RATES_INIT;

/* The centre window's half width: the console's limit in the engine, a layout parameter here. */
#define GOLDEN_CENTRE_HALF_WIDTH 0.5f

/* ---- the calls under test, the library's or the engine's ------------------------------------ */

static void k_pan(float pan, float *l, float *r) {
#ifdef OMX_GOLDEN_ENGINE
  omx_pan_law(pan, l, r);
#else
  omx_pan_stereo(pan, l, r);
#endif
}

static void k_lcr(float pan, float div, float g[3]) {
#ifdef OMX_GOLDEN_ENGINE
  omx_lcr_pan(pan, div, &g[0], &g[1], &g[2]);
#else
  struct omx_pan_layout l;
  omx_pan_layout_lcr(&l, GOLDEN_CENTRE_HALF_WIDTH);
  omx_pan_gains(&l, pan, div, g);
#endif
}

static void k_strip(int lcr, int channels, int active, float gain, float fader, int mute, float pan,
                    float div, float g[3]) {
  g[2] = 0.0f;
#ifdef OMX_GOLDEN_ENGINE
  if (lcr)
    omx_strip_main_coefs_lcr(channels, active, gain, fader, mute, pan, div, &g[0], &g[1], &g[2]);
  else
    omx_strip_main_coefs(channels, active, gain, fader, mute, pan, &g[0], &g[1]);
#else
  struct omx_pan_layout l;
  if (lcr) omx_pan_layout_lcr(&l, GOLDEN_CENTRE_HALF_WIDTH);
  else omx_pan_layout_stereo(&l);
  omx_mix_strip_gains(&l, channels, active, gain, fader, mute, pan, div, g);
#endif
}

/* The fold's gain. The engine has it only inside its mono-bus summing loop, so it is read out of
 * that loop: a unit left sample and a silent right one, a second block once the ramp has settled. */
static float k_fold(float level, float coef) {
#ifdef OMX_GOLDEN_ENGINE
  const float one = 1.0f, nothing = 0.0f;
  float bus = 0.0f, cl = 0.0f, cr = 0.0f;
  omx_bus_add_ramp(&one, &nothing, &bus, NULL, 1, level, 1.0f, 1.0f, &cl, &cr, OMX_BUS_FOLD, coef);
  bus = 0.0f;
  omx_bus_add_ramp(&one, &nothing, &bus, NULL, 1, level, 1.0f, 1.0f, &cl, &cr, OMX_BUS_FOLD, coef);
  return bus;
#else
  return omx_mix_fold_gain(level, coef);
#endif
}

static void k_bus_out(float *l, float *r, uint32_t n, int channels, float fader, int mute,
                      float bal, float *cl, float *cr, float *pl, float *pr) {
#ifdef OMX_GOLDEN_ENGINE
  _Atomic float sl = 0.0f, sr = 0.0f;
  omx_bus_out_stage(l, r, n, channels, fader, mute, bal, cl, cr, &sl, &sr);
  *pl = sl;
  *pr = sr;
#else
  omx_bus_out_run(l, r, n, channels, fader, mute, bal, cl, cr, pl, pr);
#endif
}

static float k_peak(const float *a, const float *b, uint32_t n) {
#ifdef OMX_GOLDEN_ENGINE
  return omx_block_peak(a, b, n);
#else
  return omx_meter_block_peak(a, b, n);
#endif
}

static void k_leg_peaks(const float *a, const float *b, uint32_t n, float cl, float cr, float *pl,
                        float *pr) {
#ifdef OMX_GOLDEN_ENGINE
  omx_strip_meter_legs(a, b, n, cl, cr, pl, pr);
#else
  omx_meter_leg_peaks(a, b, n, cl, cr, pl, pr);
#endif
}

static int k_clip(const float *a, const float *b, uint32_t n, float scale) {
#ifdef OMX_GOLDEN_ENGINE
  return omx_sustained_over(a, b, n, scale, OMX_CLIP_LINEAR, OMX_CLIP_RUN);
#else
  return omx_meter_clip_run(a, b, n, scale, OMX_METER_CLIP_LINEAR, OMX_METER_CLIP_RUN);
#endif
}

/* ---- the record ---------------------------------------------------------------------------- */

#define RECORD_BYTES (8u << 20)
static uint8_t g_rec[RECORD_BYTES];
static size_t g_len;
static int g_overflow;

static void put(const void *p, size_t n) {
  if (g_len + n > RECORD_BYTES) { g_overflow = 1; return; }
  memcpy(g_rec + g_len, p, n);
  g_len += n;
}
static void put_f(float v) { put(&v, sizeof v); }
static void put_u32(uint32_t v) { put(&v, sizeof v); }

/* ---- the stimulus -------------------------------------------------------------------------- */

static float lcg_noise(uint32_t *s) {
  *s = *s * 1664525u + 1013904223u;
  return (float)(int32_t)(*s >> 8) / 8388608.0f - 1.0f;
}

/* The i-th of m pan positions, -1.25 to +1.25: the clamp is on the path at both ends. */
static float sweep(uint32_t i, uint32_t m) { return -1.25f + 2.5f * (float)i / (float)(m - 1u); }

#define BLOCK_MAX 1920u
static float g_a[BLOCK_MAX], g_b[BLOCK_MAX];

/* ---- the kernels --------------------------------------------------------------------------- */

static void render(const char *k, uint32_t rate) {
  const uint32_t m = rate / 100u; /* sweep points and block length, 10 ms */
  if (strcmp(k, "pan") == 0) {
    for (uint32_t i = 0; i < m; i++) {
      float l, r;
      k_pan(sweep(i, m), &l, &r);
      put_f(l);
      put_f(r);
    }
  } else if (strcmp(k, "lcr") == 0) {
    static const float divs[] = {0.0f, 0.3f, 0.75f, 1.0f};
    for (uint32_t d = 0; d < 4; d++)
      for (uint32_t i = 0; i < m; i++) {
        float g[3];
        k_lcr(sweep(i, m), divs[d], g);
        put(g, sizeof g);
      }
  } else if (strcmp(k, "strip") == 0) {
    static const float levels[][2] = {{1.0f, 1.0f}, {0.5f, 0.7f}, {2.0f, 3.1623f}, {0.001f, 1.0f}};
    for (int lcr = 0; lcr <= 1; lcr++)
      for (int ch = 1; ch <= 2; ch++)
        for (int act = 0; act <= 1; act++)
          for (int mute = 0; mute <= 1; mute++)
            for (uint32_t v = 0; v < 4; v++)
              for (uint32_t i = 0; i < m; i += 7u) {
                float g[3];
                k_strip(lcr, ch, act, levels[v][0], levels[v][1], mute, sweep(i, m), 0.4f, g);
                put(g, sizeof g);
              }
  } else if (strcmp(k, "fold") == 0) {
    static const float coefs[] = {0.5f, 0.70710678f, 1.0f, 0.0f, NAN, INFINITY};
    for (uint32_t c = 0; c < 6; c++)
      for (uint32_t i = 0; i < m; i++) {
        const float level = (i == 1u) ? NAN : (i == 2u) ? -INFINITY : 4.0f * (float)i / (float)m;
        put_f(k_fold(level, coefs[c]));
      }
  } else if (strcmp(k, "bus_out") == 0) {
    uint32_t s = rate;
    for (int ch = 1; ch <= 2; ch++) {
      float cl = 1.0f, cr = 1.0f;
      for (uint32_t blk = 0; blk < 24u; blk++) {
        for (uint32_t i = 0; i < m; i++) {
          g_a[i] = lcg_noise(&s);
          g_b[i] = 0.5f * lcg_noise(&s);
        }
        const float fader = (blk % 4u == 3u) ? 3.1623f : 1.0f - 0.04f * (float)blk;
        const int mute = blk == 9u || blk == 10u;
        const float bal = -1.0f + (float)blk / 11.5f;
        float pl, pr;
        k_bus_out(g_a, g_b, m, ch, fader, mute, bal, &cl, &cr, &pl, &pr);
        put(g_a, m * sizeof(float));
        put(g_b, m * sizeof(float));
        put_f(cl);
        put_f(cr);
        put_f(pl);
        put_f(pr);
      }
    }
  } else if (strcmp(k, "peak") == 0) {
    uint32_t s = rate ^ 0x5eedu;
    for (uint32_t blk = 0; blk < 16u; blk++) {
      const uint32_t n = blk == 0 ? 0u : m - blk;
      for (uint32_t i = 0; i < n; i++) {
        g_a[i] = lcg_noise(&s) * (float)(blk + 1u) / 16.0f;
        g_b[i] = lcg_noise(&s);
      }
      put_f(k_peak(g_a, NULL, n));
      put_f(k_peak(g_a, g_a, n));
      put_f(k_peak(g_a, g_b, n));
      float pl, pr;
      k_leg_peaks(g_a, g_b, n, 0.3f, 1.7f, &pl, &pr);
      put_f(pl);
      put_f(pr);
    }
  } else if (strcmp(k, "clip") == 0) {
    static const float scales[] = {1.0f, 0.99f, 1.0001f, 2.0f};
    for (uint32_t run = 0; run <= 5u; run++)
      for (uint32_t leg = 0; leg < 2u; leg++)
        for (uint32_t sc = 0; sc < 4u; sc++) {
          for (uint32_t i = 0; i < m; i++) g_a[i] = g_b[i] = 0.5f;
          float *x = leg ? g_b : g_a;
          for (uint32_t i = 0; i < run; i++) x[m / 2u + i] = (i & 1u) ? -1.0f : 1.0f;
          x[m / 4u] = 1.0f; /* a lone over */
          put_u32((uint32_t)k_clip(g_a, g_b, m, scales[sc]));
          put_u32((uint32_t)k_clip(g_a, NULL, m, scales[sc]));
          put_u32((uint32_t)k_clip(g_b, g_b, m, scales[sc]));
        }
  }
}

static int digest(const char *k, uint32_t rate, char hex[65]) {
  g_len = 0;
  g_overflow = 0;
  render(k, rate);
  if (g_overflow) {
    fprintf(stderr, "mix_golden: the %s record at %u Hz overflows\n", k, rate);
    return 1;
  }
  omx_sha256_hex(g_rec, g_len, hex);
  return 0;
}

int main(int argc, char **argv) {
  char probe[65];
  omx_sha256_hex("abc", 3, probe);
  if (strcmp(probe, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
    fprintf(stderr, "mix_golden: the SHA-256 self-test failed (%s)\n", probe);
    return 2;
  }
  const int write = argc > 1 && strcmp(argv[1], "--write") == 0;
  const char *path = argc > 1 && !write ? argv[1] : "test/golden/mix.sha256";
  FILE *f = write ? NULL : fopen(path, "r");
  if (!write && !f) { fprintf(stderr, "mix_golden: cannot read %s\n", path); return 2; }
  static const char *const kernels[] = {"pan", "lcr", "strip", "fold", "bus_out", "peak", "clip"};
  const unsigned nk = sizeof kernels / sizeof kernels[0];
  int fail = 0, checked = 0;
  for (unsigned ki = 0; ki < nk; ki++)
    for (uint32_t r = 0; r < OMX_RME_RATES_COUNT; r++) {
      const uint32_t rate = (uint32_t)GOLDEN_RATES[r];
      char hex[65];
      if (digest(kernels[ki], rate, hex) != 0) return 2;
      if (write) { printf("%s %u %s\n", kernels[ki], rate, hex); continue; }
      char name[16], want[65] = {0};
      unsigned got_rate = 0;
      int found = 0;
      rewind(f);
      while (fscanf(f, "%15s %u %64s", name, &got_rate, want) == 3)
        if (strcmp(name, kernels[ki]) == 0 && got_rate == rate) { found = 1; break; }
      checked++;
      if (!found) { fprintf(stderr, "FAIL: %s %u Hz has no golden digest\n", kernels[ki], rate); fail++; }
      else if (strcmp(want, hex) != 0) {
        fprintf(stderr, "FAIL: %s %u Hz output moved: %s, golden %s\n", kernels[ki], rate, hex, want);
        fail++;
      }
    }
  if (f) fclose(f);
  if (!write) printf("mix_golden: %d digests (%u kernels at %u rates), %d moved\n", checked, nk,
                     (unsigned)OMX_RME_RATES_COUNT, fail);
  return fail == 0 ? 0 : 1;
}
