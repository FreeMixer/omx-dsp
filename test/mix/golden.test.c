// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * The golden digests of the mix module (include/omxdsp/mix/, FreeMixer/omx-dsp#98): hashed per
 * kernel and per rate at the nine oracle rates and compared with test/golden/mix.sha256.
 *
 *   make test-mix                     compare
 *   build/mix_golden --write          print the lines the digest file holds
 *
 * The committed digests were written by this program compiled against the engine's own copies
 * (OpenMixer's mix_dsp.h, mix_lcr.h, mix_bus.h and mix_gr_meter.h; engine commit ae922d2e7d74 for
 * pan to clip, ca811a46c for hold, gr, rta, bus_sum and rt_load) by tools/mix-engine-identity.sh, so the moved code is
 * held to the engine's output bit for bit. -DOMX_GOLDEN_ENGINE is that
 * build: each kernel below names the library's call and the engine's.
 *
 * The stimulus makes no libm call (an integer generator, rational pan sweeps); the kernels call
 * cosf, sinf, sqrtf, powf and log10f, the engine's own arithmetic. This program is compiled with
 * test/support/log10f_subst.h, so its log10f is the correctly rounded omx_log10f and the
 * gain-reduction digests read the same on glibc 2.36, 2.39 and 2.43.
 *
 *   pan      the stereo pan law over a pan sweep past both ends
 *   lcr      the LCR layout at four divergences over the same sweep
 *   strip    a strip's gains into a stereo and an LCR bus: mono and stereo strips, assigned or
 *            not, muted or not, at four levels
 *   fold     the mono fold's gain at levels and coefficients past the finite (NaN, ±inf)
 *   bus_out  a run of blocks through a stereo and a mono bus's out stage, the controls moving
 *            between blocks: the bus, the cursors and the measured peaks
 *   peak     the block peak over one, two and aliased legs, and the per-leg peaks
 *   clip     the sustained-over run on blocks with runs of 0 to 5 full-scale samples
 *   hold     the peak hold over a run of leg peaks, with equal, signed-zero and NaN peaks and a
 *            cleared slot every 50 blocks
 *   gr       the gain-reduction reading over runs of blocks of seven sizes, at seven release poles
 *            (inside [0, 1) and outside it): the reading and its hold state after each block
 *   rta      one analyser tap over two legs of noise: the rate's spectrum alone, then with the
 *            low zoom (the engine writes the legs into its rings and reads its snapshot back)
 *   bus_sum  sends of noise into a stereo and a mono bus over a run of blocks, the level, the image
 *            and the fold moving between blocks, NaN and infinite levels and cursors among them:
 *            the bus, the cursors and whether each send wrote
 *   rt_load  the load meter over a run of cycles of moving work, quantum and driver xrun total,
 *            the peak cleared every 50 cycles: the smoothed reading, the peak, the overruns, the
 *            cycles, the xrun events and the total
 *
 * The rest of the mix maths has its own engine build, -DOMX_GOLDEN_ENGINE_REST: the engine copies of
 * these kernels outlived the ones above (OpenMixer's mix_dsp.h and mix_sum.h at engine commit
 * e4c7e00a4, where the kernels above already call this library), so that build renders only these,
 * and the kernels above through the library:
 *
 *   split        a strip's level and image apart over a stereo bus: mono and stereo strips,
 *                assigned or not, muted or not, at four levels, over the pan sweep
 *   route_fill   a run of blocks of noise copied at a ramped gain, the target moving between blocks
 *                (a polarity invert, NaN and infinite targets, an empty block), in place every
 *                third block: the output and the cursor
 *   route_fold   the same run folded L + R into one buffer: the output and the cursor
 *   fold_fill    the same run folded in place onto both legs, the two cursors apart at the start:
 *                both legs and both cursors
 *   strip_meter  a strip's post-fader reading over blocks of noise on one, two and aliased legs at
 *                levels from 0 to +10 dB
 *   settled      the settled-silence test over every combination of five values (0, −0, 1e-30,
 *                0.5, NaN) in its four arguments
 *   lcr_clev     the LCR fold's centre level, and a level times the cue's fold switch times it
 *
 * The bus stages (mix/omx_bus_stage.h) have a third engine build, -DOMX_GOLDEN_ENGINE_STRIP: the
 * engine copies of these three outlived all the rest (OpenMixer's mix_dsp.h at engine commit
 * 108aa0952, which takes everything else from this library), and they keep the same names here, so
 * only the header differs between the two builds:
 *
 *   mix_strip      a run of blocks of noise summed into a stereo bus that already holds noise, mono
 *                  and stereo sources, the targets moving between blocks (a polarity invert, +10 dB,
 *                  NaN and infinite targets, poisoned cursors, an empty block): both legs and both
 *                  cursors
 *   bus_out_gain   a run of blocks through a stereo and a mono bus's out gain, fader (to +10 dB),
 *                  mute and balance moving between blocks, an empty block: both legs and the cursors
 *   bus_out_stage  the same run through the whole out stage, the meter slots held across blocks and
 *                  cleared every fifth: both legs, the cursors and both slots
 *
 * A dynamics slot's key (mix/omx_key.h) and the source switch (mix/omx_src_blend.h) each have an
 * engine build of their own, -DOMX_GOLDEN_ENGINE_KEY and -DOMX_GOLDEN_ENGINE_PB: the engine's
 * omx_key_build (OpenMixer's mix_lane.h) and omx_src_blend (its mix_pb.h), cut out of those
 * headers by tools/mix-engine-identity.sh into the header the build includes in place of the
 * library's (the rest of mix_lane.h needs the engine's plugin host). They keep the same names, so
 * only that header differs between the builds; everything else is the library's:
 *
 *   key    runs of blocks of noise keyed from the lane's two legs, from one leg and from another
 *          lane's block, the high-pass and low-pass edges switched on and off and their sections
 *          swapped between blocks, listen-only keys, no-key slots and an empty block: what the
 *          call returns, whether it built, the key buffer and both sections' history
 *   pb     a run of blocks of noise switched between live and track, settled and ramping, with no
 *          live input, NaN and infinite targets, a poisoned cursor and an empty block: the track
 *          and the cursor
 *
 * The level meter's reading (omx_meter_level_db) has no engine copy: it is the omx-plugins faces'
 * g_level_db, and its digests are the library's own:
 *
 *   level  blocks of noise from silence to +10 dB, NaN and infinite samples among them, an empty
 *          block, at five display minimums: each reading, in double
 */
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <omxdsp/omxdsp_definitions.h>

