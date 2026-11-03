// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// fastmath_vec.c — one out-of-line function per loop that omx_fastmath.h exists for. It is compiled to
// assembly only (tools/fastmath-vec-check.sh), never run: the check requires each function's assembly to
// call the libmvec vector symbol of every libm function the loop makes, and no scalar call of it.
#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/omx_dyn.h>

void vc_drive_soft(const struct omx_drive_bias *s, float d, float *x, uint32_t n) {
  omx_drive_shape_block(OMX_DRIVE_SOFT, s, d, x, n);
}
void vc_drive_exciter(const struct omx_drive_bias *s, float d, float *x, uint32_t n) {
  omx_drive_shape_block(OMX_DRIVE_EXCITER, s, d, x, n);
}
void vc_drive_tape(const struct omx_drive_bias *s, float d, float *x, uint32_t n) {
  omx_drive_shape_block(OMX_DRIVE_TAPE, s, d, x, n);
}
void vc_drive_tube(const struct omx_drive_bias *s, float d, float *x, uint32_t n) {
  omx_drive_shape_block(OMX_DRIVE_TUBE, s, d, x, n);
}

void vc_gain_centred(const struct omx_dyn *p, float *x, uint32_t n) {
  OMX_FM_SIMD
  for (uint32_t i = 0; i < n; i++) x[i] = omx_gaincomp_gain(&p->gc, x[i]);
}
void vc_gain_shifted(const struct omx_dyn *p, float *x, uint32_t n) {
  OMX_FM_SIMD
  for (uint32_t i = 0; i < n; i++) x[i] = omx_gaincomp_gain_shifted_lane(&p->gc, p->knee_shift_db, x[i]);
}
void vc_gain_flat(const struct omx_dyn_flat *f, const struct omx_dyn *p, float *x, uint32_t n) {
  omx_dyn_flat_gain_block(f, p, x, x, n);
}
