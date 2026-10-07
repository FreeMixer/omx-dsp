// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * fbs_detect.test.c — the FBS detector's closed-form oracles, at every declared rate.
 *
 * Each case builds its spectra by hand (a tone over a −100 dB floor), so the expected verdict and
 * the confirming frame follow from the gates alone. The frame size is the RTA's for the rate
 * (fftSizeForRate, from the same declared constants), so a gate that only holds at 48 kHz fails
 * here at 192 kHz.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The compiled unit is included, not linked, so its file-static gates are testable by name. */
#include "omx_fbs_detect.c"

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what, double rate) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL @%.0f Hz: %s\n", rate, what);
  }
}

static uint32_t fft_for_rate(double rate) {
  uint32_t n = 2048;
  while (n < OMX_RTA_FFT_SIZE_MAX && rate / n > OMX_RTA_TARGET_BIN_HZ_DOUBLE) n *= 2;
  return n;
}

#define FLOOR_LIN 1e-5

struct rig {
  OmxFbsDetector d;
  int32_t *bin_slot;
  OmxFbsSlot *slots;
  uint32_t *free_stack;
  OmxFbsCandidate *confirmed;
  OmxFbsCandidate *snapshot;
  uint32_t *active;
  OmxFbsResult r;
  double *mags;
  uint32_t nbins;
  double rate;
  uint32_t fft;
};

static void rig_open(struct rig *g, double rate, const OmxFbsThresholds *t, OmxFbsPool pool, uint32_t n_slots) {
  memset(g, 0, sizeof(*g));
  g->rate = rate;
  g->fft = fft_for_rate(rate);
  g->nbins = g->fft / 2;
  g->bin_slot = calloc(g->nbins, sizeof(int32_t));
  g->slots = calloc(n_slots, sizeof(OmxFbsSlot));
  g->free_stack = calloc(n_slots, sizeof(uint32_t));
  g->confirmed = calloc(n_slots, sizeof(OmxFbsCandidate));
  g->snapshot = calloc(n_slots, sizeof(OmxFbsCandidate));
  g->active = calloc(n_slots, sizeof(uint32_t));
  g->mags = calloc(g->nbins, sizeof(double));
  g->r.confirmed = g->confirmed;
  g->r.snapshot = g->snapshot;
  g->r.active = g->active;
  g->d = (OmxFbsDetector){.t = *t, .pool = pool, .sample_rate = rate, .fft_size = g->fft, .bin_slot = g->bin_slot,
                          .nbins_cap = g->nbins, .slots = g->slots, .free_stack = g->free_stack, .n_slots = n_slots};
  omx_fbs_init(&g->d);
}

static void rig_close(struct rig *g) {
  free(g->bin_slot);
  free(g->slots);
  free(g->free_stack);
  free(g->confirmed);
  free(g->snapshot);
  free(g->active);
  free(g->mags);
}

static void floor_fill(struct rig *g) {
  for (uint32_t i = 0; i < g->nbins; i++) g->mags[i] = FLOOR_LIN;
}

static double lin(double db) { return pow(10.0, db / 20.0); }

static uint32_t bin_of(const struct rig *g, double hz) { return (uint32_t)floor(hz * g->fft / g->rate + 0.5); }

static void push(struct rig *g) { omx_fbs_push(&g->d, g->mags, g->nbins, &g->r); }

/* A linear-in-dB run-away (3 dB/frame from −60) confirms exactly once, on frame 4: persistence
 * needs 5 frames, the ramp gate holds from frame 2 and must hold twice. */
