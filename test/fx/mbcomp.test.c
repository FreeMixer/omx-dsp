// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file mbcomp.test.c
 * @brief The multiband compressor's oracle at the nine RME rates (OMX_FX_RME_RATES, fx_rates.h),
 *        every band count OMX_MBC_MIN_BANDS … OMX_MBC_MAX_BANDS at its come-up corners:
 *   O1 the sum at rest is flat (±0.01 dB);
 *   O2 the rest response is the product of the crossovers' allpasses (1e-5 per sample, float vs double);
 *      bands that are off pass at unity whatever their atoms hold;
 *   O3 off: output and state memcmp untouched;
 *   O4 held gains: |Y/X| = Σ g_k|B_k| (±0.02 dB) and never above the input (+0.01 dB), with the
 *      subtractive split's counter-example red;
 *   O5 each band's GR is the gain computer at its RMS level (±0.05 dB), and bands 2+ octaves from
 *      the tone read 0 dB;
 *   O7 every split is −6.0206 dB at its corner (±0.001 dB);
 *   O8 one tap for all bands: 4x puts the first output at OMX_OVS_LATENCY_4X and the sum stays flat;
 *   O9 no denormal state word after a burst and 2 s of silence;
 *   RT design, state init and process allocate nothing (malloc, calloc, realloc and free wrapped at
 *      link time and counted), at OMX_MBC_MAX_BANDS bands, 4x, a block longer than one chunk.
 * Contracts are compiled in: every rate's ledger is drained, a declared rate's violation counts.
 * test/fx/mbcomp-perturb.sh proves the arms see the defects they are for.
 */
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/omx_contract.h>

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/fx/omx_mbcomp.h>
#include <omxdsp/omx_denormal.h>
#include <omxdsp/omx_onepole.h>

#include "fx_rates.h"

static int g_checks, g_failed;

/* The allocation counter: the oracle links with --wrap=malloc,calloc,realloc,free (the Makefile's
 * FX_LDFLAGS_mbcomp), and counts only while an RT arm runs. */
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);
static int g_counting;
static long g_allocs;
void *__wrap_malloc(size_t n) {
  g_allocs += g_counting;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t sz) {
  g_allocs += g_counting;
  return __real_calloc(n, sz);
}
void *__wrap_realloc(void *p, size_t n) {
  g_allocs += g_counting;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  g_allocs += g_counting;
  __real_free(p);
}
static uint32_t g_unexplained;

/* Drain the ledger at `sr`. Outside the declared six every design and every pole records
 * `rate-is-declared`, and the ledger keeps 64 records, so it is drained often enough that a real
 * violation is never pushed past it unread. */
static void drain(float sr) { g_unexplained += omx_fx_drain_ledger(sr); }

static void ok(int cond, const char *arm, const char *what, double got, double want, float sr, uint32_t bands) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s @ %.0f Hz, %u bands: got %.6g, want %.6g\n", arm, what, (double)sr, bands, got, want);
  }
}

static const float *corners(uint32_t bands) { return omx_mbcomp_come_up_hz(bands); }

static struct omx_dyn comp_atom(float sr, float attack_ms, float release_ms) {
  struct omx_dyn d;
  memset(&d, 0, sizeof d);
  d.enabled = 1;
  d.gc.mode = OMX_DYN_ABOVE;
  d.gc.thresh_db = OMX_COMP_THRESHOLD_DB_DEFAULT;
  d.gc.ratio = OMX_COMP_RATIO_DEFAULT;
  d.gc.knee_db = OMX_COMP_KNEE_DB_DEFAULT;
  d.gc.makeup_lin = omx_db_to_lin(OMX_COMP_MAKEUP_DB_DEFAULT);
  d.detect = OMX_DETECT_RMS;
  d.attack_ms = attack_ms;
  d.attack_coeff = omx_pole_from_time_ms(attack_ms, sr);
  d.release_coeff = omx_pole_from_time_ms(release_ms, sr);
  d.ovs_mode = OMX_DYN_OVS_OFF;
  return d;
}

static void stage(struct omx_mbcomp *m, uint32_t bands, float sr) {
  memset(m, 0, sizeof *m);
  m->enabled = 1;
  m->ovs_mode = OMX_DYN_OVS_OFF;
  omx_mbcomp_design(m, bands, corners(bands), sr);
  for (uint32_t k = 0; k < bands; k++) m->band[k] = comp_atom(sr, 5.0f, 200.0f);
}

