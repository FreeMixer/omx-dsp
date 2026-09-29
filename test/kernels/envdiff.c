
/* ---- envdiff (row 8) ----------------------------------------------------------------------- */

static double cascade_step(uint32_t n, double q);

/* The transient spec §2 peak contrasts, dB, ΔS then ΔA: the same at every rate (L9). */
static const double ENVDIFF_PEAK_DB[2] = {19.753, 19.784};

struct envdiff_run { double worst, bound, peak_w, peak_c, q, slope_from, slope_to; uint32_t at_c, from_w, from_c, to_w, to_c; };

/* The 0.1 -> 1.0 step (rise = 1, ΔA) or 1.0 -> 0.1 (rise = 0, ΔS) at the declared defaults, the
 * word against the closed form on the float32 per-stage poles it runs. */
static struct envdiff_run envdiff_step_run(float sr, int rise) {
  const float slow_ms = rise ? OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT : OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT;
  const struct omx_env_params fe = {omx_pole_from_time_ms(OMX_TRANSIENT_FAST_ATTACK_MS, sr),
                                    omx_pole_from_time_ms(OMX_TRANSIENT_FAST_RELEASE_MS, sr), OMX_DETECT_PEAK};
  const struct omx_env_params se = {rise ? omx_pole_from_time_ms(slow_ms, sr) : fe.attack_pole,
                                    rise ? fe.release_pole : omx_pole_from_time_ms(slow_ms, sr), OMX_DETECT_PEAK};
  struct omx_envdiff_poles p;
  omx_env_stage_poles(&fe, 1u, &p.fast_attack, &p.fast_release);
  omx_env_stage_poles(&se, 1u, &p.slow_attack, &p.slow_release);
  const double a = rise ? 0.1 : 1.0, b = rise ? 1.0 : 0.1;
  const double qf = rise ? p.fast_attack : p.fast_release, qs = rise ? p.slow_attack : p.slow_release;
  struct envdiff_run r = {0.0, 0.0, -1.0, -1.0, qs, 0.0, 0.0, 0u, 0u, 0u, 0u, 0u};
  double dc_prev = 0.0;
  r.bound = 20.0 * log10(1.0 + OMX_DYN_ENV_STAGES * 3.0 * ldexp(1.0, -24) / (1.0 - qs)) + 2e-4;
  struct omx_env fast, slow;
  for (int s = 0; s < OMX_DYN_ENV_STAGES; s++) fast.stage[s] = slow.stage[s] = (float)a;
  const uint32_t len = (uint32_t)((rise ? 0.02f : 0.5f) * sr);
  for (uint32_t i = 0; i < len; i++) {
    const float fl = omx_env_step(&fast, &fe, (float)b, p.fast_attack, p.fast_release);
    const float dw = omx_envdiff_step(&slow, &fe, (float)b, fl, &p, OMX_TRANSIENT_FLOOR_LIN);
    const double ef = a + (b - a) * cascade_step(i, qf), es = a + (b - a) * cascade_step(i, qs);
    const double dc = rise ? 20.0 * log10(ef / es) : 20.0 * log10(es / ef);
    if (fabs(dw - dc) > r.worst) r.worst = fabs(dw - dc);
    if (dw > r.peak_w) r.peak_w = dw;
    if (dc > r.peak_c) { r.peak_c = dc; r.at_c = i; }
    if (dw >= OMX_TRANSIENT_REF_DB) { if (!r.from_w) r.from_w = i; r.to_w = i; }
    if (dc >= OMX_TRANSIENT_REF_DB) { if (!r.from_c) { r.from_c = i; r.slope_from = fabs(dc - dc_prev); } r.to_c = i; }
    else if (r.from_c && !r.slope_to) r.slope_to = fabs(dc - dc_prev);
    dc_prev = dc;
  }
  return r;
}

