// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "../fx/fx_rates.h"

/* ---- EQ design and the matched pair ---------------------------------------------------------- */

/* The steady-state magnitude of a float section at `hz`, measured by running a sine through it. */
static double section_mag_at(const float c[5], double hz, double sr) {
  float st[4] = {0};
  const double w = 2.0 * M_PI * hz / sr;
  const uint32_t settle = (uint32_t)(sr * 0.25), meas = (uint32_t)(sr * 0.25);
  double re = 0.0, im = 0.0;
  for (uint32_t i = 0; i < settle + meas; i++) {
    const float y = omx_biquad(sinf((float)(w * i)), c, st);
    if (i >= settle) { re += y * cos(w * i); im += y * sin(w * i); }
  }
  return 20.0 * log10(2.0 * sqrt(re * re + im * im) / meas);
}

/* ---- the matched design against its ANALOGUE prototype (operator ruling, 2026-09-14) -------- */

/* The prototypes, transcribed from the filter definitions and not from the design under test: s is
 * normalised to w0, so s = i*x with x = f/f0, and A = 10^(g/40). Returned as |H(i*x)|^2 (power). */
enum { EQP_PEAK, EQP_HIGHSHELF, EQP_LOWSHELF, EQP_LOWPASS, EQP_HIGHPASS, EQP_NOTCH };
static double eqp_proto_pow(int kind, double x, double q, double A, double sa) {
  const double u = 1.0 - x * x, ni = sa * x / q;
  switch (kind) {
    case EQP_PEAK: /* (s^2 + A s/Q + 1) / (s^2 + s/(A Q) + 1) */
      return (u * u + (A * x / q) * (A * x / q)) / (u * u + (x / (A * q)) * (x / (A * q)));
    case EQP_HIGHSHELF: /* A (A s^2 + sqrt(A) s/Q + 1) / (s^2 + sqrt(A) s/Q + A) */
      return A * A * ((1 - A * x * x) * (1 - A * x * x) + ni * ni) / ((A - x * x) * (A - x * x) + ni * ni);
    case EQP_LOWSHELF: /* A (s^2 + sqrt(A) s/Q + A) / (A s^2 + sqrt(A) s/Q + 1) */
      return A * A * ((A - x * x) * (A - x * x) + ni * ni) / ((1 - A * x * x) * (1 - A * x * x) + ni * ni);
    case EQP_LOWPASS: return 1.0 / (u * u + (x / q) * (x / q));       /* 1 / (s^2 + s/Q + 1) */
    case EQP_HIGHPASS: return x * x * x * x / (u * u + (x / q) * (x / q)); /* s^2 / (...) */
    default: return u * u / (u * u + (x / q) * (x / q));               /* (s^2 + 1) / (...) */
  }
}
static double eqp_analogue_pow(int kind, double f, double f0, double q, double gdb) {
  const double A = pow(10.0, gdb / 40.0);
  return eqp_proto_pow(kind, f / f0, q, A, sqrt(A));
}
static double eqp_analogue_db(int kind, double f, double f0, double q, double gdb) {
  return 10.0 * log10(eqp_analogue_pow(kind, f, f0, q, gdb));
}
static enum omx_eq_kind eqp_design_kind(int kind) {
  static const enum omx_eq_kind k[] = {OMX_EQ_PEAKING, OMX_EQ_HIGHSHELF, OMX_EQ_LOWSHELF,
                                       OMX_EQ_LOWPASS, OMX_EQ_HIGHPASS, OMX_EQ_NOTCH};
  return k[kind];
}

/* The RBJ cookbook (bilinear, pre-warped at f0), transcribed: the design the matched one replaced,
 * kept as the BEFORE every AFTER below is judged against. Not omx-dsp code. */
