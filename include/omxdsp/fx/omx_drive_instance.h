/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_drive_instance.h (formerly mix_drive_lv2.h) — the DRIVE stage as a live insert for other
 * hosts: the shell's core, with no LV2 in it.
 *
 * Plan: docs/design/notes/2026-09-23-omx-lv2-pack-scope.md §7 lane A, under the HRP shells
 * rule (docs/design/specs/2026-07-16-recording-vsc.md §0.5: one C core, several shells — what
 * differs is who calls it) and the drive stage's own law
 * (docs/design/specs/2026-09-14-native-drive-stage.md §4). §3 of that spec rules drive NATIVE
 * inside the console; this shell does not reopen that — it is the same kernel offered to a
 * host that is not openmixer.
 *
 * ## What this file is, and what mix_drive_lv2.c is
 *
 * Everything below is plain C over `mix_drive.h` and knows nothing about LV2 — no handle, no
 * port, no host. Same discipline as hrp_lv2.h: it makes the shell's core reachable by
 * `mix_drive_lv2.test.c` with a plain `cc`, and keeps `mix_drive_lv2.c` down to what it is —
 * a descriptor and seventeen `connect_port` cases.
 *
 * ## ZERO DUPLICATED DSP
 *
 * {@link omx_drive_lv2_run} copies the host's input to its output and calls
 * `omx_drive_process` — the SAME inline the console's lane walk calls (mix_lane.h) — on it.
 * There is no second waveshaper, no second oversampler, no second auto-gain. What this file
 * owns is the RESOLVER: the console builds its `struct omx_drive` from word-atomic controls
 * plus TypeScript-designed coefficients (mixer_rt.c's resolve_fx_drive over console-rig.ts's
 * driveStateToNativeDrive); a foreign host hands us twelve floats and a rate, so
 * {@link omx_drive_lv2_resolve} does the same job from those, with the coefficient design in C
 * (mix_drive_design.h — a pinned twin of the TypeScript, not a second opinion) and the time
 * constants from the kernel's own `omx_drive_time_constants`.
 *
 * ## NO WORKER, and why (the one place this shell is simpler than HRP's)
 *
 * HRP's live shell needs `lv2:worker` because its analysis is an FFT chain that must never run
 * in an audio callback. The drive has no analysis: its `run()` is a resolve (a dozen loads and
 * clamps, a coefficient redesign ONLY when a frequency port moved — a few trig calls, no
 * allocation) and the kernel, which is fixed work per sample with every buffer inline in
 * `struct omx_drive_state`. Nothing here needs a second thread, so none is asked for.
 *
 * ## What `run()` may do to the state, and what it may not
 *
 * The kernel's state is built by `omx_drive_state_init` for ONE oversampling factor and
 * `omx_drive_process` refuses (returns, leaving the audio untouched) when the atom's factor and
 * the state's disagree. In the console the CONTROL thread rebuilds the state on a factor
 * change. In an LV2 host a control port changes on the audio thread, so the rebuild happens
 * there — it is a `memset` and two `omx_oversampler_init`s (no allocation: the oversampler's
 * histories are inline arrays), and a factor switch is a history reset the operator hears as a
 * click at most once per switch. That is the same audible event the console's rebuild produces.
 *
 * ## LATENCY IS PUBLISHED, and it is the ELEMENT's number
 *
 * {@link omx_drive_lv2_latency} is `omx_drive_latency`: the shared oversampler's round-trip
 * (72 base samples at 4x, 48 at 2x, 0 at 1x — drive-stage spec §3b, "declared by the element,
 * derived by everyone else"), and 0 while bypassed, exactly as the console's `latencySamples`
 * row derives it. The plugin's `latency` port carries it every cycle, so a host can compensate
 * the dry paths beside it.
 *
 * ## STEREO, 2 in / 2 out
 *
 * The kernel is a stereo pair with an optional right leg, and `stereoLink` (§4e: ONE auto-gain
 * path from both legs' summed energy) is a fact about a PAIR. A mono plugin instantiated twice
 * by a host cannot link, so the plugin is 2x2 and a host racking it on a mono strip feeds one
 * leg; the other runs on silence at no audible cost.
 */
#ifndef OMX_MIX_DRIVE_LV2_H
#define OMX_MIX_DRIVE_LV2_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/fx/omx_drive_design.h>
#include <omxdsp/omx_port_int.h>
#include <omxdsp/omx_contract_limits.h> /* CORE_LIMITS.drive, generated: every travel below */
#include <omxdsp/omx_param.h>

/** The row's `hfRolloff` enum as the port carries it: 0 = off, else the corner in Hz. */
#define OMX_DRIVE_LV2_HF_OFF 0.0f
/** What `off` designs the roll-off section at (console-rig.ts: `hfHz = ... ? 20000 : ...`). */
#define OMX_DRIVE_LV2_HF_OFF_DESIGN_HZ 20000.0f
/** The oversample port's travel, the factor the stage asks the shared oversampler for: 1 (none)
 * up to its largest, which is also the declared default (omx_drive_factor_of maps the 3 between). */
#define OMX_DRIVE_LV2_FACTOR_MIN 1
#define OMX_DRIVE_LV2_FACTOR_DEFAULT ((int)OMX_OVS_MAX_FACTOR)

/*
 * The twelve control-port values, as floats, in the row's units (drive-stage spec §4h): dB,
 * percent, Hz, and the enums as their integer values. The plugin fills this from its ports
 * every cycle; the test fills it by hand.
 */
struct omx_drive_lv2_ports {
  float bypass;      /* toggled: 1 = the stage is a wire (the row's `on`, inverted) */
  float curve;       /* enum omx_drive_curve, 0..3 */
  float drive_db;    /* 0..36 */
  float character;   /* -1..+1 */
  float band;        /* enum omx_drive_band, 0..3 */
  float band_hz;     /* 20..20000 */
  float mix_pct;     /* 0..100 */
  float trim_db;     /* -24..+12 */
  float auto_gain;   /* toggled */
  float stereo_link; /* toggled */
  float hf_rolloff;  /* 0 | 12000 | 16000 */
  float oversample;  /* 1 | 2 | 4 */
};

/** The row's declared defaults (§4h), with `bypass` = 0: an insert a host racks is ENGAGED, and
 * at 0 dB drive an engaged stage "changes almost nothing", which is what the spec's own default
 * means. (HRP ships bypassed because its corrections are learned; a saturator's are asked for.) */
#define OMX_DRIVE_LV2_PORT_DEFAULTS \
  { 0.0f, (float)OMX_DRIVE_SOFT, OMX_DRIVE_DRIVE_DB_DEFAULT, OMX_DRIVE_CHARACTER_DEFAULT, (float)OMX_DRIVE_BAND_FULL, \
    OMX_DRIVE_BAND_HZ_DEFAULT, OMX_DRIVE_MIX_DEFAULT, OMX_DRIVE_TRIM_DB_DEFAULT, \
    1.0f, 1.0f, OMX_DRIVE_LV2_HF_OFF, (float)OMX_DRIVE_LV2_FACTOR_DEFAULT }

typedef struct {
  uint32_t rate;
  struct omx_drive atom;
  struct omx_drive_state state;
  /* What the coefficient bank was last designed for, so run() redesigns only on a change. */
  float designed_band_hz;
  float designed_hf_hz;
} OmxDriveLv2;

/**
 * Resolve the twelve port values into the atom for this cycle. The clamps are the row's
 * (strip-fx-limits.ts, drive-stage spec §4h), applied here for the same reason the console's
 * controller applies them: the kernel's contract requires mix in [0,1] and a factor in
 * {1,2,4}, and a host is free to write anything to a port.
 *
 * Coefficients are redesigned only when `band_hz` or the roll-off corner moved; the state is
 * rebuilt only when the factor moved. Both are allocation-free (see the header note).
 */
#define OMX_CONTRACT_STAGE "drive-lv2/resolve"
static inline void omx_drive_lv2_resolve(OmxDriveLv2 *c, const struct omx_drive_lv2_ports *p) {
  /* CONTRACT (omx_contract.h). Whatever a host wrote to the ports, the atom leaves here inside
   * omx_drive_process's own PRE: mix in [0,1], the factor one of 1|2|4 with the state built for
   * it, the bias weight in [0,1], and the two gains finite and positive. */
  struct omx_drive *o = &c->atom;
  o->enabled = p->bypass > 0.5f ? 0 : 1;
  /* The integer ports go through omx_port_int(): held inside their travel, every non-finite
   * word the declared default, before the round (dsp-primitives row 23). */
  o->curve = omx_port_int(p->curve, OMX_DRIVE_SOFT, OMX_DRIVE_EXCITER, OMX_DRIVE_SOFT);
  o->band = omx_port_int(p->band, OMX_DRIVE_BAND_FULL, OMX_DRIVE_BAND_TILT, OMX_DRIVE_BAND_FULL);
  o->drive_lin = omx_db_to_lin(omx_clampf(p->drive_db, OMX_DRIVE_DRIVE_DB_MIN, OMX_DRIVE_DRIVE_DB_MAX));
  /* even_w = (character + 1) / 2, the same fold resolve_fx_drive does. */
  o->even_w = 0.5f * (omx_clampf(p->character, OMX_DRIVE_CHARACTER_MIN, OMX_DRIVE_CHARACTER_MAX) + 1.0f);
  o->mix = 0.01f * omx_clampf(p->mix_pct, OMX_DRIVE_MIX_MIN, OMX_DRIVE_MIX_MAX);
  o->trim_lin = omx_db_to_lin(omx_clampf(p->trim_db, OMX_DRIVE_TRIM_DB_MIN, OMX_DRIVE_TRIM_DB_MAX));
  o->auto_gain = p->auto_gain > 0.5f ? 1 : 0;
  o->stereo_link = p->stereo_link > 0.5f ? 1 : 0;
  /* hf_rolloff is an ENUM, {0, 12000, 16000} — the same three members the console's own
   * `hfRolloff` row declares (drive-stage spec §4f, console-rig.ts's `driveStateToNativeDrive`
   * never sends anything else). The TTL says so (`lv2:enumeration`), but a host is free to
   * write any float to a control port, and the console never has to defend against a corner
   * the low-pass design can't take: docs/design/notes/2026-09-24-drive-lv2-hf-rolloff-sub-audio-poles.md
   * found sub-~11 Hz corners narrowing the poles onto the unit circle. Snap to the nearest
   * declared member instead of a plain clamp, so a value below the lowest real corner (12 kHz)
   * lands ON a stable, declared corner rather than in the unstable gap below it. */
  const float hf_in = omx_clamp_or(p->hf_rolloff, OMX_DRIVE_LV2_HF_OFF, 16000.0f, OMX_DRIVE_LV2_HF_OFF);
  const float hf = hf_in <= 0.0f ? 0.0f : (hf_in < 14000.0f ? 12000.0f : 16000.0f);
  o->hf_on = hf > 0.0f ? 1 : 0;
  const float hf_hz = hf > 0.0f ? hf : OMX_DRIVE_LV2_HF_OFF_DESIGN_HZ;
  o->os_factor = omx_drive_factor_of(
      omx_port_int(p->oversample, OMX_DRIVE_LV2_FACTOR_MIN, OMX_DRIVE_LV2_FACTOR_DEFAULT, OMX_DRIVE_LV2_FACTOR_DEFAULT));

  const float band_hz = omx_clampf(p->band_hz, OMX_DRIVE_BAND_HZ_MIN, OMX_DRIVE_BAND_HZ_MAX);
  if (band_hz != c->designed_band_hz || hf_hz != c->designed_hf_hz) {
    omx_drive_design_bank(o, band_hz, hf_hz, c->rate);
    c->designed_band_hz = band_hz;
    c->designed_hf_hz = hf_hz;
  }
  if (c->state.factor != omx_drive_factor_of(o->os_factor)) omx_drive_state_init(&c->state, o->os_factor);
  OMX_POST(o->mix >= 0.0f && o->mix <= 1.0f, "mix-in-unit-range");
  OMX_POST(o->even_w >= 0.0f && o->even_w <= 1.0f, "bias-weight-in-unit-range");
  OMX_POST(o->os_factor == 1 || o->os_factor == 2 || o->os_factor == 4, "factor-is-1-2-or-4");
  OMX_POST(c->state.factor == o->os_factor, "state-built-for-the-factor");
  OMX_POST(o->drive_lin >= 1.0f && o->drive_lin - o->drive_lin == 0.0f, "drive-gain-finite-and-at-least-unity");
  OMX_POST(o->trim_lin > 0.0f && o->trim_lin - o->trim_lin == 0.0f, "trim-gain-finite-and-positive");
}
#undef OMX_CONTRACT_STAGE

/** Build the core for a host rate: defaults resolved, coefficients designed, state built at
 * the default factor. Allocates nothing — the caller owns the struct. */
static inline void omx_drive_lv2_init(OmxDriveLv2 *c, uint32_t rate) {
  memset(c, 0, sizeof(*c));
  c->rate = rate;
  omx_drive_time_constants(&c->atom, (float)rate);
  c->designed_band_hz = -1.0f; /* forces the first design */
  c->designed_hf_hz = -1.0f;
  const struct omx_drive_lv2_ports defaults = OMX_DRIVE_LV2_PORT_DEFAULTS;
  omx_drive_lv2_resolve(c, &defaults);
}

/**
 * One cycle: `out = drive(in)` for both legs, `n` frames. `in_r`/`out_r` may both be NULL for
 * a one-leg use; the kernel then runs its right leg not at all. In-place (`out == in`) is
 * allowed, as LV2 allows it. Bypassed, this is a copy and nothing else — the kernel's own
 * "zero cost when disengaged" (§4g), reached through the same `enabled` word.
 */
#define OMX_CONTRACT_STAGE "drive-lv2/run"
static inline void omx_drive_lv2_run(OmxDriveLv2 *c, const float *in_l, const float *in_r,
                                     float *out_l, float *out_r, uint32_t n) {
  /* CONTRACT (omx_contract.h). Finite in, finite out, on every leg that is connected — the
   * kernel's own law, restated at the host boundary because the copy is this function's. */
  if (n == 0u || !in_l || !out_l) return;
  OMX_PRE(omx_block_finite(in_l, n) && (!in_r || omx_block_finite(in_r, n)), "finite-in");
  if (out_l != in_l) memcpy(out_l, in_l, n * sizeof(float));
  float *r = NULL;
  if (in_r && out_r) {
    if (out_r != in_r) memcpy(out_r, in_r, n * sizeof(float));
    r = out_r;
  }
  omx_drive_process(out_l, r, n, &c->atom, &c->state);
  OMX_POST(omx_block_finite(out_l, n) && (!r || omx_block_finite(r, n)), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/** The stage's declared latency in frames at the host rate — the element's, 0 while bypassed. */
static inline int omx_drive_lv2_latency(const OmxDriveLv2 *c) {
  return omx_drive_latency(&c->atom);
}

#endif /* OMX_MIX_DRIVE_LV2_H */
