// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The transient designer's oracle (omx_transient.h), docs/design/specs/2026-09-26-transient-designer.md
// §5, arms A–E, G, H at every declared rate (arm F and the RT rows are the row lane's, §8 item 4).
// Closed forms in double; the step closed form is the cascade's negative-binomial CDF on the
// float32 per-stage poles the kernel runs. `--cost` prints the kernel's ns/sample per rate instead.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_transient.h>

#include "fx_rates.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int g_checks = 0, g_failed = 0;
static const char *g_arm = "";

__attribute__((unused)) static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s — measured %.9g, limit %.9g\n", g_arm, what, measured, limit);
  }
}

#ifdef OMX_CONTRACTS
static uint32_t recorded(const char *token) {
  uint32_t n = 0u;
  const uint32_t kept = omx_contract_log.count < OMX_CONTRACT_MAX ? omx_contract_log.count : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++)
    if (omx_contract_record_ready(&omx_contract_log.rec[i]) && strcmp(omx_contract_log.rec[i].token, token) == 0) n++;
  return n;
}

static void expect_clean(void) {
  const uint32_t seen = omx_contract_log.count;
  const uint32_t kept = seen < OMX_CONTRACT_MAX ? seen : OMX_CONTRACT_MAX;
  for (uint32_t i = 0; i < kept; i++) {
    const struct omx_contract_record *r = &omx_contract_log.rec[i];
    if (omx_contract_record_ready(r)) printf("VIOLATION [%s] %s %s\n", r->stage, r->kind, r->token);
  }
  ok(seen == 0u, "no contract violation in this arm", (double)seen, 0.0);
  omx_contract_reset();
}

#endif

static uint32_t g_seed;
static float rnd(void) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return (float)(g_seed >> 8) / 16777216.0f * 2.0f - 1.0f;
}

/* The N-stage cascade of identical one-poles q from rest: P(Bin(n+N, 1-q) >= N). */
__attribute__((unused)) static double cascade_step(uint32_t n, double q) {
  double below = 0.0, c = 1.0;
  for (int k = 0; k < OMX_DYN_ENV_STAGES; k++) {
    if (k > 0) c *= (double)(n + OMX_DYN_ENV_STAGES + 1u - (uint32_t)k) / (double)k;
    below += c * pow(1.0 - q, k) * pow(q, (double)(n + OMX_DYN_ENV_STAGES) - k);
  }
  return 1.0 - below;
}

/* The arm A bound on a contrast, dB: N·3·2^-24/(1-q) relative on the slowest pole, plus the poly log. */
__attribute__((unused)) static double contrast_bound(double q) { return 20.0 * log10(1.0 + OMX_DYN_ENV_STAGES * 3.0 * ldexp(1.0, -24) / (1.0 - q)) + 2e-4; }

#define MAXLEN 192000u
#ifdef OMX_CONTRACTS
static float g_l[MAXLEN], g_r[MAXLEN], g_l2[MAXLEN], g_r2[MAXLEN];
static float g_gain[MAXLEN], g_gain2[MAXLEN];
#endif

/* Gated noise on both legs (R a scaled copy of L when `stereo`), `amp` the gate's two levels. */
static void gated(float *l, float *r, uint32_t len, float sr, float scale, int stereo) {
  const uint32_t gate = (uint32_t)(sr / 8.0f);
  g_seed = 0x2f6e2b1du;
  for (uint32_t i = 0; i < len; i++) {
    const float amp = ((i / gate) & 1u) ? 0.1f : 0.9f;
    l[i] = scale * (amp * rnd());
    r[i] = stereo ? scale * (0.5f * amp * rnd()) : l[i];
  }
}

#ifdef OMX_CONTRACTS
/* Runs the block through the stage sample by sample, recording the applied gain. */
static void run_gains(float *l, float *r, uint32_t len, const struct omx_transient *t, float *gains) {
  struct omx_transient_state st;
  omx_transient_state_init(&st);
  for (uint32_t i = 0; i < len; i++) {
    omx_transient_process(l + i, r + i, 1u, t, &st);
    gains[i] = st.gain_db;
  }
}

