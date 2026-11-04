// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The glue face (fx/omx_glue_instance.h) at each of the nine oracle rates:
//   A  a bypass and a mix of 0 are the input, bit for bit; a rate outside the set is refused;
//   B  each detent is the comp slot with the detent's value, written out here, bit for bit, on noise,
//      the auto release among them, in each topology: the face mints no DSP and its tables are what they say;
//      it comes up feed-forward and a topology outside the set reads as feed-forward;
//   C  the settled gain on a held level is the closed form with GLUE_KNEE_DB of knee plus the make-up
//      (1e-3 dB), at each ratio; the readout is that reduction;
//   D  the detector's high-pass: with a corner at 200 Hz, a 40 Hz sine is pulled 2nd-order less, the level
//      the detector hears being -28 dB of the sine's within 1 dB, while a 4 kHz sine pulls as it did (0.1 dB);
//   E  hostile words land inside the travels, the ledger is clean, nothing allocates;
//   F  detectorTopology: the settled gain at each ratio, feed-forward its closed form and feedback the solution of
//      y = x + g_fb(y) (1e-3 dB), one curve above the knee (ssl-bus-compressor §4 L2);
//   G  a small step settles on ONE pole p - (1 - p)(R - 1) in feedback and through the four-pole cascade
//      feed-forward (area, 5 %), the loop dividing the time by the ratio (L3); effectiveAttackMs is the
//      floor of L4 where the detent is faster (1e-4 ms);
//   H  every ratio x attack x release in feedback: the gain moves one way per step, never rings (L4);
//   I  in feedback the key corner is inert (the loop hears the full band), feed-forward it is live.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_glue_instance.h>

#include "instance_oracle.h"
#include "instance_no_alloc.h"

#define KNEE 3.0
static const float RATIOS[3] = {2, 4, 10};
static const float ATTACKS[6] = {0.1f, 0.3f, 1, 3, 10, 30};
static const float RELEASES[4] = {100, 300, 600, 1200};
#define MAXN 400000u
static float g_l[MAXN], g_r[MAXN], g_ol[MAXN], g_or[MAXN], g_xl[MAXN], g_xr[MAXN];
static OmxGlueInstance g_g;
static OmxCompInstance g_c;

/* A block cut small and the ledger drained between: at 32, 64 and 128 kHz the slot's poles write records. */
static void grun(OmxGlueInstance *g, const float *l, const float *r, float *ol, float *orr, uint32_t n) {
  drain();
  for (uint32_t o = 0; o < n; o += 16u) {
    omx_glue_instance_run(g, l + o, r + o, ol + o, orr + o, n - o < 16u ? n - o : 16u);
    drain();
  }
}
static void crun(OmxCompInstance *c, const float *l, const float *r, float *ol, float *orr, uint32_t n) {
  drain();
  for (uint32_t o = 0; o < n; o += 16u) {
    omx_comp_instance_run_keyed(c, NULL, NULL, l + o, r + o, ol + o, orr + o, n - o < 16u ? n - o : 16u);
    drain();
  }
}

static double law(double t, double r, double l) {
  const double x = l - t, s = 1.0 / r - 1.0;
  if (fabs(x) < 0.5 * KNEE) return s * (x + 0.5 * KNEE) * (x + 0.5 * KNEE) / (2.0 * KNEE);
  return x <= 0.0 ? 0.0 : s * x;
}

