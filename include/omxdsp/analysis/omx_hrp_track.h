/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * HRP voice tracking: the same singer, frame after frame.
 *
 * Spec: openmixer docs/design/specs/2026-08-18-hrp-polyphonic-architecture.md §12, §13, §46.
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/hrp_track.h (omx-dsp#15, lean-engine
 * spec §1(a)). The engine keeps the spectrum rings, the control-thread tick and the strip's EQ
 * bands that apply a correction; the analysis itself lives here.
 *
 * ## What tracking is for
 *
 * `omx_hrp_detect` answers one frame at a time and has no memory: the voice it lists first this
 * frame need not be the one it listed first last frame. Everything downstream needs the opposite
 * — a per-note harmonic model, a baseline, a correction that persists — and all of that keys on
 * an identity that survives frames.
 *
 * ## Identity and stability are DIFFERENT questions, and §12/§13 ask them separately
 *
 * §12 wants identity preserved when a voice CHANGES NOTE — its own example moves voice B from C5
 * to D5 and still calls it voice B. §13 wants a new note NOT to cause an immediate correction.
 *
 * Both hold at once because they are about different fields. A moved voice keeps its `id` (the
 * player is the same player) and has its `age_frames` reset to zero (the NOTE is new, so every
 * harmonic measurement taken against the old one is void). `stable` is what the correction tier
 * reads, and it does not come back until the new note has held.
 *
 * ## Deliberately simple
 *
 * §12: "Do NOT initially introduce a Kalman filter or complex probabilistic tracker unless
 * benchmarks demonstrate that simpler tracking is insufficient." So this is greedy nearest-match
 * in cents, and the benchmark that would justify more does not exist yet.
 *
 * Allocation-free, no globals, caller owns the tracker. Never called from the RT callback.
 */
#ifndef OMX_HRP_TRACK_H
#define OMX_HRP_TRACK_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_pitch.h>

#define OMX_HRP_MAX_TRACKS OMX_HRP_MAX_VOICES

/** One voice followed across frames. */
typedef struct {
  uint32_t id; /* stable while the voice lives; never reused within a session */
  float f0_hz;
  float confidence;
  float salience; /* the voice's share of the pitched content — carried, never re-derived */
  float amplitude_db;
  int midi_note;
  float cents;
  /** Frames this voice has held its CURRENT note. Reset by a note change, not by a wobble. */
  uint32_t age_frames;
  /** Consecutive frames the detector has not found it. A voice is not dead the first time a
   *  frame misses it — a breath, a bow change or a quiet moment is not the end of a note. */
  uint32_t missing_frames;
  /** §13: has this note held long enough to be acted on? The correction tier reads THIS. */
  int stable;
  int active;
  /** Consecutive frames the detector reported this note ONE OCTAVE away (see the fold in
   *  {@link omx_hrp_track_update}), and which side (+1 above / −1 below). Crossing
   *  {@link OmxHrpTrackConfig.octave_hold_frames} migrates the track — a real leap. */
  uint32_t octave_frames;
  int octave_dir;
} OmxHrpTrack;

/** Every voice the desk is currently following on one channel. */
typedef struct {
  OmxHrpTrack tracks[OMX_HRP_MAX_TRACKS];
  uint32_t next_id;
} OmxHrpTracker;

typedef struct {
  /** How far a voice may move between frames and still be the SAME voice. A fifth by default:
   *  wide enough for a leap or a slide, narrow enough that two separate notes are two voices. */
  float match_cents;
  /** Movement within a match that still counts as the same NOTE — vibrato, drift, a bend that has
   *  not arrived. Beyond it the identity survives but the note is new and its age restarts. */
  float steady_cents;
  /** §13: frames a note must hold before it may be corrected. */
  uint32_t promote_frames;
  /** Consecutive misses before a voice is considered gone. */
  uint32_t retire_frames;
  /** Consecutive frames the detector must INSIST on the other octave before the track
   *  migrates there as a real leap. Below it, an octave-away report of the same pitch class
   *  is the detector flipping octaves on one note (low brass H2 dominance, §55) and the
   *  track holds its own octave. */
  uint32_t octave_hold_frames;
} OmxHrpTrackConfig;

