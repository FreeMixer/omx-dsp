// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_reverb.h — native algorithmic reverb, ONE Schroeder-Moorer comb+allpass kernel (Freeverb
 * lineage) serving TWO configurations (OMX_REVERB_ROOM, OMX_REVERB_HALL — same omx_fv_comb /
 * omx_fv_allpass functions, each with its own delay-line lengths + feedback/damping curve) plus a
 * second, structurally different kernel, OMX_REVERB_PLATE (Dattorro figure-8 plate tank: input
 * diffusers -> a damped delay-network tank). Three configurations, two kernels — HALL is not a new
 * mechanism, it is ROOM's kernel handed a bigger, darker tuning (see the HALL comment below for what
 * that means and why it earns the name rather than just being "room, but with size turned up").
 * Pure-C, RT-safe: NO PipeWire, NO allocation here, NO libc beyond <math.h>. All delay-line memory is
 * a single caller-owned float POOL bound by omx_reverb_state_layout (allocated on insert). Denormals
 * are flushed to zero so a long tail never falls into denormal-slow math. mix=0 is bit-identical dry.
 */
#ifndef OMX_MIX_REVERB_H
#define OMX_MIX_REVERB_H

#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_lfo.h> /* omx_lfo_shape — the tier's ONE oscillator shape, minted 2026-09-22 */
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_lookahead.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_param.h>
#include <omxdsp/omx_units.h>

/*
 * FIVE configurations over TWO kernels. REVERSE and GATED are the ROOM comb network handed an
 * OUTPUT STAGE — a windowed time mirror and a hold-and-cut envelope — never a third reverberator:
 * what is new is what happens to the wet after the network, which is why they are values of this
 * enum and not a second mechanism (spec 2026-07-15-native-delay-reverb.md, Amendment 2026-09-17).
 */
enum omx_reverb_algo {
  OMX_REVERB_ROOM = 0,
  OMX_REVERB_PLATE = 1,
  OMX_REVERB_HALL = 2,
  OMX_REVERB_REVERSE = 3,
  OMX_REVERB_GATED = 4
};

/* REVERSE's mirror window, in ms — the declared range of `reverse_ms`. The ceiling is what the
 * pool budgets a ring for at OMX_REVERB_MAX_RATE; the window is a DECLARED field and never
 * derived from `size`'s RT60, because an RT60 of seconds cannot be buffered and a window silently
 * clamped to fit is the silent-clamp defect. */
#define OMX_REVERB_REVERSE_MIN_MS OMX_REVERB_REVERSE_RANGE_MIN
#define OMX_REVERB_REVERSE_MAX_MS OMX_REVERB_REVERSE_RANGE_MAX

/* --- fixed tunings (samples @ 44.1 kHz, scaled to the live rate at layout) --------------------- */
/* Freeverb comb tunings (the canonical set) + the stereo spread offset for the R channel. Shared
 * shape for ROOM and HALL: 8 combs + 4 series allpass per channel, each configuration owning its
 * own lengths (see omx_reverb_state_layout — HALL_COMB/HALL_AP below the room set). */
#define OMX_FV_NCOMB 8
#define OMX_FV_NALLPASS 4
/* Dattorro plate delay-line lengths (the classic figure-8, @29761 Hz in the paper — scaled here). */

/* THE PLATE'S TANK IS MODULATED, as Dattorro's figure-8 is (spec 2026-07-15-native-delay-reverb.md).
 * The tank's first allpass in each half recirculates at 0.7 with nothing to smear it, so a fixed
 * length made the late tail PERIODIC at that length: autocorrelation 0.709 at 22.6 ms on the left
 * leg and 0.53 at 30.5 ms on the right, against ROOM's 0.26 — the metallic ring a plate is
 * reached for in spite of (2026-09-17-reverb-math-review.md R-2, ruled 2026-09-17).
 *
 * The paper's own numbers: +-8 samples of excursion at its 29 761 Hz tuning rate, at about 1 Hz.
 * The excursion is scaled to the live rate with the same `sr / 29761` the tank LENGTHS are scaled
 * by, so the modulation is the same fraction of the same line at every clock; the rate is in Hz
 * and is NOT scaled, because 1 Hz is one hertz. BOTH halves are modulated, in quadrature: each
 * half feeds one leg, and modulating only the first would leave the right leg ringing at its own
 * 30.5 ms and the mono sum would bring it straight back. */
#define OMX_REVERB_PLATE_TUNING_RATE 29761.0f
#define OMX_REVERB_PLATE_MOD_SAMPLES 8.0f
#define OMX_REVERB_PLATE_MOD_HZ 1.0f
/* THE PLATE'S DECAY, declared once: the tank's loop gain is FLOOR + SPAN * size (0.5 at size 0,
 * 0.99 at size 1, the same 0.99 ceiling the other configurations keep under unity), and the tank
 * lines are Dattorro's lengths at OMX_REVERB_PLATE_TUNING_RATE, scaled to the live rate at layout.
 * A test that waits out a plate's tail reads its decay from these, never from a copy. */
#define OMX_REVERB_PLATE_DECAY_FLOOR 0.5f
#define OMX_REVERB_PLATE_DECAY_SPAN 0.49f
#define OMX_REVERB_PLATE_TANK_TUNINGS 672, 1800, 908, 2656, 1996, 3163
/* THE DEPTH IS A FIELD, `plate_mod_depth`, in percent of the paper's excursion above
 * (2026-09-26-native-fx-catalogue.md §2, Reverb): 100 is the plate as it always ran, so the
 * come-up is bit-identical to the tank before the field; 0 is the unmodulated, ringing tank. The
 * row's travel is `REVERB_PLATE_MOD_DEPTH_RANGE`; this is its come-up, generated from that travel's
 * `default` (dsp-primitives §7), the value a fresh lane's atom holds before the row writes one. */
#define OMX_REVERB_PLATE_MOD_DEPTH_DEFAULT ((float)OMX_REVERB_PLATE_MOD_DEPTH_RANGE_DEFAULT)

/* GATED'S ARMING IS A RAMP, NOT A SNAP, and the ramp is in MILLISECONDS so it is the same ramp at
 * every clock. `gate_env` used to go to 1.0 in ONE SAMPLE: a hit arriving while the envelope was
 * mid-release multiplied the wet straight back up, and because the wet's own step shrinks with
 * the rate while a one-sample jump does not, the click grew with the clock — 1.65 / 4.84 / 9.84 /
 * 20.47 times the largest step the ROOM reference makes through the same input, at 44.1 / 48 /
 * 96 / 192 kHz (2026-09-17-reverb-math-review.md R-7, ruled 2026-09-17).
 *
 * 1.5 ms is the LONGEST ramp inside the ruled 1-2 ms window that keeps the CLOSING's own law at
 * every declared rate — the gate adds no step the ROOM reference does not already carry.
 * Measured, not chosen: at 0.5, 1.0 and 1.5 ms the arming step is exactly ROOM's own at all four
 * rates; at 2.0 ms it exceeds it by 0.4 % at 192 kHz, because the ramp is still climbing when the
 * wet's own largest step arrives. The longest passing ramp is taken because a longer one is the
 * gentler edge, and 1.5 ms is far inside a snare transient, so the Phil-Collins arm keeps its
 * bite. It is not a control: the control set is the spec's, and this is the shape of an edge
 * inside it. */

