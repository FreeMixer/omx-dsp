// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The golden digests of the kernels moved from the OpenMixer engine (FreeMixer/openmixer#1580): the
 * sample-rate converter (omx_src.h), the generators (omx_gen.h) and the analyser's spectrum
 * readings and half-band decimator (analysis/omx_rta.h), hashed per kernel and per rate at the nine RME
 * rates and compared with test/golden/engine_kernels.sha256.
 *
 *   make test-engine-kernels                      compare
 *   build/engine_kernels_golden --write           print the lines the digest file holds
 *
 * The committed digests were written by this program compiled against the engine's own copies
 * (openmixer 6ee8a9bde101: mix_src.h and mix_dsp.h) by tools/engine-kernels-identity.sh, so the
 * moved code is held to the engine's output bit for bit. -DOMX_GOLDEN_ENGINE is that build: the
 * engine spells the converter's design rolloff OMX_SRC_ROLLOFF, a double, where omx-contract's
 * double spelling is OMX_SRC_ROLLOFF_DOUBLE (the same 0.92).
 *
 * The stimulus makes no libm call: an integer generator, impulses, steps and square waves whose
 * periods are computed from the rate by integer division.
 *
 *   src  every RME rate to every RME rate: the plan, the designed table and the conversion of a
 *        4096-sample stimulus fed in uneven blocks with the flush, plus the 40 dB, 0.8 design of
 *        the rate to its double
 *   gen  every kind and waveform through a run of fills of uneven lengths, frequencies (in band,
 *        at the guard, past Nyquist, NaN) and levels (ramps up and down, past 1, NaN), then the
 *        state
 *   rta  the decimator on blocks of 0 to 129 samples and three cascaded stages of a 65536-sample
 *        window, the transform at 2, 4 and 8192 points, and both spectrum readings at 8192
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_gen.h>
#include <omxdsp/analysis/omx_rta.h>
#include <omxdsp/omx_src.h>
#include <omxcontract/omx_contract_limits.h>

#include "../fx/sha256.h"

#ifdef OMX_GOLDEN_ENGINE
#define GOLDEN_SRC_ROLLOFF OMX_SRC_ROLLOFF
#else
#define GOLDEN_SRC_ROLLOFF OMX_SRC_ROLLOFF_DOUBLE
#endif

static const float GOLDEN_RATES[OMX_RME_RATES_COUNT] = OMX_RME_RATES_INIT;

/* ---- the record ---------------------------------------------------------------------------- */

#define RECORD_BYTES (16u << 20)
static uint8_t g_rec[RECORD_BYTES];
static size_t g_len;
static int g_overflow;

static void put(const void *p, size_t n) {
  if (g_len + n > RECORD_BYTES) { g_overflow = 1; return; }
  memcpy(g_rec + g_len, p, n);
  g_len += n;
}
static void put_u32(uint32_t v) { put(&v, sizeof v); }
static void put_u64(uint64_t v) { put(&v, sizeof v); }

/* ---- the stimulus -------------------------------------------------------------------------- */

static float lcg_noise(uint32_t *s) {
  *s = *s * 1664525u + 1013904223u;
  return (float)(int32_t)(*s >> 8) / 8388608.0f - 1.0f;
}

/* `n` samples at `rate`: an impulse, a quarter of noise, a DC step, a square wave of period
 * rate/1000 samples, then noise at a tenth of the level, then silence. */
static void stimulus(float *x, uint32_t n, uint32_t rate, uint32_t seed) {
  uint32_t s = seed;
  const uint32_t period = rate / 1000u;
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t part = i * 6u / n;
    float v = 0.0f;
    if (i == 0) v = 1.0f;
    else if (part == 0 || part == 1) v = 0.5f * lcg_noise(&s);
    else if (part == 2) v = 0.25f;
    else if (part == 3) v = (i % period) < period / 2u ? 0.7f : -0.7f;
    else if (part == 4) v = 0.1f * lcg_noise(&s);
    x[i] = v;
  }
}

/* ---- src ------------------------------------------------------------------------------------- */

#define SRC_IN 4096u
static float src_in[SRC_IN];
static float src_out[SRC_IN * 8u + 1024u];

