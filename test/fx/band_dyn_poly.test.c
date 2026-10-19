// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file band_dyn_poly.test.c
 * @brief The band-dynamics loop (the de-esser's and the dynamic EQ band's) against the same loop on
 *        the libm pair, frozen in fixtures/band_dyn_libm.h, at the nine oracle rates.
 *
 * The declared bound: `g` within `s·1e-4 + 2e-5` dB of the libm loop's, `s` the characteristic's
 * steepest slope (`1 − 1/ratio` above, `ratio − 1` below) — 1e-4 dB is omx_lin_to_db_poly()'s
 * bound, 2e-5 dB omx_db_to_lin_poly()'s 2e-6 relative. Arms (a failure prints `FAIL [arm]`):
 *   gain    the de-esser's gain word (the stage and its GR meter) over every level a float holds,
 *           both modes, the contract's threshold, ratio and range travel
 *   output  every sample within the bound: split `|Δy| ≤ |Δg|·|B|`, wideband `|Δy| ≤ |Δg|·|x|`,
 *           plus the float rounding of the application
 *   unity   where the libm loop's level sits further than the bound inside the zero region of the
 *           characteristic (its offset is 0 at the level moved by the bound either way), the
 *           output is the libm loop's bit for bit
 *   state   the band histories and the detector cascade byte-identical (the detector never reads
 *           `g`); the GR reading within the bound
 *   chunk   the output does not depend on the block size (1, 61, 64, 1024), bit for bit, and each
 *           sample takes the gain omx_band_dyn_detect() gives it
 */
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omxdsp_definitions.h>
#include <omxdsp/fx/omx_deesser.h>
#include <omxdsp/omx_band_dyn.h>
#include <omxdsp/omx_eq_design.h>

#include "fixtures/band_dyn_libm.h"
#include "fx_rates.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/** @brief omx_lin_to_db_poly()'s declared bound against omx_lin_to_db(), dB. */
#define LIN_TO_DB_BOUND 1e-4
/** @brief omx_db_to_lin_poly()'s declared 2e-6 relative bound, in dB (rounded up). */
#define DB_TO_LIN_BOUND 2e-5
/** @brief The signal: 0.4 s at the highest oracle rate. */
#define MAXN 76800u

static int g_fail = 0, g_checks = 0;
static double g_worst_gain = 0.0, g_worst_out = 0.0;

static void check(int cond, const char *arm, const char *what, double sr) {
  g_checks++;
  if (!cond) {
    g_fail++;
    if (g_fail <= 40) printf("FAIL [%s] @ %.0f Hz: %s\n", arm, sr, what);
  }
}

/** @brief The steepest slope of the characteristic, dB of gain per dB of level. */
static double slope(int mode, double ratio) { return mode == OMX_DYN_BELOW ? ratio - 1.0 : 1.0 - 1.0 / ratio; }

/** @brief The gain bound, dB, for a characteristic. */
static double gain_bound_db(int mode, double ratio) { return slope(mode, ratio) * LIN_TO_DB_BOUND + DB_TO_LIN_BOUND; }

static struct omx_dyn dyn_atom(double sr, int mode, float thr, float ratio, float range) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = mode;
  d.gc.thresh_db = thr;
  d.gc.ratio = ratio;
  d.gc.knee_db = (float)OMX_DEESS_KNEE_DB;
  d.gc.range_db = range;
  d.gc.makeup_lin = 1.0f;
  d.detect = OMX_DETECT_PEAK;
  d.ovs_mode = OMX_DYN_OVS_OFF;
  d.attack_ms = (float)OMX_DEESS_ATTACK_RANGE_DEFAULT;
  d.attack_coeff = omx_pole_from_time_ms((float)OMX_DEESS_ATTACK_RANGE_DEFAULT, (float)sr);
  d.release_coeff = omx_pole_from_time_ms((float)OMX_DEESS_RELEASE_RANGE_DEFAULT, (float)sr);
  return d;
}

/* ---- gain: the stage's gain word and its meter, over every level a float holds ----------- */

