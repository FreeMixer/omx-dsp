// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- the Hann window and the radix-2 FFTs ---------------------------------------------------- */

/* The fixed vector set: a 32-bit LCG in [-1, 1), seeded per length, and a two-tone block (a
 * 0.9 sine at 1 kHz and a 1e-3 one at 7531.25 Hz, at 96 kHz). */
static uint32_t fft_lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
static void fft_fill(float *x, uint32_t n, uint32_t seed) {
  uint32_t s = seed ^ (n * 2654435761u);
  for (uint32_t i = 0; i < n; i++) x[i] = (float)((double)(fft_lcg(&s) >> 8) / 8388608.0 - 1.0);
}
static void fft_tones(float *x, uint32_t n) {
  for (uint32_t i = 0; i < n; i++)
    x[i] = (float)(0.9 * sin(2.0 * M_PI * 1000.0 * (double)i / 96000.0) +
                   1e-3 * sin(2.0 * M_PI * 7531.25 * (double)i / 96000.0));
}
static uint64_t fft_fnv(uint64_t h, const void *p, size_t len) {
  const unsigned char *b = p;
  for (size_t i = 0; i < len; i++) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

/* FNV-1a 64 over every length 2..65536 and both inputs: the window's output, the complex
 * transform's (LCG re + LCG im, and the tones on a zero im) and the real transform's n/2 bins. */
static uint64_t fft_digest(float *a, float *b) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t n = 2; n <= 65536u; n <<= 1) {
    for (int v = 0; v < 2; v++) {
      if (v == 0) fft_fill(a, n, 0x5eedu); else fft_tones(a, n);
      omx_hann_window(a, n);
      h = fft_fnv(h, a, n * sizeof(float));
      if (v == 0) { fft_fill(a, n, 0x5eedu); fft_fill(b, n, 0xbeefu); }
      else { fft_tones(a, n); memset(b, 0, n * sizeof(float)); }
      omx_fft_radix2(a, b, n);
      h = fft_fnv(h, a, n * sizeof(float));
      h = fft_fnv(h, b, n * sizeof(float));
      if (n >= 4) {
        if (v == 0) fft_fill(a, n, 0x5eedu); else fft_tones(a, n);
        omx_fft_real_half(a, b, n);
        h = fft_fnv(h, a, n / 2 * sizeof(float));
        h = fft_fnv(h, b, n / 2 * sizeof(float));
      }
    }
  }
  return h;
}

/* The digest of the OpenMixer engine's own omx_hann_window / omx_fft_radix2 / omx_fft_real_half
 * (packages/pipewire-native/src/mix_dsp.h at openmixer integration/waves-2026-10 7574f4fa6, copied
 * verbatim into a harness built with -ffp-contract=off at -O0 and -O2, both giving this value) on
 * the vector set above. The move into omx-dsp is bit-exact when this matches. */
#define FFT_ENGINE_DIGEST 0x14b42bb357621e5full

