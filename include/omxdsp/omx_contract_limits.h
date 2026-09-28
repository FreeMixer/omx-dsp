#ifndef OMX_CONTRACT_LIMITS_H
#define OMX_CONTRACT_LIMITS_H
/*
 * GENERATED — DO NOT EDIT BY HAND.
 * Produced by harness/contract-limits-gen.mjs from packages/core/src/channel-contract.ts's
 * `laws` column (R-056), every table packages/core/src/native-limit-tables.ts's registry
 * holds, packages/core/src/strip-dynamics-limits.ts's PROGRAM_RELEASE profile,
 * packages/core/src/control-bounds.ts's CORE_LIMITS for the pan/delay/reverb/drive
 * facts, packages/core/src/fdelay-kernel.ts's read-kernel l1 norm,
 * packages/core/src/dsp-primitive-limits.ts's all-pass and crossover numbers (not a
 * NativeLimitTable shape — design constants and contract tolerances, hand-imported) and
 * packages/core/src/row-codecs.ts's STANDARD_SAMPLE_RATES. Regenerate:
 * `node harness/contract-limits-gen.mjs`, then commit the result — see this file's own
 * generator for why it is committed rather than gitignored.
 *
 * A C contract (OMX_PRE/OMX_POST/OMX_INVARIANT) reads a limit from HERE, never a literal:
 * packages/server/src/contract-limits-generated-conformance.test.ts refuses both a stale
 * header and a C literal that duplicates a number this file already declares.
 */

/* The closed law vocabulary (core's LAW_KINDS, fact-declaration.ts) — one member per law. */
typedef enum {
  OMX_LAW_NO_GAIN_ADDED = 0,
  OMX_LAW_OUTPUT_LE_INPUT_PLUS,
  OMX_LAW_UNITY_AT_DEFAULT,
  OMX_LAW_DELAYED_COPY,
  OMX_LAW_BYPASS_IDENTITY,
  OMX_LAW_TAIL_DECAYS,
  OMX_LAW_CONSTANT_POWER_PAN,
  OMX_LAW_LINEAR_SUM,
  OMX_LAW_FINITE,
  OMX_LAW_NO_DENORMAL_STATE,
} omx_contract_law_t;

/* Per-stage law lists, one per channel fact whose row declares `laws` — the SAME array
 * harness/contracts-scaffold.mjs's applier and the MCP contract_laws tool read; a stage's
 * OMX_POST tokens implement one or more of these, never a second spelling of the law. */
static const omx_contract_law_t OMX_STAGE_LAWS_PAN[] = { OMX_LAW_FINITE, OMX_LAW_CONSTANT_POWER_PAN };
#define OMX_STAGE_LAWS_PAN_COUNT 2

static const omx_contract_law_t OMX_STAGE_LAWS_EQ[] = { OMX_LAW_FINITE, OMX_LAW_NO_DENORMAL_STATE, OMX_LAW_BYPASS_IDENTITY };
#define OMX_STAGE_LAWS_EQ_COUNT 3

static const omx_contract_law_t OMX_STAGE_LAWS_GATE[] = { OMX_LAW_FINITE, OMX_LAW_NO_DENORMAL_STATE, OMX_LAW_BYPASS_IDENTITY, OMX_LAW_NO_GAIN_ADDED, OMX_LAW_UNITY_AT_DEFAULT };
#define OMX_STAGE_LAWS_GATE_COUNT 5

static const omx_contract_law_t OMX_STAGE_LAWS_DYNAMICS[] = { OMX_LAW_FINITE, OMX_LAW_NO_DENORMAL_STATE, OMX_LAW_BYPASS_IDENTITY, OMX_LAW_OUTPUT_LE_INPUT_PLUS, OMX_LAW_UNITY_AT_DEFAULT };
#define OMX_STAGE_LAWS_DYNAMICS_COUNT 5

static const omx_contract_law_t OMX_STAGE_LAWS_DELAY[] = { OMX_LAW_FINITE, OMX_LAW_BYPASS_IDENTITY, OMX_LAW_DELAYED_COPY };
#define OMX_STAGE_LAWS_DELAY_COUNT 3