static double complex section(const double c[5], double w) {
  const double complex z1 = cexp(-I * w), z2 = z1 * z1;
  return (c[0] + c[1] * z1 + c[2] * z2) / (1.0 + c[3] * z1 + c[4] * z2);
}

/* |B_k(f)|: the band's path magnitude from its crossovers' designs (allpasses are unit). */
static double band_mag(const struct omx_mbcomp *m, uint32_t k, double w) {
  double g = 1.0;
  for (uint32_t j = 0; j + 1u < m->bands; j++) {
    if (j < k) g *= pow(cabs(section(m->xo[j].hp, w)), 2.0);
    else if (j == k) g *= pow(cabs(section(m->xo[j].lp, w)), 2.0);
  }
  return g;
}

/* The DFT of `h` at `w`, in double. */
static double complex dft(const float *h, uint32_t len, double w) {
  double complex acc = 0.0, ph = 1.0;
  const double complex step = cexp(-I * w);
  for (uint32_t i = 0; i < len; i++) {
    acc += (double)h[i] * ph;
    ph *= step;
    if ((i & 4095u) == 4095u) ph /= cabs(ph);
  }
  return acc;
}

/* O1 + O2 + O8: the rest impulse response (every band below threshold), both legs. */
static void arm_rest(float sr, uint32_t bands, int ovs_mode) {
  const char *arm = ovs_mode == OMX_DYN_OVS_X4 ? "O8" : "O1";
  struct omx_mbcomp m;
  stage(&m, bands, sr);
  m.ovs_mode = ovs_mode;
  static struct omx_mbcomp_state st;
  omx_mbcomp_state_init(&st, omx_mbcomp_factor(&m));
  const uint32_t len = (uint32_t)sr;
  float *h = calloc(len, sizeof *h), *hr = calloc(len, sizeof *hr);
  if (!h || !hr) abort();
  const float amp = 1e-3f;
  h[0] = amp;
  hr[0] = amp;
  omx_mbcomp_process(h, hr, len, &m, &st);
  const uint32_t tap = omx_mbcomp_factor(&m) == 4u ? OMX_OVS_LATENCY_4X : 0u;
  uint32_t first = len;
  for (uint32_t i = 0; i < len && first == len; i++) if (h[i] != 0.0f) first = i;
  ok(first == tap, "O8", ovs_mode == OMX_DYN_OVS_X4 ? "4x: the first non-zero output is at the declared tap" : "off: the first non-zero output is at 0",
     first, tap, sr, bands);
  ok(memcmp(h, hr, len * sizeof *h) == 0, arm, "both legs run the same tree", 0, 0, sr, bands);
  double worst = 0.0;
  const double top = fmin(20000.0, OMX_MBC_CORNER_MAX_FRACTION * sr);
  for (int q = 0; q < 40; q++) {
    const double f = 20.0 * pow(top / 20.0, q / 39.0), w = 2.0 * M_PI * f / sr;
    const double db = 20.0 * log10(cabs(dft(h + tap, len - tap, w)) / amp);
    worst = fmax(worst, fabs(db));
  }
  ok(worst <= 0.01, arm, ovs_mode == OMX_DYN_OVS_X4 ? "4x: the sum at rest is still flat (+-0.01 dB)" : "the sum at rest is flat (+-0.01 dB)", worst, 0.01, sr, bands);
  if (ovs_mode == OMX_DYN_OVS_OFF) {
    double s[OMX_MBC_MAX_XOVERS][2] = {{0}}, perr = 0.0;
    for (uint32_t i = 0; i < len; i++) {
      double y = i == 0 ? 1.0 : 0.0;
      for (uint32_t j = 0; j + 1u < bands; j++) y = omx_biquad_tdf2_d(y, m.xo[j].ap, s[j]);
      perr = fmax(perr, fabs((double)h[i] / amp - y));
    }
    ok(perr <= 1e-5, "O2", "the rest response is the product of the crossovers' allpasses (1e-5)", perr, 1e-5, sr, bands);
  }
  free(h);
  free(hr);
}

/* A band that is off is the unity compressor: every band off, each atom set to compress hard
 * (threshold -80 dB, ratio 20, make-up +12 dB), and the -60 dBFS impulse still sums flat. */