/* The reference: radix-2 in long double, each twiddle from cosl/sinl of its own angle. */
static void fft_ref(long double *re, long double *im, uint32_t n, int inverse) {
  for (uint32_t i = 1, j = 0; i < n; i++) {
    uint32_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      long double t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  const long double sign = inverse ? 1.0L : -1.0L;
  for (uint32_t len = 2; len <= n; len <<= 1) {
    for (uint32_t k = 0; k < len / 2; k++) {
      const long double ang = sign * 2.0L * 3.14159265358979323846264338327950288L * (long double)k / (long double)len;
      const long double wr = cosl(ang), wi = sinl(ang);
      for (uint32_t i = 0; i < n; i += len) {
        const uint32_t a = i + k, b = a + len / 2;
        const long double vr = re[b] * wr - im[b] * wi, vi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - vr; im[b] = im[a] - vi;
        re[a] += vr; im[a] += vi;
      }
    }
  }
  if (inverse)
    for (uint32_t i = 0; i < n; i++) { re[i] /= (long double)n; im[i] /= (long double)n; }
}

/* ‖got − ref‖ / ‖ref‖ over m bins. */
static double fft_rel_f(const float *gr, const float *gi, const long double *rr, const long double *ri, uint32_t m) {
  long double e = 0.0L, r = 0.0L;
  for (uint32_t k = 0; k < m; k++) {
    const long double dr = (long double)gr[k] - rr[k], di = (long double)gi[k] - ri[k];
    e += dr * dr + di * di;
    r += rr[k] * rr[k] + ri[k] * ri[k];
  }
  return (double)sqrtl(e / r);
}
static double fft_rel_d(const double *gr, const double *gi, const long double *rr, const long double *ri, uint32_t m) {
  long double e = 0.0L, r = 0.0L;
  for (uint32_t k = 0; k < m; k++) {
    const long double dr = (long double)gr[k] - rr[k], di = (long double)gi[k] - ri[k];
    e += dr * dr + di * di;
    r += rr[k] * rr[k] + ri[k] * ri[k];
  }
  return (double)sqrtl(e / r);
}

static void arm_fft(void) {
  g_arm = "fft";
  const uint32_t nmax = 1u << 19;
  float *a = malloc(nmax * sizeof *a), *b = malloc(nmax * sizeof *b);
  double *dr = malloc(nmax * sizeof *dr), *di = malloc(nmax * sizeof *di);
  long double *lr = malloc(nmax * sizeof *lr), *li = malloc(nmax * sizeof *li);
  ok(a && b && dr && di && lr && li, "the fft buffers hold 2^19 points", nmax, nmax);
  if (!(a && b && dr && di && lr && li)) { free(a); free(b); free(dr); free(di); free(lr); free(li); return; }

  /* A: bit-exact with the engine's transforms on the fixed vector set. */
  const uint64_t got = fft_digest(a, b);
  ok(got == FFT_ENGINE_DIGEST, "window, complex f32 and real f32 transforms are bit-exact with the engine's",
     (double)(got != FFT_ENGINE_DIGEST), 0.0);

  /* B: the reference is a DFT: against a direct long-double sum at 64 points. */
  {
    const uint32_t n = 64;
    fft_fill(a, n, 0x1234u); fft_fill(b, n, 0x4321u);
    for (uint32_t i = 0; i < n; i++) { lr[i] = a[i]; li[i] = b[i]; }
    fft_ref(lr, li, n, 0);
    double worst = 0.0;
    for (uint32_t k = 0; k < n; k++) {
      long double sr = 0.0L, si = 0.0L;
      for (uint32_t j = 0; j < n; j++) {
        const long double ang = -2.0L * 3.14159265358979323846264338327950288L * (long double)((j * k) % n) / (long double)n;
        sr += (long double)a[j] * cosl(ang) - (long double)b[j] * sinl(ang);
        si += (long double)a[j] * sinl(ang) + (long double)b[j] * cosl(ang);
      }
      const double e = (double)fabsl(sr - lr[k]) + (double)fabsl(si - li[k]);
      if (e > worst) worst = e;
    }
    ok(worst < 1e-15, "the long-double reference is the direct DFT to 1e-15 at 64 points", worst, 1e-15);
  }

  double worst_f = 0.0, worst_d = 0.0, floor_dbc = 0.0;
  /* C: f32 accuracy against the reference, 1024..65536. */
  for (uint32_t n = 1024; n <= 65536u; n <<= 1) {
    fft_fill(a, n, 0x77u); fft_fill(b, n, 0x88u);
    for (uint32_t i = 0; i < n; i++) { lr[i] = a[i]; li[i] = b[i]; }
    omx_fft_radix2(a, b, n);
    fft_ref(lr, li, n, 0);
    const double ec = fft_rel_f(a, b, lr, li, n);
    if (ec > worst_f) worst_f = ec;
    ok(ec < 2e-7, "complex f32 relRMS against the long-double reference", ec, 2e-7);

    fft_fill(a, n, 0x99u);
    for (uint32_t i = 0; i < n; i++) { lr[i] = a[i]; li[i] = 0.0L; }
    omx_fft_real_half(a, b, n);
    fft_ref(lr, li, n, 0);
    const double er = fft_rel_f(a, b, lr, li, n / 2);
    if (er > worst_f) worst_f = er;
    ok(er < 2e-7, "real f32 relRMS (n/2 bins) against the long-double reference", er, 2e-7);
  }

  /* D: the transform's own floor: a full-scale bin-centred sine at 16384, windowed, real
   * transform; the largest per-bin error against the reference on the same windowed input sits
   * below -130 dBc of the peak (a float twiddle recurrence put it near -95). */
  {
    const uint32_t n = 16384;
    for (uint32_t i = 0; i < n; i++) a[i] = (float)sin(2.0 * M_PI * 683.0 * (double)i / (double)n);
    omx_hann_window(a, n);
    for (uint32_t i = 0; i < n; i++) { lr[i] = a[i]; li[i] = 0.0L; }
    omx_fft_real_half(a, b, n);
    fft_ref(lr, li, n, 0);
    double peak = 0.0, err = 0.0;
    for (uint32_t k = 0; k < n / 2; k++) {
      const double m = (double)hypotl(lr[k], li[k]);
      const double e = (double)hypotl((long double)a[k] - lr[k], (long double)b[k] - li[k]);
      if (m > peak) peak = m;
      if (e > err) err = e;
    }
    floor_dbc = 20.0 * log10(err / peak);
    ok(floor_dbc < -130.0, "the real transform's worst bin error at 16384, dBc of a full-scale sine", floor_dbc, -130.0);
  }

  /* E: f64 forward and inverse against the reference, 1024..2^19 (the room sweep's sizes). The
   * twiddle recurrence compounds in double across a stage: measured relRMS 2.2e-12 forward and
   * 3.2e-12 inverse at 2^19, a 2.3e-15 floor at 1024; the limit is 1e-11. */
  for (uint32_t n = 1024; n <= nmax; n <<= 2) {
    fft_fill(a, n, 0x55u); fft_fill(b, n, 0x66u);
    for (uint32_t i = 0; i < n; i++) { dr[i] = lr[i] = a[i]; di[i] = li[i] = b[i]; }
    omx_fft_radix2_d(dr, di, n, 0);
    fft_ref(lr, li, n, 0);
    const double ef = fft_rel_d(dr, di, lr, li, n);
    if (ef > worst_d) worst_d = ef;
    ok(ef < 1e-11, "complex f64 forward relRMS against the long-double reference", ef, 1e-11);
    /* the inverse on its own input, then the round trip from x */
    fft_fill(a, n, 0x5au); fft_fill(b, n, 0x6bu);
    for (uint32_t i = 0; i < n; i++) { dr[i] = lr[i] = a[i]; di[i] = li[i] = b[i]; }
    omx_fft_radix2_d(dr, di, n, 1);
    fft_ref(lr, li, n, 1);
    const double ei = fft_rel_d(dr, di, lr, li, n);
    if (ei > worst_d) worst_d = ei;
    ok(ei < 1e-11, "complex f64 inverse relRMS against the long-double reference", ei, 1e-11);
    fft_fill(a, n, 0x55u); fft_fill(b, n, 0x66u);
    for (uint32_t i = 0; i < n; i++) { dr[i] = lr[i] = a[i]; di[i] = li[i] = b[i]; }
    omx_fft_radix2_d(dr, di, n, 0);
    omx_fft_radix2_d(dr, di, n, 1);
    const double et = fft_rel_d(dr, di, lr, li, n);
    ok(et < 1e-11, "complex f64 inverse(forward(x)) returns x", et, 1e-11);
  }

  /* F: the lengths the transforms take. */
  ok(omx_fft_length_ok(2) && omx_fft_length_ok(65536) && !omx_fft_length_ok(0) && !omx_fft_length_ok(1) &&
         !omx_fft_length_ok(96) && !omx_fft_length_ok(3),
     "omx_fft_length_ok takes exactly the powers of two from 2", 0.0, 0.0);

  printf("  fft: engine digest %s; f32 relRMS worst %.3g, floor %.1f dBc at 16384; f64 relRMS worst %.3g\n",
         got == FFT_ENGINE_DIGEST ? "bit-exact" : "DIFFERS", worst_f, floor_dbc, worst_d);
  expect_clean();
  free(a); free(b); free(dr); free(di); free(lr); free(li);
}