static void arm_gain(void) {
  static const float THR[] = {(float)OMX_DEESS_THRESHOLD_RANGE_MIN, (float)OMX_DEESS_THRESHOLD_RANGE_DEFAULT,
                              (float)OMX_DEESS_THRESHOLD_RANGE_MAX};
  static const float RATIO[] = {1.1f, (float)OMX_DEESS_RATIO_RANGE_DEFAULT, (float)OMX_DEESS_RATIO_RANGE_MAX};
  static const float RANGE[] = {(float)OMX_DEESS_RANGE_RANGE_MIN, (float)OMX_DEESS_RANGE_RANGE_DEFAULT, -0.5f};
  for (unsigned a = 0; a < sizeof THR / sizeof THR[0]; a++)
    for (unsigned b = 0; b < sizeof RATIO / sizeof RATIO[0]; b++)
      for (unsigned c = 0; c < sizeof RANGE / sizeof RANGE[0]; c++) {
        struct omx_deess p;
        memset(&p, 0, sizeof p);
        p.enabled = 1;
        p.dyn = dyn_atom(48000.0, OMX_DYN_ABOVE, THR[a], RATIO[b], RANGE[c]);
        const double bound = gain_bound_db(OMX_DYN_ABOVE, RATIO[b]);
        int ok = 1;
        /* Every positive finite float from the floor up, strided through the bit patterns. */
        for (uint32_t u = 0x3089705fu /* 1e-9f */; u < 0x7f800000u; u += 4099u) {
          float level;
          memcpy(&level, &u, sizeof level);
          const double got = omx_deess_gain_db(&p, level);
          const double want = ref_band_dyn_offset_db(&p.dyn.gc, level);
          const double e = fabs(got - want);
          if (e / bound > g_worst_gain) g_worst_gain = e / bound;
          ok &= e <= bound;
        }
        check(ok, "gain", "the de-esser's gain word within the bound over every level a float holds", 48000.0);
      }
  /* The band's other mode and sign, BELOW, at the steepest ratio this oracle exercises below. */
  for (int sgn = -1; sgn <= 1; sgn += 2) {
    struct omx_dyn d = dyn_atom(48000.0, OMX_DYN_BELOW, -30.0f, 4.0f, 12.0f * (float)sgn);
    const double bound = gain_bound_db(OMX_DYN_BELOW, 4.0);
    int ok = 1;
    for (uint32_t u = 0x3089705fu; u < 0x7f800000u; u += 4099u) {
      float level;
      memcpy(&level, &u, sizeof level);
      const double e = fabs((double)omx_band_dyn_offset_db(&d.gc, level) - (double)ref_band_dyn_offset_db(&d.gc, level));
      if (e / bound > g_worst_gain) g_worst_gain = e / bound;
      ok &= e <= bound;
    }
    check(ok, "gain", "the band's below-mode offset within the bound over every level a float holds", 48000.0);
  }
}

/* ---- the signal: a 1 kHz vowel and a 7 kHz sibilant swept -70 .. +6 dBFS, and noise ------ */

static void signal(float *l, float *r, uint32_t n, double sr) {
  uint32_t seed = 0x2545f491u;
  for (uint32_t i = 0; i < n; i++) {
    const double t = (double)i / sr;
    const double sweep = pow(10.0, (-70.0 + 76.0 * (double)i / (double)n) / 20.0);
    seed = seed * 1664525u + 1013904223u;
    const double nz = (double)(int32_t)seed / 2147483648.0;
    const double v = 0.3 * sin(2.0 * M_PI * 1000.0 * t);
    const double s = sin(2.0 * M_PI * 7000.0 * t + 0.3);
    l[i] = (float)(sweep * (0.5 * v + s) + 1e-3 * nz);
    r[i] = (float)(sweep * (0.25 * v - 0.7 * s) - 1e-3 * nz);
  }
}

