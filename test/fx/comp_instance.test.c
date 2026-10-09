// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
//
// The strip compressor instance's oracle (fx/omx_comp_instance.h), the shell over omx_dyn.h's
// omx_dynamics (omx-dsp-dev#34). At every declared rate, contracts on, an empty ledger after each
// arm:
//   A  ready: a NULL instance, a zero, negative or NaN rate is refused; a refused instance is the
//      identity (separate and aliased buffers) and reports no latency; a ready one starts at the
//      declared OMX_COMP_* defaults, comp (the RMS detector), `auto`;
//   B  resolve: the atom is ACT-ABOVE, each value clamped into the declared travel (NaN to the
//      floor), make-up omx_db_to_lin of the clamped dB, the poles omx_pole_from_time_ms of the
//      clamped times at the rate, the attack's ms beside its pole, the kind's detector (comp RMS,
//      limiter peak, anything else comp); a mode outside DETECTOR_OVERSAMPLINGS is
//      `auto`; bypass is enabled off;
//   C  latency: OMX_OVS_LATENCY_4X while 4x is engaged (a 0.2 ms attack on `auto`, or `4x`), else
//      0, and 0 bypassed or `off`;
//   D  run: the instance in uneven host blocks, resolved each block, is omx_dynamics over the whole
//      block, bit for bit — peak and RMS, base path, `4x`, a soft knee with make-up; bypassed is
//      the identity; an engaged comp moves the signal;
//   E  aliasing: outputs on the inputs is the unaliased run, bit for bit;
//   M  mix: the face's mix reaches the kernel's dry share (omx_dyn_dry_share of the clamped
//      percent, a non-finite word at the declared 100 %), and the run is omx_dynamics on that
//      atom, bit for bit; 50 % moves the output against 100 %;
//   H  init, resolve (re-engage included) and run allocate nothing: malloc, calloc, realloc and
//      free are wrapped at link time and counted across the instance's whole life.
#define OMX_CONTRACT_STORAGE 1
#include <omxdsp/fx/omx_comp_instance.h>

/* The detector the console picks: a comp detects RMS, a limiter peak (COMP_KINDS). */
#define KIND(rms) ((rms) ? (int)OMX_COMP_KINDS_COMP : (int)OMX_COMP_KINDS_LIMITER)

#include "fx_rates.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_checks = 0, g_failed = 0;
static const char *g_arm = "";

static void ok(int cond, const char *what, double measured, double limit) {
  g_checks++;
  if (!cond) {
    g_failed++;
    printf("FAIL [%s] %s — measured %.9g, limit %.9g\n", g_arm, what, measured, limit);
  }
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

static uint32_t g_seed;
static float rnd(void) {
  g_seed ^= g_seed << 13;
  g_seed ^= g_seed >> 17;
  g_seed ^= g_seed << 5;
  return (float)(g_seed >> 8) / 16777216.0f * 2.0f - 1.0f;
}

#define N 3000u

static OmxCompInstance g_inst;

/* ---- the allocation counter (linked with --wrap=malloc,calloc,realloc,free) ---------------- */

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);

static int g_counting = 0;
static long g_allocs = 0;

void *__wrap_malloc(size_t n) {
  if (g_counting) g_allocs++;
  return __real_malloc(n);
}
void *__wrap_calloc(size_t n, size_t sz) {
  if (g_counting) g_allocs++;
  return __real_calloc(n, sz);
}
void *__wrap_realloc(void *p, size_t n) {
  if (g_counting) g_allocs++;
  return __real_realloc(p, n);
}
void __wrap_free(void *p) {
  if (g_counting) g_allocs++;
  __real_free(p);
}

static void fill(float *l, float *r) {
  g_seed = 81u;
  for (uint32_t i = 0; i < N; i++) {
    const float amp = ((i / 700u) % 2u == 0u) ? 0.9f : 0.01f;
    l[i] = amp * rnd();
    r[i] = amp * rnd();
  }
}

