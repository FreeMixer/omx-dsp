// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * omx_drive.h — the native DRIVE stage: one waveshaper, run inside the console's ONE oversampler.
 *
 * It takes `omx_oversampler.h` — the shared element — rather than carrying a rate changer of its
 * own. Both were written the same night in different lanes and collided at the merge; the
 * element on the integration branch is the one that stands, and what this lane measured about
 * its FILTER is filed as a measured recommendation rather than as a second implementation
 * (docs/design/notes/2026-09-14-oversampler-filter-quality.md).
 *
 * Spec: docs/design/specs/2026-09-14-native-drive-stage.md §4.
 *
 * Same contract as the primitives and omx_delay.h: NO PipeWire, NO napi, NO allocation on the RT
 * thread, NO libc beyond <math.h>/<string.h>. Controls (`struct omx_drive`) are a word-atomic
 * snapshot the RT reads once per block; the working state (`struct omx_drive_state`) is ONE
 * plain struct the CONTROL thread callocs on first enable and keeps until the mixer is
 * destroyed — the same lifecycle as the delay's rings and the reverb's pool, and for the same
 * reason: this kernel has buffers and the RT callback never allocates.
 *
 * ## The shape, in one line each
 *
 *   band split (base rate, complementary)  ->  UP  ->  shaper  ->  DOWN  ->  un-tilt, DC block,
 *   HF roll-off  ->  auto-gain  ->  mix back against the DELAY-COMPENSATED dry  ->  trim.
 *
 * ## ONE mechanism for even harmonics, and it is `character` — a BIAS, not a residue blend
 *
 * (Spec §4b as amended 2026-09-15. The form this file ran until then, `u + (1-w)*(F(u)-u) +
 * w*(|F(u)|-|u|)`, kept the FUNDAMENTAL linear and had slope -2 with no floor on the negative
 * half at w > 0 -- at 36 dB of drive a full-scale sample computed to about -125 linear. It was
 * replaced, not patched.)
 *
 * The even harmonics come from BIASING the shaper's input, which is what the analogue circuits
 * do and the only form that cannot leave the curve's own ceiling:
 *
 *     b   = w * OMX_DRIVE_BIAS_MAX,   w = (character + 1) / 2,  in [0,1]
 *     out = ( F(u + b) - F(b) ) / (1 + F(b))
 *
 * BOUNDED BY CONSTRUCTION. Every knee has ceiling 1, so F(u+b) - F(b) lies in
 * [-1 - F(b), 1 - F(b)] whose widest magnitude is 1 + F(b) -- which is the divisor. |out| < 1
 * for every u, every b >= 0 and every curve. `test_character_is_bounded` sweeps all four curve
 * members x character -1..+1 x driveDb 0..36 x a full-scale input and measures a worst 1.000000.
 *
 * w = 0 (character -1) gives b = 0 and out = F(u) EXACTLY -- the pure-odd end is bit-identical
 * to what shipped, so nothing an operator dialled at that end moves. w = 1 (character +1) is
 * EVEN-DOMINANT, not pure even: H2 leads H3 by 26.2 dB at 6 dB of drive and 8.5 dB at 12 dB,
 * and past ~15 dB the clipping's odd harmonics take over (a hard-clipped wave's evens come from
 * a duty-cycle asymmetry, and a fixed bias is a shrinking fraction of a growing swing). The
 * oracle measures all of it; §4b's table is those numbers.
 *
 * Nothing else in this file produces an asymmetry — the "tube" curve is a KNEE, not a second
 * bias mechanism, because a second one would have to be kept equal to this one by hand.
 *
 * ## Delay compensation is not optional
 *
 * The wet path comes back omx_oversampler_latency_for() samples late. Summing that against an
 * UNDELAYED dry is a comb filter, not a mix — so the two dry taps this stage needs (the stage
 * input, and the band the curve did not see) run through a matching delay line. That is why
 * `mix = 0` stays bit-identical dry at every factor and every band, and the oracle checks it at
 * all twelve combinations rather than at the one the author happened to try.
 */
#ifndef OMX_MIX_DRIVE_H
#define OMX_MIX_DRIVE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <omxdsp/omx_biquad.h>
#include <omxdsp/omx_envelope.h>
#include <omxdsp/omx_onepole.h>
#include <omxdsp/omx_ramp.h>
#include <omxdsp/omx_contract.h>
#include <omxdsp/omx_oversampler.h>

/* The shaper's knee. Members, never merged — the same law mix_reverb.h's three algorithms carry.
 * OMX_DRIVE_EXCITER selects the parallel-SUM topology and runs the SOFT shape in it. */