/* The configurations: the de-esser in both modes and the dynamic band's below/boost forms. */
struct cfg {
  int mode, wideband;
  float thr, ratio, range;
};
static const struct cfg CFGS[] = {
    {OMX_DYN_ABOVE, 0, -30.0f, 4.0f, -12.0f}, {OMX_DYN_ABOVE, 1, -30.0f, 4.0f, -12.0f},
    {OMX_DYN_ABOVE, 0, -60.0f, 20.0f, -24.0f}, {OMX_DYN_ABOVE, 1, -10.0f, 1.5f, -24.0f},
    {OMX_DYN_ABOVE, 0, -20.0f, 2.0f, 6.0f},    {OMX_DYN_BELOW, 0, -30.0f, 4.0f, -12.0f},
    {OMX_DYN_BELOW, 0, -40.0f, 2.0f, 9.0f},
};

static float g_l[MAXN], g_r[MAXN], g_lr[MAXN], g_rr[MAXN], g_x[MAXN], g_xr[MAXN];
static float g_lv[MAXN], g_g[MAXN], g_bl[MAXN], g_br[MAXN], g_l2[MAXN], g_r2[MAXN];

static void run_lib(float *l, float *r, uint32_t n, uint32_t block, const struct omx_dyn *d, const float c[5],
                    struct omx_band_dyn_state *st, int wideband) {
  for (uint32_t o = 0; o < n; o += block) {
    const uint32_t m = n - o < block ? n - o : block;
    omx_band_dyn_run(l + o, r ? r + o : NULL, m, d, c, st, wideband, 1);
  }
}

