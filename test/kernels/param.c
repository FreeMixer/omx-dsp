// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <float.h>

/* ---- param: the clamp family and its NaN law (spec §1 row 23) ------------------------------- */

static void arm_param(void) {
  g_arm = "param";
  const float lo = -0.9f, hi = 0.9f;
  const float dn = 1.4e-45f; /* the smallest positive denormal */
  /* omx_clampf: NaN -> floor, ±Inf saturate, inside passes bit for bit. Rate-free: no rate enters. */
  ok(omx_clampf(NAN, lo, hi) == lo, "clampf: NaN reads as the floor", omx_clampf(NAN, lo, hi), lo);
  ok(omx_clampf(-NAN, lo, hi) == lo, "clampf: a negative NaN reads as the floor", omx_clampf(-NAN, lo, hi), lo);
  ok(omx_clampf(INFINITY, lo, hi) == hi, "clampf: +Inf saturates to the ceiling", omx_clampf(INFINITY, lo, hi), hi);
  ok(omx_clampf(-INFINITY, lo, hi) == lo, "clampf: -Inf saturates to the floor", omx_clampf(-INFINITY, lo, hi), lo);
  ok(omx_clampf(lo, lo, hi) == lo && omx_clampf(hi, lo, hi) == hi, "clampf: both edges are kept", lo, hi);
  ok(omx_clampf(2.0f, lo, hi) == hi && omx_clampf(-2.0f, lo, hi) == lo, "clampf: beyond the travel is the edge", 2.0, hi);
  const float in[] = {0.0f, -0.0f, dn, -dn, 0.123456789f, -0.5f, 0.8999999f};
  for (unsigned i = 0; i < sizeof in / sizeof in[0]; i++) {
    const float c = omx_clampf(in[i], lo, hi);
    ok(memcmp(&c, &in[i], sizeof c) == 0, "clampf: inside the travel passes bit for bit", c, in[i]);
    const float d = omx_clamp_or(in[i], lo, hi, 0.0f);
    ok(memcmp(&d, &in[i], sizeof d) == 0, "clamp_or: inside the travel passes bit for bit", d, in[i]);
  }
  ok(omx_clampf(5.0f, 3.0f, 3.0f) == 3.0f && omx_clampf(NAN, 3.0f, 3.0f) == 3.0f, "clampf: a one-point travel is that point", 3.0, 3.0);

  /* omx_clamp_or: every non-finite value reads as the declared neutral value. */
  ok(omx_clamp_or(NAN, lo, hi, 0.0f) == 0.0f, "clamp_or: NaN reads as the default", omx_clamp_or(NAN, lo, hi, 0.0f), 0.0);
  ok(omx_clamp_or(INFINITY, lo, hi, 0.0f) == 0.0f, "clamp_or: +Inf reads as the default", omx_clamp_or(INFINITY, lo, hi, 0.0f), 0.0);
  ok(omx_clamp_or(-INFINITY, lo, hi, 0.25f) == 0.25f, "clamp_or: -Inf reads as the default", omx_clamp_or(-INFINITY, lo, hi, 0.25f), 0.25);
  ok(omx_clamp_or(2.0f, lo, hi, 0.0f) == hi && omx_clamp_or(-2.0f, lo, hi, 0.0f) == lo,
     "clamp_or: a finite value beyond the travel is the edge, not the default", 2.0, hi);
  ok(omx_clamp_or(FLT_MAX, lo, hi, 0.0f) == hi, "clamp_or: FLT_MAX is finite and saturates", omx_clamp_or(FLT_MAX, lo, hi, 0.0f), hi);

  /* omx_unit: [0, 1], NaN and -Inf are 0, +Inf is 1. */
  ok(omx_unit(NAN) == 0.0f, "unit: NaN is 0", omx_unit(NAN), 0.0);
  ok(omx_unit(INFINITY) == 1.0f, "unit: +Inf is 1", omx_unit(INFINITY), 1.0);
  ok(omx_unit(-INFINITY) == 0.0f, "unit: -Inf is 0", omx_unit(-INFINITY), 0.0);
  ok(omx_unit(0.5f) == 0.5f && omx_unit(1.5f) == 1.0f && omx_unit(-dn) == 0.0f, "unit: inside kept, outside the edge", 0.5, 0.5);
  /* A dense sweep: every output inside, every inside input unchanged — the POST holds everywhere. */
  for (float v = -3.0f; v <= 3.0f; v += 0.001953125f) {
    const float c = omx_clampf(v, lo, hi), u = omx_unit(v), d = omx_clamp_or(v, lo, hi, 0.0f);
    if (!(c >= lo && c <= hi && u >= 0.0f && u <= 1.0f && d == c)) {
      ok(0, "sweep: a clamp left its travel", v, 0.0);
      break;
    }
  }
  expect_clean();
}