/* A: on a 0.1 -> 1.0 step (rise) or back, the applied g_dB is the knob times min(1, Δ/REF) of the closed form. */
static void arm_steps(float sr) {
  g_arm = "A: steps";
  const float times[2][3] = {{OMX_TRANSIENT_ATTACK_TIME_MS_MIN, OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT, OMX_TRANSIENT_ATTACK_TIME_MS_MAX},
                             {OMX_TRANSIENT_SUSTAIN_TIME_MS_MIN, OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT, OMX_TRANSIENT_SUSTAIN_TIME_MS_MAX}};
  for (int rise = 1; rise >= 0; rise--) {
    for (int k = 0; k < 3; k++) {
      struct omx_transient t;
      const float knob = 12.0f;
      omx_transient_resolve(&t, 0, rise ? knob : 0.0f, rise ? 0.0f : knob,
                            rise ? times[0][k] : OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT,
                            rise ? OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT : times[1][k], 0.0f, sr);
      const double a = rise ? 0.1 : 1.0, b = rise ? 1.0 : 0.1;
      const double qf = rise ? t.on.fast_attack : t.dec.fast_release, qs = rise ? t.on.slow_attack : t.dec.slow_release;
      const double bound = contrast_bound(qs) * knob / OMX_TRANSIENT_REF_DB;
      struct omx_transient_state st;
      omx_transient_state_init(&st);
      for (int s = 0; s < OMX_DYN_ENV_STAGES; s++)
        st.fast.stage[s] = st.slow_attack.stage[s] = st.slow_release.stage[s] = (float)a;
      const uint32_t len = (uint32_t)((rise ? 0.2f : 3.0f) * sr);
      double worst = 0.0, peak = 0.0;
      for (uint32_t i = 0; i < len && i < MAXLEN * 3u; i++) {
        float l = (float)b, r = (float)b;
        omx_transient_process(&l, &r, 1u, &t, &st);
        const double ef = a + (b - a) * cascade_step(i, qf), es = a + (b - a) * cascade_step(i, qs);
        const double delta = rise ? 20.0 * log10(ef / es) : 20.0 * log10(es / ef);
        const double want = knob * fmin(1.0, delta / OMX_TRANSIENT_REF_DB);
        if (fabs(st.gain_db - want) > worst) worst = fabs(st.gain_db - want);
        if (st.gain_db > peak) peak = st.gain_db;
      }
      ok(worst <= bound, rise ? "g_dB on the onset is attackDb·min(1, ΔA/REF) of the closed form" : "g_dB on the decay is sustainDb·min(1, ΔS/REF) of the closed form", worst, bound);
      /* B: a 20 dB step's contrast passes REF (spec §2 table), so the knob reaches its full value exactly. */
      g_arm = "B: full knob";
      ok(peak == knob, "a 20 dB step reaches the knob's full value exactly", peak, knob);
      g_arm = "A: steps";
    }
  }
}

/* C: the attack knob acts only on onsets, the sustain knob only on decays (L2): signs never cross. */
static void arm_ordering(float sr) {
  g_arm = "C: ordering";
  const uint32_t len = (uint32_t)sr;
  const float knobs[2][2] = {{12.0f, 0.0f}, {0.0f, -12.0f}};
  for (int c = 0; c < 2; c++) {
    struct omx_transient t;
    omx_transient_resolve(&t, 0, knobs[c][0], knobs[c][1], OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT, OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT, 0.0f, sr);
    gated(g_l, g_r, len, sr, 1.0f, 1);
    run_gains(g_l, g_r, len, &t, g_gain);
    uint32_t wrong = 0u;
    for (uint32_t i = 0; i < len; i++) wrong += c == 0 ? g_gain[i] < 0.0f : g_gain[i] > 0.0f;
    ok(wrong == 0u, c == 0 ? "a positive attackDb never cuts" : "a negative sustainDb never boosts", wrong, 0.0);
  }
}

/* D: the applied gain does not depend on the input level (L3). */
static void arm_level(float sr) {
  g_arm = "D: level";
  const uint32_t len = (uint32_t)sr, skip = (uint32_t)(0.01f * sr);
  struct omx_transient t;
  omx_transient_resolve(&t, 0, 12.0f, -12.0f, OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT, OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT, 0.0f, sr);
  gated(g_l, g_r, len, sr, 1.0f, 1);
  run_gains(g_l, g_r, len, &t, g_gain);
  const float scales[6] = {0x1p-7f, 0x1p-3f, 0x1p3f, 1e-2f, 1e-1f, 10.0f};
  const double bound = contrast_bound(t.dec.slow_release) * 24.0 / OMX_TRANSIENT_REF_DB;
  uint32_t differ = 0u;
  double worst = 0.0;
  for (int k = 0; k < 6; k++) {
    gated(g_l2, g_r2, len, sr, scales[k], 1);
    run_gains(g_l2, g_r2, len, &t, g_gain2);
    for (uint32_t i = skip; i < len; i++) {
      if (k < 3) differ += g_gain2[i] != g_gain[i];
      else if (fabs(g_gain2[i] - g_gain[i]) > worst) worst = fabs(g_gain2[i] - g_gain[i]);
    }
  }
  ok(differ == 0u, "at x2^-7, x2^-3, x2^3 the applied gain is bit-identical", differ, 0.0);
  ok(worst <= bound, "at x0.01, x0.1, x10 the applied gain agrees within the arm A bound", worst, bound);
}