/* The resolution the instance promises, restated from the declaration. */
static void expect_atom(const struct omx_dyn *p, float sr, int bypass, float t, float ra, float k, float a,
                        float rl, float mk, int rms, int ovs) {
  ok(p->enabled == !bypass, "bypass is enabled off", p->enabled, !bypass);
  ok(p->gc.mode == OMX_DYN_ABOVE, "the comp acts above", p->gc.mode, OMX_DYN_ABOVE);
  ok(p->detect == (rms ? OMX_DETECT_RMS : OMX_DETECT_PEAK), "the detector is the toggle's", p->detect, rms);
  ok(p->gc.thresh_db == omx_clampf(t, OMX_COMP_THRESHOLD_DB_MIN, OMX_COMP_THRESHOLD_DB_MAX), "threshold in travel",
     p->gc.thresh_db, t);
  ok(p->gc.ratio == omx_clampf(ra, OMX_COMP_RATIO_MIN, OMX_COMP_RATIO_MAX), "ratio in travel", p->gc.ratio, ra);
  ok(p->gc.knee_db == omx_clampf(k, OMX_COMP_KNEE_DB_MIN, OMX_COMP_KNEE_DB_MAX), "knee in travel", p->gc.knee_db, k);
  ok(p->gc.makeup_lin == omx_db_to_lin(omx_clampf(mk, OMX_COMP_MAKEUP_DB_MIN, OMX_COMP_MAKEUP_DB_MAX)),
     "make-up is omx_db_to_lin of the clamped dB", p->gc.makeup_lin, mk);
  const float ca = omx_clampf(a, OMX_COMP_ATTACK_MS_MIN, OMX_COMP_ATTACK_MS_MAX);
  const float cr = omx_clampf(rl, OMX_COMP_RELEASE_MS_MIN, OMX_COMP_RELEASE_MS_MAX);
  ok(p->attack_ms == ca, "the attack's ms rides beside its pole", p->attack_ms, ca);
  ok(p->attack_coeff == omx_pole_from_time_ms(ca, sr), "the attack pole is the clamped time's at the rate",
     p->attack_coeff, 0);
  ok(p->release_coeff == omx_pole_from_time_ms(cr, sr), "the release pole is the clamped time's at the rate",
     p->release_coeff, 0);
  const int want = ovs == OMX_DYN_OVS_OFF || ovs == OMX_DYN_OVS_X4 ? ovs : OMX_DYN_OVS_AUTO;
  ok(p->ovs_mode == want, "the oversampling mode, unknown read as auto", p->ovs_mode, want);
}

static void arm_ready(float sr) {
  g_arm = "A ready";
  static float l[N], r[N], ol[N], or_[N], cl[N], cr[N];
  ok(omx_comp_instance_init(NULL, sr) == 0, "a NULL instance is refused", 1, 0);
  static const float bad[] = {0.0f, -48000.0f};
  for (int b = 0; b < 3; b++) {
    const float rate = b < 2 ? bad[b] : nanf("");
    ok(omx_comp_instance_init(&g_inst, rate) == 0 && !g_inst.ready, "a rate that is not positive is refused", rate, 0);
    ok(omx_comp_instance_latency(&g_inst) == 0.0f, "a refused instance reports no latency", 0, 0);
    omx_comp_instance_resolve(&g_inst, 0, -30.0f, 8.0f, 0.0f, 0.1f, 50.0f, 6.0f, 100.0f, KIND(0), OMX_DYN_OVS_X4);
    fill(l, r);
    omx_comp_instance_run(&g_inst, l, r, ol, or_, N);
    memcpy(cl, l, sizeof l);
    memcpy(cr, r, sizeof r);
    omx_comp_instance_run(&g_inst, cl, cr, cl, cr, N);
    ok(!memcmp(ol, l, sizeof l) && !memcmp(or_, r, sizeof r) && !memcmp(cl, l, sizeof l) && !memcmp(cr, r, sizeof r),
       "a refused instance is the identity, separate and aliased", 1, 0);
  }
  omx_comp_instance_run(NULL, l, r, ol, or_, N);
  ok(!memcmp(ol, l, sizeof l) && !memcmp(or_, r, sizeof r), "a NULL instance is the identity", 1, 0);
  ok(omx_comp_instance_init(&g_inst, sr) == 1 && g_inst.ready, "a declared rate is accepted", sr, 0);
  expect_atom(&g_inst.atom, sr, 0, OMX_COMP_THRESHOLD_DB_DEFAULT, OMX_COMP_RATIO_DEFAULT, OMX_COMP_KNEE_DB_DEFAULT,
              OMX_COMP_ATTACK_MS_DEFAULT, OMX_COMP_RELEASE_MS_DEFAULT, OMX_COMP_MAKEUP_DB_DEFAULT, 1, OMX_DYN_OVS_AUTO);
  /* a kind outside COMP_KINDS reads as the declared default, comp: the RMS detector */
  static const int BAD[] = {-1, 2, 7, 0x7fffffff};
  for (int m = 0; m < 4; m++) {
    omx_comp_instance_resolve(&g_inst, 0, -20.0f, 4.0f, 6.0f, 5.0f, 100.0f, 0.0f, 100.0f, BAD[m], 9);
    ok(g_inst.atom.detect == OMX_DETECT_RMS && g_inst.atom.ovs_mode == OMX_DYN_OVS_AUTO,
       "a kind or detector oversampling outside its ids reads as the declared default", BAD[m], 0);
  }
}

