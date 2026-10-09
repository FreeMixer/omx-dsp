/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_drive_instance.h (formerly mix_drive_lv2.h) — the DRIVE stage as a live insert for other
 * hosts: the shell's core, with no format in it.
 *
 * Plan: docs/design/notes/2026-09-23-omx-lv2-pack-scope.md §7 lane A, under the HRP shells
 * rule (docs/design/specs/2026-07-16-recording-vsc.md §0.5: one C core, several shells — what
 * differs is who calls it) and the drive stage's own law
 * (docs/design/specs/2026-09-14-native-drive-stage.md §4). §3 of that spec rules drive NATIVE
 * inside the console; this shell does not reopen that — it is the same kernel offered to a
 * host that is not openmixer.
 *
 * ## The face: init, resolve, run, latency
 *
 * Everything below is plain C over `omx_drive.h` and knows nothing about a plugin format. Its shape
 * is every instance face's: {@link omx_drive_instance_init} binds an instance to a declared rate,
 * {@link omx_drive_instance_resolve} takes the drive kernel's contract controls in their declared
 * order and user units (`amount` dB, `character`, `band_freq` Hz, `mix` %, `trim` dB, then the
 * `curve` and `band` choice indices, the `auto_gain` and `stereo_link` switches and the
 * `hf_rolloff` corner in Hz), {@link omx_drive_instance_run} runs the kernel and
 * {@link omx_drive_instance_latency} publishes its latency. A plugin binding generated from the
 * contract calls them with no hand-written map.
 *
 * ## What the face holds that the contract does not declare
 *
 * The oversampling factor is no contract control of the drive kernel (omx-contract 2.1.0): the face
 * runs the oversampler's largest, {@link OMX_DRIVE_INSTANCE_FACTOR}, and builds the state for it
 * once, at init. Every other word of the row is a control (omx-contract 2.2.0).
 *
 * ## ZERO DUPLICATED DSP
 *
 * {@link omx_drive_instance_run} copies the host's input to its output and calls
 * `omx_drive_process` — the SAME inline the console's lane walk calls (mix_lane.h) — on it.
 * There is no second waveshaper, no second oversampler, no second auto-gain. What this file
 * owns is the RESOLVER: the console builds its `struct omx_drive` from word-atomic controls
 * plus TypeScript-designed coefficients (mixer_rt.c's resolve_fx_drive over console-rig.ts's
 * driveStateToNativeDrive); a foreign host hands us seven values and a rate, so
 * {@link omx_drive_instance_resolve} does the same job from those, with the coefficient design in C
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
 * ## LATENCY IS PUBLISHED, and it is the ELEMENT's number
 *
 * {@link omx_drive_instance_latency} is `omx_drive_latency`: the shared oversampler's round-trip
 * at the face's factor (72 base samples at 4x — drive-stage spec §3b, "declared by the element,
 * derived by everyone else"), and 0 while bypassed, exactly as the console's `latencySamples`
 * row derives it. A plugin publishes it every cycle, so a host can compensate the dry paths
 * beside it.
 *
 * ## STEREO, 2 in / 2 out
 *
 * The kernel is a stereo pair with an optional right leg, and `stereoLink` (§4e: ONE auto-gain
 * path from both legs' summed energy) is a fact about a PAIR. A mono plugin instantiated twice
 * by a host cannot link, so the plugin is 2x2 and a host racking it on a mono strip feeds one
 * leg; the other runs on silence at no audible cost.
 */
#ifndef OMX_DRIVE_INSTANCE_H
#define OMX_DRIVE_INSTANCE_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/fx/omx_drive_design.h>
#include <omxcontract/omx_contract_limits.h> /* the drive kernel's controls: every travel below */
#include <omxdsp/omx_param.h>

#define OMX_DRIVE_INSTANCE_CHANNELS 2

/** The oversampling factor the face runs the shaper at: the shared oversampler's largest. A
 * constant of the face, not a contract control. */
#define OMX_DRIVE_INSTANCE_FACTOR ((int)OMX_OVS_MAX_FACTOR)
/** What the face designs the HF roll-off section at with the roll-off off (console-rig.ts:
 * `hfHz = ... ? 20000 : ...`). */
#define OMX_DRIVE_INSTANCE_HF_OFF_DESIGN_HZ 20000.0f

/** Control defaults: the contract's (DRIVE_*_RANGE.default); a choice comes up at its first id. */
#define OMX_DRIVE_INSTANCE_AMOUNT_DEFAULT ((float)OMX_DRIVE_AMOUNT_RANGE_DEFAULT)
#define OMX_DRIVE_INSTANCE_CHARACTER_DEFAULT ((float)OMX_DRIVE_CHARACTER_RANGE_DEFAULT)
#define OMX_DRIVE_INSTANCE_BAND_FREQ_DEFAULT ((float)OMX_DRIVE_BAND_FREQ_RANGE_DEFAULT)
#define OMX_DRIVE_INSTANCE_MIX_DEFAULT ((float)OMX_DRIVE_MIX_RANGE_DEFAULT)
#define OMX_DRIVE_INSTANCE_TRIM_DEFAULT ((float)OMX_DRIVE_TRIM_RANGE_DEFAULT)
#define OMX_DRIVE_INSTANCE_CURVE_DEFAULT OMX_DRIVE_SOFT
#define OMX_DRIVE_INSTANCE_BAND_DEFAULT OMX_DRIVE_BAND_FULL
#define OMX_DRIVE_INSTANCE_AUTO_GAIN_DEFAULT ((int)OMX_DRIVE_AUTO_GAINS_DEFAULT)
#define OMX_DRIVE_INSTANCE_STEREO_LINK_DEFAULT ((int)OMX_DRIVE_STEREO_LINKS_DEFAULT)
#define OMX_DRIVE_INSTANCE_HF_ROLLOFF_DEFAULT ((int)OMX_DRIVE_HF_ROLLOFFS_DEFAULT)

/** One instance: the atom, the kernel's state (inline: the oversampler histories and the
 * compensation line), and what the coefficient bank was last designed for. Nothing is caller-owned. */
typedef struct {
  uint32_t rate;
  struct omx_drive atom;
  struct omx_drive_state state;
  /* What the coefficient bank was last designed for, so resolve() redesigns only on a change. */
  float designed_band_hz;
  float designed_hf_hz;
  int ready;
} OmxDriveInstance;

/**
 * Resolve the host's control values into the atom for this cycle. `bypass` non-zero disables the
 * atom. The arguments are the drive kernel's contract controls, in their declared order and user
 * units: `amount` (dB), `character` (-1..+1), `band_freq` (Hz), `mix` (percent) and `trim` (dB)
 * are clamped into their declared travels, a non-finite word reading as the declared default;
 * `curve` and `band` are the DRIVE_CURVES and DRIVE_BANDS indices (`enum omx_drive_curve`,
 * `enum omx_drive_band`), any other value reading as the first; `auto_gain` and `stereo_link` are
 * the DRIVE_AUTO_GAINS and DRIVE_STEREO_LINKS indices (0 off, 1 on), any other value reading as the
 * declared default; `hf_rolloff` is a DRIVE_HF_ROLLOFFS id, the corner in Hz (0 none, 12000,
 * 16000), any other value reading as the declared default: a corner the low-pass design cannot
 * take is never designed. The clamps are the row's, applied
 * for the same reason the console's controller applies them: the kernel's contract requires mix in
 * [0,1], and a host is free to write anything to a port.
 *
 * Coefficients are redesigned only when `band_freq` or the roll-off corner moved. Allocation-free.
 */
#define OMX_CONTRACT_STAGE "drive-instance/resolve"
static inline void omx_drive_instance_resolve(OmxDriveInstance *s, int bypass, float amount, float character,
                                              float band_freq, float mix, float trim, int curve, int band,
                                              int auto_gain, int stereo_link, int hf_rolloff) {
  if (!s || !s->ready) return;
  /* CONTRACT (omx_contract.h). Whatever a host wrote to the ports, the atom leaves here inside
   * omx_drive_process's own PRE: mix in [0,1], the factor the state is built for, the bias weight
   * in [0,1], and the two gains finite and positive. */
  struct omx_drive *o = &s->atom;
  o->enabled = bypass ? 0 : 1;
  o->curve = curve >= OMX_DRIVE_SOFT && curve <= OMX_DRIVE_EXCITER ? curve : OMX_DRIVE_INSTANCE_CURVE_DEFAULT;
  o->band = band >= OMX_DRIVE_BAND_FULL && band <= OMX_DRIVE_BAND_TILT ? band : OMX_DRIVE_INSTANCE_BAND_DEFAULT;
  o->drive_lin = omx_db_to_lin(omx_clamp_or(amount, (float)OMX_DRIVE_AMOUNT_RANGE_MIN, (float)OMX_DRIVE_AMOUNT_RANGE_MAX,
                                            OMX_DRIVE_INSTANCE_AMOUNT_DEFAULT));
  /* even_w = (character + 1) / 2, the same fold resolve_fx_drive does. */
  o->even_w = 0.5f * (omx_clamp_or(character, (float)OMX_DRIVE_CHARACTER_RANGE_MIN, (float)OMX_DRIVE_CHARACTER_RANGE_MAX,
                                   OMX_DRIVE_INSTANCE_CHARACTER_DEFAULT) +
                      1.0f);
  o->mix = 0.01f * omx_clamp_or(mix, (float)OMX_DRIVE_MIX_RANGE_MIN, (float)OMX_DRIVE_MIX_RANGE_MAX,
                                OMX_DRIVE_INSTANCE_MIX_DEFAULT);
  o->trim_lin = omx_db_to_lin(omx_clamp_or(trim, (float)OMX_DRIVE_TRIM_RANGE_MIN, (float)OMX_DRIVE_TRIM_RANGE_MAX,
                                           OMX_DRIVE_INSTANCE_TRIM_DEFAULT));
  o->auto_gain = auto_gain == 0 || auto_gain == 1 ? auto_gain : OMX_DRIVE_INSTANCE_AUTO_GAIN_DEFAULT;
  o->stereo_link = stereo_link == 0 || stereo_link == 1 ? stereo_link : OMX_DRIVE_INSTANCE_STEREO_LINK_DEFAULT;
  const int hf = hf_rolloff == OMX_DRIVE_HF_ROLLOFFS_0 || hf_rolloff == OMX_DRIVE_HF_ROLLOFFS_12000 ||
                         hf_rolloff == OMX_DRIVE_HF_ROLLOFFS_16000
                     ? hf_rolloff
                     : OMX_DRIVE_INSTANCE_HF_ROLLOFF_DEFAULT;
  o->hf_on = hf > 0 ? 1 : 0;
  const float hf_hz = hf > 0 ? (float)hf : OMX_DRIVE_INSTANCE_HF_OFF_DESIGN_HZ;
  const float band_hz = omx_clamp_or(band_freq, (float)OMX_DRIVE_BAND_FREQ_RANGE_MIN,
                                     (float)OMX_DRIVE_BAND_FREQ_RANGE_MAX, OMX_DRIVE_INSTANCE_BAND_FREQ_DEFAULT);
  if (band_hz != s->designed_band_hz || hf_hz != s->designed_hf_hz) {
    omx_drive_design_bank(o, band_hz, hf_hz, s->rate);
    s->designed_band_hz = band_hz;
    s->designed_hf_hz = hf_hz;
  }
  OMX_POST(o->mix >= 0.0f && o->mix <= 1.0f, "mix-in-unit-range");
  OMX_POST(o->even_w >= 0.0f && o->even_w <= 1.0f, "bias-weight-in-unit-range");
  OMX_POST(s->state.factor == o->os_factor, "state-built-for-the-factor");
  OMX_POST(o->drive_lin >= 1.0f && o->drive_lin - o->drive_lin == 0.0f, "drive-gain-finite-and-at-least-unity");
  OMX_POST(o->trim_lin > 0.0f && o->trim_lin - o->trim_lin == 0.0f, "trim-gain-finite-and-positive");
}
#undef OMX_CONTRACT_STAGE

/**
 * Bind an instance to its rate: the time constants, the state built for the face's factor, the
 * contract's defaults resolved and the bank designed, engaged. Returns 1 when usable, 0 when not: a
 * rate the console does not declare is refused, leaving `ready` clear, and run() is then the
 * identity. Allocates nothing — the caller owns the struct.
 */
static inline int omx_drive_instance_init(OmxDriveInstance *s, float sr) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (!omx_rate_is_declared(sr)) return 0;
  s->rate = (uint32_t)sr;
  omx_drive_time_constants(&s->atom, sr);
  s->atom.os_factor = omx_drive_factor_of(OMX_DRIVE_INSTANCE_FACTOR);
  omx_drive_state_init(&s->state, s->atom.os_factor);
  s->designed_band_hz = -1.0f; /* forces the first design */
  s->designed_hf_hz = -1.0f;
  s->ready = 1;
  omx_drive_instance_resolve(s, 0, OMX_DRIVE_INSTANCE_AMOUNT_DEFAULT, OMX_DRIVE_INSTANCE_CHARACTER_DEFAULT,
                             OMX_DRIVE_INSTANCE_BAND_FREQ_DEFAULT, OMX_DRIVE_INSTANCE_MIX_DEFAULT,
                             OMX_DRIVE_INSTANCE_TRIM_DEFAULT, OMX_DRIVE_INSTANCE_CURVE_DEFAULT,
                             OMX_DRIVE_INSTANCE_BAND_DEFAULT, OMX_DRIVE_INSTANCE_AUTO_GAIN_DEFAULT,
                             OMX_DRIVE_INSTANCE_STEREO_LINK_DEFAULT, OMX_DRIVE_INSTANCE_HF_ROLLOFF_DEFAULT);
  return 1;
}

