// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* Writes the seed corpus of every harness in fuzz/ (`make fuzz-corpus`).
 *
 * Every seed is built from what the repository already holds: the control ports each effect
 * declares (the instance faces' own defaults, read through the harness's resolve functions so
 * that a seed's port block is exactly as long as the face reads), the EQ's default bands, the
 * converter's rate pairs, the note map's policies. Nothing is fetched and no capture is read, so
 * the corpus regenerates byte for byte on any machine.
 *
 * Each seed is shaped the way its harness reads its input; the harness's header comment is the
 * grammar. Usage: fuzz-gen-corpus <corpus dir>. */
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

/* The effect table and the port readers of fuzz_fx.c, reused so a seed knows how many bytes a
 * face's resolve reads. The harness's entry point is defined and never called. */
#include "fuzz_fx.c"

/* ---- a growable byte buffer, one seed at a time ---- */
struct seed {
  uint8_t *p;
  size_t n, cap;
};

static void put(struct seed *s, const void *p, size_t n) {
  if (s->n + n > s->cap) {
    s->cap = (s->n + n) * 2 + 64;
    s->p = realloc(s->p, s->cap);
    if (!s->p) abort();
  }
  memcpy(s->p + s->n, p, n);
  s->n += n;
}
static void u8(struct seed *s, unsigned v) {
  const uint8_t b = (uint8_t)v;
  put(s, &b, 1);
}
static void u16(struct seed *s, unsigned v) {
  u8(s, v & 0xFFu);
  u8(s, v >> 8);
}
static void u32(struct seed *s, uint32_t v) {
  u16(s, v & 0xFFFFu);
  u16(s, v >> 16);
}
static void f32(struct seed *s, float f) {
  uint32_t w;
  memcpy(&w, &f, sizeof w);
  u32(s, w);
}
static void f64(struct seed *s, double d) {
  uint64_t w;
  memcpy(&w, &d, sizeof w);
  u32(s, (uint32_t)w);
  u32(s, (uint32_t)(w >> 32));
}
/* n samples of a sine at `cycles` per block, `amp` of full scale, as the signed 16-bit words fuzz_block reads */
static void sine(struct seed *s, unsigned n, double cycles, double amp) {
  for (unsigned i = 0; i < n; i++) {
    const double x = amp * sin(6.283185307179586 * cycles * (double)i / (double)n);
    u16(s, (unsigned)(int16_t)lrint(x * 32767.0) & 0xFFFFu);
  }
}
/* a u16 over [lo, hi], as fuzz_range reads it */
static unsigned range16(double x, double lo, double hi) { return (unsigned)lrint((x - lo) / (hi - lo) * 65535.0); }

static const char *g_dir;

static void write_seed(const char *harness, const char *name, const struct seed *s) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", g_dir, harness);
  if (mkdir(path, 0755) != 0 && errno != EEXIST) {
    perror(path);
    exit(1);
  }
  snprintf(path, sizeof path, "%s/%s/%s", g_dir, harness, name);
  FILE *f = fopen(path, "wb");
  if (!f || fwrite(s->p, 1, s->n, f) != s->n || fclose(f) != 0) {
    perror(path);
    exit(1);
  }
}

static void finish(const char *harness, const char *name, struct seed *s) {
  write_seed(harness, name, s);
  s->n = 0;
}

/* ---- fuzz_fx: each face, at 48 kHz, two steps of ports and a block ---- */

/* The words a port block is made of: a table cycled, as floats. An integer port reads the same word's
 * bits, which no choice id matches, so it takes the face's default; the two tables differ in what they
 * make a float port say. */
static const float PORTS_NEUTRAL[] = {0.0f, 0.5f, 1.0f, 0.0f, 100.0f, 0.25f, 1000.0f, 0.0f, 10.0f, 0.75f};
static const float PORTS_HOT[] = {-18.0f, 4.0f, 6.0f, 5.0f, 120.0f, 3.0f, 2000.0f, 0.9f, 40.0f, -6.0f, 12.0f, 0.3f};

