/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * HRP multi-f0 estimation: a magnitude spectrum in, a SET of musical voices out.
 *
 * Spec: openmixer docs/design/specs/2026-08-18-hrp-polyphonic-architecture.md §3, §5, §9, §10, §55.
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/hrp_pitch.h (omx-dsp#15, lean-engine
 * spec §1(a)). The engine keeps the spectrum rings, the control-thread tick and the strip's EQ
 * bands that apply a correction; the analysis itself lives here.
 *
 * ## Why a voice SET, from the first line
 *
 * §5 forbids the obvious API — a `PitchResult` carrying one `f0` — and it is right to. A detector
 * that returns one fundamental forces every caller above it to assume one, and the assumption
 * spreads: the planter indexes harmonics off `the` f0, the UI draws `the` note, the learning model
 * keys on it. Widening that later is not a signature change, it is a rewrite of everything that
 * consumed it. So the set is the unit here even while the detector finds a single voice, and
 * `voice_count == 0` is a first-class answer meaning the material is not pitched.
 *
 * ## What this is NOT
 *
 * Not source separation (§10 says so explicitly). Attribution here is a conservative harmonic
 * window: energy near n·f0 is credited to that voice, and where two voices' partials land in the
 * same bins the ambiguity is REPORTED rather than resolved, so the policy tier can decline to
 * correct (§11, §54: a missed correction is preferable to a wrong cut).
 *
 * Not an FFT. §1 forbids a second one and there is no need: `omx_rta_spectrum` in `mix_dsp.h`
 * already produces the magnitudes, and this consumes them.
 *
 * ## Real-time
 *
 * NEVER call this from the audio callback (§2, §4). It runs on the analysis side, behind the
 * existing RTA ring. It allocates nothing, takes every buffer from the caller, holds no global
 * mutable state, and does no I/O or logging — so it is deterministic and safe to call from a
 * worker, which is the only place it is called from.
 */
#ifndef OMX_HRP_PITCH_H
#define OMX_HRP_PITCH_H

#include <math.h>
#include <omxdsp/omx_contract_limits.h>
#include <stdint.h>
#include <string.h>

/* §7: an implementation limit, not a claim about music.
 *
 * EIGHT (operator sizing, 2026-09-01), raised from the spec's opening four once increment 9 made
 * the corrections real. The number is what the INSTRUMENTS on a desk carry: a guitar sounds six
 * strings, a piano or string-section voicing sits at eight, and a search that stops at four
 * cannot even represent the chord it is listening to. It costs what it says only on material that
 * has that many voices — the search abandons a round the moment nothing credible remains
 * (`omx_hrp_detect` breaks out below `min_confidence`), so a monophonic channel does the same
 * work at eight as it did at four.
 *
 * OMX_HRP_MAX_VOICES itself comes from the generated omx_contract_limits.h (declared once, in
 * the declarations package) — one number, no twin. */
/* §9: H1..H12 per voice. 8 × 12 = 96 potential components, from which correction stays sparse. */
#define OMX_HRP_HARMONICS 12

/** The confidence floor is HALF a voice's fair share — the divisor, named so both languages
 * derive the floor the same way (see {@link omx_hrp_min_confidence_for}). */
#define OMX_HRP_CONFIDENCE_SHARE_DIVISOR 2

/** One musical fundamental the analyser believes is sounding. */
typedef struct {
  float f0_hz;
  float confidence;   /* 0..1 — this comb's share of the WHOLE spectrum (the detector's gate) */
  /** 0..1 — this voice's share of the PITCHED content: its explained energy over every
   * detected voice's. The display's percentage: a solo over an orchestra bed can only ever
   * explain its own slice of the whole spectrum (confidence tops out low however clearly the
   * note is heard), but among what is PITCHED it dominates — which is the question an
   * operator's "how sure are we of this note" actually asks. */
  float salience;
  float amplitude_db; /* dBFS of the strongest partial credited to this voice */
  int midi_note;      /* nearest equal-tempered note, A4 = 69 */
  float cents;        /* signed distance from that note, −50..+50 */
  int valid;
} OmxHrpVoice;

