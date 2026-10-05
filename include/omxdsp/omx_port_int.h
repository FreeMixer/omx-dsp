/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_port_int.h — a float control word read as an INTEGER, the one reader every plugin face uses
 * for a port its TTL marks `lv2:integer` (an enum member, a slope, a factor, a band budget).
 *
 * Converting a NaN, an infinity or a float outside an int's range to an int is undefined
 * behaviour (C11 6.3.1.4), and a host is free to write any of them to a control port. So the
 * word is held FIRST, by the parameter clamp's omx_clamp_or() — every non-finite value reads as
 * the port's declared default, a finite one saturates at the edge of its declared travel — and
 * only the held value is rounded. Inside the travel a word rounds half away from zero, as every
 * face rounded it before.
 *
 * Design: docs/design/specs/2026-09-26-dsp-primitives.md §1 row 23 ("an integer port"); oracle
 * test/kernels/port_int.c (the face-level oracle stays the engine's src/lv2_int_ports.test.c).
 */
#ifndef OMX_PORT_INT_H
#define OMX_PORT_INT_H

#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_param.h>

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "port/int"

/**
 * @brief A control word as an integer inside [lo, hi]; every non-finite word reads as `dflt`.
 * @param v The word a host wrote — any float, NaN and ±Inf included.
 * @param lo The declared minimum.
 * @param hi The declared maximum, `>= lo`.
 * @param dflt The declared default, inside [lo, hi].
 * @return `dflt` for a non-finite `v`, else `v` held inside [lo, hi] and rounded half away from 0.
 * @pre `default-inside-the-travel` (omx_clamp_or's).
 * @post `inside-the-travel`.
 * @note RT-safe and thread-safe: omx_clamp_or() and one conversion of a value already inside the
 *       travel, so the conversion is always defined.
 */
static inline int omx_port_int(float v, int lo, int hi, int dflt) {
  const float c = omx_clamp_or(v, (float)lo, (float)hi, (float)dflt);
  const int i = (int)(c + (c < 0.0f ? -0.5f : 0.5f));
  OMX_POST(i >= lo && i <= hi, "inside-the-travel");
  return i;
}

#undef OMX_CONTRACT_STAGE

#endif /* OMX_PORT_INT_H */