static void arm_kernel(double sr) {
  const uint32_t n = (uint32_t)(0.4 * sr);
  float c[5];
  omx_eq_design_f(OMX_EQ_BANDPASS, (double)OMX_DEESS_FREQ_RANGE_DEFAULT, 1.41, 0.0, sr, c);
  for (unsigned k = 0; k < sizeof CFGS / sizeof CFGS[0]; k++) {
    const struct cfg *cf = &CFGS[k];
    const struct omx_dyn d = dyn_atom(sr, cf->mode, cf->thr, cf->ratio, cf->range);
    const double eg = pow(10.0, gain_bound_db(cf->mode, cf->ratio) / 20.0) - 1.0;
    /* The level moved by omx_lin_to_db_poly()'s bound, up and down. */
    const float hi = (float)pow(10.0, 2.0 * LIN_TO_DB_BOUND / 20.0), lo = 1.0f / hi;
    for (int stereo = 0; stereo <= 1; stereo++) {
      signal(g_x, g_xr, n, sr);
      memcpy(g_l, g_x, n * sizeof(float));
      memcpy(g_r, g_xr, n * sizeof(float));
      memcpy(g_lr, g_x, n * sizeof(float));
      memcpy(g_rr, g_xr, n * sizeof(float));
      struct omx_band_dyn_state st, sr_st;
      omx_band_dyn_state_init(&st);
      omx_band_dyn_state_init(&sr_st);
      int out_ok = 1, unity_ok = 1, state_ok = 1, gr_ok = 1, unity_seen = 0;
      for (uint32_t o = 0; o < n; o += 61u) {
        const uint32_t m = n - o < 61u ? n - o : 61u;
        omx_band_dyn_run(g_l + o, stereo ? g_r + o : NULL, m, &d, c, &st, cf->wideband, 1);
        ref_band_dyn_run(g_lr + o, stereo ? g_rr + o : NULL, m, &d, c, &sr_st, cf->wideband, 1, g_lv + o, g_g + o, g_bl + o,
                         g_br + o);
        state_ok &= memcmp(&st.env, &sr_st.env, sizeof st.env) == 0 && memcmp(st.b_l, sr_st.b_l, sizeof st.b_l) == 0 &&
                    memcmp(st.b_r, sr_st.b_r, sizeof st.b_r) == 0;
        gr_ok &= fabs((double)st.gr_min_db - (double)sr_st.gr_min_db) <= gain_bound_db(cf->mode, cf->ratio) + 1e-5;
      }
      for (uint32_t i = 0; i < n; i++) {
        for (int leg = 0; leg <= stereo; leg++) {
          const double x = leg ? g_xr[i] : g_x[i], y = leg ? g_r[i] : g_l[i], yr = leg ? g_rr[i] : g_lr[i];
          const double b = leg ? g_br[i] : g_bl[i];
          if (ref_band_dyn_offset_db(&d.gc, g_lv[i]) == 0.0f && ref_band_dyn_offset_db(&d.gc, g_lv[i] * hi) == 0.0f &&
              ref_band_dyn_offset_db(&d.gc, g_lv[i] * lo) == 0.0f) {
            unity_seen = 1;
            unity_ok &= y == yr;
          }
          const double gr = g_g[i];
          const double tol = cf->wideband ? eg * gr * fabs(x) + 2.0 * FLT_EPSILON * fabs(yr)
                                          : eg * gr * fabs(b) + 2.0 * FLT_EPSILON * (fabs(x) + fabs(b));
          const double e = fabs(y - yr);
          if (tol > 0.0 && e / tol > g_worst_out) g_worst_out = e / tol;
          out_ok &= e <= tol;
        }
      }
      check(out_ok, "output", "every sample within the bound of the libm loop", sr);
      check(unity_ok && unity_seen, "unity", "where the libm gain is exactly 1, the output is its, bit for bit", sr);
      check(state_ok, "state", "band histories and detector cascade byte-identical to the libm loop", sr);
      check(gr_ok, "state", "the GR reading within the bound of the libm loop's", sr);

      /* chunk: block sizes 1, 64 and 1024 give the 61-sample run's output bit for bit. */
      static const uint32_t BLK[] = {1u, 64u, 1024u};
      for (unsigned bi = 0; bi < sizeof BLK / sizeof BLK[0]; bi++) {
        memcpy(g_l2, g_x, n * sizeof(float));
        memcpy(g_r2, g_xr, n * sizeof(float));
        struct omx_band_dyn_state s2;
        omx_band_dyn_state_init(&s2);
        run_lib(g_l2, stereo ? g_r2 : NULL, n, BLK[bi], &d, c, &s2, cf->wideband);
        check(memcmp(g_l2, g_l, n * sizeof(float)) == 0 && (!stereo || memcmp(g_r2, g_r, n * sizeof(float)) == 0),
              "chunk", "the output does not depend on the block size, bit for bit", sr);
      }
      /* chunk: each sample takes omx_band_dyn_detect()'s gain (the per-sample word, the GR meter's). */
      struct omx_band_dyn_state s3;
      omx_band_dyn_state_init(&s3);
      const struct omx_band_dyn_block bk = omx_band_dyn_begin(&d);
      int det_ok = 1;
      for (uint32_t i = 0; i < n; i++) {
        float bl, br;
        const float g = omx_band_dyn_detect(c, &s3, &d.gc, &bk, g_x[i], stereo ? g_xr[i] : 0.0f, stereo, &bl, &br);
        const float want = cf->wideband ? g_x[i] * g : omx_band_dyn_split(g_x[i], g, bl);
        det_ok &= want == g_l2[i];
        if ((i + 1u) % 1024u == 0u) omx_band_dyn_flush(&s3);
      }
      check(det_ok, "chunk", "each sample takes the gain omx_band_dyn_detect() gives it, bit for bit", sr);
    }
  }
}

int main(void) {
  omx_fx_require_rate_floor();
  printf("fx/band_dyn_poly: the libm-free band-dynamics loop against the libm loop, %d oracle rates\n",
         (int)OMX_FX_ORACLE_RATE_COUNT);
  arm_gain();
  for (int k = 0; k < (int)OMX_FX_ORACLE_RATE_COUNT; k++) arm_kernel(OMX_FX_ORACLE_RATES[k]);
  printf("  worst gain error %.3g of the bound; worst sample error %.3g of the bound\n", g_worst_gain, g_worst_out);
  printf("%s: %d checks, %d failures\n", g_fail ? "FAIL" : "PASS", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