/** What one analysis frame concluded. `voice_count == 0` means "nothing pitched here". */
typedef struct {
  OmxHrpVoice voices[OMX_HRP_MAX_VOICES];
  uint32_t voice_count;
} OmxHrpVoiceSet;

/** The search, as the caller declares it. No global tuning — a second console may want another. */
typedef struct {
  float f0_min_hz;
  float f0_max_hz;
  uint32_t harmonics;        /* clamped to OMX_HRP_HARMONICS */
  uint32_t max_voices;       /* clamped to OMX_HRP_MAX_VOICES */
  /** Below this a candidate is not admitted as a voice. Derive it from `max_voices` with
   * {@link omx_hrp_min_confidence_for} — a floor typed independently of the budget it is a
   * fraction of goes wrong in whichever direction the budget last moved. */
  float min_confidence;
  float subharmonic_accept;  /* §55's octave guard — see omx_hrp_detect */
  float cents_step;          /* search resolution; smaller is finer and slower */
} OmxHrpConfig;

/** The confidence floor that goes with a voice budget — the DERIVATION, not a tuned number.
 *
 * `max_voices` equal voices share the spectrum, so each explains about `1/max_voices` of it. A
 * floor at that share admits only the loudest of a full chord; the floor has to sit BELOW a fair
 * share or a chord loses its quietest real note. Half a share is the rule, and it is the rule and
 * not a constant because the budget moves: at four voices it gives 0.125 (the 0.12 that was typed
 * here before the budget grew), at eight it gives 0.0625.
 *
 * The other end is the noise floor, and it is what stops the halving from continuing: flat
 * broadband hiss scores ~0.015 (`test_noise_is_not_a_voice` builds exactly that spectrum), so
 * eight voices still leave a factor of four between music and hiss. A budget large enough to push
 * the floor into that gap would need a different discriminator, not a smaller number. */
static inline float omx_hrp_min_confidence_for(uint32_t max_voices) {
  if (max_voices == 0) max_voices = 1;
  if (max_voices > OMX_HRP_MAX_VOICES) max_voices = OMX_HRP_MAX_VOICES;
  return 1.0f / (float)(max_voices * OMX_HRP_CONFIDENCE_SHARE_DIVISOR);
}

/** The defaults this desk searches with. */
static inline OmxHrpConfig omx_hrp_config_default(void) {
  OmxHrpConfig c;
  c.f0_min_hz = 25.0f;   /* below a 5-string bass' B0 */
  c.f0_max_hz = 2100.0f; /* above a soprano's top */
  c.harmonics = OMX_HRP_HARMONICS;
  c.max_voices = OMX_HRP_MAX_VOICES;
  c.min_confidence = omx_hrp_min_confidence_for(c.max_voices);
  c.subharmonic_accept = 0.6f;
  c.cents_step = 25.0f;
  return c;
}

/** Nearest equal-tempered MIDI note, and the signed cents to it. A4 = 440 Hz = note 69. */
static inline int omx_hrp_midi_note(float f0_hz, float *cents_out) {
  if (!(f0_hz > 0.0f)) {
    if (cents_out) *cents_out = 0.0f;
    return -1;
  }
  double exact = 69.0 + 12.0 * log2((double)f0_hz / 440.0);
  double nearest = floor(exact + 0.5);
  if (cents_out) *cents_out = (float)((exact - nearest) * 100.0);
  return (int)nearest;
}

/** The peak-search half-width, in CENTS: how far a real partial may sit from the comb's
 * prediction and still be THIS harmonic. Musical, not a bin count, because the displacement
 * IS musical — vibrato and inharmonicity move partial n proportionally to n·f0, so a fixed
 * bin radius that is generous at H1 reads H6 as absent and confidence collapses exactly on
 * expressive playing (measured live, trombone at 5.86 Hz bins, 2026-08-18). 30 cents: wide
 * enough for concert vibrato, well inside the 100-cent spacing of the next semitone. */