/* Pre-delay ring: up to 100 ms @ the declared rate roof (RT_HARD_TARGET_RATE, generated). */
#define OMX_REVERB_PREDELAY_MAX_MS OMX_REVERB_PREDELAY_RANGE_MAX
#define OMX_REVERB_MAX_RATE OMX_RT_HARD_TARGET_RATE
#define OMX_REVERB_PREDELAY_CAP (((OMX_REVERB_MAX_RATE / 1000) * OMX_REVERB_PREDELAY_MAX_MS) + 1)

/* The pool must hold: 2ch x (8 comb + 4 allpass) Freeverb lines TWICE (ROOM's set and HALL's own,
 * longer set) + the plate tank lines + 2 pre-delay rings + REVERSE's two mirror rings. Sized
 * generously (fixed upper bound) so one pool serves any of the five configurations; unused lines
 * just sit idle, and it is allocated ONLY on a strip's first reverb enable.
 *
 * THE FIGURE IS MEASURED, NOT DERIVED BY HAND. Every line here is `ms -> samples` at the rate,
 * so the requirement is proportional to it; `omx_reverb_state_layout` is asked what it takes and
 * the answer is the pool (`npm run probe:reverb-pool`, and rt_contracts.test.c's first reverb
 * arm refuses any rate it does not cover):
 *
 *     44 100 Hz   187 469 floats      96 000 Hz   408 006 floats
 *     48 000 Hz   204 021 floats     192 000 Hz   815 976 floats
 *
 * The pool is sized from the layout's own arithmetic at the highest declared rate: 0.875 Mi
 * floats (3.5 MB per engaged strip) covers 192 kHz with 101 528 floats of headroom. A pool
 * that cannot hold the layout is a refusal (`_exhausted`, and `omx_reverb_process` returns dry),
 * never a silent passthrough; `test/fx/reverb_math.test.c` measures wet energy at every declared
 * rate. 384 kHz takes 1 631 922 floats, does not fit, and is not a rate this console declares.
 * (A pool 48 745 floats short once made 192 kHz a silent passthrough — review 2026-09-17.) */
#define OMX_REVERB_POOL_FLOATS (917504)

struct omx_reverb {
  int enabled;        /* 0 -> passthrough */
  int algorithm;      /* OMX_REVERB_ROOM | OMX_REVERB_PLATE | OMX_REVERB_HALL */
  float size;         /* 0..1 -> comb feedback / tank decay (bigger = longer) */
  float damping;      /* 0..1 -> comb/tank low-pass in the feedback (HF absorption) */
  float predelay_ms;  /* onset delay before the reverb (ms) */
  float width;        /* 0..1 stereo spread of the wet output */
  float mix;          /* 0 = dry (bit-identical), 1 = wet-only */
  float lowcut;       /* wet-path high-pass corner (Hz); 0 = none */
  float highcut;      /* wet-path low-pass corner (Hz); >= Nyquist = none */
  /* REVERSE only: the mirror window in ms (OMX_REVERB_REVERSE_MIN_MS..MAX_MS). The swell a hit
   * produces spans TWO windows after it; the dry is never delayed (see omx_reverb_process). */
  float reverse_ms;
  /* GATED only: the wet's hold-and-cut envelope. `gate_threshold_db` is the level the kernel's own
   * mono feed must reach to ARM the hold - an explicit comparison, never a hidden onset detector. */
  float hold_ms;
  float release_ms;
  float gate_threshold_db;
  /* PLATE only: the tank's modulation depth, % of Dattorro's excursion
   * (OMX_REVERB_PLATE_MOD_DEPTH_DEFAULT = the plate's own). Read by omx_reverb_plate_excursion. */
  float plate_mod_depth;
};

struct omx_reverb_state {
  float *_pool;             /* caller-owned pool base (freed by the caller, not here) */
  int _exhausted;           /* set by omx_pool_take when the pool ran out — see the layout */
  uint32_t pool_len;
  float sr;
  /* Freeverb: per-channel comb + allpass lines (pointers into _pool) + their state. */
  float *comb_l[OMX_FV_NCOMB]; uint32_t comb_len_l[OMX_FV_NCOMB]; uint32_t comb_pos_l[OMX_FV_NCOMB]; float comb_damp_l[OMX_FV_NCOMB];
  float *comb_r[OMX_FV_NCOMB]; uint32_t comb_len_r[OMX_FV_NCOMB]; uint32_t comb_pos_r[OMX_FV_NCOMB]; float comb_damp_r[OMX_FV_NCOMB];
  float *ap_l[OMX_FV_NALLPASS]; uint32_t ap_len_l[OMX_FV_NALLPASS]; uint32_t ap_pos_l[OMX_FV_NALLPASS];
  float *ap_r[OMX_FV_NALLPASS]; uint32_t ap_len_r[OMX_FV_NALLPASS]; uint32_t ap_pos_r[OMX_FV_NALLPASS];
  /* HALL: the SAME Freeverb kernel (omx_fv_comb/omx_fv_allpass) over its own, longer/wider-spread
   * lines — a second configuration of the comb+allpass network, not a second mechanism. */
  float *comb_l_hall[OMX_FV_NCOMB]; uint32_t comb_len_l_hall[OMX_FV_NCOMB]; uint32_t comb_pos_l_hall[OMX_FV_NCOMB]; float comb_damp_l_hall[OMX_FV_NCOMB];
  float *comb_r_hall[OMX_FV_NCOMB]; uint32_t comb_len_r_hall[OMX_FV_NCOMB]; uint32_t comb_pos_r_hall[OMX_FV_NCOMB]; float comb_damp_r_hall[OMX_FV_NCOMB];
  float *ap_l_hall[OMX_FV_NALLPASS]; uint32_t ap_len_l_hall[OMX_FV_NALLPASS]; uint32_t ap_pos_l_hall[OMX_FV_NALLPASS];
  float *ap_r_hall[OMX_FV_NALLPASS]; uint32_t ap_len_r_hall[OMX_FV_NALLPASS]; uint32_t ap_pos_r_hall[OMX_FV_NALLPASS];
  /* Dattorro plate: input diffusers + the two tank halves (pointers into _pool) + state. */
  float *plate_diff[4]; uint32_t plate_diff_len[4]; uint32_t plate_diff_pos[4];
  float *plate_tank[6]; uint32_t plate_tank_len[6]; uint32_t plate_tank_pos[6];
  float plate_damp1, plate_damp2;
  float plate_mod_phase;    /* the tank LFO, in turns [0,1) — advanced per sample, never a libm call */
  /* REVERSE's mirror rings (one per leg) + the writer and its position inside the current hop.
   * The ring holds 2 x OMX_REVERB_REVERSE_MAX_MS at the laid-out rate, which is every sample the
   * two readers can ask for. */
  float *rev_l; float *rev_r; uint32_t rev_cap; uint32_t rev_w; uint32_t rev_phase;
  /* GATED's envelope: what the wet is multiplied by, and how many samples of hold are left. */
  float gate_env; uint32_t gate_hold_left;
  /* The shared pre-delay ring — ONE ring, because the network is fed the mono sum of the legs
   * (`xin` in omx_reverb_process); a second ring would hold the same samples twice and read
   * them never (2026-09-17-reverb-math-review.md R-3). Wet-path filter state follows. */
  float *pre; uint32_t pre_cap; uint32_t pre_pos;
  float lc_l, lc_r, hc_l, hc_r; /* one-pole low/high-cut state per leg */
  uint32_t next_free;           /* pool cursor used during layout */
};

