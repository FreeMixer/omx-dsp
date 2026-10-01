// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

static void arm_version(void) {
  g_arm = "version";
  ok(omxdsp_version() == ((uint32_t)OMXDSP_VERSION_MAJOR << 16 | (uint32_t)OMXDSP_VERSION_MINOR << 8 | (uint32_t)OMXDSP_VERSION_PATCH),
     "the version packs major, minor and patch", (double)omxdsp_version(), 0.0);
  expect_clean();
}