#if defined(OMX_GOLDEN_ENGINE) + defined(OMX_GOLDEN_ENGINE_REST) + defined(OMX_GOLDEN_ENGINE_STRIP) + \
        defined(OMX_GOLDEN_ENGINE_KEY) + defined(OMX_GOLDEN_ENGINE_PB) > 1
#error "one engine build at a time: OMX_GOLDEN_ENGINE (the first kernels), OMX_GOLDEN_ENGINE_REST, OMX_GOLDEN_ENGINE_STRIP, OMX_GOLDEN_ENGINE_KEY or OMX_GOLDEN_ENGINE_PB"
#endif
#if !defined(OMX_GOLDEN_ENGINE) && !defined(OMX_GOLDEN_ENGINE_REST) && !defined(OMX_GOLDEN_ENGINE_STRIP) && \
    !defined(OMX_GOLDEN_ENGINE_KEY) && !defined(OMX_GOLDEN_ENGINE_PB)
#define GOLDEN_LIBRARY 1 /* the library build: every kernel, none through an engine copy */
#endif

#ifdef OMX_GOLDEN_ENGINE
#include "mix_bus.h"
#include "mix_dsp.h"
#include "mix_gr_meter.h"
#include "mix_lcr.h"
#else
#ifdef OMX_GOLDEN_ENGINE_REST
#include "mix_dsp.h"
#include "mix_sum.h"
#else
#ifdef OMX_GOLDEN_ENGINE_STRIP
#include "mix_dsp.h" /* the engine's omx_mix_strip, omx_bus_out_gain and omx_bus_out_stage */
#else
#include <omxdsp/mix/omx_bus_stage.h>
#endif
#include <omxdsp/mix/omx_route_gain.h>
#endif
#include <omxdsp/mix/omx_bus_sum.h>
#include <omxdsp/mix/omx_rt_load.h>
#include <omxdsp/mix/omx_bus_out.h>
#include <omxdsp/mix/omx_meter.h>
#include <omxdsp/mix/omx_mix_coef.h>
#include <omxdsp/mix/omx_pan_law.h>
#include <omxdsp/mix/omx_rta_tap.h>
#ifdef OMX_GOLDEN_ENGINE_KEY
#include "mix_key_engine.h" /* the engine's struct omx_key and omx_key_build, cut out of mix_lane.h */
#elif defined(GOLDEN_LIBRARY)
#include <omxdsp/mix/omx_key.h>
#endif
#ifdef OMX_GOLDEN_ENGINE_PB
#include "mix_src_blend_engine.h" /* the engine's omx_src_blend, cut out of mix_pb.h */
#elif defined(GOLDEN_LIBRARY)
#include <omxdsp/mix/omx_src_blend.h>
#endif
#endif

#include "../fx/sha256.h"

static const float GOLDEN_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;

/* The centre window's half width: the console's limit in the engine, a layout parameter here. */
#define GOLDEN_CENTRE_HALF_WIDTH 0.5f

/* ---- the calls under test, the library's or the engine's ------------------------------------ */

static void k_pan(float pan, float *l, float *r) {
#ifdef OMX_GOLDEN_ENGINE
  omx_pan_law(pan, l, r);
#else
  omx_pan_stereo(pan, l, r);
#endif
}

static void k_lcr(float pan, float div, float g[3]) {
#ifdef OMX_GOLDEN_ENGINE
  omx_lcr_pan(pan, div, &g[0], &g[1], &g[2]);
#else
  struct omx_pan_layout l;
  omx_pan_layout_lcr(&l, GOLDEN_CENTRE_HALF_WIDTH);
  omx_pan_gains(&l, pan, div, g);
#endif
}

static void k_strip(int lcr, int channels, int active, float gain, float fader, int mute, float pan,
                    float div, float g[3]) {
  g[2] = 0.0f;
#ifdef OMX_GOLDEN_ENGINE
  if (lcr)
    omx_strip_main_coefs_lcr(channels, active, gain, fader, mute, pan, div, &g[0], &g[1], &g[2]);
  else
    omx_strip_main_coefs(channels, active, gain, fader, mute, pan, &g[0], &g[1]);
#else
  struct omx_pan_layout l;
  if (lcr) omx_pan_layout_lcr(&l, GOLDEN_CENTRE_HALF_WIDTH);
  else omx_pan_layout_stereo(&l);
  omx_mix_strip_gains(&l, channels, active, gain, fader, mute, pan, div, g);
#endif
}

/* The fold's gain. The engine has it only inside its mono-bus summing loop, so it is read out of
 * that loop: a unit left sample and a silent right one, a second block once the ramp has settled. */
static float k_fold(float level, float coef) {
#ifdef OMX_GOLDEN_ENGINE
  const float one = 1.0f, nothing = 0.0f;
  float bus = 0.0f, cl = 0.0f, cr = 0.0f;
  omx_bus_add_ramp(&one, &nothing, &bus, NULL, 1, level, 1.0f, 1.0f, &cl, &cr, OMX_BUS_FOLD, coef);
  bus = 0.0f;
  omx_bus_add_ramp(&one, &nothing, &bus, NULL, 1, level, 1.0f, 1.0f, &cl, &cr, OMX_BUS_FOLD, coef);
  return bus;
#else
  return omx_mix_fold_gain(level, coef);
#endif
}

static void k_bus_out(float *l, float *r, uint32_t n, int channels, float fader, int mute,
                      float bal, float *cl, float *cr, float *pl, float *pr) {
#ifdef OMX_GOLDEN_ENGINE
  _Atomic float sl = 0.0f, sr = 0.0f;
  omx_bus_out_stage(l, r, n, channels, fader, mute, bal, cl, cr, &sl, &sr);
  *pl = sl;
  *pr = sr;
#else
  omx_bus_out_run(l, r, n, channels, fader, mute, bal, cl, cr, pl, pr);
#endif
}

static float k_peak(const float *a, const float *b, uint32_t n) {
#ifdef OMX_GOLDEN_ENGINE
  return omx_block_peak(a, b, n);
#else
  return omx_meter_block_peak(a, b, n);
#endif
}