static void arm_resolve(float sr) {
  g_arm = "B resolve";
  omx_comp_instance_init(&g_inst, sr);
  static const float v[][6] = {
      {-18.0f, 4.0f, 6.0f, 5.0f, 200.0f, 0.0f},           /* in travel */
      {-40.0f, 20.0f, 0.0f, 0.2f, 40.0f, 12.0f},          /* in travel, the 4x attack */
      {10.0f, 1000.0f, 99.0f, 9000.0f, 9000.0f, 99.0f},   /* above the travel */
      {-200.0f, 0.0f, -5.0f, -5.0f, -5.0f, -30.0f},       /* below it */
  };
  static const int modes[] = {OMX_DYN_OVS_AUTO, OMX_DYN_OVS_OFF, OMX_DYN_OVS_X4, 7, -1};
  for (int bypass = 0; bypass < 2; bypass++)
    for (int rms = 0; rms < 2; rms++)
      for (int i = 0; i < 5; i++) {
        float t, ra, k, a, rl, mk;
        if (i < 4) { t = v[i][0]; ra = v[i][1]; k = v[i][2]; a = v[i][3]; rl = v[i][4]; mk = v[i][5]; }
        else t = ra = k = a = rl = mk = nanf("");
        const int ovs = modes[(i + bypass + rms) % 5];
        omx_comp_instance_resolve(&g_inst, bypass, t, ra, k, a, rl, mk, 100.0f, KIND(rms), ovs);
        expect_atom(&g_inst.atom, sr, bypass, t, ra, k, a, rl, mk, rms, ovs);
      }
}

static void arm_latency(float sr) {
  g_arm = "C latency";
  omx_comp_instance_init(&g_inst, sr);
  ok(omx_comp_instance_latency(&g_inst) == 0.0f, "the defaults report none", omx_comp_instance_latency(&g_inst), 0);
  omx_comp_instance_resolve(&g_inst, 0, -18.0f, 4.0f, 6.0f, 0.2f, 200.0f, 0.0f, 100.0f, KIND(0), OMX_DYN_OVS_AUTO);
  ok(omx_comp_instance_latency(&g_inst) == (float)OMX_OVS_LATENCY_4X, "a 0.2 ms attack on auto reports the 4x latency",
     omx_comp_instance_latency(&g_inst), OMX_OVS_LATENCY_4X);
  omx_comp_instance_resolve(&g_inst, 0, -18.0f, 4.0f, 6.0f, 0.2f, 200.0f, 0.0f, 100.0f, KIND(0), OMX_DYN_OVS_OFF);
  ok(omx_comp_instance_latency(&g_inst) == 0.0f, "off reports none", omx_comp_instance_latency(&g_inst), 0);
  omx_comp_instance_resolve(&g_inst, 0, -18.0f, 4.0f, 6.0f, 20.0f, 200.0f, 0.0f, 100.0f, KIND(0), OMX_DYN_OVS_X4);
  ok(omx_comp_instance_latency(&g_inst) == (float)OMX_OVS_LATENCY_4X, "4x reports the 4x latency",
     omx_comp_instance_latency(&g_inst), OMX_OVS_LATENCY_4X);
  omx_comp_instance_resolve(&g_inst, 1, -18.0f, 4.0f, 6.0f, 20.0f, 200.0f, 0.0f, 100.0f, KIND(0), OMX_DYN_OVS_X4);
  ok(omx_comp_instance_latency(&g_inst) == 0.0f, "bypassed reports none", omx_comp_instance_latency(&g_inst), 0);
}

