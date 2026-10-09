// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * kernel-cost.c — one effect kernel's cost alone, ns per sample, at every declared rate.
 *
 * The method: a non-silent stereo input (a -12 dBFS two-tone per leg plus a small noise floor,
 * phase-continuous across blocks, so neither a denormal path nor a "silence is cheap" branch is
 * measured); 512-frame blocks, the input written outside the timed region; 64 warm-up blocks
 * discarded, then 2048 blocks each timed on CLOCK_MONOTONIC; the median and the p95 block divided
 * by the block length. Contracts off. Kernel alone, not an engine's walk or a plugin's shell.
 *
 * Parts (argv[1], default `all`), each at two settings: `default`, the contract's come-up values,
 * and `full`, every optional path of the kernel engaged with its controls at the travels' ends
 * (a reference point, not a proof of the worst case: a kernel may be cheaper there):
 *   gate     omx_gate_run. default: the gate's declared defaults, the two-tone above the
 *            threshold (open). full: threshold at its roof and ratio at its floor, so every
 *            sample sits on the expansion slope, and a 0.25 ms attack, which engages 4x.
 *   flanger  omx_flanger_process. default: base OMX_FLANGER_BASE_MS, the declared depth, rate,
 *            feedback and mix. full: the deepest and fastest sweep, feedback at the clamp, wet.
 *   drive    omx_drive_process at 4x. default: SOFT, full band, 0 dB, character 0, wet, auto-gain
 *            linked, no roll-off. full: the same curve on the high band split, the amount roof,
 *            half wet, auto-gain per leg, the roll-off on.
 *   ovs      the shared oversampler alone, 4x up then down, one leg (the drive's attribution).
 *   shape    the SOFT shaper alone over 4n samples, one leg (the other half of it).
 *   deesser  omx_deess_process, split. default: the declared defaults. full: the threshold floor,
 *            the ratio roof, the range floor and the widest band, so the stage always reduces.
 *   delay    omx_fx_delay_process. default: the declared time and feedback, half wet. full: the
 *            time roof (the largest ring walk), feedback at the clamp, wet, a dark tone and
 *            ping-pong.
 * `ovs` and `shape` have one setting, `default`.
 *
 * Prints one TSV row per part, setting and rate: part, setting, rate, quantum, blocks,
 * p50_ns_per_sample, p95_ns_per_sample and an FNV-1a digest of every output sample, so two builds
 * timed against each other (tools/kernel-cost-ab.sh) also show they rendered the same bits: a
 * faster kernel that renders something else is not the same kernel. The numbers are relative to
 * the machine that ran them. Exits 1 on a non-finite output sample, 2 on an unknown part.
 */
#include <omxdsp/omxdsp.h>
#include <omxdsp/omx_eq_design.h>
#include <omxdsp/omx_gate.h>
#include <omxdsp/fx/omx_deesser.h>
#include <omxdsp/fx/omx_delay.h>
#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/fx/omx_flanger.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define Q 512u
#define WARM 64
#define BLOCKS 2048

static float g_l[Q], g_r[Q], g_up[Q * OMX_OVS_MAX_FACTOR];
static uint64_t g_t[BLOCKS];
static float g_fl_l[OMX_FLANGER_CAP], g_fl_r[OMX_FLANGER_CAP];
static float g_dl_l[OMX_FXDELAY_CAP], g_dl_r[OMX_FXDELAY_CAP];
static struct omx_gate g_gate;
static int g_finite = 1;

static uint64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
  const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

static uint64_t fnv(uint64_t h, const float *x, uint32_t n) {
  const unsigned char *p = (const unsigned char *)x;
  for (size_t i = 0; i < (size_t)n * sizeof(float); i++) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}

/* The two-tone per leg at -12 dBFS peak, a noise floor 60 dB under it. */
static void fill(uint64_t *phase, float sr, uint32_t *seed) {
  for (uint32_t i = 0; i < Q; i++) {
    const double t = (double)(*phase + i) / sr;
    *seed = *seed * 1664525u + 1013904223u;
    const float noise = ((float)(*seed >> 8) / 16777216.0f - 0.5f) * 1e-3f;
    g_l[i] = 0.167f * (float)sin(2.0 * M_PI * 997.0 * t) + 0.084f * (float)sin(2.0 * M_PI * 3001.0 * t) + noise;
    g_r[i] = 0.167f * (float)sin(2.0 * M_PI * 1499.0 * t) + 0.084f * (float)sin(2.0 * M_PI * 5003.0 * t) - noise;
  }
  *phase += Q;
}

static void identity5(float *c) { c[0] = 1.0f; c[1] = c[2] = c[3] = c[4] = 0.0f; }

/* One part at one setting and one rate: every block through `run`, the median and p95 printed. */
struct cell {
  const char *part;
  int full;
  float sr;
  void *ctx;
  void (*run)(struct cell *c);
};

static void timed(struct cell *c) {
  uint64_t phase = 0, h = 1469598103934665603ull;
  uint32_t seed = 12345u;
  for (int b = 0; b < WARM + BLOCKS; b++) {
    fill(&phase, c->sr, &seed);
    const uint64_t t0 = now_ns();
    c->run(c);
    const uint64_t t1 = now_ns();
    if (!isfinite(g_l[Q - 1]) || !isfinite(g_r[Q - 1])) g_finite = 0;
    if (b >= WARM) {
      g_t[b - WARM] = t1 - t0;
      h = fnv(h, g_l, Q);
      h = fnv(h, g_r, Q);
    }
  }
  qsort(g_t, BLOCKS, sizeof g_t[0], cmp_u64);
  printf("%s\t%s\t%.0f\t%u\t%d\t%.3f\t%.3f\t%016llx\n", c->part, c->full ? "full" : "default",
         (double)c->sr, Q, BLOCKS, (double)g_t[BLOCKS / 2] / Q, (double)g_t[(BLOCKS * 95) / 100] / Q,
         (unsigned long long)h);
}

/* ---- gate ---------------------------------------------------------------------------------- */
static void gate_run(struct cell *c) {
  omx_gate_run(&g_gate, (const struct omx_dyn *)c->ctx, NULL, NULL, g_l, g_r, g_l, g_r, Q);
}
static void gate(int full, float sr) {
  const float thr = OMX_GATE_THRESHOLD_DB_MAX, ratio = OMX_GATE_RATIO_MIN, attack = 0.25f;
  struct omx_gate_controls ctl = {0};
  if (full) { ctl.threshold = &thr; ctl.ratio = &ratio; ctl.attack = &attack; }
  struct omx_dyn p;
  omx_gate_init(&g_gate, sr);
  omx_gate_resolve(&g_gate, &ctl, &p);
  struct cell c = {"gate", full, sr, &p, gate_run};
  timed(&c);
}

struct flanger_ctx {
  struct omx_flanger p;
  struct omx_flanger_state s;
};
struct deess_ctx {
  struct omx_deess p;
  struct omx_deess_state s;
};
struct fx_delay_ctx {
  struct omx_fx_delay p;
  struct omx_fx_delay_state s;
};

/* ---- flanger ------------------------------------------------------------------------------- */
static void flanger_run(struct cell *c) {
  struct flanger_ctx *x = c->ctx;
  omx_flanger_process(g_l, g_r, Q, &x->p, &x->s);
}
static void flanger(int full, float sr) {
  struct flanger_ctx f;
  memset(&f, 0, sizeof f);
  f.p.enabled = 1;
  f.p.base_samples = OMX_FLANGER_BASE_MS * sr / 1000.0f;
  f.p.depth_samples = (full ? OMX_FLANGER_MAX_DEPTH_MS : (float)OMX_FLANGER_DEPTH_RANGE_DEFAULT) * sr / 1000.0f;
  f.p.lfo_inc = omx_lfo_inc(full ? (float)OMX_FLANGER_RATE_RANGE_MAX : OMX_FLANGER_RATE_RANGE_DEFAULT, sr);
  f.p.feedback = full ? OMX_FLANGER_FB_MAX : OMX_FLANGER_FEEDBACK_RANGE_DEFAULT;
  f.p.mix = full ? 1.0f : (float)OMX_FLANGER_MIX_RANGE_DEFAULT / 100.0f;
  memset(g_fl_l, 0, sizeof g_fl_l);
  memset(g_fl_r, 0, sizeof g_fl_r);
  if (omx_flanger_state_init(&f.s, g_fl_l, g_fl_r, OMX_FLANGER_CAP) != OMX_FDELAY_OK) g_finite = 0;
  struct cell c = {"flanger", full, sr, &f, flanger_run};
  timed(&c);
}

/* ---- drive, and its two halves -------------------------------------------------------------- */
struct drive_ctx {
  struct omx_drive p;
  struct omx_drive_state *st;
  struct omx_oversampler os;
};
static void drive_run(struct cell *c) {
  struct drive_ctx *d = c->ctx;
  omx_drive_process(g_l, g_r, Q, &d->p, d->st);
}
static void ovs_run(struct cell *c) {
  struct drive_ctx *d = c->ctx;
  omx_oversampler_up(&d->os, g_l, Q, g_up);
  omx_oversampler_down(&d->os, g_up, Q, g_l);
}
static void shape_run(struct cell *c) {
  (void)c;
  for (uint32_t i = 0; i < Q * 4u; i++) g_up[i] = omx_drive_shape(OMX_DRIVE_SOFT, g_l[i / 4u]);
  for (uint32_t i = 0; i < Q; i++) g_l[i] = g_up[4u * i + 3u];
}
static void drive(const char *part, int full, float sr) {
  struct drive_ctx d;
  memset(&d, 0, sizeof d);
  struct omx_drive *p = &d.p;
  p->enabled = 1;
  p->curve = OMX_DRIVE_SOFT;
  p->band = full ? OMX_DRIVE_BAND_HIGH : OMX_DRIVE_BAND_FULL;
  p->drive_lin = full ? powf(10.0f, (float)OMX_DRIVE_AMOUNT_RANGE_MAX / 20.0f) : 1.0f;
  p->even_w = 0.5f;
  p->mix = full ? 0.5f : 1.0f;
  p->trim_lin = 1.0f;
  p->auto_gain = 1;
  p->stereo_link = !full;
  p->hf_on = full;
  p->os_factor = omx_drive_factor_of(4);
  identity5(p->band_c); identity5(p->tilt_c); identity5(p->tilt_inv_c); identity5(p->hf_c);
  if (full) {
    omx_eq_design_f(OMX_EQ_LOWPASS, OMX_DRIVE_BAND_HZ_DEFAULT, 0.7071, 0.0, sr, p->band_c);
    omx_eq_design_f(OMX_EQ_LOWPASS, 12000.0, 0.7071, 0.0, sr, p->hf_c);
  }
  omx_drive_time_constants(p, sr);
  d.st = calloc(1, sizeof *d.st);
  if (!d.st) { g_finite = 0; return; }
  omx_drive_state_init(d.st, p->os_factor);
  omx_oversampler_init(&d.os, 4u);
  struct cell c = {part, full, sr, &d, !strcmp(part, "ovs") ? ovs_run : !strcmp(part, "shape") ? shape_run : drive_run};
  timed(&c);
  free(d.st);
}

/* ---- deesser ------------------------------------------------------------------------------- */
static void deesser_run(struct cell *c) {
  struct deess_ctx *x = c->ctx;
  omx_deess_process(g_l, g_r, Q, &x->p, &x->s);
}
static void deesser(int full, float sr) {
  struct deess_ctx d;
  memset(&d, 0, sizeof d);
  const double width = full ? (double)OMX_DEESS_WIDTH_RANGE_MAX : (double)OMX_DEESS_WIDTH_RANGE_DEFAULT;
  d.p.enabled = 1;
  d.p.mode = OMX_DEESS_SPLIT;
  d.p.dyn.enabled = 1;
  d.p.dyn.gc.mode = OMX_DYN_ABOVE;
  d.p.dyn.detect = OMX_DETECT_PEAK;
  d.p.dyn.gc.thresh_db = full ? (float)OMX_DEESS_THRESHOLD_RANGE_MIN : (float)OMX_DEESS_THRESHOLD_RANGE_DEFAULT;
  d.p.dyn.gc.ratio = full ? (float)OMX_DEESS_RATIO_RANGE_MAX : (float)OMX_DEESS_RATIO_RANGE_DEFAULT;
  d.p.dyn.gc.knee_db = (float)OMX_DEESS_KNEE_DB;
  d.p.dyn.gc.range_db = full ? (float)OMX_DEESS_RANGE_RANGE_MIN : (float)OMX_DEESS_RANGE_RANGE_DEFAULT;
  d.p.dyn.gc.makeup_lin = 1.0f;
  d.p.dyn.attack_ms = (float)OMX_DEESS_ATTACK_RANGE_DEFAULT;
  d.p.dyn.ovs_mode = OMX_DYN_OVS_OFF;
  d.p.dyn.attack_coeff = omx_pole_from_time_ms((float)OMX_DEESS_ATTACK_RANGE_DEFAULT, sr);
  d.p.dyn.release_coeff = omx_pole_from_time_ms((float)OMX_DEESS_RELEASE_RANGE_DEFAULT, sr);
  /* The cookbook's bandwidth-in-octaves Q at the band's centre. */
  const double w0 = 2.0 * M_PI * OMX_DEESS_FREQ_RANGE_DEFAULT / sr;
  const double q = 1.0 / (2.0 * sinh(M_LN2 / 2.0 * width * w0 / sin(w0)));
  omx_eq_design_f(OMX_EQ_BANDPASS, OMX_DEESS_FREQ_RANGE_DEFAULT, q, 0.0, sr, d.p.bp_c);
  omx_deess_state_init(&d.s);
  struct cell c = {"deesser", full, sr, &d, deesser_run};
  timed(&c);
}

/* ---- delay --------------------------------------------------------------------------------- */
static void delay_run(struct cell *c) {
  struct fx_delay_ctx *x = c->ctx;
  omx_fx_delay_process(g_l, g_r, Q, &x->p, &x->s, c->sr);
}
static void delay(int full, float sr) {
  struct fx_delay_ctx d;
  memset(&d, 0, sizeof d);
  const uint32_t tap = omx_fxdelay_ms_to_samples(full ? (float)OMX_FX_DELAY_TIME_RANGE_MAX : (float)OMX_FX_DELAY_TIME_RANGE_DEFAULT, sr);
  d.p.enabled = 1;
  d.p.d_l = omx_fxdelay_clamp(tap, OMX_FXDELAY_CAP);
  d.p.d_r = d.p.d_l;
  d.p.feedback = full ? OMX_FX_DELAY_FEEDBACK_RANGE_MAX : OMX_FX_DELAY_FEEDBACK_RANGE_DEFAULT;
  d.p.mix = full ? 1.0f : 0.5f;
  d.p.tone = full ? 0.5f : 0.0f;
  d.p.pingpong = full;
  memset(g_dl_l, 0, sizeof g_dl_l);
  memset(g_dl_r, 0, sizeof g_dl_r);
  d.s.ring_l = g_dl_l;
  d.s.ring_r = g_dl_r;
  d.s.cap = OMX_FXDELAY_CAP;
  struct cell c = {"delay", full, sr, &d, delay_run};
  timed(&c);
}

static const char *const PARTS[] = {"gate", "flanger", "drive", "ovs", "shape", "deesser", "delay"};
#define NPARTS (sizeof PARTS / sizeof PARTS[0])

static void part_at(const char *part, float sr) {
  const int two = strcmp(part, "ovs") && strcmp(part, "shape");
  for (int full = 0; full <= two; full++) {
    if (!strcmp(part, "gate")) gate(full, sr);
    else if (!strcmp(part, "flanger")) flanger(full, sr);
    else if (!strcmp(part, "deesser")) deesser(full, sr);
    else if (!strcmp(part, "delay")) delay(full, sr);
    else drive(part, full, sr);
  }
}

int main(int argc, char **argv) {
  const char *want = argc > 1 ? argv[1] : "all";
  int known = !strcmp(want, "all");
  for (size_t i = 0; i < NPARTS; i++) known |= !strcmp(want, PARTS[i]);
  if (!known) {
    fprintf(stderr, "kernel-cost: unknown part '%s' (all, gate, flanger, drive, ovs, shape, deesser, delay)\n", want);
    return 2;
  }
  printf("part\tsetting\trate\tquantum\tblocks\tp50_ns_per_sample\tp95_ns_per_sample\toutput_fnv1a\n");
  for (size_t i = 0; i < NPARTS; i++) {
    if (strcmp(want, "all") && strcmp(want, PARTS[i])) continue;
    for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) part_at(PARTS[i], OMX_DECLARED_RATES[ri]);
  }
  fflush(stdout);
  if (!g_finite) {
    fprintf(stderr, "kernel-cost: a non-finite output sample, or a kernel that would not arm\n");
    return 1;
  }
  return 0;
}