static void k_leg_peaks(const float *a, const float *b, uint32_t n, float cl, float cr, float *pl,
                        float *pr) {
#ifdef OMX_GOLDEN_ENGINE
  omx_strip_meter_legs(a, b, n, cl, cr, pl, pr);
#else
  omx_meter_leg_peaks(a, b, n, cl, cr, pl, pr);
#endif
}

static int k_clip(const float *a, const float *b, uint32_t n, float scale) {
#ifdef OMX_GOLDEN_ENGINE
  return omx_sustained_over(a, b, n, scale, OMX_CLIP_LINEAR, OMX_CLIP_RUN);
#else
  return omx_meter_clip_run(a, b, n, scale, OMX_METER_CLIP_LINEAR, OMX_METER_CLIP_RUN);
#endif
}

static float k_hold(float held, float peak) {
#ifdef OMX_GOLDEN_ENGINE
  _Atomic float slot = held, other = 0.0f;
  omx_meter_hold(&slot, &other, peak, 0.0f);
  return slot;
#else
  return omx_meter_hold_peak(held, peak);
#endif
}

#ifdef OMX_GOLDEN_ENGINE
typedef struct omx_gr_meter golden_gr;
#else
typedef struct omx_meter_gr golden_gr;
#endif

static float k_gr(golden_gr *m, float block_db, uint32_t n, float sr, float pole) {
#ifdef OMX_GOLDEN_ENGINE
  return omx_gr_meter_step(m, block_db, n, sr, pole);
#else
  return omx_meter_gr_step(m, block_db, n, sr, pole);
#endif
}

/* One tap's reading over the newest window of two legs of OMX_RTA_LOW_WINDOW samples. The
 * engine's sum is inside its ring snapshot, so its legs go through two of its rings first. */
static float g_leg_l[OMX_RTA_LOW_WINDOW], g_leg_r[OMX_RTA_LOW_WINDOW];

static const float *k_rta(uint32_t n, int low, const float **low_mags) {
#ifdef OMX_GOLDEN_ENGINE
  static struct omx_ring rl, rr;
  static float leg[OMX_RTA_LOW_WINDOW];
  static struct omx_rta_tap_snap snap;
  static struct omx_rta_tap_scratch scr;
  static struct omx_rta_tap_mags out;
  memset(&rl, 0, sizeof rl);
  memset(&rr, 0, sizeof rr);
  omx_ring_write(&rl, g_leg_l, OMX_RTA_LOW_WINDOW);
  omx_ring_write(&rr, g_leg_r, OMX_RTA_LOW_WINDOW);
  omx_rta_tap_snapshot(&rl, &rr, leg, &snap, n, low);
  omx_rta_tap_compute(&snap, &scr, &out, n, low);
#else
  static struct omx_rta_tap_window w;
  static struct omx_rta_tap_work scr;
  static struct omx_rta_tap_reading out;
  omx_rta_tap_mono_sum(w.block, g_leg_l + OMX_RTA_LOW_WINDOW - n, g_leg_r + OMX_RTA_LOW_WINDOW - n, n);
  if (low) omx_rta_tap_mono_sum(w.block_low, g_leg_l, g_leg_r, OMX_RTA_LOW_WINDOW);
  omx_rta_tap_read(&w, &scr, &out, n, low);
#endif
  *low_mags = out.low;
  return out.mags;
}

static int k_bus_sum(const float *l, const float *r, float *bl, float *br, uint32_t n, float level,
                     float il, float ir, float *cl, float *cr, int fold, float fold_coef) {
#ifdef OMX_GOLDEN_ENGINE
  return omx_bus_add_ramp(l, r, bl, br, n, level, il, ir, cl, cr, fold ? OMX_BUS_FOLD : OMX_BUS_FOLLOW,
                          fold_coef);
#else
  return omx_bus_sum_add(l, r, bl, br, n, level, il, ir, cl, cr,
                         fold ? OMX_BUS_SUM_FOLD : OMX_BUS_SUM_FOLLOW, fold_coef);
#endif
}

/* The load meter's state as the reader sees it. The engine folds into its shared stats block;
 * the library's pure steps are folded here the same way. */
struct golden_load {
  float ema, peak;
  uint32_t overruns, cycles, xruns, seeded;
  uint64_t xrun_ns;
};

#ifdef OMX_GOLDEN_ENGINE
static struct omx_rt_load g_load;
#endif

static void k_rt_load_clear(struct golden_load *g) {
#ifdef OMX_GOLDEN_ENGINE
  memset(&g_load, 0, sizeof g_load);
#endif
  memset(g, 0, sizeof *g);
}

static void k_rt_load(struct golden_load *g, uint64_t busy_ns, uint32_t frames, float sr,
                      uint64_t xrun_ns, int clear_peak) {
#ifdef OMX_GOLDEN_ENGINE
  if (clear_peak) atomic_store(&g_load.peak, 0.0f);
  omx_rt_load_update(&g_load, omx_rt_busy_ratio(busy_ns, frames, sr));
  omx_rt_xrun_update(&g_load, xrun_ns);
  g->ema = g_load.ema;
  g->peak = g_load.peak;
  g->overruns = g_load.overruns;
  g->cycles = g_load.cycles;
  g->xruns = g_load.driver_xruns;
  g->seeded = g_load.driver_xrun_seeded;
  g->xrun_ns = g_load.driver_xrun_ns;
#else
  if (clear_peak) g->peak = 0.0f;
  const float ratio = omx_rt_load_ratio(busy_ns, frames, sr);
  g->ema = omx_rt_load_smooth(g->ema, g->cycles, ratio);
  g->peak = omx_rt_load_peak(g->peak, ratio);
  if (omx_rt_load_overrun(ratio)) g->overruns++;
  g->cycles++;
  g->xruns += omx_rt_load_xrun_events(g->seeded != 0u, g->xrun_ns, xrun_ns);
  g->xrun_ns = xrun_ns;
  g->seeded = 1u;
#endif
}

/* ---- the rest of the mix maths, the library's or the engine's (OMX_GOLDEN_ENGINE_REST); the first
 * kernels' engine build carries none of it ---------------------------------------------------- */
#ifndef OMX_GOLDEN_ENGINE

