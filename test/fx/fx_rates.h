// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#ifndef OMXDSP_TEST_FX_RATES_H
#define OMXDSP_TEST_FX_RATES_H

#include <stdio.h>
#include <stdlib.h>

#include <omxdsp/omx_contract_limits.h>

/** The rates every kernel oracle runs at, whatever the declaration says: a declared list missing
 * one of them stops the suite (exit 2) instead of testing less. */
static const float OMX_FX_RATE_FLOOR[4] = {44100.0f, 48000.0f, 96000.0f, 192000.0f};

static void omx_fx_require_rate_floor(void) {
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

#endif