static void eqp_cookbook(int kind, double f0, double q, double gdb, double fs, double c[5]) {
  const double w0 = 2.0 * M_PI * f0 / fs, cw = cos(w0), sw = sin(w0);
  const double al = sw / (2.0 * q), A = pow(10.0, gdb / 40.0), s = 2.0 * sqrt(A) * al;
  double b0, b1, b2, a0, a1, a2;
  switch (kind) {
    case EQP_PEAK:
      b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A;
      a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; break;
    case EQP_HIGHSHELF:
      b0 = A * (A + 1 + (A - 1) * cw + s); b1 = -2 * A * (A - 1 + (A + 1) * cw);
      b2 = A * (A + 1 + (A - 1) * cw - s);
      a0 = A + 1 - (A - 1) * cw + s; a1 = 2 * (A - 1 - (A + 1) * cw);
      a2 = A + 1 - (A - 1) * cw - s; break;
    case EQP_LOWSHELF:
      b0 = A * (A + 1 - (A - 1) * cw + s); b1 = 2 * A * (A - 1 - (A + 1) * cw);
      b2 = A * (A + 1 - (A - 1) * cw - s);
      a0 = A + 1 + (A - 1) * cw + s; a1 = -2 * (A - 1 + (A + 1) * cw);
      a2 = A + 1 + (A - 1) * cw - s; break;
    case EQP_LOWPASS:
      b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
      a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    case EQP_HIGHPASS:
      b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
      a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
    default:
      b0 = 1; b1 = -2 * cw; b2 = 1;
      a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
  }
  c[0] = b0 / a0; c[1] = b1 / a0; c[2] = b2 / a0; c[3] = a1 / a0; c[4] = a2 / a0;
}

/* Orfanidis, "Digital Parametric Equalizer Design With Prescribed Nyquist-Frequency Gain" (JAES
 * 1997), second order, Nyquist gain prescribed from the prototype: an INDEPENDENT published design,
 * built from different algebra, EXACT at DC, at f0 and at Nyquist by construction. It is not what
 * omx-dsp runs; it is the control that the prototype above is the one the published method
 * targets. Returns 0 when the method refuses (a band edge past Nyquist: its domain, not a fault). */
static int eqp_orfanidis(double f0, double q, double gdb, double fs, double c[5]) {
  const double G0 = 1.0, G = pow(10.0, gdb / 20.0), GB = sqrt(G * G0);
  const double G1 = sqrt(eqp_analogue_pow(EQP_PEAK, fs / 2.0, f0, q, gdb));
  const double w0 = 2.0 * M_PI * f0 / fs, Dw = w0 / q;
  if (w0 + Dw / 2.0 >= M_PI || w0 - Dw / 2.0 <= 0.0) return 0;
  const double F = fabs(G * G - GB * GB), G00 = fabs(G * G - G0 * G0);
  const double F00 = fabs(GB * GB - G0 * G0), G01 = fabs(G * G - G0 * G1);
  const double G11 = fabs(G * G - G1 * G1), F01 = fabs(GB * GB - G0 * G1);
  const double F11 = fabs(GB * GB - G1 * G1);
  if (F < 1e-300 || G00 < 1e-300 || F11 < 1e-300) return 0;
  const double t0 = tan(w0 / 2.0), W2 = sqrt(G11 / G00) * t0 * t0;
  const double DW = (1.0 + sqrt(F00 / F11) * W2) * tan(Dw / 2.0);
  const double C = F11 * DW * DW - 2.0 * W2 * (F01 - sqrt(F00 * F11));
  const double D = 2.0 * W2 * (G01 - sqrt(G00 * G11));
  const double AA = sqrt((C + D) / F), BB = sqrt((G * G * C + GB * GB * D) / F);
  const double den = 1.0 + W2 + AA;
  c[0] = (G1 + G0 * W2 + BB) / den; c[1] = -2.0 * (G1 - G0 * W2) / den;
  c[2] = (G1 - BB + G0 * W2) / den; c[3] = -2.0 * (1.0 - W2) / den;
  c[4] = (1.0 + W2 - AA) / den;
  for (int i = 0; i < 5; i++) if (!isfinite(c[i])) return 0;
  return 1;
}

/* |H(e^jw)|^2 of {b0,b1,b2,a1,a2}, evaluated from the coefficients. */
static double eqp_closed_pow_cs(const double c[5], double cw, double sw) {
  const double c2 = 2.0 * cw * cw - 1.0, s2 = 2.0 * sw * cw;
  const double nr = c[0] + c[1] * cw + c[2] * c2, ni = -(c[1] * sw + c[2] * s2);
  const double dr = 1.0 + c[3] * cw + c[4] * c2, di = -(c[3] * sw + c[4] * s2);
  return (nr * nr + ni * ni) / (dr * dr + di * di);
}
static double eqp_closed_pow(const double c[5], double f, double fs) {
  const double w = 2.0 * M_PI * f / fs;
  return eqp_closed_pow_cs(c, cos(w), sin(w));
}
/* cos and sin of every whole-hertz bin up to 20 kHz at the rate in hand, filled once per rate: the
 * sweeps below evaluate thousands of designs on the same grid. */