static void k_split(int channels, int active, float gain, float fader, int mute, float pan, float *coef,
                    float img[2]) {
#ifdef OMX_GOLDEN_ENGINE_REST
  omx_sum_main_split(channels, active, gain, fader, mute, pan, coef, &img[0], &img[1]);
#else
  struct omx_pan_layout l;
  omx_pan_layout_stereo(&l);
  omx_mix_strip_split(&l, channels, active, gain, fader, mute, pan, 0.0f, coef, img);
#endif
}

static void k_route_fill(const float *in, float *out, uint32_t n, float tgt, float *cur) {
#ifdef OMX_GOLDEN_ENGINE_REST
  omx_fill_gain(in, out, n, tgt, cur);
#else
  omx_route_fill_gain(in, out, n, tgt, cur);
#endif
}

static void k_route_fold(const float *l, const float *r, float *out, uint32_t n, float tgt, float *cur) {
#ifdef OMX_GOLDEN_ENGINE_REST
  omx_fold_gain(l, r, out, n, tgt, cur);
#else
  omx_route_fold_gain(l, r, out, n, tgt, cur);
#endif
}

static void k_fold_fill(float *l, float *r, uint32_t n, float tgt, float *cl, float *cr) {
#ifdef OMX_GOLDEN_ENGINE_REST
  omx_fold_fill(l, r, n, tgt, cl, cr);
#else
  omx_route_fold_fill(l, r, n, tgt, cl, cr);
#endif
}

static float k_strip_meter(const float *l, const float *r, uint32_t n, float level) {
#ifdef OMX_GOLDEN_ENGINE_REST
  return omx_strip_meter(l, r, n, level);
#else
  return omx_meter_strip_peak(l, r, n, level);
#endif
}

static int k_settled(float tl, float tr, float cl, float cr) {
#ifdef OMX_GOLDEN_ENGINE_REST
  return omx_strip_silent(tl, tr, cl, cr);
#else
  return omx_mix_settled_silent(tl, tr, cl, cr);
#endif
}

static float k_clev(void) {
#ifdef OMX_GOLDEN_ENGINE_REST
  return OMX_CUE_FOLD_CLEV;
#else
  return OMX_PAN_LCR_FOLD_CLEV;
#endif
}

#endif /* !OMX_GOLDEN_ENGINE */

/* ---- the record ---------------------------------------------------------------------------- */

#define RECORD_BYTES (8u << 20)
static uint8_t g_rec[RECORD_BYTES];
static size_t g_len;
static int g_overflow;

static void put(const void *p, size_t n) {
  if (g_len + n > RECORD_BYTES) { g_overflow = 1; return; }
  memcpy(g_rec + g_len, p, n);
  g_len += n;
}
static void put_f(float v) { put(&v, sizeof v); }
static void put_u32(uint32_t v) { put(&v, sizeof v); }

/* ---- the stimulus -------------------------------------------------------------------------- */

static float lcg_noise(uint32_t *s) {
  *s = *s * 1664525u + 1013904223u;
  return (float)(int32_t)(*s >> 8) / 8388608.0f - 1.0f;
}

/* The i-th of m pan positions, -1.25 to +1.25: the clamp is on the path at both ends. */
static float sweep(uint32_t i, uint32_t m) { return -1.25f + 2.5f * (float)i / (float)(m - 1u); }

#define BLOCK_MAX 1920u
static float g_a[BLOCK_MAX], g_b[BLOCK_MAX];

/* ---- the kernels --------------------------------------------------------------------------- */