#define OMX_HRP_PEAK_TOL_CENTS 30.0f

/** {@link OMX_HRP_PEAK_TOL_CENTS} as a frequency fraction (2^(30/1200) − 1). */
#define OMX_HRP_PEAK_TOL_FRAC 0.01748f

/** The half-width in BINS for a partial predicted at `exact` bins: the cents tolerance in
 * this spectrum's own units, floored at ±1 because a Hann window's main lobe is two bins
 * wide — the energy of even a perfectly-placed partial straddles its neighbours. */
static inline int32_t omx_hrp_peak_halfwidth(float exact_bins) {
  int32_t half = (int32_t)(exact_bins * OMX_HRP_PEAK_TOL_FRAC);
  return half < 1 ? 1 : half;
}

/** The magnitude at `hz`, taken as the LOCAL PEAK over the straddling bins.
 *
 * Interpolating would be wrong here: a partial that falls between bins is smeared across them by
 * the window, and the peak is the honest reading of "how much is there". The search radius is
 * {@link omx_hrp_peak_halfwidth} — proportional, so a displaced upper partial still reads as its
 * own harmonic's level. */
static inline float omx_hrp_bin_peak(const float *mag, uint32_t nbins, float bin_hz, float hz) {
  if (!(bin_hz > 0.0f) || nbins == 0) return 0.0f;
  float exact = hz / bin_hz;
  if (exact < 0.0f) return 0.0f;
  int32_t centre = (int32_t)(exact + 0.5f);
  int32_t half = omx_hrp_peak_halfwidth(exact);
  float best = 0.0f;
  for (int32_t b = centre - half; b <= centre + half; b++) {
    if (b < 0 || (uint32_t)b >= nbins) continue;
    if (mag[b] > best) best = mag[b];
  }
  return best;
}

/** How well a harmonic comb at `f0` explains `mag` — a 1/n-WEIGHTED mean of its partials.
 *
 * The weighting is what makes the score mean anything. An unweighted mean cannot tell a comb
 * whose H1 sits on a partial from one whose H12 happens to land on the same partial: both credit
 * one hit over twelve slots and score identically. With the sweep running upward and ties going
 * to the first maximum, the winner was then the LOWEST impostor — a clean 440 Hz sine read as
 * 35.87 Hz, whose H12 is 430 Hz and lands within a bin of it.
 *
 * 1/n is not arbitrary: it is where a real fundamental keeps its energy, and it is the envelope
 * of every sawtooth-like instrument tone. A missing partial contributes zero and costs nothing
 * more — instruments genuinely skip harmonics, and penalising absence would refuse a clarinet.
 *
 * Normalised by the weights actually used, so combs with different harmonic counts below Nyquist
 * remain comparable. */
static inline float omx_hrp_comb_score(const float *mag, uint32_t nbins, float bin_hz, float f0_hz,
                                       uint32_t harmonics) {
  if (!(f0_hz > 0.0f) || !(bin_hz > 0.0f) || nbins == 0) return 0.0f;
  float nyquist = (float)(nbins - 1) * bin_hz;
  float sum = 0.0f, weight = 0.0f;
  for (uint32_t n = 1; n <= harmonics; n++) {
    float hz = (float)n * f0_hz;
    if (hz > nyquist) break;
    float w = 1.0f / (float)n;
    sum += w * omx_hrp_bin_peak(mag, nbins, bin_hz, hz);
    weight += w;
  }
  return weight > 0.0f ? sum / weight : 0.0f;
}

/** The mean magnitude of the partials a comb at `f0` predicts — the yardstick the octave guard
 *  measures "is this partial actually there?" against. Unweighted on purpose: presence is a fact
 *  about a bin, not about which harmonic index happens to point at it. */
