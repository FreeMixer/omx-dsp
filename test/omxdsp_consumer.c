// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omxdsp_consumer.c — the stateful stages driven the way a consumer drives them, at release flags
 * (the CFLAGS a consumer builds with, -Werror included, no contracts): a state on the stack, never
 * zeroed, armed by an init or a settle whose answer is not read, then run. Every word a stage reads
 * must be one its init or settle wrote, or the compiler sees the indeterminate read and an -O2
 * -Werror build fails here (GCC 13 at -Wmaybe-uninitialized does) before it fails in a consumer.
 *
 * One stage per build, chosen by -DARM_<stage>: what the compiler can see depends on what it
 * inlines, and a consumer's translation unit holds one stage's call, so this one does too.
 *
 *   pitch    an impulse through omx_pitch_state_init + omx_pitch_process, every declared rate;
 *            the wet runs.
 *   chorus   the same through the chorus, flanger through the flanger, over their own rings, at
 *            48 kHz.
 *   rotor    a settled horn ticked over two lines; the output is finite and the phase turns.
 *
 *   make consumer
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_chorus.h>
#include <omxdsp/fx/omx_flanger.h>
#include <omxdsp/fx/omx_pitch.h>
#include <omxdsp/fx/omx_rotary.h>
#include <omxdsp/fx/omx_rotor.h>
#include <omxdsp/omxdsp_definitions.h>

static int g_checks = 0, g_failed = 0;

static void ok(int cond, const char *what, double sr) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL %s @ %.0f Hz\n", what, sr);
  }
}

#if !defined(ARM_rotor)
/* The energy after sample 0 on both legs: the wet arm of a stage fed an impulse. */
static double tail(const float *l, const float *r, uint32_t n) {
  double t = 0.0;
  for (uint32_t i = 1; i < n; i++) t += fabs((double)l[i]) + fabs((double)r[i]);
  return t;
}
#endif

#if defined(ARM_pitch)
static void run(void) {
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    const uint32_t cap = omx_pitch_cap_for(sr), n = cap;
    float *ring_l = calloc(cap, sizeof(float)), *ring_r = calloc(cap, sizeof(float));
    float *l = calloc(n, sizeof(float)), *r = calloc(n, sizeof(float));
    struct omx_pitch p;
    memset(&p, 0, sizeof p);
    omx_pitch_resolve(&p, 1, 7.0f, 0.0f, 50.0f, sr);
    struct omx_pitch_state s;
    omx_pitch_state_init(&s, ring_l, ring_r, cap);
    l[0] = r[0] = 1.0f;
    omx_pitch_process(l, r, n, &p, &s);
    const double t = tail(l, r, n);
    ok(t > 1e-3 && t - t == 0.0, "P the pitch shifter's wet runs", sr);
    free(ring_l);
    free(ring_r);
    free(l);
    free(r);
  }
}

#elif defined(ARM_chorus)
#define N 2048u
static void run(void) {
  static float ring_l[OMX_CHORUS_CAP], ring_r[OMX_CHORUS_CAP], l[N], r[N];
  struct omx_chorus p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.voices = 1;
  p.base_samples = OMX_CHORUS_BASE_MS * 48.0f;
  p.depth_samples = 4.0f * 48.0f;
  p.mix = 0.5f;
  struct omx_chorus_state s;
  omx_chorus_state_init(&s, ring_l, ring_r, OMX_CHORUS_CAP);
  memset(l, 0, sizeof l);
  memset(r, 0, sizeof r);
  l[0] = r[0] = 1.0f;
  omx_chorus_process(l, r, N, &p, &s);
  const double t = tail(l, r, N);
  ok(t > 1e-3 && t - t == 0.0, "C the chorus's wet runs", 48000.0);
}
#elif defined(ARM_flanger)
#define N 2048u
static void run(void) {
  static float ring_l[OMX_FLANGER_CAP], ring_r[OMX_FLANGER_CAP], l[N], r[N];
  struct omx_flanger p;
  memset(&p, 0, sizeof p);
  p.enabled = 1;
  p.base_samples = OMX_FLANGER_BASE_MS * 48.0f;
  p.depth_samples = 2.0f * 48.0f;
  p.feedback = 0.5f;
  p.mix = 0.5f;
  struct omx_flanger_state s;
  omx_flanger_state_init(&s, ring_l, ring_r, OMX_FLANGER_CAP);
  memset(l, 0, sizeof l);
  memset(r, 0, sizeof r);
  l[0] = r[0] = 1.0f;
  omx_flanger_process(l, r, N, &p, &s);
  const double t = tail(l, r, N);
  ok(t > 1e-3 && t - t == 0.0, "F the flanger's wet runs", 48000.0);
}
#elif defined(ARM_rotor)
#define N 2048u
static void run(void) {
  static float ring_l[OMX_ROTARY_RING_FLOATS], ring_r[OMX_ROTARY_RING_FLOATS];
  struct omx_fdelay ll, lr;
  omx_fdelay_init(&ll, ring_l, OMX_ROTARY_RING_FLOATS, OMX_ROTOR_ORDER);
  omx_fdelay_init(&lr, ring_r, OMX_ROTARY_RING_FLOATS, OMX_ROTOR_ORDER);
  struct omx_rotor_state s;
  omx_rotor_settle(&s, 0.8f, 0.0f);
  struct omx_rotor p;
  omx_rotor_resolve(&p, &s, 6.8f, OMX_ROTARY_HORN_ACCEL_MS, OMX_ROTARY_HORN_DECEL_MS, 1.0f,
                    OMX_ROTARY_HORN_DOPPLER_MS, OMX_ROTARY_HORN_AM, 48000.0f);
  const float u0 = s.lfo.phase, c0 = s.lfo.carry;
  double sum = 0.0;
  for (uint32_t i = 0; i < N; i++) {
    float yl = 0.0f, yr = 0.0f;
    omx_rotor_tick(&p, &s, &ll, &lr, 1.0f, 1.0f, &yl, &yr);
    sum += (double)yl + (double)yr;
  }
  ok(sum - sum == 0.0 && sum > 0.0, "R the rotor's output is finite", 48000.0);
  ok(s.lfo.phase != u0 && c0 == 0.0f, "R a settled rotor starts with no carry and turns", 48000.0);
}

#else
#error "build with -DARM_<stage>: pitch, chorus, flanger or rotor"
#endif

int main(void) {
  run();
  printf("omxdsp_consumer: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