static size_t port_bytes(const struct fx *fx) {
  /* a face reads as many bytes as its resolve takes: run it over a long block and count */
  static uint8_t zeros[8192];
  void *a = fx_alloc(fx->size);
  fx->init(a, 48000.0f);
  struct fuzz_in c = fuzz_in(zeros, sizeof zeros);
  fx->resolve(a, 0, &c);
  free(a);
  return sizeof zeros - c.n;
}

static void fx_step(struct seed *s, const struct fx *fx, const float *table, size_t tn, int bypass, double amp,
                    unsigned n) {
  u8(s, (unsigned)bypass);
  const size_t bytes = port_bytes(fx);
  for (size_t i = 0; i < bytes / 4; i++) f32(s, table[i % tn]);
  for (size_t i = 0; i < bytes % 4; i++) u8(s, 0);
  u16(s, n - 1u);
  sine(s, n, 3.0, amp); /* left */
  sine(s, n, 5.0, amp); /* right */
}

static void gen_fx(void) {
  struct seed s = {0};
  for (size_t k = 0; k < FX_COUNT; k++) {
    const struct fx *fx = &FX[k];
    char name[64];
    /* flags: bit 0 clear (a rate of the grid), bits 2.. the loudness; the rate byte 2 is 48 kHz */
    u8(&s, (unsigned)k);
    u8(&s, 0u);
    u8(&s, 2u);
    fx_step(&s, fx, PORTS_NEUTRAL, sizeof PORTS_NEUTRAL / sizeof PORTS_NEUTRAL[0], 0, 0.5, 256u);
    fx_step(&s, fx, PORTS_NEUTRAL, sizeof PORTS_NEUTRAL / sizeof PORTS_NEUTRAL[0], 0, 0.5, 100u);
    snprintf(name, sizeof name, "%s-neutral", fx->name);
    finish("fx", name, &s);

    u8(&s, (unsigned)k);
    u8(&s, 0x0Cu); /* loudness 3: samples up to 8 */
    u8(&s, 6u);    /* 128 kHz: not a declared rate */
    fx_step(&s, fx, PORTS_HOT, sizeof PORTS_HOT / sizeof PORTS_HOT[0], 0, 1.0, 200u);
    fx_step(&s, fx, PORTS_HOT, sizeof PORTS_HOT / sizeof PORTS_HOT[0], 1, 1.0, 64u);
    fx_step(&s, fx, PORTS_NEUTRAL, sizeof PORTS_NEUTRAL / sizeof PORTS_NEUTRAL[0], 0, 0.9, 150u);
    snprintf(name, sizeof name, "%s-hot", fx->name);
    finish("fx", name, &s);
  }
  /* a rate off the grid: every face refuses it and is the identity (22050 Hz, on the 8..768 kHz scale) */
  u8(&s, 12u); /* limiter */
  u8(&s, 1u);
  u16(&s, range16(22050.0, 8000.0, 768000.0));
  fx_step(&s, &FX[12], PORTS_NEUTRAL, sizeof PORTS_NEUTRAL / sizeof PORTS_NEUTRAL[0], 0, 0.5, 128u);
  finish("fx", "refused-rate", &s);
  /* a rate the comp and the gate take though it is no rate of the grid */
  u8(&s, 1u);
  u8(&s, 1u);
  u16(&s, range16(22050.0, 8000.0, 768000.0));
  fx_step(&s, &FX[1], PORTS_HOT, sizeof PORTS_HOT / sizeof PORTS_HOT[0], 0, 0.9, 128u);
  finish("fx", "comp-22050", &s);
  /* hostile samples: bit 1 of the flags, raw float words per sample (NaN, Inf, a denormal, a huge one) */
  u8(&s, 10u); /* geq */
  u8(&s, 2u);
  u8(&s, 2u);
  {
    u8(&s, 0u);
    const size_t bytes = port_bytes(&FX[10]);
    for (size_t i = 0; i < bytes; i++) u8(&s, 0u);
    u16(&s, 7u);
    static const uint32_t words[] = {0x7FC00000u, 0x7F800000u, 0xFF800000u, 0x00000001u, 0x7F7FFFFFu, 0x3F800000u,
                                     0x80000000u, 0x7FC00000u, 0x7F800000u, 0xFF800000u, 0x00000001u, 0x7F7FFFFFu,
                                     0x3F800000u, 0x80000000u};
    for (size_t i = 0; i < sizeof words / sizeof words[0]; i++) u32(&s, words[i]);
  }
  finish("fx", "hostile-samples", &s);
  free(s.p);
}

