// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * Standalone unit test for HRP voice tracking (hrp_track.h):
 *   cc -Wall -Wextra -O2 -o /tmp/hrp_track_test src/hrp_track.test.c -lm && /tmp/hrp_track_test
 * (also driven from `pnpm test` via the test:dsp script).
 *
 * The tracker is fed voice sets built by hand rather than by the detector. That is deliberate
 * isolation: a tracking bug and a detection bug look identical through the detector, and these
 * tests must fail for tracking reasons only.
 *
 * Covers §39's list — A4→A4, A4→B♭4, A4+C5→A4+D5, disappearance, appearance, simultaneous
 * change — plus the §13 rule the correction tier depends on: a new note is not immediately
 * actionable.
 *
 * And, since the fold of audit D4, {@link omx_hrp_project_voices}: the ONE read-out from the
 * tracker to a voice set, which every tier downstream of tracking marshals through. Its live
 * caller (`hrp_analyze.c`) is the translation unit the review baseline's M3 names as untested,
 * and this is where that projection is now oracled; `hrp_render.test.c` separately proves the
 * offline and live chains project the same tracker to the same bytes.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_track.h>

static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what) {
  g_checks++;
  if (!cond) {
    g_fail++;
    fprintf(stderr, "FAIL: %s\n", what);
  }
}

static const float A4 = 440.0f;
static const float Bb4 = 466.164f;
static const float C5 = 523.251f;
static const float D5 = 587.330f;
static const float E5 = 659.255f;

/** A frame carrying the given pitches, all confident and valid. */
static OmxHrpVoiceSet frame_of(const float *hz, uint32_t n) {
  OmxHrpVoiceSet v;
  memset(&v, 0, sizeof(v));
  for (uint32_t i = 0; i < n && i < OMX_HRP_MAX_VOICES; i++) {
    v.voices[i].f0_hz = hz[i];
    v.voices[i].confidence = 0.8f;
    v.voices[i].amplitude_db = -12.0f;
    v.voices[i].midi_note = omx_hrp_midi_note(hz[i], &v.voices[i].cents);
    v.voices[i].valid = 1;
    v.voice_count++;
  }
  return v;
}

static void feed(OmxHrpTracker *t, const OmxHrpTrackConfig *c, const float *hz, uint32_t n,
                 uint32_t times) {
  for (uint32_t i = 0; i < times; i++) {
    OmxHrpVoiceSet f = frame_of(hz, n);
    omx_hrp_track_update(t, &f, c);
  }
}

/** The track nearest `hz`, or NULL — how a test asks "what happened to that voice". */
static const OmxHrpTrack *nearest(const OmxHrpTracker *t, float hz) {
  const OmxHrpTrack *best = NULL;
  float bd = 1e9f;
  for (uint32_t k = 0; k < OMX_HRP_MAX_TRACKS; k++) {
    if (!t->tracks[k].active) continue;
    float d = omx_hrp_cents_between(t->tracks[k].f0_hz, hz);
    if (d < bd) {
      bd = d;
      best = &t->tracks[k];
    }
  }
  return best;
}

/* ---- octave continuity (measured live: low brass H2 dominance flips the detector) -------- */

static void test_octave_flap_is_one_note(void) {
  /* Low-register reality: on a note whose H2 dominates its fundamental, the detector reports
     F2 one frame and F3 the next — the SAME note, seen through §55's own ambiguity. Identity,
     age and the held octave must all survive the flap, or the note never promotes and the
     panel reads "undetected" exactly where the operator plays low (measured, trombone 2:52). */
  OmxHrpTracker t;
  omx_hrp_tracker_init(&t);
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  const float f2 = 87.31f, f3 = 174.61f;

  feed(&t, &c, &f2, 1, 2);
  const OmxHrpTrack *tr = nearest(&t, f2);
  uint32_t id = tr->id;
  for (uint32_t i = 0; i < 6; i++) {
    const float hz = (i % 2 == 0) ? f3 : f2; /* alternating octaves, one per frame */
    feed(&t, &c, &hz, 1, 1);
  }
  check(omx_hrp_track_count(&t) == 1, "octave flap: still ONE voice, not a rival per flip");
  tr = nearest(&t, f2);
  check(tr != NULL && tr->id == id, "octave flap: the identity survives");
  check(tr->stable == 1, "octave flap: the note PROMOTES — flips are not note changes");
  check(omx_hrp_cents_between(tr->f0_hz, f2) < 100.0f,
        "octave flap: the track holds its own octave, not the flip's");
}