enum omx_drive_curve {
  OMX_DRIVE_SOFT = 0,
  OMX_DRIVE_TAPE = 1,
  OMX_DRIVE_TUBE = 2,
  OMX_DRIVE_EXCITER = 3,
};

/* Which part of the spectrum the curve is fed. The split is COMPLEMENTARY: hi = x - LP(x), so
 * the two halves sum back to x exactly at any frequency and any phase. A pair of independently
 * designed filters does not, and "mix 0 is bit-identical dry" would then be false the moment the
 * band left FULL. */
enum omx_drive_band {
  OMX_DRIVE_BAND_FULL = 0,
  OMX_DRIVE_BAND_LOW = 1,
  OMX_DRIVE_BAND_HIGH = 2,
  OMX_DRIVE_BAND_TILT = 3,
};

/* How many base-rate samples the kernel processes per pass. The whole point of a fixed chunk is
 * that every scratch buffer becomes an INLINE array: the state is one calloc, not a pool with a
 * layout. The oversampler's state is streaming, so chunking is invisible in the result —
 * omx_oversampler.test.c's own streaming arms are what
 * makes that a checked claim rather than a hope. */
#define OMX_DRIVE_CHUNK 64u

/* The factors this stage offers, clamped the way the shared element clamps them: it refuses to
 * invent a rate change it has not been measured at, rather than approximating one. */
static inline int omx_drive_factor_of(int requested) {
  if (requested >= 4) return 4;
  if (requested == 2) return 2;
  return 1;
}

/*
 * The compensation delay line. It must hold the largest latency any factor declares AND a whole
 * chunk on top of it, because the chunk is WRITTEN before it is READ: at 4x the read window for
 * output 0 sits 66 samples back, and a 128-long ring would have that index land on a sample
 * written three lines earlier in the same pass. (Found by the block-size arm of
 * test/fx/drive.test.c, which is the whole reason that arm exists: a stage that is correct at one
 * block size and wrong at another is a stage that is wrong on a console whose quantum changed.)
 */
#define OMX_DRIVE_DLINE 256u
#define OMX_DRIVE_DMASK (OMX_DRIVE_DLINE - 1u)

/*
 * The resolved control atom — built once per block by the caller from the word-atomic controls
 * plus the graph's sample rate (the DC blocker and the auto-gain window are time constants, so
 * they need the rate; the same shape resolve_dyn() already has).
 *
 * The BIQUAD coefficients are designed in TypeScript, like every other coefficient this node
 * runs (`@freemixer/core`'s rbjSection) — the C only RUNS them. `{b0,b1,b2,a1,a2}`, normalised.
 */
struct omx_drive {
  int enabled;    /* 0 -> the whole stage is a no-op: no sample and no state word is touched */
  int curve;      /* enum omx_drive_curve */
  int band;       /* enum omx_drive_band */
  float drive_lin;   /* pre-curve gain, 10^(driveDb/20) */
  float even_w;      /* (character + 1) / 2, in [0,1] */
  float mix;         /* 0..1 (the row carries percent; the resolver divides) */
  float trim_lin;    /* post-stage make-up, 10^(trimDb/20) */
  int auto_gain;     /* 1 -> the measured level match of §4d */
  int stereo_link;   /* 1 -> ONE auto-gain path for both legs */
  int hf_on;         /* 1 -> the post-curve roll-off runs */
  int os_factor;     /* 1 | 2 | 4 */
  uint32_t reset_gen; /* a re-enable's clean-start request: the state rebuilds when it differs */
  float band_c[5];     /* the band split's low-pass section */
  float tilt_c[5];     /* TILT: the pre-emphasis shelf */
  float tilt_inv_c[5]; /* TILT: its exact inverse, applied after the curve */
  float hf_c[5];       /* the post-curve roll-off's low-pass section */
  float dc_coeff;    /* DC blocker pole: 1 - 2*pi*5Hz/sr, clamped below 1 */
  float ag_coeff;    /* auto-gain POLE: omx_pole_from_time_ms(OMX_DRIVE_AG_WINDOW_MS, sr) */
};

/* One auto-gain path's detectors. Two of these on a lane whose legs are unlinked, one shared
 * when they are — which is the whole of what `stereo_link` governs, because a waveshaper is
 * memoryless and per-sample and has no other cross-leg state to link. */
struct omx_drive_ag {
  float ms_in;   /* one-pole mean-square of the stage input */
  float ms_out;  /* one-pole mean-square of the shaped output */
  float gain;    /* the smoothed gain actually applied */
  int primed;    /* 0 until the first block, so the first gain is not a ramp from zero */
};