/* ---- fuzz_laws: one call of each selector ---- */
static void gen_laws(void) {
  struct seed s = {0};
  /* clamp: v, lo, hi, default */
  u8(&s, 0); f32(&s, 5.0f); f32(&s, -1.0f); f32(&s, 1.0f); f32(&s, 0.0f);
  u8(&s, 0); f32(&s, NAN); f32(&s, -10.0f); f32(&s, 10.0f); f32(&s, 3.0f);
  finish("laws", "clamp", &s);
  /* units: dB, linear */
  u8(&s, 1); f32(&s, -6.0f); f32(&s, 0.5f);
  u8(&s, 1); f32(&s, 140.0f); f32(&s, 0.0f);
  u8(&s, 1); f32(&s, INFINITY); f32(&s, 1e-12f);
  finish("laws", "units", &s);
  /* pan and fader: pan, any pan, fader, leg, send, mute, dca, dca mute */
  u8(&s, 2); u16(&s, range16(0.3, -1.0, 1.0)); f32(&s, 0.4f); u16(&s, range16(-6.0, -200.0, 60.0)); u8(&s, 1);
  f32(&s, 0.8f); u8(&s, 0); f32(&s, 1.0f); u8(&s, 0);
  u8(&s, 2); u16(&s, 65535u); f32(&s, NAN); u16(&s, range16(0.0, -200.0, 60.0)); u8(&s, 1);
  f32(&s, 1.0f); u8(&s, 0); f32(&s, 1.0f); u8(&s, 0);
  finish("laws", "pan-fader", &s);
  /* ramp: start, target, length */
  u8(&s, 3); f32(&s, 0.25f); f32(&s, 0.75f); u8(&s, 63);
  u8(&s, 3); f32(&s, NAN); f32(&s, INFINITY); u8(&s, 0);
  finish("laws", "ramp", &s);
  /* wet/dry: mix, dry l r, wet l r, block length, then the block's two bodies */
  u8(&s, 4); u16(&s, range16(0.3, 0.0, 1.0)); u16(&s, 1000); u16(&s, 50000); u16(&s, 20000); u16(&s, 40000);
  u8(&s, 7);
  for (int i = 0; i < 16; i++) u16(&s, (unsigned)(i * 4099) & 0xFFFFu);
  finish("laws", "wetdry", &s);
  /* one-pole: rate index, ms, hz, pole, ref, run, state, input */
  u8(&s, 5); u8(&s, 2); f32(&s, 50.0f); f32(&s, 1000.0f); u16(&s, 40000); f32(&s, 96000.0f); f32(&s, 48000.0f);
  u16(&s, 30000); u16(&s, 50000);
  u8(&s, 5); u8(&s, 4); f32(&s, 1e30f); f32(&s, 1e-9f); u16(&s, 65535); f32(&s, 0.0f); f32(&s, NAN);
  u16(&s, 0); u16(&s, 65535);
  finish("laws", "onepole", &s);
  free(s.p);
}