static void arm_identity(void) {
  g_arm = "A";
  OmxGlueInstance x;
  ok(omx_glue_instance_init(&x, 11025.0f) == 0, "a rate outside the oracle set is refused", 1, 0);
  const uint32_t n = 4096u;
  stimulus(g_l, g_r, n, 0x6123u, 512u);
  omx_glue_instance_init(&g_g, g_sr);
  omx_glue_instance_resolve(&g_g, 1, -30.0f, 2, 0, 0, 6.0f, 100.0f, 100.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
  grun(&g_g, g_l, g_r, g_ol, g_or, n);
  drain();
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "a bypass is the input, bit for bit", 1, 0);
  omx_glue_instance_resolve(&g_g, 0, -30.0f, 2, 0, 0, 0.0f, 0.0f, 0.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
  grun(&g_g, g_l, g_r, g_ol, g_or, n);
  drain();
  ok(memcmp(g_ol, g_l, n * sizeof(float)) == 0 && memcmp(g_or, g_r, n * sizeof(float)) == 0, "a mix of 0 is the dry bus, bit for bit", 1, 0);
  clean();
}

static void comp_as(float t, float r, float a, float rel, int mode, float mk, float hp, float mix, int tp) {
  omx_comp_instance_init(&g_c, g_sr);
  omx_comp_instance_resolve(&g_c, 0, t, r, 3.0f, a, rel, mk, mix, (int)OMX_COMP_KINDS_COMP, (int)OMX_DETECTOR_OVERSAMPLINGS_OFF,
                            tp, (int)OMX_COMP_KEY_SOURCES_SELF, hp, 0.0f, 0.0f,
                            (int)OMX_COMP_KEY_LISTENS_OFF, 0.0f, (int)OMX_COMP_DETECT_LINKS_LINKED, (int)OMX_COMP_DUCKS_OFF,
                            -12.0f, mode);
}

static void arm_detents(void) {
  g_arm = "B";
  const uint32_t n = 8192u;
  stimulus(g_l, g_r, n, 0x6124u, 1024u);
  for (int tp = 0; tp < 2; tp++)
    for (int ra = 0; ra < 3; ra++)
      for (int at = 0; at < 6; at++)
        for (int rl = 0; rl < 5; rl++) {
          const int aut = rl == 4;
          omx_glue_instance_init(&g_g, g_sr);
          omx_glue_instance_resolve(&g_g, 0, -24.0f, ra, at, rl, 2.0f, 120.0f, 80.0f, tp);
          grun(&g_g, g_l, g_r, g_ol, g_or, n);
          /* feedback: the slot runs the loop on the full band, its corner never handed over */
          comp_as(-24.0f, RATIOS[ra], ATTACKS[at], aut ? RELEASES[0] : RELEASES[rl],
                  aut ? (int)OMX_COMP_RELEASE_MODES_AUTO : (int)OMX_COMP_RELEASE_MODES_FIXED, 2.0f, tp ? 0.0f : 120.0f, 80.0f,
                  tp ? (int)OMX_DETECTOR_TOPOLOGY_FEEDBACK : (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
          crun(&g_c, g_l, g_r, g_xl, g_xr, n);
          drain();
          ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0 && memcmp(g_or, g_xr, n * sizeof(float)) == 0,
             "the detent is the comp slot with its written value and topology, bit for bit", tp * 1000 + ra * 100 + at * 10 + rl, 0);
        }
  /* the default is feed-forward: an instance never told its topology, and a word outside the set, are the
   * feed-forward glue bit for bit (an older session resolves to the glue it saved) */
  omx_glue_instance_init(&g_g, g_sr);
  grun(&g_g, g_l, g_r, g_ol, g_or, n);
  comp_as((float)OMX_GLUE_THRESHOLD_DB_RANGE_DEFAULT, RATIOS[OMX_GLUE_RATIOS_DEFAULT], ATTACKS[OMX_GLUE_ATTACKS_DEFAULT], RELEASES[0],
          (int)OMX_COMP_RELEASE_MODES_AUTO, 0.0f, 0.0f, 100.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
  crun(&g_c, g_l, g_r, g_xl, g_xr, n);
  drain();
  ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0, "the instance comes up feed-forward", 0, 0);
  omx_glue_instance_init(&g_g, g_sr);
  omx_glue_instance_resolve(&g_g, 0, -24.0f, 1, 2, 4, 2.0f, 120.0f, 80.0f, 7);
  grun(&g_g, g_l, g_r, g_ol, g_or, n);
  comp_as(-24.0f, 4.0f, 1.0f, RELEASES[0], (int)OMX_COMP_RELEASE_MODES_AUTO, 2.0f, 120.0f, 80.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
  crun(&g_c, g_l, g_r, g_xl, g_xr, n);
  drain();
  ok(memcmp(g_ol, g_xl, n * sizeof(float)) == 0, "a topology outside the set reads as feed-forward", 0, 0);
  clean();
}

static void arm_law(void) {
  g_arm = "C";
  const uint32_t n = (uint32_t)(0.5f * g_sr);
  static const double LS[] = {-40.0, -21.0, -20.0, -18.0, -6.0};
  for (int ra = 0; ra < 3; ra++)
    for (int k = 0; k < 5; k++) {
      const float x = (float)pow(10.0, LS[k] / 20.0);
      for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = x;
      omx_glue_instance_init(&g_g, g_sr);
      omx_glue_instance_resolve(&g_g, 0, -20.0f, ra, 2, 0, 3.0f, 0.0f, 100.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
      grun(&g_g, g_l, g_r, g_ol, g_or, n);
      drain();
      const double got = 20.0 * log10((double)g_ol[n - 1u] / (double)x), want = law(-20.0, RATIOS[ra], LS[k]) + 3.0;
      ok(fabs(got - want) < 1e-3, "the settled gain is the closed form plus the make-up (1e-3 dB)", got, want);
      /* the readout is the slot's, make-up included: at a make-up of 0 it reads the reduction */
      omx_glue_instance_init(&g_g, g_sr);
      omx_glue_instance_resolve(&g_g, 0, -20.0f, ra, 2, 0, 0.0f, 0.0f, 100.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
      grun(&g_g, g_l, g_r, g_ol, g_or, n);
      grun(&g_g, g_l, g_r, g_ol, g_or, 64u);
      drain();
      ok(fabs((double)omx_glue_instance_gain_reduction_db(&g_g) - (want - 3.0)) < 5e-2, "the readout is the reduction", omx_glue_instance_gain_reduction_db(&g_g), want - 3.0);
    }
  clean();
}

static double pull(double hz, float hp, int tp) {
  const uint32_t n = (uint32_t)(1.0f * g_sr), tail = (uint32_t)(0.5f * g_sr);
  sine(g_l, n, hz, g_sr, 0.5);
  memcpy(g_r, g_l, n * sizeof(float));
  omx_glue_instance_init(&g_g, g_sr);
  omx_glue_instance_resolve(&g_g, 0, -40.0f, 1, 5, 3, 0.0f, hp, 100.0f, tp);
  grun(&g_g, g_l, g_r, g_ol, g_or, n);
  drain();
  return 20.0 * log10(rms(g_ol, n - tail, n) / rms(g_l, n - tail, n));
}

static void arm_key(void) {
  g_arm = "D";
  const double lo_off = pull(40.0, 0.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD), lo_on = pull(40.0, 200.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD),
               hi_off = pull(4000.0, 0.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD), hi_on = pull(4000.0, 200.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
  /* the detector hears the 40 Hz sine 28 dB down, (40/200)^2; on the 4:1 slope the reduction falls by 3/4 of that */
  ok(fabs((lo_on - lo_off) - 0.75 * 28.0) < 1.0, "a corner at 200 Hz keeps a 40 Hz sine off the detector", lo_on - lo_off, 21.0);
  ok(fabs(hi_on - hi_off) < 0.1, "a 4 kHz sine pulls as it did", hi_on - hi_off, 0.1);
  clean();
}

static void arm_hostile(void) {
  g_arm = "E";
  static const float W[] = {NAN, INFINITY, -INFINITY, 1e30f, -1e30f, -1.0f};
  static const int I[] = {-1, 99, 0, 1};
  stimulus(g_l, g_r, 512u, 0x77u, 128u);
  g_counting = 1;
  for (int a = 0; a < 6; a++)
    for (int b = 0; b < 4; b++) {
      omx_glue_instance_init(&g_g, g_sr);
      omx_glue_instance_resolve(&g_g, 0, W[a], I[b], I[b], I[b], W[a], W[a], W[a], I[b]);
      grun(&g_g, g_l, g_r, g_ol, g_or, 512u);
      drain();
      uint32_t finite = 1u;
      for (uint32_t i = 0; i < 512u; i++) finite &= g_ol[i] - g_ol[i] == 0.0f && g_or[i] - g_or[i] == 0.0f;
      ok(finite, "hostile words land inside the travels", finite, 1);
    }
  g_counting = 0;
  ok(g_allocs == 0, "nothing allocates", (double)g_allocs, 0);
  clean();
}

/* The feedback loop's static curve in closed form: the output-domain characteristic g_fb(u), u = y - T, the
 * C1 knee across +-KNEE/2 and the slope -(R - 1) above it (ssl-bus-compressor §4 L2); the settled output solves
 * y = x + g_fb(y - T), unique because y - g_fb(y - T) strictly increases. Bisection to 1e-12 dB. */
static double gfb(double u, double r) {
  if (u <= -0.5 * KNEE) return 0.0;
  if (u >= 0.5 * KNEE) return -(r - 1.0) * u;
  return -(r - 1.0) * (u + 0.5 * KNEE) * (u + 0.5 * KNEE) / (2.0 * KNEE);
}
static double law_fb(double t, double r, double l) {
  double lo = l - 200.0, hi = l;
  for (int k = 0; k < 200 && hi - lo > 1e-12; k++) {
    const double y = 0.5 * (lo + hi);
    if (y - (l + gfb(y - t, r)) > 0.0) hi = y; else lo = y;
  }
  return 0.5 * (lo + hi) - l;
}

/* A held level x through the glue at `tp` from cold; the settled gain, dB. */
static double settled(int ra, int tp, double level_db) {
  const uint32_t n = (uint32_t)(0.5f * g_sr);
  const float x = (float)pow(10.0, level_db / 20.0);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = x;
  omx_glue_instance_init(&g_g, g_sr);
  omx_glue_instance_resolve(&g_g, 0, -20.0f, ra, 2, 0, 0.0f, 0.0f, 100.0f, tp);
  grun(&g_g, g_l, g_r, g_ol, g_or, n);
  drain();
  return 20.0 * log10((double)g_ol[n - 1u] / (double)x);
}

static void arm_static(void) {
  g_arm = "F";
  static const double LS[] = {-40.0, -22.0, -21.0, -20.0, -19.0, -18.0, -12.0, -6.0, 0.0};
  for (int ra = 0; ra < 3; ra++)
    for (int k = 0; k < 9; k++) {
      const double ff = settled(ra, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD, LS[k]);
      const double fb = settled(ra, (int)OMX_DETECTOR_TOPOLOGY_FEEDBACK, LS[k]);
      const double want_ff = law(-20.0, RATIOS[ra], LS[k]), want_fb = law_fb(-20.0, RATIOS[ra], LS[k]);
      ok(fabs(ff - want_ff) < 1e-3, "feed-forward settles on its closed form (1e-3 dB)", ff, want_ff);
      ok(fabs(fb - want_fb) < 1e-3, "feedback settles on the solution of y = x + g_fb(y) (1e-3 dB)", fb, want_fb);
      /* L2: past the knee's reach the two topologies are ONE static curve, -(1 - 1/R)(x - T) */
      if (LS[k] - -20.0 >= 0.5 * KNEE * RATIOS[ra]) ok(fabs(fb - ff) < 2e-3, "above the knee the two static curves agree", fb - ff, 2e-3);
      if (ra == 1 && g_sr == 48000.0f)
        printf("  F 4:1 x=%+6.1f dBFS  GR feed-forward %8.4f dB  feedback %8.4f dB\n", LS[k], ff, fb);
    }
  clean();
}

/* The remaining fraction's area, in samples, after a small step from a settled level: a +0.2 dB DC step at
 * 25 dB over a -30 dB threshold (past the knee's reach at 10:1, whose output sits 2.5 dB over), attack detent 10 ms, the release held at 1.2 s (an attack-direction step never
 * reads it). Small signal the feedback loop is ONE pole rho = p - (1 - p)(R - 1) (§4 L3), area 1/(1 - rho);
 * the feed-forward cascade hears the step on its own sample through four poles q = p^4, area 4q/(1 - q). */
static double step_area(int ra, int tp) {
  const uint32_t pre = (uint32_t)(1.0f * g_sr), post = (uint32_t)(0.5f * g_sr), n = pre + post;
  const float x0 = (float)pow(10.0, -5.0 / 20.0), x1 = (float)pow(10.0, -4.8 / 20.0);
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = i < pre ? x0 : x1;
  omx_glue_instance_init(&g_g, g_sr);
  omx_glue_instance_resolve(&g_g, 0, -30.0f, ra, 4, 3, 0.0f, 0.0f, 100.0f, tp);
  grun(&g_g, g_l, g_r, g_ol, g_or, n);
  drain();
  const double g0 = log((double)g_ol[pre - 1u] / x0), ge = log((double)g_ol[n - 1u] / x1);
  double area = 0.0;
  for (uint32_t i = pre; i < n; i++) area += (log((double)g_ol[i] / x1) - ge) / (g0 - ge);
  return area;
}

static void arm_timing(void) {
  g_arm = "G";
  const double p = exp(-1.0 / (10.0 * 1e-3 * (double)g_sr)), q = pow(p, 4.0);
  for (int ra = 0; ra < 3; ra++) {
    const double r = RATIOS[ra], rho = p - (1.0 - p) * (r - 1.0);
    const double fb = step_area(ra, (int)OMX_DETECTOR_TOPOLOGY_FEEDBACK), ff = step_area(ra, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
    const double want_fb = 1.0 / (1.0 - rho), want_ff = 4.0 * q / (1.0 - q);
    ok(fabs(fb / want_fb - 1.0) < 0.05, "feedback: the step settles on ONE pole p - (1 - p)(R - 1) (area, 5 %)", fb, want_fb);
    ok(fabs(ff / want_ff - 1.0) < 0.05, "feed-forward: the step settles through the four-pole cascade (area, 5 %)", ff, want_ff);
    ok(fabs((ff / fb) / r - 1.0) < 0.1, "the loop divides the detector's time by the ratio (10 %)", ff / fb, r);
    if (g_sr == 48000.0f)
      printf("  G %2.0f:1 settle area: feed-forward %7.1f samples (%.3f ms), feedback %7.1f (%.3f ms), ratio %.2f\n", r, ff,
             1e3 * ff / g_sr, fb, 1e3 * fb / g_sr, ff / fb);
  }
  /* effectiveAttackMs: the detent, or the floor -1000/(sr ln((R - 1)/R)) where the detent is faster */
  for (int ra = 0; ra < 3; ra++)
    for (int at = 0; at < 6; at++) {
      omx_glue_instance_init(&g_g, g_sr);
      omx_glue_instance_resolve(&g_g, 0, -20.0f, ra, at, 0, 0.0f, 0.0f, 100.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDBACK);
      drain();
      const double r = RATIOS[ra], pd = exp(-1.0 / ((double)ATTACKS[at] * 1e-3 * (double)g_sr)), fl = (r - 1.0) / r;
      const double want = pd >= fl ? (double)ATTACKS[at] : -1000.0 / ((double)g_sr * log(fl));
      ok(fabs(omx_glue_instance_effective_attack_ms(&g_g) - want) < 1e-4, "effectiveAttackMs is the detent or the loop's floor (1e-4 ms)",
         omx_glue_instance_effective_attack_ms(&g_g), want);
      omx_glue_instance_resolve(&g_g, 0, -20.0f, ra, at, 0, 0.0f, 0.0f, 100.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
      ok(omx_glue_instance_effective_attack_ms(&g_g) == ATTACKS[at], "feed-forward runs the detent", omx_glue_instance_effective_attack_ms(&g_g), ATTACKS[at]);
    }
  clean();
}

/* L4, every detent at this rate: DC 30 dB over the threshold from cold, then 20 dB down (still 10 over). The loop
 * never rings: the gain never alternates sample to sample (a negative loop pole's chatter) and leaves no finite value
 * (a divergent one). With a fixed release it moves ONE way per phase, down through the attack and up through the
 * release (1e-4 dB of float rounding on a settled envelope). The auto release is program-dependent: the gain is
 * slowed while the loop's own detector releases at the fastest detent, so as the gain comes back the detector can
 * re-attack once; that excursion is measured, printed and pinned (AUTO_REVERSAL_DB), not hidden. */
/* measured 0.2561 .. 0.2567 dB over the nine rates (4:1 and 10:1 at 10 and 30 ms), pinned with headroom of 0.04 */
#define AUTO_REVERSAL_DB 0.30
static void arm_stable(void) {
  g_arm = "H";
  const uint32_t ph = (uint32_t)(0.3f * g_sr), n = 2u * ph;
  const float hi = 1.0f, lo = 0.1f;
  double auto_worst = 0.0;
  for (uint32_t i = 0; i < n; i++) g_l[i] = g_r[i] = i < ph ? hi : lo;
  for (int ra = 0; ra < 3; ra++)
    for (int at = 0; at < 6; at++)
      for (int rl = 0; rl < 5; rl++) {
        omx_glue_instance_init(&g_g, g_sr);
        omx_glue_instance_resolve(&g_g, 0, -30.0f, ra, at, rl, 0.0f, 0.0f, 100.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDBACK);
        grun(&g_g, g_l, g_r, g_ol, g_or, n);
        drain();
        double depth = 0.0, ext = 0.0, prev = 0.0, dprev = 0.0;
        uint32_t nonfinite = 0u, chatter = 0u;
        for (uint32_t i = 0; i < n; i++) {
          const double g = 20.0 * log10((double)g_ol[i] / (double)(i < ph ? hi : lo));
          if (!(g - g == 0.0)) { nonfinite++; continue; }
          if (i == 0u || i == ph) { ext = g; prev = g; dprev = 0.0; continue; }
          /* attack: the gain only goes down, so the excursion is how far it climbed back over its lowest; release the mirror */
          ext = i < ph ? fmin(ext, g) : fmax(ext, g);
          depth = fmax(depth, i < ph ? g - ext : ext - g);
          const double dg = g - prev;
          /* a flip counts above 1e-4 dB a sample: a float gain near 0.3 rounds in steps of ~1e-6 dB, and a turning
           * point dithers at that grain; a negative loop pole's chatter is the size of the reduction */
          if (fabs(dg) > 1e-4 && fabs(dprev) > 1e-4 && dg * dprev < 0.0) chatter++;
          if (fabs(dg) > 1e-4) dprev = dg;
          prev = g;
        }
        const int cell = ra * 100 + at * 10 + rl;
        ok(nonfinite == 0u, "the feedback loop never diverges (non-finite samples; the cell is ra.at.rl)", nonfinite, cell);
        /* none with a fixed release; the auto release's one re-attack is two turning points */
        const uint32_t flips = rl == (int)OMX_GLUE_RELEASES_AUTO ? 2u : 0u;
        ok(chatter <= flips, "the feedback loop never rings: the gain does not alternate (direction flips; the cell)", chatter, cell);
        if (rl == (int)OMX_GLUE_RELEASES_AUTO) {
          auto_worst = fmax(auto_worst, depth);
          ok(depth <= AUTO_REVERSAL_DB, "auto release: one re-attack, within its pinned depth (dB)", depth, AUTO_REVERSAL_DB);
        } else
          ok(depth <= 1e-4, "a fixed release moves one way per phase (dB against the direction; the cell)", depth, cell);
      }
  printf("  H @ %6.0f Hz: the auto release's deepest re-attack %.4f dB (pinned at most %.2f)\n", (double)g_sr, auto_worst, AUTO_REVERSAL_DB);
  clean();
}

/* keyHpHz in feedback: the loop hears the full band, so a corner is inert there, and stays live in feed-forward. */
static void arm_fb_key(void) {
  g_arm = "I";
  const double fb_off = pull(40.0, 0.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDBACK), fb_on = pull(40.0, 200.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDBACK);
  const double ff_on = pull(40.0, 200.0f, (int)OMX_DETECTOR_TOPOLOGY_FEEDFORWARD);
  ok(fb_on == fb_off, "feedback: a corner does not reach the loop's detector", fb_on - fb_off, 0.0);
  ok(ff_on - fb_on > 10.0, "feed-forward: the same corner keeps the 40 Hz sine off the detector", ff_on - fb_on, 10.0);
  clean();
}

OMX_INSTANCE_ORACLE_MAIN("glue_instance", arm_identity(); arm_detents(); arm_law(); arm_key(); arm_hostile(); arm_static();
                         arm_timing(); arm_stable(); arm_fb_key();)
