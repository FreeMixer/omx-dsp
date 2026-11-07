// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_glue_instance.h
 * @brief A glue compressor for a bus: the compressor's detents, its detector kept off the low end.
 *
 * It mints no DSP: one comp slot (fx/omx_comp_instance.h) resolved from fixed detents (GLUE_RATIO_VALUES,
 * GLUE_ATTACK_MS, GLUE_RELEASE_MS, the program release), RMS, stereo-linked, with GLUE_KNEE_DB of knee.
 * keyHpHz is the slot's key filter on its own legs. mixPct is the slot's parallel mix. A bypass is the
 * identity; a mix of 0 is the dry bus, bit for bit.
 *
 * detectorTopology (appended last, default feedforward, so an older session resolves to the glue it saved; the comp
 * slot's own DETECTOR_TOPOLOGY, which glue.json uses, not a copy) picks where the slot's detector listens: `feedforward` hears the bus; `feedback` runs omx_dyn.h's loop, the
 * detector hearing the slot's own pre-make-up output one sample late through ONE stage whose attack pole is
 * floored at (R - 1)/R (omx_dyn_fb_attack_pole), the VCA bus compressor's character (ssl-bus-compressor
 * §1-§4). The loop hears the full band: a key filter makes the slot keyed and a keyed slot detects
 * feed-forward, so in `feedback` the corner is not handed to the slot (keyHpHz is inert there, declared).
 *
 * Console use: on a group or MAIN, so the mix moves as one; the wider mix for parallel compression
 * on a drum group. LATENCY IS ZERO: the slot never takes the 4x control path. Nothing is allocated.
 */
#ifndef OMX_GLUE_INSTANCE_H
#define OMX_GLUE_INSTANCE_H

#include <stdint.h>

#include <omxdsp/fx/omx_comp_instance.h>
#include <omxdsp/fx/omx_instance_io.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omxdsp_definitions.h>

#define OMX_GLUE_INSTANCE_CHANNELS 2

static const float OMX_GLUE_INSTANCE_RATIOS[OMX_GLUE_RATIO_VALUES_COUNT] = OMX_GLUE_RATIO_VALUES_INIT;
static const float OMX_GLUE_INSTANCE_ATTACKS[OMX_GLUE_ATTACK_MS_COUNT] = OMX_GLUE_ATTACK_MS_INIT;
static const float OMX_GLUE_INSTANCE_RELEASES[OMX_GLUE_RELEASE_MS_COUNT] = OMX_GLUE_RELEASE_MS_INIT;
_Static_assert(OMX_GLUE_RATIO_VALUES_COUNT == OMX_GLUE_RATIOS_COUNT && OMX_GLUE_ATTACK_MS_COUNT == OMX_GLUE_ATTACKS_COUNT &&
                   OMX_GLUE_RELEASE_MS_COUNT + 1u == OMX_GLUE_RELEASES_COUNT,
               "one value per detent, the release's last being auto");

typedef struct {
  OmxCompInstance comp;
  int ready;
} OmxGlueInstance;

static inline void omx_glue_instance_resolve(OmxGlueInstance *s, int bypass, float threshold_db, int ratio, int attack,
                                             int release, float makeup_db, float key_hp_hz, float mix_pct,
                                             int detector_topology);

/** Bind an instance to its rate. Returns 1 when usable, 0 when not: the rate must be one of the oracle
 * set. It comes up at its declared defaults. Allocates nothing.
 * @thread control */
static inline int omx_glue_instance_init(OmxGlueInstance *s, float sr) {
  if (!omx_instance_bind(s, sizeof(*s), sr)) return 0;
  if (!omx_comp_instance_init(&s->comp, sr)) return 0;
  s->ready = 1;
  omx_glue_instance_resolve(s, 0, (float)OMX_GLUE_THRESHOLD_DB_RANGE_DEFAULT, (int)OMX_GLUE_RATIOS_DEFAULT,
                            (int)OMX_GLUE_ATTACKS_DEFAULT, (int)OMX_GLUE_RELEASES_DEFAULT,
                            (float)OMX_GLUE_MAKEUP_DB_RANGE_DEFAULT, (float)OMX_GLUE_KEY_HP_HZ_RANGE_DEFAULT,
                            (float)OMX_GLUE_MIX_PCT_RANGE_DEFAULT, (int)OMX_DETECTOR_TOPOLOGY_DEFAULT);
  return 1;
}

/**
 * Resolve the host's control values for one cycle. `bypass` non-zero makes run() the identity. The
 * arguments are the glue kernel's contract controls in their declared order: a travel clamped, a
 * non-finite one reading as its default; a choice its index, one outside its set reading as its default.
 * @thread audio
 */