/* One leg's filter and delay memory. */
struct omx_drive_leg {
  struct omx_oversampler os;
  float band_st[4];     /* the band split's low-pass history */
  float tilt_st[4];     /* pre-emphasis */
  float tiltinv_st[4];  /* de-emphasis */
  float hf_st[4];       /* post-curve roll-off */
  float dc_x, dc_y;     /* DC blocker */
  float dline_x[OMX_DRIVE_DLINE];    /* the stage INPUT, delay-compensated */
  float dline_keep[OMX_DRIVE_DLINE]; /* the band the curve did not see */
  uint32_t dw;
  /* scratch, inline because the chunk is fixed */
  float up[OMX_DRIVE_CHUNK * OMX_OVS_MAX_FACTOR];
  float curve_in[OMX_DRIVE_CHUNK];
  float keep[OMX_DRIVE_CHUNK];
  float wet[OMX_DRIVE_CHUNK];
};

/*
 * The whole working state: ONE calloc on first enable, held until mixerDestroy, NULL until then.
 * `factor` is what the oversamplers were built for — a change re-inits them (and only then), so
 * an operator turning 4x into 2x does not carry a history designed for another rate.
 */
struct omx_drive_state {
  struct omx_drive_leg l, r;
  struct omx_drive_ag ag_l, ag_r;
  float prev_gl, prev_gr; /* last chunk's applied gain, for the ramp */
  int factor;
  uint32_t gen; /* the atom's reset_gen this state was last built for */
};

/** Build (or rebuild for a new factor) the state. Fixed work — a memset and two oversampler
 * inits, no allocation — so it runs on the AUDIO thread too: `omx_drive_process` is the only
 * caller that rebuilds a state the RT may be walking (2026-09-25-native-fx-rt-review.md F3). */
#define OMX_CONTRACT_STAGE "drive/state-init"
static inline void omx_drive_state_init(struct omx_drive_state *s, int factor) {
  /* CONTRACT (omx_contract.h). Whatever factor is asked for, the state is built for one the
   * element offers (1, 2 or 4) and the gain ramp starts at unity — the two facts
   * omx_drive_process's own PRE reads back. */
  memset(s, 0, sizeof(*s));
  s->factor = omx_drive_factor_of(factor);
  s->prev_gl = s->prev_gr = 1.0f;
  omx_oversampler_init(&s->l.os, (uint32_t)s->factor);
  omx_oversampler_init(&s->r.os, (uint32_t)s->factor);
  OMX_POST(s->factor == 1 || s->factor == 2 || s->factor == 4, "factor-is-1-2-or-4");
  OMX_POST(s->prev_gl == 1.0f && s->prev_gr == 1.0f, "ramp-starts-at-unity");
}
#undef OMX_CONTRACT_STAGE

/*
 * The two TIME CONSTANTS the atom carries, from the rate — the one part of resolving an
 * `omx_drive` that is neither a straight load nor a coefficient TypeScript designed. ONE writer,
 * called by the console's resolve_fx_drive (mixer_rt.c) and by the LV2 shell (mix_drive_lv2.h),
 * so the two shells cannot drift by a pole. Defined after OMX_DRIVE_AG_WINDOW_MS below.
 *
 * The DC blocker's pole is the FIRST-ORDER APPROXIMATION 1 - 2*pi*fc/sr, NOT the tier's
 * `omx_pole_from_cutoff_hz` (exp(-2*pi*fc/sr)). The convention is the same — this is a pole —
 * but the VALUE is not: at 5 Hz / 96 kHz the two differ in the 10th significant figure, and at
 * 5 Hz / 44.1 kHz in the 9th. Folding it would MOVE a shipped coefficient, which is a finding
 * and not a fold (2026-09-17 duplication audit, S1's note section). Left exactly as it was;
 * the divergence is named here so the next reader does not have to re-derive it.
 */
static inline void omx_drive_time_constants(struct omx_drive *o, float sr);

/** The stage's added latency in base-rate samples — 0 while it is off. */
static inline int omx_drive_latency(const struct omx_drive *p) {
  return p->enabled ? (int)omx_oversampler_latency_for((uint32_t)p->os_factor) : 0;
}

/* ---- the shaper ---------------------------------------------------------------------- */