static inline float omx_hrp_partial_mean(const float *mag, uint32_t nbins, float bin_hz,
                                         float f0_hz, uint32_t harmonics) {
  if (!(f0_hz > 0.0f) || !(bin_hz > 0.0f) || nbins == 0) return 0.0f;
  float nyquist = (float)(nbins - 1) * bin_hz;
  float sum = 0.0f;
  uint32_t counted = 0;
  for (uint32_t n = 1; n <= harmonics; n++) {
    float hz = (float)n * f0_hz;
    if (hz > nyquist) break;
    sum += omx_hrp_bin_peak(mag, nbins, bin_hz, hz);
    counted++;
  }
  return counted == 0 ? 0.0f : sum / (float)counted;
}

/** Total magnitude in the spectrum — the denominator confidence is measured against. */
static inline float omx_hrp_total(const float *mag, uint32_t nbins) {
  float t = 0.0f;
  for (uint32_t i = 0; i < nbins; i++) t += mag[i];
  return t;
}

/** Does `f0` carry energy in its OWN low harmonics, or is it a coincidence of other notes?
 *
 * The trap this closes, measured on A4+C5: a comb at 175 Hz scores well on that dyad because its
 * H3 lands on C5, its H5 on A4's H2, its H10 on A4's H4 and its H12 on C5's H4. It "explains"
 * both real notes as its own upper partials — and there is NOTHING at 175 or 350 Hz. Every
 * polyphonic detector meets this; a common subharmonic of two real notes always scores.
 *
 * A note is audible at its fundamental or at its octave. A missing-fundamental tone is still loud
 * at H2, which is why H1 alone is not required and why this does not undo the subharmonic guard.
 * A coincidence has neither.
 *
 * Judged against the candidate's OWN loudest partial rather than any global level, so a quiet
 * voice inside a loud chord is held to the same shape rather than to the chord's volume.
 */
static inline int omx_hrp_low_support(const float *mag, uint32_t nbins, float bin_hz, float f0_hz,
                                      uint32_t harmonics, float accept) {
  if (!(f0_hz > 0.0f) || !(bin_hz > 0.0f)) return 0;
  /* Measured against the MEAN partial, not the loudest. Against the loudest, a tone whose H3
     dominates fails its own test — H1 at 0.3 cannot clear 0.6 of a 1.0 H3 — and "a dominant upper
     harmonic" is precisely one of the shapes §55 requires to keep working. The mean asks the
     question that was meant: is there anything at the bottom of this series at all? */
  float mean = omx_hrp_partial_mean(mag, nbins, bin_hz, f0_hz, harmonics);
  if (!(mean > 0.0f)) return 0;
  float need = accept * mean;
  return omx_hrp_bin_peak(mag, nbins, bin_hz, f0_hz) >= need ||
         omx_hrp_bin_peak(mag, nbins, bin_hz, 2.0f * f0_hz) >= need;
}

/** Remove one voice's partials from `residual`, so the next search cannot re-find them.
 *
 * This is what makes the search polyphonic: without it, the second-best candidate is almost
 * always an octave or fifth of the first, because those combs share partials with it. Zeroing
 * ±1 bin matches the width {@link omx_hrp_bin_peak} reads over, so a partial is either fully
 * credited to this voice or fully left for the next one. */
static inline void omx_hrp_cancel(float *residual, uint32_t nbins, float bin_hz, float f0_hz,
                                  uint32_t harmonics) {
  if (!(f0_hz > 0.0f) || !(bin_hz > 0.0f)) return;
  float nyquist = (float)(nbins - 1) * bin_hz;
  for (uint32_t n = 1; n <= harmonics; n++) {
    float hz = (float)n * f0_hz;
    if (hz > nyquist) break;
    float exact = hz / bin_hz;
    int32_t centre = (int32_t)(exact + 0.5f);
    /* Erase over the SAME radius the readers credit ({@link omx_hrp_peak_halfwidth}): energy a
       wider read attributed to this voice but a narrower erase left standing would be found
       again next round as a phantom. */
    int32_t half = omx_hrp_peak_halfwidth(exact);
    for (int32_t b = centre - half; b <= centre + half; b++) {
      if (b < 0 || (uint32_t)b >= nbins) continue;
      residual[b] = 0.0f;
    }
  }
}

