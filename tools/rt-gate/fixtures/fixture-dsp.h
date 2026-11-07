/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> */
/* fixture-dsp.h — two tagged library functions, as an omx-dsp header ships them (fixture.tags). */
#ifndef OMX_RT_GATE_FIXTURE_DSP_H
#define OMX_RT_GATE_FIXTURE_DSP_H
#include <math.h>
/** @brief One sample of gain. @thread audio */
static inline float fixture_gain(float x, float g) { return x * g; }
/** @brief A pass over a whole block: unbounded, the worker's. @thread worker */
static inline float fixture_analyse(const float *x, unsigned n) {
  float s = 0.0f;
  for (unsigned i = 0; i < n; i++) s += sqrtf(x[i] * x[i]);
  return s;
}
#endif