/*
 * The three knees. Every one is ODD, monotone through the origin, and normalised to F'(0) = 1,
 * so a small signal passes at unity and turning DRIVE up adds harmonics without also adding a
 * level jump the operator has to chase.
 *
 *              u = 1     u = 3     ceiling   H5/H3 at +12 dB drive
 *   SOFT       0.7616    0.9951    1         -12.90 dB   firmest knee, fastest roll-off
 *   TAPE       0.7071    0.9487    1         -11.22 dB   middle
 *   TUBE       0.5000    0.7500    1          -9.28 dB   gentlest knee, slowest roll-off
 *
 * Every one of those nine numbers is asserted in test/fx/drive.test.c, so "three shapes and not one"
 * is a measured fact of the shipped code rather than a sentence in a comment.
 *
 * THE ROLL-OFF ORDER IS THE OPPOSITE OF THE OBVIOUS GUESS, and it was measured rather than
 * assumed: tanh is analytic, so its harmonic series dies away fastest; u/(1+|u|) carries an |u|
 * whose higher derivatives are discontinuous, so TUBE spreads energy furthest up the series and
 * sounds the richest even though it is the gentlest knee. (THD alone does not order them at all
 * — it is measured against the fundamental, which a gentler knee also compresses, and the three
 * come out within 0.5 dB of each other in no useful order. The first version of the oracle
 * asserted a THD ordering and was wrong to.)
 */
#define OMX_CONTRACT_STAGE "drive/shape"
static inline float omx_drive_shape(int curve, float u) {
  float y;
  switch (curve) {
    case OMX_DRIVE_TAPE:
      y = u / sqrtf(1.0f + u * u);
      break;
    case OMX_DRIVE_TUBE:
      y = u / (1.0f + fabsf(u));
      break;
    case OMX_DRIVE_SOFT:
    case OMX_DRIVE_EXCITER:
    default:
      y = tanhf(u);
      break;
  }
  /* R-056/laws: finite -- accepted 2026-09-17, docs/design/notes/2026-09-17-contracts-proposals.json.
   * Every curve branch is bounded and finite for any finite u. */
  OMX_POST(y - y == 0.0f, "finite");
  return y;
}
#undef OMX_CONTRACT_STAGE

/*
 * The bias at `character = +1`, in the SHAPER's own input units — ONE normalised value for all
 * three knees (spec §4b).
 *
 * DERIVED, not chosen. In the saturating limit every knee tends to sgn(), the output becomes a
 * rectangular wave of duty d = 1/2 + asin(b)/pi, and a rectangular wave's nth harmonic goes as
 * sin(n*pi*d)/n — so |H2| is maximal at d = 3/4, which is b = sin(pi/4) = 1/sqrt(2) exactly.
 * Measured as well as derived: the H2-maximising bias for a full-scale sine at unity drive is
 * 0.712 (soft), 0.658 (tape), 0.645 (tube), and the maximum is flat enough that this one value
 * costs 0.000 / 0.035 / 0.088 dB of H2 against each curve's own optimum. One bias, not three.
 *
 * It is in the SHAPER's units and is NOT scaled by the drive. A drive-scaled bias was measured
 * and refused: it puts the operating point at 0.7071*drive, past the swing of any input below
 * -3 dBFS, so the waveform never crosses back — a 1 kHz tone at -6 dBFS with 18 dB of drive came
 * out 40 dB down, a channel the operator would hear go silent. OMX_DRIVE_BIAS_MAX comes from the
 * generated omx_contract_limits.h (declared once, in core's drive-shaper).
 */

/* The shaper's per-block constants for one `character`: the bias, and the offset+scale that
 * make the biased curve pass through the origin and stay inside [-1, +1]. Computed ONCE per
 * block by the caller, never per sample — `character` is a control word, not a signal. */
struct omx_drive_bias {
  float b;    /* w * OMX_DRIVE_BIAS_MAX */
  float off;  /* F(b) — what the biased curve reads at silence, subtracted so 0 in is 0 out */
  float inv;  /* 1 / (1 + F(b)) — the bound normaliser; exactly 1 at character -1 */
};

#define OMX_CONTRACT_STAGE "drive/bias-for"
static inline struct omx_drive_bias omx_drive_bias_for(int curve, float w) {
  struct omx_drive_bias s;
  s.b = w * OMX_DRIVE_BIAS_MAX;
  s.off = omx_drive_shape(curve, s.b);
  s.inv = 1.0f / (1.0f + s.off);
  /* R-056/laws: finite -- accepted 2026-09-17, docs/design/notes/2026-09-17-contracts-proposals.json.
   * s.off is bounded by omx_drive_shape's own finite range, so the reciprocal stays finite. */
  OMX_POST(s.inv - s.inv == 0.0f, "finite");
  return s;
}
#undef OMX_CONTRACT_STAGE

/*
 * ONE sample through the biased curve. ONE call to the transcendental, as before.
 *
 * At `character = -1` this is F(u) exactly: b = 0, off = F(0) = 0, inv = 1. There is no branch
 * for that case because there does not need to be — the arithmetic IS the identity.
 */
