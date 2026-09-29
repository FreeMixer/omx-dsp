
/* ---- program-dependent release ------------------------------------------------------------- */

/* O3: a hold at -10 dB, then 0; the reference is the product, in double, of the word's own float
 * poles, the memory stepped in closed form m0·p_dis^k. Returns the worst |gr - reference|, dB,
 * until the reference has recovered to 1 % of the hold. */
static double prel_release_error(float sr, float hold_ms, float *m0_out) {
  struct omx_env_program_release_params p;
  omx_env_program_release_opto(&p, sr, 1u);
  struct omx_env_program_release st;
  memset(&st, 0, sizeof st);
  const float g = -10.0f;
  const uint32_t hold = (uint32_t)(hold_ms * 0.001f * sr);
  for (uint32_t i = 0; i < hold; i++) omx_env_program_release(&st, &p, g);
  const double m0 = st.mem;
  *m0_out = st.mem;
  double fast = (1.0 - p.share) * g, slow = p.share * (double)g, m = m0, worst = 0.0;
  while (fast + slow < 0.01 * g) {
    const float gr = omx_env_program_release(&st, &p, 0.0f);
    fast *= p.fast_pole;
    slow *= omx_pole_from_time_ms(p.slow_min_ms + (float)m * p.slow_span_ms, sr);
    m *= p.discharge_pole;
    const double d = fabs((double)gr - (fast + slow));
    if (d > worst) worst = d;
  }
  return worst;
}

static void arm_program_release(void) {
  g_arm = "program-release";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    float m_long, m_short;
    const double e_long = prel_release_error(sr, 7.0f * OMX_PROGRAM_RELEASE_CHARGE_MS, &m_long);
    const double e_short = prel_release_error(sr, 0.2f * OMX_PROGRAM_RELEASE_CHARGE_MS, &m_short);
    printf("program-release O3 @ %.0f Hz: worst %.3g dB after a long hold (m0 %.5f), %.3g dB after a short one (m0 %.5f)\n",
           sr, e_long, m_long, e_short, m_short);
    ok(e_long <= 1e-3, "O3: the release after a long hold is the closed form of the word's poles", e_long, 1e-3);
    ok(e_short <= 1e-3, "O3: the release after a short hold is the closed form of the word's poles", e_short, 1e-3);
    struct omx_env_program_release_params p;
    omx_env_program_release_opto(&p, sr, 1u);
    const double stall = ldexp(1.0, -24) / (1.0 - (double)p.charge_pole);
    ok(fabs(m_long - (1.0 - exp(-7.0))) <= stall + 1e-4, "L4: 7 charge constants fill the memory to 1 - e^-7, short of it by at most the float stall", m_long, 1.0 - exp(-7.0));
    ok(fabs(m_short - (1.0 - exp(-0.2))) <= 1e-3, "L4: 0.2 charge constants fill it to 1 - e^-0.2", m_short, 1.0 - exp(-0.2));
    /* Each pole is its declared time constant at this rate: -1/(sr·ln p), within the float pole's spacing. */
    const double tf = -1000.0 / (sr * log((double)p.fast_pole)), tc = -1000.0 / (sr * log((double)p.charge_pole)),
                 td = -1000.0 / (sr * log((double)p.discharge_pole));
    ok(fabs(tf / OMX_PROGRAM_RELEASE_PROFILES_OPTO_FAST_MS - 1.0) <= 1e-3, "the fast pole is the declared fastMs", tf, OMX_PROGRAM_RELEASE_PROFILES_OPTO_FAST_MS);
    ok(fabs(tc / OMX_PROGRAM_RELEASE_CHARGE_MS - 1.0) <= 1e-2, "the charge pole is the declared chargeMs", tc, OMX_PROGRAM_RELEASE_CHARGE_MS);
    ok(fabs(td / OMX_PROGRAM_RELEASE_DISCHARGE_MS - 1.0) <= 5e-2, "the discharge pole is the declared dischargeMs", td, OMX_PROGRAM_RELEASE_DISCHARGE_MS);
    ok(p.slow_min_ms == OMX_PROGRAM_RELEASE_PROFILES_OPTO_SLOW_MIN_MS && p.slow_min_ms + p.slow_span_ms == OMX_PROGRAM_RELEASE_PROFILES_OPTO_SLOW_MAX_MS &&
           p.share == OMX_PROGRAM_RELEASE_PROFILES_OPTO_SHARE,
       "the slow constants and the share are the declared profile", p.slow_span_ms, OMX_PROGRAM_RELEASE_PROFILES_OPTO_SLOW_MAX_MS);
    /* L3: a deeper target is taken on the same sample by both components. */
    struct omx_env_program_release st;
    memset(&st, 0, sizeof st);
    omx_env_program_release(&st, &p, -3.0f);
    omx_env_program_release(&st, &p, -1.0f);
    omx_env_program_release(&st, &p, -12.0f);
    ok(st.fast == -12.0f && st.slow == -12.0f, "L3: attack is not slowed — a deeper target lands on the same sample", st.fast, -12.0);
    /* L7: a profile made at 4x the rate is the base-rate profile's continuous-time twin. */
    struct omx_env_program_release_params p4;
    omx_env_program_release_opto(&p4, sr, 4u);
    const double f4 = pow((double)p4.fast_pole, 4.0), c4 = pow((double)p4.charge_pole, 4.0);
    ok(fabs(f4 - p.fast_pole) <= 1e-5, "L7: four 4x fast steps are one base-rate step", f4, p.fast_pole);
    ok(fabs(c4 - p.charge_pole) <= 1e-5, "L7: four 4x charge steps are one base-rate step", c4, p.charge_pole);
    ok(p4.slow_min_ms == 4.0f * p.slow_min_ms && p4.slow_span_ms == 4.0f * p.slow_span_ms, "L7: the slow constants scale by the multiplier", p4.slow_min_ms, 4.0 * p.slow_min_ms);
    /* O7: a 0 dBFS burst's reduction, then silence: every state word is 0 or normal, every sample. */
    memset(&st, 0, sizeof st);
    uint32_t subnormal = 0u;
    const uint32_t burst = (uint32_t)(0.1f * sr), quiet = (uint32_t)(30.0f * sr);
    for (uint32_t i = 0; i < burst + quiet; i++) {
      const float gr = omx_env_program_release(&st, &p, i < burst ? -20.0f : 0.0f);
      subnormal += fpclassify(st.fast) == FP_SUBNORMAL || fpclassify(st.slow) == FP_SUBNORMAL ||
                   fpclassify(st.mem) == FP_SUBNORMAL || fpclassify(gr) == FP_SUBNORMAL;
    }
    ok(subnormal == 0u, "O7: no subnormal state word through 30 s of release", subnormal, 0.0);
    ok(st.fast == 0.0f && st.slow == 0.0f, "O7: both components land on exactly 0 dB", st.slow, 0.0);
  }
  ok(omx_env_program_release_state_size() == sizeof(struct omx_env_program_release) &&
         omx_env_program_release_state_align() == _Alignof(struct omx_env_program_release),
     "the state layout is exported", (double)omx_env_program_release_state_size(), sizeof(struct omx_env_program_release));
  expect_clean();
}