static void test_two_octave_flap_is_still_one_note(void) {
  /* The measured live shape: one held F2 reported as F2, F3 and F4 in alternation — its H1,
     H2 and H4 combs winning by turns under the orchestra. Octave equivalence spans ANY power
     of two, so the whole stack is one note holding its own octave. */
  OmxHrpTracker t;
  omx_hrp_tracker_init(&t);
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  const float f2 = 87.31f, f3 = 174.61f, f4 = 349.23f;

  feed(&t, &c, &f2, 1, 2);
  const OmxHrpTrack *tr = nearest(&t, f2);
  uint32_t id = tr->id;
  const float cycle[6] = {f3, f2, f4, f2, f3, f4};
  for (uint32_t i = 0; i < 6; i++) feed(&t, &c, &cycle[i], 1, 1);
  check(omx_hrp_track_count(&t) == 1, "two-octave flap: still ONE voice");
  tr = nearest(&t, f2);
  check(tr != NULL && tr->id == id, "two-octave flap: the identity survives");
  check(tr->stable == 1, "two-octave flap: the note promotes");
  check(omx_hrp_cents_between(tr->f0_hz, f2) < 100.0f, "two-octave flap: the held octave wins");
}

static void test_persistent_octave_is_a_real_leap(void) {
  /* A player really can jump an octave. When the detector INSISTS on the new octave, the
     track migrates: same identity (§12 — the player is the same player), but the note is new
     and its history is void (§13 — age restarts, stable drops until re-earned). */
  OmxHrpTracker t;
  omx_hrp_tracker_init(&t);
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  const float f2 = 87.31f, f3 = 174.61f;

  feed(&t, &c, &f2, 1, 5);
  const OmxHrpTrack *tr = nearest(&t, f2);
  uint32_t id = tr->id;
  check(tr->stable == 1, "leap setup: the low note promoted");

  feed(&t, &c, &f3, 1, c.octave_hold_frames);
  tr = nearest(&t, f3);
  check(tr != NULL && tr->id == id, "octave leap: the identity survives the migration");
  check(omx_hrp_cents_between(tr->f0_hz, f3) < 100.0f, "octave leap: the track adopts the new octave");
  check(tr->stable == 0, "octave leap: a REAL new note re-earns stability (§13)");

  feed(&t, &c, &f3, 1, c.promote_frames);
  tr = nearest(&t, f3);
  check(tr->stable == 1, "octave leap: the new octave promotes after holding");
}

static void test_simultaneous_octaves_are_two_voices(void) {
  /* Polyphonic reality (operator ruling: every instrument, pianos and guitars included): a
     piano's C3+C4, a guitar's octave double — two REAL voices an octave apart, in the SAME
     frame. Folding must never merge them: continuity applies to alternation, not to company. */
  OmxHrpTracker t;
  omx_hrp_tracker_init(&t);
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  const float pair[2] = {130.81f, 261.63f}; /* C3 + C4 */

  feed(&t, &c, pair, 2, 5);
  check(omx_hrp_track_count(&t) == 2, "octave pair: TWO voices, never folded into one");
  const OmxHrpTrack *lo = nearest(&t, pair[0]);
  const OmxHrpTrack *hi = nearest(&t, pair[1]);
  check(lo != NULL && hi != NULL && lo->id != hi->id, "octave pair: distinct identities");
  check(lo->stable == 1 && hi->stable == 1, "octave pair: both promote");
}

/* ---- §39 ---------------------------------------------------------------------------------- */

