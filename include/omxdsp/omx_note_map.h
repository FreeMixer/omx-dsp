// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_note_map.h
 * @brief The note map: one MIDI 1.0 channel message through a note leg's policy (a channel
 *        filter, a key split, a transpose and a velocity curve), with a held-note table so a note
 *        off always ends the note its note on started.
 *
 * A note source feeds an instrument through a LEG; the leg carries the policy of that pairing. The
 * map takes one channel message and answers with zero, one or two messages to deliver:
 *
 *   - Note on (`9n kk vv`, `vv` > 0): dropped unless the leg takes channel `n` and `keyLow <= kk
 *     <= keyHigh`. The output key is `kk + transpose`; one outside 0..127 is DROPPED AND COUNTED
 *     (`out_of_range`), never wrapped. The velocity goes through the curve. The input key is marked
 *     held, with the output key it sounded. A note on for a key already held first releases the
 *     output key it holds (two messages), so a retrigger after a transpose change leaves no note
 *     sounding.
 *   - Note off (`8n kk vv`, or `9n kk 00`, which every curve treats as a note off): delivered only
 *     when input key `kk` on channel `n` is held, to the OUTPUT KEY IT WAS HELD AT, whatever the
 *     policy is now; the release velocity passes unchanged (64 for a velocity-0 note on, MIDI
 *     1.0's reading of it). A note off whose note on was dropped (outside the split, the channel or
 *     the range) is dropped.
 *   - Polyphonic key pressure (`An kk pp`): delivered to the output key of a held input key, else
 *     dropped.
 *   - Every other channel message (control change, program change, channel pressure, pitch bend):
 *     delivered unchanged when the leg takes its channel. All sound off (CC 120) and all notes off
 *     (CC 123) also forget the channel's held notes, since the instrument has released them.
 *   - A system message, a data byte above 127 or a status byte below 0x80 is dropped.
 *
 * The velocity curves, on a note-on velocity `v` in 1..127 (closed forms, every result in 1..127):
 *   - `linear`: `v`.
 *   - `soft`:   `round(sqrt(127 * v))`: the inverse of `hard`, louder under a light touch.
 *   - `hard`:   `max(1, round(v * v / 127))`: a square law, softer under a light touch.
 *   - `fixed`:  `velocityFixed`, whatever `v` is.
 * `round` is to the nearest integer; neither `127 * v` nor `v * v / 127` lies half-way between two
 * integers' squares or two integers, so no tie arises. The arithmetic is integer: the results do
 * not depend on the platform's libm.
 *
 * The names are the console's `VELOCITY_CURVES`, in the order of enum omx_velocity_curve.
 *
 * A map is a fixed-size state the caller allocates (omx_note_map_state_size()): 16 channels by
 * 128 keys as a bit table plus the output key of each held input key. Every call is pure C with no
 * allocation, no lock and no syscall; omx_note_map_release_all() lists the held notes (to unlink a
 * source, remove a leg or stop everything) in bounded slices.
 *
 * Oracle: test/kernels/note_map.c; its sabotage arm test/note-map-sabotage.sh.
 */
#ifndef OMX_NOTE_MAP_H
#define OMX_NOTE_MAP_H

#include <stdint.h>

#include "omxdsp.h"
#include "omx_contract.h"

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "note/map"

/** @brief MIDI 1.0 channels a leg can take: 1..16. */
#define OMX_NOTE_CHANNELS 16
/** @brief MIDI 1.0 keys: 0..127. */
#define OMX_NOTE_KEYS 128
/** @brief A leg's `channel` that takes every channel. */
#define OMX_NOTE_CHANNEL_OMNI 0
/** @brief The largest transpose a leg declares, in semitones, either way. */
#define OMX_NOTE_TRANSPOSE_MAX 48
/** @brief The most messages one omx_note_map_event() call writes: a release and a note on. */
#define OMX_NOTE_MAP_OUT_MAX 2
/** @brief The most notes a map can hold, every key of every channel. */
#define OMX_NOTE_MAP_HELD_MAX (OMX_NOTE_CHANNELS * OMX_NOTE_KEYS)

