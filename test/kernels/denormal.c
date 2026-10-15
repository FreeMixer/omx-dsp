// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- denormal ------------------------------------------------------------------------------- */

static void arm_denormal(void) {
  g_arm = "denormal";
  ok(omx_flush(1e-30f) == 0.0f, "a value below the floor flushes to zero", 1e-30, 0.0);
  ok(omx_flush(-1e-30f) == 0.0f, "a negative value below the floor flushes to zero", -1e-30, 0.0);
  ok(omx_flush(1e-19f) == 1e-19f, "a value above the floor is untouched", 1e-19, 1e-19);
  ok(omx_flush(-0.5f) == -0.5f, "an ordinary value is untouched", -0.5, -0.5);
  omx_denormals_off();
#if defined(__x86_64__) || defined(__i386__)
  ok((_mm_getcsr() & 0x8040u) == 0x8040u, "FTZ and DAZ are set for this thread", (double)(_mm_getcsr() & 0x8040u), 0x8040);
#endif
#if defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || (defined(__arm__) && defined(__ARM_FP))
  /* the mode's effect, not its bit: a subnormal operand reads as zero and a subnormal result is
   * zero (1e-40f and 2e-45f are below FLT_MIN, 1.17549435e-38f). volatile keeps the compiler from
   * folding the products at build time, where no flush-to-zero mode applies. */
  {
    volatile float sub = 1e-40f, one = 1.0f, half = 0.5f, flt_min = 1.17549435e-38f;
    const float in_zero = sub * one;
    const float out_zero = flt_min * half;
    ok(in_zero == 0.0f, "a subnormal operand reads as zero once denormals are off", in_zero, 0.0);
    ok(out_zero == 0.0f, "a subnormal result is exactly zero once denormals are off", out_zero, 0.0);
  }
#endif
  expect_clean();
}
