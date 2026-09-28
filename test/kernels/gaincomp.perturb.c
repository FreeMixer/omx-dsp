static int perturb_gaincomp(int failed) {
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
  if (gc_count != 0u) { printf("FAIL control: ratio 1.25 recorded %u violations against the real header\n", gc_count); failed++; }
  printf("omxdsp_perturb (real header): ratio 1.25 above the declared floor, nothing recorded; %d failed\n", failed);
#endif
  return failed;
}
