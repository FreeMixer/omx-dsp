// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <float.h>

#include <omxdsp/mix/omx_slew.h>

/* ---- a gain that crosses a declared time (mix/omx_slew.h). At each of the nine oracle rates:
 *   the step is 1000 / (ms x sr), in double within 1 ulp, and a time under one sample is a jump;
 *   a fade of ms milliseconds from 1 to 0 lands at sample ceil(ms x sr / 1000) within one sample of
 *   float accumulation, whatever the quantum, and each sample is in x (1 - (i + 1) step), the
 *   accumulated rounding (i + 2) eps at most;
 *   one run cut into blocks of 1, 7, 64, 256 and 1024 is the same output, bit for bit, both ways;
 *   a distance of d takes d of the time; the cursor lands on the target exactly and never passes it;
 *   a held gain is the plain product, bit for bit; n = 0 leaves the cursor; a non-finite end heals;
 *   the hold: the way down moves on silence, the way back waits for a carried sample (a silent block
 *   leaves the position alone, then the first carried sample is one step along), and over a stimulus
 *   with silent runs it is the engine's loop (mixer_rt.c, THE REWIRE HOLD, written out here) bit for
 *   bit at quanta 64, 256 and 1024. ---------------------------------------------------------------- */

#define MIX_SLEW_MS 5.0f
#define MIX_SLEW_MAX 4096u
static const float MIX_SLEW_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;
static float g_slew_in[MIX_SLEW_MAX], g_slew_ref[MIX_SLEW_MAX], g_slew_out[MIX_SLEW_MAX];
static float g_slew_in_r[MIX_SLEW_MAX], g_slew_ref_r[MIX_SLEW_MAX], g_slew_out_r[MIX_SLEW_MAX];

/* `in` through omx_slew_gain cut into blocks of `q`: from `from` toward `tgt`. Returns the cursor. */
static float mix_slew_run(const float *in, float *out, uint32_t n, uint32_t q, float from, float tgt, float step) {
  float cur = from;
  for (uint32_t o = 0; o < n; o += q) omx_slew_gain(in + o, out + o, n - o < q ? n - o : q, tgt, step, &cur);
  return cur;
}

/* The engine's loop as the engine wrote it, the reference the hold replaces. */
static void mix_slew_engine(const float *in_l, const float *in_r, float *out_l, float *out_r, uint32_t n, float tgt,
                            float step, float *pos) {
  const int two = in_r != in_l;
  float h = *pos;
  for (uint32_t i = 0; i < n; i++) {
    const int carried = in_l[i] != 0.0f || in_r[i] != 0.0f;
    if (h < tgt) h = h + step < tgt ? h + step : tgt;
    else if (carried) h = h - step > tgt ? h - step : tgt;
    const float g = 1.0f - h;
    out_l[i] = in_l[i] * g;
    if (two) out_r[i] = in_r[i] * g;
  }
  *pos = h;
}