static void arm_envdiff(void) {
  g_arm = "envdiff";
  uint32_t tabled = 0u;
  double worst_ratio = 0.0;
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    for (int rise = 1; rise >= 0; rise--) {
      const struct envdiff_run r = envdiff_step_run(sr, rise);
      if (r.worst / r.bound > worst_ratio) worst_ratio = r.worst / r.bound;
      ok(r.worst <= r.bound, rise ? "ΔA on the rising step is the closed form" : "ΔS on the falling step is the closed form", r.worst, r.bound);
      ok(fabs(r.peak_w - r.peak_c) <= 0.01, "the word's peak contrast is the closed form's", r.peak_w, r.peak_c);
      const double tol_from = 1.0 + r.bound / r.slope_from, tol_to = 1.0 + r.bound / r.slope_to;
      ok(fabs((double)r.from_w - r.from_c) <= tol_from, "the ≥ REF window opens within 1 + bound/slope samples of the closed form", fabs((double)r.from_w - r.from_c), tol_from);
      ok(fabs((double)r.to_w - r.to_c) <= tol_to, "the ≥ REF window closes within 1 + bound/slope samples of the closed form", fabs((double)r.to_w - r.to_c), tol_to);
      ok(fabs(r.peak_c - ENVDIFF_PEAK_DB[rise]) <= 0.001, "the closed-form peak is the spec §2 table's, at every rate", r.peak_c, ENVDIFF_PEAK_DB[rise]);
      tabled++;
    }
    /* L2 and L3 over gated noise: both contrasts non-negative at every sample; identical at every
     * power-of-two level, within the arm A bound at the decimal ones. */
    const float k[7] = {1.0f, 0x1p-7f, 0x1p-3f, 0x1p3f, 1e-2f, 1e-1f, 10.0f};
    const struct omx_env_params fe = {omx_pole_from_time_ms(OMX_TRANSIENT_FAST_ATTACK_MS, sr),
                                      omx_pole_from_time_ms(OMX_TRANSIENT_FAST_RELEASE_MS, sr), OMX_DETECT_PEAK};
    const struct omx_env_params ae = {omx_pole_from_time_ms(OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT, sr), fe.release_pole, OMX_DETECT_PEAK};
    const struct omx_env_params re = {fe.attack_pole, omx_pole_from_time_ms(OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT, sr), OMX_DETECT_PEAK};
    struct omx_envdiff_poles pa, pr;
    omx_env_stage_poles(&fe, 1u, &pa.fast_attack, &pa.fast_release);
    pr.fast_attack = pa.fast_attack; pr.fast_release = pa.fast_release;
    omx_env_stage_poles(&ae, 1u, &pa.slow_attack, &pa.slow_release);
    omx_env_stage_poles(&re, 1u, &pr.slow_attack, &pr.slow_release);
    const double bound = 20.0 * log10(1.0 + OMX_DYN_ENV_STAGES * 3.0 * ldexp(1.0, -24) / (1.0 - pr.slow_release)) + 2e-4;
    const uint32_t len = (uint32_t)(1.0f * sr), gate = (uint32_t)(sr / 8.0f);
    static float ref_a[192000], ref_s[192000];
    uint32_t negative = 0u, pow2_diff = 0u;
    double worst_level = 0.0;
    for (int ki = 0; ki < 7; ki++) {
      struct omx_env f, sa, ss;
      memset(&f, 0, sizeof f); memset(&sa, 0, sizeof sa); memset(&ss, 0, sizeof ss);
      g_seed = 0x2f6e2b1du;
      for (uint32_t i = 0; i < len; i++) {
        const float amp = ((i / gate) & 1u) ? 0.1f : 0.9f;
        const float d = k[ki] * (amp * (0.5f + 0.5f * fabsf(rnd(-1.0f, 1.0f))));
        const float fl = omx_env_step(&f, &fe, d, pa.fast_attack, pa.fast_release);
        const float da = omx_envdiff_step(&sa, &fe, d, fl, &pa, OMX_TRANSIENT_FLOOR_LIN);
        const float ds = omx_envdiff_step(&ss, &fe, d, fl, &pr, OMX_TRANSIENT_FLOOR_LIN);
        negative += (da < 0.0f) + (ds < 0.0f);
        if (ki == 0) { ref_a[i] = da; ref_s[i] = ds; continue; }
        if (i < (uint32_t)(0.01f * sr)) continue;
        if (ki < 4) { pow2_diff += (da != ref_a[i]) + (ds != ref_s[i]); continue; }
        const double e = fmax(fabs(da - ref_a[i]), fabs(ds - ref_s[i]));
        if (e > worst_level) worst_level = e;
      }
    }
    ok(negative == 0u, "L2: both contrasts are non-negative at every sample (gated noise)", negative, 0.0);
    ok(pow2_diff == 0u, "L3: at x2^-7, x2^-3, x2^3 the contrasts are bit-identical", pow2_diff, 0.0);
    ok(worst_level <= bound, "L3: at x0.01, x0.1, x10 the contrasts agree within the arm A bound", worst_level, bound);
    struct omx_env f, s;
    memset(&f, 0, sizeof f); memset(&s, 0, sizeof s);
    float silent = 0.0f;
    for (uint32_t i = 0; i < 1000u; i++) {
      const float fl = omx_env_step(&f, &fe, 0.0f, pa.fast_attack, pa.fast_release);
      silent = fmaxf(silent, omx_envdiff_step(&s, &fe, 0.0f, fl, &pa, OMX_TRANSIENT_FLOOR_LIN));
    }
    ok(silent == 0.0f, "below the floor the contrast is exactly 0", silent, 0.0);
  }
  ok(tabled == 2u * OMX_DECLARED_RATE_COUNT, "every declared rate was checked against the §2 peaks, both steps", tabled, 2.0 * OMX_DECLARED_RATE_COUNT);
  printf("envdiff: worst step error %.3g of its bound, %u declared rates\n", worst_ratio, OMX_DECLARED_RATE_COUNT);
  expect_clean();
}