/* E: over a 5 x 5 x 3 grid of the travels, g_dB inside L4's interval and the output peak inside its bound. */
static void arm_bounds(float sr) {
  g_arm = "E: bounds";
  const uint32_t len = (uint32_t)(0.5f * sr);
  uint32_t outside = 0u, over = 0u;
  for (int ia = 0; ia < 5; ia++)
    for (int is = 0; is < 5; is++)
      for (int io = 0; io < 3; io++) {
        const float a = OMX_TRANSIENT_ATTACK_DB_MIN + ia * (OMX_TRANSIENT_ATTACK_DB_MAX - OMX_TRANSIENT_ATTACK_DB_MIN) / 4.0f;
        const float s = OMX_TRANSIENT_SUSTAIN_DB_MIN + is * (OMX_TRANSIENT_SUSTAIN_DB_MAX - OMX_TRANSIENT_SUSTAIN_DB_MIN) / 4.0f;
        const float o = OMX_TRANSIENT_OUTPUT_DB_MIN + io * (OMX_TRANSIENT_OUTPUT_DB_MAX - OMX_TRANSIENT_OUTPUT_DB_MIN) / 2.0f;
        struct omx_transient t;
        omx_transient_resolve(&t, 0, a, s, OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT, OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT, o, sr);
        gated(g_l, g_r, len, sr, 1.0f, 1);
        float in_peak = 0.0f, out_peak = 0.0f;
        for (uint32_t i = 0; i < len; i++) in_peak = fmaxf(in_peak, fmaxf(fabsf(g_l[i]), fabsf(g_r[i])));
        run_gains(g_l, g_r, len, &t, g_gain);
        const float hi = fmaxf(0.0f, a) + fmaxf(0.0f, s) + o, lo = fminf(0.0f, a) + fminf(0.0f, s) + o;
        for (uint32_t i = 0; i < len; i++) {
          outside += g_gain[i] > hi || g_gain[i] < lo;
          out_peak = fmaxf(out_peak, fmaxf(fabsf(g_l[i]), fabsf(g_r[i])));
        }
        over += (double)out_peak > (double)in_peak * pow(10.0, hi / 20.0) * (1.0 + 4e-6);
      }
  ok(outside == 0u, "g_dB lies inside L4's interval at every sample of every grid point", outside, 0.0);
  ok(over == 0u, "the output peak is inside input peak x 10^(bound/20)", over, 0.0);
}

/* G: bypass and the all-zero gains are memcmp-identical, subnormal input words included (L5). */
static void arm_bypass(float sr) {
  g_arm = "G: bypass";
  const uint32_t len = 4096u;
  for (int mode = 0; mode < 2; mode++) {
    struct omx_transient t;
    omx_transient_resolve(&t, mode == 0, mode == 0 ? 12.0f : 0.0f, mode == 0 ? 12.0f : 0.0f,
                          OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT, OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT, 0.0f, sr);
    gated(g_l, g_r, len, sr, 1.0f, 1);
    for (uint32_t i = 0; i < len; i += 7u) { g_l[i] = 1e-40f; g_r[i] = -1e-41f; }
    memcpy(g_l2, g_l, len * sizeof(float));
    memcpy(g_r2, g_r, len * sizeof(float));
    struct omx_transient_state st, st0;
    omx_transient_state_init(&st);
    memset(&st0, 0, sizeof st0);
    omx_transient_process(g_l, g_r, len, &t, &st);
    ok(memcmp(g_l, g_l2, len * sizeof(float)) == 0 && memcmp(g_r, g_r2, len * sizeof(float)) == 0,
       mode == 0 ? "bypass leaves both legs bit-identical" : "zero gains leave both legs bit-identical", 0.0, 0.0);
    if (mode == 0) ok(memcmp(&st, &st0, sizeof st) == 0, "bypass touches no state word", 0.0, 0.0);
    else ok(st.fast.stage[OMX_DYN_ENV_STAGES - 1] > 0.0f, "at zero gains the detectors still run", st.fast.stage[OMX_DYN_ENV_STAGES - 1], 0.0);
  }
}