static void test_held_note_keeps_identity_and_becomes_stable(void) {
  OmxHrpTracker t;
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  omx_hrp_tracker_init(&t);
  float a[] = {A4};

  feed(&t, &c, a, 1, 1);
  const OmxHrpTrack *first = nearest(&t, A4);
  check(first != NULL, "A4 is tracked on its first frame");
  check(first && !first->stable, "a note is NOT actionable on its first frame (§13)");
  uint32_t id = first ? first->id : 0;

  feed(&t, &c, a, 1, 3);
  const OmxHrpTrack *later = nearest(&t, A4);
  check(later && later->id == id, "A4 -> A4 keeps its identity");
  check(later && later->stable, "a note that has held becomes actionable");
  check(omx_hrp_track_count(&t) == 1, "one held note is one voice, not a new one per frame");
}

static void test_note_change_keeps_identity_but_restarts_the_note(void) {
  /* §12 wants the identity preserved; §13 wants the correction to wait. Both, on one event. */
  OmxHrpTracker t;
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  omx_hrp_tracker_init(&t);
  float a[] = {A4}, b[] = {Bb4};

  feed(&t, &c, a, 1, 4);
  const OmxHrpTrack *before = nearest(&t, A4);
  check(before && before->stable, "A4 is stable before the change");
  uint32_t id = before ? before->id : 0;

  feed(&t, &c, b, 1, 1);
  const OmxHrpTrack *after = nearest(&t, Bb4);
  check(after && after->id == id, "A4 -> B♭4 KEEPS the voice identity (§12)");
  check(after && !after->stable, "…and the new note is not immediately actionable (§13)");
  check(after && after->age_frames == 1, "the new note's history starts over, not the voice's");
  check(after && after->midi_note == 70, "the tracked note follows the pitch to B♭4");
  check(omx_hrp_track_count(&t) == 1, "a note change is not a second voice");
}

static void test_vibrato_is_not_a_note_change(void) {
  /* A wobble inside `steady_cents` must not keep resetting stability, or a sung note is never
     actionable at all — every real voice moves a little. */
  OmxHrpTracker t;
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  omx_hrp_tracker_init(&t);
  float centre[] = {A4}, sharp[] = {A4 * 1.015f}, flat[] = {A4 * 0.985f}; /* ±~26 cents */

  feed(&t, &c, centre, 1, 3);
  feed(&t, &c, sharp, 1, 1);
  feed(&t, &c, flat, 1, 1);
  const OmxHrpTrack *v = nearest(&t, A4);
  check(v && v->stable, "vibrato inside the steady window does not un-promote a held note");
  check(omx_hrp_track_count(&t) == 1, "vibrato is one voice");
}

static void test_one_voice_moves_and_the_other_holds(void) {
  /* §12's own example: A stays, B moves C5 -> D5. */
  OmxHrpTracker t;
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  omx_hrp_tracker_init(&t);
  float pair[] = {A4, C5}, moved[] = {A4, D5};

  feed(&t, &c, pair, 2, 4);
  const OmxHrpTrack *a0 = nearest(&t, A4);
  const OmxHrpTrack *c0 = nearest(&t, C5);
  check(a0 && c0 && a0->id != c0->id, "two notes are two identities");
  uint32_t a_id = a0 ? a0->id : 0, c_id = c0 ? c0->id : 0;
  check(a0 && a0->stable && c0 && c0->stable, "both are stable after holding");

  feed(&t, &c, moved, 2, 1);
  const OmxHrpTrack *a1 = nearest(&t, A4);
  const OmxHrpTrack *d1 = nearest(&t, D5);
  check(a1 && a1->id == a_id, "the voice that did NOT move keeps its identity");
  check(a1 && a1->stable, "…and stays actionable — its note never changed");
  check(d1 && d1->id == c_id, "the voice that moved keeps its identity too (§12)");
  check(d1 && !d1->stable, "…but its new note must earn stability again (§13)");
  check(omx_hrp_track_count(&t) == 2, "one mover and one holder are still two voices");
}