static const omx_contract_law_t OMX_STAGE_LAWS_REVERB[] = { OMX_LAW_FINITE, OMX_LAW_NO_DENORMAL_STATE, OMX_LAW_BYPASS_IDENTITY, OMX_LAW_TAIL_DECAYS };
#define OMX_STAGE_LAWS_REVERB_COUNT 4

static const omx_contract_law_t OMX_STAGE_LAWS_DRIVE[] = { OMX_LAW_FINITE, OMX_LAW_BYPASS_IDENTITY, OMX_LAW_UNITY_AT_DEFAULT };
#define OMX_STAGE_LAWS_DRIVE_COUNT 3

static const omx_contract_law_t OMX_STAGE_LAWS_DEESSER[] = { OMX_LAW_FINITE, OMX_LAW_NO_DENORMAL_STATE, OMX_LAW_BYPASS_IDENTITY, OMX_LAW_NO_GAIN_ADDED };
#define OMX_STAGE_LAWS_DEESSER_COUNT 4

static const omx_contract_law_t OMX_STAGE_LAWS_CHORUS[] = { OMX_LAW_FINITE, OMX_LAW_BYPASS_IDENTITY };
#define OMX_STAGE_LAWS_CHORUS_COUNT 2

static const omx_contract_law_t OMX_STAGE_LAWS_FLANGER[] = { OMX_LAW_FINITE, OMX_LAW_NO_DENORMAL_STATE, OMX_LAW_BYPASS_IDENTITY };
#define OMX_STAGE_LAWS_FLANGER_COUNT 3

static const omx_contract_law_t OMX_STAGE_LAWS_TOMAIN[] = { OMX_LAW_FINITE, OMX_LAW_LINEAR_SUM };
#define OMX_STAGE_LAWS_TOMAIN_COUNT 2

/* Per-parameter numeric travel, one block per table registered in
 * packages/core/src/native-limit-tables.ts (GATE/COMP today; a kernel that registers a table
 * lands here because it is REGISTERED, not because this generator names it). */
#define OMX_COMP_THRESHOLD_DB_MIN -60.0f
#define OMX_COMP_THRESHOLD_DB_MAX 0.0f
#define OMX_COMP_THRESHOLD_DB_DEFAULT -18.0f
#define OMX_COMP_RATIO_MIN 1.0f
#define OMX_COMP_RATIO_MAX 20.0f
#define OMX_COMP_RATIO_DEFAULT 4.0f
#define OMX_COMP_KNEE_DB_MIN 0.0f
#define OMX_COMP_KNEE_DB_MAX 24.0f
#define OMX_COMP_KNEE_DB_DEFAULT 6.0f
#define OMX_COMP_ATTACK_MS_MIN 0.1f
#define OMX_COMP_ATTACK_MS_MAX 100.0f
#define OMX_COMP_ATTACK_MS_DEFAULT 5.0f
#define OMX_COMP_RELEASE_MS_MIN 5.0f
#define OMX_COMP_RELEASE_MS_MAX 3000.0f
#define OMX_COMP_RELEASE_MS_DEFAULT 200.0f
#define OMX_COMP_MAKEUP_DB_MIN 0.0f
#define OMX_COMP_MAKEUP_DB_MAX 24.0f
#define OMX_COMP_MAKEUP_DB_DEFAULT 0.0f
#define OMX_COMP_MIX_PCT_MIN 0.0f
#define OMX_COMP_MIX_PCT_MAX 100.0f
#define OMX_COMP_MIX_PCT_DEFAULT 100.0f