static inline OmxHrpTrackConfig omx_hrp_track_config_default(void) {
  OmxHrpTrackConfig c;
  c.match_cents = 700.0f;  /* a fifth */
  c.steady_cents = 60.0f;  /* a little over a semitone's half-width */
  c.promote_frames = 3;
  /* 600 ms at the 10 Hz cadence: a masked beat under an orchestra swell is not the end of a
     note (measured live: 300-400 ms detection holes killed identities mid-phrase and every
     reappearance restarted from zero). A retired note that RETURNS is still merged for the
     operator by the log's own rejoin window. */
  c.retire_frames = 6;
  c.octave_hold_frames = 4;
  return c;
}

static inline void omx_hrp_tracker_init(OmxHrpTracker *t) {
  memset(t, 0, sizeof(*t));
  t->next_id = 1; /* 0 is "no track", so identities start at one */
}

/** Distance between two pitches, in cents. Musical distance, not hertz: a 10 Hz move is nothing
 *  at 2 kHz and a minor third at 40 Hz, and a tracker that used hertz would follow the top of the
 *  range and lose the bottom of it. */
static inline float omx_hrp_cents_between(float a_hz, float b_hz) {
  if (!(a_hz > 0.0f) || !(b_hz > 0.0f)) return 1e9f;
  return fabsf(1200.0f * log2f(a_hz / b_hz));
}

/** Advance the tracker by one detected frame.
 *
 * Greedy nearest-match: every detected voice takes the closest unclaimed track within
 * `match_cents`. Greedy is not optimal — a globally best assignment could differ — but with at
 * most four voices a fifth apart the cases where it differs are cases where the answer is
 * ambiguous anyway, and §12 explicitly asks for the simple thing first.
 *
 * @param tracker  caller-owned; carries the identities forward
 * @param frame    what the detector just found
 */