static void render(const char *k, uint32_t rate) {
  const uint32_t m = rate / 100u; /* sweep points and block length, 10 ms */
  if (strcmp(k, "pan") == 0) {
    for (uint32_t i = 0; i < m; i++) {
      float l, r;
      k_pan(sweep(i, m), &l, &r);
      put_f(l);
      put_f(r);
    }
  } else if (strcmp(k, "lcr") == 0) {
    static const float divs[] = {0.0f, 0.3f, 0.75f, 1.0f};
    for (uint32_t d = 0; d < 4; d++)
      for (uint32_t i = 0; i < m; i++) {
        float g[3];
        k_lcr(sweep(i, m), divs[d], g);
        put(g, sizeof g);
      }
  } else if (strcmp(k, "strip") == 0) {
    static const float levels[][2] = {{1.0f, 1.0f}, {0.5f, 0.7f}, {2.0f, 3.1623f}, {0.001f, 1.0f}};
    for (int lcr = 0; lcr <= 1; lcr++)
      for (int ch = 1; ch <= 2; ch++)
        for (int act = 0; act <= 1; act++)
          for (int mute = 0; mute <= 1; mute++)
            for (uint32_t v = 0; v < 4; v++)
              for (uint32_t i = 0; i < m; i += 7u) {
                float g[3];
                k_strip(lcr, ch, act, levels[v][0], levels[v][1], mute, sweep(i, m), 0.4f, g);
                put(g, sizeof g);
              }
  } else if (strcmp(k, "fold") == 0) {
    static const float coefs[] = {0.5f, 0.70710678f, 1.0f, 0.0f, NAN, INFINITY};
    for (uint32_t c = 0; c < 6; c++)
      for (uint32_t i = 0; i < m; i++) {
        const float level = (i == 1u) ? NAN : (i == 2u) ? -INFINITY : 4.0f * (float)i / (float)m;
        put_f(k_fold(level, coefs[c]));
      }
  } else if (strcmp(k, "bus_out") == 0) {
    uint32_t s = rate;
    for (int ch = 1; ch <= 2; ch++) {
      float cl = 1.0f, cr = 1.0f;
      for (uint32_t blk = 0; blk < 24u; blk++) {
        for (uint32_t i = 0; i < m; i++) {
          g_a[i] = lcg_noise(&s);
          g_b[i] = 0.5f * lcg_noise(&s);
        }
        const float fader = (blk % 4u == 3u) ? 3.1623f : 1.0f - 0.04f * (float)blk;
        const int mute = blk == 9u || blk == 10u;
        const float bal = -1.0f + (float)blk / 11.5f;
        float pl, pr;
        k_bus_out(g_a, g_b, m, ch, fader, mute, bal, &cl, &cr, &pl, &pr);
        put(g_a, m * sizeof(float));
        put(g_b, m * sizeof(float));
        put_f(cl);
        put_f(cr);
        put_f(pl);
        put_f(pr);
      }
    }
  } else if (strcmp(k, "peak") == 0) {
    uint32_t s = rate ^ 0x5eedu;
    for (uint32_t blk = 0; blk < 16u; blk++) {
      const uint32_t n = blk == 0 ? 0u : m - blk;
      for (uint32_t i = 0; i < n; i++) {
        g_a[i] = lcg_noise(&s) * (float)(blk + 1u) / 16.0f;
        g_b[i] = lcg_noise(&s);
      }
      put_f(k_peak(g_a, NULL, n));
      put_f(k_peak(g_a, g_a, n));
      put_f(k_peak(g_a, g_b, n));
      float pl, pr;
      k_leg_peaks(g_a, g_b, n, 0.3f, 1.7f, &pl, &pr);
      put_f(pl);
      put_f(pr);
    }
  } else if (strcmp(k, "clip") == 0) {
    static const float scales[] = {1.0f, 0.99f, 1.0001f, 2.0f};
    for (uint32_t run = 0; run <= 5u; run++)
      for (uint32_t leg = 0; leg < 2u; leg++)
        for (uint32_t sc = 0; sc < 4u; sc++) {
          for (uint32_t i = 0; i < m; i++) g_a[i] = g_b[i] = 0.5f;
          float *x = leg ? g_b : g_a;
          for (uint32_t i = 0; i < run; i++) x[m / 2u + i] = (i & 1u) ? -1.0f : 1.0f;
          x[m / 4u] = 1.0f; /* a lone over */
          put_u32((uint32_t)k_clip(g_a, g_b, m, scales[sc]));
          put_u32((uint32_t)k_clip(g_a, NULL, m, scales[sc]));
          put_u32((uint32_t)k_clip(g_b, g_b, m, scales[sc]));
        }
  } else if (strcmp(k, "hold") == 0) {
    uint32_t s = rate ^ 0x401du;
    float held_l = 0.0f, held_r = 0.0f;
    for (uint32_t i = 0; i < m; i++) {
      if (i % 50u == 0u) held_l = held_r = 0.0f; /* the reader's clear */
      float pl = 0.5f * (lcg_noise(&s) + 1.0f) * (float)(i % 7u + 1u) / 7.0f;
      float pr = 0.5f * (lcg_noise(&s) + 1.0f);
      if (i % 97u == 3u) pl = NAN;
      if (i % 89u == 5u) pr = held_r;
      if (i % 83u == 0u) pl = -0.0f;
      held_l = k_hold(held_l, pl);
      held_r = k_hold(held_r, pr);
      put_f(held_l);
      put_f(held_r);
    }
  } else if (strcmp(k, "gr") == 0) {
    static const float poles[] = {0.999f, 0.99995f, 0.5f, 0.0f, 1.0f, NAN, -0.25f};
    const uint32_t sizes[] = {64u, m, 32u, 1u, 0u, 256u, rate / 1000u};
    uint32_t s = rate ^ 0x6e0du;
    for (uint32_t p = 0; p < 7u; p++) {
      golden_gr g = {0.0f, 0.0f};
      for (uint32_t b = 0; b < 3000u; b++) {
        const float u = 0.5f * (lcg_noise(&s) + 1.0f);
        float db = 0.0f;
        if ((b / 400u) % 3u == 2u) db = -5.0f;
        else if (u < 1.0f / 48.0f) db = -40.0f * 0.5f * (lcg_noise(&s) + 1.0f);
        put_f(k_gr(&g, db, sizes[b % 7u], (float)rate, poles[p]));
        put_f(g.db);
        put_f(g.hold_left_s);
      }
    }
  } else if (strcmp(k, "rta") == 0) {
    uint32_t s = rate ^ 0x47a0u;
    for (uint32_t i = 0; i < OMX_RTA_LOW_WINDOW; i++) {
      g_leg_l[i] = lcg_noise(&s);
      g_leg_r[i] = 0.25f * lcg_noise(&s) + 0.5f * g_leg_l[i];
    }
    const uint32_t n = omx_rta_fft_size_for(rate);
    for (int low = 0; low <= 1; low++) {
      const float *lm;
      const float *mags = k_rta(n, low, &lm);
      put(mags, (n / 2u) * sizeof(float));
      if (low) put(lm, (OMX_RTA_LOW_FFT_SIZE / 2u) * sizeof(float));
    }
  } else if (strcmp(k, "bus_sum") == 0) {
    static float bl[BLOCK_MAX], br[BLOCK_MAX];
    static const float levels[] = {1.0f, 0.5f, 0.0f, 0.0f, 3.1623f, NAN, 0.25f, INFINITY, 0.0f, 0.7f};
    static const float images[][2] = {{1.0f, 1.0f}, {1.0f, 0.0f}, {0.70710678f, 0.70710678f}, {0.0f, 1.0f}, {-1.0f, 0.5f}};
    static const float folds[] = {0.5f, 0.70710678f, 1.0f, NAN};
    uint32_t s = rate ^ 0xb5u;
    for (int fold = 0; fold <= 1; fold++) {
      float cl = 0.0f, cr = 0.0f;
      for (uint32_t blk = 0; blk < 30u; blk++) {
        const uint32_t n = blk == 13u ? 0u : m - (blk % 5u);
        for (uint32_t i = 0; i < n; i++) {
          g_a[i] = lcg_noise(&s);
          g_b[i] = 0.5f * lcg_noise(&s);
          bl[i] = 0.25f * lcg_noise(&s);
          br[i] = 0.25f * lcg_noise(&s);
        }
        if (blk == 21u) cl = NAN;
        if (blk == 22u) cr = INFINITY;
        const float *inr = (blk % 3u == 0u) ? g_a : g_b;
        const int wrote = k_bus_sum(g_a, inr, bl, fold ? NULL : br, n, levels[blk % 10u],
                                    images[blk % 5u][0], images[blk % 5u][1], &cl, &cr, fold,
                                    folds[blk % 4u]);
        put(bl, n * sizeof(float));
        if (!fold) put(br, n * sizeof(float));
        put_f(cl);
        put_f(cr);
        put_u32((uint32_t)wrote);
      }
    }
  } else if (strcmp(k, "rt_load") == 0) {
    struct golden_load g;
    k_rt_load_clear(&g);
    uint32_t s = rate ^ 0x10adu;
    uint64_t xr = 777000u;
    for (uint32_t c = 0; c < 400u; c++) {
      const uint32_t frames = (c % 37u == 5u) ? 0u : (64u << (c % 4u));
      const float budget = (float)frames / (float)rate * 1e9f;
      const float u = 0.5f * (lcg_noise(&s) + 1.0f);
      const uint64_t busy = (uint64_t)(budget * 1.3f * u);
      if (c % 11u == 4u) xr += 1000u * (c % 7u);
      if (c == 250u) xr = 40u;
      k_rt_load(&g, busy, frames, (c == 77u) ? 0.0f : (float)rate, xr, c % 50u == 0u);
      put_f(g.ema);
      put_f(g.peak);
      put_u32(g.overruns);
      put_u32(g.cycles);
      put_u32(g.xruns);
      put_u32(g.seeded);
      put(&g.xrun_ns, sizeof g.xrun_ns);
    }
  }
}