static void test_simultaneous_change(void) {
  OmxHrpTracker t;
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  omx_hrp_tracker_init(&t);
  float pair[] = {A4, C5}, both[] = {Bb4, D5};

  feed(&t, &c, pair, 2, 4);
  feed(&t, &c, both, 2, 1);
  check(omx_hrp_track_count(&t) == 2, "two voices moving at once are still two voices");
  const OmxHrpTrack *x = nearest(&t, Bb4);
  const OmxHrpTrack *y = nearest(&t, D5);
  check(x && y && !x->stable && !y->stable, "neither new note is actionable yet");
}

static void test_disappearance_is_not_instant(void) {
  OmxHrpTracker t;
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  omx_hrp_tracker_init(&t);
  float a[] = {A4};
  OmxHrpVoiceSet empty;
  memset(&empty, 0, sizeof(empty));

  feed(&t, &c, a, 1, 4);
  omx_hrp_track_update(&t, &empty, &c);
  check(omx_hrp_track_count(&t) == 1,
        "one silent frame does not end a note — a breath or a bow change is not the end");
  const OmxHrpTrack *dying = nearest(&t, A4);
  check(dying && !dying->stable,
        "…but a voice nobody can hear is NOT actionable while it is missing");

  for (uint32_t i = 1; i < c.retire_frames; i++) omx_hrp_track_update(&t, &empty, &c);
  check(omx_hrp_track_count(&t) == 0, "a voice gone for retire_frames is retired");
}

static void test_appearance_gets_a_fresh_identity(void) {
  OmxHrpTracker t;
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  omx_hrp_tracker_init(&t);
  float one[] = {A4}, two[] = {A4, E5};

  feed(&t, &c, one, 1, 4);
  uint32_t a_id = nearest(&t, A4)->id;
  feed(&t, &c, two, 2, 1);
  check(omx_hrp_track_count(&t) == 2, "an arriving note is a second voice");
  const OmxHrpTrack *e = nearest(&t, E5);
  check(e && e->id != a_id, "the arrival gets its OWN identity");
  check(e && !e->stable, "an arrival is not actionable on arrival");
  check(nearest(&t, A4)->id == a_id, "the note already sounding is undisturbed by the arrival");
}

static void test_identities_are_never_reused(void) {
  /* A retired voice's id must not come back on a later note: anything keyed on it — a learned
     model, a planted correction — would silently attach to the wrong note. */
  OmxHrpTracker t;
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  omx_hrp_tracker_init(&t);
  float a[] = {A4}, e[] = {E5};
  OmxHrpVoiceSet empty;
  memset(&empty, 0, sizeof(empty));

  feed(&t, &c, a, 1, 2);
  uint32_t first = nearest(&t, A4)->id;
  for (uint32_t i = 0; i < c.retire_frames; i++) omx_hrp_track_update(&t, &empty, &c);
  check(omx_hrp_track_count(&t) == 0, "the first voice is gone");
  feed(&t, &c, e, 1, 1);
  check(nearest(&t, E5)->id != first, "a new voice never inherits a retired identity");
}

static void test_lookup_by_identity(void) {
  OmxHrpTracker t;
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  omx_hrp_tracker_init(&t);
  float a[] = {A4};
  feed(&t, &c, a, 1, 2);
  uint32_t id = nearest(&t, A4)->id;
  check(omx_hrp_track_by_id(&t, id) != NULL, "a live voice is reachable by its identity");
  check(omx_hrp_track_by_id(&t, 0) == NULL, "0 is not an identity");
  check(omx_hrp_track_by_id(&t, id + 999) == NULL, "an unknown identity resolves to nothing");
}