static inline void omx_glue_instance_resolve(OmxGlueInstance *s, int bypass, float threshold_db, int ratio, int attack,
                                             int release, float makeup_db, float key_hp_hz, float mix_pct,
                                             int detector_topology) {
  if (!s || !s->ready) return;
  const float th = omx_clamp_or(threshold_db, (float)OMX_GLUE_THRESHOLD_DB_RANGE_MIN, (float)OMX_GLUE_THRESHOLD_DB_RANGE_MAX,
                                (float)OMX_GLUE_THRESHOLD_DB_RANGE_DEFAULT);
  const int ra = ratio >= 0 && ratio < (int)OMX_GLUE_RATIOS_COUNT ? ratio : (int)OMX_GLUE_RATIOS_DEFAULT;
  const int at = attack >= 0 && attack < (int)OMX_GLUE_ATTACKS_COUNT ? attack : (int)OMX_GLUE_ATTACKS_DEFAULT;
  const int rl = release >= 0 && release < (int)OMX_GLUE_RELEASES_COUNT ? release : (int)OMX_GLUE_RELEASES_DEFAULT;
  const float mk = omx_clamp_or(makeup_db, (float)OMX_GLUE_MAKEUP_DB_RANGE_MIN, (float)OMX_GLUE_MAKEUP_DB_RANGE_MAX,
                                (float)OMX_GLUE_MAKEUP_DB_RANGE_DEFAULT);
  const float hp = omx_clamp_or(key_hp_hz, (float)OMX_GLUE_KEY_HP_HZ_RANGE_MIN, (float)OMX_GLUE_KEY_HP_HZ_RANGE_MAX,
                                (float)OMX_GLUE_KEY_HP_HZ_RANGE_DEFAULT);
  const float mx = omx_clamp_or(mix_pct, (float)OMX_GLUE_MIX_PCT_RANGE_MIN, (float)OMX_GLUE_MIX_PCT_RANGE_MAX,
                                (float)OMX_GLUE_MIX_PCT_RANGE_DEFAULT);
  const int tp = detector_topology == (int)OMX_DETECTOR_TOPOLOGY_FEEDBACK ? (int)OMX_DETECTOR_TOPOLOGY_FEEDBACK
                                                                          : (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD;
  const int is_auto = rl == (int)OMX_GLUE_RELEASES_AUTO;
  /* The fixed release's value stays where it was last set while auto runs: the slot ignores it there. */
  const float rel_ms = is_auto ? OMX_GLUE_INSTANCE_RELEASES[0] : OMX_GLUE_INSTANCE_RELEASES[rl];
  /* The loop hears the full band: a corner would make the slot keyed, and a keyed slot detects feed-forward. */
  const float key_hp = tp == (int)OMX_DETECTOR_TOPOLOGY_FEEDBACK ? 0.0f : hp;
  omx_comp_instance_resolve(&s->comp, bypass, th, OMX_GLUE_INSTANCE_RATIOS[ra], (float)OMX_GLUE_KNEE_DB,
                            OMX_GLUE_INSTANCE_ATTACKS[at], rel_ms, mk, mx, (int)OMX_COMP_KINDS_COMP,
                            (int)OMX_DETECTOR_OVERSAMPLINGS_OFF, tp,
                            (int)OMX_COMP_KEY_SOURCES_SELF, key_hp, 0.0f, 0.0f, (int)OMX_COMP_KEY_LISTENS_OFF, 0.0f,
                            (int)OMX_COMP_DETECT_LINKS_LINKED, (int)OMX_COMP_DUCKS_OFF, (float)OMX_COMP_RANGE_DB_RANGE_DEFAULT,
                            is_auto ? (int)OMX_COMP_RELEASE_MODES_AUTO : (int)OMX_COMP_RELEASE_MODES_FIXED);
}

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE: the slot over both legs, in to out. `in_*` and `out_*` may alias.
 * Not ready or bypassed is the identity.
 * @thread audio
 */
static inline void omx_glue_instance_run(OmxGlueInstance *s, const float *in_l, const float *in_r, float *out_l,
                                         float *out_r, uint32_t n) {
  if (!s || !s->ready) {
    (void)omx_instance_copy_through(in_l, in_r, out_l, out_r, n);
    return;
  }
  omx_comp_instance_run_keyed(&s->comp, NULL, NULL, in_l, in_r, out_l, out_r, n);
}

/** The gainReductionDb readout: the deepest gain of the last block, dB, 0 or negative.
 * @thread audio */
static inline float omx_glue_instance_gain_reduction_db(const OmxGlueInstance *s) { return s ? s->comp.st.gr_min_db : 0.0f; }

/** The attack the detector runs, ms: the detent's, or in `feedback` the floor of omx_dyn_fb_attack_pole() at this
 * rate where the detent is faster (0.1 ms at 10:1 runs 0.198 ms at 48 kHz).
 * @thread audio */
static inline float omx_glue_instance_effective_attack_ms(const OmxGlueInstance *s) {
  return s ? omx_comp_instance_effective_attack_ms(&s->comp) : 0.0f;
}

/** The frames of latency the instance introduces: none, in either topology (the slot never takes the 4x path).
 * @thread audio */
static inline uint32_t omx_glue_instance_latency(const OmxGlueInstance *s) {
  (void)s;
  return 0u;
}

#endif /* OMX_GLUE_INSTANCE_H */