static void arm_run(float sr) {
  g_arm = "D run";
  static float l[N], r[N], ol[N], or_[N], el[N], er[N];
  static const uint32_t blocks[] = {1u, 77u, 256u, 1000u, 513u};
  /* bypass, knee, attack, make-up, rms, ovs */
  static const float cases[][6] = {
      {0, 0.0f, 5.0f, 0.0f, 0, OMX_DYN_OVS_AUTO},
      {0, 6.0f, 10.0f, 6.0f, 1, OMX_DYN_OVS_AUTO},
      {0, 0.0f, 0.2f, 0.0f, 0, OMX_DYN_OVS_AUTO},
      {0, 12.0f, 3.0f, 3.0f, 1, OMX_DYN_OVS_X4},
      {1, 6.0f, 0.2f, 6.0f, 0, OMX_DYN_OVS_X4},
  };
  for (int ci = 0; ci < 5; ci++) {
    const int bypass = (int)cases[ci][0], rms = (int)cases[ci][4], ovs = (int)cases[ci][5];
    fill(l, r);
    omx_comp_instance_init(&g_inst, sr);
    for (uint32_t off = 0, b = 0; off < N; b++) {
      const uint32_t m = N - off < blocks[b % 5u] ? N - off : blocks[b % 5u];
      omx_comp_instance_resolve(&g_inst, bypass, -30.0f, 8.0f, cases[ci][1], cases[ci][2], 80.0f, cases[ci][3], 100.0f,
                                    KIND(rms), ovs);
      omx_comp_instance_run(&g_inst, l + off, r + off, ol + off, or_ + off, m);
      off += m;
    }
    memcpy(el, l, sizeof l);
    memcpy(er, r, sizeof r);
    struct omx_dyn_state st;
    omx_dyn_state_init(&st, 1u);
    omx_dynamics(el, er, N, &g_inst.atom, &st);
    uint32_t mis = 0, moved = 0;
    for (uint32_t i = 0; i < N; i++) {
      mis += ol[i] != el[i] || or_[i] != er[i];
      moved += ol[i] != l[i] || or_[i] != r[i];
    }
    ok(mis == 0u, "the instance is omx_dynamics over the whole block, bit for bit", mis, ci);
    if (bypass) ok(moved == 0u, "a bypassed comp is the identity", moved, 0);
    else ok(moved > 0u, "an engaged comp moves the signal", moved, 0);
  }
}

static void arm_alias(float sr) {
  g_arm = "E aliasing";
  static float l[N], r[N], ol[N], or_[N];
  fill(l, r);
  omx_comp_instance_init(&g_inst, sr);
  omx_comp_instance_resolve(&g_inst, 0, -30.0f, 8.0f, 6.0f, 0.2f, 80.0f, 6.0f, 100.0f, KIND(0), OMX_DYN_OVS_AUTO);
  omx_comp_instance_run(&g_inst, l, r, ol, or_, N);
  omx_comp_instance_init(&g_inst, sr);
  omx_comp_instance_resolve(&g_inst, 0, -30.0f, 8.0f, 6.0f, 0.2f, 80.0f, 6.0f, 100.0f, KIND(0), OMX_DYN_OVS_AUTO);
  omx_comp_instance_run(&g_inst, l, r, l, r, N);
  uint32_t mis = 0;
  for (uint32_t i = 0; i < N; i++) mis += l[i] != ol[i] || r[i] != or_[i];
  ok(mis == 0u, "outputs on the inputs is the unaliased run, bit for bit", mis, 0);
}

