/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * HRP spectral attribution: which voice does this energy belong to, and how sure are we?
 *
 * Spec: openmixer docs/design/specs/2026-08-18-hrp-polyphonic-architecture.md §9, §10, §11, §54.
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/hrp_attribute.h (omx-dsp#15, lean-engine
 * spec §1(a)). The engine keeps the spectrum rings, the control-thread tick and the strip's EQ
 * bands that apply a correction; the analysis itself lives here.
 *
 * ## The measurement
 *
 * For each voice, the harmonic grid `n · f0` for n = 1..12 (§9), and for each of those a
 * frequency, a measured level and a confidence (§10). Harmonics that are not there are reported
 * ABSENT rather than as zero-level measurements — "there is nothing at H7" and "H7 is very quiet"
 * are different facts, and only the first should stop a correction being planted.
 *
 * ## The part that matters more than the measurement
 *
 * §11: when two voices' partials land close enough that the energy cannot be assigned, DO NOT
 * make a strong correction. Reduce confidence, mark it ambiguous, let the analyser show it.
 *
 * This is why attribution takes the WHOLE voice set rather than one voice at a time. Ambiguity is
 * not a property of a partial — it is a property of a partial IN THE PRESENCE OF the others, and
 * a per-voice call cannot see it. A4 and E5 sounding together put A4's H3 and E5's H2 in the same
 * bin: that energy is real, and it belongs to both, and cutting it because one voice looks loud
 * there would attenuate a note nobody complained about.
 *
 * Deliberately NOT source separation (§10 says so outright). The ambiguity is reported, not
 * resolved. §54: a missed correction is preferable to an incorrect EQ cut.
 *
 * Allocation-free, caller-owned buffers, no globals. Never called from the RT callback.
 */
#ifndef OMX_HRP_ATTRIBUTE_H
#define OMX_HRP_ATTRIBUTE_H

#include <math.h>
#include <omxdsp/omx_contract_limits.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_pitch.h>

/* The attribution window is OMX_HRP_ATTRIBUTION_CENTS from the generated omx_contract_limits.h (declared
 * once, in the declarations package; §11) — two partials closer than it are one spectral event. */

/** What ambiguity does to a measurement's confidence. Not zero: the partial IS there and the
 *  analyser should show it. Low enough that a correction tier honouring §54 will decline. */
#define OMX_HRP_AMBIGUOUS_CONFIDENCE 0.25f

/** One harmonic of one voice, measured. */
typedef struct {
  uint32_t harmonic; /* n, 1-based: harmonic 1 IS the fundamental */
  float freq_hz;     /* where the grid says it should be — n · f0 */
  float level_db;
  float confidence; /* 0..1; reduced when another voice claims the same region */
  int ambiguous;    /* another voice's grid lands within OMX_HRP_ATTRIBUTION_CENTS */
  int present;      /* is there anything here at all? */
} OmxHrpPartial;

/** One voice's whole harmonic grid, measured and attributed. */
typedef struct {
  OmxHrpPartial partials[OMX_HRP_HARMONICS];
  uint32_t count; /* how many fitted below Nyquist */
} OmxHrpVoiceHarmonics;

/** Is a partial audible at all, judged against this voice's own mean partial? Self-relative for
 *  the same reason everywhere else in HRP is: a quiet voice inside a loud chord must be held to
 *  its own shape, not to the chord's level. */
static inline int omx_hrp_partial_present(float level, float voice_mean, float accept) {
  return voice_mean > 0.0f && level >= accept * voice_mean;
}

/**
 * Measure every voice's harmonic grid, and mark the partials that cannot be told apart.
 *
 * @param mag     magnitude spectrum
 * @param bin_hz  hertz per bin
 * @param voices  the frame's voice set
 * @param cfg     the search config (harmonics, and the presence yardstick)
 * @param out     caller-owned array of at least `voices->voice_count` grids; fully written
 */