static void test_capacity(void) {
  OmxHrpTracker t;
  OmxHrpTrackConfig c = omx_hrp_track_config_default();
  omx_hrp_tracker_init(&t);
  /* AS MANY NOTES AS THE TRACKER HOLDS, built from the macro rather than listed: the ceiling
     moved 4 -> 8 with the polyphony sizing (2026-09-01), and a hand-listed chord asserts "the
     tracker is full" while leaving half of it empty — which is exactly what a fixed list of four
     did the moment the budget grew. Whole tones up from A4, so every voice is its own pitch and
     none is another's octave. */
  float notes[OMX_HRP_MAX_TRACKS];
  for (uint32_t i = 0; i < OMX_HRP_MAX_TRACKS; i++)
    notes[i] = A4 * powf(2.0f, (float)(2 * i) / 12.0f);
  feed(&t, &c, notes, OMX_HRP_MAX_TRACKS, 2);
  check(omx_hrp_track_count(&t) == OMX_HRP_MAX_TRACKS,
        "MAX_TRACKS simultaneous voices fill the tracker");
  check(omx_hrp_track_count(&t) <= OMX_HRP_MAX_TRACKS, "and never exceed it");
}

/* ---- the projection: the read-out both HRP tiers marshal through ------------------------- */

/* A track with a DISTINCT value in every field, planted straight into the slot rather than
   tracked in. The projection's claim is about what it copies field for field, and a swapped or
   dropped assignment must not be maskable by the detector or the tracker — the same isolation
   the rest of this file is built on. */
static void plant_track(OmxHrpTrack *t, uint32_t id, float hz) {
  memset(t, 0, sizeof(*t));
  t->id = id;
  t->f0_hz = hz;
  t->confidence = 0.11f;
  t->salience = 0.22f;
  t->amplitude_db = -33.5f;
  t->midi_note = omx_hrp_midi_note(hz, &t->cents);
  t->age_frames = 12;
  t->stable = 1;
  t->active = 1;
}

static void test_projection_carries_every_field(void) {
  OmxHrpTracker t;
  omx_hrp_tracker_init(&t);
  plant_track(&t.tracks[0], 7, A4);

  OmxHrpVoiceSet out;
  const OmxHrpTrack *by_slot[OMX_HRP_MAX_VOICES];
  omx_hrp_project_voices(&t, &out, by_slot);

  check(out.voice_count == 1u, "one active track projects one voice");
  check(out.voices[0].f0_hz == t.tracks[0].f0_hz, "the projection carries f0");
  check(out.voices[0].confidence == t.tracks[0].confidence, "the projection carries confidence");
  /* Carried, never re-derived (hrp_pitch.h's own note on the field): the tracker's share of the
     pitched content is a fact about the ensemble the detector saw, not about one voice. */
  check(out.voices[0].salience == t.tracks[0].salience, "the projection carries salience");
  check(out.voices[0].amplitude_db == t.tracks[0].amplitude_db,
        "the projection carries the amplitude");
  check(out.voices[0].midi_note == t.tracks[0].midi_note, "the projection carries the note");
  check(out.voices[0].cents == t.tracks[0].cents, "the projection carries the cents offset");
  check(out.voices[0].valid == 1, "a projected voice is valid — it is a voice the desk is on");

  check(by_slot[0] == &t.tracks[0], "by_slot says which TRACK the voice came from");
  check(by_slot[1] == NULL, "and every entry past the count is NULL");
  check(out.voices[1].valid == 0 && out.voices[1].f0_hz == 0.0f,
        "so is every voice past the count — the output is zeroed, not left as the stack was");
}

