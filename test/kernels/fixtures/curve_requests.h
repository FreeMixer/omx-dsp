// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The curve requests the golden vectors are written from (tools/curve-golden.c) and read back against
 * (test/kernels/curve.c): one per model, each a request a served description makes, in its own units.
 */
#ifndef OMXDSP_TEST_CURVE_REQUESTS_H
#define OMXDSP_TEST_CURVE_REQUESTS_H

#include <omxdsp/omx_curve.h>

#define CT(role, index, value, unit, shape) \
  { OMX_CURVE_ROLES_##role, (index), (value), OMX_CURVE_UNITS_##unit, (shape), OMX_CURVE_ORIGINS_USER }
/* a term of a feedback corrector's band */
#define CTO(role, index, value, unit, shape, origin) \
  { OMX_CURVE_ROLES_##role, (index), (value), OMX_CURVE_UNITS_##unit, (shape), OMX_CURVE_ORIGINS_##origin }
#define CT_NS OMX_CURVE_NO_SHAPE

/* A four-band parametric with both head filters, at 48 kHz: a bell from an octave width, a low shelf whose gain is
 * a linear factor, a high shelf in kHz from a band_type choice, a notch, a 24 dB/oct high-pass, a 12 dB/oct low-pass (each slope resolved to dB per octave). */
static const struct omx_curve_term CURVE_GOLDEN_EQ_TERMS[] = {
    CT(BAND_FREQ, 0, 1000.0, HZ, OMX_CURVE_SHAPES_BELL),
    CT(BAND_GAIN, 0, 6.0, DB, CT_NS),
    CT(BAND_Q, 0, 1.0, OCT, CT_NS),
    CT(BAND_TYPE, 1, 2.0, NONE, OMX_CURVE_SHAPES_LOWSHELF),
    CT(BAND_FREQ, 1, 100.0, HZ, CT_NS),
    CT(BAND_GAIN, 1, 2.0, COEF, CT_NS),
    CT(BAND_Q, 1, 0.7071, NONE, CT_NS),
    CT(BAND_TYPE, 2, 3.0, NONE, OMX_CURVE_SHAPES_HIGHSHELF),
    CT(BAND_FREQ, 2, 8.0, KHZ, CT_NS),
    CT(BAND_GAIN, 2, -4.0, DB, CT_NS),
    CT(BAND_Q, 2, 0.7071, COEF, CT_NS),
    CT(BAND_FREQ, 3, 3000.0, HZ, OMX_CURVE_SHAPES_NOTCH),
    CT(BAND_Q, 3, 8.0, NONE, CT_NS),
    CT(BAND_ON, 3, 1.0, NONE, CT_NS),
    CT(HPF_FREQ, 0, 40.0, HZ, CT_NS),
    CT(HPF_SLOPE, 0, 24.0, DB, CT_NS),
    CT(LPF_FREQ, 0, 16.0, KHZ, CT_NS),
    CT(LPF_SLOPE, 0, 12.0, DB, CT_NS),
    CT(LPF_ON, 0, 1.0, NONE, CT_NS),
};
static const struct omx_curve_request CURVE_GOLDEN_EQ = {
    OMX_CURVE_DISPLAYS_EQ_RESPONSE, OMX_CURVE_FAMILIES_EQ, 48000.0, 32u, 0, 0.0, 0.0,
    CURVE_GOLDEN_EQ_TERMS, (uint32_t)(sizeof CURVE_GOLDEN_EQ_TERMS / sizeof CURVE_GOLDEN_EQ_TERMS[0]), 0};

/* A compressor at 96 kHz: -20 dBFS, 4:1, a 6 dB knee, 3 dB of make-up. */
static const struct omx_curve_term CURVE_GOLDEN_COMP_TERMS[] = {
    CT(THRESHOLD, 0, -20.0, DBFS, CT_NS),
    CT(RATIO, 0, 4.0, RATIO, CT_NS),
    CT(KNEE, 0, 6.0, DB, CT_NS),
    CT(MAKEUP, 0, 3.0, DB, CT_NS),
};
static const struct omx_curve_request CURVE_GOLDEN_COMP = {
    OMX_CURVE_DISPLAYS_TRANSFER_CURVE, OMX_CURVE_FAMILIES_DYNAMICS, 96000.0, 41u, 0, 0.0, 0.0,
    CURVE_GOLDEN_COMP_TERMS, (uint32_t)(sizeof CURVE_GOLDEN_COMP_TERMS / sizeof CURVE_GOLDEN_COMP_TERMS[0]), 0};

/* A gate at 44.1 kHz: -40 dB, a -60 dB range, 4:1 below, the knee range -46 to -38 dB, off the threshold. */
static const struct omx_curve_term CURVE_GOLDEN_GATE_TERMS[] = {
    CT(THRESHOLD, 0, -40.0, DB, CT_NS),
    CT(RANGE, 0, -60.0, DB, CT_NS),
    CT(RATIO, 0, 4.0, COEF, CT_NS),
    CT(KNEE_START, 0, -46.0, DB, CT_NS),
    CT(KNEE_END, 0, -38.0, DB, CT_NS),
};
static const struct omx_curve_request CURVE_GOLDEN_GATE = {
    OMX_CURVE_DISPLAYS_TRANSFER_CURVE, OMX_CURVE_FAMILIES_GATE, 44100.0, 41u, 0, 0.0, 0.0,
    CURVE_GOLDEN_GATE_TERMS, (uint32_t)(sizeof CURVE_GOLDEN_GATE_TERMS / sizeof CURVE_GOLDEN_GATE_TERMS[0]), 0};

/* The strip EQ the feedback corrector planted two notches in, at 48 kHz: two user bells, a ring-out notch at 2.5 kHz
 * (a notch band), a show notch at 630 Hz (a -9 dB bell at Q 16), an HRP cut at 1.8 kHz (-4.5 dB, Q 24), and a
 * 12 dB/oct high-pass; the user-only curve
 * asked for beside the marks. */
static const struct omx_curve_term CURVE_GOLDEN_FBS_TERMS[] = {
    CT(BAND_FREQ, 0, 120.0, HZ, OMX_CURVE_SHAPES_BELL),
    CT(BAND_GAIN, 0, 3.0, DB, CT_NS),
    CT(BAND_Q, 0, 0.8, NONE, CT_NS),
    CT(BAND_FREQ, 1, 4000.0, HZ, OMX_CURVE_SHAPES_HIGHSHELF),
    CT(BAND_GAIN, 1, 2.0, DB, CT_NS),
    CT(BAND_Q, 1, 0.7071, NONE, CT_NS),
    CTO(BAND_FREQ, 2, 2500.0, HZ, OMX_CURVE_SHAPES_NOTCH, FBS_FIXED),
    CTO(BAND_Q, 2, 30.0, NONE, CT_NS, FBS_FIXED),
    CTO(BAND_FREQ, 3, 630.0, HZ, OMX_CURVE_SHAPES_BELL, FBS_LIVE),
    CTO(BAND_GAIN, 3, -9.0, DB, CT_NS, FBS_LIVE),
    CTO(BAND_Q, 3, 16.0, NONE, CT_NS, FBS_LIVE),
    CTO(BAND_FREQ, 4, 1800.0, HZ, OMX_CURVE_SHAPES_BELL, HRP),
    CTO(BAND_GAIN, 4, -4.5, DB, CT_NS, HRP),
    CTO(BAND_Q, 4, 24.0, NONE, CT_NS, HRP),
    CT(HPF_FREQ, 0, 60.0, HZ, CT_NS),
    CT(HPF_SLOPE, 0, 12.0, DB, CT_NS),
};
static const struct omx_curve_request CURVE_GOLDEN_FBS = {
    OMX_CURVE_DISPLAYS_EQ_RESPONSE, OMX_CURVE_FAMILIES_EQ, 48000.0, 48u, 0, 0.0, 0.0,
    CURVE_GOLDEN_FBS_TERMS, (uint32_t)(sizeof CURVE_GOLDEN_FBS_TERMS / sizeof CURVE_GOLDEN_FBS_TERMS[0]), 1};

/* The golden requests by name, in the order tools/curve-golden.c writes them. */
static const struct {
  const char *name;
  const struct omx_curve_request *rq;
} CURVE_GOLDEN_REQUESTS[] = {{"eq", &CURVE_GOLDEN_EQ}, {"comp", &CURVE_GOLDEN_COMP}, {"gate", &CURVE_GOLDEN_GATE},
                           {"fbs", &CURVE_GOLDEN_FBS}};
#define CURVE_GOLDEN_COUNT 4u

#endif /* OMXDSP_TEST_CURVE_REQUESTS_H */
