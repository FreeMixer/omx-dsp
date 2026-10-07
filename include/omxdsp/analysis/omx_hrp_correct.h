/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_hrp_correct.h — the correcting half of HRP, in C: what a shell needs to turn the render
 * engine's MEASUREMENTS into a filter, when there is no TypeScript on the other side of the
 * boundary to do it.
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/hrp_correct.h (omx-dsp#15,
 * lean-engine spec §1(a)); the one change is that the cascade comes from <omxdsp/omx_biquad.h>
 * and <omxdsp/omx_contract.h> directly instead of through the engine's mix_dsp.h.
 *
 * Spec: openmixer docs/design/specs/2026-08-18-hrp-polyphonic-architecture.md (§16 the family is
 * a broad prior, §23 positive deviations only, §25 cuts never boosts, §26/§29/§30 the existing EQ and
 * the one globally ranked list), under docs/design/specs/2026-07-16-recording-vsc.md §0.5's
 * "one C core, three shells".
 *
 * ## Why this header exists — the third shell, and what it cannot reach
 *
 * `hrp_render.h` is the engine, and it is already shell-neutral: header-only, allocation-free,
 * every buffer the caller's, and its own note names "an LV2 host's own pool" as one of the
 * places that memory may come from. Two shells drive it today — the console's N-API render and
 * the `omx-hrp` CLI — and BOTH of them decide what to cut in TypeScript, because both run with
 * `@freemixer/core` on the other side of the N-API boundary: `hrpSearchFor` narrows the search,
 * `harmonicDeviationGainsDb` sizes the cut, `selectHrpCorrections` ranks it and `eqCoeffs`
 * designs the curve. The engine is handed ready `{b0,b1,b2,a1,a2}` sets and never designs one.
 *
 * A LIVE LV2 INSERT HAS NO SUCH BOUNDARY. It is a `.so` in a foreign host's process; there is no
 * JavaScript to call and nothing to hand it ready coefficients. So the three facts the existing
 * shells get from core — the family prior, the cut sizing plus §29/§30's ranking, and the matched
 * curve — need a home the third shell can link. This is that home, and it is ONE home: the
 * offline cascade moved here rather than being copied, so the render shell and the live shell
 * apply corrections through the same four functions and the same biquad.
 *
 * ## What this is NOT
 *
 * NOT a second EQ (§1). The filter is `omx_biquad_cascade`'s, from `omx_biquad.h`, exactly as the
 * RT lane's and the render's are. What is added here is the DESIGN of one section shape — the
 * peaking bell, the only curve HRP has ever planted — not a bank, not a filter, not a slot.
 *
 * NOT a second vocabulary. {@link OmxHrpFamily}'s members are `INSTRUMENT_FAMILIES` in its own
 * order, so a family's INDEX is its identity across the boundary; the numbers below are the
 * console's own, twinned rather than re-decided. Both twinnings are asserted from the TypeScript
 * side by `packages/audio-engine/src/hrp-native-twins.test.ts`, which reads THIS FILE — the same
 * idiom that holds `OMX_EQ_MAX_BANDS` equal to `EQ_MAX_BANDS`. A number edited here and not
 * there goes red; a family added there and not here goes red.
 *
 * NOT a place decisions get re-made. Every rule below is transcribed from the module named
 * beside it. Where the transcription and its TypeScript twin could disagree, a test makes them
 * agree rather than a comment asking someone to remember.
 */
#ifndef OMX_HRP_CORRECT_H
#define OMX_HRP_CORRECT_H

/* The matched-Z primitive first: it carries <math.h> and M_PI for everything below. */
#include <omxdsp/omx_matched_pair.h>

#include <stdint.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_pitch.h>
#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_contract.h>

/* ---- the declared numbers ------------------------------------------------------------
 *
 * Twins of `@freemixer/declarations`. Each is the console's ONE answer; the C copy exists
 * because a `.so` in someone else's host cannot read a TypeScript module, and it is held equal
 * by `hrp-native-twins.test.ts` rather than by anyone noticing. */

/** Largest automatic cut, positive dB magnitude. Declared as `HRP_MAX_CUT_DB`. */
/* OMX_HRP_MAX_CUT_DB: read from omx_contract_limits.h (the declaration's own door). */