static void arm_off_bands(float sr, uint32_t bands) {
  struct omx_mbcomp m;
  stage(&m, bands, sr);
  for (uint32_t k = 0; k < bands; k++) {
    m.band[k].enabled = 0;
    m.band[k].gc.thresh_db = -80.0f;
    m.band[k].gc.ratio = 20.0f;
    m.band[k].gc.makeup_lin = omx_db_to_lin(12.0f);
  }
  static struct omx_mbcomp_state st;
  omx_mbcomp_state_init(&st, omx_mbcomp_factor(&m));
  const uint32_t len = (uint32_t)sr;
  float *h = calloc(len, sizeof *h);
  if (!h) abort();
  h[0] = 1e-3f;
  omx_mbcomp_process(h, NULL, len, &m, &st);
  double worst = 0.0;
  const double top = fmin(20000.0, OMX_MBC_CORNER_MAX_FRACTION * sr);
  for (int q = 0; q < 40; q++) {
    const double f = 20.0 * pow(top / 20.0, q / 39.0), w = 2.0 * M_PI * f / sr;
    worst = fmax(worst, fabs(20.0 * log10(cabs(dft(h, len, w)) / 1e-3)));
  }
  ok(worst <= 0.01, "O1", "bands that are off pass at unity whatever their atoms say (+-0.01 dB)", worst, 0.01, sr, bands);
  for (uint32_t k = 0; k < bands; k++)
    ok(omx_mbcomp_band_gr_db(&m, &st, k) == 0.0f, "O1", "a band that is off reads 0 dB GR", omx_mbcomp_band_gr_db(&m, &st, k), 0.0, sr, bands);
  free(h);
}

/* O3: off leaves every sample and every state word untouched. */
static void arm_bypass(float sr, uint32_t bands) {
  struct omx_mbcomp m;
  stage(&m, bands, sr);
  static struct omx_mbcomp_state st, before;
  omx_mbcomp_state_init(&st, omx_mbcomp_factor(&m));
  float x[512], y[512], xr[512], yr[512];
  for (int i = 0; i < 512; i++) x[i] = xr[i] = 0.5f * sinf(0.01f * (float)i * (float)(1 + i % 7));
  omx_mbcomp_process(x, xr, 512, &m, &st);
  m.enabled = 0;
  memcpy(&before, &st, sizeof st);
  memcpy(y, x, sizeof x);
  memcpy(yr, xr, sizeof xr);
  omx_mbcomp_process(y, yr, 512, &m, &st);
  ok(memcmp(y, x, sizeof x) == 0 && memcmp(yr, xr, sizeof xr) == 0, "O3", "off: output memcmp input", 0, 0, sr, bands);
  ok(memcmp(&st, &before, sizeof st) == 0, "O3", "off: state untouched", 0, 0, sr, bands);
}

/* O4: held gains (ratio 1, the make-up the held gain): |Y/X| = Σ g_k|B_k| and never above max g. */
static void arm_convex(float sr, uint32_t bands) {
  struct omx_mbcomp m;
  stage(&m, bands, sr);
  float g[OMX_MBC_MAX_BANDS];
  for (uint32_t k = 0; k < bands; k++) {
    g[k] = k == 1u ? omx_db_to_lin(-12.0f) : 1.0f;
    m.band[k].gc.ratio = 1.0f;
    m.band[k].gc.knee_db = 0.0f;
    m.band[k].gc.makeup_lin = g[k];
  }
  static struct omx_mbcomp_state st;
  omx_mbcomp_state_init(&st, omx_mbcomp_factor(&m));
  const uint32_t len = (uint32_t)sr;
  float *h = calloc(len, sizeof *h);
  if (!h) abort();
  h[0] = 1e-3f;
  omx_mbcomp_process(h, NULL, len, &m, &st);
  double over = -1e9, dev = 0.0;
  const double top = fmin(20000.0, OMX_MBC_CORNER_MAX_FRACTION * sr);
  for (int q = 0; q < 40; q++) {
    const double f = 20.0 * pow(top / 20.0, q / 39.0), w = 2.0 * M_PI * f / sr;
    const double db = 20.0 * log10(cabs(dft(h, len, w)) / 1e-3);
    double want = 0.0;
    for (uint32_t k = 0; k < bands; k++) want += g[k] * band_mag(&m, k, w);
    over = fmax(over, db);
    dev = fmax(dev, fabs(db - 20.0 * log10(want)));
  }
  ok(over <= 0.01, "O4", "with band 2 held at -12 dB the stage never exceeds its input (+0.01 dB)", over, 0.01, sr, bands);
  ok(dev <= 0.02, "O4", "the response is the convex combination sum g_k|B_k| (+-0.02 dB)", dev, 0.02, sr, bands);
  if (bands == 2u) {
    double sub = -1e9;
    for (int q = 0; q < 400; q++) {
      const double f = 20.0 * pow(top / 20.0, q / 399.0), w = 2.0 * M_PI * f / sr;
      const double complex lp = section(m.xo[0].lp, w);
      sub = fmax(sub, 20.0 * log10(cabs(1.0 - (1.0 - 0.25) * lp * lp)));
    }
    ok(sub > 0.5, "O4", "counter-example: the subtractive split x - (1-g)LP(x) at g=-12 dB exceeds its input", sub, 0.5, sr, bands);
    if (sr == 48000.0f) printf("fx/mbcomp: O4 counter-example: the subtractive split peaks at %+.2f dB (48 kHz, 1 kHz corner, band 1 at -12 dB)\n", sub);
  }
  free(h);
}