static inline void omx_hrp_attribute(const float *mag, uint32_t nbins, float bin_hz,
                                     const OmxHrpVoiceSet *voices, const OmxHrpConfig *cfg,
                                     OmxHrpVoiceHarmonics *out) {
  if (!mag || !voices || !cfg || !out || nbins == 0 || !(bin_hz > 0.0f)) return;
  uint32_t harmonics = cfg->harmonics;
  if (harmonics == 0 || harmonics > OMX_HRP_HARMONICS) harmonics = OMX_HRP_HARMONICS;
  float nyquist = (float)(nbins - 1) * bin_hz;

  /* Pass 1: measure each grid on its own terms. */
  for (uint32_t v = 0; v < voices->voice_count && v < OMX_HRP_MAX_VOICES; v++) {
    OmxHrpVoiceHarmonics *g = &out[v];
    memset(g, 0, sizeof(*g));
    float f0 = voices->voices[v].f0_hz;
    if (!(f0 > 0.0f)) continue;
    float mean = omx_hrp_partial_mean(mag, nbins, bin_hz, f0, harmonics);

    for (uint32_t n = 1; n <= harmonics; n++) {
      float hz = (float)n * f0;
      if (hz > nyquist) break;
      float level = omx_hrp_bin_peak(mag, nbins, bin_hz, hz);
      OmxHrpPartial *p = &g->partials[g->count++];
      p->harmonic = n;
      p->freq_hz = hz;
      p->level_db = level > 0.0f ? 20.0f * log10f(level) : -999.0f;
      p->present = omx_hrp_partial_present(level, mean, cfg->subharmonic_accept);
      /* The voice's own confidence, until pass 2 finds a reason to doubt this partial. An absent
         partial carries none: there is nothing here to be confident ABOUT. */
      p->confidence = p->present ? voices->voices[v].confidence : 0.0f;
      p->ambiguous = 0;
    }
  }

  /* Pass 2: collisions. A partial that another voice's grid also predicts cannot be attributed,
     and §11 says the answer is to say so rather than to pick. */
  for (uint32_t v = 0; v < voices->voice_count && v < OMX_HRP_MAX_VOICES; v++) {
    for (uint32_t i = 0; i < out[v].count; i++) {
      OmxHrpPartial *p = &out[v].partials[i];
      if (!p->present) continue;
      for (uint32_t w = 0; w < voices->voice_count && w < OMX_HRP_MAX_VOICES; w++) {
        if (w == v) continue;
        for (uint32_t j = 0; j < out[w].count; j++) {
          const OmxHrpPartial *q = &out[w].partials[j];
          if (!q->present) continue;
          float cents = fabsf(1200.0f * log2f(p->freq_hz / q->freq_hz));
          if (cents <= OMX_HRP_ATTRIBUTION_CENTS) {
            p->ambiguous = 1;
            if (p->confidence > OMX_HRP_AMBIGUOUS_CONFIDENCE)
              p->confidence = OMX_HRP_AMBIGUOUS_CONFIDENCE;
            break;
          }
        }
        if (p->ambiguous) break;
      }
    }
  }
}

/** The measured partial for harmonic `n` of one voice, or NULL if it was never measured. */
static inline const OmxHrpPartial *omx_hrp_partial_of(const OmxHrpVoiceHarmonics *grid,
                                                      uint32_t harmonic) {
  if (!grid) return NULL;
  for (uint32_t i = 0; i < grid->count; i++)
    if (grid->partials[i].harmonic == harmonic) return &grid->partials[i];
  return NULL;
}

/** How many of a voice's partials could be attributed to it ALONE — the ones a correction may
 *  safely act on. A voice whose every partial is shared has nothing HRP can honestly cut. */
static inline uint32_t omx_hrp_unambiguous_count(const OmxHrpVoiceHarmonics *grid) {
  uint32_t n = 0;
  if (!grid) return 0;
  for (uint32_t i = 0; i < grid->count; i++)
    if (grid->partials[i].present && !grid->partials[i].ambiguous) n++;
  return n;
}

#endif /* OMX_HRP_ATTRIBUTE_H */