#ifndef OMX_GOLDEN_ENGINE
/* A run of route blocks: noise, a moving target, an empty block, non-finite targets among them.
 * `what` 0 = fill, 1 = fold, 2 = fold-fill. */
static void render_route(int what, uint32_t rate, uint32_t m) {
  static float bl[BLOCK_MAX], br[BLOCK_MAX], out[BLOCK_MAX];
  static const float tgts[] = {1.0f, 0.5f, 0.5f, -0.5f, 0.0f, 3.1623f, NAN, 0.25f, INFINITY, 0.70710678f, 0.0f, 1.0f};
  uint32_t s = rate ^ (0x7a11u + (uint32_t)what);
  float c1 = 0.0f, c2 = 0.3f;
  for (uint32_t blk = 0; blk < 36u; blk++) {
    const uint32_t n = blk == 17u ? 0u : m - (blk % 7u);
    for (uint32_t i = 0; i < n; i++) {
      bl[i] = lcg_noise(&s);
      br[i] = (blk % 4u == 1u) ? bl[i] : 0.5f * lcg_noise(&s);
    }
    float t = tgts[blk % 12u];
    if (what == 2 && !isfinite(t)) t = 0.125f; /* the in-place fold reads its target as given */
    if (what == 0) {
      if (blk % 3u == 2u) {
        k_route_fill(bl, bl, n, t, &c1);
        put(bl, n * sizeof(float));
      } else {
        k_route_fill(bl, out, n, t, &c1);
        put(out, n * sizeof(float));
      }
      put_f(c1);
    } else if (what == 1) {
      k_route_fold(bl, br, out, n, t, &c1);
      put(out, n * sizeof(float));
      put_f(c1);
    } else {
      k_fold_fill(bl, br, n, t, &c1, &c2);
      put(bl, n * sizeof(float));
      put(br, n * sizeof(float));
      put_f(c1);
      put_f(c2);
    }
  }
}

static void render_rest(const char *k, uint32_t rate) {
  const uint32_t m = rate / 100u;
  if (strcmp(k, "split") == 0) {
    static const float levels[][2] = {{1.0f, 1.0f}, {0.5f, 0.7f}, {2.0f, 3.1623f}, {0.001f, 1.0f}};
    for (int ch = 1; ch <= 2; ch++)
      for (int act = 0; act <= 1; act++)
        for (int mute = 0; mute <= 1; mute++)
          for (uint32_t v = 0; v < 4; v++)
            for (uint32_t i = 0; i < m; i += 7u) {
              float coef, img[2];
              k_split(ch, act, levels[v][0], levels[v][1], mute, sweep(i, m), &coef, img);
              put_f(coef);
              put(img, sizeof img);
            }
  } else if (strcmp(k, "route_fill") == 0) {
    render_route(0, rate, m);
  } else if (strcmp(k, "route_fold") == 0) {
    render_route(1, rate, m);
  } else if (strcmp(k, "fold_fill") == 0) {
    render_route(2, rate, m);
  } else if (strcmp(k, "strip_meter") == 0) {
    static const float levels[] = {0.0f, 0.001f, 0.5f, 0.70794578f, 1.0f, 3.1622777f};
    uint32_t s = rate ^ 0x3e7eu;
    for (uint32_t blk = 0; blk < 12u; blk++) {
      const uint32_t n = blk == 0 ? 0u : m - blk;
      for (uint32_t i = 0; i < n; i++) {
        g_a[i] = lcg_noise(&s) * (float)(blk + 1u) / 12.0f;
        g_b[i] = lcg_noise(&s);
      }
      for (uint32_t v = 0; v < 6u; v++) {
        put_f(k_strip_meter(g_a, NULL, n, levels[v]));
        put_f(k_strip_meter(g_a, g_a, n, levels[v]));
        put_f(k_strip_meter(g_a, g_b, n, levels[v]));
      }
    }
  } else if (strcmp(k, "settled") == 0) {
    const float v[5] = {0.0f, -0.0f, 1e-30f, 0.5f, NAN};
    for (uint32_t a = 0; a < 5u; a++)
      for (uint32_t b = 0; b < 5u; b++)
        for (uint32_t c = 0; c < 5u; c++)
          for (uint32_t d = 0; d < 5u; d++) put_u32((uint32_t)k_settled(v[a], v[b], v[c], v[d]));
  } else if (strcmp(k, "lcr_clev") == 0) {
    put_f(k_clev());
    for (uint32_t i = 0; i < m; i++) {
      const float eff = 4.0f * (float)i / (float)m;
      for (int on = 0; on <= 1; on++) put_f(eff * (on ? 1.0f : 0.0f) * k_clev());
    }
  }
}

#endif /* !OMX_GOLDEN_ENGINE */

/* ---- the bus stages: the same calls in the library build and the bus stages' engine build ---- */
#if !defined(OMX_GOLDEN_ENGINE) && !defined(OMX_GOLDEN_ENGINE_REST)