#define OMX_CONTRACT_STAGE "drive/sample"
static inline float omx_drive_sample(int curve, float u, const struct omx_drive_bias *s) {
  float y = (omx_drive_shape(curve, u + s->b) - s->off) * s->inv;
  /* R-056/laws: finite -- accepted 2026-09-17, docs/design/notes/2026-09-17-contracts-proposals.json.
   * composition of finite omx_drive_shape outputs and a finite s->inv. */
  OMX_POST(y - y == 0.0f, "finite");
  return y;
}
#undef OMX_CONTRACT_STAGE

/* ---- small in-place helpers ----------------------------------------------------------- */

/** Push `n` samples into a delay line and read back what was written `lat` samples ago. */
static inline void omx_drive_delay_write(float *line, uint32_t w, const float *in, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) line[(w + i) & OMX_DRIVE_DMASK] = in[i];
}
static inline void omx_drive_delay_read(const float *line, uint32_t w, uint32_t lat, float *out,
                                        uint32_t n) {
  for (uint32_t i = 0; i < n; i++) out[i] = line[(w + i - lat) & OMX_DRIVE_DMASK];
}

/* ---- the stage ------------------------------------------------------------------------ */

/*
 * ONE leg, ONE chunk. Returns the leg's mean-square SUMS for the auto-gain path (input and
 * output), because a linked pair needs both legs' numbers before either can be scaled — so the
 * gain is applied by the caller, after both legs have been shaped.
 *
 * `out` receives the shaped-but-not-yet-auto-gained, not-yet-trimmed result.
 */
#define OMX_CONTRACT_STAGE "drive/leg-chunk"
static inline void omx_drive_leg_chunk(struct omx_drive_leg *g, const struct omx_drive *p,
                                       const float *in, uint32_t n, float *out, double *sum_in,
                                       double *sum_out) {
  OMX_PRE(omx_block_finite(in, n), "finite-in");
  const int shaper = p->curve;
  const int exciter = (p->curve == OMX_DRIVE_EXCITER);
  const uint32_t lat = omx_oversampler_latency_for((uint32_t)p->os_factor);

  /* 1. the complementary band split, at the base rate. */
  if (p->band == OMX_DRIVE_BAND_LOW || p->band == OMX_DRIVE_BAND_HIGH) {
    memcpy(g->keep, in, n * sizeof(float));
    /* keep := LP(x) */
    for (uint32_t i = 0; i < n; i++) g->keep[i] = omx_biquad(g->keep[i], p->band_c, g->band_st);
    if (p->band == OMX_DRIVE_BAND_LOW) {
      /* the curve sees the LOW band; the HIGH half bypasses */
      for (uint32_t i = 0; i < n; i++) {
        g->curve_in[i] = g->keep[i];
        g->keep[i] = in[i] - g->keep[i];
      }
    } else {
      /* the curve sees the HIGH band (x - LP(x)); the LOW half bypasses */
      for (uint32_t i = 0; i < n; i++) g->curve_in[i] = in[i] - g->keep[i];
    }
  } else {
    memcpy(g->curve_in, in, n * sizeof(float));
    memset(g->keep, 0, n * sizeof(float));
    if (p->band == OMX_DRIVE_BAND_TILT) {
      for (uint32_t i = 0; i < n; i++) g->curve_in[i] = omx_biquad(g->curve_in[i], p->tilt_c, g->tilt_st);
    }
  }

  /* 2. the dry taps go into the compensation line BEFORE anything is read out of it, so a
   *    latency of 0 (factor 1) reads back exactly what was just written. */
  omx_drive_delay_write(g->dline_x, g->dw, in, n);
  omx_drive_delay_write(g->dline_keep, g->dw, g->keep, n);

  /* 3. up -> shape -> down. */
  omx_oversampler_up(&g->os, g->curve_in, n, g->up);
  const uint32_t n_up = n * (uint32_t)p->os_factor;
  const float drive = p->drive_lin;
  const float w = p->even_w;
  const struct omx_drive_bias bias = omx_drive_bias_for(shaper, w);
  for (uint32_t i = 0; i < n_up; i++) g->up[i] = omx_drive_sample(shaper, drive * g->up[i], &bias);
  omx_oversampler_down(&g->os, g->up, n, g->wet);

  /* 4. un-tilt (the exact inverse of step 1's shelf, so the dry path's tone is untouched),
   *    then the DC blocker — needed whenever any EVEN content was asked for, because an even
   *    residue carries DC by construction — then the optional roll-off. */
  if (p->band == OMX_DRIVE_BAND_TILT) {
    for (uint32_t i = 0; i < n; i++) g->wet[i] = omx_biquad(g->wet[i], p->tilt_inv_c, g->tiltinv_st);
  }
  if (w > 0.0f) {
    float x1 = g->dc_x, y1 = g->dc_y;
    for (uint32_t i = 0; i < n; i++) {
      float x = g->wet[i];
      float y = x - x1 + p->dc_coeff * y1;
      x1 = x;
      y1 = y;
      g->wet[i] = y;
    }
    g->dc_x = x1;
    g->dc_y = y1;
  }
  if (p->hf_on) {
    for (uint32_t i = 0; i < n; i++) g->wet[i] = omx_biquad(g->wet[i], p->hf_c, g->hf_st);
  }

  /* 5. mix back against the DELAY-COMPENSATED dry. */
  float dx[OMX_DRIVE_CHUNK], dk[OMX_DRIVE_CHUNK];
  omx_drive_delay_read(g->dline_x, g->dw, lat, dx, n);
  omx_drive_delay_read(g->dline_keep, g->dw, lat, dk, n);
  g->dw = (g->dw + n) & OMX_DRIVE_DMASK;

  /*
   * BOTH topologies are written as `dry + mix * delta`, and that form is not cosmetic: the
   * algebraically equal `keep + (1-mix)*(dry-keep) + mix*wet` is NOT bit-identical to `dry` at
   * mix 0 once `keep` is non-zero, because `keep + (dry - keep)` rounds. Written this way, mix 0
   * multiplies the whole wet path by exactly 0.0f and leaves the delayed input untouched — which
   * is what "mix = 0 is bit-identical dry" has to mean at EVERY band, not only at FULL.
   *
   * PARALLEL (exciter): the driven band is ADDED on top of the FULL dry signal — what makes an
   * enhancer an enhancer and not a distortion. CROSSFADE: `keep + wet` is the fully shaped strip
   * (the band the curve did not see, plus the band it did), faded against the untouched input.
   */
  const float mix = p->mix;
  double si = 0.0, so = 0.0;
  for (uint32_t i = 0; i < n; i++) {
    float dry = dx[i];
    float delta = exciter ? g->wet[i] : (g->wet[i] + dk[i] - dry);
    float y = dry + mix * delta;
    out[i] = y;
    si += (double)dry * (double)dry;
    so += (double)y * (double)y;
  }
  *sum_in = si;
  *sum_out = so;
  /* R-056/laws: finite -- corrected 2026-09-17 from the cluster's output-le-input-plus,
   * docs/design/notes/2026-09-17-contracts-proposals.json (`mix` is a dry/wet blend fraction,
   * not the makeup-gain shape that law names). dry/delta are each finite and mix is clamped
   * upstream, so y stays finite at every sample. */
  OMX_POST(omx_block_finite(out, n), "finite-out");
}
#undef OMX_CONTRACT_STAGE

