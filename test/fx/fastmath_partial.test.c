// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// A libmvec that has some of the four functions and not the others is the plain path for every loop that
// needs one it lacks. aarch64 glibc before 2.40 has log10f and expf but no powf and no tanhf; the dynamics
// gain loops call log10f AND powf, the drive's soft curve tanhf. Built with that set
// (fastmath_partial_render.c, `make fastmath-partial`):
//   LANES  the dynamics loops do not take the branch-free lanes and the drive's tanhf loop is not vectorised;
//   VALUE  every case renders bit for bit what the plain libm build renders.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OMX_PARTIAL_CASES 6u
#define OMX_PARTIAL_N 8192u

int partial_lanes(void);
int plain_lanes(void);
void partial_render(float *out);
void plain_render(float *out);

static const char *const names[OMX_PARTIAL_CASES] = {"ABOVE soft knee", "ABOVE hard knee", "UPWARD",
                                                     "BELOW knee off centre", "BELOW knee at +knee/2",
                                                     "drive tanh"};

int main(void) {
  static float a[OMX_PARTIAL_CASES * OMX_PARTIAL_N], b[OMX_PARTIAL_CASES * OMX_PARTIAL_N];
  int failed = 0, checks = 0;
  const int lanes = partial_lanes();
  checks += 2;
  if (lanes & 1) {
    printf("FAIL [LANES] log10f and expf without powf: the dynamics loops take the branch-free lanes\n");
    failed++;
  }
  if (lanes & 2) {
    printf("FAIL [LANES] log10f and expf without tanhf: the drive's tanhf loop is marked for vectorising\n");
    failed++;
  }
  checks++;
  if (plain_lanes() != 0) {
    printf("FAIL [LANES] the plain build takes a lane (%d)\n", plain_lanes());
    failed++;
  }
  partial_render(a);
  plain_render(b);
  for (uint32_t c = 0; c < OMX_PARTIAL_CASES; c++) {
    long mis = 0;
    for (uint32_t i = 0; i < OMX_PARTIAL_N; i++)
      mis += memcmp(&a[c * OMX_PARTIAL_N + i], &b[c * OMX_PARTIAL_N + i], sizeof(float)) != 0;
    checks++;
    if (mis) {
      printf("FAIL [VALUE] %s: %ld of %u samples differ from the plain libm build\n", names[c], mis, OMX_PARTIAL_N);
      failed++;
    }
  }
  printf("fx/fastmath_partial: %d checks, %d failed\n", checks, failed);
  return failed ? 1 : 0;
}
