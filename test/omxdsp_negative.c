// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omxdsp_negative — one arm per contract kind, each from a reset ledger, each recording EXACTLY
 * the one {stage, kind, token} it names (docs/design/specs/2026-09-26-dsp-primitives.md §8b
 * item 2). Built by the Makefile against test/sabotage.sh's `kernel` copy of the headers, ahead
 * of the real ones, so the POST arm sees a broken Lagrange sum; the PRE and INVARIANT arms never
 * reach that kernel. A contract that cannot fail is decoration; these prove each kind can.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omxdsp.h>

#include <stdio.h>
#include <string.h>

static int g_checks = 0, g_failed = 0;

static void print_ledger(void) {
  const uint32_t n = omx_contract_log.count;
  for (uint32_t i = 0; i < n && i < OMX_CONTRACT_MAX; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (omx_contract_record_ready(r)) printf(" {%s %s %s frame %u}", r->stage, r->kind, r->token, r->frame);
  }
}

/* The ledger holds exactly one record, and it is this one; then the ledger is reset. */
static void expect_exactly(const char *arm, const char *stage, const char *kind, const char *token) {
  g_checks++;
  const uint32_t n = omx_contract_log.count;
  const struct omx_contract_record *r = &omx_contract_log.rec[0];
  const int one = n == 1u && omx_contract_record_ready(r);
  const int same = one && strcmp(r->stage, stage) == 0 && strcmp(r->kind, kind) == 0 &&
                   strcmp(r->token, token) == 0;
  if (same) {
    printf("ok   [%s] exactly {%s %s %s}\n", arm, stage, kind, token);
  } else {
    g_failed++;
    printf("FAIL [%s] wanted exactly {%s %s %s}, the ledger holds %u:", arm, stage, kind, token, n);
    print_ledger();
    printf("\n");
  }
  omx_contract_reset();
}

/* The ledger holds nothing; then it is reset. */
static void expect_none(const char *arm) {
  g_checks++;
  const uint32_t n = omx_contract_log.count;
  if (n == 0u) {
    printf("ok   [%s] nothing recorded\n", arm);
  } else {
    g_failed++;
    printf("FAIL [%s] wanted an empty ledger, it holds %u:", arm, n);
    print_ledger();
    printf("\n");
  }
  omx_contract_reset();
}

/* A legal init records nothing: the baseline every arm below departs from. */
static void arm_control(void) {
  static float ring[64];
  struct omx_fdelay l;
  omx_contract_reset();
  (void)omx_fdelay_init(&l, ring, 64u, 3);
  expect_none("control");
}

/* PRE: an even Lagrange order into the constructor. */
static void arm_pre(void) {
  static float ring[64];
  struct omx_fdelay l;
  omx_contract_reset();
  const enum omx_fdelay_code code = omx_fdelay_init(&l, ring, 64u, 4);
  g_checks++;
  if (code != OMX_FDELAY_BAD_ORDER) { g_failed++; printf("FAIL [pre] an even order was not refused (code %d)\n", (int)code); }
  expect_exactly("pre", "fdelay/init", "pre", "order-is-odd-and-within-the-kernel");
}

/* POST: the sabotaged kernel's taps sum to 0.99, not 1. */
static void arm_post(void) {
  float c[OMX_FDELAY_MAX_TAPS];
  omx_contract_reset();
  omx_fdelay_lagrange(3, 0.5f, c);
  double sum = 0.0;
  for (int k = 0; k <= 3; k++) sum += (double)c[k];
  g_checks++;
  if (!(sum < 0.995)) { g_failed++; printf("FAIL [post] the kernel sums to %.6f — built against the real header, not the sabotaged copy\n", sum); }
  expect_exactly("post", "fdelay/kernel", "post", "the-kernel-is-unity-at-dc");
}

/* INVARIANT: the write cursor placed AT the cap, over a ring with room past it; one whole-sample
 * process call writes past the cap and leaves the cursor outside the ring. */
static void arm_invariant(void) {
  static float ring[128];
  struct omx_fdelay l;
  float buf[8];
  memset(ring, 0, sizeof ring);
  memset(buf, 0, sizeof buf);
  omx_contract_reset();
  (void)omx_fdelay_init(&l, ring, 64u, 3);
  omx_contract_reset();
  l.wpos = 64u;
  omx_fdelay_process(buf, 8u, &l, 32.0f);
  expect_exactly("invariant", "fdelay", "invariant", "write-cursor-inside-the-ring");
}

int main(void) {
  arm_control();
  arm_pre();
  arm_post();
  arm_invariant();
  printf("omxdsp_negative: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