/* ---- fuzz_eq: the EQ's default bank, a head filter and a block ---- */
static void gen_eq(void) {
  struct seed s = {0};
  for (int variant = 0; variant < 4; variant++) {
    u8(&s, variant == 0 ? 0x06 : variant == 1 ? 0x07 : variant == 2 ? 0x02 : 0x04); /* rate source, EQ on, stereo */
    if (variant == 1) u16(&s, range16(44100.0, 8000.0, 768000.0)); else u8(&s, variant == 2 ? 0 : 2);
    /* head filters: high-pass on at 80 Hz, 24 dB; low-pass on at 16 kHz, 12 dB */
    u8(&s, 1); f64(&s, 80.0); u8(&s, 1);
    u8(&s, variant == 3 ? 0 : 1); f64(&s, 16000.0); u8(&s, 0);
    const int bands = variant == 2 ? 24 : 6;
    u8(&s, (unsigned)(bands - 1));
    static const double F[6] = {60.0, 250.0, 1000.0, 4000.0, 8000.0, 12000.0};
    static const double Q[6] = {0.7, 1.0, 1.4, 2.0, 0.7, 0.7};
    static const double G[6] = {4.0, -3.0, 2.5, -5.0, 3.0, -2.0};
    for (int i = 0; i < bands; i++) {
      u8(&s, (unsigned)(i % 9) % (variant == 0 ? 3u : 9u)); /* kind */
      u8(&s, 1);                                            /* on */
      f64(&s, F[i % 6]);
      f64(&s, Q[i % 6]);
      u16(&s, range16(G[i % 6], -24.0, 24.0));
      u8(&s, i == 3 ? 1u : 0u); /* one parked band */
    }
    f64(&s, 1000.0);
    u8(&s, 199);
    sine(&s, 200, 7.0, 0.4);
    sine(&s, 200, 11.0, 0.4);
    f64(&s, 1.0);   /* the width */
    f64(&s, 1000.0); /* and its corner */
    char name[32];
    snprintf(name, sizeof name, "bank-%d", variant);
    finish("eq", name, &s);
  }
  free(s.p);
}

/* ---- fuzz_src: rate pairs the converters really make, whole and in blocks ---- */
static void gen_src(void) {
  static const struct { const char *name; uint32_t in, out; unsigned flags; } PAIRS[] = {
      {"44100-48000", 44100, 48000, 3}, {"48000-44100", 48000, 44100, 3}, {"96000-48000", 96000, 48000, 1},
      {"48000-96000", 48000, 96000, 2}, {"48000-192000", 48000, 192000, 3}, {"192000-44100", 192000, 44100, 1},
      {"88200-48000", 88200, 48000, 3}, {"48000-48000", 48000, 48000, 3}, {"32000-48000", 32000, 48000, 2},
  };
  struct seed s = {0};
  for (size_t k = 0; k < sizeof PAIRS / sizeof PAIRS[0]; k++) {
    u32(&s, PAIRS[k].in);
    u32(&s, PAIRS[k].out);
    u8(&s, PAIRS[k].flags);
    static const unsigned blocks[] = {200, 100, 300, 13, 512};
    for (size_t b = 0; b < sizeof blocks / sizeof blocks[0]; b++) {
      u16(&s, blocks[b]);
      sine(&s, blocks[b], 4.0 + (double)b, 0.5);
    }
    finish("src", PAIRS[k].name, &s);
  }
  u32(&s, 0); u32(&s, 48000); u8(&s, 0); finish("src", "zero-rate", &s);
  u32(&s, 44100); u32(&s, 4294967295u); u8(&s, 0); finish("src", "huge-ratio", &s);
  free(s.p);
}

/* ---- fuzz_delay: the fractional line at both orders, the integer line and its fades ---- */
static void gen_delay(void) {
  struct seed s = {0};
  static const float D[] = {0.0f, 1.0f, 2.5f, 10.0f, 33.75f, 100.0f, 0.3f, 400.0f};
  for (int order5 = 0; order5 < 2; order5++) {
    u8(&s, 1u | (order5 ? 2u : 0u));
    u16(&s, 999);
    for (size_t b = 0; b < sizeof D / sizeof D[0]; b++) {
      u8(&s, 79);
      f32(&s, D[b]);
      sine(&s, 80, 3.0 + (double)b, 0.5);
    }
    finish("delay", order5 ? "fractional-5" : "fractional-3", &s);
  }
  u8(&s, 1u);
  u16(&s, 2);
  u8(&s, 31); f32(&s, 1.5f); sine(&s, 32, 3.0, 0.5);
  u8(&s, 31); f32(&s, NAN); sine(&s, 32, 3.0, 0.5);
  u8(&s, 31); f32(&s, INFINITY); sine(&s, 32, 3.0, 0.5);
  finish("delay", "fractional-tiny-ring", &s);
  for (int mono = 0; mono < 2; mono++) {
    u8(&s, mono ? 4u : 0u);
    u16(&s, 3000);
    static const unsigned MS_L[] = {100, 100, 350, 350, 0, 2000}, MS_R[] = {200, 200, 200, 90, 90, 20};
    for (size_t b = 0; b < sizeof MS_L / sizeof MS_L[0]; b++) {
      u8(&s, 99);
      u8(&s, 2); /* 48 kHz */
      u16(&s, MS_L[b]);
      u16(&s, MS_R[b]);
      sine(&s, 100, 3.0, 0.5);
      sine(&s, 100, 5.0, 0.5);
    }
    finish("delay", mono ? "integer-mono" : "integer-pair", &s);
  }
  free(s.p);
}