static void case_ramp(double rate) {
  struct rig g;
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  rig_open(&g, rate, &t, OMX_FBS_POOL_LIVE, OMX_FBS_TRACK_SLOTS);
  const uint32_t b = bin_of(&g, 1000.0);
  int confirms = 0, at = -1;
  OmxFbsCandidate c = {0};
  for (int i = 0; i < 10; i++) {
    floor_fill(&g);
    g.mags[b] = lin(-60.0 + 3.0 * i);
    push(&g);
    if (g.r.n_confirmed) {
      confirms += (int)g.r.n_confirmed;
      if (at < 0) {
        at = i;
        c = g.r.confirmed[0];
      }
    }
    check(g.r.n_active == 1 && g.active[0] == b, "the ramp's bin is the one active bin", rate);
  }
  check(confirms == 1, "a run-away confirms exactly once", rate);
  check(at == 4, "and on frame 4", rate);
  check(c.bin == b, "at the ramp's bin", rate);
  check(c.freq_hz == ((double)b * rate) / (double)g.fft, "freqHz is bin * rate / fftSize", rate);
  check(c.depth_db == -(double)OMX_FBS_MAX_NOTCH_DEPTH_DB, "a tone 100 dB over the floor asks the full depth law", rate);
  check(c.q == (double)OMX_FBS_DEFAULT_NOTCH_Q, "the notch Q is the declared default", rate);
  check(c.persistence == 5, "persistence is the streak at confirmation", rate);
  check(fabs(c.level_db - -48.0) < 1e-9, "levelDb is the bin's own level", rate);
  check(g.r.n_snapshot == 1 && g.snapshot[0].bin == b, "the confirmed bin stays in the snapshot", rate);
  check(fabs(g.snapshot[0].level_db - (-60.0 + 3.0 * 9)) < 1e-9, "the snapshot reads the level of THIS frame", rate);
  floor_fill(&g);
  push(&g);
  check(g.r.n_snapshot == 0 && g.r.n_active == 0, "a ring that stops leaves the snapshot and the active bins", rate);
  rig_close(&g);
}

/* A held tone is persistent and prominent and never runs away: active, never confirmed. */
static void case_held(double rate) {
  struct rig g;
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  rig_open(&g, rate, &t, OMX_FBS_POOL_LIVE, OMX_FBS_TRACK_SLOTS);
  const uint32_t b = bin_of(&g, 440.0);
  int confirms = 0;
  for (int i = 0; i < 30; i++) {
    floor_fill(&g);
    g.mags[b] = lin(-30.0);
    push(&g);
    confirms += (int)g.r.n_confirmed;
  }
  check(confirms == 0, "a held tone never confirms", rate);
  check(g.r.n_active == 1 && g.active[0] == b, "but reads as active", rate);
  rig_close(&g);
}

/* PNPR's neighbourhood is ±3 bins with the peak's own ±1 skirt excluded: equal lines 4 bins either
 * side leave all three candidates; at 3 bins they would drown the centre's ratio. */
static void case_pnpr_reach(double rate) {
  struct rig g;
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  rig_open(&g, rate, &t, OMX_FBS_POOL_LIVE, OMX_FBS_TRACK_SLOTS);
  const uint32_t b = bin_of(&g, 1200.0);
  floor_fill(&g);
  g.mags[b - 4] = g.mags[b] = g.mags[b + 4] = lin(-30.0);
  push(&g);
  check(g.r.n_active == 3 && g.active[1] == b, "lines 4 bins apart are three candidates", rate);
  floor_fill(&g);
  g.mags[b - 3] = g.mags[b] = g.mags[b + 3] = lin(-30.0);
  push(&g);
  int centre = 0;
  for (uint32_t i = 0; i < g.r.n_active; i++) centre |= g.active[i] == b;
  check(!centre, "lines 3 bins either side take the centre out of candidacy", rate);
  rig_close(&g);
}

/* A decaying tone persists as a candidate and never confirms. */
static void case_decay(double rate) {
  struct rig g;
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  rig_open(&g, rate, &t, OMX_FBS_POOL_LIVE, OMX_FBS_TRACK_SLOTS);
  const uint32_t b = bin_of(&g, 800.0);
  int confirms = 0;
  for (int i = 0; i < 12; i++) {
    floor_fill(&g);
    g.mags[b] = lin(-20.0 - 3.0 * i);
    push(&g);
    confirms += (int)g.r.n_confirmed;
  }
  check(confirms == 0, "a decaying tone never confirms", rate);
  rig_close(&g);
}

