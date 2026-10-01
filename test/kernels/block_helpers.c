// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

static void arm_block_helpers(void) {
  g_arm = "block-helpers";
  float b[8];
  for (int i = 0; i < 8; i++) b[i] = rnd(-1.0f, 1.0f);
  ok(omx_block_finite(b, 8u), "a finite block is finite", 0.0, 1.0);
  ok(omx_block_finite(NULL, 8u), "a NULL block promises nothing and passes", 0.0, 1.0);
  b[3] = NAN;
  ok(!omx_block_finite(b, 8u), "a NaN is seen", 0.0, 0.0);
  b[3] = INFINITY;
  ok(!omx_block_finite(b, 8u), "an infinity is seen", 0.0, 0.0);
  b[3] = -INFINITY;
  ok(!omx_block_finite(b, 8u), "a negative infinity is seen", 0.0, 0.0);
  b[3] = 0.25f;
  ok(omx_lane_finite(b, NULL, 8u), "a mono lane is finite on its one leg", 0.0, 1.0);
  float r[8] = {0};
  r[7] = NAN;
  ok(!omx_lane_finite(b, r, 8u), "a lane's R leg is checked", 0.0, 0.0);
  float m[4] = {0.5f, -0.75f, 0.25f, 0.0f};
  ok(omx_block_absmax(m, 4u) == 0.75f, "absmax is the largest magnitude", (double)omx_block_absmax(m, 4u), 0.75);
  ok(omx_block_absmax(NULL, 4u) == 0.0f, "absmax of NULL is 0", 0.0, 0.0);
  expect_clean();
}
