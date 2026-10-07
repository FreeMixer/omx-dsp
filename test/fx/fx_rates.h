// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#ifndef OMXDSP_TEST_FX_RATES_H
#define OMXDSP_TEST_FX_RATES_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_contract_limits.h>

/** The rates every kernel oracle runs at, whatever the declaration says: a declared list missing
 * one of them stops the suite (exit 2) instead of testing less. */
static const float OMX_FX_RATE_FLOOR[4] = {44100.0f, 48000.0f, 96000.0f, 192000.0f};

static inline void omx_fx_require_rate_floor(void) {
  for (int f = 0; f < 4; f++) {
    int found = 0;
    for (int k = 0; k < (int)OMX_DECLARED_RATE_COUNT; k++)
      if (OMX_DECLARED_RATES[k] == OMX_FX_RATE_FLOOR[f]) found = 1;
    if (!found) {
      fprintf(stderr, "OMX_DECLARED_RATES lacks %.0f Hz, one of the four floor rates\n",
              (double)OMX_FX_RATE_FLOOR[f]);
      exit(2);
    }
  }
}

/** The nine rates an RME interface clocks at, 32 to 192 kHz. The console declares six of them
 * (OMX_DECLARED_RATES); an oracle that runs here also covers 32, 64 and 128 kHz, where a kernel
 * still has to meet its closed form even though the console never asks for them. */
#define OMX_FX_RME_RATE_COUNT 9u
static const float OMX_FX_RME_RATES[OMX_FX_RME_RATE_COUNT] = {
    32000.0f, 44100.0f, 48000.0f, 64000.0f, 88200.0f, 96000.0f, 128000.0f, 176400.0f, 192000.0f};

#ifdef OMX_CONTRACTS
/**
 * Print and forget the ledger after an arm run at `sr`, and return how many violations `sr` does
 * not explain. At a declared rate that is every violation. At a rate outside the declaration a
 * `rate-is-declared` precondition is the contract saying so, which is what it is for, and is not
 * counted; every other law must still hold. Violations past OMX_CONTRACT_MAX were never stored, so
 * nothing shows they were explained and they count.
 */
static inline uint32_t omx_fx_drain_ledger(float sr) {
  const int declared = omx_rate_is_declared(sr);
  const uint32_t seen = omx_contract_log.count;
  const uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  uint32_t unexplained = seen - kept;
  for (uint32_t i = 0; i < kept; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (!omx_contract_record_ready(r)) { unexplained++; continue; }
    if (!declared && strcmp(r->kind, "pre") == 0 && strcmp(r->token, "rate-is-declared") == 0) continue;
    unexplained++;
    printf("VIOLATION @ %.0f Hz [%s] %s %s (frame %u)\n", (double)sr, r->stage, r->kind, r->token, r->frame);
  }
  omx_contract_reset();
  return unexplained;
}
#endif

#endif