/*
 * THE DAMPING ONE-POLE — the ONE form a damping filter takes inside a reverb's feedback path
 * (spec 2026-07-15-native-delay-reverb.md, Amendment 2026-09-17 §A).
 *
 * `y += (1 - p)(x - y)` with the POLE `p` = `damping`: DC gain is exactly 1 for every p < 1, so
 * damping shortens the HIGH end's decay and NEVER the tail's level. The pole is clamped to
 * OMX_REVERB_DAMP_POLE_MAX because a pole of exactly 1 FREEZES the filter — its output stops
 * following its input, the loop it sits in loses its feedback and the tail dies.
 *
 * ONE inline for the comb and the tank alike, because the other spelling is silent and wrong: a
 * one-pole whose COEFFICIENT is the damping (`y += damping * (x - y)`, pole `1 - damping`) has DC
 * gain `damping`, so at damping 0 its loop carries nothing — a plate with no tail — and the knob
 * runs backwards. That defect shipped here and cost a lane (spec Amendment 2026-09-17 §A).
 */
#define OMX_REVERB_DAMP_POLE_MAX 0.98f

/*
 * THE DAMPING POLE IS QUOTED AT A REFERENCE RATE, AND THE REFERENCE RATE IS 96 kHz.
 *
 * A one-pole's corner is `-ln(p) * sr / 2pi`, so a pole used RAW is a different filter at every
 * clock: damping 0.6 cornered at 3 585 Hz at 44.1 kHz and at 15 610 Hz at 192 kHz, and the same
 * knob therefore gave a ROOM HF decay 10.6 % longer and a HALL tail 8.3 % longer at 192 k than at
 * 44.1 k (2026-09-17-reverb-math-review.md R-1). A REAC box clocks its own segment, so a session
 * carried between segments changed voice for no reason the operator could see.
 *
 * `p' = p^(REF/sr)` holds the CORNER, which is what the knob means. 96 000 Hz is the reference
 * because it is the rate the desk runs (ruling 2026-09-14): at that rate the exponent is exactly
 * 1, no pow() is taken, and the 96 kHz response is bit-for-bit the one this console has always
 * made — pinned, all five configurations at three damping settings, by
 * `test/fx/fixtures/reverb_96k_reference.h` and `test/fx/reverb_math.test.c`'s identity arm.
 *
 * The CLAMP is applied at the reference rate, before the normalisation, because the clamp's
 * reason is a corner the tail can still hear — a pole of 1 freezes the filter and kills the loop
 * it sits in. Normalising a clamped 0.98 gives 0.98995 at 192 kHz, which is the SAME 308.7 Hz
 * corner and still comfortably below 1.
 */
#define OMX_REVERB_DAMP_REFERENCE_RATE ((float)OMX_DSP_KNOB_REFERENCE_RATE)

/* The damping knob turned into the pole the live rate needs. Called ONCE PER BLOCK — a powf per
 * sample in a comb bank would be the RT path's most expensive line by an order of magnitude — and
 * the per-sample filter is `omx_onepole_flush` itself, which is what every other recursive
 * low-pass in this file already is. */
static inline float omx_reverb_damp_pole(float damping, float sr) {
  float p = omx_clampf(damping, 0.0f, OMX_REVERB_DAMP_POLE_MAX); /* NaN: no damping */
  if (p <= 0.0f || sr == OMX_REVERB_DAMP_REFERENCE_RATE) return p; /* the reference rate is untouched */
  return powf(p, OMX_REVERB_DAMP_REFERENCE_RATE / sr);
}

/* Carve `len` floats out of the pool; returns the sub-buffer base (or NULL if the pool is
 * exhausted). THE LINE COMES BACK SILENT: a delay line is state, and state handed out dirty
 * makes a re-layout replay whatever the last layout left in it. The clear lives HERE, in the
 * primitive that hands the memory out, and not in each caller, because a caller that forgets is
 * a reverb that opens playing the old room (test/fx/reverb_math.test.c's re-layout arm bounds the
 * residue). Layout runs on INSERT, never on the RT thread, so the cost is paid once. */
static inline float *omx_pool_take(struct omx_reverb_state *s, uint32_t len) {
  if (s->next_free + len > s->pool_len) { s->_exhausted = 1; return NULL; }
  float *p = s->_pool + s->next_free;
  s->next_free += len;
  for (uint32_t i = 0; i < len; i++) p[i] = 0.0f;
  return p;
}

