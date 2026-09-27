// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omxdsp_perturb.c — built twice by test/perturb.sh: against a perturbed omx_contract_limits.h
 * (OMXDSP_PERTURBED defined; 96000 dropped from the declared rates) and against the real one.
 * The perturbed build must see the drop through the contracts; the control must not. Built again
 * with OMXDSP_PERTURBED_XOVER_Q (the LR4 section Q moved) and OMXDSP_PERTURBED_TOLERANCES (the
 * all-pass and crossover tolerances moved to 1e-12): each must record what the move predicts.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omxdsp.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

/* One all-pass block and one LR4 crossover block over the same deterministic signal: returns the
 * violations the two recorded, and writes the measured relative energy error and partition error. */
static uint32_t run_primitives(double *energy_err, double *partition_err) {
  float x[512], lo[512], hi[512], ap[512];
  uint32_t seed = 0x1234567u;
  for (int i = 0; i < 512; i++) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    x[i] = ap[i] = 0.5f * ((float)(seed >> 8) / 16777216.0f * 2.0f - 1.0f);
  }
  const double K = tan(M_PI * 700.0 / 48000.0);
  const float a = (float)omx_allpass1_coeff(700.0, 48000.0);
  struct omx_allpass1_state s = {0.0f};
  omx_contract_reset();
  float y[512];
  memcpy(y, x, sizeof y);
  omx_allpass1_process(y, 512u, a, &s);
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

static int count_token(const char *token) {
  int n = 0;
  const uint32_t kept = omx_contract_log.count < OMX_CONTRACT_MAX ? omx_contract_log.count : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++)
    if (strcmp(omx_contract_log.rec[i].token, token) == 0) n++;
  return n;
}

int main(void) {
  int failed = 0;
#if defined(OMXDSP_PERTURBED_XOVER_Q) || defined(OMXDSP_PERTURBED_TOLERANCES)
  double energy_err, partition_err;
  const uint32_t recorded = run_primitives(&energy_err, &partition_err);
  const int unity = count_token("unity-magnitude"), partition = count_token("bands-partition-unity");
#ifdef OMXDSP_PERTURBED_XOVER_Q
  if (OMX_XOVER_LR4_SECTION_Q != 0.6) { printf("FAIL xover-q: the header's Q did not move\n"); failed++; }
  if (partition_err <= OMX_XOVER_PARTITION_TOL) {
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
  const int want_unity = energy_err > OMX_ALLPASS_UNITY_TOL, want_partition = partition_err > OMX_XOVER_PARTITION_TOL;
  if (!want_unity || !want_partition) { printf("FAIL tolerances: the measured errors do not exceed 1e-12\n"); failed++; }
  if (unity != want_unity || partition != want_partition) {
    printf("FAIL tolerances: predicted %d unity / %d partition from the measured %.3g / %.3g, recorded %d / %d\n",
           want_unity, want_partition, energy_err, partition_err, unity, partition);
    failed++;
  }
  printf("omxdsp_perturb (tolerances moved to 1e-12): measured %.3g energy / %.3g partition, the postconditions "
         "recorded exactly the predicted violations; %d failed\n", energy_err, partition_err, failed);
#endif
  return failed == 0 ? 0 : 1;
#else
  omx_contract_reset();
  const int declared = omx_rate_is_declared(96000.0f);
  omx_pole_from_time_ms(1.0f, 96000.0f);
  const uint32_t count = omx_contract_log.count;
#ifdef OMXDSP_PERTURBED
  if (declared) { printf("FAIL perturbed: 96000 still reads as declared — a hardcoded rate list\n"); failed++; }
  if (count != 1u || strcmp(omx_contract_log.rec[0].token, "rate-is-declared") != 0 ||
      strcmp(omx_contract_log.rec[0].stage, "onepole/from-time-ms") != 0) {
    printf("FAIL perturbed: expected exactly one onepole/from-time-ms rate-is-declared violation, got %u\n", count);
    failed++;
  }
  if (OMX_DECLARED_RATE_COUNT != 5u) { printf("FAIL perturbed: the count did not follow the list\n"); failed++; }
  printf("omxdsp_perturb (perturbed header): 96000 refused, the rate precondition recorded it; %d failed\n", failed);
#else
  if (!declared) { printf("FAIL control: 96000 is not declared against the real header\n"); failed++; }
  if (count != 0u) { printf("FAIL control: %u violations against the real header\n", count); failed++; }
  printf("omxdsp_perturb (real header): 96000 declared, nothing recorded; %d failed\n", failed);
  double energy_err, partition_err;
  const uint32_t recorded = run_primitives(&energy_err, &partition_err);
  if (recorded != 0u) { printf("FAIL control: the all-pass and crossover recorded %u violations against the real header\n", recorded); failed++; }
  printf("omxdsp_perturb (real header): all-pass energy %.3g, crossover partition %.3g, nothing recorded; %d failed\n",
         energy_err, partition_err, failed);
#endif
  return failed == 0 ? 0 : 1;
#endif
}