/* An attack: flat, then one +10 dB hop, then flat. One hop carries the whole rise (#106). */
static void case_attack(double rate) {
  struct rig g;
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  rig_open(&g, rate, &t, OMX_FBS_POOL_LIVE, OMX_FBS_TRACK_SLOTS);
  const uint32_t b = bin_of(&g, 2000.0);
  int confirms = 0;
  for (int i = 0; i < 12; i++) {
    floor_fill(&g);
    g.mags[b] = lin(i < 4 ? -50.0 : -40.0);
    push(&g);
    confirms += (int)g.r.n_confirmed;
  }
  check(confirms == 0, "an attack step never confirms", rate);
  rig_close(&g);
}

/* An overtone: a climbing line at 2f riding a static line at f that stays within 10 dB above it
 * (PSHR < 10 on every frame, #116) never confirms; the static line at f never runs away. */
static void case_overtone(double rate) {
  struct rig g;
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  rig_open(&g, rate, &t, OMX_FBS_POOL_LIVE, OMX_FBS_TRACK_SLOTS);
  const uint32_t f = bin_of(&g, 300.0);
  int confirms = 0;
  double worst_pshr = -INFINITY;
  for (int i = 0; i < 16; i++) {
    floor_fill(&g);
    g.mags[f] = lin(-20.0);
    const double up = -60.0 + 3.0 * i;
    g.mags[2 * f] = lin(up < -32.0 ? up : -32.0);
    push(&g);
    confirms += (int)g.r.n_confirmed;
    const double p = omx_fbs_pshr(g.mags, g.nbins, 2 * f);
    if (p > worst_pshr) worst_pshr = p;
  }
  check(confirms == 0, "an overtone riding its fundamental never confirms", rate);
  check(worst_pshr < OMX_PSHR_MIN_DB, "its PSHR never reaches the bar", rate);
  rig_close(&g);
}

/* A pure tone over the floor has no sub-line; a line at f/2 six dB down reads PSHR 6. */
static void case_pshr(double rate) {
  struct rig g;
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  rig_open(&g, rate, &t, OMX_FBS_POOL_LIVE, 4);
  floor_fill(&g);
  g.mags[300] = lin(-30.0);
  check(omx_fbs_pshr(g.mags, g.nbins, 300) > 60.0, "a lone tone's PSHR is far above the bar", rate);
  g.mags[151] = lin(-36.0);
  g.mags[99] = lin(-50.0);
  check(fabs(omx_fbs_pshr(g.mags, g.nbins, 300) - 6.0) < 1e-9, "PSHR reads the strongest line near f/2 or f/3", rate);
  rig_close(&g);
}

/* Two rings confirmed on different frames: the snapshot keeps confirmation order, not bin order;
 * `confirmed` on a frame is ascending bin. */
static void case_snapshot_order(double rate) {
  struct rig g;
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  rig_open(&g, rate, &t, OMX_FBS_POOL_FIXED, OMX_FBS_TRACK_SLOTS);
  const uint32_t hi = bin_of(&g, 3000.0), lo = bin_of(&g, 700.0);
  for (int i = 0; i < 12; i++) {
    floor_fill(&g);
    g.mags[hi] = lin(fmin(-60.0 + 3.0 * i, -20.0));
    if (i >= 3) g.mags[lo] = lin(fmin(-60.0 + 3.0 * (i - 3), -20.0));
    push(&g);
  }
  check(g.r.n_snapshot == 2, "both rings are in the snapshot", rate);
  check(g.r.n_snapshot == 2 && g.snapshot[0].bin == hi && g.snapshot[1].bin == lo,
        "the ring confirmed first leads the snapshot, whatever its bin", rate);
  check(g.r.n_active == 2 && g.active[0] == lo && g.active[1] == hi, "active bins ascend", rate);
  rig_close(&g);
}

/* A ring that drops out of candidacy for one frame starts over: its confirmation moves later. */
static void case_dropout(double rate) {
  struct rig g;
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  rig_open(&g, rate, &t, OMX_FBS_POOL_LIVE, OMX_FBS_TRACK_SLOTS);
  const uint32_t b = bin_of(&g, 1500.0);
  int at = -1;
  for (int i = 0; i < 14; i++) {
    floor_fill(&g);
    if (i != 3) g.mags[b] = lin(-60.0 + 3.0 * i);
    push(&g);
    if (g.r.n_confirmed && at < 0) at = i;
  }
  check(at == 8, "a one-frame dropout restarts the streak (confirms on frame 8, not 4)", rate);
  rig_close(&g);
}

