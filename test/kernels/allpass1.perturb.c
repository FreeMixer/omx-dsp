static int perturb_allpass1(int failed) {
  omx_contract_reset();
  omx_allpass1_coef(1000.0f, 96000.0f);
  const uint32_t ap_count = omx_contract_log.count;
#ifdef OMXDSP_PERTURBED
  if (ap_count != 1u || strcmp(omx_contract_log.rec[0].token, "rate-is-declared") != 0 ||
      strcmp(omx_contract_log.rec[0].stage, "allpass1/coef") != 0) {
    printf("FAIL perturbed: expected exactly one allpass1/coef rate-is-declared violation, got %u\n", ap_count);
    failed++;
  }
  printf("omxdsp_perturb (perturbed header): the all-pass design refused 96000; %d failed\n", failed);
#else
  if (ap_count != 0u) { printf("FAIL control: the all-pass design recorded %u violations at 96000\n", ap_count); failed++; }
#endif
  return failed;
}