/* ---- fuzz_note_map: a leg and a stream of messages ---- */
static void msg(struct seed *s, unsigned st, unsigned d1, unsigned d2) {
  u8(s, 0); /* a message record */
  u8(s, st);
  u8(s, d1);
  u8(s, d2);
}
static void leg(struct seed *s, unsigned ch, unsigned lo, unsigned hi_extra, int xpose, unsigned curve, unsigned fixed) {
  u8(s, ch);
  u8(s, lo);
  u8(s, hi_extra);
  u8(s, (unsigned)(xpose + OMX_NOTE_TRANSPOSE_MAX));
  u8(s, curve);
  u8(s, fixed - 1u);
}
static void gen_note_map(void) {
  struct seed s = {0};
  leg(&s, 0, 0, 127, 0, 0, 64);
  msg(&s, 0x90, 60, 100); msg(&s, 0x90, 64, 80); msg(&s, 0x90, 60, 90); msg(&s, 0x80, 60, 0);
  msg(&s, 0x90, 64, 0); msg(&s, 0xB0, 1, 64); msg(&s, 0xE0, 0, 64); msg(&s, 0xA0, 60, 10);
  finish("note_map", "omni-linear", &s);
  leg(&s, 1, 36, 60, 12, 1, 64);
  msg(&s, 0x90, 40, 100); msg(&s, 0x91, 40, 100); msg(&s, 0x90, 100, 100); msg(&s, 0x90, 59, 127);
  msg(&s, 0xB0, 123, 0); msg(&s, 0x90, 60, 1); u8(&s, 231);
  finish("note_map", "channel-1-up-an-octave", &s);
  leg(&s, 3, 0, 127, -48, 2, 100);
  msg(&s, 0x92, 30, 64); msg(&s, 0x92, 100, 64); msg(&s, 0x92, 100, 64); msg(&s, 0x82, 100, 0); msg(&s, 0x92, 60, 10);
  u8(&s, 200); for (int i = 0; i < 6; i++) u32(&s, (uint32_t)(i * 3)); u8(&s, 0); /* an invalid leg, refused */
  u8(&s, 201); for (int i = 0; i < 6; i++) u32(&s, 0u); u8(&s, 1); leg(&s, 5, 10, 50, 3, 1, 50); /* a valid one */
  msg(&s, 0x94, 20, 90); msg(&s, 0x92, 70, 90); u8(&s, 252);
  finish("note_map", "transposed-below-zero-and-change-leg", &s);
  leg(&s, 0, 0, 127, 5, 3, 77);
  for (unsigned k = 0; k < 16; k++) msg(&s, 0x90 | (k & 15u), 100 + k, 1 + k * 7);
  msg(&s, 0xF8, 0, 0); msg(&s, 0xFF, 0, 0); msg(&s, 0x90, 200, 100); msg(&s, 0x70, 1, 1);
  u8(&s, 233);
  finish("note_map", "all-channels-and-noise", &s);
  free(s.p);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <corpus dir>\n", argv[0]);
    return 2;
  }
  g_dir = argv[1];
  if (mkdir(g_dir, 0755) != 0 && errno != EEXIST) {
    perror(g_dir);
    return 1;
  }
  gen_fx();
  gen_laws();
  gen_eq();
  gen_src();
  gen_delay();
  gen_note_map();
  return 0;
}