/** The bell's Q for every planted band. Declared as `HRP_BAND_Q`. */
/* OMX_HRP_BAND_Q: read from omx_contract_limits.h (the declaration's own door). */

/** Deviation below which a partial is expression, not resonance. Declared as `HRP_EXCESS_THRESHOLD_DB`. */
/* OMX_HRP_EXCESS_THRESHOLD_DB: read from omx_contract_limits.h (the declaration's own door). */

/** Two candidates closer than this are the same spectral REGION and one of them is discarded
 * (§30). Declared as `HRP_COLLISION_CENTS`.
 *
 * NOT `hrp_attribute.h`'s `OMX_HRP_ATTRIBUTION_CENTS` (the declared 60), and the name is what keeps the two
 * apart. That one is §11's ATTRIBUTION window: how close two partials must sit before the desk
 * cannot tell which voice owns the energy, so it is the analyser's resolution and follows the
 * ±1 bin `omx_hrp_bin_peak` reads over. This one is §30's SELECTION window: how close two
 * CORRECTIONS must sit before planting both merely deepens one bell, so it follows
 * {@link OMX_HRP_BAND_Q} — a Q-5 bell's -3 dB half-width is ~170 cents, and a semitone apart
 * they overlap over most of their travel. Different laws, different derivations, different
 * numbers; they shared a name for one commit and the build now refuses that (-Werror on the
 * two binaries that include both headers). */
#define OMX_HRP_SELECT_COLLISION_CENTS OMX_HRP_COLLISION_CENTS /* the declaration, through omx_contract_limits.h */

/** How far ONE pass may move a band's gain, dB — §48's smoothing, at the control level because
 * §48 forbids interpolating in the native biquad. Declared as `HRP_GAIN_STEP_DB`.
 *
 * A correction ENTERS by deepening from silence and LEAVES by walking to exactly 0 dB, the
 * provable parked identity, from which removal is bit-identical. An unbounded step is a
 * waveform discontinuity — the click heard live on 2026-08-20. */
/* OMX_HRP_GAIN_STEP_DB: read from omx_contract_limits.h (the declaration's own door). */

/** The channel's band budget across ALL voices (§29). Declared as `HRP_DEFAULT_MAX_AUTO_BANDS`. */
/* OMX_HRP_DEFAULT_MAX_AUTO_BANDS: read from omx_contract_limits.h (the declaration's own door). */

/** The corrector's default strength, 0..1. Twin of `makeHrpChannelState().amount`. */
#define OMX_HRP_DEFAULT_AMOUNT 0.5f

/* ---- §16: the instrument family, as a broad prior ------------------------------------- */

/**
 * The console's instrument families, in `INSTRUMENT_FAMILIES` order — so a family's INDEX is
 * what crosses a wire, a port or a process boundary, and the two lists cannot silently
 * disagree about which integer means `guitar`.
 *
 * §16: "Reuse OpenMixer's existing InstrumentFamily. Do not create a second instrument
 * enumeration." This is not a second one; it is the same list, spelled for C, and the twin test
 * fails if it ever stops being the same list.
 */
typedef enum {
  /** No family assigned — the honest answer, and NOT a copy of the wide defaults: a caller with
   *  no family runs `omx_hrp_config_default`, which is their one home. */
  OMX_HRP_FAMILY_NONE = -1,
  OMX_HRP_FAMILY_DRUMS = 0,
  OMX_HRP_FAMILY_PERCUSSION = 1,
  OMX_HRP_FAMILY_BASS = 2,
  OMX_HRP_FAMILY_GUITAR = 3,
  OMX_HRP_FAMILY_KEYS = 4,
  OMX_HRP_FAMILY_VOCALS = 5,
  OMX_HRP_FAMILY_BRASS = 6,
  OMX_HRP_FAMILY_WIND = 7,
  OMX_HRP_FAMILY_STRINGS = 8,
  OMX_HRP_FAMILY_PLAYBACK = 9
} OmxHrpFamily;

/** How many families the vocabulary holds. Twin of `INSTRUMENT_FAMILIES.length`. */
#define OMX_HRP_FAMILY_COUNT 10

