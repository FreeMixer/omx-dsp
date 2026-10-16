// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* ---- note_map: a note leg's channel, split, transpose and velocity curve ------------------- */

/* The curves' closed forms, written apart from the kernel: in doubles through libm, where the
 * kernel is integer-only. */
static int note_map_curve_oracle(int curve, int v, int fixed) {
  double r;
  switch (curve) {
  case OMX_VELOCITY_SOFT: r = floor(sqrt(127.0 * v) + 0.5); break;
  case OMX_VELOCITY_HARD: r = floor((double)v * v / 127.0 + 0.5); if (r < 1.0) r = 1.0; break;
  case OMX_VELOCITY_FIXED: r = fixed; break;
  default: r = v; break;
  }
  return (int)r;
}

static struct omx_note_msg note_msg(int status, int d1, int d2) {
  struct omx_note_msg m = {(uint8_t)status, (uint8_t)d1, (uint8_t)d2};
  return m;
}

static int note_msg_is(struct omx_note_msg m, int status, int d1, int d2) {
  return m.status == status && m.data1 == d1 && m.data2 == d2;
}

static void arm_note_map(void) {
  g_arm = "note_map";
  struct omx_note_map m;
  struct omx_note_msg out[OMX_NOTE_MAP_OUT_MAX];
  struct omx_note_leg leg = omx_note_leg_default();

  /* The names are the console's VELOCITY_CURVES, in order. */
  static const char *const names[] = {"linear", "soft", "hard", "fixed"};
  for (int c = 0; c < OMX_VELOCITY_CURVE_COUNT; c++)
    ok(strcmp(omx_velocity_curve_name(c), names[c]) == 0, "note_map: the curve's name", c, c);
  ok(omx_velocity_curve_name(OMX_VELOCITY_CURVE_COUNT) == NULL, "note_map: no name past the last curve", 0, 0);

  /* Done-when 1: every curve over all 127 velocities, through the kernel and through the map. */
  static const int fixeds[] = {1, 64, 100, 127};
  for (int c = 0; c < OMX_VELOCITY_CURVE_COUNT; c++)
    for (int f = 0; f < 4; f++) {
      int bad = 0, worst_v = 0, through_map = 0;
      leg = omx_note_leg_default();
      leg.velocity_curve = c;
      leg.velocity_fixed = fixeds[f];
      omx_note_map_init(&m, &leg);
      for (int v = 1; v <= 127; v++) {
        const int want = note_map_curve_oracle(c, v, fixeds[f]);
        if (omx_velocity_curve_apply(c, v, fixeds[f]) != want) bad++, worst_v = v;
        const int key = v % 128;
        if (omx_note_map_event(&m, note_msg(0x90, key, v), out) != 1 || !note_msg_is(out[0], 0x90, key, want))
          through_map++, worst_v = v;
        omx_note_map_event(&m, note_msg(0x80, key, 0), out);
      }
      ok(bad == 0, "note_map: the curve's closed form at every velocity", worst_v, c);
      ok(through_map == 0, "note_map: the note on carries the curve's velocity", worst_v, c);
    }
  ok(omx_velocity_curve_apply(OMX_VELOCITY_SOFT, 1, 100) == 11 &&
     omx_velocity_curve_apply(OMX_VELOCITY_HARD, 64, 100) == 32 &&
     omx_velocity_curve_apply(OMX_VELOCITY_HARD, 1, 100) == 1 &&
     omx_velocity_curve_apply(OMX_VELOCITY_SOFT, 127, 100) == 127 &&
     omx_velocity_curve_apply(OMX_VELOCITY_HARD, 127, 100) == 127,
     "note_map: hand-worked curve points (soft 1->11, hard 64->32, hard 1->1, both 127->127)", 0, 0);

  /* Done-when 1: a velocity-0 note on is a note off under every curve. */
  for (int c = 0; c < OMX_VELOCITY_CURVE_COUNT; c++) {
    leg = omx_note_leg_default();
    leg.velocity_curve = c;
    omx_note_map_init(&m, &leg);
    omx_note_map_event(&m, note_msg(0x93, 60, 100), out);
    const int n = omx_note_map_event(&m, note_msg(0x93, 60, 0), out);
    ok(n == 1 && note_msg_is(out[0], 0x83, 60, 64) && m.held_count == 0u,
       "note_map: velocity-0 note on releases the note", n, c);
    ok(omx_note_map_event(&m, note_msg(0x93, 61, 0), out) == 0,
       "note_map: velocity-0 note on of a key not held is dropped", 0, c);
  }

  /* Done-when 2: a split keyboard, bass up to 59, piano from 60. */
  struct omx_note_map bass, piano;
  leg = omx_note_leg_default();
  leg.key_high = 59;
  omx_note_map_init(&bass, &leg);
  leg = omx_note_leg_default();
  leg.key_low = 60;
  omx_note_map_init(&piano, &leg);
  for (int key = 0; key < 128; key++) {
    const int nb = omx_note_map_event(&bass, note_msg(0x90, key, 90), out);
    const int np = omx_note_map_event(&piano, note_msg(0x90, key, 90), out);
    if (key == 59 || key == 60)
      ok(nb == (key <= 59) && np == (key >= 60), "note_map: 59 to the bass leg, 60 to the piano leg", key, 1);
    if (nb + np != 1) {
      ok(0, "note_map: every key lands on exactly one leg of the split", key, nb + np);
      break;
    }
  }
  ok(bass.held_count == 60u && piano.held_count == 68u, "note_map: the split holds 60 + 68 keys",
     bass.held_count, piano.held_count);
  /* the split moved while held: the note off still ends the note */
  leg.key_low = 100;
  omx_note_map_set_leg(&piano, &leg);
  ok(omx_note_map_event(&piano, note_msg(0x80, 60, 0), out) == 1 && note_msg_is(out[0], 0x80, 60, 0),
     "note_map: a note off follows its note on after the split moved", 0, 0);

  /* Done-when 3: transpose changed while held. */
  leg = omx_note_leg_default();
  leg.transpose = 12;
  omx_note_map_init(&m, &leg);
  int n = omx_note_map_event(&m, note_msg(0x90, 60, 100), out);
  ok(n == 1 && note_msg_is(out[0], 0x90, 72, 100), "note_map: note on 60 at +12 sounds 72", out[0].data1, 72);
  leg.transpose = 0;
  ok(omx_note_map_set_leg(&m, &leg) == 1, "note_map: the transpose is taken while held", 0, 0);
  n = omx_note_map_event(&m, note_msg(0x80, 60, 33), out);
  ok(n == 1 && note_msg_is(out[0], 0x80, 72, 33), "note_map: the note off after transpose 0 is for 72", out[0].data1,
     72);
  ok(m.held_count == 0u, "note_map: nothing held after the note off", m.held_count, 0);
  /* a retrigger after the transpose moved releases the old output key first */
  leg.transpose = 5;
  omx_note_map_set_leg(&m, &leg);
  omx_note_map_event(&m, note_msg(0x90, 60, 100), out);
  leg.transpose = -5;
  omx_note_map_set_leg(&m, &leg);
  n = omx_note_map_event(&m, note_msg(0x90, 60, 90), out);
  ok(n == 2 && note_msg_is(out[0], 0x80, 65, 64) && note_msg_is(out[1], 0x90, 55, 90) && m.held_count == 1u,
     "note_map: a retrigger releases 65 then sounds 55", n, 2);
  n = omx_note_map_event(&m, note_msg(0xA0, 60, 77), out);
  ok(n == 1 && note_msg_is(out[0], 0xA0, 55, 77), "note_map: key pressure follows the held key", out[0].data1, 55);
  ok(omx_note_map_event(&m, note_msg(0xA0, 61, 77), out) == 0, "note_map: key pressure of a key not held is dropped",
     0, 0);

  /* Done-when 4: the channel filter. */
  leg = omx_note_leg_default();
  leg.channel = 2;
  omx_note_map_init(&m, &leg);
  ok(omx_note_map_event(&m, note_msg(0x90, 60, 100), out) == 0, "note_map: a leg on channel 2 drops channel 1", 0, 0);
  ok(omx_note_map_event(&m, note_msg(0x91, 60, 100), out) == 1 && note_msg_is(out[0], 0x91, 60, 100),
     "note_map: a leg on channel 2 passes channel 2", 0, 0);
  ok(omx_note_map_event(&m, note_msg(0xB0, 64, 127), out) == 0 &&
         omx_note_map_event(&m, note_msg(0xB1, 64, 127), out) == 1,
     "note_map: a controller follows the channel filter", 0, 0);
  {
    int passed = 0, kept = 0;
    for (int c = 0; c < 16; c++) {
      leg = omx_note_leg_default();
      omx_note_map_init(&m, &leg);
      passed += omx_note_map_event(&m, note_msg(0x90 | c, 64, 100), out) == 1 && out[0].status == (0x90 | c);
      leg.channel = c + 1;
      omx_note_map_init(&m, &leg);
      for (int o = 0; o < 16; o++) kept += omx_note_map_event(&m, note_msg(0x90 | o, 64, 100), out);
    }
    ok(passed == 16, "note_map: omni passes all 16 channels", passed, 16);
    ok(kept == 16, "note_map: a leg on channel c passes c alone", kept, 16);
  }

  /* Done-when 5: out of range, dropped and counted, never wrapped. */
  leg = omx_note_leg_default();
  leg.transpose = 48;
  omx_note_map_init(&m, &leg);
  ok(omx_note_map_event(&m, note_msg(0x90, 79, 100), out) == 1 && out[0].data1 == 127,
     "note_map: 79 + 48 = 127 sounds", out[0].data1, 127);
  ok(omx_note_map_event(&m, note_msg(0x90, 80, 100), out) == 0 && m.out_of_range == 1u,
     "note_map: 80 + 48 is dropped and counted", m.out_of_range, 1);
  ok(omx_note_map_event(&m, note_msg(0x80, 80, 0), out) == 0, "note_map: its note off is dropped", 0, 0);
  leg.transpose = -48;
  omx_note_map_set_leg(&m, &leg);
  ok(omx_note_map_event(&m, note_msg(0x90, 48, 100), out) == 1 && out[0].data1 == 0,
     "note_map: 48 - 48 = 0 sounds", out[0].data1, 0);
  ok(omx_note_map_event(&m, note_msg(0x90, 47, 100), out) == 0 && m.out_of_range == 2u,
     "note_map: 47 - 48 is dropped and counted", m.out_of_range, 2);
  {
    int wrapped = 0;
    for (int t = -48; t <= 48; t++) {
      leg.transpose = t;
      omx_note_map_init(&m, &leg);
      uint32_t want = 0u;
      for (int key = 0; key < 128; key++) {
        const int nn = omx_note_map_event(&m, note_msg(0x90, key, 100), out);
        if (key + t < 0 || key + t > 127) {
          want++;
          wrapped += nn != 0;
        } else {
          wrapped += nn != 1 || out[0].data1 != key + t;
        }
      }
      wrapped += m.out_of_range != want;
    }
    ok(wrapped == 0, "note_map: every transpose maps or drops every key, never wraps", wrapped, 0);
  }

  /* Release all: every held note, at its output key, in bounded slices. */
  leg = omx_note_leg_default();
  leg.transpose = 7;
  omx_note_map_init(&m, &leg);
  for (int c = 0; c < 16; c += 5) omx_note_map_event(&m, note_msg(0x90 | c, 40 + c, 100), out);
  omx_note_map_event(&m, note_msg(0x90, 41, 100), out);
  {
    struct omx_note_msg rel[OMX_NOTE_MAP_HELD_MAX];
    const int first = omx_note_map_release_all(&m, rel, 3);
    const int rest = omx_note_map_release_all(&m, rel + 3, OMX_NOTE_MAP_HELD_MAX - 3);
    ok(first == 3 && rest == 2 && m.held_count == 0u, "note_map: release-all lists 5 held notes in two slices",
       first + rest, 5);
    ok(note_msg_is(rel[0], 0x80, 47, 64) && note_msg_is(rel[1], 0x80, 48, 64) && note_msg_is(rel[2], 0x85, 52, 64) &&
           note_msg_is(rel[3], 0x8A, 57, 64) && note_msg_is(rel[4], 0x8F, 62, 64),
       "note_map: release-all names each output key", rel[0].data1, 47);
    ok(omx_note_map_release_all(&m, rel, OMX_NOTE_MAP_HELD_MAX) == 0, "note_map: a second release-all is empty", 0, 0);
    /* a full table */
    for (int c = 0; c < 16; c++)
      for (int key = 0; key < 121; key++) omx_note_map_event(&m, note_msg(0x90 | c, key, 1), out);
    ok(m.held_count == 16u * 121u, "note_map: 1936 keys held", m.held_count, 1936);
    ok(omx_note_map_release_all(&m, rel, OMX_NOTE_MAP_HELD_MAX) == 1936 && m.held_count == 0u,
       "note_map: release-all frees the full table", m.held_count, 0);
  }

  /* All notes off from the source forgets the channel; every other channel keeps its notes. */
  omx_note_map_init(&m, &leg);
  omx_note_map_event(&m, note_msg(0x90, 60, 100), out);
  omx_note_map_event(&m, note_msg(0x91, 60, 100), out);
  ok(omx_note_map_event(&m, note_msg(0xB0, 123, 0), out) == 1 && m.held_count == 1u &&
         omx_note_map_is_held(&m, 1, 60),
     "note_map: CC 123 forgets its own channel's notes", m.held_count, 1);

  /* What is not a channel message is dropped; an invalid policy is refused. */
  omx_note_map_init(&m, &leg);
  ok(omx_note_map_event(&m, note_msg(0xF8, 0, 0), out) == 0 && omx_note_map_event(&m, note_msg(0x40, 0, 0), out) == 0 &&
         omx_note_map_event(&m, note_msg(0x90, 200, 100), out) == 0 && m.held_count == 0u,
     "note_map: system, running-status and bad data bytes are dropped", 0, 0);
  struct omx_note_leg badl = leg;
  badl.transpose = 49;
  ok(omx_note_map_set_leg(&m, &badl) == 0 && m.leg.transpose == 7, "note_map: transpose 49 is refused", 49, 48);
  badl = leg;
  badl.channel = 17;
  ok(!omx_note_leg_valid(&badl), "note_map: channel 17 is invalid", 17, 16);
  badl = leg;
  badl.key_low = 61;
  badl.key_high = 60;
  ok(!omx_note_leg_valid(&badl), "note_map: an empty split is invalid", 61, 60);
  ok(omx_note_map_state_size() == sizeof(struct omx_note_map), "note_map: the state layout is exported",
     (double)omx_note_map_state_size(), (double)sizeof(struct omx_note_map));
  expect_clean();
}