/**
 * One cycle: `out = drive(in)` for both legs, `n` frames. `in_*` and `out_*` may alias. Not ready,
 * or bypassed, is a copy and nothing else — the kernel's own "zero cost when disengaged" (§4g),
 * reached through the same `enabled` word.
 */
#define OMX_CONTRACT_STAGE "drive-instance/run"
static inline void omx_drive_instance_run(OmxDriveInstance *s, const float *in_l, const float *in_r, float *out_l,
                                          float *out_r, uint32_t n) {
  /* CONTRACT (omx_contract.h). Finite in, finite out, on both legs — the kernel's own law,
   * restated at the host boundary because the copy is this function's. */
  if (!in_l || !in_r || !out_l || !out_r || n == 0u) return;
  OMX_PRE(omx_block_finite(in_l, n) && omx_block_finite(in_r, n), "finite-in");
  if (out_l != in_l) memmove(out_l, in_l, (size_t)n * sizeof(float));
  if (out_r != in_r) memmove(out_r, in_r, (size_t)n * sizeof(float));
  if (!s || !s->ready) return;
  omx_drive_process(out_l, out_r, n, &s->atom, &s->state);
  OMX_POST(omx_block_finite(out_l, n) && omx_block_finite(out_r, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/** The stage's declared latency in frames at the instance rate — the element's, 0 while bypassed
 * or not ready. */
static inline uint32_t omx_drive_instance_latency(const OmxDriveInstance *s) {
  return s && s->ready ? (uint32_t)omx_drive_latency(&s->atom) : 0u;
}

#endif /* OMX_DRIVE_INSTANCE_H */