/*
 * THE AUTO-GAIN, MEASURED and never estimated (§4d).
 *
 * A one-pole mean-square of the stage input and of the shaped output over the same ~50 ms window,
 * and the applied gain is sqrt(ms_in / ms_out), itself smoothed by the same pole.
 *
 * HELD below -90 dBFS input, not tracked. A level ratio taken on digital silence is a
 * measurement of nothing that reads as a number, and the console has paid for that lesson twice
 * (the house rule: "A RATIO SURVIVES A SILENT DESK; AN ABSOLUTE DOES NOT"). Hard-clamped to +-24 dB
 * so no arrangement of inputs can turn this into a fader.
 */
/* The detector window, as a TIME — the number the prose above quotes, now where the prose is and
 * in the units the tier's one pole constructor takes (`omx_pole_from_time_ms`, omx_envelope.h). It was
 * a bare `0.05f` seconds inside `resolve_drive()` in mixer_rt.c, which is the file that clocks it,
 * not the file that owns it. */
#define OMX_DRIVE_AG_WINDOW_MS 50.0f
#define OMX_DRIVE_AG_FLOOR_MS 1e-9f /* (-90 dBFS)^2 */
#define OMX_DRIVE_AG_MAX 15.848932f /* +24 dB */
#define OMX_DRIVE_AG_MIN 0.063095734f /* -24 dB */

#define OMX_CONTRACT_STAGE "drive/time-constants"
static inline void omx_drive_time_constants(struct omx_drive *o, float sr) {
  /* CONTRACT (omx_contract.h). A positive rate gives two poles in [0, 1): the DC blocker's
   * clamped to [0, 0.999999], the auto-gain's by omx_pole_from_time_ms's own POST. */
  OMX_PRE(sr > 0.0f && sr - sr == 0.0f, "positive-finite-rate");
  float dc = 1.0f - 2.0f * 3.14159265f * 5.0f / sr;
  o->dc_coeff = dc > 0.999999f ? 0.999999f : (dc < 0.0f ? 0.0f : dc);
  o->ag_coeff = omx_pole_from_time_ms(OMX_DRIVE_AG_WINDOW_MS, sr);
  OMX_POST(o->dc_coeff >= 0.0f && o->dc_coeff < 1.0f, "dc-pole-in-range");
  OMX_POST(o->ag_coeff >= 0.0f && o->ag_coeff < 1.0f, "ag-pole-in-range");
}
#undef OMX_CONTRACT_STAGE