static double g_eqp_cos[20001], g_eqp_sin[20001], g_eqp_trig_fs;
static void eqp_trig_at(double fs) {
  if (g_eqp_trig_fs == fs) return;
  for (int f = 0; f <= 20000; f++) { g_eqp_cos[f] = cos(2.0 * M_PI * f / fs); g_eqp_sin[f] = sin(2.0 * M_PI * f / fs); }
  g_eqp_trig_fs = fs;
}
static double eqp_closed_db(const double c[5], double f, double fs) { return 10.0 * log10(eqp_closed_pow(c, f, fs)); }

/* The worst |digital - analogue| in dB over [lo, hi] (whole hertz, hi <= 20 kHz) on a 1 Hz grid. Bins where the PROTOTYPE is
 * below `floor_db` are skipped: the dB distance to a true null (a notch centre, a deep stopband) is
 * unbounded for any filter without one at exactly the same place. Tracked as the extreme power
 * RATIOS and taken to dB once, which is the same max as the per-bin dB distance (log is monotone). */
static double eqp_worst_err(const double c[5], int kind, double f0, double q, double gdb, double fs,
                            double lo, double hi, double floor_db) {
  const double floor_pow = pow(10.0, floor_db / 10.0), A = pow(10.0, gdb / 40.0), sa = sqrt(A);
  double rmax = 1.0, rmin = 1.0;
  eqp_trig_at(fs);
  for (int f = (int)lo; f <= (int)hi; f++) {
    const double a = eqp_proto_pow(kind, f / f0, q, A, sa);
    if (a < floor_pow) continue;
    const double r = eqp_closed_pow_cs(c, g_eqp_cos[f], g_eqp_sin[f]) / a;
    if (r > rmax) rmax = r;
    if (r < rmin) rmin = r;
  }
  return fmax(10.0 * log10(rmax), -10.0 * log10(rmin));
}

/* The response of omx_biquad itself, float32, by impulse: what the DSP RUNS, not what the
 * coefficients say. A swapped coefficient, a flipped sign, an a-term added instead of subtracted
 * are invisible to the closed form and obvious here. The tail is run until it is exactly zero. */
#define EQP_IMPULSE_N 262144
static float g_eqp_imp[EQP_IMPULSE_N];
static int g_eqp_imp_len;
static void eqp_impulse(const double c[5]) {
  float cf[5], st[4] = {0};
  for (int i = 0; i < 5; i++) cf[i] = (float)c[i];
  g_eqp_imp_len = EQP_IMPULSE_N;
  g_eqp_imp[0] = omx_biquad(1.0f, cf, st);
  for (int i = 1; i < EQP_IMPULSE_N; i++) {
    g_eqp_imp[i] = omx_biquad(0.0f, cf, st);
    if (g_eqp_imp[i] == 0.0f && i > 4096) { g_eqp_imp_len = i; break; } /* the tail is gone */
  }
}
/* A single-bin DFT of that impulse response; the e^-jwi phasor is rotated, not re-evaluated, and
 * re-seeded from cos/sin every 4096 samples so its drift stays at rounding level. */
static double eqp_impulse_db(double f, double fs) {
  const double w = 2.0 * M_PI * f / fs, cr = cos(w), sr = sin(w);
  double re = 0.0, im = 0.0, pr = 1.0, pi = 0.0;
  for (int i = 0; i < g_eqp_imp_len; i++) {
    if ((i & 4095) == 0) { pr = cos(w * i); pi = -sin(w * i); }
    re += (double)g_eqp_imp[i] * pr; im += (double)g_eqp_imp[i] * pi;
    const double t = pr * cr + pi * sr; pi = pi * cr - pr * sr; pr = t;
  }
  return 10.0 * log10(re * re + im * im);
}

/* The four bands the ruling named. */
static const struct { const char *name; int kind; double f0, q, g; } EQP_RULING[] = {
  {"bell +12 dB 16 kHz Q 2", EQP_PEAK, 16000.0, 2.0, 12.0},
  {"bell -12 dB 10 kHz Q 1", EQP_PEAK, 10000.0, 1.0, -12.0},
  {"high shelf +6 dB 12 kHz", EQP_HIGHSHELF, 12000.0, 0.707, 6.0},
  {"low shelf -6 dB 80 Hz", EQP_LOWSHELF, 80.0, 0.707, -6.0},
};