/**
 * The search a family implies — `HRP_FAMILY_SEARCH`, transcribed. Every reason for every number
 * is written at the TypeScript table and is not repeated here; what IS repeated is the one rule
 * that a transcription could plausibly get wrong.
 *
 * THE FLOOR DOES NOT FOLLOW THE FAMILY. `hrpSearchFor` narrows `f0_min_hz`, `f0_max_hz` and
 * `max_voices`, and then sets `min_confidence` from the CONSOLE's full voice budget, never from
 * the family's narrowed one. Deriving it from the narrowed budget would raise the floor to 0.25
 * at two voices and 0.50 at one, and the measured scores of exactly the material those families
 * name — a floor tom at 0.0635, a vocal over a room bed at 0.1014 — sit far below that. A
 * narrowed search says how many voices to look for; it says nothing about how loud one must be,
 * and a family prior that went deaf on its own instrument would be worse than no prior at all.
 *
 * An unknown or {@link OMX_HRP_FAMILY_NONE} family returns the console's own defaults unchanged.
 */
static inline OmxHrpConfig omx_hrp_family_search(int family) {
  OmxHrpConfig c = omx_hrp_config_default();
  if (family < 0 || family >= OMX_HRP_FAMILY_COUNT) return c;
  switch ((OmxHrpFamily)family) {
    case OMX_HRP_FAMILY_DRUMS:      c.max_voices = 1; c.f0_min_hz = 35.0f;   c.f0_max_hz = 400.0f;  break;
    case OMX_HRP_FAMILY_PERCUSSION: c.max_voices = 1; c.f0_min_hz = 60.0f;   c.f0_max_hz = 1200.0f; break;
    case OMX_HRP_FAMILY_BASS:       c.max_voices = 2; c.f0_min_hz = 25.0f;   c.f0_max_hz = 400.0f;  break;
    case OMX_HRP_FAMILY_GUITAR:     c.max_voices = 6; c.f0_min_hz = 60.0f;   c.f0_max_hz = 1400.0f; break;
    case OMX_HRP_FAMILY_KEYS:       c.max_voices = 8; c.f0_min_hz = 27.5f;   c.f0_max_hz = 4200.0f; break;
    case OMX_HRP_FAMILY_VOCALS:     c.max_voices = 2; c.f0_min_hz = 80.0f;   c.f0_max_hz = 1200.0f; break;
    case OMX_HRP_FAMILY_BRASS:      c.max_voices = 2; c.f0_min_hz = 40.0f;   c.f0_max_hz = 1400.0f; break;
    case OMX_HRP_FAMILY_WIND:       c.max_voices = 2; c.f0_min_hz = 55.0f;   c.f0_max_hz = 2600.0f; break;
    case OMX_HRP_FAMILY_STRINGS:    c.max_voices = 8; c.f0_min_hz = 40.0f;   c.f0_max_hz = 2800.0f; break;
    case OMX_HRP_FAMILY_PLAYBACK:   c.max_voices = 8; c.f0_min_hz = 25.0f;   c.f0_max_hz = 2100.0f; break;
    case OMX_HRP_FAMILY_NONE:
    default: return c;
  }
  /* The console's floor, from the FULL budget — see the note above. */
  c.min_confidence = omx_hrp_min_confidence_for(OMX_HRP_MAX_VOICES);
  return c;
}

/* ---- the one curve HRP designs -------------------------------------------------------- */