#define OMX_GATE_THRESHOLD_DB_MIN -80.0f
#define OMX_GATE_THRESHOLD_DB_MAX 0.0f
#define OMX_GATE_THRESHOLD_DB_DEFAULT -40.0f
#define OMX_GATE_RANGE_DB_MIN -90.0f
#define OMX_GATE_RANGE_DB_MAX 0.0f
#define OMX_GATE_RANGE_DB_DEFAULT -90.0f
#define OMX_GATE_KNEE_START_DB_MIN -80.0f
#define OMX_GATE_KNEE_START_DB_MAX 0.0f
#define OMX_GATE_KNEE_START_DB_DEFAULT -43.0f
#define OMX_GATE_KNEE_END_DB_MIN -80.0f
#define OMX_GATE_KNEE_END_DB_MAX 0.0f
#define OMX_GATE_KNEE_END_DB_DEFAULT -37.0f
#define OMX_GATE_ATTACK_MS_MIN 0.0f
#define OMX_GATE_ATTACK_MS_MAX 500.0f
#define OMX_GATE_ATTACK_MS_DEFAULT 1.0f
#define OMX_GATE_HOLD_MS_MIN 0.0f
#define OMX_GATE_HOLD_MS_MAX 2000.0f
#define OMX_GATE_HOLD_MS_DEFAULT 10.0f
#define OMX_GATE_RELEASE_MS_MIN 0.0f
#define OMX_GATE_RELEASE_MS_MAX 5000.0f
#define OMX_GATE_RELEASE_MS_DEFAULT 100.0f
#define OMX_GATE_HYSTERESIS_DB_MIN 0.0f
#define OMX_GATE_HYSTERESIS_DB_MAX 24.0f
#define OMX_GATE_HYSTERESIS_DB_DEFAULT 3.0f
#define OMX_GATE_RATIO_MIN 1.0f
#define OMX_GATE_RATIO_MAX 100.0f
#define OMX_GATE_RATIO_DEFAULT 16.0f

#define OMX_PITCH_SEMITONES_MIN -12.0f
#define OMX_PITCH_SEMITONES_MAX 12.0f
#define OMX_PITCH_SEMITONES_DEFAULT 0.0f
#define OMX_PITCH_CENTS_MIN -50.0f
#define OMX_PITCH_CENTS_MAX 50.0f
#define OMX_PITCH_CENTS_DEFAULT 0.0f
#define OMX_PITCH_MIX_MIN 0.0f
#define OMX_PITCH_MIX_MAX 100.0f
#define OMX_PITCH_MIX_DEFAULT 100.0f

/* The program-dependent release's model constants (PROGRAM_RELEASE, strip-dynamics-limits.ts;
 * docs/design/specs/2026-09-26-compressor-models.md §3b) — read by omx_envelope.h's
 * omx_env_program_release_profile(), never restated. */
#define OMX_PROGRAM_RELEASE_CHARGE_MS 1000.0f
#define OMX_PROGRAM_RELEASE_DISCHARGE_MS 5000.0f
#define OMX_PROGRAM_RELEASE_OPTO_FAST_MS 70.0f
#define OMX_PROGRAM_RELEASE_OPTO_SLOW_MIN_MS 110.0f
#define OMX_PROGRAM_RELEASE_OPTO_SLOW_MAX_MS 1100.0f
#define OMX_PROGRAM_RELEASE_OPTO_SHARE 0.5f

/* The four coreLimits-sourced facts with a native meaning — pan, delay, reverb, drive
 * (CORE_LIMITS, control-bounds.ts; F7). Not every field here has a C-side literal to replace:
 * the reverb cut corners and the drive stage's five numerics are clamped only on the TS side
 * today (mix_reverb.h's own header comment: "there is no 20 kHz anywhere in the C"), so those
 * defines exist for completeness and for the day a native clamp is added, without yet being
 * read by a stage. */
#define OMX_PAN_PAN_MIN -1.0f
#define OMX_PAN_PAN_MAX 1.0f

#define OMX_DELAY_TIME_MS_MIN 0.0f
#define OMX_DELAY_TIME_MS_MAX 2000.0f
#define OMX_DELAY_TIME_MS_DEFAULT 300.0f

#define OMX_REVERB_LOWCUT_MIN 0.0f
#define OMX_REVERB_LOWCUT_MAX 20000.0f
#define OMX_REVERB_HIGHCUT_MIN 0.0f
#define OMX_REVERB_HIGHCUT_MAX 20000.0f

#define OMX_DRIVE_DRIVE_DB_MIN 0.0f
#define OMX_DRIVE_DRIVE_DB_MAX 36.0f
#define OMX_DRIVE_DRIVE_DB_DEFAULT 0.0f
#define OMX_DRIVE_CHARACTER_MIN -1.0f
#define OMX_DRIVE_CHARACTER_MAX 1.0f
#define OMX_DRIVE_CHARACTER_DEFAULT 0.0f
#define OMX_DRIVE_BAND_HZ_MIN 20.0f
#define OMX_DRIVE_BAND_HZ_MAX 20000.0f
#define OMX_DRIVE_BAND_HZ_DEFAULT 2000.0f
#define OMX_DRIVE_MIX_MIN 0.0f
#define OMX_DRIVE_MIX_MAX 100.0f
#define OMX_DRIVE_MIX_DEFAULT 100.0f
#define OMX_DRIVE_TRIM_DB_MIN -24.0f
#define OMX_DRIVE_TRIM_DB_MAX 12.0f
#define OMX_DRIVE_TRIM_DB_DEFAULT 0.0f