/* THE MATCHED DESIGN AGAINST ITS ANALOGUE PROTOTYPE — moved from openmixer's
 * packages/pipewire-native/tools/eq-matched-probe.c (the before/after of the 2026-09-14 ruling: the
 * EQ must match the analogue prototype to Nyquist BY COEFFICIENT DESIGN at the rate the desk runs;
 * the bilinear cookbook was 1.28 dB low at 20 kHz on a +12 dB bell at 16 kHz Q 2 at 96 kHz). The
 * three controls come first: an oracle that cannot detect presence cannot report absence. */
static void eqp_prototype_checks(void) {
  double c[5];
  /* CONTROL (a): the cookbook's PUBLISHED error, at the single rate it was published at (96 kHz,
   * docs/design/notes/2026-09-14-oversampling-vs-192k.md §5: -1.28 dB at 20 kHz). */
  eqp_cookbook(EQP_PEAK, 16000.0, 2.0, 12.0, 96000.0, c);
  const double e20 = eqp_closed_db(c, 20000.0, 96000.0) - eqp_analogue_db(EQP_PEAK, 20000.0, 16000.0, 2.0, 12.0);
  ok(e20 > -1.33 && e20 < -1.23, "control: the cookbook bell +12/16k/Q2 reads its published -1.28 dB at 20 kHz, 96 kHz",
     e20, -1.28);
  /* CONTROL (c): the matched bell with its pole damping undivided by A (the most plausible
   * transcription slip) is caught by the same measurement, loudly. One rate: it tests the oracle. */
  {
    double p[3], z[3];
    const double A = pow(10.0, 12.0 / 40.0), w0 = 2.0 * M_PI * 16000.0 / 96000.0;
    omx_eq_matched_pair(w0, 1.0 / (2.0 * 2.0), p); /* the A is missing, on purpose */
    omx_eq_matched_pair(w0, A / (2.0 * 2.0), z);
    const double k = p[2] / z[2];
    c[0] = k; c[1] = k * z[0]; c[2] = k * z[1]; c[3] = p[0]; c[4] = p[1];
    const double e = eqp_worst_err(c, EQP_PEAK, 16000.0, 2.0, 12.0, 96000.0, 1000.0, 20000.0, -200.0);
    ok(e > 1.0, "control: a sabotaged matched bell (pole damping missing its A) is > 1 dB off the prototype", e, 1.0);
  }
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const double fs = OMX_FX_RME_RATES[ri], nyq = fs * 0.5;
    /* the span the ruling measured, 1..20 kHz, stopped at Nyquist where Nyquist is lower (32 kHz) */
    const double top = nyq < 20000.0 ? nyq : 20000.0;
    /* CONTROL (d): Orfanidis is EXACT at DC, f0 and Nyquist by construction, so reading zero there
     * proves the prototype above is the one the published method targets. It refuses at 32 kHz (a
     * band edge past Nyquist: its own domain); at 96 kHz it must answer. Points are exactly DC, f0
     * and Nyquist: a point 1e-4 Hz off Nyquist read 2.6e-9 dB and looked like a design error. */
    if (eqp_orfanidis(16000.0, 2.0, 12.0, fs, c)) {
      const double pts[3] = {0.0, 16000.0, nyq};
      double w = 0.0;
      for (int i = 0; i < 3; i++)
        w = fmax(w, fabs(eqp_closed_db(c, pts[i], fs) - eqp_analogue_db(EQP_PEAK, pts[i], 16000.0, 2.0, 12.0)));
      ok(w < 1e-9, "control: Orfanidis is exact at DC / f0 / Nyquist against the prototype", w, 1e-9);
    } else {
      ok(fs != 96000.0, "control: Orfanidis answers the +12/16k/Q2 band at 96 kHz", fs, 96000.0);
    }
    for (size_t b = 0; b < sizeof EQP_RULING / sizeof EQP_RULING[0]; b++) {
      const int kind = EQP_RULING[b].kind;
      const double f0 = EQP_RULING[b].f0, q = EQP_RULING[b].q, g = EQP_RULING[b].g;
      double bc[5], mc[5];
      eqp_cookbook(kind, f0, q, g, fs, bc);
      omx_eq_design(eqp_design_kind(kind), f0, q, g, fs, mc);
      const double eb = eqp_worst_err(bc, kind, f0, q, g, fs, 1000.0, top, -200.0);
      const double em = eqp_worst_err(mc, kind, f0, q, g, fs, 1000.0, top, -200.0);
      /* AFTER is no further from the prototype than BEFORE, at every rate (the whole requirement
       * at 44.1/48 kHz, where no second-order design reaches 0.1 dB at 20 kHz) */
      ok(em <= eb, "the matched band is no further from the prototype than the cookbook, 1 kHz..20 kHz", em, eb);
      /* THE RULING'S TARGET: <= 0.1 dB to 20 kHz at 96 kHz, and at every rate from 88.2 kHz up,
       * where the warp only shrinks */
      if (fs >= 88200.0) ok(em <= 0.1, "the matched band is within 0.1 dB of the prototype to 20 kHz", em, 0.1);
      /* WHAT THE DSP RUNS: omx_biquad's float32 impulse response agrees with the closed form of the
       * same coefficients to 0.01 dB, 100 Hz..20 kHz (the low shelf at 80 Hz from 1 kHz, the span the
       * probe's table walked: below that, float32 coefficient rounding of a 80 Hz section at 192 kHz
       * moves it by up to 0.018 dB, which is the format's, not the design's) */
      eqp_impulse(mc);
      double gap = 0.0;
      for (double f = kind == EQP_LOWSHELF ? 1000.0 : 100.0; f <= top; f += 100.0)
        gap = fmax(gap, fabs(eqp_impulse_db(f, fs) - eqp_closed_db(mc, f, fs)));
      ok(gap < 0.01, "omx_biquad's float32 impulse response is the closed form of its coefficients", gap, 0.01);
    }
    /* THE PASS FILTERS AND THE NOTCH, over the span each serves (HPF_FREQ_RANGE 20..1000 Hz,
     * LPF_FREQ_RANGE 1..20 kHz, a notch anywhere in the band; corners stopped under Nyquist), every
     * f0 on a 1.06 ratio grid and the Butterworth-family / notch Qs, measured 20 Hz..20 kHz where the
     * prototype is above -20 dB. The high-pass is the cookbook's own design, bit for bit; the
     * low-pass (matched poles, fitted numerator) and the notch (matched poles,
     * the cookbook's numerator) are no further from the prototype than the cookbook at any of them. */
    static const double pq[3] = {0.5412, 0.707, 1.3066}, nq[5] = {0.3, 1.0, 4.0, 16.0, 116.0};
    const struct { int kind; double lo, hi; const double *qs; int nq; } S[3] = {
      {EQP_HIGHPASS, 20.0, 1000.0, pq, 3}, {EQP_LOWPASS, 1000.0, 20000.0, pq, 3}, {EQP_NOTCH, 20.0, 20000.0, nq, 5},
    };
    for (int i = 0; i < 3; i++) {
      double wb = 0.0, wm = 0.0, hp_dc = 0.0;
      for (double f0 = S[i].lo; f0 <= S[i].hi * 1.0001 && f0 < nyq * 0.999; f0 *= 1.06) {
        for (int k = 0; k < S[i].nq; k++) {
          double bc[5], mc[5];
          eqp_cookbook(S[i].kind, f0, S[i].qs[k], 0.0, fs, bc);
          omx_eq_design(eqp_design_kind(S[i].kind), f0, S[i].qs[k], 0.0, fs, mc);
          if (S[i].kind == EQP_HIGHPASS) {
            for (int j = 0; j < 5; j++) hp_dc = fmax(hp_dc, fabs(mc[j] - bc[j]));
            continue;
          }
          wb = fmax(wb, eqp_worst_err(bc, S[i].kind, f0, S[i].qs[k], 0.0, fs, 20.0, top, -20.0));
          wm = fmax(wm, eqp_worst_err(mc, S[i].kind, f0, S[i].qs[k], 0.0, fs, 20.0, top, -20.0));
        }
      }
      if (S[i].kind == EQP_HIGHPASS)
        ok(hp_dc == 0.0, "the high-pass is the cookbook's, bit for bit (unchanged by the ruling)", hp_dc, 0.0);
      else
        ok(wm <= wb, S[i].kind == EQP_LOWPASS ? "the matched low-pass is no further from the prototype than the cookbook"
                                              : "the matched notch is no further from the prototype than the cookbook", wm, wb);
    }
  }
}

