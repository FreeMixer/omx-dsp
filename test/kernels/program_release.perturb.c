static int perturb_program_release(int failed) {
  struct omx_env_program_release_params pr;
  omx_env_program_release_opto(&pr, 48000.0f, 1u);
#ifdef OMXDSP_PERTURBED
  if (pr.fast_pole != omx_pole_from_time_ms(35.0f, 48000.0f)) {
    printf("FAIL perturbed: the opto fast pole did not follow the moved fastMs — a copy of the profile\n");
    failed++;
  }
  printf("omxdsp_perturb (perturbed header): the opto release's fast pole is the moved 35 ms; %d failed\n", failed);
#else
  if (pr.fast_pole != omx_pole_from_time_ms(OMX_PROGRAM_RELEASE_PROFILES_OPTO_FAST_MS, 48000.0f) ||
      pr.fast_pole == omx_pole_from_time_ms(35.0f, 48000.0f)) {
    printf("FAIL control: the opto fast pole is not the declared fastMs\n");
    failed++;
  }
  printf("omxdsp_perturb (real header): the opto release's fast pole is the declared fastMs; %d failed\n", failed);
#endif
  return failed;
}