/* The fractional read kernel's l1 norm, Λ = Σ|c| of the order-3 Lagrange read at its worst
 * fraction (packages/core/src/fdelay-kernel.ts, derived from omx_fdelay_lagrange's own formula).
 * A read is not convex: |read| ≤ Λ·max|x|. The chorus and flanger gain bounds carry it
 * (docs/design/specs/2026-09-22-chorus-and-flanger.md §6, F6). */
#define OMX_FDELAY_READ_L1_NORM 1.25f

/* The pitch shifter's declared constants (packages/core/src/pitch-kernel.ts, PITCH_KERNEL; its travels
 * are PITCH_LIMITS, rendered above with every other table;
 * docs/design/specs/2026-09-22-native-pitch-shift.md §11) — read by mix_pitch.h and its oracle. */
#define OMX_PITCH_WINDOW_MS 40.0f
#define OMX_PITCH_PREFILTER_CEILING_HZ 20000.0f
#define OMX_PITCH_PREFILTER_Q 0.7071067811865476f

/* The in-process LV2 stage's numbers (packages/core/src/lv2-stage-limits.ts, LV2_STAGE_LIMITS;
 * docs/design/specs/2026-09-04-lv2-hosting-path.md §4/§5) — read by mix_lv2.h, never restated. */
#define OMX_LV2_STAGE_WARMUP_BLOCKS 64u
#define OMX_LV2_STAGE_WARMUP_LEVEL_DBFS -6.0f
#define OMX_LV2_STAGE_WARMUP_LEVEL_LINEAR 0.501187205f
#define OMX_LV2_STAGE_NONFINITE_STRIKES 3u
#define OMX_LV2_STAGE_CLAMP_DBFS 24.0f
#define OMX_LV2_STAGE_CLAMP_LINEAR 15.8489323f

/* The all-pass and Linkwitz-Riley crossover primitives' numbers (packages/core/src/
 * dsp-primitive-limits.ts; docs/design/specs/2026-09-26-dsp-primitives.md §1 rows 4–6) — the
 * section Qs omx_xover.h designs with and the tolerances its and omx_allpass.h's POSTs read. */
#define OMX_ALLPASS_UNITY_TOL 0.0001f
#define OMX_XOVER_LR2_SECTION_Q 0.5
#define OMX_XOVER_LR4_SECTION_Q 0.7071067811865476
#define OMX_XOVER_PARTITION_TOL 0.00001f

#endif /* OMX_CONTRACT_LIMITS_H */

/* The sample rates the console declares (core's STANDARD_SAMPLE_RATES, row-codecs.ts): the set a
 * contract's rate-is-declared precondition accepts, through omx_rate_is_declared(), and the grid a
 * native tool iterates. Guarded on its own so a tool built against a pinned copy of this header
 * (lv2-inprocess-pin.sh) can include this tree's copy after it and gain the rates without mixing
 * the pin's limits. */
#ifndef OMX_DECLARED_RATE_COUNT
#define OMX_DECLARED_RATE_COUNT 6u
static const float OMX_DECLARED_RATES[OMX_DECLARED_RATE_COUNT] = { 44100.0f, 48000.0f, 88200.0f, 96000.0f, 176400.0f, 192000.0f };
/* The declared rates that are whole multiples of `base` (a tool's default grid: the 48 kHz family),
 * in declaration order, at most `cap` of them; returns how many were written to `out`. */
static inline int omx_declared_rates_multiple_of(unsigned base, int *out, int cap) {
  int n = 0;
  for (unsigned k = 0; k < OMX_DECLARED_RATE_COUNT && n < cap; k++)
    if ((unsigned)OMX_DECLARED_RATES[k] % base == 0) out[n++] = (int)OMX_DECLARED_RATES[k];
  return n;
}
#endif