/**
 * The MATCHED PEAKING section, normalised to the `{b0,b1,b2,a1,a2}` tuple `omx_biquad`
 * consumes — the C twin of `@freemixer/core`'s `rbjCoeffs('peaking', …)`.
 *
 * MATCHED, NOT BILINEAR (operator ruling, 2026-09-14). The console's graph runs at 96 kHz and
 * its EQ must match the analogue prototype to Nyquist by coefficient design; the RBJ cookbook
 * this replaced, being a bilinear image, was 1.28 dB low at 20 kHz on a +12 dB bell at 16 kHz
 * Q 2. Both of the prototype's pole pairs are finite, so the matched-Z map `z = exp(sT)` places
 * all four of its poles and zeros exactly and one scalar sets unity at DC:
 *
 *   H(s) = (s^2 + A s/Q + 1) / (s^2 + s/(A Q) + 1),  A = 10^(gain/40)
 *   zeros: a pair at f0 damped A/(2Q)      -> matched_pair(w0, A/(2Q))
 *   poles: a pair at f0 damped 1/(2 A Q)   -> matched_pair(w0, 1/(2 A Q))
 *
 * A 0 dB section is EXACTLY identity: A = 1 makes the two dampings the same number, so the two
 * calls return the same pair and the scalar is exactly 1.
 *
 * ONE SHAPE, NOT A BANK. §26 says HRP corrects through the existing EQ and plants nothing but
 * bells; §25 says it cuts and never boosts. So the third shell needs exactly one section shape
 * designed, and giving it the whole design family would be inventing a second EQ (§1) to avoid
 * writing six lines. A caller wanting a shelf or a pass filter is a caller doing something HRP
 * does not do.
 *
 * The clamps are the TypeScript's, term for term: the centre is held just under Nyquist so a
 * partial detected at the top of the band cannot produce a degenerate section, and a
 * non-positive Q becomes a tiny positive one rather than a division by zero. `out` is written
 * only on success; a NULL out or a non-positive rate leaves the caller's memory alone.
 *
 * Held equal to `rbjCoeffs` by the corpus in `hrp_peaking_corpus.h`, which BOTH sides check.
 */

static inline void omx_hrp_peaking_coeffs(float freq_hz, float q, float gain_db, uint32_t rate,
                                          float out[5]) {
  if (!out || rate == 0u) return;
  const double w0 = omx_matched_w0((double)freq_hz, (double)rate);
  double qq = (double)q;
  if (!(qq > 0.0)) qq = 1e-3;
  const double a = pow(10.0, (double)gain_db / 40.0);
  /* poles at f0 damped 1/(2AQ), zeros at f0 damped A/(2Q) */
  omx_matched_pair_section(out, w0, 1.0 / (2.0 * a * qq), w0, a / (2.0 * qq));
}

/* ---- the cascade ---------------------------------------------------------------------
 *
 * MOVED HERE FROM `hrp_render.h`, not copied into a second place. The offline render and the
 * live insert apply their corrections through these four functions and through
 * `omx_biquad_cascade` beneath them, so "a rendered cut and a live cut are the same filter" is
 * true because it is the same code, not because two files were kept in step.
 */

/** The cascade one shell applies, plus its per-section state. One instance per mono stream. */
typedef struct {
  uint32_t count;
  float coeffs[OMX_EQ_MAX_BANDS][5];
  uint8_t enabled[OMX_EQ_MAX_BANDS];
  float state[OMX_EQ_MAX_BANDS][4];
} OmxHrpCascade;

/** An empty cascade: no bands, no state. Running audio through it is a copy. */
static inline void omx_hrp_cascade_init(OmxHrpCascade *c) {
  if (c) memset(c, 0, sizeof(*c));
}

/**
 * Append one designed section. Returns 1 when it was taken, 0 when the cascade is full — a
 * refusal, never a silent drop past the end of the array, because a band that did not reach
 * the audio would otherwise show in the report and not in the sound.
 */
static inline int omx_hrp_cascade_add(OmxHrpCascade *c, const float coeffs[5], int enabled) {
  if (!c || !coeffs) return 0;
  if (c->count >= OMX_EQ_MAX_BANDS) return 0;
  memcpy(c->coeffs[c->count], coeffs, 5u * sizeof(float));
  c->enabled[c->count] = enabled ? 1u : 0u;
  memset(c->state[c->count], 0, sizeof(c->state[c->count]));
  c->count++;
  return 1;
}

/** Sections that will actually run — a parked (identity) band costs nothing and is skipped. */
static inline uint32_t omx_hrp_cascade_live(const OmxHrpCascade *c) {
  if (!c) return 0;
  uint32_t live = 0;
  for (uint32_t b = 0; b < c->count && b < OMX_EQ_MAX_BANDS; b++)
    if (c->enabled[b]) live++;
  return live;
}

