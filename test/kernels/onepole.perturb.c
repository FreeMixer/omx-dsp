// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
static int perturb_onepole(int failed) {
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
#endif
  return failed;
}