/** @brief The velocity curves, in the console's `VELOCITY_CURVES` order. */
enum omx_velocity_curve {
  OMX_VELOCITY_LINEAR = 0, /**< `linear`: the identity. */
  OMX_VELOCITY_SOFT = 1,   /**< `soft`: `round(sqrt(127 v))`. */
  OMX_VELOCITY_HARD = 2,   /**< `hard`: `max(1, round(v² / 127))`. */
  OMX_VELOCITY_FIXED = 3,  /**< `fixed`: the leg's `velocity_fixed`. */
  OMX_VELOCITY_CURVE_COUNT = 4 /**< How many curves there are. */
};

/**
 * @brief A curve's name, as the console's `VELOCITY_CURVES` spells it.
 * @param curve An enum omx_velocity_curve value.
 * @return `"linear"`, `"soft"`, `"hard"` or `"fixed"`; NULL for any other value.
 * @note RT-safe: returns a string literal.
 */
static inline const char *omx_velocity_curve_name(int curve) {
  switch (curve) {
  case OMX_VELOCITY_LINEAR: return "linear";
  case OMX_VELOCITY_SOFT: return "soft";
  case OMX_VELOCITY_HARD: return "hard";
  case OMX_VELOCITY_FIXED: return "fixed";
  default: return 0;
  }
}

/**
 * @brief `round(sqrt(n))` for `n` in 0..16129, in integers.
 * @param n The radicand.
 * @return The integer nearest the square root.
 * @note RT-safe: at most 128 steps, no libm.
 */
static inline int omx_note_isqrt_round(int n) {
  int s = 0;
  while ((s + 1) * (s + 1) <= n) s++;
  /* sqrt(n) >= s + 1/2  <=>  n >= s² + s + 1/4  <=>  n > s² + s (integers) */
  return n - s * s > s ? s + 1 : s;
}

/**
 * @brief A note-on velocity through a curve.
 * @param curve An enum omx_velocity_curve value.
 * @param v The note-on velocity, 1..127.
 * @param fixed The leg's fixed velocity, 1..127, read by `fixed` only.
 * @return The mapped velocity, 1..127 (the closed forms of the file comment).
 * @pre `velocity-is-a-note-on`: `v` is 1..127.
 * @post `velocity-sounds`: the result is 1..127, never a note off.
 * @note RT-safe: integer arithmetic only. An unknown curve reads as `linear`.
 */
static inline int omx_velocity_curve_apply(int curve, int v, int fixed) {
  OMX_PRE(v >= 1 && v <= 127, "velocity-is-a-note-on");
  int out;
  switch (curve) {
  case OMX_VELOCITY_SOFT: out = omx_note_isqrt_round(127 * v); break;
  case OMX_VELOCITY_HARD: out = (2 * v * v + 127) / 254; break;
  case OMX_VELOCITY_FIXED: out = fixed; break;
  default: out = v; break;
  }
  if (out < 1) out = 1;
  if (out > 127) out = 127;
  OMX_POST(out >= 1 && out <= 127, "velocity-sounds");
  return out;
}

/** @brief A note leg's policy: which channel, which keys, how far transposed, which curve. */
struct omx_note_leg {
  int channel;        /**< OMX_NOTE_CHANNEL_OMNI or 1..16. */
  int key_low;        /**< The lowest key the leg takes, 0..127. */
  int key_high;       /**< The highest key the leg takes, key_low..127. */
  int transpose;      /**< Semitones, -OMX_NOTE_TRANSPOSE_MAX..+OMX_NOTE_TRANSPOSE_MAX. */
  int velocity_curve; /**< An enum omx_velocity_curve value. */
  int velocity_fixed; /**< 1..127, read by the `fixed` curve only. */
};

/**
 * @brief The policy a new leg starts with: omni, every key, no transpose, `linear`, fixed 100.
 * @return The default leg.
 * @note RT-safe.
 */
static inline struct omx_note_leg omx_note_leg_default(void) {
  struct omx_note_leg l = {OMX_NOTE_CHANNEL_OMNI, 0, 127, 0, OMX_VELOCITY_LINEAR, 100};
  return l;
}

/**
 * @brief Whether every field of a leg lies inside its declared travel.
 * @param l The leg.
 * @return 1 when valid, else 0.
 * @note RT-safe.
 */
static inline int omx_note_leg_valid(const struct omx_note_leg *l) {
  return l->channel >= OMX_NOTE_CHANNEL_OMNI && l->channel <= OMX_NOTE_CHANNELS && l->key_low >= 0 &&
         l->key_low <= l->key_high && l->key_high <= 127 && l->transpose >= -OMX_NOTE_TRANSPOSE_MAX &&
         l->transpose <= OMX_NOTE_TRANSPOSE_MAX && l->velocity_curve >= 0 &&
         l->velocity_curve < OMX_VELOCITY_CURVE_COUNT && l->velocity_fixed >= 1 && l->velocity_fixed <= 127;
}

