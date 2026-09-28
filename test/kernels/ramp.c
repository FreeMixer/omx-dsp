
/* ---- ramp ----------------------------------------------------------------------------------- */

static void arm_ramp(void) {
  g_arm = "ramp";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const uint32_t n = (uint32_t)(OMX_DECLARED_RATES[ri] / 375.0f);
    float cur = 0.25f;
    const float tgt = 0.75f;
    const struct omx_ramp r = omx_ramp_begin(&cur, tgt, n);
    const float step = (tgt - cur) / (float)n;
    int affine = 1;
    for (uint32_t i = 0; i < n; i++)
      if (omx_ramp_at(r, i) != cur + step * (float)i) affine = 0;
    ok(affine, "the value at every sample is cur + step*i", affine, 1.0);
    ok(fabsf(omx_ramp_at(r, n) - tgt) < 1e-6f, "the affine line reaches the target at n", omx_ramp_at(r, n), tgt);
    omx_ramp_end(r, &cur);
    ok(cur == tgt, "closing stores the target itself", cur, tgt);

    float drift = 0.0f;
    for (uint32_t b = 0; b < 1000; b++) {
      const float t = (b & 1u) ? 0.1f : 0.3f;
      const struct omx_ramp rb = omx_ramp_begin(&drift, t, n);
      omx_ramp_end(rb, &drift);
      if (drift != t) break;
    }
    ok(drift == 0.1f, "a thousand blocks end exactly on the last target", drift, 0.1);

    float poisoned = NAN;
    const struct omx_ramp h = omx_ramp_begin(&poisoned, INFINITY, n);
    ok(h.cur == 0.0f && h.tgt == 0.0f && h.step == 0.0f, "a non-finite start and target heal to 0", h.cur, 0.0);
    float half = 0.5f;
    const struct omx_ramp hn = omx_ramp_begin(&half, NAN, n);
    ok(hn.tgt == 0.0f && omx_ramp_at(hn, 0) == 0.5f, "a NaN target ramps the finite start to 0", hn.tgt, 0.0);
  }
}
