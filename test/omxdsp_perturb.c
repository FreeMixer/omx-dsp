// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omxdsp_perturb.c — built twice by test/perturb.sh: against a perturbed omx_contract_limits.h
 * (OMXDSP_PERTURBED defined; 96000 dropped from the declared rates, the comp's ratio floor and the
 * opto release's fastMs moved) and against the real one.
 * The perturbed build must see the drop through the contracts; the control must not.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omxdsp.h>

#include <stdio.h>
#include <string.h>

int main(void) {
  int failed = 0;
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
#endif
  omx_contract_reset();
  const struct omx_gaincomp_params gc = {OMX_DYN_ABOVE, -20.0f, 1.25f, 0.0f, 0.0f, 1.0f};
  omx_gaincomp_db(&gc, -10.0f);
  const uint32_t gc_count = omx_contract_log.count;
#ifdef OMXDSP_PERTURBED
  if (gc_count != 1u || strcmp(omx_contract_log.rec[0].token, "ratio-at-least-one") != 0 ||
      strcmp(omx_contract_log.rec[0].stage, "gaincomp/db") != 0) {
    printf("FAIL perturbed: expected exactly one gaincomp/db ratio-at-least-one violation, got %u\n", gc_count);
    failed++;
  }
  printf("omxdsp_perturb (perturbed header): ratio 1.25 under the moved floor 1.5 recorded by gaincomp/db; %d failed\n", failed);
#else
  if (!declared) { printf("FAIL control: 96000 is not declared against the real header\n"); failed++; }
  if (count != 0u) { printf("FAIL control: %u violations against the real header\n", count); failed++; }
  printf("omxdsp_perturb (real header): 96000 declared, nothing recorded; %d failed\n", failed);
#endif
#ifndef OMXDSP_PERTURBED
  if (gc_count != 0u) { printf("FAIL control: ratio 1.25 recorded %u violations against the real header\n", gc_count); failed++; }
  printf("omxdsp_perturb (real header): ratio 1.25 above the declared floor, nothing recorded; %d failed\n", failed);
#endif
  struct omx_env_program_release_params pr;
  omx_env_program_release_opto(&pr, 48000.0f, 1u);
#ifdef OMXDSP_PERTURBED
  if (pr.fast_pole != omx_pole_from_time_ms(35.0f, 48000.0f)) {
    printf("FAIL perturbed: the opto fast pole did not follow the moved fastMs — a copy of the profile\n");
    failed++;
  }
  printf("omxdsp_perturb (perturbed header): the opto release's fast pole is the moved 35 ms; %d failed\n", failed);
#else
  if (pr.fast_pole != omx_pole_from_time_ms(OMX_PROGRAM_RELEASE_OPTO_FAST_MS, 48000.0f) ||
      pr.fast_pole == omx_pole_from_time_ms(35.0f, 48000.0f)) {
    printf("FAIL control: the opto fast pole is not the declared fastMs\n");
    failed++;
  }
  printf("omxdsp_perturb (real header): the opto release's fast pole is the declared fastMs; %d failed\n", failed);
#endif
  return failed == 0 ? 0 : 1;
}