/* More candidate bins than slots: the overflow is COUNTED, and the tracked ones still confirm. */
static void case_exhaustion(double rate) {
  struct rig g;
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  rig_open(&g, rate, &t, OMX_FBS_POOL_LIVE, 2);
  const uint32_t b[3] = {bin_of(&g, 1000.0), bin_of(&g, 1700.0), bin_of(&g, 2900.0)};
  uint32_t confirms = 0;
  for (int i = 0; i < 8; i++) {
    floor_fill(&g);
    for (int k = 0; k < 3; k++) g.mags[b[k]] = lin(-60.0 + 3.0 * i);
    push(&g);
    check(g.r.untracked == 1, "the third candidate is counted as untracked", rate);
    check(g.r.n_active == 2, "two bins are tracked", rate);
    confirms += g.r.n_confirmed;
  }
  check(confirms == 2, "the tracked two confirm", rate);
  rig_close(&g);
}

/* The ramp gate over histories: the #106 corpus shapes, verbatim. */
static void case_is_growing(void) {
  OmxFbsThresholds t = omx_fbs_thresholds_default();
  const double ramp[] = {-60, -56, -52, -48, -44};
  const double steady[] = {-60, -57.5, -55.4, -53, -50.6};
  const double wobble[] = {-58, -55.9, -56.2, -53.5, -51.2};
  const double attack1[] = {-57.3, -58.2, -59.4, -55.6, -52.8};
  const double attack2[] = {-55.2, -54.2, -54.2, -45.3, -44.9};
  const double stepdom[] = {-44.4, -43.8, -42.4, -42.1, -36.8};
  const double swell[] = {-59.5, -55.1, -53.7, -52.9, -53.1};
  const double two[] = {-60, -50};
  const double flat[] = {-6, -6, -6, -6, -6}, fading[] = {2, -2, -6, -10, -14};
  const double spike[] = {-6, -5.9, 3, -5.9, -8}, clean[] = {-10, -4, 2, 8, 14};
  check(!omx_fbs_is_growing(flat, 5, &t), "a flat run is a held note", 0);
  check(!omx_fbs_is_growing(fading, 5, &t), "a fading run is a decay", 0);
  check(!omx_fbs_is_growing(spike, 5, &t), "one mid-window spike whose end is not above its start is no ramp", 0);
  check(omx_fbs_is_growing(clean, 5, &t), "a clean 6 dB/frame ramp grows", 0);
  const double flat4[] = {-6, -6, -6, -6}, climb[] = {-10, -6, -2, 2}, fall[] = {2, -2, -6, -10};
  check(fabs(growth_slope(flat4, 4)) < 1e-12, "the slope of a flat run is 0", 0);
  check(growth_slope(climb, 4) > OMX_GROWTH_MIN_DB_PER_FRAME_DOUBLE, "a climb's slope clears the bar", 0);
  check(growth_slope(fall, 4) < 0, "a fade's slope is negative", 0);
  check(growth_slope(flat4, 1) == 0 && growth_slope(flat4, 0) == 0, "under two samples there is no slope", 0);
  check(omx_fbs_is_growing(ramp, 5, &t), "4 dB/frame ramp grows", 0);
  check(omx_fbs_is_growing(steady, 5, &t), "2.4 dB/frame ramp grows", 0);
  check(omx_fbs_is_growing(wobble, 5, &t), "a -0.3 dB wobble frame still grows", 0);
  check(!omx_fbs_is_growing(attack1, 5, &t), "flat-then-jump is an attack", 0);
  check(!omx_fbs_is_growing(attack2, 5, &t), "a re-articulation is an attack", 0);
  check(!omx_fbs_is_growing(stepdom, 5, &t), "one hop over half the rise is a step", 0);
  check(!omx_fbs_is_growing(swell, 5, &t), "a levelling swell is a crescendo", 0);
  check(!omx_fbs_is_growing(two, 2, &t), "two samples are never a measured ramp", 0);
}

