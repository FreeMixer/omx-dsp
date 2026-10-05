// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- port_int: a control word as an integer (spec §1 row 23) -------------------------------- */

static void arm_port_int(void) {
  g_arm = "port_int";
  /* Non-finite reads as the default, never through an undefined float->int conversion. */
  ok(omx_port_int(NAN, 1, 4, 3) == 3, "port_int: NaN reads as the default", omx_port_int(NAN, 1, 4, 3), 3);
  ok(omx_port_int(INFINITY, 1, 4, 3) == 3, "port_int: +Inf reads as the default", omx_port_int(INFINITY, 1, 4, 3), 3);
  ok(omx_port_int(-INFINITY, 1, 4, 3) == 3, "port_int: -Inf reads as the default", omx_port_int(-INFINITY, 1, 4, 3), 3);
  /* A finite word beyond the travel is the edge — including one beyond an int's range. */
  ok(omx_port_int(1e30f, 1, 4, 3) == 4, "port_int: +1e30 is the maximum", omx_port_int(1e30f, 1, 4, 3), 4);
  ok(omx_port_int(-1e30f, 1, 4, 3) == 1, "port_int: -1e30 is the minimum", omx_port_int(-1e30f, 1, 4, 3), 1);
  /* Inside the travel a word rounds half away from zero. */
  ok(omx_port_int(2.5f, 1, 4, 3) == 3 && omx_port_int(2.49f, 1, 4, 3) == 2, "port_int: rounds half up", 2.5, 3);
  ok(omx_port_int(-2.5f, -4, 4, 0) == -3 && omx_port_int(-2.49f, -4, 4, 0) == -2,
     "port_int: rounds half away from zero below zero", -2.5, -3);
  for (int i = -4; i <= 4; i++)
    ok(omx_port_int((float)i, -4, 4, 0) == i, "port_int: an integer word is itself", i, i);
}