/** May the detector believe a fundamental at `sub`, one step below what it picked?
 *
 * The question splits, and conflating the two halves is what makes a tuned threshold necessary:
 *
 *   H1 AUDIBLE      the fundamental is simply there and was not the loudest partial. A tone with
 *                   a dominant H2 or H3 is the ordinary case, not an extraordinary claim, and it
 *                   needs no further proof.
 *
 *   H1 ABSENT       this is a MISSING-FUNDAMENTAL claim, and those are the dangerous ones (§55).
 *                   Demand an unbroken run H2..H5. A real series has no holes in its low
 *                   harmonics; a phantom assembled from other notes' partials always does,
 *                   because it only has energy where a real note happens to land.
 *
 * The measured case that forces the split, on A4+C5+E5: the phantom at 221 Hz has H2, H3, H4
 * (A4, E5, A5) and then a HOLE at H5 — nothing in the chord sounds at 1106 Hz. It scored exactly
 * 0.700 against a 0.70 completeness gate, which is a coincidence rather than a decision. Asking
 * for contiguity instead, it fails outright at H5 while a true missing fundamental passes with
 * every one of H2..H5 present.
 */
static inline int omx_hrp_descent_admissible(const float *mag, uint32_t nbins, float bin_hz,
                                             float sub_hz, const OmxHrpConfig *cfg) {
  float mean = omx_hrp_partial_mean(mag, nbins, bin_hz, sub_hz, cfg->harmonics);
  if (!(mean > 0.0f)) return 0;
  float threshold = cfg->subharmonic_accept * mean;
  if (omx_hrp_bin_peak(mag, nbins, bin_hz, sub_hz) >= threshold) return 1;

  float nyquist = (float)(nbins - 1) * bin_hz;
  for (uint32_t n = 2; n <= 5; n++) {
    float hz = (float)n * sub_hz;
    if (hz > nyquist) return 0; /* it cannot be proven up here, so it is not granted */
    if (omx_hrp_bin_peak(mag, nbins, bin_hz, hz) < threshold) return 0;
  }
  return 1;
}

/** The subharmonic guard §55 demands: is the real fundamental BELOW the one we just picked?
 *
 * The dangerous error is reading 440 Hz as H1 when the note is 220 and its fundamental is weak or
 * missing: every harmonic index then shifts, and a correction aimed at H4 lands on H8 — a
 * destructive cut at a frequency nobody asked about.
 *
 * Two things this cannot be. It cannot compare the two combs' total scores, because the lower
 * comb contains every partial of the upper one and so scores at least as well on a pure sine —
 * that test drops a clean 440 to 220. And it cannot test only f0/2, because the error is not
 * always an octave: a tone whose H3 dominates is picked at 3·f0, and 660 → 220 is a factor of
 * THREE, which no halving test will ever reach.
 *
 * So it asks the one question that discriminates, for each divisor: of the partials the LOWER
 * comb predicts and the upper one does not (the n not divisible by d), how many are actually
 * there? Two independent ones is the bar — one could be any neighbouring sound. The lowest
 * divisor that clears it wins, because that is the deepest fundamental the evidence supports.
 */