/* O5: a tone at each band's centre, levels -60..0 dBFS: the band's GR is the gain computer at
 * A·|B_k|/√2 (attack = release, so the RMS cascade is linear and unbiased); isolation beyond 2 oct. */
static void arm_static(float sr, uint32_t bands) {
  const float *fc = corners(bands);
  const float levels[] = {-60.0f, -30.0f, -21.0f, -18.0f, -15.0f, -9.0f, 0.0f};
  for (uint32_t k = 0; k < bands; k++) {
    const double lo = k == 0 ? fc[0] / 4.0 : fc[k - 1], hi = k + 1u == bands ? fmin(4.0 * fc[k - 1], 0.4 * sr) : fc[k];
    const double f = sqrt(lo * hi), w = 2.0 * M_PI * f / sr;
    double worst = 0.0, leak = 0.0;
    for (size_t li = 0; li < sizeof levels / sizeof *levels; li++) {
      struct omx_mbcomp m;
      stage(&m, bands, sr);
      for (uint32_t j = 0; j < bands; j++) m.band[j] = comp_atom(sr, 50.0f, 50.0f);
      static struct omx_mbcomp_state st;
      omx_mbcomp_state_init(&st, omx_mbcomp_factor(&m));
      const float a = omx_db_to_lin(levels[li]);
      float buf[4096];
      uint64_t t = 0;
      const uint64_t len = (uint64_t)(0.6f * sr);
      while (t < len) {
        for (int i = 0; i < 4096; i++, t++) buf[i] = a * (float)sin(w * (double)t);
        omx_mbcomp_process(buf, NULL, 4096, &m, &st);
      }
      const float gr = omx_mbcomp_band_gr_db(&m, &st, k);
      const float want = omx_gaincomp_db(&m.band[k].gc, omx_lin_to_db((float)(a * band_mag(&m, k, w) / sqrt(2.0))));
      worst = fmax(worst, fabs((double)gr - want));
      drain(sr);
      for (uint32_t j = 0; j < bands; j++) {
        const double jlo = j == 0 ? 0.0 : fc[j - 1], jhi = j + 1u == bands ? 1e9 : fc[j];
        if (f < jlo / 4.0 || f > jhi * 4.0) leak = fmax(leak, fabs(omx_mbcomp_band_gr_db(&m, &st, j)));
      }
    }
    ok(worst <= 0.05, "O5", "the band's GR is the gain computer at its RMS level (+-0.05 dB)", worst, 0.05, sr, bands);
    ok(leak == 0.0, "O5", "isolation: bands 2+ octaves from the tone read 0 dB GR", leak, 0.0, sr, bands);
  }
}

/* O7: each split is -6.0206 dB on both sides at its corner. */
static void arm_corners(float sr, uint32_t bands) {
  struct omx_mbcomp m;
  stage(&m, bands, sr);
  double worst = 0.0;
  for (uint32_t j = 0; j + 1u < bands; j++) {
    const double w = 2.0 * M_PI * corners(bands)[j] / sr;
    worst = fmax(worst, fabs(40.0 * log10(cabs(section(m.xo[j].lp, w))) + 6.0206));
    worst = fmax(worst, fabs(40.0 * log10(cabs(section(m.xo[j].hp, w))) + 6.0206));
  }
  ok(worst <= 0.001, "O7", "every split is -6.0206 dB at its corner (+-0.001 dB)", worst, 0.001, sr, bands);
}

/* The corner line: a corner above OMX_MBC_CORNER_MAX_FRACTION·sr is designed AT it. */
static void arm_corner_line(float sr) {
  struct omx_mbcomp m;
  memset(&m, 0, sizeof m);
  const float hz[1] = {20000.0f};
  omx_mbcomp_design(&m, 2u, hz, sr);
  const double line = OMX_MBC_CORNER_MAX_FRACTION * sr, want = hz[0] < line ? hz[0] : line;
  const double w = 2.0 * M_PI * want / sr;
  const double got = 40.0 * log10(cabs(section(m.xo[0].lp, w)));
  ok(fabs(got + 6.0206) <= 0.001, "O7", "a 20 kHz corner is designed at min(20 kHz, the corner line)", got, -6.0206, sr, 2u);
}

