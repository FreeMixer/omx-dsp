// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
static int perturb_gaincomp(int failed) {
  omx_contract_reset();
  const struct omx_gaincomp_params gc = {OMX_DYN_ABOVE, -20.0f, OMXDSP_COMP_RATIO_BETWEEN, OMX_COMP_KNEE_DB_MIN, 0.0f, 1.0f};
  omx_gaincomp_db(&gc, -10.0f);
  const uint32_t gc_count = omx_contract_log.count;
#ifdef OMXDSP_PERTURBED
  if (gc_count != 1u || strcmp(omx_contract_log.rec[0].token, "ratio-at-least-one") != 0 ||
      strcmp(omx_contract_log.rec[0].stage, "gaincomp/db") != 0) {
    printf("FAIL perturbed: expected exactly one gaincomp/db ratio-at-least-one violation, got %u\n", gc_count);
    failed++;
  }
  printf("omxdsp_perturb (perturbed header): ratio %g under the moved floor %g recorded by gaincomp/db; %d failed\n",
         (double)OMXDSP_COMP_RATIO_BETWEEN, (double)OMXDSP_MOVED_COMP_RATIO_MIN, failed);
#else
  if (gc_count != 0u) { printf("FAIL control: ratio %g recorded %u violations against the real header\n", (double)OMXDSP_COMP_RATIO_BETWEEN, gc_count); failed++; }
  printf("omxdsp_perturb (real header): ratio %g above the declared floor, nothing recorded; %d failed\n", (double)OMXDSP_COMP_RATIO_BETWEEN, failed);
#endif
  return failed;
}
