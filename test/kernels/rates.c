// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

static void arm_rates(void) {
  g_arm = "rates";
  ok(OMX_DECLARED_RATE_COUNT >= 4u, "the declaration carries at least the four basic rates", (double)OMX_DECLARED_RATE_COUNT, 4.0);
  for (uint32_t i = 0; i < OMX_DECLARED_RATE_COUNT; i++) {
    ok(omx_rate_is_declared(OMX_DECLARED_RATES[i]), "a declared rate is declared", OMX_DECLARED_RATES[i], 1.0);
    ok(OMX_RATE_IS_DECLARED(OMX_DECLARED_RATES[i]), "the macro forwards to the table", OMX_DECLARED_RATES[i], 1.0);
  }
  const float not_declared[] = {0.0f, -48000.0f, 22050.0f, 96001.0f, 384000.0f, NAN};
  for (size_t i = 0; i < sizeof not_declared / sizeof not_declared[0]; i++)
    ok(!omx_rate_is_declared(not_declared[i]), "an undeclared rate is refused", not_declared[i], 0.0);
  expect_clean();
}