static void render_strip(const char *k, uint32_t rate) {
  const uint32_t m = rate / 100u;
  static float bl[BLOCK_MAX], br[BLOCK_MAX];
  if (strcmp(k, "mix_strip") == 0) {
    static const float tgts[] = {1.0f, 0.5f, 0.0f, -0.5f, 3.1623f, NAN, 0.25f, INFINITY, 0.0f, 0.70710678f, -INFINITY, 1.0f};
    uint32_t s = rate ^ 0x5717u;
    float cl = 0.0f, cr = 0.0f;
    for (uint32_t blk = 0; blk < 36u; blk++) {
      const uint32_t n = blk == 11u ? 0u : m - (blk % 5u);
      for (uint32_t i = 0; i < n; i++) {
        g_a[i] = lcg_noise(&s);
        g_b[i] = 0.5f * lcg_noise(&s);
        bl[i] = 0.25f * lcg_noise(&s);
        br[i] = 0.25f * lcg_noise(&s);
      }
      if (blk == 19u) cl = NAN;
      if (blk == 20u) cr = -INFINITY;
      const float *inr = (blk % 4u == 1u) ? g_a : g_b; /* a mono source passes its one leg twice */
      omx_mix_strip(g_a, inr, bl, br, n, tgts[blk % 12u], tgts[(blk + 5u) % 12u], &cl, &cr);
      put(bl, n * sizeof(float));
      put(br, n * sizeof(float));
      put_f(cl);
      put_f(cr);
    }
  } else if (strcmp(k, "bus_out_gain") == 0 || strcmp(k, "bus_out_stage") == 0) {
    const int stage = strcmp(k, "bus_out_stage") == 0;
    uint32_t s = rate ^ (stage ? 0x0b57u : 0x0b56u);
    for (int ch = 1; ch <= 2; ch++) {
      float cl = 1.0f, cr = 1.0f;
      _Atomic float pl = 0.0f, pr = 0.0f;
      for (uint32_t blk = 0; blk < 24u; blk++) {
        const uint32_t n = blk == 7u ? 0u : m - (blk % 3u);
        for (uint32_t i = 0; i < n; i++) {
          g_a[i] = lcg_noise(&s) * (float)(blk % 6u + 1u) / 6.0f;
          g_b[i] = 0.5f * lcg_noise(&s);
        }
        const float fader = (blk % 4u == 3u) ? 3.1623f : (blk == 17u) ? 0.0f : 1.0f - 0.04f * (float)blk;
        const int mute = blk == 9u || blk == 10u;
        const float bal = -1.0f + (float)blk / 11.5f;
        if (stage) {
          if (blk % 5u == 0u) { /* the reader's clear */
            atomic_store(&pl, 0.0f);
            atomic_store(&pr, 0.0f);
          }
          omx_bus_out_stage(g_a, g_b, n, ch, fader, mute, bal, &cl, &cr, &pl, &pr);
        } else {
          omx_bus_out_gain(g_a, g_b, n, ch, fader, mute, bal, &cl, &cr);
        }
        put(g_a, n * sizeof(float));
        put(g_b, n * sizeof(float));
        put_f(cl);
        put_f(cr);
        if (stage) {
          put_f(atomic_load(&pl));
          put_f(atomic_load(&pr));
        }
      }
    }
  }
}

#endif /* the bus stages */

/* ---- a slot's key: the same calls in the library build and the key's engine build ------------ */
#if defined(GOLDEN_LIBRARY) || defined(OMX_GOLDEN_ENGINE_KEY)

/* RBJ sections, Butterworth Q, designed at 48 kHz and written out, so the stimulus makes no libm
 * call: high-pass at 200 Hz and 80 Hz, low-pass at 5 kHz and 12 kHz. The key runs them as given
 * at every rate. */
static const float KEY_HP[2][5] = {{0.981658268f, -1.96331654f, 0.981658268f, -1.96298009f, 0.963652984f},
                                   {0.992622543f, -1.98524509f, 0.992622543f, -1.98519066f, 0.985299513f}};
static const float KEY_LP[2][5] = {{0.0722308753f, 0.144461751f, 0.0722308753f, -1.10922879f, 0.398152293f},
                                   {0.292893219f, 0.585786437f, 0.292893219f, -7.17381485e-17f, 0.171572874f}};

