// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * bench-mixmatrix.c — ns per strip-output-frame of omx_mixmatrix_dense() and
 * omx_mixmatrix_sparse() (lane/mix-matrix §3), contracts off, at 32/64/97 strips x 1024 frames.
 *
 * Both passes route every strip to the same OUT=16 outputs (a stereo main plus six stereo buses
 * and a stereo post-fader tap — OMX_BUS_INPUT_CAP's neighbourhood, never every strip to every
 * output in a real console). Dense computes the full strips*OUT grid; sparse is handed only the
 * 4 non-zero entries per strip a channel strip actually feeds (main L/R and one send L/R), so its
 * wall time is normalised by the SAME nominal strips*OUT*frames denominator as dense — the
 * ns/strip-output-frame it prints is the one that falls as the matrix gets sparser, which is the
 * number the "sparse-aware" decision is for. Every entry ramps (the worst case for both passes).
 * Prints one TSV row per strip count: strips, frames, dense_ns, sparse_ns.
 */
#include <omxdsp/omxdsp.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define OUT 16u
#define FRAMES 1024u
#define SENDS_PER_STRIP 4u /* main L/R + one send L/R: the sparse connection count per strip */
#define REPS 50u

static int cmp(const void *a, const void *b) {
  const double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

int main(void) {
  static const uint32_t strip_counts[] = {32u, 64u, 97u};
  uint32_t seed = 0x5eed5eedu;

  printf("strips\tframes\tdense_ns_per_sof\tsparse_ns_per_sof\n");
  for (size_t sc = 0; sc < sizeof strip_counts / sizeof strip_counts[0]; sc++) {
    const uint32_t n_strips = strip_counts[sc];
    const uint32_t n_entries = n_strips * SENDS_PER_STRIP;

    float *in_buf = calloc((size_t)n_strips * FRAMES, sizeof(float));
    float *out_buf = calloc((size_t)OUT * FRAMES, sizeof(float));
    float *g_prev = calloc((size_t)n_strips * OUT, sizeof(float));
    float *g_cur = calloc((size_t)n_strips * OUT, sizeof(float));
    const float **in_ptr = calloc(n_strips, sizeof *in_ptr);
    float **out_ptr = calloc(OUT, sizeof *out_ptr);
    struct omx_mixmatrix_entry *entries = calloc(n_entries, sizeof *entries);
    double *dense_ns = calloc(REPS, sizeof *dense_ns);
    double *sparse_ns = calloc(REPS, sizeof *sparse_ns);
    if (!in_buf || !out_buf || !g_prev || !g_cur || !in_ptr || !out_ptr || !entries || !dense_ns ||
        !sparse_ns) {
      fprintf(stderr, "bench-mixmatrix: allocation failed\n");
      return 1;
    }

    for (uint32_t s = 0; s < n_strips; s++) {
      in_ptr[s] = &in_buf[(size_t)s * FRAMES];
      for (uint32_t i = 0; i < FRAMES; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        in_buf[(size_t)s * FRAMES + i] = ((float)(seed >> 8) / 16777216.0f) * 2.0f - 1.0f;
      }
    }
    for (uint32_t o = 0; o < OUT; o++) out_ptr[o] = &out_buf[(size_t)o * FRAMES];

    /* dense: every strip feeds the first SENDS_PER_STRIP outputs, the rest of the row is zero */
    uint32_t e = 0;
    for (uint32_t s = 0; s < n_strips; s++) {
      for (uint32_t o = 0; o < OUT; o++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        const float gp = (float)(seed >> 8) / 16777216.0f;
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        const float gc = (float)(seed >> 8) / 16777216.0f;
        const int connected = o < SENDS_PER_STRIP;
        g_prev[o * n_strips + s] = connected ? gp : 0.0f;
        g_cur[o * n_strips + s] = connected ? gc : 0.0f;
        if (connected) {
          entries[e].strip = s;
          entries[e].out = o;
          entries[e].g_prev = gp;
          entries[e].g_cur = gc;
          e++;
        }
      }
    }

    for (uint32_t r = 0; r < REPS; r++) {
      const double a = now_ns();
      omx_mixmatrix_dense(out_ptr, in_ptr, g_prev, g_cur, n_strips, OUT, FRAMES);
      const double b = now_ns();
      dense_ns[r] = b - a;
    }
    for (uint32_t r = 0; r < REPS; r++) {
      const double a = now_ns();
      omx_mixmatrix_sparse(out_ptr, in_ptr, entries, n_entries, n_strips, OUT, FRAMES);
      const double b = now_ns();
      sparse_ns[r] = b - a;
    }
    qsort(dense_ns, REPS, sizeof dense_ns[0], cmp);
    qsort(sparse_ns, REPS, sizeof sparse_ns[0], cmp);
    const double sof = (double)n_strips * (double)OUT * (double)FRAMES;
    printf("%u\t%u\t%.4f\t%.4f\n", n_strips, FRAMES, dense_ns[REPS / 2u] / sof, sparse_ns[REPS / 2u] / sof);

    free(in_buf); free(out_buf); free(g_prev); free(g_cur);
    free(in_ptr); free(out_ptr); free(entries); free(dense_ns); free(sparse_ns);
  }
  return 0;
}