static void render_src_pair(uint32_t in_rate, uint32_t out_rate, double rolloff, double stopband) {
  struct omx_src_plan plan = {0};
  const int r = omx_src_plan_of(in_rate, out_rate, &plan);
  put_u32((uint32_t)r);
  if (r != 0) return;
  put_u32(plan.l);
  put_u32(plan.m);
  put_u32(plan.taps);
  float *coef = calloc(omx_src_coef_count(&plan), sizeof(float));
  float *hist = calloc(omx_src_hist_count(&plan), sizeof(float));
  omx_src_design(&plan, coef, rolloff, stopband);
  put(coef, omx_src_coef_count(&plan) * sizeof(float));
  struct omx_src s;
  omx_src_init(&s, &plan, coef, hist);
  const uint32_t cap = (uint32_t)(sizeof src_out / sizeof src_out[0]);
  static const uint32_t blocks[] = {1u, 7u, 64u, 1000u, 333u};
  uint32_t written = 0, i = 0, b = 0;
  while (i < SRC_IN) {
    uint32_t take = blocks[b++ % 5u];
    if (take > SRC_IN - i) take = SRC_IN - i;
    const int last = i + take >= SRC_IN;
    written += omx_src_run(&s, src_in + i, take, src_out + written, cap - written, last);
    i += take;
  }
  put_u32(written);
  put_u64(omx_src_out_frames(SRC_IN, &plan));
  put_u32(omx_src_out_capacity(&plan, SRC_IN));
  put_u64(s.acc);
  put_u64(s.consumed);
  put(src_out, written * sizeof(float));
  free(coef);
  free(hist);
}

static void render_src(uint32_t rate) {
  stimulus(src_in, SRC_IN, rate, 0x1234567u);
  for (uint32_t k = 0; k < OMX_RME_RATES_COUNT; k++)
    render_src_pair(rate, (uint32_t)GOLDEN_RATES[k], GOLDEN_SRC_ROLLOFF, OMX_SRC_STOPBAND_DB);
  render_src_pair(rate, 2u * rate, 0.8, 40.0);
  put_u32(omx_src_gcd(rate, 2u * rate + 1u));
  const double xs[] = {0.0, 1e-13, 0.5, 1.0, 2.5, -3.25, 31.5};
  for (unsigned k = 0; k < sizeof xs / sizeof xs[0]; k++) {
    const double v[2] = {omx_src_sinc(xs[k]), omx_src_bessel_i0(xs[k] * 2.0)};
    put(v, sizeof v);
  }
}

/* ---- gen ------------------------------------------------------------------------------------- */

static float gen_out[4096];

static void render_gen(float sr) {
  const int kinds[] = {OMX_GEN_OSC, OMX_GEN_WHITE, OMX_GEN_PINK, 7};
  const int waves[] = {OMX_WAVE_SINE, OMX_WAVE_SQUARE, OMX_WAVE_SAW, OMX_WAVE_TRIANGLE, -1};
  const uint32_t lens[] = {1u, 7u, 64u, 256u, 1000u, 4096u, 0u, 333u};
  const float freqs[] = {100.0f, 1000.0f, 997.3f, 0.45f * sr, 0.6f * sr, 2.0f * sr, NAN, -5.0f};
  const float levels[] = {0.25f, 1.0f, 0.5f, 1.5f, NAN, 0.0f, 0.75f, -1.0f};
  for (unsigned ki = 0; ki < sizeof kinds / sizeof kinds[0]; ki++)
    for (unsigned wi = 0; wi < sizeof waves / sizeof waves[0]; wi++) {
      if (kinds[ki] != OMX_GEN_OSC && wi > 0) break; /* the noises take no waveform */
      struct omx_gen_state st;
      omx_gen_init(&st, ki == 3 ? 0u : 0xabcdef01u + ki);
      for (unsigned step = 0; step < 24; step++) {
        const uint32_t n = lens[step % 8u];
        omx_gen_fill(&st, gen_out, n, kinds[ki], waves[wi], freqs[(step * 3u) % 8u], levels[(step * 5u) % 8u], sr);
        put(gen_out, n * sizeof(float));
        put(&st, sizeof st);
      }
    }
  uint32_t rng = 1u;
  float b[7] = {0};
  for (int i = 0; i < 1000; i++) {
    const float w = omx_gen_white(&rng);
    const float p = omx_gen_pink(w, b);
    put(&w, sizeof w);
    put(&p, sizeof p);
  }
  for (int w = 0; w < 5; w++)
    for (int i = 0; i < 64; i++) {
      const float v = omx_gen_osc(w, (float)i / 64.0f);
      put(&v, sizeof v);
    }
}