/** @brief One MIDI 1.0 channel message: a status byte and up to two data bytes. */
struct omx_note_msg {
  uint8_t status; /**< 0x80..0xEF: the kind in the high nibble, channel - 1 in the low. */
  uint8_t data1;  /**< The key, the controller or the low byte; 0..127. */
  uint8_t data2;  /**< The velocity, the value or the high byte; 0..127 (0 when unused). */
};

/**
 * @brief A leg's state: its policy, the held-note table and the drop counter.
 * @invariant `held-key-in-range`: a held input key's output key is 0..127.
 */
struct omx_note_map {
  struct omx_note_leg leg;                               /**< The policy now. */
  uint32_t held[OMX_NOTE_CHANNELS][OMX_NOTE_KEYS / 32]; /**< Bit `k` of channel `c`: key k is held. */
  uint8_t out_key[OMX_NOTE_CHANNELS][OMX_NOTE_KEYS];    /**< The output key each held key sounds. */
  uint32_t held_count;                                  /**< How many keys are held now. */
  uint32_t out_of_range;                                /**< Note ons dropped: the output key was outside 0..127. */
};

/** @brief The state layout, exported: omx_note_map_state_size() and omx_note_map_state_align(). */
OMXDSP_STATE_LAYOUT(note_map, struct omx_note_map)

/**
 * @brief Set a map up with a policy, nothing held, the counter at zero.
 * @param m The caller's state.
 * @param leg The policy; an invalid one reads as omx_note_leg_default().
 * @pre `leg-valid`: every field inside its travel.
 * @note Not RT: called where the leg is created. Writes every byte of the held table.
 */
static inline void omx_note_map_init(struct omx_note_map *m, const struct omx_note_leg *leg) {
  OMX_PRE(omx_note_leg_valid(leg), "leg-valid");
  m->leg = omx_note_leg_valid(leg) ? *leg : omx_note_leg_default();
  for (int c = 0; c < OMX_NOTE_CHANNELS; c++) {
    for (int w = 0; w < OMX_NOTE_KEYS / 32; w++) m->held[c][w] = 0u;
    for (int k = 0; k < OMX_NOTE_KEYS; k++) m->out_key[c][k] = 0u;
  }
  m->held_count = 0u;
  m->out_of_range = 0u;
}

/**
 * @brief Change a map's policy; the held notes keep the output keys they sounded.
 * @param m The state.
 * @param leg The new policy.
 * @return 1 when taken; 0 when `leg` is invalid, and the policy is left as it was.
 * @note RT-safe: one copy.
 */
static inline int omx_note_map_set_leg(struct omx_note_map *m, const struct omx_note_leg *leg) {
  if (!omx_note_leg_valid(leg)) return 0;
  m->leg = *leg;
  return 1;
}

/**
 * @brief Whether input key `key` on channel index `ch` (0..15) is held.
 * @param m The state.
 * @param ch The channel index, 0..15.
 * @param key The input key, 0..127.
 * @return 1 when held, else 0.
 * @note RT-safe.
 */
static inline int omx_note_map_is_held(const struct omx_note_map *m, int ch, int key) {
  return (int)((m->held[ch][key >> 5] >> (key & 31)) & 1u);
}

/**
 * @brief Mark or clear one held key (the table's single writer).
 * @param m The state.
 * @param ch The channel index, 0..15.
 * @param key The input key, 0..127.
 * @param on 1 to hold, 0 to release.
 * @note RT-safe. Keeps held_count equal to the table's population.
 */
static inline void omx_note_map_mark(struct omx_note_map *m, int ch, int key, int on) {
  const uint32_t bit = 1u << (key & 31);
  const int was = (m->held[ch][key >> 5] & bit) != 0u;
  if (on && !was) {
    m->held[ch][key >> 5] |= bit;
    m->held_count++;
  } else if (!on && was) {
    m->held[ch][key >> 5] &= ~bit;
    m->held_count--;
  }
}

/**
 * @brief Whether the leg takes channel index `ch` (0..15).
 * @param m The state.
 * @param ch The channel index.
 * @return 1 when omni or `ch + 1` is the leg's channel, else 0.
 * @note RT-safe.
 */