/* Bind a caller pool into all sub-buffers, scaling the canonical tunings to `sr`. Call once on insert. */
static inline void omx_reverb_state_layout(struct omx_reverb_state *s, float *pool, uint32_t pool_len, float sr) {
  /* bind the pool; zero the sub-buffer state (the caller calloc'd the pool, but be explicit for reuse) */
  /* THE RATE IS PUBLISHED LAST. `omx_reverb_process` runs a pool only when the rate it is
   * called at equals `s->sr`, so a layout in progress reads `sr == 0` — a rate no graph runs at —
   * and every block that meets it is a passthrough. That is what lets the control thread re-lay
   * out an ENGAGED reverb after a live rate change (mixer_strip.c's setter): the RT is already
   * passing through (the live rate is not the old layout's), and it resumes on the new room only
   * once every line below is cut (2026-09-25-native-fx-rt-review.md F1). */
  s->sr = 0.0f;
  atomic_thread_fence(memory_order_release);
  s->_pool = pool; s->pool_len = pool_len; s->next_free = 0; s->_exhausted = 0;
  const float scale = sr / 44100.0f;
  /* Freeverb comb tunings (Jezar's canonical set) and allpass tunings, in samples @ 44.1k. */
  static const uint32_t COMB[OMX_FV_NCOMB] = {1116,1188,1277,1356,1422,1491,1557,1617};
  static const uint32_t AP[OMX_FV_NALLPASS] = {556,441,341,225};
  static const uint32_t STEREO_SPREAD = 23; /* R channel offset for a wider image */
  for (int i = 0; i < OMX_FV_NCOMB; i++) {
    uint32_t ll = (uint32_t)(COMB[i] * scale) + 1;
    uint32_t lr = (uint32_t)((COMB[i] + STEREO_SPREAD) * scale) + 1;
    s->comb_l[i] = omx_pool_take(s, ll); s->comb_len_l[i] = ll; s->comb_pos_l[i] = 0; s->comb_damp_l[i] = 0.0f;
    s->comb_r[i] = omx_pool_take(s, lr); s->comb_len_r[i] = lr; s->comb_pos_r[i] = 0; s->comb_damp_r[i] = 0.0f;
  }
  for (int i = 0; i < OMX_FV_NALLPASS; i++) {
    uint32_t ll = (uint32_t)(AP[i] * scale) + 1;
    uint32_t lr = (uint32_t)((AP[i] + STEREO_SPREAD) * scale) + 1;
    s->ap_l[i] = omx_pool_take(s, ll); s->ap_len_l[i] = ll; s->ap_pos_l[i] = 0;
    s->ap_r[i] = omx_pool_take(s, lr); s->ap_len_r[i] = lr; s->ap_pos_r[i] = 0;
  }
  /* HALL comb + allpass tunings, in samples @ 44.1k, laid out the same way as ROOM's above but
   * into their own lines (HALL runs concurrently in the same pool so switching `algorithm` at
   * runtime never re-lays-out memory). The combs are ~2.2x ROOM's canonical set — a bigger space's
   * reflections travel farther before they recur, which is the "wider early-reflection spacing"
   * that makes a hall read as a bigger room rather than the same room turned up, AND (combined with
   * hall_feedback's higher floor below) the longer per-comb loop a decaying tail needs to keep
   * outlasting PLATE's much longer single Dattorro tank loop at the same `size` — measured, not
   * assumed: see the "hall tail outlasts plate" golden in test/fx/reverb.test.c. The allpass steps are
   * shorter than ROOM's (467/353/251/167 vs. 556/441/341/225): denser post-diffusion pulses layered
   * onto those wider comb echoes, which is what keeps a long hall tail from sounding sparse. The
   * wider stereo spread (37 vs. ROOM's 23 samples) widens the L/R image to match. */
  static const uint32_t HALL_COMB[OMX_FV_NCOMB] = {2455,2614,2809,2983,3128,3280,3425,3557};
  static const uint32_t HALL_AP[OMX_FV_NALLPASS] = {467,353,251,167};
  static const uint32_t HALL_STEREO_SPREAD = 37;
  for (int i = 0; i < OMX_FV_NCOMB; i++) {
    uint32_t ll = (uint32_t)(HALL_COMB[i] * scale) + 1;
    uint32_t lr = (uint32_t)((HALL_COMB[i] + HALL_STEREO_SPREAD) * scale) + 1;
    s->comb_l_hall[i] = omx_pool_take(s, ll); s->comb_len_l_hall[i] = ll; s->comb_pos_l_hall[i] = 0; s->comb_damp_l_hall[i] = 0.0f;
    s->comb_r_hall[i] = omx_pool_take(s, lr); s->comb_len_r_hall[i] = lr; s->comb_pos_r_hall[i] = 0; s->comb_damp_r_hall[i] = 0.0f;
  }
  for (int i = 0; i < OMX_FV_NALLPASS; i++) {
    uint32_t ll = (uint32_t)(HALL_AP[i] * scale) + 1;
    uint32_t lr = (uint32_t)((HALL_AP[i] + HALL_STEREO_SPREAD) * scale) + 1;
    s->ap_l_hall[i] = omx_pool_take(s, ll); s->ap_len_l_hall[i] = ll; s->ap_pos_l_hall[i] = 0;
    s->ap_r_hall[i] = omx_pool_take(s, lr); s->ap_len_r_hall[i] = lr; s->ap_pos_r_hall[i] = 0;
  }
  /* Dattorro plate diffusers + tank (classic lengths @ 29761 Hz, scaled to sr). */
  static const uint32_t PDIFF[4] = {142,107,379,277};
  static const uint32_t PTANK[6] = {OMX_REVERB_PLATE_TANK_TUNINGS};
  const float pscale = sr / OMX_REVERB_PLATE_TUNING_RATE;
  for (int i = 0; i < 4; i++) {
    uint32_t l = (uint32_t)(PDIFF[i] * pscale) + 1;
    s->plate_diff[i] = omx_pool_take(s, l); s->plate_diff_len[i] = l; s->plate_diff_pos[i] = 0;
  }
  for (int i = 0; i < 6; i++) {
    uint32_t l = (uint32_t)(PTANK[i] * pscale) + 1;
    s->plate_tank[i] = omx_pool_take(s, l); s->plate_tank_len[i] = l; s->plate_tank_pos[i] = 0;
  }
  s->plate_damp1 = s->plate_damp2 = 0.0f;
  s->plate_mod_phase = 0.0f;
  /* pre-delay rings */
  uint32_t pcap = (uint32_t)((OMX_REVERB_PREDELAY_MAX_MS / 1000.0f) * sr) + 1;
  s->pre = omx_pool_take(s, pcap); s->pre_cap = pcap; s->pre_pos = 0;
  s->lc_l = s->lc_r = s->hc_l = s->hc_r = 0.0f;
  /* REVERSE's mirror rings: two full windows at the ceiling, plus the two samples the reader
   * offsets need past 2W (see the reader's `offB`). */
  uint32_t rcap = 2u * (uint32_t)((OMX_REVERB_REVERSE_MAX_MS / 1000.0f) * sr) + 2u;
  s->rev_l = omx_pool_take(s, rcap); s->rev_r = omx_pool_take(s, rcap);
  s->rev_cap = rcap; s->rev_w = 0; s->rev_phase = 0;
  s->gate_env = 0.0f; s->gate_hold_left = 0;

  /* POOL EXHAUSTION IS A SILENT REVERB, NEVER A NULL DEREFERENCE.
   *
   * `omx_pool_take` returns NULL when the pool runs out, and every sub-buffer above stores that
   * return UNCHECKED. A NULL in `comb_l[i]` is not a bad reverb — it is a segfault ON THE RT
   * THREAD, which takes the whole console down, not one insert. Found 2026-08-07 by
   * `gcc -fanalyzer` (CWE-690); UBSan and the closed-form oracles had both been clean, because
   * neither ever ran the layout against a pool too small to satisfy it.
   *
   * HOW MUCH HEADROOM THERE ACTUALLY IS, per RATE — because it scales with sr, and because two
   * hand counts in a row got it wrong here. What the layout TAKES is MEASURED
   * (`npm run probe:reverb-pool`); the count that stood here read 282 392 floats at 192 kHz
   * because it omitted the predelay ring and the whole HALL comb set, and the pool was sized
   * from it, which is how every reverb became a silent passthrough at 192 kHz until 2026-09-17.
   *
   *     44 100 Hz   187 469 floats   fits, x4.89 headroom
   *     96 000 Hz   408 006 floats   fits, x2.25     <- THE RIG'S RATE (ruling 2026-09-14)
   *    192 000 Hz   815 976 floats   fits, x1.12     <- OMX_REVERB_MAX_RATE, the declared ceiling
   *    384 000 Hz 1 631 922 floats   EXHAUSTS by 714 418
   *
   * THE GUARD BELOW IS THEREFORE REACHABLE, not theoretical: the operator stated on 2026-08-07
   * that the desk runs "at most 192 or 384 kHz, usually 96", and at 384 kHz this layout does not
   * fit. What happens then is a SILENT REVERB on that insert — not a NULL dereference on the RT
   * thread, which is what it was before the guard.
   *
   * A rate above OMX_REVERB_MAX_RATE is out of contract for this file and belongs refused ABOVE
   * it, where the operator can be told, rather than absorbed here as a missing tail. Sizing the
   * pool for 384 kHz instead would cost (1 631 922 - 917 504) x 4 B = 2.7 MB more per reverb
   * state and is a deliberate decision, not a default.
   *
   * failure mode is exactly the one already designed for: the insert is a no-op. */
  if (s->_exhausted) s->_pool = NULL;
  atomic_thread_fence(memory_order_release); /* every line above is cut before the rate says so */
  s->sr = sr;
}

/*
 * THE LINES RUN A CHUNK AT A TIME. Each word below runs its line(s) over a CHUNK of up to
 * OMX_REVERB_CHUNK samples with its position and its filter state in registers, and splits the
 * chunk only where the line wraps, so the inner loop is straight-line arithmetic over two arrays
 * (an allpass or diffuser carries no recursion inside a chunk shorter than its line).
 *
 * THE ARITHMETIC IS PER-SAMPLE ORDER: a sample's eight comb outputs are summed c0..c7 into its
 * accumulator, and a sample passes the allpasses in series order, so the output is bit-identical
 * to one sample at a time — `test/fx/reverb_math.test.c`'s 96 kHz identity arm (0 ulp, all five
 * configurations x three dampings) and `mix_golden.test.c` hold it.
 *
 * A position that is not inside its line (a stale one past a re-cut line, F7) restarts the line at
 * 0 rather than reading one sample past it.
 */