static void arm_mix_slew(void) {
  g_arm = "mix_slew";
  static const uint32_t cuts[] = {1u, 7u, 64u, 256u, 1024u};
  for (uint32_t r = 0; r < OMX_ORACLE_FULL_RATES_COUNT; r++) {
    const float sr = MIX_SLEW_RATES[r];
    const float step = omx_slew_step(MIX_SLEW_MS, sr);
    const double want = 1000.0 / ((double)MIX_SLEW_MS * (double)sr);
    ok(fabs((double)step - want) <= 2.0 * want * FLT_EPSILON, "the step is 1000 / (ms x sr)", step, want);
    ok(omx_slew_step(0.0f, sr) == 1.0f && omx_slew_step(-1.0f, sr) == 1.0f && omx_slew_step(NAN, sr) == 1.0f &&
           omx_slew_step(INFINITY, sr) == 1.0f && omx_slew_step(1e-4f, sr) == 1.0f,
       "no time, a bad time and a time under one sample are a jump", omx_slew_step(0.0f, sr), 1.0);

    const uint32_t len = (uint32_t)ceil((double)MIX_SLEW_MS * (double)sr / 1000.0);
    const uint32_t n = len + 64u;
    for (uint32_t i = 0; i < n; i++) g_slew_in[i] = 0.25f + 0.5f * (float)(i % 17u) / 17.0f;

    /* down: closed form sample by sample, the length, the landing */
    const float end_down = mix_slew_run(g_slew_in, g_slew_ref, n, 1u, 1.0f, 0.0f, step);
    ok(end_down == 0.0f, "the fade lands exactly on 0", end_down, 0.0);
    uint32_t bad = 0u, first_zero = n;
    for (uint32_t i = 0; i < n; i++) {
      const double g = fmax(0.0, 1.0 - (double)(i + 1u) * want);
      const double got = (double)g_slew_ref[i], x = (double)g_slew_in[i];
      if (fabs(got - x * g) > (double)(i + 2u) * 2.0 * FLT_EPSILON * x) bad++;
      if (g_slew_ref[i] == 0.0f && first_zero == n) first_zero = i + 1u;
      if (g_slew_ref[i] < 0.0f) bad++;
    }
    ok(bad == 0u, "each sample is in x (1 - (i + 1) step) within the accumulated rounding", bad, 0);
    ok(first_zero + 1u >= len && first_zero <= len + 1u, "the fade lasts its time in samples, at this rate", first_zero, len);

    /* a block cut changes no sample, either way */
    for (int up = 0; up < 2; up++) {
      const float from = up ? 0.0f : 1.0f, to = up ? 1.0f : 0.0f;
      mix_slew_run(g_slew_in, g_slew_ref, n, 1u, from, to, step);
      for (uint32_t c = 1; c < sizeof cuts / sizeof cuts[0]; c++) {
        const float end = mix_slew_run(g_slew_in, g_slew_out, n, cuts[c], from, to, step);
        ok(end == to && memcmp(g_slew_out, g_slew_ref, n * sizeof(float)) == 0,
           "one run cut into any blocks is the same output, bit for bit", cuts[c], 0);
      }
    }

    /* a distance of a quarter takes a quarter of the time */
    float q = 0.25f;
    uint32_t steps = 0u;
    while (q != 0.5f && steps < n) {
      q = omx_slew_move(q, 0.5f, step);
      steps++;
    }
    ok(steps + 1u >= (len + 3u) / 4u && steps <= len / 4u + 2u, "a quarter of the way takes a quarter of the time", steps, len / 4u);

    /* held: the plain product, bit for bit */
    float cur = 0.75f;
    omx_slew_gain(g_slew_in, g_slew_out, n, 0.75f, step, &cur);
    bad = 0u;
    for (uint32_t i = 0; i < n; i++) bad += g_slew_out[i] != g_slew_in[i] * 0.75f;
    ok(bad == 0u && cur == 0.75f, "a held gain is the plain product, bit for bit", bad, 0);

    /* the hold: down on silence, back only on carried samples */
    memset(g_slew_in_r, 0, sizeof g_slew_in_r);
    float pos = 0.0f;
    omx_slew_hold(g_slew_in_r, g_slew_in_r, g_slew_out, g_slew_out_r, len + 1u, 1.0f, step, &pos);
    ok(pos == 1.0f, "the way down moves on silence and lands at 1", pos, 1.0);
    omx_slew_hold(g_slew_in_r, g_slew_in_r, g_slew_out, g_slew_out_r, n, 0.0f, step, &pos);
    ok(pos == 1.0f, "the way back waits while the input is silent", pos, 1.0);
    float one = 0.5f, out1 = 9.0f, out1r = 9.0f;
    omx_slew_hold(&one, &one, &out1, &out1r, 1u, 0.0f, step, &pos);
    ok(pos == 1.0f - step && out1 == 0.5f * (1.0f - pos) && out1r == 9.0f,
       "the first carried sample is one step along, one leg leaves out_r alone", pos, 1.0f - step);
    const float zero = 0.0f;
    float rr = 0.25f, oz = 9.0f, ozr = 9.0f, p2 = 1.0f;
    omx_slew_hold(&zero, &rr, &oz, &ozr, 1u, 0.0f, step, &p2);
    ok(p2 == 1.0f - step && ozr == 0.25f * (1.0f - p2), "a sample on either leg counts as carried", p2, 1.0f - step);

    /* the engine's loop, bit for bit, at its quanta */
    static const uint32_t quanta[] = {64u, 256u, 1024u};
    uint32_t seed = (uint32_t)sr ^ 0x51ee7u;
    for (uint32_t i = 0; i < MIX_SLEW_MAX; i++) {
      seed = seed * 1664525u + 1013904223u;
      const float x = (float)(int32_t)seed / 2147483648.0f;
      const int gap = (i / 300u) % 3u == 1u;
      g_slew_in[i] = gap ? 0.0f : x;
      g_slew_in_r[i] = gap ? 0.0f : 0.5f * x;
    }
    for (uint32_t c = 0; c < 3u; c++) {
      float a = 0.0f, b = 0.0f;
      uint32_t diff = 0u;
      for (uint32_t o = 0, k = 0; o < MIX_SLEW_MAX; o += quanta[c], k++) {
        const uint32_t m = MIX_SLEW_MAX - o < quanta[c] ? MIX_SLEW_MAX - o : quanta[c];
        const float t = (k % 3u) == 0u ? 1.0f : 0.0f;
        omx_slew_hold(g_slew_in + o, g_slew_in_r + o, g_slew_out + o, g_slew_out_r + o, m, t, step, &a);
        mix_slew_engine(g_slew_in + o, g_slew_in_r + o, g_slew_ref + o, g_slew_ref_r + o, m, t, step, &b);
        diff += memcmp(g_slew_out + o, g_slew_ref + o, m * sizeof(float)) != 0 ||
                memcmp(g_slew_out_r + o, g_slew_ref_r + o, m * sizeof(float)) != 0 || a != b;
      }
      ok(diff == 0u, "the hold is the engine's loop, bit for bit", quanta[c], 0);
    }
  }
  /* the ends */
  float x = 1.0f, y = 9.0f, c0 = 0.4f;
  omx_slew_gain(&x, &y, 0u, 1.0f, 0.1f, &c0);
  ok(c0 == 0.4f && y == 9.0f, "n = 0 leaves the cursor and the output", c0, 0.4);
  float cn = NAN;
  omx_slew_gain(&x, &y, 1u, 1.0f, 0.1f, &cn);
  ok(fabsf(cn - 0.1f) < 1e-6f && fabsf(y - 0.1f) < 1e-6f, "a non-finite cursor reads as 0", cn, 0.1);
  float ct = 0.05f;
  omx_slew_gain(&x, &y, 1u, NAN, 0.1f, &ct);
  ok(ct == 0.0f && y == 0.0f, "a non-finite target reads as 0", ct, 0.0);
  float ph = INFINITY, oh = 9.0f;
  omx_slew_hold(&x, &x, &oh, &oh, 1u, 1.0f, 0.1f, &ph);
  ok(ph == 1.0f && oh == 0.0f, "a non-finite position reads as silent", ph, 1.0);
}
