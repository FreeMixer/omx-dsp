// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The rates the analysis oracles run at: the nine an RME interface offers, 32 to 192 kHz. The
 * declared rates (OMX_DECLARED_RATES) are a subset of them, and a declared rate missing from this
 * list stops the program (exit 2) instead of going untested.
 */
#ifndef OMXDSP_TEST_ANALYSIS_RATES_H
#define OMXDSP_TEST_ANALYSIS_RATES_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <omxcontract/omx_contract_limits.h>

#define OMX_ANALYSIS_RATE_COUNT ((int)OMX_RME_RATES_COUNT)

static const double OMX_ANALYSIS_RATES[OMX_ANALYSIS_RATE_COUNT] = OMX_RME_RATES_INIT;

static inline void omx_analysis_require_rates(void) {
  for (uint32_t k = 0; k < OMX_DECLARED_RATE_COUNT; k++) {
    int found = 0;
    for (int r = 0; r < OMX_ANALYSIS_RATE_COUNT; r++)
      if ((double)OMX_DECLARED_RATES[k] == OMX_ANALYSIS_RATES[r]) found = 1;
    if (!found) {
      fprintf(stderr, "OMX_DECLARED_RATES carries %.0f Hz, which the analysis rates lack\n",
              (double)OMX_DECLARED_RATES[k]);
      exit(2);
    }
  }
}

/* The RTA's frame size at a rate: the smallest power of two from 2048 whose bins are no wider
 * than the declared target, capped at the declared maximum. The same rule as the FBS oracle's. */
static inline uint32_t omx_analysis_fft_for_rate(double rate) {
  uint32_t n = 2048;
  while (n < OMX_RTA_FFT_SIZE_MAX && rate / n > OMX_RTA_TARGET_BIN_HZ_DOUBLE) n *= 2;
  return n;
}

#endif
