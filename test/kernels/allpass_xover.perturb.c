// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/* One all-pass block and one LR4 crossover block over the same deterministic signal: returns the
 * violations the two recorded, and writes the measured relative energy error and partition error.
 * Shared by the two special builds below (perturb.sh's `xover-q` and `tolerances`, each defining
 * ONE of OMXDSP_PERTURBED_XOVER_Q / OMXDSP_PERTURBED_TOLERANCES) and by the generic
 * OMXDSP_PERTURBED/control builds, where neither primitive's numbers moved and nothing is ever
 * predicted to record. */
static uint32_t allpass_xover_run(double *energy_err, double *partition_err) {
  float x[512], lo[512], hi[512], ap[512];
  uint32_t seed = 0x1234567u;
  for (int i = 0; i < 512; i++) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    x[i] = ap[i] = 0.5f * ((float)(seed >> 8) / 16777216.0f * 2.0f - 1.0f);
  }
  const double K = tan(M_PI * 700.0 / 48000.0);
  const float a = (float)omx_allpass1_coef_d(700.0, 48000.0);
  struct omx_allpass1 s = {0.0f};
  omx_contract_reset();
  float y[512];
  memcpy(y, x, sizeof y);
  omx_allpass1_block(&s, y, 512u, a);
  double ein = 0.0, eout = s.s * (double)s.s / K;
  for (int i = 0; i < 512; i++) { ein += (double)x[i] * x[i]; eout += (double)y[i] * y[i]; }
  *energy_err = fabs(eout - ein) / fmax(ein, eout);
  struct omx_xover c;
  struct omx_xover_state xs;
  struct omx_xover_ap_state as;
  memset(&xs, 0, sizeof xs);
  memset(&as, 0, sizeof as);
  omx_xover_design(&c, 4u, 1000.0, 48000.0);
  omx_xover_process(x, lo, hi, 512u, &c, &xs);
  const uint32_t recorded = omx_contract_log.count;
  omx_xover_allpass(ap, 512u, &c, &as);
  double worst = 0.0, peak = 0.0;
  for (int i = 0; i < 512; i++) {
    worst = fmax(worst, fabs((double)lo[i] + hi[i] - ap[i]));
    peak = fmax(peak, fabs(x[i]));
  }
  *partition_err = worst / fmax(1.0, peak);
  return recorded;
}

static int allpass_xover_count_token(const char *token) {
  int n = 0;
  const uint32_t kept = omx_contract_log.count < OMX_CONTRACT_MAX ? omx_contract_log.count : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++)
    if (strcmp(omx_contract_log.rec[i].token, token) == 0) n++;
  return n;
}

static int perturb_allpass_xover(int failed) {
#if defined(OMXDSP_PERTURBED_XOVER_Q) || defined(OMXDSP_PERTURBED_TOLERANCES)
  double energy_err, partition_err;
  const uint32_t recorded = allpass_xover_run(&energy_err, &partition_err);
  const int unity = allpass_xover_count_token("unity-magnitude");
  const int partition = allpass_xover_count_token("bands-partition-unity");
#ifdef OMXDSP_PERTURBED_XOVER_Q
  if (OMX_XOVER_LR4_SECTION_Q_DOUBLE != 0.6) { printf("FAIL xover-q: the header's Q did not move\n"); failed++; }
  if (partition_err <= OMX_XOVER_PARTITION_TOLERANCE) {
    printf("FAIL xover-q: an LR4 pair at Q 0.6 still sums to its all-pass (%.3g) — the design did not read the moved Q\n", partition_err);
    failed++;
  }
  if (partition != 1 || unity != 0 || recorded != 1u) {
    printf("FAIL xover-q: expected exactly one bands-partition-unity, got %d partition, %d unity, %u total\n", partition, unity, recorded);
    failed++;
  }
  printf("omxdsp_perturb (LR4 Q moved to 0.6): the pair designed at the moved Q, lo + hi off the all-pass by %.3g, "
         "the partition postcondition recorded it; %d failed\n", partition_err, failed);
#else
  const int want_unity = energy_err > OMX_ALLPASS_UNITY_TOLERANCE, want_partition = partition_err > OMX_XOVER_PARTITION_TOLERANCE;
  if (!want_unity || !want_partition) { printf("FAIL tolerances: the measured errors do not exceed 1e-12\n"); failed++; }
  if (unity != want_unity || partition != want_partition || recorded != (uint32_t)(want_unity + want_partition)) {
    printf("FAIL tolerances: predicted %d unity / %d partition from the measured %.3g / %.3g, recorded %d / %d\n",
           want_unity, want_partition, energy_err, partition_err, unity, partition);
    failed++;
  }
  printf("omxdsp_perturb (tolerances moved to 1e-12): measured %.3g energy / %.3g partition, the postconditions "
         "recorded exactly the predicted violations; %d failed\n", energy_err, partition_err, failed);
#endif
  return failed;
#else
  double energy_err, partition_err;
  const uint32_t recorded = allpass_xover_run(&energy_err, &partition_err);
  if (recorded != 0u || allpass_xover_count_token("unity-magnitude") + allpass_xover_count_token("bands-partition-unity") != 0) {
    printf("FAIL: the all-pass and crossover recorded %u violations over an unmoved Q and tolerances\n", recorded);
    failed++;
  }
  printf("omxdsp_perturb: all-pass energy %.3g, crossover partition %.3g under the unmoved primitive numbers, "
         "nothing recorded; %d failed\n", energy_err, partition_err, failed);
  return failed;
#endif
}