static inline float omx_hrp_octave_guard(const float *mag, uint32_t nbins, float bin_hz,
                                         float f0_hz, const OmxHrpConfig *cfg) {
  float nyquist = (float)(nbins - 1) * bin_hz;
  float current = f0_hz;

  /* ITERATIVE, one step at a time, each step judged against the CURRENT candidate rather than the
     original. Testing every divisor against f0 at once looks equivalent and is not: with f0=440
     over a missing-fundamental 220, the ÷4 candidate at 110 is "supported" by the partials at 660
     and 1100 — which are 220's H3 and H5, evidence for 220 and not for 110 at all. Descending in
     steps makes each drop justify itself against the note we now believe, and 110 is then correctly
     refused because ITS own new partials (110, 330, 550) are absent.

     Bounded at three descents: a fourth would be a factor of 16 or more below the picked pitch,
     which is not a harmonic confusion any more, it is a different instrument. */
  for (uint32_t step = 0; step < 3; step++) {
    float mean = omx_hrp_partial_mean(mag, nbins, bin_hz, current, cfg->harmonics);
    if (!(mean > 0.0f)) break;
    float threshold = cfg->subharmonic_accept * mean;

    float next = current;
    /* 2 before 3: prefer the octave explanation to the twelfth when both would fit. */
    for (uint32_t d = 2; d <= 3; d++) {
      float sub = current / (float)d;
      if (sub < cfg->f0_min_hz) continue;
      uint32_t present = 0;
      for (uint32_t n = 1; n <= cfg->harmonics; n++) {
        if (n % d == 0) continue; /* shared with the current comb — carries no evidence either way */
        float hz = (float)n * sub;
        if (hz > nyquist) break;
        if (omx_hrp_bin_peak(mag, nbins, bin_hz, hz) >= threshold) present++;
      }
      /* Two independent new partials AND audibility at its own fundamental or octave. The second
         condition is not redundant with the first: descending is how the detector reaches a
         spurious low f0 that the sweep itself already refuses. On A4+C5 the descent lands on
         175 Hz — which has energy at 525, 875 and 1750 (partials of the two REAL notes) and
         nothing whatever at 175 or 350. Filtering the sweep and not the descent leaves the trap
         wide open, because the descent is the door it actually comes through. */
      /* Completeness is judged against the CANDIDATE's own mean partial, never the current comb's.
         Using the current one lowers the bar for exactly the wrong candidate: on A4+C5+E5 the
         440 comb's mean is 0.272 and the 220 phantom's is 0.336, so borrowing 440's threshold
         admits partials that 220's own yardstick calls absent, and a 0.583-complete phantom
         reads as complete enough. Self-relative, the phantom fails and the true missing
         fundamental (0.875) still passes. */
      if (present >= 2 && omx_hrp_descent_admissible(mag, nbins, bin_hz, sub, cfg)) {
        next = sub;
        break;
      }
    }
    if (next == current) break;
    current = next;
  }
  return current;
}

/** Sharpen `f0` against the partials actually observed, by least squares through them.
 *
 * The sweep alone cannot do better than its own plateau. `omx_hrp_bin_peak` reads ±1 bin, so every
 * comb within about a bin of the truth scores identically, and an ascending sweep taking the first
 * maximum returns the LOW EDGE of that plateau — measured at 429.9 Hz for a 440 Hz sine, 40 cents
 * flat, which is enough to name the wrong note.
 *
 * Each observed partial gives an estimate `peak_hz / n`, and the high harmonics are the precise
 * ones: a fixed ±half-bin uncertainty at n·f0 is only a third as much error in f0 at H3 as at H1.
 * Fitting `peak_hz(n) = n · f0` weighted by that precision is exactly `Σ n·peak / Σ n²`.
 */