/* H: block splits are memcmp-identical, identical legs stay identical, no subnormal survives. */
static void arm_blocks(float sr) {
  g_arm = "H: blocks";
  const uint32_t len = (uint32_t)sr;
  struct omx_transient t;
  omx_transient_resolve(&t, 0, 9.0f, -7.0f, 5.0f, 400.0f, 1.5f, sr);
  gated(g_l, g_r, len, sr, 1.0f, 0);
  memcpy(g_l2, g_l, len * sizeof(float));
  memcpy(g_r2, g_r, len * sizeof(float));
  struct omx_transient_state a, b;
  omx_transient_state_init(&a);
  omx_transient_state_init(&b);
  omx_transient_process(g_l, g_r, len, &t, &a);
  uint32_t seed = 0x51ed27u;
  for (uint32_t i = 0; i < len;) {
    seed = seed * 1664525u + 1013904223u;
    uint32_t n = 1u + (seed >> 8) % 4096u;
    if (n > len - i) n = len - i;
    omx_transient_process(g_l2 + i, g_r2 + i, n, &t, &b);
    i += n;
  }
  ok(memcmp(g_l, g_l2, len * sizeof(float)) == 0 && memcmp(&a, &b, sizeof a) == 0, "one block and random splits 1..4096 are memcmp-identical", 0.0, 0.0);
  ok(memcmp(g_l, g_r, len * sizeof(float)) == 0, "identical legs stay identical", 0.0, 0.0);
  for (uint32_t i = 0; i < len; i++) g_l[i] = g_r[i] = 1e-3f * powf(1e-3f, (float)i / (float)len * 12.0f);
  omx_transient_process(g_l, g_r, len, &t, &a);
  memset(g_l, 0, len * sizeof(float));
  memset(g_r, 0, len * sizeof(float));
  omx_transient_process(g_l, g_r, len, &t, &a);
  uint32_t sub = 0u;
  for (int s = 0; s < OMX_DYN_ENV_STAGES; s++)
    sub += (fpclassify(a.fast.stage[s]) == FP_SUBNORMAL) + (fpclassify(a.slow_attack.stage[s]) == FP_SUBNORMAL) +
           (fpclassify(a.slow_release.stage[s]) == FP_SUBNORMAL);
  ok(sub == 0u, "a decay into silence leaves no subnormal state word", sub, 0.0);
  ok(omx_transient_latency() == 0, "the latency is zero (L1)", omx_transient_latency(), 0.0);
}

/* The kernel follows the generated travel: a slow attack below OMX_TRANSIENT_ATTACK_TIME_MS_MIN is
 * recorded by the resolve and clamped to it. Under tools/transient-perturb.sh's moved header the
 * same 10 ms is below the moved floor; under the real header it is inside. */
static void arm_declared_travel(void) {
  g_arm = "declared travel";
  const float sr = OMX_DECLARED_RATES[0], asked = 10.0f;
  struct omx_transient t;
  omx_contract_reset();
  omx_transient_resolve(&t, 0, 0.0f, 0.0f, asked, OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT, 0.0f, sr);
  const int below = asked < OMX_TRANSIENT_ATTACK_TIME_MS_MIN;
  const struct omx_env_params want = {omx_pole_from_time_ms(below ? OMX_TRANSIENT_ATTACK_TIME_MS_MIN : asked, sr), 0.0f, OMX_DETECT_PEAK};
  float wa, wr;
  omx_env_stage_poles(&want, 1u, &wa, &wr);
  ok(recorded("control-inside-travel") == (below ? 1u : 0u), "the resolve records a slow attack outside the declared travel", recorded("control-inside-travel"), below);
  ok(t.on.slow_attack == wa, "the resolved pole is the declared travel's", t.on.slow_attack, wa);
  printf("fx/transient: declared travel attackTimeMs.min %.1f ms, 10 ms %s\n", OMX_TRANSIENT_ATTACK_TIME_MS_MIN, below ? "RECORDED and clamped" : "inside");
  omx_contract_reset();
}

#endif

/* N-thread identity (rt-thread-split §6): 8 threads, each over its OWN state, run the same input
 * at every declared rate in ragged blocks; every output is memcmp-identical to one thread's. */
