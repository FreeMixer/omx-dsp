// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_loudlevel_instance.h
 * @brief A leveler to a loudness target: a hand on a fader, reading BS.1770 loudness (omx_loudness.h).
 *
 * The input is measured K-weighted, over the chosen window; every 10 ms (a loudness sub-block) the gain
 * wants `target − L`, inside ±rangeDb, and walks there at speedDbPerS. Under gateLufs, or before the
 * window has filled, the gain holds. Between two ticks it moves by omx_ramp's line, so a block cut
 * changes no sample. The measure is ahead of the gain: there is no loop, and the settled level is the
 * target whatever the speed.
 *
 * Console use: a speech or a playback strip, MAIN or a recording or streaming feed held at a house
 * loudness (-23 LUFS broadcast, -16 a stream). LATENCY IS ZERO. A range of 0 and a bypass are the input,
 * bit for bit. Nothing is allocated; every word of state is in {@link OmxLoudlevelInstance}.
 */
#ifndef OMX_LOUDLEVEL_INSTANCE_H
#define OMX_LOUDLEVEL_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_instance_io.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_loudness.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_ramp.h>
#include <omxdsp/omx_units.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_LOUDLEVEL_INSTANCE_CHANNELS 2

typedef struct {
  float sr;
  struct omx_loudness meter;
  float target, range, step_db, gate; /**< the controls; step_db is the walk per tick */
  int win;
  float gain_db;  /**< the cursor, dB */
  float gain_lin; /**< the cursor, linear, where the running ramp stands */
  float ramp_end; /**< the gain the running tick ends on, linear */
  float ramp_step; /**< the running tick's step per sample */
  uint32_t tick;  /**< samples in a tick */
  uint32_t left;  /**< samples left in the running tick */
  uint32_t ticks; /**< ticks since the meter started, up to the longest window */
  float lufs;     /**< the loudnessLufs readout */
  int enabled;
  int ready;
} OmxLoudlevelInstance;

static inline void omx_loudlevel_instance_resolve(OmxLoudlevelInstance *s, int bypass, float target_lufs, float range_db,
                                                  float speed_db_per_s, float gate_lufs, int window);

/** The meter and the gain from the start: the engage edge.
 * @thread audio */
static inline void omx_loudlevel_instance_restart(OmxLoudlevelInstance *s) {
  omx_loudness_reset(&s->meter);
  s->gain_db = 0.0f;
  s->gain_lin = 1.0f;
  s->ramp_end = 1.0f;
  s->ramp_step = 0.0f;
  s->left = s->tick;
  s->ticks = 0u;
  s->lufs = (float)OMX_LOUDNESS_ABSOLUTE_GATE_LUFS;
}

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. It comes up at its declared defaults, a range of 0. Allocates nothing.
 * @thread control */