static void arm_mix(float sr) {
  g_arm = "M mix";
  static float l[N], r[N], ol[N], or_[N], el[N], er[N], fl[N], fr[N];
  static const float mixes[] = {50.0f, 0.0f, 100.0f, 150.0f, -5.0f, 25.0f};
  for (int i = 0; i < 7; i++) {
    const float mx = i < 6 ? mixes[i] : nanf("");
    const float want = omx_dyn_dry_share(mx - mx != 0.0f ? OMX_COMP_MIX_PCT_DEFAULT
                                         : mx < OMX_COMP_MIX_PCT_MIN ? OMX_COMP_MIX_PCT_MIN
                                         : mx > OMX_COMP_MIX_PCT_MAX ? OMX_COMP_MIX_PCT_MAX : mx);
    fill(l, r);
    omx_comp_instance_init(&g_inst, sr);
    omx_comp_instance_resolve(&g_inst, 0, -30.0f, 8.0f, 6.0f, 5.0f, 80.0f, 3.0f, mx, KIND(1), OMX_DYN_OVS_AUTO);
    ok(g_inst.atom.dry == want, "the mix lands in the kernel's dry share", g_inst.atom.dry, want);
    omx_comp_instance_run(&g_inst, l, r, ol, or_, N);
    memcpy(el, l, sizeof l);
    memcpy(er, r, sizeof r);
    struct omx_dyn_state st;
    omx_dyn_state_init(&st, 1u);
    omx_dynamics(el, er, N, &g_inst.atom, &st);
    uint32_t mis = 0;
    for (uint32_t j = 0; j < N; j++) mis += ol[j] != el[j] || or_[j] != er[j];
    ok(mis == 0u, "with a mix the instance is omx_dynamics on that atom, bit for bit", mis, mx);
    if (i == 0) memcpy(fl, ol, sizeof fl), memcpy(fr, or_, sizeof fr);
    if (i == 2) {
      uint32_t moved = 0;
      for (uint32_t j = 0; j < N; j++) moved += ol[j] != fl[j];
      ok(moved > 0u, "50 % differs from 100 %", moved, 0);
    }
  }
}

static void arm_no_alloc(float sr) {
  g_arm = "H no allocation";
  static float l[1024], r[1024];
  for (int i = 0; i < 1024; i++) l[i] = 0.5f * sinf(0.03f * (float)i), r[i] = 0.4f * cosf(0.021f * (float)i);
  g_allocs = 0;
  g_counting = 1;
  const int ready = omx_comp_instance_init(&g_inst, sr);
  omx_comp_instance_resolve(&g_inst, 0, -30.0f, 8.0f, 6.0f, 0.2f, 80.0f, 6.0f, 100.0f, KIND(1), OMX_DYN_OVS_X4);
  omx_comp_instance_run(&g_inst, l, r, l, r, 512u);
  omx_comp_instance_resolve(&g_inst, 1, -30.0f, 8.0f, 6.0f, 0.2f, 80.0f, 6.0f, 100.0f, KIND(0), OMX_DYN_OVS_AUTO);
  omx_comp_instance_run(&g_inst, l, r, l, r, 256u);
  omx_comp_instance_resolve(&g_inst, 0, -24.0f, 4.0f, 0.0f, 10.0f, 200.0f, 0.0f, 100.0f, KIND(0), OMX_DYN_OVS_OFF);
  omx_comp_instance_run(&g_inst, l + 512, r + 512, l + 512, r + 512, 512u);
  g_counting = 0;
  ok(ready && g_allocs == 0, "init, resolve, a re-engage and run allocate nothing", (double)g_allocs, 0);
  void *(*volatile alloc)(size_t) = malloc;
  void (*volatile release)(void *) = free;
  g_allocs = 0;
  g_counting = 1;
  release(alloc(16));
  g_counting = 0;
  ok(g_allocs == 2, "the allocation counter counts (a malloc and a free inside the window)", (double)g_allocs, 2);
}

int main(void) {
  omx_fx_require_rate_floor();
  omx_contract_reset();
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
    const float sr = OMX_DECLARED_RATES[ri];
    arm_ready(sr);
    expect_clean();
    arm_resolve(sr);
    expect_clean();
    arm_latency(sr);
    expect_clean();
    arm_run(sr);
    expect_clean();
    arm_alias(sr);
    expect_clean();
    arm_mix(sr);
    expect_clean();
    arm_no_alloc(sr);
    expect_clean();
  }
  printf("fx/comp_instance: %d checks, %d failed, %u rates\n", g_checks, g_failed, (unsigned)OMX_DECLARED_RATE_COUNT);
  return g_failed == 0 ? 0 : 1;
}
