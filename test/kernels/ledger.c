/* ---- the contract mechanism ----------------------------------------------------------------- */

#define OMX_CONTRACT_STAGE "suite/contract"
static void arm_ledger(void) {
  g_arm = "ledger";
  omx_contract_reset();
  const uint32_t before = omx_contract_log.checks;
  OMX_PRE(1, "holds");
  ok(omx_contract_log.checks == before + 1u, "an evaluated contract counts once", (double)omx_contract_log.checks, before + 1.0);
  ok(omx_contract_log.count == 0u, "a held contract records nothing", (double)omx_contract_log.count, 0.0);
  OMX_POST_AT(0, "broken", 17u);
  ok(omx_contract_log.count == 1u, "a broken contract counts once", (double)omx_contract_log.count, 1.0);
  ok(omx_contract_record_ready(&omx_contract_log.rec[0]), "the record is published", 0.0, 1.0);
  ok(strcmp(omx_contract_log.rec[0].kind, "post") == 0, "the record carries its kind", 0.0, 0.0);
  ok(strcmp(omx_contract_log.rec[0].token, "broken") == 0, "the record carries its token", 0.0, 0.0);
  ok(strcmp(omx_contract_log.rec[0].stage, OMX_CONTRACT_STAGE) == 0, "the record carries its stage", 0.0, 0.0);
  ok(omx_contract_log.rec[0].frame == 17u, "the record carries its frame", (double)omx_contract_log.rec[0].frame, 17.0);
  for (uint32_t i = 0; i < OMX_CONTRACT_MAX + 10u; i++) OMX_INVARIANT(0, "overflow");
  ok(omx_contract_log.count == OMX_CONTRACT_MAX + 11u, "the count stays exact past the kept records",
     (double)omx_contract_log.count, (double)OMX_CONTRACT_MAX + 11.0);
  ok(omx_contract_record_ready(&omx_contract_log.rec[OMX_CONTRACT_MAX - 1u]), "the last kept slot is published", 0.0, 1.0);
  omx_contract_reset();
  ok(omx_contract_log.count == 0u, "reset forgets the violations", (double)omx_contract_log.count, 0.0);
  ok(!omx_contract_record_ready(&omx_contract_log.rec[0]), "reset unpublishes the records", 0.0, 0.0);
  ok(omx_contract_log.checks > before, "reset keeps the evaluated count", (double)omx_contract_log.checks, (double)before);
  expect_clean();
}
#undef OMX_CONTRACT_STAGE
