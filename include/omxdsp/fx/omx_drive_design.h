/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_drive_design.h — the drive stage's four biquad sections, designed in C.
 *
 * Inside the console every coefficient `omx_drive.h` runs is designed in TypeScript
 * (`packages/server/src/console-rig.ts`'s `driveStateToNativeDrive` over `@freemixer/core`'s
 * `rbjSection`) and the C only RUNS it. A foreign LV2 host has no TypeScript in its process, so
 * the drive's third shell (mix_drive_lv2.h, the `omx-drive.lv2` bundle) needs the same four
 * designs reachable from C — exactly the position HRP's live shell was in, and this file is the
 * drive's `omx_hrp_peaking_coeffs`: a TWIN of the console's designer, not a second opinion.
 *
 * Held equal to the TypeScript by `omx_drive_design_corpus.h`, which both sides check:
 * `test/fx/drive_math.test.c` runs these functions over every row, and
 * `packages/server/src/drive-lv2-design-twins.test.ts` parses the same file and runs
 * `driveStateToNativeDrive` over it. Editing either designer's arithmetic turns one red.
 *
 * THE DESIGN IS MATCHED, NOT BILINEAR (operator ruling 2026-09-14, eq.ts's `rbjSection` doc):
 * the shelf is a matched-Z pole pair at `f0*sqrt(A)` over a zero pair at `f0/sqrt(A)`, both
 * damped `1/(2Q)`, unity at DC; the low-pass is matched-Z poles under a numerator FITTED to the
 * prototype's magnitude at DC, `f0` and Nyquist (Vicanek 2016), because its two zeros sit at
 * `s = infinity` and the map has nowhere to put them. test/fx/drive.test.c's own `rbj_lowpass` /
 * `rbj_highshelf` are the COOKBOOK (bilinear) and are deliberately not this: that oracle
 * measures the kernel's spectrum under sections it designs itself, and is indifferent to which
 * transcription placed them. The LV2 shell is not — it must push what the desk pushes.
 *
 * The clamps are the TypeScript's, term for term: the corner is held in [1, 0.999*Nyquist] and a
 * non-positive Q becomes 1e-3. Q is always SQRT1_2 here, the one value the console passes.
 */
#ifndef OMX_MIX_DRIVE_DESIGN_H
#define OMX_MIX_DRIVE_DESIGN_H

#include <math.h>
#include <stdint.h>

#include <omxdsp/fx/omx_drive.h>
#include <omxdsp/omx_matched_pair.h>

/** `DRIVE_TILT_SHELF_DB` in console-rig.ts — the tilt band's pre-emphasis, undone exactly. */
#define OMX_DRIVE_TILT_SHELF_DB 6.0
/** The Q every drive section is designed at: the one Butterworth Q, generated from core's
 *  BUTTERWORTH_Q in double (dsp-primitives §7), the value console-rig.ts designs with. */
#define OMX_DRIVE_DESIGN_Q OMX_BUTTERWORTH_Q_DOUBLE

/**
 * `rbjSection('highShelf', freq_hz, SQRT1_2, gain_db, rate)`, normalised to `{b0,b1,b2,a1,a2}`.
 * A 0 dB shelf is EXACTLY identity (both pairs come from the same call with the same arguments).
 */
#define OMX_CONTRACT_STAGE "drive/design-highshelf"
static inline void omx_drive_design_highshelf(float freq_hz, float gain_db, uint32_t rate,
                                              float out[5]) {
  /* CONTRACT (omx_contract.h). A finite corner and gain at a positive rate give five finite
   * coefficients over a STABLE pole pair (`a2 < 1`, `|a1| < 1 + a2`): the section the kernel
   * runs can never grow without bound. `out == NULL` or `rate == 0` writes nothing. */
  OMX_PRE(freq_hz - freq_hz == 0.0f && gain_db - gain_db == 0.0f, "finite-corner-and-gain");
  if (!out || rate == 0u) return;
  const double zeta = 1.0 / (2.0 * OMX_DRIVE_DESIGN_Q);
  const double w0 = omx_matched_w0((double)freq_hz, (double)rate);
  const double sa = sqrt(pow(10.0, (double)gain_db / 40.0));
  /* poles at f0*sqrt(A), zeros at f0/sqrt(A), both damped 1/(2Q) */
  omx_matched_pair_section(out, w0 * sa, zeta, w0 / sa, zeta);
  OMX_POST(omx_block_finite(out, 5u), "finite-section");
  OMX_POST(out[4] < 1.0f && fabsf(out[3]) < 1.0f + out[4], "stable-poles");
}
#undef OMX_CONTRACT_STAGE

/**
 * `rbjSection('lowpass', freq_hz, SQRT1_2, 0, rate)`, normalised. The numerator is fitted to
 * the prototype's magnitude-squared at DC (1), at `f0` (Q^2) and at Nyquist, then `b0,b1,b2`
 * are recovered from `N(1)`, `N(-1)` and the `sin^2(w/2)` expansion — eq.ts, line for line.
 */
#define OMX_CONTRACT_STAGE "drive/design-lowpass"
static inline void omx_drive_design_lowpass(float freq_hz, uint32_t rate, float out[5]) {
  /* CONTRACT (omx_contract.h). Same laws as the shelf: finite in, five finite coefficients
   * over a stable pole pair out, nothing written for a NULL out or a zero rate. */
  OMX_PRE(freq_hz - freq_hz == 0.0f, "finite-corner");
  if (!out || rate == 0u) return;
  const double qq = OMX_DRIVE_DESIGN_Q;
  const double sr = (double)rate, nyquist = sr * 0.5;
  const double f0 = omx_matched_f0((double)freq_hz, sr), w0 = 2.0 * M_PI * f0 / sr;
  const OmxMatchedPair p = omx_matched_pair(w0, 1.0 / (2.0 * qq));
  const double xn = nyquist / f0;
  const double un = 1.0 - xn * xn;
  const double t_pi = 1.0 / (un * un + (xn / qq) * (xn / qq));
  const double d_pi = 1.0 - p.p1 + p.p2;
  const double cw = cos(w0);
  const double dr = 1.0 + p.p1 * cw + p.p2 * cos(2.0 * w0);
  const double di = -(p.p1 * sin(w0) + p.p2 * sin(2.0 * w0));
  const double s0 = sin(w0 / 2.0) * sin(w0 / 2.0);
  const double u = p.dc * p.dc;
  const OmxMatchedNumerator n =
      omx_matched_fit_numerator(u, t_pi * d_pi * d_pi, qq * qq * (dr * dr + di * di), s0);
  omx_matched_section(out, n.b0, n.b1, n.b2, p);
  OMX_POST(omx_block_finite(out, 5u), "finite-section");
  OMX_POST(out[4] < 1.0f && fabsf(out[3]) < 1.0f + out[4], "stable-poles");
}
#undef OMX_CONTRACT_STAGE

/**
 * The bank `driveStateToNativeDrive` pushes, from its two frequency inputs: the band split's
 * low-pass and the tilt pair at `band_hz`, the roll-off at `hf_hz` (20 000 when the row says
 * `off` — a section is still designed so the C never runs an uninitialised one; `hf_on` is what
 * decides whether it runs). Only the four coefficient arrays are written.
 */
static inline void omx_drive_design_bank(struct omx_drive *o, float band_hz, float hf_hz,
                                         uint32_t rate) {
  omx_drive_design_lowpass(band_hz, rate, o->band_c);
  omx_drive_design_highshelf(band_hz, (float)OMX_DRIVE_TILT_SHELF_DB, rate, o->tilt_c);
  omx_drive_design_highshelf(band_hz, (float)-OMX_DRIVE_TILT_SHELF_DB, rate, o->tilt_inv_c);
  omx_drive_design_lowpass(hf_hz, rate, o->hf_c);
}

#endif /* OMX_MIX_DRIVE_DESIGN_H */