static inline void omx_hrp_track_update(OmxHrpTracker *tracker, const OmxHrpVoiceSet *frame,
                                        const OmxHrpTrackConfig *cfg) {
  int claimed[OMX_HRP_MAX_TRACKS];
  int matched[OMX_HRP_MAX_VOICES];
  memset(claimed, 0, sizeof(claimed));
  memset(matched, 0, sizeof(matched));

  /* 1. Existing voices keep their identities. */
  for (uint32_t v = 0; v < frame->voice_count && v < OMX_HRP_MAX_VOICES; v++) {
    const OmxHrpVoice *voice = &frame->voices[v];
    if (!voice->valid) continue;

    int best = -1;
    int best_fold = 0; /* signed OCTAVE COUNT when the winning match is a fold of the track's note */
    float best_cents = cfg->match_cents;
    for (uint32_t k = 0; k < OMX_HRP_MAX_TRACKS; k++) {
      if (!tracker->tracks[k].active || claimed[k]) continue;
      float d = omx_hrp_cents_between(voice->f0_hz, tracker->tracks[k].f0_hz);
      if (d <= best_cents) {
        best_cents = d;
        best = (int)k;
        best_fold = 0;
      }
      /* Octave equivalence, applied with hysteresis: the same PITCH CLASS any power of two
         away is standard theory's "same note", and on material whose upper harmonics dominate
         the fundamental the detector flips OCTAVES frame to frame — measured live on low
         brass, where one held F2 was reported as F2, F3 and F4 in alternation (its H1, H2 and
         H4 combs winning by turns under the orchestra). Only the tight `steady_cents` radius
         qualifies — the fold is for the same note seen through a different octave, never a
         doorway for unrelated notes near one. A claimed track cannot fold (checked above), so
         a SIMULTANEOUS octave pair — a piano's C3+C4, a guitar's octave double — stays two
         voices. */
      if (voice->f0_hz > 0.0f && tracker->tracks[k].f0_hz > 0.0f) {
        float ratio_log2 = log2f(voice->f0_hz / tracker->tracks[k].f0_hz);
        int octaves = (int)floorf(ratio_log2 + 0.5f);
        if (octaves != 0) {
          float folded = voice->f0_hz * powf(2.0f, (float)-octaves);
          float d_fold = omx_hrp_cents_between(folded, tracker->tracks[k].f0_hz);
          if (d_fold <= cfg->steady_cents && d_fold < best_cents) {
            best_cents = d_fold;
            best = (int)k;
            best_fold = octaves;
          }
        }
      }
    }
    if (best < 0) continue;

    OmxHrpTrack *t = &tracker->tracks[best];
    float adopted_f0 = voice->f0_hz;
    if (best_fold != 0) {
      int fold_dir = best_fold > 0 ? 1 : -1;
      if (t->octave_dir == fold_dir) {
        if (t->octave_frames < UINT32_MAX) t->octave_frames++;
      } else {
        t->octave_dir = fold_dir;
        t->octave_frames = 1;
      }
      if (t->octave_frames >= cfg->octave_hold_frames) {
        /* The detector INSISTS: a real octave leap. Identity survives (§12), the note is new
           and re-earns stability (§13), and the track adopts the new octave. */
        t->age_frames = 0;
        t->stable = 0;
        t->octave_frames = 0;
        t->octave_dir = 0;
      } else {
        /* A flip of the same note: hold the track's own octave — the fold expressed the
           detection in it (over however many octaves the detector jumped), so age keeps
           counting and a low note can still promote. */
        adopted_f0 = voice->f0_hz * powf(2.0f, (float)-best_fold);
      }
    } else {
      t->octave_frames = 0;
      t->octave_dir = 0;
    }
    /* A real note change: the identity survives (§12) and the note's history does not (§13).
       Every harmonic measurement taken against the old pitch describes a note that is over.
       `best_cents` is already the folded distance when a fold matched, so a flip of the same
       pitch class never trips this. */
    if (best_cents > cfg->steady_cents) {
      t->age_frames = 0;
      t->stable = 0;
    }
    t->f0_hz = adopted_f0;
    t->confidence = voice->confidence;
    t->salience = voice->salience;
    t->amplitude_db = voice->amplitude_db;
    /* From the ADOPTED pitch: a folded detection's own note name is the other octave's. */
    t->midi_note = omx_hrp_midi_note(adopted_f0, &t->cents);
    t->missing_frames = 0;
    if (t->age_frames < UINT32_MAX) t->age_frames++;
    if (t->age_frames >= cfg->promote_frames) t->stable = 1;
    claimed[best] = 1;
    matched[v] = 1;
  }

  /* 2. Voices nobody claimed are new arrivals. */
  for (uint32_t v = 0; v < frame->voice_count && v < OMX_HRP_MAX_VOICES; v++) {
    if (matched[v]) continue;
    const OmxHrpVoice *voice = &frame->voices[v];
    if (!voice->valid) continue;
    for (uint32_t k = 0; k < OMX_HRP_MAX_TRACKS; k++) {
      if (tracker->tracks[k].active) continue;
      OmxHrpTrack *t = &tracker->tracks[k];
      memset(t, 0, sizeof(*t));
      t->id = tracker->next_id++;
      t->f0_hz = voice->f0_hz;
      t->confidence = voice->confidence;
      t->salience = voice->salience;
      t->amplitude_db = voice->amplitude_db;
      t->midi_note = voice->midi_note;
      t->cents = voice->cents;
      t->age_frames = 1;
      t->active = 1;
      /* Never stable on arrival, whatever `promote_frames` says: §13's whole point is that a note
         that has just appeared has not yet earned a correction. */
      t->stable = cfg->promote_frames <= 1 ? 1 : 0;
      claimed[k] = 1;
      break;
    }
  }

  /* 3. Voices nothing matched are missing, and eventually gone. */
  for (uint32_t k = 0; k < OMX_HRP_MAX_TRACKS; k++) {
    OmxHrpTrack *t = &tracker->tracks[k];
    if (!t->active || claimed[k]) continue;
    t->missing_frames++;
    /* A missing voice is NOT stable: whatever was true of it, we are no longer watching it
       happen, and a correction must not go on being justified by a note that has stopped. */
    t->stable = 0;
    if (t->missing_frames >= cfg->retire_frames) memset(t, 0, sizeof(*t));
  }
}