static inline float omx_hrp_refine(const float *mag, uint32_t nbins, float bin_hz, float f0_hz,
                                   uint32_t harmonics, float present_threshold) {
  if (!(f0_hz > 0.0f) || !(bin_hz > 0.0f)) return f0_hz;
  float nyquist = (float)(nbins - 1) * bin_hz;
  double num = 0.0, den = 0.0;
  for (uint32_t n = 1; n <= harmonics; n++) {
    float hz = (float)n * f0_hz;
    if (hz > nyquist) break;
    /* The bin this partial actually peaks in, searched over the same radius the score reads
       ({@link omx_hrp_peak_halfwidth}). */
    float exact = hz / bin_hz;
    int32_t centre = (int32_t)(exact + 0.5f);
    int32_t half = omx_hrp_peak_halfwidth(exact);
    int32_t at = -1;
    float best = 0.0f;
    for (int32_t b = centre - half; b <= centre + half; b++) {
      if (b < 0 || (uint32_t)b >= nbins) continue;
      if (mag[b] > best) {
        best = mag[b];
        at = b;
      }
    }
    if (at < 0 || best < present_threshold) continue; /* absent partials say nothing about f0 */
    num += (double)n * ((double)at * (double)bin_hz);
    den += (double)n * (double)n;
  }
  if (den <= 0.0) return f0_hz;
  float refined = (float)(num / den);
  return refined > 0.0f ? refined : f0_hz;
}

/** Estimate every fundamental present, strongest first.
 *
 * Iterative estimate-and-cancel: score every candidate on the residual, take the best, believe it
 * only if it clears `min_confidence`, correct its octave, record it, then erase its partials and
 * search again. Stops at `max_voices`, or the moment a round finds nothing worth believing —
 * which is why a single note yields exactly one voice rather than four increasingly imaginary ones.
 *
 * @param mag       magnitude spectrum, `nbins` bins, bin 0 at DC
 * @param bin_hz    hertz per bin — `rate / fft_size`
 * @param residual  CALLER-OWNED scratch of `nbins` floats; clobbered. No allocation happens here.
 * @param out       the voice set; always written, `voice_count` 0 when nothing is pitched
 */