/* O9: a 0 dBFS burst then 2 s of silence under the thread's FTZ: every word 0 or normal. */
static void arm_denormal(float sr, uint32_t bands) {
  struct omx_mbcomp m;
  stage(&m, bands, sr);
  static struct omx_mbcomp_state st;
  omx_mbcomp_state_init(&st, omx_mbcomp_factor(&m));
  float l[1024], r[1024];
  uint32_t seed = 12345u;
  const uint32_t burst = (uint32_t)(0.1f * sr), quiet = (uint32_t)(2.0f * sr);
  for (uint32_t t = 0; t < burst + quiet; t += 1024u) {
    for (int i = 0; i < 1024; i++) {
      seed = seed * 1664525u + 1013904223u;
      l[i] = t < burst ? (float)((int32_t)seed) / 2147483648.0f : 0.0f;
      r[i] = -l[i];
    }
    omx_mbcomp_process(l, r, 1024, &m, &st);
  }
  uint32_t bad = 0;
  const double *d = &st.leg[0].xs[0].lp[0][0];
  for (size_t i = 0; i < sizeof st.leg / (sizeof(double)); i++) bad += d[i] != 0.0 && !isnormal(d[i]);
  for (uint32_t k = 0; k < bands; k++)
    for (int s = 0; s < OMX_DYN_ENV_STAGES; s++) bad += st.dyn[k].env.stage[s] != 0.0f && !isnormal(st.dyn[k].env.stage[s]);
  ok(bad == 0, "O9", "no denormal state word after a burst and 2 s of silence", bad, 0, sr, bands);
}

/* RT: design, state init and a process call over more than one chunk, at the most bands and 4x,
 * allocate nothing. */
static void arm_no_alloc(float sr) {
  const uint32_t bands = OMX_MBC_MAX_BANDS;
  static struct omx_mbcomp m;
  static struct omx_mbcomp_state st;
  static float l[3u * OMX_DYN_OVS_CHUNK + 7u], r[3u * OMX_DYN_OVS_CHUNK + 7u];
  const uint32_t n = sizeof l / sizeof *l;
  for (uint32_t i = 0; i < n; i++) l[i] = r[i] = (i % 97u) < 48u ? 0.5f : -0.5f;
  g_allocs = 0;
  g_counting = 1;
  stage(&m, bands, sr);
  m.ovs_mode = OMX_DYN_OVS_X4;
  omx_mbcomp_state_init(&st, omx_mbcomp_factor(&m));
  omx_mbcomp_process(l, r, n, &m, &st);
  g_counting = 0;
  ok(g_allocs == 0, "RT", "design, state init and process allocate nothing", (double)g_allocs, 0, sr, bands);
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_denormals_off();
  for (uint32_t ri = 0; ri < OMX_FX_RME_RATE_COUNT; ri++) {
    const float sr = OMX_FX_RME_RATES[ri];
    for (uint32_t b = OMX_MBC_MIN_BANDS; b <= OMX_MBC_MAX_BANDS; b++) {
      arm_rest(sr, b, OMX_DYN_OVS_OFF);
      drain(sr);
      arm_rest(sr, b, OMX_DYN_OVS_X4);
      drain(sr);
      arm_off_bands(sr, b);
      drain(sr);
      arm_bypass(sr, b);
      drain(sr);
      arm_convex(sr, b);
      drain(sr);
      arm_static(sr, b);
      drain(sr);
      arm_corners(sr, b);
      drain(sr);
      arm_denormal(sr, b);
      drain(sr);
    }
    arm_corner_line(sr);
    drain(sr);
    arm_no_alloc(sr);
    drain(sr);
  }
  const uint32_t evaluated = omx_contract_log.checks;
  ok(g_unexplained == 0u, "contracts", "no unexplained contract violation", g_unexplained, 0, 0.0f, 0u);
  ok(evaluated >= 1000u, "contracts", "the run evaluated the stage's contracts", evaluated, 1000, 0.0f, 0u);
  printf("fx/mbcomp: %d checks, %d failed, %u contracts evaluated (%u RME rates, bands %u..%u)\n", g_checks, g_failed,
         evaluated, (unsigned)OMX_FX_RME_RATE_COUNT, (unsigned)OMX_MBC_MIN_BANDS, (unsigned)OMX_MBC_MAX_BANDS);
  return g_failed == 0 ? 0 : 1;
}