/**
 * The tracker as a VOICE SET — the one projection both HRP tiers marshal through.
 *
 * Everything downstream of tracking (attribution, learning, the N-API result, the offline
 * report) works on an `OmxHrpVoiceSet`, so the tracker has to be read out as one. This is that
 * read-out, and there is exactly one of it: `hrp_analyze.c` (live, per analysis frame) and
 * `omx_hrp_render_window` (offline, per window) both call it. Two spellings of it is how the
 * live desk and a rendered file would come to disagree about what the SAME tracker contains —
 * a divergence no gate could see, because each tier would be self-consistent.
 *
 * ACTIVE TRACKS IN SLOT ORDER, packed: an inactive slot is skipped and leaves no hole, so
 * `out->voices[i]` is never an empty voice below `voice_count`. Slot order, not any ranking —
 * attribution wants the ensemble (§11) and the order it arrives in is not evidence.
 *
 * `by_slot` (optional, `OMX_HRP_MAX_VOICES` entries) answers "which track is output voice i",
 * which is the question every caller that LEARNS has to ask: the gates that decide whether a
 * measurement teaches (§13's `stable`, the note's identity) live on the track, not on the
 * projected voice. Pass NULL when only the set is wanted.
 *
 * The output is zeroed first, so the caller declares nothing and every unused slot — voice and
 * `by_slot` entry alike — is a zero rather than whatever the stack held.
 */
static inline void omx_hrp_project_voices(const OmxHrpTracker *tracker, OmxHrpVoiceSet *out,
                                          const OmxHrpTrack **by_slot) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (by_slot) memset(by_slot, 0, sizeof(*by_slot) * OMX_HRP_MAX_VOICES);
  if (!tracker) return;

  for (uint32_t k = 0; k < OMX_HRP_MAX_TRACKS && out->voice_count < OMX_HRP_MAX_VOICES; k++) {
    const OmxHrpTrack *t = &tracker->tracks[k];
    if (!t->active) continue;
    OmxHrpVoice *v = &out->voices[out->voice_count];
    v->f0_hz = t->f0_hz;
    v->confidence = t->confidence;
    v->salience = t->salience;
    v->amplitude_db = t->amplitude_db;
    v->midi_note = t->midi_note;
    v->cents = t->cents;
    v->valid = 1;
    if (by_slot) by_slot[out->voice_count] = t;
    out->voice_count++;
  }
}

/** The track carrying `id`, or NULL. Identity is the point of this module, so looking one up by
 *  identity — rather than by slot, which moves — is how every caller should reach it. */
static inline const OmxHrpTrack *omx_hrp_track_by_id(const OmxHrpTracker *tracker, uint32_t id) {
  if (id == 0) return NULL;
  for (uint32_t k = 0; k < OMX_HRP_MAX_TRACKS; k++) {
    if (tracker->tracks[k].active && tracker->tracks[k].id == id) return &tracker->tracks[k];
  }
  return NULL;
}

/** How many voices are being followed right now. */
static inline uint32_t omx_hrp_track_count(const OmxHrpTracker *tracker) {
  uint32_t n = 0;
  for (uint32_t k = 0; k < OMX_HRP_MAX_TRACKS; k++)
    if (tracker->tracks[k].active) n++;
  return n;
}

#endif /* OMX_HRP_TRACK_H */
