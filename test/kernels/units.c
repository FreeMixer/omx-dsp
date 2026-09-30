
/* ---- units ---------------------------------------------------------------------------------- */

static void arm_units(void) {
  g_arm = "units";
  ok(omx_db_to_lin(0.0f) == 1.0f, "0 dB is unity", omx_db_to_lin(0.0f), 1.0);
  ok(fabsf(omx_db_to_lin(-6.0f) - 0.501187f) < 1e-5f, "-6 dB is 0.501187", omx_db_to_lin(-6.0f), 0.501187);
  ok(fabsf(omx_db_to_lin(20.0f) - 10.0f) < 1e-5f, "+20 dB is 10", omx_db_to_lin(20.0f), 10.0);
  for (float db = -120.0f; db <= 24.0f; db += 3.7f) {
    const float back = omx_lin_to_db(omx_db_to_lin(db));
    ok(fabsf(back - db) < 1e-3f, "dB -> lin -> dB round trip", back, db);
  }
  ok(omx_lin_to_db(0.0f) == -180.0f, "silence is a finite -180 dB", omx_lin_to_db(0.0f), -180.0);
  ok(omx_lin_to_db(1e-12f) == -180.0f, "below the floor reads as the floor", omx_lin_to_db(1e-12f), -180.0);
  /* The double-precision analysis word: floored where the CALLER says, exact against its closed form. */
  ok(omx_lin_to_db_d(1.0, 1e-12) == 0.0, "lin_to_db_d: unity is 0 dB", omx_lin_to_db_d(1.0, 1e-12), 0.0);
  ok(omx_lin_to_db_d(0.0, 1e-12) == -240.0, "lin_to_db_d: silence reads as the named floor", omx_lin_to_db_d(0.0, 1e-12), -240.0);
  ok(omx_lin_to_db_d(1e-15, 1e-9) == -180.0, "lin_to_db_d: below the floor reads as the floor", omx_lin_to_db_d(1e-15, 1e-9), -180.0);
  for (double lg = -11.0; lg <= 3.0; lg += 0.25) {
    const double x = pow(10.0, lg);
    ok(fabs(omx_lin_to_db_d(x, 1e-12) - 20.0 * lg) < 1e-9, "lin_to_db_d is 20*log10 over the travel", omx_lin_to_db_d(x, 1e-12), 20.0 * lg);
  }
  /* The per-sample pair against the libm pair, dense over the declared travel (row 17's oracle).
   * The worst error is PRINTED, so the tolerance the spec declares is read from a run. */
  double worst_db = 0.0, worst_rel = 0.0;
  for (double lg = -9.0; lg <= 4.0; lg += 1.0 / 4096.0) {
    const float x = (float)pow(10.0, lg);
    const double d = fabs((double)omx_lin_to_db_poly(x) - (double)omx_lin_to_db(x));
    if (d > worst_db) worst_db = d;
  }
  for (double db = -300.0; db <= 80.0; db += 1.0 / 512.0) {
    const double ref = (double)omx_db_to_lin((float)db);
    const double rel = fabs((double)omx_db_to_lin_poly((float)db) - ref) / ref;
    if (rel > worst_rel) worst_rel = rel;
  }
  printf("  units: lin_to_db_poly worst %.3g dB over [1e-9, 1e4]; db_to_lin_poly worst %.3g relative over [-300, +80] dB\n",
         worst_db, worst_rel);
  ok(worst_db <= 1e-4, "lin_to_db_poly within 1e-4 dB of lin_to_db", worst_db, 1e-4);
  ok(worst_rel <= 2e-6, "db_to_lin_poly within 2e-6 relative of db_to_lin", worst_rel, 2e-6);
  ok(omx_db_to_lin_poly(0.0f) == 1.0f, "db_to_lin_poly: 0 dB is EXACTLY 1", omx_db_to_lin_poly(0.0f), 1.0);
  ok(omx_lin_to_db_poly(1.0f) == 0.0f, "lin_to_db_poly: unity is EXACTLY 0 dB", omx_lin_to_db_poly(1.0f), 0.0);
  ok(omx_lin_to_db_poly(0.0f) == omx_lin_to_db_poly(1e-9f), "lin_to_db_poly: silence reads as the floor",
     omx_lin_to_db_poly(0.0f), omx_lin_to_db_poly(1e-9f));
  ok(omx_db_to_lin_poly(-1e4f) > 0.0f && omx_db_to_lin_poly(-1e4f) - omx_db_to_lin_poly(-1e4f) == 0.0f,
     "db_to_lin_poly: far below the travel is a finite positive", omx_db_to_lin_poly(-1e4f), 0.0);
  expect_clean();
}