static inline int omx_loudlevel_instance_init(OmxLoudlevelInstance *s, float sr) {
  if (!omx_instance_bind(s, sizeof(*s), sr)) return 0;
  s->sr = sr;
  s->ready = 1;
  s->tick = (uint32_t)sr / OMX_LOUDNESS_SUBBLOCKS_PER_S;
  omx_loudness_init(&s->meter, sr, 2u, NULL);
  omx_loudlevel_instance_restart(s);
  omx_loudlevel_instance_resolve(s, 0, (float)OMX_LOUDLEVEL_TARGET_LUFS_RANGE_DEFAULT, (float)OMX_LOUDLEVEL_RANGE_DB_RANGE_DEFAULT,
                                 (float)OMX_LOUDLEVEL_SPEED_DB_PER_S_RANGE_DEFAULT, (float)OMX_LOUDLEVEL_GATE_LUFS_RANGE_DEFAULT,
                                 (int)OMX_LOUDLEVEL_WINDOWS_DEFAULT);
  return 1;
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the kernel's contract controls in their declared order: a travel clamped, a non-finite one
 * reading as its default; the window its index, one outside the set reading as its default. A face that
 * comes back into use starts over.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "loudlevel/instance-resolve"
static inline void omx_loudlevel_instance_resolve(OmxLoudlevelInstance *s, int bypass, float target_lufs, float range_db,
                                                  float speed_db_per_s, float gate_lufs, int window) {
  if (!s || !s->ready) return;
  const float tg = omx_clamp_or(target_lufs, (float)OMX_LOUDLEVEL_TARGET_LUFS_RANGE_MIN, (float)OMX_LOUDLEVEL_TARGET_LUFS_RANGE_MAX,
                                (float)OMX_LOUDLEVEL_TARGET_LUFS_RANGE_DEFAULT);
  const float rg = omx_clamp_or(range_db, (float)OMX_LOUDLEVEL_RANGE_DB_RANGE_MIN, (float)OMX_LOUDLEVEL_RANGE_DB_RANGE_MAX,
                                (float)OMX_LOUDLEVEL_RANGE_DB_RANGE_DEFAULT);
  const float sp = omx_clamp_or(speed_db_per_s, (float)OMX_LOUDLEVEL_SPEED_DB_PER_S_RANGE_MIN,
                                (float)OMX_LOUDLEVEL_SPEED_DB_PER_S_RANGE_MAX, (float)OMX_LOUDLEVEL_SPEED_DB_PER_S_RANGE_DEFAULT);
  const float gt = omx_clamp_or(gate_lufs, (float)OMX_LOUDLEVEL_GATE_LUFS_RANGE_MIN, (float)OMX_LOUDLEVEL_GATE_LUFS_RANGE_MAX,
                                (float)OMX_LOUDLEVEL_GATE_LUFS_RANGE_DEFAULT);
  const int wn = window >= 0 && window < (int)OMX_LOUDLEVEL_WINDOWS_COUNT ? window : (int)OMX_LOUDLEVEL_WINDOWS_DEFAULT;
  const int was = s->enabled;
  s->target = tg;
  s->range = rg;
  s->step_db = sp / (float)OMX_LOUDNESS_SUBBLOCKS_PER_S;
  s->gate = gt;
  s->win = wn;
  s->enabled = !bypass && rg > 0.0f;
  if (s->enabled && !was) omx_loudlevel_instance_restart(s);
  OMX_POST(s->step_db > 0.0f && s->range >= 0.0f, "controls-inside-the-travels");
}
#undef OMX_CONTRACT_STAGE

/** One tick's decision: read the window and open the next ramp. RT-safe: arithmetic and one log.
 * @thread audio */
static inline void omx_loudlevel_tick(OmxLoudlevelInstance *s) {
  const uint32_t full = s->win == (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY ? OMX_LOUDNESS_MOMENTARY : OMX_LOUDNESS_SHORT_TERM;
  if (s->ticks < full) s->ticks++;
  const double ms = s->win == (int)OMX_LOUDLEVEL_WINDOWS_MOMENTARY ? omx_loudness_momentary_ms(&s->meter) : omx_loudness_short_term_ms(&s->meter);
  s->lufs = ms > 0.0 ? (float)omx_loudness_lufs(ms) : (float)OMX_LOUDNESS_ABSOLUTE_GATE_LUFS;
  float want = s->gain_db;
  if (s->ticks >= full && s->lufs > s->gate) {
    want = s->target - s->lufs;
    want = want > s->range ? s->range : want < -s->range ? -s->range : want;
  }
  const float d = want - s->gain_db;
  s->gain_db = d > s->step_db ? s->gain_db + s->step_db : d < -s->step_db ? s->gain_db - s->step_db : want;
  s->gain_lin = s->ramp_end;
  s->ramp_end = omx_db_to_lin(s->gain_db);
  s->ramp_step = (s->ramp_end - s->gain_lin) / (float)s->tick;
  s->left = s->tick;
}

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: the meter over the input, the gain over both legs, in to out.
 * `in_*` and `out_*` may alias. Not ready, bypassed or a range of 0 is the identity and moves no state.
 * @thread audio
 */
#define OMX_CONTRACT_STAGE "loudlevel/instance-run"
static inline void omx_loudlevel_instance_run(OmxLoudlevelInstance *s, const float *in_l, const float *in_r, float *out_l,
                                              float *out_r, uint32_t n) {
  if (!omx_instance_copy_through(in_l, in_r, out_l, out_r, n)) return;
  if (!s || !s->ready || !s->enabled) return;
  OMX_PRE(omx_block_pair_finite(out_l, out_r, n), "finite-in");
  uint32_t o = 0u;
  while (o < n) {
    const uint32_t m = n - o < s->left ? n - o : s->left;
    const float *const legs[2] = {out_l + o, out_r + o};
    omx_loudness_process(&s->meter, legs, m);
    const uint32_t done = s->tick - s->left;
    for (uint32_t i = 0; i < m; i++) {
      const float g = s->gain_lin + s->ramp_step * (float)(done + i);
      out_l[o + i] *= g;
      out_r[o + i] *= g;
    }
    s->left -= m;
    o += m;
    if (s->left == 0u) omx_loudlevel_tick(s);
  }
}
#undef OMX_CONTRACT_STAGE

/** The loudnessLufs readout: what the detector reads now, before the gain.
 * @thread audio */
static inline float omx_loudlevel_instance_loudness_lufs(const OmxLoudlevelInstance *s) { return s ? s->lufs : 0.0f; }

/** The gainDb readout: the gain the running tick walks to, dB.
 * @thread audio */
static inline float omx_loudlevel_instance_gain_db(const OmxLoudlevelInstance *s) { return s ? s->gain_db : 0.0f; }

/** The frames of latency the instance introduces: none.
 * @thread audio */
static inline uint32_t omx_loudlevel_instance_latency(const OmxLoudlevelInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_LOUDLEVEL_INSTANCE_H */