/* PAPR's floor is O(guard) off a once-per-frame total; it must agree with the O(n) scan it
 * replaced at every bin of spiky random spectra (a deterministic LCG, so a failure reproduces). */
static double papr_reference(const double *m, uint32_t n, uint32_t bin, uint32_t guard) {
  double sum = 0;
  uint32_t count = 0;
  for (uint32_t i = 0; i < n; i++) {
    if ((i > bin ? i - bin : bin - i) <= guard) continue;
    sum += m[i];
    count += 1;
  }
  const double avg = count > 0 ? sum / count : 0;
  return 20.0 * log10(fmax(m[bin], 1e-12)) - 20.0 * log10(fmax(avg, 1e-12));
}

static void case_papr_floor(void) {
  uint32_t s = 0x0b;
  double worst = 0;
  double m[128];
  for (int trial = 0; trial < 20; trial++) {
    s = s * 1664525u + 1013904223u;
    const uint32_t n = 8 + (uint32_t)((s / 4294967296.0) * 120);
    for (uint32_t i = 0; i < n; i++) {
      s = s * 1664525u + 1013904223u;
      const double r = s / 4294967296.0;
      m[i] = r * r * r;
    }
    s = s * 1664525u + 1013904223u;
    m[(uint32_t)((s / 4294967296.0) * n)] = 1.0;
    const double total = spectrum_sum(m, n);
    for (uint32_t bin = 0; bin < n; bin++) {
      const double got = to_db(m[bin]) - to_db(average_excluding(m, n, bin, OMX_FBS_PAPR_GUARD_BINS, total));
      const double d = fabs(got - papr_reference(m, n, bin, OMX_FBS_PAPR_GUARD_BINS));
      if (d > worst) worst = d;
    }
  }
  check(worst < 1e-9, "PAPR's O(guard) floor equals the O(n) scan at every bin", 0);
  const double one[1] = {1.0};
  check(isfinite(papr(one, 1, 0, 1.0)), "a one-bin spectrum answers without dividing by zero", 0);
}

/* The plateau gate: a held near-clip window passes; a level under the floor, a hop under -dip or
 * too short a history fails. */
static void case_plateau(void) {
  const double held[] = {-4, -4, -4, -4, -4}, jump_in[] = {-60, -4, -4, -4}, low[] = {-20, -20, -20};
  const double dip1[] = {-4, -4, -8, -4}, dip2[] = {-4, -4, -4, -8, -12}, short_[] = {-4, -4};
  check(is_saturated_plateau(held, 5, -10.8, 0.5, 3), "a held near-clip plateau passes", 0);
  check(is_saturated_plateau(jump_in, 4, -10.8, 0.5, 3), "a jump into the plateau passes on its last window", 0);
  check(!is_saturated_plateau(low, 3, -12, 0.5, 3), "a level below the floor fails", 0);
  check(!is_saturated_plateau(dip1, 4, -12, 0.5, 3), "a dip in the window fails", 0);
  check(!is_saturated_plateau(dip2, 5, -14, 0.5, 3), "a decay in the window fails", 0);
  check(!is_saturated_plateau(short_, 2, -12, 0.5, 3), "too short a history is never a plateau", 0);
}

int main(void) {
  case_papr_floor();
  case_plateau();
  case_is_growing();
  for (uint32_t k = 0; k < OMX_DECLARED_RATE_COUNT; k++) {
    const double rate = (double)OMX_DECLARED_RATES[k];
    case_ramp(rate);
    case_held(rate);
    case_decay(rate);
    case_pnpr_reach(rate);
    case_attack(rate);
    case_overtone(rate);
    case_pshr(rate);
    case_snapshot_order(rate);
    case_dropout(rate);
    case_exhaustion(rate);
  }
  printf("fbs_detect: %d checks over %d declared rates, %d failed\n", g_checks, OMX_DECLARED_RATE_COUNT, g_fail);
  return g_fail ? 1 : 0;
}