static void render_key(const char *k, uint32_t rate) {
  if (strcmp(k, "key") != 0) return;
  const uint32_t m = rate / 100u;
  static float l[BLOCK_MAX], r[BLOCK_MAX], src[BLOCK_MAX], buf[BLOCK_MAX];
  uint32_t s = rate ^ 0x4e7u;
  /* 0 = self on a stereo lane, 1 = self on a mono lane, 2 = another lane's block */
  for (uint32_t mode = 0; mode < 3u; mode++) {
    float hs[4] = {0.0f, 0.0f, 0.0f, 0.0f}, ls[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (uint32_t blk = 0; blk < 24u; blk++) {
      const uint32_t n = blk == 9u ? 0u : m - (blk % 5u);
      for (uint32_t i = 0; i < n; i++) {
        l[i] = lcg_noise(&s);
        r[i] = 0.5f * lcg_noise(&s);
        src[i] = 0.25f * lcg_noise(&s) + 0.5f * l[i];
      }
      for (uint32_t i = 0; i < m; i++) buf[i] = -7.0f; /* what the call leaves unwritten shows */
      struct omx_key key;
      memset(&key, 0, sizeof key);
      key.src = mode == 2u ? src : NULL;
      key.buf = (blk % 11u == 10u) ? NULL : buf; /* a slot with no key */
      key.hp_on = (int)(blk % 4u) & 1;
      key.lp_on = (int)((blk % 4u) >> 1);
      memcpy(key.hp_c, KEY_HP[(blk / 4u) & 1u], sizeof key.hp_c);
      memcpy(key.lp_c, KEY_LP[(blk / 8u) & 1u], sizeof key.lp_c);
      key.to_detector = blk % 3u != 1u; /* a listen-only key every third block */
      const float *got = omx_key_build(blk == 21u ? NULL : &key, l, mode == 1u ? NULL : r, n, hs, ls);
      put_u32(got == NULL ? 0u : got == buf ? 1u : 2u);
      put_u32((uint32_t)key.built);
      put(buf, m * sizeof(float));
      put(hs, sizeof hs);
      put(ls, sizeof ls);
    }
  }
}

#endif /* a slot's key */

/* ---- the source switch: the same call in the library build and the switch's engine build ----- */
#if defined(GOLDEN_LIBRARY) || defined(OMX_GOLDEN_ENGINE_PB)

static void render_pb(const char *k, uint32_t rate) {
  if (strcmp(k, "pb") != 0) return;
  const uint32_t m = rate / 100u;
  static float live[BLOCK_MAX], track[BLOCK_MAX];
  static const float tgts[] = {1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.5f, NAN, 1.0f,
                               INFINITY, 0.0f, 0.25f, 1.0f, 1.0f, 0.0f, -INFINITY, 1.0f};
  uint32_t s = rate ^ 0x9b1u;
  float cur = 0.0f;
  for (uint32_t blk = 0; blk < 40u; blk++) {
    const uint32_t n = blk == 13u ? 0u : m - (blk % 6u);
    for (uint32_t i = 0; i < n; i++) {
      live[i] = lcg_noise(&s);
      track[i] = 0.5f * lcg_noise(&s);
    }
    if (blk == 27u) cur = NAN; /* a poisoned cursor */
    omx_src_blend((blk % 5u == 3u) ? NULL : live, track, n, tgts[blk % 16u], &cur);
    put(track, n * sizeof(float));
    put_f(cur);
  }
}

#endif /* the source switch */

/* ---- the level meter's reading: the library build only ------------------------------------- */
#ifdef GOLDEN_LIBRARY

static void render_level(const char *k, uint32_t rate) {
  if (strcmp(k, "level") != 0) return;
  const uint32_t m = rate / 100u;
  static const float scales[] = {1.0f, 0.5f, 0.001f, 1e-4f, 3.1623f, 0.0f, 1e-6f, 0.70794578f};
  static const double mins[] = {-60.0, -90.0, -144.0, 6.0, 0.0};
  uint32_t s = rate ^ 0x1e7du;
  for (uint32_t blk = 0; blk < 32u; blk++) {
    const uint32_t n = blk == 4u ? 0u : m - (blk % 7u);
    for (uint32_t i = 0; i < n; i++) g_a[i] = lcg_noise(&s) * scales[blk % 8u];
    if (n > 0u && blk == 17u) g_a[n / 2u] = NAN;
    if (n > 0u && blk == 23u) g_a[n / 3u] = INFINITY;
    if (n > 0u && blk == 29u) g_a[n / 4u] = -INFINITY;
    for (uint32_t v = 0; v < 5u; v++) {
      const double db = omx_meter_level_db(g_a, n, mins[v]);
      put(&db, sizeof db);
    }
  }
}

#endif /* the level meter's reading */

static int digest(const char *k, uint32_t rate, char hex[65]) {
  g_len = 0;
  g_overflow = 0;
  render(k, rate);
#ifndef OMX_GOLDEN_ENGINE
  render_rest(k, rate);
#endif
#if !defined(OMX_GOLDEN_ENGINE) && !defined(OMX_GOLDEN_ENGINE_REST)
  render_strip(k, rate);
#endif
#if defined(GOLDEN_LIBRARY) || defined(OMX_GOLDEN_ENGINE_KEY)
  render_key(k, rate);
#endif
#if defined(GOLDEN_LIBRARY) || defined(OMX_GOLDEN_ENGINE_PB)
  render_pb(k, rate);
#endif
#ifdef GOLDEN_LIBRARY
  render_level(k, rate);
#endif
  if (g_overflow) {
    fprintf(stderr, "mix_golden: the %s record at %u Hz overflows\n", k, rate);
    return 1;
  }
  omx_sha256_hex(g_rec, g_len, hex);
  return 0;
}

int main(int argc, char **argv) {
  char probe[65];
  omx_sha256_hex("abc", 3, probe);
  if (strcmp(probe, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") != 0) {
    fprintf(stderr, "mix_golden: the SHA-256 self-test failed (%s)\n", probe);
    return 2;
  }
  const int write = argc > 1 && strcmp(argv[1], "--write") == 0;
  const char *path = argc > 1 && !write ? argv[1] : "test/golden/mix.sha256";
  FILE *f = write ? NULL : fopen(path, "r");
  if (!write && !f) { fprintf(stderr, "mix_golden: cannot read %s\n", path); return 2; }
  /* An engine build compares only the kernels its engine copies render. */
#if defined(OMX_GOLDEN_ENGINE)
  static const char *const kernels[] = {"pan", "lcr", "strip", "fold", "bus_out", "peak", "clip", "hold", "gr", "rta", "bus_sum", "rt_load"};
#elif defined(OMX_GOLDEN_ENGINE_REST)
  static const char *const kernels[] = {"split", "route_fill", "route_fold", "fold_fill", "strip_meter", "settled", "lcr_clev"};
#elif defined(OMX_GOLDEN_ENGINE_STRIP)
  static const char *const kernels[] = {"mix_strip", "bus_out_gain", "bus_out_stage"};
#elif defined(OMX_GOLDEN_ENGINE_KEY)
  static const char *const kernels[] = {"key"};
#elif defined(OMX_GOLDEN_ENGINE_PB)
  static const char *const kernels[] = {"pb"};
#else
  static const char *const kernels[] = {"pan", "lcr", "strip", "fold", "bus_out", "peak", "clip", "hold", "gr", "rta", "bus_sum", "rt_load",
                                        "split", "route_fill", "route_fold", "fold_fill", "strip_meter", "settled", "lcr_clev",
                                        "mix_strip", "bus_out_gain", "bus_out_stage", "key", "pb", "level"};
#endif
  const unsigned nk = sizeof kernels / sizeof kernels[0];
  int fail = 0, checked = 0;
  for (unsigned ki = 0; ki < nk; ki++)
    for (uint32_t r = 0; r < OMX_ORACLE_FULL_RATES_COUNT; r++) {
      const uint32_t rate = (uint32_t)GOLDEN_RATES[r];
      char hex[65];
      if (digest(kernels[ki], rate, hex) != 0) return 2;
      if (write) { printf("%s %u %s\n", kernels[ki], rate, hex); continue; }
      char name[16], want[65] = {0};
      unsigned got_rate = 0;
      int found = 0;
      rewind(f);
      while (fscanf(f, "%15s %u %64s", name, &got_rate, want) == 3)
        if (strcmp(name, kernels[ki]) == 0 && got_rate == rate) { found = 1; break; }
      checked++;
      if (!found) { fprintf(stderr, "FAIL: %s %u Hz has no golden digest\n", kernels[ki], rate); fail++; }
      else if (strcmp(want, hex) != 0) {
        fprintf(stderr, "FAIL: %s %u Hz output moved: %s, golden %s\n", kernels[ki], rate, hex, want);
        fail++;
      }
    }
  if (f) fclose(f);
  if (!write) printf("mix_golden: %d digests (%u kernels at %u rates), %d moved\n", checked, nk,
                     (unsigned)OMX_ORACLE_FULL_RATES_COUNT, fail);
  return fail == 0 ? 0 : 1;
}
