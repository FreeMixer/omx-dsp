// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * A contracts consumer of the compiled unit. A NaN handed to omx_oversampler_up, which lives in the
 * archive and not in a header, breaks its `finite-in` precondition: the violation must reach this
 * program's ledger. It does only when the program links the archive built with OMX_CONTRACTS
 * (libomxdsp-contracts.a or libomxdsp-tsan.a); linked against libomxdsp.a the ledger stays empty
 * and the program exits 1.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omxdsp.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef OMX_CONTRACTS
#error "omxdsp_flavour.c is a contracts consumer: build it with -DOMX_CONTRACTS"
#endif

int main(void) {
  enum { N = 16 };
  float in[N], out[4 * N];
  struct omx_oversampler o;
  for (int i = 0; i < N; i++) in[i] = 0.25f;
  in[5] = NAN;
  omx_oversampler_init(&o, 4u);
  omx_contract_reset();
  omx_oversampler_up(&o, in, N, out);
  const uint32_t n = atomic_load(&omx_contract_log.count);
  for (uint32_t i = 0; i < n && i < OMX_CONTRACT_MAX; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (!omx_contract_record_ready(r)) continue;
    if (strcmp(r->stage, "oversampler/up") == 0 && strcmp(r->token, "finite-in") == 0 && strcmp(r->kind, "pre") == 0) {
      printf("flavour: a NaN into omx_oversampler_up reached this ledger as oversampler/up finite-in\n");
      return 0;
    }
  }
  printf("flavour: FAIL — a NaN into omx_oversampler_up left no oversampler/up finite-in record "
         "(%u violations): this contracts build linked an archive without OMX_CONTRACTS\n", (unsigned)n);
  return 1;
}