static void test_projection_packs_the_active_tracks_in_slot_order(void) {
  OmxHrpTracker t;
  omx_hrp_tracker_init(&t);
  plant_track(&t.tracks[0], 1, A4);
  plant_track(&t.tracks[2], 2, C5);
  plant_track(&t.tracks[5], 3, E5);
  /* An INACTIVE slot carrying a stale pitch: `active` is the gate, and a voice the desk has
     retired must not reappear in the set because its numbers are still sitting in the slot. */
  plant_track(&t.tracks[1], 4, D5);
  t.tracks[1].active = 0;

  OmxHrpVoiceSet out;
  const OmxHrpTrack *by_slot[OMX_HRP_MAX_VOICES];
  omx_hrp_project_voices(&t, &out, by_slot);

  check(out.voice_count == 3u, "three active tracks project three voices");
  check(out.voices[0].f0_hz == A4 && out.voices[1].f0_hz == C5 && out.voices[2].f0_hz == E5,
        "the active tracks arrive PACKED, in slot order, with no hole for the gap");
  check(by_slot[0] == &t.tracks[0] && by_slot[1] == &t.tracks[2] && by_slot[2] == &t.tracks[5],
        "and by_slot follows the packing, not the slots");
  for (uint32_t i = 0; i < out.voice_count; i++)
    check(out.voices[i].f0_hz != D5, "the inactive slot's stale pitch is not in the set");
}

static void test_projection_fills_at_most_the_voice_set(void) {
  OmxHrpTracker t;
  omx_hrp_tracker_init(&t);
  /* Every slot active. MAX_TRACKS == MAX_VOICES today, so the set exactly fills; the bound is
     asserted from the macro rather than a literal so it still holds the day they differ. */
  for (uint32_t k = 0; k < OMX_HRP_MAX_TRACKS; k++)
    plant_track(&t.tracks[k], k + 1u, A4 * powf(2.0f, (float)(2 * k) / 12.0f));

  OmxHrpVoiceSet out;
  const OmxHrpTrack *by_slot[OMX_HRP_MAX_VOICES];
  omx_hrp_project_voices(&t, &out, by_slot);

  check(out.voice_count <= OMX_HRP_MAX_VOICES, "the projection never overruns the voice set");
  check(out.voice_count == (OMX_HRP_MAX_TRACKS < OMX_HRP_MAX_VOICES ? OMX_HRP_MAX_TRACKS
                                                                    : OMX_HRP_MAX_VOICES),
        "a full tracker projects as many voices as the set can hold");
}

static void test_projection_answers_nothing_when_there_is_nothing(void) {
  OmxHrpTracker t;
  omx_hrp_tracker_init(&t);
  OmxHrpVoiceSet out;
  const OmxHrpTrack *by_slot[OMX_HRP_MAX_VOICES];

  omx_hrp_project_voices(&t, &out, by_slot);
  check(out.voice_count == 0u, "a fresh tracker is an empty set, not an error");

  /* A caller that wants only the set passes NULL, and gets the same set. */
  OmxHrpVoiceSet with_slots, without_slots;
  plant_track(&t.tracks[3], 9, C5);
  omx_hrp_project_voices(&t, &with_slots, by_slot);
  omx_hrp_project_voices(&t, &without_slots, NULL);
  check(memcmp(&with_slots, &without_slots, sizeof(with_slots)) == 0,
        "the voice set does not depend on whether by_slot was asked for");

  /* Absence is a fact, not a crash — the rule every entry in this tier follows. */
  omx_hrp_project_voices(NULL, &out, by_slot);
  check(out.voice_count == 0u, "no tracker projects an empty set");
  omx_hrp_project_voices(&t, NULL, by_slot);
  check(1, "no output is a no-op");
}

int main(void) {
  test_octave_flap_is_one_note();
  test_two_octave_flap_is_still_one_note();
  test_persistent_octave_is_a_real_leap();
  test_simultaneous_octaves_are_two_voices();
  test_held_note_keeps_identity_and_becomes_stable();
  test_note_change_keeps_identity_but_restarts_the_note();
  test_vibrato_is_not_a_note_change();
  test_one_voice_moves_and_the_other_holds();
  test_simultaneous_change();
  test_disappearance_is_not_instant();
  test_appearance_gets_a_fresh_identity();
  test_identities_are_never_reused();
  test_lookup_by_identity();
  test_capacity();
  test_projection_carries_every_field();
  test_projection_packs_the_active_tracks_in_slot_order();
  test_projection_fills_at_most_the_voice_set();
  test_projection_answers_nothing_when_there_is_nothing();

  printf("hrp_track: %d checks, %d failures\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