#define OMX_REVERB_CHUNK 64u

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "reverb/comb"
/* THE EIGHT COMBS OF ONE LEG over `n` samples: each reads its tail, low-pass filters it
 * (`damp_pole` from omx_reverb_damp_pole), stores in+fb*damped, and the eight tails are summed
 * c0..c7 into `acc`. The combs run SAMPLE-MAJOR, all eight inside the sample: each comb's damping
 * filter is a recursion one sample long, and eight independent recursions in flight keep the core
 * busy — comb-major, gathered-vector and time-major-tile orders all measure slower
 * (`reverb-cost-ab.sh`). The segment ends where the first of the eight lines wraps, so the inner
 * loop carries no wrap test. */
static inline void omx_fv_comb_bank_block(float *const *comb, const uint32_t *comb_len,
                                          uint32_t *comb_pos, float *comb_damp, const float *in,
                                          float *acc, uint32_t n, float feedback, float damp_pole) {
  OMX_PRE(n <= OMX_REVERB_CHUNK, "chunk-bound");
  OMX_PRE(feedback >= 0.0f && feedback < 1.0f, "feedback-below-unity");
  OMX_PRE(damp_pole >= 0.0f && damp_pole < 1.0f, "pole-in-declared-range");
  uint32_t pos[OMX_FV_NCOMB];
  float d[OMX_FV_NCOMB];
  float *line[OMX_FV_NCOMB];
  for (int c = 0; c < OMX_FV_NCOMB; c++) {
    OMX_PRE(comb[c] != NULL && comb_len[c] > 0u, "line-bound");
    pos[c] = comb_pos[c] < comb_len[c] ? comb_pos[c] : 0u;
    d[c] = comb_damp[c];
  }
  for (uint32_t i = 0; i < n;) {
    uint32_t m = n - i;
    for (int c = 0; c < OMX_FV_NCOMB; c++) {
      if (comb_len[c] - pos[c] < m) m = comb_len[c] - pos[c];
      line[c] = comb[c] + pos[c];
    }
    for (uint32_t j = 0; j < m; j++) {
      const float x = in[i + j];
      float sum = 0.0f;
#pragma GCC unroll 8
      for (int c = 0; c < OMX_FV_NCOMB; c++) {
        const float out = line[c][j];
        const float damped = omx_onepole_flush(&d[c], out, damp_pole);
        line[c][j] = omx_flush(x + damped * feedback);
        sum += out;
      }
      acc[i + j] = sum;
    }
    i += m;
    for (int c = 0; c < OMX_FV_NCOMB; c++) {
      pos[c] += m;
      if (pos[c] >= comb_len[c]) pos[c] = 0u;
    }
  }
  for (int c = 0; c < OMX_FV_NCOMB; c++) {
    comb_pos[c] = pos[c]; comb_damp[c] = d[c];
    OMX_POST(comb_pos[c] < comb_len[c], "position-inside-line");
    OMX_POST(d[c] - d[c] == 0.0f, "finite-damping-state");
  }
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "reverb/allpass"
/* One Schroeder allpass (fixed 0.5 coefficient) over `n` samples of `x`, in place. */
static inline void omx_fv_allpass_block(float *buf, uint32_t len, uint32_t *pos, float *x, uint32_t n) {
  OMX_PRE(buf != NULL && len > 0u, "line-bound");
  OMX_PRE(n <= OMX_REVERB_CHUNK, "chunk-bound");
  uint32_t p = *pos < len ? *pos : 0u;
  for (uint32_t i = 0; i < n;) {
    uint32_t m = len - p;
    if (m > n - i) m = n - i;
    float *line = buf + p;
    float *xs = x + i;
    for (uint32_t j = 0; j < m; j++) {
      const float in = xs[j], buf_out = line[j];
      xs[j] = -in + buf_out;
      line[j] = omx_flush(in + buf_out * 0.5f);
    }
    i += m; p += m;
    if (p >= len) p = 0u;
  }
  *pos = p;
  OMX_POST(*pos < len, "position-inside-line");
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "reverb/diffuser"
/* One Dattorro diffuser (allpass with an explicit coefficient) over `n` samples of `x`, in place. */
static inline void omx_plate_ap_block(float *buf, uint32_t len, uint32_t *pos, float *x, uint32_t n,
                                      float coef) {
  OMX_PRE(buf != NULL && len > 0u, "line-bound");
  OMX_PRE(n <= OMX_REVERB_CHUNK, "chunk-bound");
  OMX_PRE(coef > -1.0f && coef < 1.0f, "coefficient-inside-unit-circle");
  uint32_t p = *pos < len ? *pos : 0u;
  for (uint32_t i = 0; i < n;) {
    uint32_t m = len - p;
    if (m > n - i) m = n - i;
    float *line = buf + p;
    float *xs = x + i;
    for (uint32_t j = 0; j < m; j++) {
      const float in = xs[j];
      const float out = -in * coef + line[j];
      line[j] = omx_flush(in + out * coef);
      xs[j] = out;
    }
    i += m; p += m;
    if (p >= len) p = 0u;
  }
  *pos = p;
  OMX_POST(*pos < len, "position-inside-line");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "reverb/plate-excursion"
/**
 * @brief The plate tank's modulation excursion at the live rate, in samples.
 *
 * `depth_pct / 100 · OMX_REVERB_PLATE_MOD_SAMPLES · sr / OMX_REVERB_PLATE_TUNING_RATE`: the
 * paper's ±8 samples scaled by the same `sr / 29761` the tank lengths are, times the depth field.
 * The read swings over `[len - 2·exc, len]`, so a line too short to hold the swing is given the
 * widest swing it can hold rather than a read that crosses its own write.
 *
 * @param depth_pct `omx_reverb.plate_mod_depth`; negative or NaN is no modulation.
 * @param sr        the live rate, Hz.
 * @param shortest  the shortest modulated line, samples.
 * @return the excursion in samples, in `[0, (shortest - 2) / 2]`.
 */
static inline float omx_reverb_plate_excursion(float depth_pct, float sr, float shortest) {
  OMX_PRE(sr > 0.0f, "rate-positive");
  const float depth = depth_pct > 0.0f ? depth_pct : 0.0f;
  float exc = (depth / 100.0f) * OMX_REVERB_PLATE_MOD_SAMPLES * sr / OMX_REVERB_PLATE_TUNING_RATE;
  if (2.0f * exc + 2.0f > shortest) exc = (shortest - 2.0f) * 0.5f;
  if (exc < 0.0f) exc = 0.0f;
  OMX_POST(exc == 0.0f || 2.0f * exc + 2.0f <= shortest, "swing-inside-line");
  return exc;
}
#undef OMX_CONTRACT_STAGE

/*
 * One Dattorro allpass whose DELAY is modulated, read with LINEAR interpolation.
 *
 * Linear and not allpass interpolation, deliberately: an allpass interpolator carries STATE whose
 * coefficient moves with the delay, and a moving coefficient over a stateful filter is exactly
 * where a modulated tank finds its transients — the thing this modulation exists to avoid. Linear
 * is stateless, so the only price is a gentle, slowly-varying HF loss (a comb notch at 1/2T that
 * moves at 1 Hz), which in a diffuser's feedback is indistinguishable from the damping already
 * there. `test/fx/reverb_math.test.c`'s zipper arm measures the result against the signal's own step.
 *
 * `delay` is in samples and must lie in [1, len]: the caller derives it from the line's own
 * length minus twice the excursion, so the read can never cross the write.
 */
static inline float omx_plate_ap_mod(float *buf, uint32_t len, uint32_t *pos, float in, float coef,
                                     float delay) {
  uint32_t i0 = (uint32_t)delay;
  float frac = delay - (float)i0;
  uint32_t k0 = *pos + len - i0;
  if (k0 >= len) k0 -= len;
  uint32_t k1 = (k0 == 0u) ? len - 1u : k0 - 1u;
  float buf_out = buf[k0] + frac * (buf[k1] - buf[k0]);
  float out = -in * coef + buf_out;
  buf[*pos] = omx_flush(in + out * coef);
  *pos = (*pos + 1 >= len) ? 0 : *pos + 1; /* >=: a stale pos past a re-cut line wraps (F7) */
  return out;
}

/* A modulated READ off a plain delay line, linear-interpolated, same rule as the allpass above:
 * `delay` in [1, len]. The line's own write is the caller's, unmoved. */
static inline float omx_delay_read_mod(const float *buf, uint32_t len, uint32_t pos, float delay) {
  uint32_t i0 = (uint32_t)delay;
  float frac = delay - (float)i0;
  uint32_t k0 = pos + len - i0;
  if (k0 >= len) k0 -= len;
  uint32_t k1 = (k0 == 0u) ? len - 1u : k0 - 1u;
  return buf[k0] + frac * (buf[k1] - buf[k0]);
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "reverb/leg"
/* ONE LEG OF THE FREEVERB NETWORK over a chunk: eight parallel combs summed into `wet`, then four
 * series allpasses, then the Freeverb output scaling. ROOM and HALL are this word over their own
 * lines (REVERSE and GATED are ROOM's), each leg called once — the two legs share no state, so
 * running L's whole network before R's is the same arithmetic as interleaving them. */
static inline void omx_fv_leg_block(float *const *comb, const uint32_t *comb_len, uint32_t *comb_pos,
                                    float *comb_damp, float *const *ap, const uint32_t *ap_len,
                                    uint32_t *ap_pos, const float *fed, float *wet, uint32_t n,
                                    float feedback, float damp_pole) {
  OMX_PRE(n <= OMX_REVERB_CHUNK, "chunk-bound");
  OMX_PRE(omx_block_finite(fed, n), "finite-in");
  omx_fv_comb_bank_block(comb, comb_len, comb_pos, comb_damp, fed, wet, n, feedback, damp_pole);
  for (int a = 0; a < OMX_FV_NALLPASS; a++) omx_fv_allpass_block(ap[a], ap_len[a], &ap_pos[a], wet, n);
  for (uint32_t i = 0; i < n; i++) wet[i] *= 0.015f; /* Freeverb output scaling */
  OMX_POST(omx_block_finite(wet, n), "finite-out");
}

#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "reverb"
static inline void omx_reverb_process(float *l, float *r, uint32_t n,
                                      const struct omx_reverb *p, struct omx_reverb_state *s, float sr) {
  /* CONTRACT (omx_contract.h). A reverb is a bank of RECURSIVE networks — combs with feedback,
   * allpasses, a predelay line — so its two dangers are both about the recursion: it must not
   * grow without bound, and its tail must not sit in the denormal range costing microcode traps
   * for minutes after the last note. The bound is the caller's: `size`, `damping` and `mix` are
   * unit-range controls and the comb feedback is derived from them. The rate is a PRECONDITION
   * because every line length here is `ms -> samples` at `sr`: a stage handed a rate it was not
   * designed against silently changes its own room.
   *
   * The tail's decay (monotone RMS after the onset) and the denormal law are properties of a
   * SEQUENCE of blocks and belong to the contract battery and `rt_denormal.test.c`. */
  OMX_PRE(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-in");
  OMX_PRE(OMX_RATE_IS_DECLARED(sr), "rate-is-declared");
  /* AND THE RATE MUST BE THE ONE THE LINES WERE CUT AT. The layout turns every length from ms
   * into samples once, on insert; the predelay, the mirror window and the gate's hold are turned
   * again on every block, from the rate the caller passes. Hand those two different numbers and
   * the operator gets a room of one size wearing another's times, with nothing said: laid out at
   * 48 kHz and run at 96, the first wet sample arrives at 12.656 ms where the 96 kHz layout puts
   * it at 25.3 (2026-09-17-reverb-math-review.md R-5). The caller's job is to lay out at the
   * graph's rate and re-lay out when it moves — and between a live rate change and that
   * re-layout (or while one is in progress, `s->sr == 0`) this stage is an ANNOUNCED
   * PASSTHROUGH, the guard below, never the other rate's room. That window is an expected state
   * of a running console, not a caller's error, so it is a guard and not a precondition
   * (2026-09-25-native-fx-rt-review.md F1). */
  OMX_PRE(p->width >= 0.0f && p->width <= 1.0f, "width-in-unit-range");
  OMX_PRE(p->size >= 0.0f && p->size <= 1.0f, "size-in-unit-range");
  OMX_PRE(p->damping >= 0.0f && p->damping <= 1.0f, "damping-in-unit-range");
  OMX_PRE(p->mix >= 0.0f && p->mix <= 1.0f, "mix-in-unit-range");
  OMX_PRE(p->predelay_ms >= 0.0f, "predelay-not-negative");
  if (!p->enabled || n == 0 || s->_pool == NULL) return;
  if (s->sr != sr) return; /* laid out at another rate, or being re-laid out: passthrough */
  float mix = omx_unit(p->mix);
  if (mix == 0.0f) return; /* bit-identical dry */
  float dry = 1.0f - mix;
  float size01 = omx_unit(p->size);
  float feedback = 0.28f + 0.7f * size01; /* room comb feedback */
  /* HALL comb feedback: a much higher floor (0.90) than room's (0.28), and a narrower span up to
   * the same 0.99 ceiling plate uses. A real hall never reads as small — even `size`=0 should decay
   * far longer than a small room, and stay close to that character across the knob's range, unlike
   * room which spans floor-to-ceiling. Combined with HALL's longer comb lines
   * (omx_reverb_state_layout), this is the "longer decay tail" that outlasts PLATE at the same
   * `size` — measured in the "hall tail outlasts plate" golden in test/fx/reverb.test.c, not assumed:
   * a comb network's late-time decay is set by its longest line's loop gain, and matching or beating
   * Dattorro's much longer single tank loop needed a feedback floor this high, found by measurement
   * (see the PR that added HALL for the search — a straight `feedback = a + b*size` guess undershot
   * badly, twice). */
  float hall_feedback = 0.90f + 0.09f * size01;
  float damping = omx_unit(p->damping);
  /* The two damping poles, derived once per block at the live rate (see
   * omx_reverb_damp_pole): the knob is quoted at the 96 kHz reference and the filter it names is
   * the same filter at every clock. */
  float damp_pole = omx_reverb_damp_pole(damping, sr);
  /* HALL damping floor: even at damping=0 the feedback loop still absorbs 35% of the comb's high
   * end (vs. room's 0%, which is fully bright at damping=0). A real hall's air + distant surfaces
   * darken the tail regardless of how the operator sets the knob; this is the "darker high end
   * than plate" property (plate applies `damping` with no floor, same as room). */
  float hall_damping = damping * 0.6f + 0.35f;
  float hall_damp_pole = omx_reverb_damp_pole(hall_damping, sr);
  float width = omx_unit(p->width);
  /* Clamped in FLOAT before the cast: a negative/NaN/huge ms is UB as a uint32 conversion
   * (omx_fxdelay_ms_to_samples's rule; 2026-09-25-native-fx-rt-review.md F14). */
  const float pre_f = (p->predelay_ms * 0.001f) * sr;
  uint32_t pre = pre_f > 0.0f ? (pre_f < (float)s->pre_cap ? (uint32_t)pre_f : s->pre_cap) : 0u;
  if (pre >= s->pre_cap) pre = s->pre_cap - 1;
  /* The wet path's two cuts, as POLES — the same word the damping filter is said in, made by the
   * tier's one constructor (`omx_pole_from_cutoff_hz`, omx_onepole.h). These corners are set in Hz, so
   * they are RATE-FREE and convert at the live rate: R-058's reference-rate exponent is for the
   * UNIT-RANGE damping knob and applies to it alone. A corner of 0 Hz or at/above Nyquist is not a
   * filter, so the branch is off rather than a pole of 0. */
  int hc_on = (p->highcut > 0.0f && p->highcut < sr * 0.5f);
  float hc_pole = hc_on ? omx_pole_from_cutoff_hz(p->highcut, sr) : 0.0f;
  int lc_on = (p->lowcut > 0.0f && p->lowcut < sr * 0.5f);
  float lc_pole = lc_on ? omx_pole_from_cutoff_hz(p->lowcut, sr) : 0.0f;

  /* ---- REVERSE's mirror geometry, once per block ------------------------------------------
   * The network's wet is captured into a ring; each COMPLETED window of W samples is played
   * BACKWARD, by two readers a half-window (H = W/2) apart with a triangular cross-fade that sums
   * to unity at every sample (Bartlett, 50 % overlap-add), so no window boundary can click.
   * A window change zippers the tail, exactly as a `size` change does - documented, not hidden. */
  uint32_t rev_h = 0; float rev_inv_h = 0.0f;
  if (p->algorithm == OMX_REVERB_REVERSE) {
    float ms = p->reverse_ms;
    if (ms < (float)OMX_REVERB_REVERSE_MIN_MS) ms = (float)OMX_REVERB_REVERSE_MIN_MS;
    if (ms > (float)OMX_REVERB_REVERSE_MAX_MS) ms = (float)OMX_REVERB_REVERSE_MAX_MS;
    uint32_t w = (uint32_t)((ms * 0.001f) * sr);
    if (w < 2u) w = 2u;
    if (2u * w + 2u > s->rev_cap) w = (s->rev_cap - 2u) / 2u; /* a shorter-rate layout, never a read past the ring */
    rev_h = w / 2u;
    if (rev_h < 1u) rev_h = 1u;
    rev_inv_h = 1.0f / (float)rev_h;
    if (s->rev_phase >= rev_h) s->rev_phase = 0; /* the window moved under us */
  }

  /* ---- the plate tank's modulation, once per block ----------------------------------------- */
  float plate_exc = 0.0f, plate_mod_inc = 0.0f;
  if (p->algorithm == OMX_REVERB_PLATE) {
    const float shortest = (float)(s->plate_tank_len[0] < s->plate_tank_len[2] ? s->plate_tank_len[0]
                                                                              : s->plate_tank_len[2]);
    plate_exc = omx_reverb_plate_excursion(p->plate_mod_depth, sr, shortest);
    plate_mod_inc = OMX_REVERB_PLATE_MOD_HZ / sr;
  }

  /* ---- GATED's hold-and-cut, once per block ------------------------------------------------ */
  float gate_thresh = 0.0f, gate_rel_step = 1.0f, gate_att_step = 1.0f;
  uint32_t gate_hold_samples = 0;
  if (p->algorithm == OMX_REVERB_GATED) {
    gate_thresh = omx_db_to_lin(p->gate_threshold_db);
    float hold = p->hold_ms < 0.0f ? 0.0f : p->hold_ms;
    const float hold_f = (hold * 0.001f) * sr; /* NaN or huge -> clamped before the cast (F14) */
    gate_hold_samples = hold_f > 0.0f ? (hold_f < 4.0e9f ? (uint32_t)hold_f : 4000000000u) : 0u;
    float rel = p->release_ms < 0.01f ? 0.01f : p->release_ms;
    gate_rel_step = 1.0f / ((rel * 0.001f) * sr);
    gate_att_step = 1.0f / ((OMX_REVERB_GATE_ATTACK_MS * 0.001f) * sr);
  }

  /* ---- the sample loop, a CHUNK at a time --------------------------------------------------
   * Per-block values above; per-chunk the network runs line by line (omx_fv_leg_block,
   * omx_plate_ap_block), and per-sample only what is a recursion across the whole network (the
   * plate's cross-coupled tank) or an output stage. Every state word the per-sample stages touch is
   * held in a local for the chunk and written back once. */
  const int algo = p->algorithm;
  const float plate_decay = OMX_REVERB_PLATE_DECAY_FLOOR + OMX_REVERB_PLATE_DECAY_SPAN * size01;
  float xin[OMX_REVERB_CHUNK], fed[OMX_REVERB_CHUNK], wl[OMX_REVERB_CHUNK], wr[OMX_REVERB_CHUNK];
  for (uint32_t off = 0; off < n; off += OMX_REVERB_CHUNK) {
    const uint32_t k = (n - off < OMX_REVERB_CHUNK) ? n - off : OMX_REVERB_CHUNK;
    float *lb = l + off, *rb = r + off;
    /* mono feed into the reverb (every configuration), through the pre-delay */
    uint32_t pre_pos = s->pre_pos;
    for (uint32_t i = 0; i < k; i++) {
      xin[i] = 0.5f * (lb[i] + rb[i]);
      uint32_t rp = omx_lookahead_back(pre_pos, pre, s->pre_cap);
      s->pre[pre_pos] = xin[i];
      fed[i] = s->pre[rp];
      pre_pos = omx_lookahead_fwd(pre_pos, 1u, s->pre_cap);
    }
    s->pre_pos = pre_pos;

    if (algo == OMX_REVERB_PLATE) {
      /* Dattorro: 4 series input diffusers, then a figure-8 tank with damping + decay. */
      omx_plate_ap_block(s->plate_diff[0], s->plate_diff_len[0], &s->plate_diff_pos[0], fed, k, 0.75f);
      omx_plate_ap_block(s->plate_diff[1], s->plate_diff_len[1], &s->plate_diff_pos[1], fed, k, 0.75f);
      omx_plate_ap_block(s->plate_diff[2], s->plate_diff_len[2], &s->plate_diff_pos[2], fed, k, 0.625f);
      omx_plate_ap_block(s->plate_diff[3], s->plate_diff_len[3], &s->plate_diff_pos[3], fed, k, 0.625f);
      /* two tank halves cross-coupled */
      float *tank1 = s->plate_tank[1], *tank3 = s->plate_tank[3];
      const uint32_t len1 = s->plate_tank_len[1], len3 = s->plate_tank_len[3];
      uint32_t pos0 = s->plate_tank_pos[0], pos1 = s->plate_tank_pos[1];
      uint32_t pos2 = s->plate_tank_pos[2], pos3 = s->plate_tank_pos[3];
      float phase = s->plate_mod_phase, damp1 = s->plate_damp1, damp2 = s->plate_damp2;
      const float base0 = (float)s->plate_tank_len[0] - plate_exc, base1 = (float)s->plate_tank_len[2] - plate_exc;
      const float base2 = (float)len1 - plate_exc, base3 = (float)len3 - plate_exc;
      for (uint32_t i = 0; i < k; i++) {
        const float x = fed[i];
        /* The two halves' LFOs run in quadrature off one phase, so neither leg's tank can settle
         * into a period and the pair cannot beat against each other. */
        float ph2 = phase + 0.25f;
        if (ph2 >= 1.0f) ph2 -= 1.0f;
        float mod0 = base0 - plate_exc * omx_lfo_shape(phase);
        float mod1 = base1 - plate_exc * omx_lfo_shape(ph2);
        float ph3 = phase + 0.5f; if (ph3 >= 1.0f) ph3 -= 1.0f;
        float ph4 = phase + 0.75f; if (ph4 >= 1.0f) ph4 -= 1.0f;
        float mod2 = base2 - plate_exc * omx_lfo_shape(ph3);
        float mod3 = base3 - plate_exc * omx_lfo_shape(ph4);
        phase += plate_mod_inc;
        if (phase >= 1.0f) phase -= 1.0f;
        float t0 = omx_delay_read_mod(tank1, len1, pos1, mod2);
        float d0 = omx_onepole_flush(&damp1, t0, damp_pole);
        float a = omx_plate_ap_mod(s->plate_tank[0], s->plate_tank_len[0], &pos0, x + d0 * plate_decay, 0.7f, mod0);
        tank1[pos1] = omx_flush(a);
        pos1 = (pos1 + 1 == len1) ? 0 : pos1 + 1;
        float t1 = omx_delay_read_mod(tank3, len3, pos3, mod3);
        float d1 = omx_onepole_flush(&damp2, t1, damp_pole);
        float b = omx_plate_ap_mod(s->plate_tank[2], s->plate_tank_len[2], &pos2, x + d1 * plate_decay, 0.7f, mod1);
        tank3[pos3] = omx_flush(b);
        pos3 = (pos3 + 1 == len3) ? 0 : pos3 + 1;
        wl[i] = a; wr[i] = b;
      }
      s->plate_tank_pos[0] = pos0; s->plate_tank_pos[1] = pos1;
      s->plate_tank_pos[2] = pos2; s->plate_tank_pos[3] = pos3;
      s->plate_mod_phase = phase; s->plate_damp1 = damp1; s->plate_damp2 = damp2;
    } else if (algo == OMX_REVERB_HALL) {
      /* Same Freeverb leg as ROOM below over HALL's own lines, feedback and damping — see
       * omx_reverb_state_layout and the hall_feedback/hall_damping comments above. */
      omx_fv_leg_block(s->comb_l_hall, s->comb_len_l_hall, s->comb_pos_l_hall, s->comb_damp_l_hall, s->ap_l_hall,
                       s->ap_len_l_hall, s->ap_pos_l_hall, fed, wl, k, hall_feedback, hall_damp_pole);
      omx_fv_leg_block(s->comb_r_hall, s->comb_len_r_hall, s->comb_pos_r_hall, s->comb_damp_r_hall, s->ap_r_hall,
                       s->ap_len_r_hall, s->ap_pos_r_hall, fed, wr, k, hall_feedback, hall_damp_pole);
    } else {
      /* Freeverb: 8 parallel combs summed, then 4 series allpasses, per channel. */
      omx_fv_leg_block(s->comb_l, s->comb_len_l, s->comb_pos_l, s->comb_damp_l, s->ap_l, s->ap_len_l, s->ap_pos_l,
                       fed, wl, k, feedback, damp_pole);
      omx_fv_leg_block(s->comb_r, s->comb_len_r, s->comb_pos_r, s->comb_damp_r, s->ap_r, s->ap_len_r, s->ap_pos_r,
                       fed, wr, k, feedback, damp_pole);
    }

    /* ---- the output stages, per sample, on locals ----------------------------------------- */
    uint32_t rev_w = s->rev_w, rev_phase = s->rev_phase;
    float gate_env = s->gate_env;
    uint32_t gate_hold_left = s->gate_hold_left;
    for (uint32_t i = 0; i < k; i++) {
      float wetL = wl[i], wetR = wr[i];
      if (algo == OMX_REVERB_REVERSE) {
        /*
         * Capture, then read the two completed windows BACKWARD.
         *
         * At write index `n` and hop position `phi`, the younger reader sits at `n - 2phi - 1` and
         * the older one a full window behind it; the younger's weight is `phi/H` (fading in) and
         * the older's is `1 - phi/H` (fading out), so the pair sums to 1. Both indices walk
         * BACKWARD one sample per sample and jump forward at each hop boundary - which is what
         * makes the output the input reversed. The dry is NOT delayed: a live strip cannot carry
         * 2 x reverse_ms of latency to make a swell precede its hit, so the swell lands in the two
         * windows AFTER it (spec Amendment 2026-09-17 SSB).
         */
        s->rev_l[rev_w] = omx_flush(wetL);
        s->rev_r[rev_w] = omx_flush(wetR);
        uint32_t off_a = 1u + 2u * rev_phase;
        uint32_t off_b = off_a + 2u * rev_h;
        uint32_t ia = omx_lookahead_back(rev_w, off_a, s->rev_cap);
        uint32_t ib = omx_lookahead_back(rev_w, off_b, s->rev_cap);
        float wa = (float)rev_phase * rev_inv_h;
        wetL = wa * s->rev_l[ia] + (1.0f - wa) * s->rev_l[ib];
        wetR = wa * s->rev_r[ia] + (1.0f - wa) * s->rev_r[ib];
        rev_w = omx_lookahead_fwd(rev_w, 1u, s->rev_cap);
        rev_phase = (rev_phase + 1u == rev_h) ? 0u : rev_phase + 1u;
      }

      /* stereo width: blend the two wet legs toward mono as width->0 */
      float mid = 0.5f * (wetL + wetR), side = 0.5f * (wetL - wetR) * width;
      wetL = mid + side; wetR = mid - side;
      /* wet-path high-cut (one-pole LP) */
      if (hc_on) { wetL = omx_onepole_flush(&s->hc_l, wetL, hc_pole); wetR = omx_onepole_flush(&s->hc_r, wetR, hc_pole); }
      /* wet-path low-cut (one-pole high-pass = signal minus its low-pass) */
      if (lc_on) {
        wetL = omx_flush(wetL - omx_onepole_flush(&s->lc_l, wetL, lc_pole));
        wetR = omx_flush(wetR - omx_onepole_flush(&s->lc_r, wetR, lc_pole));
      }
      if (algo == OMX_REVERB_GATED) {
        /*
         * The gate is the LAST thing in the wet chain - a gated reverb IS reverb into a gate - so
         * the gated wet is exactly `env(t) x` the wet ROOM would have produced from the same
         * input, which is what makes the envelope measurable sample by sample against a ROOM
         * reference. The hold is armed by an explicit comparison of the kernel's own mono feed
         * against `gate_threshold_db`; when the key falls away the envelope holds for `hold_ms`
         * and then ramps LINEARLY to zero over `release_ms`.
         */
        float key = fabsf(xin[i]);
        if (key >= gate_thresh) {
          /* The ARM opens over OMX_REVERB_GATE_ATTACK_MS. The hold branch keeps climbing rather
           * than snapping to 1, because a key that falls away one sample after arming would
           * otherwise put the step back exactly where the ruling took it from. */
          gate_env += gate_att_step;
          if (gate_env > 1.0f) gate_env = 1.0f;
          gate_hold_left = gate_hold_samples;
        } else if (gate_hold_left > 0u) {
          gate_hold_left--;
          gate_env += gate_att_step;
          if (gate_env > 1.0f) gate_env = 1.0f;
        } else {
          gate_env -= gate_rel_step;
          if (gate_env < 0.0f) gate_env = 0.0f;
        }
        wetL *= gate_env; wetR *= gate_env;
      }
      lb[i] = dry * lb[i] + mix * wetL;
      rb[i] = dry * rb[i] + mix * wetR;
    }
    s->rev_w = rev_w; s->rev_phase = rev_phase;
    s->gate_env = gate_env; s->gate_hold_left = gate_hold_left;
  }
  OMX_POST(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-out");
  OMX_POST(s->gate_env >= 0.0f && s->gate_env <= 1.0f, "gate-envelope-in-unit-range");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_REVERB_H */