/**
 * Run `n` frames from `in` into `out`, advancing the cascade's state.
 *
 * BIT-PRESERVING BYPASS. With no live section the frames are copied and nothing else happens —
 * `out` is `in`, byte for byte. Offline that is what makes "the take keeps both files" honest;
 * live it is what makes a racked-but-idle corrector inaudible, which is the only reason an
 * operator would ever leave one racked. A cascade that "almost" bypassed would be a plugin
 * nobody could safely insert.
 *
 * `in` and `out` may be the same buffer. State persists across calls, so a shell may stream in
 * blocks of any size — a file in 64 k chunks, a host in 64-frame quanta — and get the same
 * samples a single call would produce.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "hrp/cascade"
static inline void omx_hrp_cascade_apply(OmxHrpCascade *c, const float *in, float *out,
                                         uint32_t n) {
  /* CONTRACT (omx_contract.h). This runs under a FOREIGN host (hrp_lv2.c) — the only thing
   * standing between us and a host handing it whatever it likes — so its preconditions are
   * finite-input and the coefficients it correcting FROM are the ones §23/§25 authorised
   * (never a correction outside the declared gain range); its postcondition is BIT-PRESERVING
   * BYPASS with no live section and a finite block otherwise, inherited from
   * {@link omx_biquad_cascade}'s own finite-state contract. */
  OMX_PRE(n == 0u || omx_block_finite(in, n), "finite-in");
  if (!in || !out || n == 0u) return;
  if (out != in) memcpy(out, in, (size_t)n * sizeof(float));
  if (!c || omx_hrp_cascade_live(c) == 0u) {
    OMX_POST(memcmp(out, in, (size_t)n * sizeof(float)) == 0, "bypass-bit-identical");
    return;
  }
  omx_biquad_cascade(out, n, c->count, (const float(*)[5])c->coeffs, c->enabled, c->state);
  OMX_POST(omx_block_finite(out, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/* ---- §23 / §25: what a measured excess authorises ------------------------------------- */

/**
 * The cut a positive deviation authorises, as a POSITIVE magnitude in dB — the C twin of
 * `harmonicDeviationGainsDb`'s per-harmonic rule.
 *
 * Only the part ABOVE {@link OMX_HRP_EXCESS_THRESHOLD_DB} is a resonance; the threshold itself
 * is the instrument's expression and is never corrected. The operator's `amount` scales what
 * remains, and {@link OMX_HRP_MAX_CUT_DB} caps it. A deviation at or below the threshold, a
 * non-positive amount, or a non-finite input authorises NOTHING — zero, which callers read as
 * "no band", never as "a band of zero dB".
 *
 * §25 lives in the sign: this returns a magnitude to CUT. There is no path here that boosts.
 */
static inline float omx_hrp_cut_db(float excess_db, float amount, float max_cut_db) {
  if (!(excess_db > OMX_HRP_EXCESS_THRESHOLD_DB)) return 0.0f;
  if (!(amount > 0.0f)) return 0.0f;
  if (!(max_cut_db > 0.0f)) return 0.0f;
  const float correctable = (excess_db - OMX_HRP_EXCESS_THRESHOLD_DB) * amount;
  return correctable < max_cut_db ? correctable : max_cut_db;
}

/**
 * §24's resonance score, the documented formula — the C twin of `hrpResonanceScore`:
 *
 *     score = cut_db × voice_confidence × partial_confidence
 *
 * The other two §24 inputs are GATES upstream rather than weights here: temporal stability is
 * binary (§13 — only a stable voice produces a candidate at all) and an ambiguous attribution
 * is unmeasurable (§11/§54 — it never becomes a deviation, so it never scores). Any unusable
 * input scores zero, because a NaN weight would poison every comparison it met.
 */
static inline float omx_hrp_resonance_score(float cut_db, float voice_confidence,
                                            float partial_confidence) {
  if (!(cut_db > 0.0f)) return 0.0f;
  if (!(voice_confidence > 0.0f)) return 0.0f;
  if (!(partial_confidence > 0.0f)) return 0.0f;
  return cut_db * voice_confidence * partial_confidence;
}

/* ---- §29 / §30: the one globally ranked list ------------------------------------------ */

/** One voice's one correctable partial, as a shell proposes it. */
typedef struct {
  /** Where the partial actually sounded, Hz — the MEASURED fundamental's harmonic, never the
   *  tempered grid's, so the bell lands on what was measured. */
  float freq_hz;
  /** The authorised cut as a positive magnitude, dB ({@link omx_hrp_cut_db}). */
  float cut_db;
  /** {@link omx_hrp_resonance_score}'s answer — the weight the global ranking orders by. */
  float score;
  /** The tracker's voice id — never reused, so a planted band can never reattach. */
  int32_t voice_id;
  /** 1-based; harmonic 1 is the fundamental, which the excess rule never cuts. */
  uint32_t harmonic;
} OmxHrpCandidate;

/** Strongest first; ties by voice id then harmonic, so the answer never depends on input order. */
static inline int omx_hrp_candidate_before(const OmxHrpCandidate *a, const OmxHrpCandidate *b) {
  if (a->score != b->score) return a->score > b->score;
  if (a->voice_id != b->voice_id) return a->voice_id < b->voice_id;
  return a->harmonic < b->harmonic;
}

/** Musical distance between two frequencies, in cents. */
static inline float omx_hrp_cents_apart(float a_hz, float b_hz) {
  if (!(a_hz > 0.0f) || !(b_hz > 0.0f)) return 0.0f;
  const float c = 1200.0f * (float)(log2((double)a_hz / (double)b_hz));
  return c < 0.0f ? -c : c;
}

/**
 * The globally ranked, collision-free, budgeted correction list (§29 + §30), IN PLACE.
 *
 * `cand[0 .. return-1]` are the kept candidates, strongest first; the rest of the array is
 * scratch the caller may ignore. Nothing is allocated and nothing is copied out, so this runs
 * as happily on an LV2 worker's stack frame as inside a render.
 *
 * §29 forbids the obvious alternative in as many words — "Do NOT independently allocate the top
 * N corrections for every voice". The budget is a property of the CHANNEL, so every voice's
 * candidates meet in ONE list before anything is spent: three voices allocated separately would
 * plant three mediocre cuts where the channel deserved the two strongest and had slots for
 * nothing more.
 *
 * COLLISIONS RESOLVE BEFORE THE BUDGET, and greedily from the strongest down: a candidate
 * within `collision_cents` of one already kept is the same spectral region and is discarded
 * (§30's "merge or select the stronger", done as selection, because a bell deepened by a
 * near-twin is not a correction anybody measured). Resolving first matters — a discarded
 * near-twin must not occupy a rank a distant candidate deserved.
 *
 * A candidate that scores zero is not a candidate: it is dropped before ranking rather than
 * ranked last, because "nothing to correct" and "the weakest correction" are different facts.
 *
 * Insertion sort, not qsort: `n` is at most voices × harmonics (8 × 12 = 96) and the comparison
 * needs no function-pointer call. `qsort` would also drag libc's allocation-free-ness into a
 * question §4 already answered.
 */
static inline uint32_t omx_hrp_select(OmxHrpCandidate *cand, uint32_t n, float collision_cents,
                                      uint32_t budget) {
  if (!cand || n == 0u || budget == 0u) return 0u;

  /* Drop the scoreless before ranking — see above. */
  uint32_t live = 0u;
  for (uint32_t i = 0; i < n; i++) {
    if (cand[i].score > 0.0f) {
      if (live != i) cand[live] = cand[i];
      live++;
    }
  }
  if (live == 0u) return 0u;

  for (uint32_t i = 1; i < live; i++) {
    const OmxHrpCandidate key = cand[i];
    uint32_t j = i;
    while (j > 0u && omx_hrp_candidate_before(&key, &cand[j - 1u])) {
      cand[j] = cand[j - 1u];
      j--;
    }
    cand[j] = key;
  }

  uint32_t kept = 0u;
  for (uint32_t i = 0; i < live && kept < budget; i++) {
    int collides = 0;
    for (uint32_t k = 0; k < kept; k++) {
      if (omx_hrp_cents_apart(cand[i].freq_hz, cand[k].freq_hz) < collision_cents) {
        collides = 1;
        break;
      }
    }
    if (collides) continue;
    const OmxHrpCandidate keep = cand[i];
    cand[kept++] = keep;
  }
  return kept;
}

#endif /* OMX_HRP_CORRECT_H */
