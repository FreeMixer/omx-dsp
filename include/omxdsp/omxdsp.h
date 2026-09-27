// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omxdsp.h
 * @brief libomxdsp: the umbrella header, the version and the state-layout export.
 *
 * Every primitive is a `static inline` function over caller-owned state in its own header;
 * this header includes them all. Design: docs/design/specs/2026-09-26-dsp-primitives.md.
 */
#ifndef OMXDSP_H
#define OMXDSP_H

#include <stddef.h>
#include <stdint.h>

/** @brief API major version: a symbol's meaning changes only with a bump here. */
#define OMXDSP_VERSION_MAJOR 0
/** @brief API minor version: additions only. */
#define OMXDSP_VERSION_MINOR 1
/** @brief Patch version: no API change. */
#define OMXDSP_VERSION_PATCH 0

/**
 * @brief The library version as one word, `major << 16 | minor << 8 | patch`.
 * @return The packed version of the headers this translation unit compiled against.
 * @note RT-safe and thread-safe: a constant.
 */
static inline uint32_t omxdsp_version(void) {
  return ((uint32_t)OMXDSP_VERSION_MAJOR << 16) | ((uint32_t)OMXDSP_VERSION_MINOR << 8) |
         (uint32_t)OMXDSP_VERSION_PATCH;
}

/**
 * @brief Export a stateful primitive's state size and alignment so a host lays it out itself.
 *
 * Expands to `omx_<word>_state_size()` and `omx_<word>_state_align()`, both `static inline`,
 * returning `sizeof` and `_Alignof` of `type`. A state so exported holds no pointer to itself.
 */
#define OMXDSP_STATE_LAYOUT(word, type)                                            \
  static inline size_t omx_##word##_state_size(void) { return sizeof(type); }     \
  static inline size_t omx_##word##_state_align(void) { return _Alignof(type); }

#include "omx_allpass.h"
#include "omx_biquad.h"
#include "omx_contract.h"
#include "omx_contract_limits.h"
#include "omx_denormal.h"
#include "omx_divider.h"
#include "omx_envelope.h"
#include "omx_eq_design.h"
#include "omx_fdelay.h"
#include "omx_gaincomp.h"
#include "omx_lfo.h"
#include "omx_matched_pair.h"
#include "omx_onepole.h"
#include "omx_oversampler.h"
#include "omx_units.h"
#include "omx_xover.h"

#endif /* OMXDSP_H */