#define THREADS 8
#define TLEN 16384u
struct tjob { float sr; float l[TLEN], r[TLEN]; };
static void *tjob_run(void *arg) {
  struct tjob *j = arg;
  struct omx_transient t;
  omx_transient_resolve(&t, 0, 9.0f, -6.0f, 5.0f, 400.0f, 1.0f, j->sr);
  struct omx_transient_state st;
  omx_transient_state_init(&st);
  static const uint32_t splits[5] = {64u, 37u, 1u, 256u, 100u};
  for (uint32_t i = 0, b = 0; i < TLEN; b++) {
    const uint32_t n = splits[b % 5u] < TLEN - i ? splits[b % 5u] : TLEN - i;
    omx_transient_process(j->l + i, j->r + i, n, &t, &st);
    i += n;
  }
  return NULL;
}
__attribute__((unused)) static void arm_threads(void) {
  g_arm = "threads";
  static struct tjob ref, jobs[THREADS];
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    ref.sr = OMX_DECLARED_RATES[ri];
    gated(ref.l, ref.r, TLEN, ref.sr, 1.0f, 1);
    for (int k = 0; k < THREADS; k++) jobs[k] = ref;
    tjob_run(&ref);
    pthread_t th[THREADS];
    for (int k = 0; k < THREADS; k++) pthread_create(&th[k], NULL, tjob_run, &jobs[k]);
    uint32_t differ = 0u;
    for (int k = 0; k < THREADS; k++) {
      pthread_join(th[k], NULL);
      differ += memcmp(jobs[k].l, ref.l, sizeof ref.l) != 0 || memcmp(jobs[k].r, ref.r, sizeof ref.r) != 0;
    }
    ok(differ == 0u, "8 threads over their own state are byte-identical to one", differ, 0.0);
  }
  printf("fx/transient: %d threads x %u rates byte-identical to the reference\n", THREADS, OMX_DECLARED_RATE_COUNT);
}

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* The kernel's cost, ns/sample, per declared rate: 512-sample blocks of gated stereo noise at
 * attackDb +6, sustainDb -6, default times; 200 warm-up blocks, then 9 runs of 2000 blocks, the median. */
static int cost(void) {
  static float l[512], r[512], src_l[512 * 64], src_r[512 * 64];
  printf("# fx/transient cost: quantum 512, 9 runs x 2000 blocks, median; contracts %s\n",
#ifdef OMX_CONTRACTS
         "ON"
#else
         "off"
#endif
  );
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    struct omx_transient t;
    omx_transient_resolve(&t, 0, 6.0f, -6.0f, OMX_TRANSIENT_ATTACK_TIME_MS_DEFAULT, OMX_TRANSIENT_SUSTAIN_TIME_MS_DEFAULT, 0.0f, sr);
    struct omx_transient_state st;
    omx_transient_state_init(&st);
    gated(src_l, src_r, 512u * 64u, sr, 1.0f, 1);
    double runs[9];
    volatile float sink = 0.0f;
    for (int w = 0; w < 200; w++) {
      memcpy(l, src_l + 512u * (w % 64), sizeof l); memcpy(r, src_r + 512u * (w % 64), sizeof r);
      omx_transient_process(l, r, 512u, &t, &st);
    }
    for (int k = 0; k < 9; k++) {
      double spent = 0.0;
      for (int b = 0; b < 2000; b++) {
        memcpy(l, src_l + 512u * (b % 64), sizeof l); memcpy(r, src_r + 512u * (b % 64), sizeof r);
        const double t0 = now_ns();
        omx_transient_process(l, r, 512u, &t, &st);
        spent += now_ns() - t0;
        sink += l[0];
      }
      runs[k] = spent / (2000.0 * 512.0);
    }
    for (int i = 0; i < 9; i++) for (int j = i + 1; j < 9; j++) if (runs[j] < runs[i]) { const double x = runs[i]; runs[i] = runs[j]; runs[j] = x; }
    printf("rate=%.0f ns_per_sample_median=%.2f min=%.2f max=%.2f\n", sr, runs[4], runs[0], runs[8]);
    (void)sink;
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--cost") == 0) return cost();
#ifndef OMX_CONTRACTS
  printf("fx/transient: the oracle needs -DOMX_CONTRACTS; this build runs --cost only\n");
  return 1;
#else
  omx_fx_require_rate_floor();
  omx_contract_reset();
  arm_declared_travel();
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    arm_steps(sr);
    expect_clean();
    arm_ordering(sr);
    expect_clean();
    arm_level(sr);
    expect_clean();
    arm_bounds(sr);
    expect_clean();
    arm_bypass(sr);
    expect_clean();
    arm_blocks(sr);
    expect_clean();
  }
  arm_threads();
  expect_clean();
  printf("fx/transient: %d checks, %d failed, %u declared rates\n", g_checks, g_failed, OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
#endif
}
