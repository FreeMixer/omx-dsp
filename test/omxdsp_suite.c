// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omxdsp_suite.c — the library suite: every primitive's oracle at every declared rate, with
 * contracts ON, ending on an EMPTY ledger (docs/design/specs/2026-09-26-dsp-primitives.md §8b).
 * Pure C, -lm, no other package.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omxdsp.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the harness -------------------------------------------------------------------------- */

static int g_checks = 0, g_failed = 0;
static const char *g_arm = "";

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s — measured %.9g, limit %.9g\n", g_arm, what, measured, limit);
  }
}

/* Every violation the ledger holds, printed, then the ledger is expected empty for this arm. */
static void expect_clean(void) {
  const uint32_t seen = omx_contract_log.count;
  const uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (!omx_contract_record_ready(r)) continue;
    printf("VIOLATION [%s] %s %s (frame %u)\n", r->stage, r->kind, r->token, r->frame);
  }
  ok(seen == 0u, "no contract violation in this arm", (double)seen, 0.0);
  omx_contract_reset();
}

static uint32_t g_seed = 0x2f6e2b1du;
static float rnd(float lo, float hi) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return lo + (hi - lo) * ((float)(g_seed >> 8) / 16777216.0f);
}

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

static void arm_rates(void) {
  g_arm = "rates";
  ok(OMX_DECLARED_RATE_COUNT >= 4u, "the declaration carries at least the four basic rates", (double)OMX_DECLARED_RATE_COUNT, 4.0);
  for (uint32_t i = 0; i < OMX_DECLARED_RATE_COUNT; i++) {
    ok(omx_rate_is_declared(OMX_DECLARED_RATES[i]), "a declared rate is declared", OMX_DECLARED_RATES[i], 1.0);
    ok(OMX_RATE_IS_DECLARED(OMX_DECLARED_RATES[i]), "the macro forwards to the table", OMX_DECLARED_RATES[i], 1.0);
  }
  const float not_declared[] = {0.0f, -48000.0f, 22050.0f, 96001.0f, 384000.0f, NAN};
  for (size_t i = 0; i < sizeof not_declared / sizeof not_declared[0]; i++)
    ok(!omx_rate_is_declared(not_declared[i]), "an undeclared rate is refused", not_declared[i], 0.0);
  expect_clean();
}

static void arm_block_helpers(void) {
  g_arm = "block-helpers";
  float b[8];
  for (int i = 0; i < 8; i++) b[i] = rnd(-1.0f, 1.0f);
  ok(omx_block_finite(b, 8u), "a finite block is finite", 0.0, 1.0);
  ok(omx_block_finite(NULL, 8u), "a NULL block promises nothing and passes", 0.0, 1.0);
  b[3] = NAN;
  ok(!omx_block_finite(b, 8u), "a NaN is seen", 0.0, 0.0);
  b[3] = INFINITY;
  ok(!omx_block_finite(b, 8u), "an infinity is seen", 0.0, 0.0);
  b[3] = -INFINITY;
  ok(!omx_block_finite(b, 8u), "a negative infinity is seen", 0.0, 0.0);
  b[3] = 0.25f;
  ok(omx_lane_finite(b, NULL, 8u), "a mono lane is finite on its one leg", 0.0, 1.0);
  float r[8] = {0};
  r[7] = NAN;
  ok(!omx_lane_finite(b, r, 8u), "a lane's R leg is checked", 0.0, 0.0);
  float m[4] = {0.5f, -0.75f, 0.25f, 0.0f};
  ok(omx_block_absmax(m, 4u) == 0.75f, "absmax is the largest magnitude", (double)omx_block_absmax(m, 4u), 0.75);
  ok(omx_block_absmax(NULL, 4u) == 0.0f, "absmax of NULL is 0", 0.0, 0.0);
  expect_clean();
}

static void arm_version(void) {
  g_arm = "version";
  ok(omxdsp_version() == ((uint32_t)OMXDSP_VERSION_MAJOR << 16 | (uint32_t)OMXDSP_VERSION_MINOR << 8 | (uint32_t)OMXDSP_VERSION_PATCH),
     "the version packs major, minor and patch", (double)omxdsp_version(), 0.0);
  expect_clean();
}

int main(void) {
  omx_contract_reset();
  arm_ledger();
  arm_rates();
  arm_block_helpers();
  arm_version();
  const uint32_t violations = omx_contract_log.count;
  const uint32_t evaluated = omx_contract_log.checks;
  printf("omxdsp_suite: %d checks, %d failed; %u contracts evaluated, %u violations left\n", g_checks,
         g_failed, evaluated, violations);
  if (evaluated < 20u) {
    printf("FAIL the suite evaluated %u contracts — floor is 20\n", evaluated);
    return 1;
  }
  if (violations != 0u) return 1;
  return g_failed == 0 ? 0 : 1;
}
