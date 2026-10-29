// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* libFuzzer harness: the MIDI note map (omx_note_map.h), the one place omx-dsp reads bytes that
 * arrive from outside as they are: a stream of MIDI 1.0 channel messages through a leg's policy
 * (channel, key range, transpose, velocity curve), with the policy changed in the middle of it.
 *
 * Input:  [0..5]  the leg to start from: channel (0..16), key low, key high, transpose
 *                 (-48..48), curve, fixed velocity, each one byte read modulo its travel
 *         [6..]   records until the input ends: a u8 selector, then
 *                 0..199: a message, three raw bytes (status, data1, data2), ANY byte, a system
 *                         message or a data byte above 127 included;
 *                 200..229: omx_note_map_set_leg() with six words, each a raw int (an invalid leg
 *                         is refused and the old one stays);
 *                 230..255: omx_note_map_release_all() with a cap of 1..4 messages, until it
 *                         returns fewer than the cap.
 *
 * The harness keeps a model of what a downstream synth would be sounding: a count of note ons
 * delivered per (channel, output key), less the note offs delivered, cleared by the channel's all
 * notes off (controller 120 or 123, which the map passes through).
 *
 * Properties, each an abort():
 *   - a message is dropped, passed or rewritten: 0, 1 or 2 are written, each with a status in
 *     0x80..0xEF and data bytes in 0..127, a note on's velocity in 1..127;
 *   - NO NOTE STICKS AND NONE IS RELEASED THAT NEVER SOUNDED: every note off delivered is for a key
 *     the model has sounding, on the channel it was played on, and after releasing everything the
 *     model has nothing sounding and held_count is 0;
 *   - held_count is the population of the held table, always; an invalid leg is refused and leaves
 *     the policy as it was; a valid one is taken;
 *   - a note on outside the leg's channel, key range or transposed key range is never delivered,
 *     and the out_of_range counter only grows;
 *   - the velocity curves keep 1..127 for every note-on velocity, with linear the identity.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_note_map.h>

#include "fuzz.h"

static int popcount_held(const struct omx_note_map *m) {
  int n = 0;
  for (int c = 0; c < OMX_NOTE_CHANNELS; c++)
    for (int w = 0; w < OMX_NOTE_KEYS / 32; w++) n += __builtin_popcount(m->held[c][w]);
  return n;
}

struct model {
  int sounding[OMX_NOTE_CHANNELS][OMX_NOTE_KEYS];
};

