
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
  expect_clean();
}