static void arm_eq_design(void) {
  g_arm = "eq-design";
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const double sr = OMX_DECLARED_RATES[ri];
    float c[5];
    omx_eq_design_f(OMX_EQ_PEAKING, 1000.0, 1.0, 6.0, sr, c);
    ok(fabs(section_mag_at(c, 1000.0, sr) - 6.0) < 0.05, "a +6 dB bell reads +6 dB at its centre", section_mag_at(c, 1000.0, sr), 6.0);
    ok(fabs(section_mag_at(c, 40.0, sr)) < 0.1, "the bell is unity far below its centre", section_mag_at(c, 40.0, sr), 0.0);
    omx_eq_design_f(OMX_EQ_HIGHPASS, 200.0, M_SQRT1_2, 0.0, sr, c);
    ok(fabs(section_mag_at(c, 200.0, sr) + 3.01) < 0.1, "the Butterworth high-pass sits at -3 dB on its corner", section_mag_at(c, 200.0, sr), -3.01);
    ok(section_mag_at(c, 20.0, sr) < -35.0, "the high-pass rejects two decades down", section_mag_at(c, 20.0, sr), -35.0);
    omx_eq_design_f(OMX_EQ_LOWPASS, 2000.0, M_SQRT1_2, 0.0, sr, c);
    ok(fabs(section_mag_at(c, 2000.0, sr) + 3.01) < 0.15, "the fitted low-pass sits at -3 dB on its corner", section_mag_at(c, 2000.0, sr), -3.01);
    ok(fabs(section_mag_at(c, 50.0, sr)) < 0.05, "the low-pass is unity at DC", section_mag_at(c, 50.0, sr), 0.0);
    omx_eq_design_f(OMX_EQ_NOTCH, 1000.0, 4.0, 0.0, sr, c);
    ok(section_mag_at(c, 1000.0, sr) < -40.0, "the notch nulls its centre", section_mag_at(c, 1000.0, sr), -40.0);
    ok(fabs(section_mag_at(c, 100.0, sr)) < 0.05, "the notch is unity a decade below", section_mag_at(c, 100.0, sr), 0.0);
    omx_eq_design_f(OMX_EQ_LOWSHELF, 300.0, M_SQRT1_2, -9.0, sr, c);
    ok(fabs(section_mag_at(c, 20.0, sr) + 9.0) < 0.1, "the low shelf carries its gain at DC", section_mag_at(c, 20.0, sr), -9.0);
    omx_eq_design_f(OMX_EQ_HIGHSHELF, 3000.0, M_SQRT1_2, 4.0, sr, c);
    ok(fabs(section_mag_at(c, 30.0, sr)) < 0.05, "the high shelf is unity at DC", section_mag_at(c, 30.0, sr), 0.0);
    ok(fabs(section_mag_at(c, sr * 0.45, sr) - 4.0) < 0.15, "the high shelf carries its gain near Nyquist", section_mag_at(c, sr * 0.45, sr), 4.0);
    /* the double design and its float narrowing agree; every design keeps its poles inside */
    double d[5];
    omx_eq_design(OMX_EQ_PEAKING, 1000.0, 1.0, 6.0, sr, d);
    omx_eq_design_f(OMX_EQ_PEAKING, 1000.0, 1.0, 6.0, sr, c);
    for (int i = 0; i < 5; i++) ok(c[i] == (float)d[i], "the float design is the narrowed double design", c[i], d[i]);
    for (int k = 0; k <= 5; k++) {
      omx_eq_design((enum omx_eq_kind)k, 15000.0, 8.0, 12.0, sr, d);
      ok(fabs(d[3]) < 1.0 + d[4] && d[4] < 1.0, "an extreme design stays inside the unit circle", d[4], 1.0);
    }
  }
  double qs[2];
  ok(omx_eq_butterworth_qs(0, qs) == 1u && fabs(qs[0] - M_SQRT1_2) < 1e-12, "12 dB/oct is one section at 1/sqrt2", qs[0], M_SQRT1_2);
  ok(omx_eq_butterworth_qs(1, qs) == 2u && fabs(qs[0] * qs[1] - M_SQRT1_2) < 1e-6, "24 dB/oct's two Qs multiply to 1/sqrt2", qs[0] * qs[1], M_SQRT1_2);
  eqp_prototype_checks();
  expect_clean();
}