static inline int omx_note_map_takes_channel(const struct omx_note_map *m, int ch) {
  return m->leg.channel == OMX_NOTE_CHANNEL_OMNI || m->leg.channel == ch + 1;
}

/**
 * @brief One message through the leg: the messages to deliver, written to `out`.
 * @param m The state.
 * @param in The incoming message.
 * @param out Room for OMX_NOTE_MAP_OUT_MAX messages.
 * @return How many messages were written: 0 (dropped), 1, or 2 (a release, then a note on).
 * @pre `leg-valid`: the policy is inside its travel.
 * @post `held-key-in-range`: every message written carries data bytes 0..127.
 * @note RT-safe: no allocation, no lock, no syscall; constant time.
 */
static inline int omx_note_map_event(struct omx_note_map *m, struct omx_note_msg in, struct omx_note_msg *out) {
  OMX_PRE(omx_note_leg_valid(&m->leg), "leg-valid");
  if (in.status < 0x80u || in.status >= 0xF0u || in.data1 > 127u || in.data2 > 127u) return 0;
  const int kind = in.status & 0xF0;
  const int ch = in.status & 0x0F;
  const int key = in.data1;
  int n = 0;
  if (kind == 0x80 || (kind == 0x90 && in.data2 == 0u)) {
    if (!omx_note_map_is_held(m, ch, key)) return 0;
    out[n].status = (uint8_t)(0x80 | ch);
    out[n].data1 = m->out_key[ch][key];
    out[n].data2 = kind == 0x80 ? in.data2 : 64u;
    n++;
    omx_note_map_mark(m, ch, key, 0);
  } else if (kind == 0x90) {
    if (!omx_note_map_takes_channel(m, ch) || key < m->leg.key_low || key > m->leg.key_high) return 0;
    const int to = key + m->leg.transpose;
    if (to < 0 || to > 127) {
      m->out_of_range++;
      return 0;
    }
    if (omx_note_map_is_held(m, ch, key)) {
      out[n].status = (uint8_t)(0x80 | ch);
      out[n].data1 = m->out_key[ch][key];
      out[n].data2 = 64u;
      n++;
    }
    out[n].status = (uint8_t)(0x90 | ch);
    out[n].data1 = (uint8_t)to;
    out[n].data2 = (uint8_t)omx_velocity_curve_apply(m->leg.velocity_curve, in.data2, m->leg.velocity_fixed);
    n++;
    m->out_key[ch][key] = (uint8_t)to;
    omx_note_map_mark(m, ch, key, 1);
  } else if (kind == 0xA0) {
    if (!omx_note_map_is_held(m, ch, key)) return 0;
    out[n] = in;
    out[n].data1 = m->out_key[ch][key];
    n++;
  } else {
    if (!omx_note_map_takes_channel(m, ch)) return 0;
    out[n++] = in;
    if (kind == 0xB0 && (in.data1 == 120u || in.data1 == 123u))
      for (int k = 0; k < OMX_NOTE_KEYS; k++) omx_note_map_mark(m, ch, k, 0);
  }
  for (int i = 0; i < n; i++) OMX_POST(out[i].data1 <= 127u && out[i].data2 <= 127u, "held-key-in-range");
  return n;
}

/**
 * @brief Release held notes: a note off for each, at the output key it sounded, at most `cap` per
 *        call; the released keys are forgotten.
 * @param m The state.
 * @param out Room for `cap` messages.
 * @param cap How many messages `out` takes; OMX_NOTE_MAP_HELD_MAX releases everything in one call.
 * @return How many note offs were written; call again while it returns `cap`.
 * @note RT-safe: bounded by OMX_NOTE_MAP_HELD_MAX steps, channel by channel, key by key.
 */
static inline int omx_note_map_release_all(struct omx_note_map *m, struct omx_note_msg *out, int cap) {
  int n = 0;
  for (int c = 0; c < OMX_NOTE_CHANNELS && n < cap && m->held_count > 0u; c++)
    for (int k = 0; k < OMX_NOTE_KEYS && n < cap; k++) {
      if (!omx_note_map_is_held(m, c, k)) continue;
      out[n].status = (uint8_t)(0x80 | c);
      out[n].data1 = m->out_key[c][k];
      out[n].data2 = 64u;
      n++;
      omx_note_map_mark(m, c, k, 0);
    }
  return n;
}

#undef OMX_CONTRACT_STAGE

#endif /* OMX_NOTE_MAP_H */