static inline void omx_hrp_detect(const float *mag, uint32_t nbins, float bin_hz,
                                  const OmxHrpConfig *cfg, float *residual, OmxHrpVoiceSet *out) {
  memset(out, 0, sizeof(*out));
  if (!mag || !residual || !cfg || nbins == 0 || !(bin_hz > 0.0f)) return;

  OmxHrpConfig c = *cfg;
  if (c.harmonics == 0 || c.harmonics > OMX_HRP_HARMONICS) c.harmonics = OMX_HRP_HARMONICS;
  if (c.max_voices == 0 || c.max_voices > OMX_HRP_MAX_VOICES) c.max_voices = OMX_HRP_MAX_VOICES;
  if (!(c.cents_step > 0.0f)) c.cents_step = 25.0f;
  if (!(c.f0_max_hz > c.f0_min_hz) || !(c.f0_min_hz > 0.0f)) return;

  memcpy(residual, mag, (size_t)nbins * sizeof(float));

  const float total = omx_hrp_total(mag, nbins);
  if (!(total > 0.0f)) return;
  /* Geometric sweep: pitch is logarithmic, so a constant Hz step would over-search the top of the
     range and under-search the bottom, where the notes are closest together in Hz. */
  const float ratio = powf(2.0f, c.cents_step / 1200.0f);

  for (uint32_t v = 0; v < c.max_voices; v++) {
    /* TWO PASSES, and the order is the whole point.
     *
     * Pass 1 considers only candidates whose OWN fundamental is audible. Pass 2 runs just for
     * material where pass 1 found nothing, and admits missing-fundamental hypotheses.
     *
     * Greedy estimate-and-cancel makes one characteristic error, and a single pass cannot avoid
     * it: on A4+C5+E5+G5 the best-scoring comb is C4 at 262 Hz, because C5 really IS its H2 and
     * G5 really IS its H3. Nothing is wrong with that arithmetic — it is a true harmonic
     * relationship — and lowering the confidence floor does not help, because the phantom wins
     * the ROUND and eats the partials of two real notes before they are ever considered.
     *
     * What separates them is what a musician would say: you can hear all four fundamentals, and
     * you cannot hear the C4. So a hypothesis that needs no missing fundamental is settled first,
     * and the expensive claim is only entertained when the cheap one explains nothing. */
    float best_f0 = 0.0f, best_score = 0.0f;
    for (uint32_t pass = 0; pass < 2 && !(best_f0 > 0.0f); pass++) {
      for (float f0 = c.f0_min_hz; f0 <= c.f0_max_hz; f0 *= ratio) {
        float mean = omx_hrp_partial_mean(residual, nbins, bin_hz, f0, c.harmonics);
        if (!(mean > 0.0f)) continue;
        if (pass == 0) {
          /* Its own fundamental, audibly present. */
          if (omx_hrp_bin_peak(residual, nbins, bin_hz, f0) < c.subharmonic_accept * mean) continue;
        } else if (!omx_hrp_low_support(residual, nbins, bin_hz, f0, c.harmonics,
                                        c.subharmonic_accept)) {
          /* Pass 2 still refuses a comb with nothing at f0 OR 2·f0 — that is not a missing
             fundamental, it is a coincidence of other notes' partials (the 175 Hz case). */
          continue;
        }
        float sc = omx_hrp_comb_score(residual, nbins, bin_hz, f0, c.harmonics);
        if (sc > best_score) {
          best_score = sc;
          best_f0 = f0;
        }
      }
    }
    if (!(best_f0 > 0.0f)) break;

    /* Confidence is the share of the WHOLE spectrum this comb accounts for, so a comb sitting on
       noise scores low however tall its own bins are. Measured against the original spectrum, not
       the residual: otherwise each cancellation shrinks the denominator and later voices are
       flattered into looking more certain than the first. */
    float explained = 0.0f;
    float nyquist = (float)(nbins - 1) * bin_hz;
    float loudest = 0.0f;
    for (uint32_t n = 1; n <= c.harmonics; n++) {
      float hz = (float)n * best_f0;
      if (hz > nyquist) break;
      float m = omx_hrp_bin_peak(residual, nbins, bin_hz, hz);
      explained += m;
      if (m > loudest) loudest = m;
    }
    float confidence = explained / total;
    if (confidence > 1.0f) confidence = 1.0f;
    if (confidence < c.min_confidence) break;

    float f0 = omx_hrp_octave_guard(residual, nbins, bin_hz, best_f0, &c);
    /* Sharpen only against partials that are really there, judged by the same yardstick the guard
       uses, so a silent slot cannot drag the fit. */
    float present_threshold =
        c.subharmonic_accept * omx_hrp_partial_mean(residual, nbins, bin_hz, f0, c.harmonics);
    f0 = omx_hrp_refine(residual, nbins, bin_hz, f0, c.harmonics, present_threshold);

    OmxHrpVoice *voice = &out->voices[out->voice_count];
    voice->f0_hz = f0;
    voice->confidence = confidence;
    voice->salience = explained; /* raw for now — normalised over ALL voices after the loop */
    voice->amplitude_db = loudest > 0.0f ? 20.0f * log10f(loudest) : -999.0f;
    voice->midi_note = omx_hrp_midi_note(f0, &voice->cents);
    voice->valid = 1;
    out->voice_count++;

    omx_hrp_cancel(residual, nbins, bin_hz, f0, c.harmonics);
  }

  /* Salience is RELATIVE, so it is settled once the set is complete: each voice's explained
     energy over every detected voice's. A lone voice reads 1.0 — all of the pitched content
     is this note — which is the number a clearly-heard solo deserves however loud the
     unpitched bed around it. */
  float explained_total = 0.0f;
  for (uint32_t i = 0; i < out->voice_count; i++) explained_total += out->voices[i].salience;
  for (uint32_t i = 0; i < out->voice_count; i++) {
    out->voices[i].salience =
        explained_total > 0.0f ? out->voices[i].salience / explained_total : 0.0f;
  }
}

#endif /* OMX_HRP_PITCH_H */