#define OMX_CONTRACT_STAGE "drive/auto-gain"
static inline float omx_drive_auto_gain(struct omx_drive_ag *ag, const struct omx_drive *p,
                                        double sum_in, double sum_out, uint32_t n) {
  if (!p->auto_gain || n == 0) {
    ag->gain = 1.0f;
    ag->primed = 0;
    return 1.0f;
  }
  float ms_in = (float)(sum_in / (double)n);
  float ms_out = (float)(sum_out / (double)n);
  /* A NaN or Inf in the chunk makes its mean square NaN; fed into the one-pole detectors it
   * would poison ag->ms_in/ms_out for the life of the strip and every later gain with them
   * (audit D7's second half — the clamp pair below compares, and a comparison never catches a
   * NaN). A measurement that is not a number is treated as silence: HOLD the gain, touch no state. */
  if (!isfinite(ms_in) || !isfinite(ms_out)) {
    if (!ag->primed) { ag->gain = 1.0f; ag->primed = 1; }
    return ag->gain;
  }
  /* `ag_coeff` is the PER-SAMPLE pole, the same convention resolve_dyn()'s attack/release
   * coefficients use. The detectors update once per CHUNK, so it is raised to the chunk's own
   * length — otherwise the declared 50 ms window would actually be 50 ms x CHUNK (3.2 s at
   * 96 kHz), which is a control that does nothing you can hear it do. */
  float a = powf(p->ag_coeff, (float)n);
  /* {@link omx_onepole_toward} — the INCREMENT kernel, not the convex {@link omx_onepole}. Same
   * convention (the parameter is the POLE), declared difference: with identical input and output
   * energies the two detectors track the SAME float sequence exactly, the ratio is exactly 1.0f,
   * and the gain stays exactly 1.0f — which is what keeps mix 0 bit-identical through the
   * auto-gain as well as through the mix. mix_dsp.h's S1 block carries the reasoning. */
  omx_onepole_toward(&ag->ms_in, ms_in, a);
  omx_onepole_toward(&ag->ms_out, ms_out, a);
  if (ag->ms_in < OMX_DRIVE_AG_FLOOR_MS || ag->ms_out < OMX_DRIVE_AG_FLOOR_MS) {
    if (!ag->primed) { ag->gain = 1.0f; ag->primed = 1; }
    return ag->gain; /* HOLD — never a ratio taken on silence */
  }
  float want = sqrtf(ag->ms_in / ag->ms_out);
  if (want > OMX_DRIVE_AG_MAX) want = OMX_DRIVE_AG_MAX;
  if (want < OMX_DRIVE_AG_MIN) want = OMX_DRIVE_AG_MIN;
  if (!ag->primed) { ag->gain = want; ag->primed = 1; }
  else omx_onepole_toward(&ag->gain, want, a);
  /* R-056/laws: finite -- accepted 2026-09-17, docs/design/notes/2026-09-17-contracts-proposals.json.
   * want and ag->gain are hard-clamped to [OMX_DRIVE_AG_MIN, OMX_DRIVE_AG_MAX] before use, and
   * the floor-ms guard above keeps the sqrt argument away from zero. */
  OMX_POST(ag->gain - ag->gain == 0.0f && ag->gain <= OMX_DRIVE_AG_MAX &&
           ag->gain >= OMX_DRIVE_AG_MIN, "finite-clamped-gain");
  return ag->gain;
}
#undef OMX_CONTRACT_STAGE

/*
 * Process one block IN PLACE. `l` is always driven; `r` may be NULL (mono).
 *
 * A DISABLED atom, a NULL state, or a state built for another factor is a PASSTHROUGH that
 * leaves every sample bit-identical AND every state word untouched — the disabled-slot contract
 * omx_dynamics and omx_biquad_cascade already carry, and the whole of "zero cost when
 * disengaged". RT-safe: fixed work per sample, no allocation, no lock, no IO.
 */