/* ---- rta ------------------------------------------------------------------------------------- */

#define RTA_WINDOW 65536u
#define RTA_FFT 8192u
static float rta_x[RTA_WINDOW], rta_s1[RTA_WINDOW / 2u], rta_s2[RTA_WINDOW / 4u], rta_s3[RTA_WINDOW / 8u];
static float rta_re[RTA_FFT], rta_im[RTA_FFT], rta_a[RTA_FFT], rta_b[RTA_FFT];

static void render_rta(uint32_t rate) {
  stimulus(rta_x, RTA_WINDOW, rate, 0x7654321u);
  for (uint32_t n = 0; n < 130u; n++) {
    float out[65];
    const uint32_t got = omx_decimate_halfband(rta_x + 1000u, n, out);
    put_u32(got);
    put(out, got * sizeof(float));
  }
  const uint32_t n1 = omx_decimate_halfband(rta_x, RTA_WINDOW, rta_s1);
  const uint32_t n2 = omx_decimate_halfband(rta_s1, n1, rta_s2);
  const uint32_t n3 = omx_decimate_halfband(rta_s2, n2, rta_s3);
  put(rta_s1, n1 * sizeof(float));
  put(rta_s2, n2 * sizeof(float));
  put(rta_s3, n3 * sizeof(float));
  const float edge = omx_decimate_halfband_edge(rta_x, 2, (int32_t)RTA_WINDOW - 1);
  put(&edge, sizeof edge);
  const uint32_t ns[] = {2u, 4u, RTA_FFT};
  for (unsigned k = 0; k < 3; k++) {
    omx_rta_transform(rta_x + 7u, ns[k], rta_re, rta_im);
    put(rta_re, ns[k] * sizeof(float));
    put(rta_im, ns[k] * sizeof(float));
  }
  omx_rta_spectrum(rta_s3, RTA_FFT, rta_re, rta_im, rta_a);
  put(rta_a, RTA_FFT / 2u * sizeof(float));
  omx_rta_spectrum(rta_x + 4096u, RTA_FFT, rta_re, rta_im, rta_a);
  put(rta_a, RTA_FFT / 2u * sizeof(float));
  omx_rta_spectrum_complex(rta_x + 4096u, RTA_FFT, rta_re, rta_im, rta_a, rta_b);
  put(rta_a, RTA_FFT / 2u * sizeof(float));
  put(rta_b, RTA_FFT / 2u * sizeof(float));
  omx_rta_spectrum_complex(rta_x + 100u, RTA_FFT, rta_re, rta_im, rta_re, rta_im); /* aliased out */
  put(rta_re, RTA_FFT / 2u * sizeof(float));
  put(rta_im, RTA_FFT / 2u * sizeof(float));
}

/* ---- the driver ------------------------------------------------------------------------------ */

static int digest(const char *kernel, uint32_t rate, char hex[65]) {
  g_len = 0;
  g_overflow = 0;
  if (strcmp(kernel, "src") == 0) render_src(rate);
  else if (strcmp(kernel, "gen") == 0) render_gen((float)rate);
  else render_rta(rate);
  if (g_overflow) { fprintf(stderr, "engine_kernels_golden: the %s record overflowed\n", kernel); return 2; }
  omx_sha256_hex(g_rec, g_len, hex);
  return 0;
}

int main(int argc, char **argv) {
  char probe[65];
  omx_sha256_hex("abc", 3, probe);
  if (strcmp(probe, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
    fprintf(stderr, "engine_kernels_golden: the SHA-256 self-test failed (%s)\n", probe);
    return 2;
  }
  const int write = argc > 1 && strcmp(argv[1], "--write") == 0;
  const char *path = argc > 1 && !write ? argv[1] : "test/golden/engine_kernels.sha256";
  FILE *f = write ? NULL : fopen(path, "r");
  if (!write && !f) { fprintf(stderr, "engine_kernels_golden: cannot read %s\n", path); return 2; }
  static const char *const kernels[] = {"src", "gen", "rta"};
  int fail = 0, checked = 0;
  for (unsigned ki = 0; ki < 3; ki++)
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
  if (!write) printf("engine_kernels_golden: %d digests (src, gen, rta at %u rates), %d moved\n", checked,
                     (unsigned)OMX_RME_RATES_COUNT, fail);
  return fail == 0 ? 0 : 1;
}
