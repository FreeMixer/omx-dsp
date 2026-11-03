// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * fastmath_cmp — two raw float dumps (the libm build's and the libmvec build's), the largest sample
 * difference between them in dB relative to full scale and to the reference's own peak.
 *
 *   fastmath_cmp OLD NEW LIMIT_DBFS [CHUNK]  with CHUNK floats per section, also prints each section's difference
 *                                        (dBFS) on a line of its own before the summary; prints "<max diff dBFS> <peak dBFS> <max diff re peak dB>"; exit 1 when
 *                                        the difference is above LIMIT_DBFS, 2 when the files cannot be read
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float *slurp(const char *path, size_t *n) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  rewind(f);
  float *b = malloc((size_t)sz + 4);
  *n = b ? fread(b, 1, (size_t)sz, f) / sizeof(float) : 0;
  fclose(f);
  return b;
}

int main(int argc, char **argv) {
  if (argc != 4 && argc != 5) return 2;
  size_t na = 0, nb = 0;
  float *a = slurp(argv[1], &na), *b = slurp(argv[2], &nb);
  if (!a || !b || na != nb || na == 0) { printf("fastmath_cmp: cannot read the dumps or their sizes differ (%zu, %zu floats)\n", na, nb); return 2; }
  double dmax = 0.0, peak = 0.0;
  for (size_t i = 0; i < na; i++) {
    if (a[i] != a[i] || b[i] != b[i] || fabsf(a[i]) > 1e30f || fabsf(b[i]) > 1e30f) { printf("not finite at %zu\n", i); return 1; }
    double d = fabs((double)a[i] - (double)b[i]);
    if (d > dmax) dmax = d;
    if (fabs((double)a[i]) > peak) peak = fabs((double)a[i]);
  }
  if (argc == 5) {
    size_t ch = (size_t)atol(argv[4]);
    for (size_t s0 = 0; ch && s0 < na; s0 += ch) {
      double m = 0.0;
      for (size_t i = s0; i < s0 + ch && i < na; i++) { double d = fabs((double)a[i] - (double)b[i]); if (d > m) m = d; }
      printf("section %zu: %.1f dBFS\n", s0 / ch, m > 0.0 ? 20.0 * log10(m) : -400.0);
    }
  }
  double dbfs = dmax > 0.0 ? 20.0 * log10(dmax) : -400.0, pk = peak > 0.0 ? 20.0 * log10(peak) : -400.0;
  printf("%.1f %.1f %.1f\n", dbfs, pk, dbfs - pk);
  return dbfs > atof(argv[3]) ? 1 : 0;
}