static void deliver(struct model *md, const struct omx_note_map *m, const struct omx_note_msg *o, int n, int max,
                    struct omx_note_msg in) {
  FUZZ_CHECK(n >= 0 && n <= max, "a call made %d outputs, room for %d", n, max);
  for (int i = 0; i < n; i++) {
    const int kind = o[i].status & 0xF0, ch = o[i].status & 0x0F;
    FUZZ_CHECK(o[i].status >= 0x80u && o[i].status < 0xF0u && o[i].data1 <= 127u && o[i].data2 <= 127u,
               "output %d of %02x %02x %02x is %02x %02x %02x", i, in.status, in.data1, in.data2, o[i].status, o[i].data1,
               o[i].data2);
    if (kind == 0x90) {
      FUZZ_CHECK(o[i].data2 >= 1u, "a note on with velocity 0 (a note off in disguise) was delivered");
      md->sounding[ch][o[i].data1]++;
      /* a note on is only ever delivered inside the leg's own range, on a channel it takes */
      FUZZ_CHECK(omx_note_map_takes_channel(m, ch), "a note on on channel %d, which the leg does not take", ch + 1);
    } else if (kind == 0x80) {
      FUZZ_CHECK(md->sounding[ch][o[i].data1] > 0, "a note off for key %d on channel %d, which was not sounding",
                 o[i].data1, ch + 1);
      md->sounding[ch][o[i].data1]--;
    } else if (kind == 0xB0 && (o[i].data1 == 120u || o[i].data1 == 123u)) {
      memset(md->sounding[ch], 0, sizeof md->sounding[ch]);
    }
  }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  struct fuzz_in in = fuzz_in(data, size);
  fuzz_ledger_reset();

  struct omx_note_leg leg;
  leg.channel = fuzz_u8(&in) % (OMX_NOTE_CHANNELS + 1);
  leg.key_low = fuzz_u8(&in) % OMX_NOTE_KEYS;
  leg.key_high = leg.key_low + fuzz_u8(&in) % (OMX_NOTE_KEYS - leg.key_low);
  leg.transpose = (int)(fuzz_u8(&in) % (2 * OMX_NOTE_TRANSPOSE_MAX + 1)) - OMX_NOTE_TRANSPOSE_MAX;
  leg.velocity_curve = fuzz_u8(&in) % OMX_VELOCITY_CURVE_COUNT;
  leg.velocity_fixed = 1 + fuzz_u8(&in) % 127;
  FUZZ_CHECK(omx_note_leg_valid(&leg), "the harness built an invalid leg");

  static struct omx_note_map m;
  static struct model md;
  memset(&md, 0, sizeof md);
  omx_note_map_init(&m, &leg);
  uint32_t out_of_range = 0;

  while (in.n > 0) {
    const uint8_t sel = fuzz_u8(&in);
    if (sel < 200) {
      struct omx_note_msg msg, out[OMX_NOTE_MAP_OUT_MAX];
      msg.status = fuzz_u8(&in);
      msg.data1 = fuzz_u8(&in);
      msg.data2 = fuzz_u8(&in);
      const int was_held = msg.status >= 0x80u && msg.status < 0xF0u && msg.data1 < 128u
                               ? omx_note_map_is_held(&m, msg.status & 0x0F, msg.data1)
                               : 0;
      const int n = omx_note_map_event(&m, msg, out);
      deliver(&md, &m, out, n, OMX_NOTE_MAP_OUT_MAX, msg);
      if (n > 0 && (msg.status & 0xF0) == 0x90 && msg.data2 > 0u && msg.data1 < 128u) {
        const int to = msg.data1 + m.leg.transpose;
        FUZZ_CHECK(msg.data1 >= m.leg.key_low && msg.data1 <= m.leg.key_high && to >= 0 && to <= 127,
                   "note on %d was delivered outside the leg's key range %d..%d, transpose %d", msg.data1, m.leg.key_low,
                   m.leg.key_high, m.leg.transpose);
        FUZZ_CHECK(n == (was_held ? 2 : 1) && out[n - 1].data1 == (uint8_t)to, "a note on of %d made %d messages", msg.data1, n);
      }
      FUZZ_CHECK(m.out_of_range >= out_of_range, "out_of_range went down");
      out_of_range = m.out_of_range;
    } else if (sel < 230) {
      struct omx_note_leg l;
      l.channel = fuzz_int(&in);
      l.key_low = fuzz_int(&in);
      l.key_high = fuzz_int(&in);
      l.transpose = fuzz_int(&in);
      l.velocity_curve = fuzz_int(&in);
      l.velocity_fixed = fuzz_int(&in);
      if (fuzz_u8(&in) & 1) { /* a valid one, from bytes, so that most of these are taken */
        l.channel = fuzz_u8(&in) % (OMX_NOTE_CHANNELS + 1);
        l.key_low = fuzz_u8(&in) % OMX_NOTE_KEYS;
        l.key_high = l.key_low + fuzz_u8(&in) % (OMX_NOTE_KEYS - l.key_low);
        l.transpose = (int)(fuzz_u8(&in) % (2 * OMX_NOTE_TRANSPOSE_MAX + 1)) - OMX_NOTE_TRANSPOSE_MAX;
        l.velocity_curve = fuzz_u8(&in) % OMX_VELOCITY_CURVE_COUNT;
        l.velocity_fixed = 1 + fuzz_u8(&in) % 127;
      }
      const struct omx_note_leg before = m.leg;
      const int taken = omx_note_map_set_leg(&m, &l);
      FUZZ_CHECK(taken == omx_note_leg_valid(&l), "set_leg answered %d for a leg that is %svalid", taken,
                 omx_note_leg_valid(&l) ? "" : "in");
      FUZZ_CHECK(taken ? memcmp(&m.leg, &l, sizeof l) == 0 : memcmp(&m.leg, &before, sizeof before) == 0,
                 "set_leg left a policy that is not the one it answered for");
    } else {
      const int cap = 1 + (int)(sel % 4);
      struct omx_note_msg out[4];
      int n;
      do {
        n = omx_note_map_release_all(&m, out, cap);
        FUZZ_CHECK(n >= 0 && n <= cap, "release_all(%d) wrote %d", cap, n);
        const struct omx_note_msg none = {0, 0, 0};
        for (int i = 0; i < n; i++) FUZZ_CHECK((out[i].status & 0xF0) == 0x80, "release_all wrote a %02x", out[i].status);
        deliver(&md, &m, out, n, cap, none);
      } while (n == cap);
      FUZZ_CHECK(m.held_count == 0u, "release_all left %u held", (unsigned)m.held_count);
      for (int c = 0; c < OMX_NOTE_CHANNELS; c++)
        for (int k = 0; k < OMX_NOTE_KEYS; k++)
          FUZZ_CHECK(md.sounding[c][k] == 0, "after release_all, key %d on channel %d still sounds (%d)", k, c + 1,
                     md.sounding[c][k]);
    }
    FUZZ_CHECK((int)m.held_count == popcount_held(&m), "held_count %u is not the table's population %d", (unsigned)m.held_count,
               popcount_held(&m));
  }

  /* what is still held is exactly what still sounds, and releasing it silences the model */
  struct omx_note_msg out[OMX_NOTE_MAP_HELD_MAX];
  const int n = omx_note_map_release_all(&m, out, OMX_NOTE_MAP_HELD_MAX);
  const struct omx_note_msg none = {0, 0, 0};
  deliver(&md, &m, out, n, OMX_NOTE_MAP_HELD_MAX, none);
  FUZZ_CHECK(m.held_count == 0u, "the final release left %u held", (unsigned)m.held_count);
  for (int c = 0; c < OMX_NOTE_CHANNELS; c++)
    for (int k = 0; k < OMX_NOTE_KEYS; k++)
      FUZZ_CHECK(md.sounding[c][k] == 0, "a note is stuck: key %d on channel %d sounds (%d)", k, c + 1, md.sounding[c][k]);

  for (int v = 1; v <= 127; v++) {
    for (int curve = 0; curve < OMX_VELOCITY_CURVE_COUNT; curve++) {
      const int o = omx_velocity_curve_apply(curve, v, leg.velocity_fixed);
      FUZZ_CHECK(o >= 1 && o <= 127, "curve %d at velocity %d gave %d", curve, v, o);
    }
    FUZZ_CHECK(omx_velocity_curve_apply(OMX_VELOCITY_LINEAR, v, 1) == v, "the linear curve moved velocity %d", v);
  }
  fuzz_ledger_clean("note map");
  return 0;
}
