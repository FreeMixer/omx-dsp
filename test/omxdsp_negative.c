// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omxdsp_negative.c — one arm per contract kind, each recording EXACTLY the violation it
 * provokes (docs/design/specs/2026-09-26-dsp-primitives.md §8b item 2). Compiled against
 * test/sabotage.sh's copy of the headers, placed ahead of the real include directory, so the
 * POST arm runs a kernel that does not sum to one.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omxdsp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks = 0, g_failed = 0;

static void ok(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s\n", what);
  }
}

/* The ledger holds exactly `n` records of the given stage and kind, carrying `tokens` in order. */
static void expect_exactly(uint32_t n, const char *stage, const char *kind, const char *const *tokens) {
  ok(omx_contract_log.count == n, "the arm recorded exactly the expected number of violations");
  const uint32_t kept = omx_contract_log.count < OMX_CONTRACT_MAX ? omx_contract_log.count : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    const char *token = i < n ? tokens[i] : "";
    ok(omx_contract_record_ready(r), "the record is published");
    ok(strcmp(r->stage, stage) == 0, "the record names the stage");
    ok(strcmp(r->kind, kind) == 0, "the record names the kind");
    ok(strcmp(r->token, token) == 0, "the record names the token");
    if (strcmp(r->stage, stage) != 0 || strcmp(r->kind, kind) != 0 || strcmp(r->token, token) != 0)
      printf("  got [%s] %s %s\n", r->stage, r->kind, r->token);
  }
  omx_contract_reset();
}

int main(void) {
  static float ring[256];
  struct omx_fdelay l;
  omx_contract_reset();

  /* PRE: an even Lagrange order is refused by code, and the contract records the refusal. */
  ok(omx_fdelay_init(&l, ring, 256u, 4) == OMX_FDELAY_BAD_ORDER, "an even order is refused with its code");
  ok(l.order == 0, "the refused line is unarmed");
  static const char *const pre_tokens[] = {"order-is-odd-and-within-the-kernel"};
  expect_exactly(1u, "fdelay/init", "pre", pre_tokens);

  /* POST: the sabotaged kernel does not sum to one, which also carries it past the declared L1
   * norm; the kernel's own postconditions see both, in their order. */
  float c[OMX_FDELAY_MAX_TAPS];
  omx_fdelay_lagrange(3, 0.5f, c);
  double sum = 0.0;
  for (int k = 0; k <= 3; k++) sum += c[k];
  ok(sum > 1.2, "the sabotage landed: the kernel sums past one");
  static const char *const post_tokens[] = {"the-kernel-is-unity-at-dc",
                                            "an-order-3-kernel-is-inside-the-declared-l1-norm"};
  expect_exactly(2u, "fdelay/kernel", "post", post_tokens);

  /* INVARIANT: a write cursor placed AT the ring's capacity, over an allocation with room past
   * it, walks out of the ring; the block door's invariant records it once. */
  static float wide[2 * 64 + 2];
  ok(omx_fdelay_init(&l, wide, 64u, 3) == OMX_FDELAY_OK, "a legal line arms");
  omx_contract_reset();
  l.wpos = 64u;
  float one[1] = {0.25f};
  omx_fdelay_process(one, 1u, &l, 3.0f);
  ok(l.wpos == 65u, "the cursor kept walking past the ring");
  static const char *const invariant_tokens[] = {"write-cursor-inside-the-ring"};
  expect_exactly(1u, "fdelay", "invariant", invariant_tokens);

  /* The control: the same doors, used legally, record nothing. */
  ok(omx_fdelay_init(&l, wide, 64u, 3) == OMX_FDELAY_OK, "the control arms");
  omx_contract_reset();
  omx_fdelay_process(one, 1u, &l, 3.0f);
  ok(omx_contract_log.count == 0u, "a legal call records no violation");

  /* The divider: a negative hysteresis is refused by PRE; a sign word that is not a unit is
   * caught by the INVARIANT; the legal call records nothing. */
  struct omx_divider dv;
  omx_divider_init(&dv);
  omx_contract_reset();
  omx_divider_step(&dv, 0.1f, -0.25f);
  static const char *const div_pre_tokens[] = {"hysteresis-non-negative"};
  expect_exactly(1u, "divider", "pre", div_pre_tokens);
  dv.q = 0.5f;
  omx_divider_step(&dv, 0.1f, 0.25f);
  static const char *const div_inv_tokens[] = {"sign-word-is-unit"};
  expect_exactly(1u, "divider", "invariant", div_inv_tokens);
  omx_divider_init(&dv);
  omx_divider_step(&dv, 0.1f, 0.25f);
  ok(omx_contract_log.count == 0u, "a legal divider step records no violation");

  printf("omxdsp_negative: %d checks, %d failed; the three contract kinds each recorded their violation\n",
         g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