#undef OMX_CONTRACT_STAGE
#define OMX_CONTRACT_STAGE "drive"
static inline void omx_drive_process(float *l, float *r, uint32_t n, const struct omx_drive *p,
                                     struct omx_drive_state *s) {
  /* CONTRACT (omx_contract.h). The one NONLINEAR stage on the strip, and the only one whose
   * error budget is about ALIASING rather than about level. Its declared laws:
   *
   *  - the oversampling factor is 1, 2 or 4 and the state is built for THAT factor (a mismatch
   *    is rebuilt below, at the block boundary, never run as another factor's history);
   *  - mix is 0..1 (the ROW carries percent; the resolver divides), and at 0 the stage is identity
   *    DELAYED by its declared latency, which the battery and test/fx/drive.test.c both assert;
   *  - the curve is bounded, so a finite input gives a finite output whatever the drive. */
  OMX_PRE(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-in");
  /* R-056 finding, 2026-09-17 (rt_alloc.test.c's real-render corpus): the factor/mix/state
   * checks are about a stage that is about to RUN; resolve_fx_drive's own early return on
   * `!enabled` leaves os_factor/mix at whatever the struct held before, so checking them
   * ahead of the enabled-gate fired on every ordinary disabled drive slot. Same fix as
   * omx_dynamics_keyed's own ratio/makeup checks just above it in this file's history. */
  if (!p->enabled || n == 0 || s == NULL) return;
  OMX_PRE(p->os_factor == 1 || p->os_factor == 2 || p->os_factor == 4, "factor-is-1-2-or-4");
  OMX_PRE(p->mix >= 0.0f && p->mix <= 1.0f, "mix-in-unit-range");
  /* THE STATE IS REBUILT HERE, AT A BLOCK BOUNDARY, AND NOWHERE ELSE WHILE IT CAN RUN. A factor
   * change or a re-enable used to re-init the state from the CONTROL thread, in place, under the
   * block in flight: the kernel checked the factor once, the setter then memset the history the
   * chunk loop was walking, and the block came out neither processed nor dry — up to 1.42 away
   * from both on a 0.5 sine (2026-09-25-native-fx-rt-review.md F3). Now the control thread only
   * publishes the factor and a reset generation on the atom, and the ONE writer of the state —
   * this function — rebuilds it before its first chunk when either differs. A block that started
   * before the publication runs to its end on the history it began with. */
  if (s->factor != omx_drive_factor_of(p->os_factor) || s->gen != p->reset_gen) {
    omx_drive_state_init(s, p->os_factor);
    s->gen = p->reset_gen;
  }
  OMX_INVARIANT(s->factor == omx_drive_factor_of(p->os_factor), "state-built-for-the-factor");

  uint32_t done = 0;
  while (done < n) {
    uint32_t c = n - done;
    if (c > OMX_DRIVE_CHUNK) c = OMX_DRIVE_CHUNK;
    float outl[OMX_DRIVE_CHUNK], outr[OMX_DRIVE_CHUNK];
    double il = 0.0, ol = 0.0, ir = 0.0, orr = 0.0;
    omx_drive_leg_chunk(&s->l, p, l + done, c, outl, &il, &ol);
    if (r) omx_drive_leg_chunk(&s->r, p, r + done, c, outr, &ir, &orr);

    float gl, gr;
    if (r && p->stereo_link) {
      /* ONE path from the two legs' summed energy, applied to both — gain riding can never
       * shift the image. */
      gl = gr = omx_drive_auto_gain(&s->ag_l, p, il + ir, ol + orr, c * 2u);
    } else {
      gl = omx_drive_auto_gain(&s->ag_l, p, il, ol, c);
      gr = r ? omx_drive_auto_gain(&s->ag_r, p, ir, orr, c) : 1.0f;
    }

    /* RAMPED, not stepped, via omx_ramp.h's omx_ramp — the same click discipline omx_fill_gain
     * and omx_mix_strip already keep, and its isfinite guard is what a poisoned auto-gain (a
     * NaN/Inf block earlier) recovers from instead of carrying the poison forward forever. */
    const float trim = p->trim_lin;
    struct omx_ramp rl = omx_ramp_begin(&s->prev_gl, gl, c);
    for (uint32_t i = 0; i < c; i++) l[done + i] = outl[i] * omx_ramp_at(rl, i) * trim;
    omx_ramp_end(rl, &s->prev_gl);
    if (r) {
      struct omx_ramp rr = omx_ramp_begin(&s->prev_gr, gr, c);
      for (uint32_t i = 0; i < c; i++) r[done + i] = outr[i] * omx_ramp_at(rr, i) * trim;
      omx_ramp_end(rr, &s->prev_gr);
    }
    done += c;
  }
  OMX_POST(omx_block_finite(l, n) && omx_block_finite(r, n), "finite-out");
  OMX_POST(s->prev_gl - s->prev_gl == 0.0f && s->prev_gr - s->prev_gr == 0.0f,
           "auto-gain-state-finite");
}
#undef OMX_CONTRACT_STAGE

#endif /* OMX_MIX_DRIVE_H */
