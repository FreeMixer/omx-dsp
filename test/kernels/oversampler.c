// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include "../fx/fx_rates.h"

/* ---- the oversampler ------------------------------------------------------------------------ */

/* ---- the output-parallel passes are the scalar passes ---------------------------------------
 * omx_halfband_decimate computes sixteen outputs at once and the interpolating pass does the same;
 * neither may change a bit, each output's taps being summed in the dot's own order. Held two ways:
 * (1) the run against omx_halfband_dot per output, every length 0..80 at four alignments of the
 * window; (2) the whole element, x2 and x4, up then down, streamed in blocks of 1..67 samples,
 * against the scalar passes re-spelled here verbatim -- memcmp, never a tolerance. Both at every
 * declared rate: the signal is band-limited differently at each, so a rate is a different input. */
struct ovs_ref { uint32_t stages; float up[2][OMX_OVS_UP_HIST]; float down[2][OMX_OVS_DOWN_HIST]; };

static void ovs_ref_up2(float *hist, const float *in, uint32_t n, float *out) {
  for (uint32_t i = 0; i < n; i++) {
    float win[OMX_OVS_UP_HIST + 1u];
    memcpy(win, hist, OMX_OVS_UP_HIST * sizeof *win);
    win[OMX_OVS_UP_HIST] = in[i];
    const float *c = win + OMX_OVS_UP_HIST - OMX_OVS_UP_DELAY;
    out[2u * i] = 2.0f * OMX_HALFBAND_CENTER * c[0];
    float acc = 0.0f;
    for (uint32_t j = 0; j < OMX_HALFBAND_ODD_TAPS; j++) acc += OMX_HALFBAND_ODD_COEF[j] * (c[-(int)j] + c[j + 1u]);
    out[2u * i + 1u] = 2.0f * acc;
    memcpy(hist, win + 1, OMX_OVS_UP_HIST * sizeof *hist);
  }
}

static void ovs_ref_down2(float *hist, const float *in, uint32_t n_out, float *out) {
  for (uint32_t o = 0; o < n_out; o++) {
    float win[OMX_OVS_DOWN_HIST + 2u];
    memcpy(win, hist, OMX_OVS_DOWN_HIST * sizeof *win);
    win[OMX_OVS_DOWN_HIST] = in[2u * o];
    win[OMX_OVS_DOWN_HIST + 1u] = in[2u * o + 1u];
    const float *c = win + OMX_OVS_DOWN_HIST - OMX_OVS_DOWN_DELAY;
    float acc = OMX_HALFBAND_CENTER * c[0];
    for (uint32_t i = 0; i < OMX_HALFBAND_ODD_TAPS; i++) {
      const int32_t k = (int32_t)(2u * i + 1u);
      acc += OMX_HALFBAND_ODD_COEF[i] * (c[-k] + c[k]);
    }
    out[o] = acc;
    memcpy(hist, win + 2, OMX_OVS_DOWN_HIST * sizeof *hist);
  }
}

static void ovs_ref_updown(struct ovs_ref *r, const float *in, uint32_t n, float *up, float *out) {
  if (r->stages == 1u) { ovs_ref_up2(r->up[0], in, n, up); ovs_ref_down2(r->down[0], up, n, out); return; }
  float mid[2u * 80u];
  ovs_ref_up2(r->up[0], in, n, mid);
  ovs_ref_up2(r->up[1], mid, 2u * n, up);
  ovs_ref_down2(r->down[0], up, 2u * n, mid);
  ovs_ref_down2(r->down[1], mid, n, out);
}

/* A broadband, deterministic test signal at sample rate sr: two tones plus a hashed dither. */
static void ovs_gen(float *x, uint32_t n, double sr) {
  for (uint32_t i = 0; i < n; i++)
    x[i] = 0.4f * sinf((float)(2.0 * M_PI * 997.0 * i / sr)) + 0.3f * sinf((float)(2.0 * M_PI * 0.31 * sr / 2.0 * i / sr)) +
           (float)((i * 2654435761u) >> 20) * 1e-4f;
}

static void arm_oversampler_parallel(float sr) {
  static float buf[4 + 2u * 80u + 4u * OMX_HALFBAND_ODD_TAPS + 8u];
  ovs_gen(buf, sizeof buf / sizeof buf[0], sr);
  int run_equal = 1;
  for (uint32_t align = 0; align < 4u; align++)
    for (uint32_t n = 0; n <= 80u; n++) {
      const float *c = buf + align + 2u * OMX_HALFBAND_ODD_TAPS - 1u;
      float got[81], want[81];
      memset(got, 0xA5, sizeof got);
      memcpy(want, got, sizeof want);
      omx_halfband_decimate(c, n, got);
      for (uint32_t o = 0; o < n; o++) want[o] = omx_halfband_dot(c + 2u * o);
      if (memcmp(got, want, sizeof got) != 0) run_equal = 0;
    }
  ok(run_equal, "omx_halfband_decimate is bit-identical to omx_halfband_dot at every length 0..80 and 4 alignments", 0.0, 0.0);

  for (uint32_t factor = 2u; factor <= 4u; factor += 2u) {
    struct omx_oversampler o;
    omx_oversampler_init(&o, factor);
    struct ovs_ref r;
    memset(&r, 0, sizeof r);
    r.stages = factor == 4u ? 2u : 1u;
    static float x[4096];
    ovs_gen(x, 4096u, sr);
    int equal = 1;
    uint32_t done = 0u, blk = 1u;
    while (done + 80u <= 4096u) {
      float up_a[4u * 80u], up_b[4u * 80u], out_a[80], out_b[80];
      omx_oversampler_up(&o, x + done, blk, up_a);
      omx_oversampler_down(&o, up_a, blk, out_a);
      ovs_ref_updown(&r, x + done, blk, up_b, out_b);
      if (memcmp(up_a, up_b, factor * blk * sizeof(float)) != 0 || memcmp(out_a, out_b, blk * sizeof(float)) != 0) equal = 0;
      done += blk;
      blk = blk % 67u + 1u;
    }
    ok(equal, "up AND down are bit-identical to the scalar passes, streamed in blocks of 1..67", (double)factor, sr);
  }
}

/* ---- the element's four promises, at the nine oracle rates --------------------------------------
 * Moved from the engine's omx_oversampler.test.c. The four things this element
 * has to get right: the round trip returns the input delayed by EXACTLY the declared latency
 * (measured, not read back); its residual is far below anything a caller hears; the quantum is
 * not observable, bit for bit; and interpolation leaves no zero-stuffing image. The engine ran
 * these at 96 kHz only -- the half-band is normalised, but the LATENCY is in samples, so the
 * figure being checked does not move with the rate. Here the latency and quantum arms run at the
 * nine oracle rates anyway (the signal is band-limited differently at each, so a rate is a different
 * input); the residual and image arms keep the engine's tones in Hz and run at every oracle rate whose
 * half-band passband holds the top tone (ovs_eng_in_band) -- eight of the nine, 96 kHz among them. */
#define OVS_EN 4096u

/* content across the band the console delivers (997, 5003, 15013 Hz), silence 256 samples either
 * side so the delay is visible as a delay and not merely as a phase */
static void ovs_eng_gen(float *x, uint32_t n, double sr) {
  for (uint32_t i = 0; i < n; i++) {
    const double t = (double)i / sr;
    x[i] = (i < 256u || i > n - 256u) ? 0.0f
         : (float)(0.30 * sin(2.0 * M_PI * 997.0 * t) + 0.20 * sin(2.0 * M_PI * 5003.0 * t) + 0.15 * sin(2.0 * M_PI * 15013.0 * t));
  }
}

/* the whole buffer through up then down at block size `blk` */
static void ovs_eng_round_trip(uint32_t factor, const float *in, uint32_t n, float *out, uint32_t blk) {
  static struct omx_oversampler o;
  static float up[4u * 1024u];
  omx_oversampler_init(&o, factor);
  for (uint32_t i = 0; i < n; i += blk) {
    const uint32_t take = (n - i < blk) ? (n - i) : blk;
    omx_oversampler_up(&o, in + i, take, up);
    omx_oversampler_down(&o, up, take, out + i);
  }
}

/* the top tone, 15013 Hz, inside the passband (0.2083 of the rate the first stage runs at, 2 sr) */
static int ovs_eng_in_band(float sr) { return 15013.0 <= 0.2083 * 2.0 * (double)sr; }

static void arm_oversampler_promises(float sr) {
  static float in[OVS_EN], out[OVS_EN], got[OVS_EN];
  ovs_eng_gen(in, OVS_EN, sr);
  /* 1. ROUND TRIP: the shift in 0..160 that best aligns the round trip with its input IS the
   * declared latency, factors 1, 2, 4. The constant is a claim; this argmin is the evidence (a
   * single tone would also align one period off). */
  for (uint32_t f = 1u; f <= 4u; f *= 2u) {
    ovs_eng_round_trip(f, in, OVS_EN, out, 256u);
    double best = 1e30;
    uint32_t best_s = 0u;
    for (uint32_t s = 0; s <= 160u; s++) {
      double num = 0.0;
      for (uint32_t i = 1024u; i < OVS_EN - 1024u; i++) { const double e = (double)out[i] - (double)in[i - s]; num += e * e; }
      if (num < best) { best = num; best_s = s; }
    }
    ok(best_s == omx_oversampler_latency_for(f), "the MEASURED round-trip alignment is the DECLARED latency", best_s, omx_oversampler_latency_for(f));
  }
  /* 3. THE SEAM: quanta 1, 3, 64, 128, 129, 256, 1024 are BIT-IDENTICAL to quantum 256, factors
   * 1, 2, 4; 129 and 1024 straddle the implementation's 128-sample chunk on purpose */
  static const uint32_t blks[7] = {1u, 3u, 64u, 128u, 129u, 256u, 1024u};
  for (uint32_t f = 1u; f <= 4u; f *= 2u) {
    ovs_eng_round_trip(f, in, OVS_EN, out, 256u);
    int same = 1;
    for (int b = 0; b < 7; b++) {
      ovs_eng_round_trip(f, in, OVS_EN, got, blks[b]);
      if (memcmp(out, got, sizeof out) != 0) same = 0;
    }
    ok(same, "quanta 1, 3, 64, 128, 129, 256, 1024 are bit-identical to quantum 256", f, sr);
  }
  /* 2. TRANSPARENCY: residual of the x2 and x4 round trip, re the delayed input, < -80 dB, over
   * the interior. The closed form (out = in delayed) holds only for content in the half-band's
   * passband: at 32 kHz 15013 Hz is 0.235 of the first stage's 64 kHz, past its 0.2083 edge (it
   * measures -28 dB there, the filter doing its job), so 32 kHz is held to the alignment and the
   * seam above, not to -80 dB. */
  if (ovs_eng_in_band(sr))
    for (uint32_t f = 2u; f <= 4u; f *= 2u) {
      ovs_eng_round_trip(f, in, OVS_EN, out, 256u);
      const uint32_t d = omx_oversampler_latency_for(f);
      double num = 0.0, den = 0.0;
      for (uint32_t i = 1024u; i < OVS_EN - 1024u; i++) {
        const double e = (double)out[i] - (double)in[i - d];
        num += e * e;
        den += (double)in[i - d] * (double)in[i - d];
      }
      const double db = 10.0 * log10(num / den);
      ok(db < -80.0, "the round trip's residual is below -80 dB re the signal", db, -80.0);
    }
  /* 4. IMAGE REJECTION: a 15013 Hz tone up-sampled x4 is THAT tone at 4x the rate and nothing
   * else -- measured against the analytic tone, delayed by the up path's own latency in 4x samples
   * (T at base x4 + T at 2x x2), residual < -80 dB. Same passband condition as above. */
  if (ovs_eng_in_band(sr)) {
    static struct omx_oversampler o;
    static float x[OVS_EN], up[4u * OVS_EN];
    omx_oversampler_init(&o, 4u);
    for (uint32_t i = 0; i < OVS_EN; i++) x[i] = (float)(0.5 * sin(2.0 * M_PI * 15013.0 * (double)i / sr));
    omx_oversampler_up(&o, x, OVS_EN, up);
    const uint32_t d4 = 4u * OMX_OVS_UP_DELAY + 2u * OMX_OVS_UP_DELAY;
    double num = 0.0, den = 0.0;
    for (uint32_t k = 2048u; k < 4u * OVS_EN - 2048u; k++) {
      const double want = 0.5 * sin(2.0 * M_PI * 15013.0 * ((double)k - (double)d4) / (4.0 * sr));
      num += ((double)up[k] - want) * ((double)up[k] - want);
      den += want * want;
    }
    const double db = 10.0 * log10(num / den);
    ok(db < -80.0, "x4 interpolation of 15013 Hz leaves no image above -80 dB re the analytic tone", db, -80.0);
  }
}

/* ---- the factor law, and the dot is the two kernels it replaced -------------------------------
 * Moved from openmixer's omx_oversampler.test.c. Factor 1 is a wire each way, on its own; an odd
 * or oversized factor is refused DOWNWARD (3 -> 2, 9 -> 4), never approximated. The shared dot is
 * held BIT-equal to both kernels it replaced (the oversampler's former decimating stage, uint32_t
 * k and c[-(int)k]; the RTA decimator's former interior, int32_t k), re-spelled verbatim, on
 * full-scale noise plus tones -- every tap sees a different magnitude, so a transposed coefficient
 * shows. On a window mirrored about its centre the dot is centre*x0 + 2*sum(odd_i * x[2i+1]), the
 * half-band's symmetry, computed in double and held to 1e-6. */
static void arm_oversampler_factor_and_dot(void) {
  struct omx_oversampler o;
  omx_oversampler_init(&o, 1u);
  float in[64], out[64];
  for (int i = 0; i < 64; i++) in[i] = (float)i * 0.01f;
  omx_oversampler_up(&o, in, 64u, out);
  ok(memcmp(in, out, sizeof in) == 0, "factor 1 up is a copy, bit for bit", 0.0, 0.0);
  memset(out, 0, sizeof out);
  omx_oversampler_down(&o, in, 64u, out);
  ok(memcmp(in, out, sizeof in) == 0, "factor 1 down is a copy, bit for bit", 0.0, 0.0);
  ok(omx_oversampler_latency(&o) == 0u, "factor 1 costs no latency", omx_oversampler_latency(&o), 0.0);
  omx_oversampler_init(&o, 3u);
  ok(o.factor == 2u, "factor 3 is taken as 2 -- never an unmeasured rate change", o.factor, 2.0);
  omx_oversampler_init(&o, 9u);
  ok(o.factor == 4u, "factor 9 is capped at 4", o.factor, 4.0);

  const uint32_t reach = 2u * OMX_HALFBAND_ODD_TAPS; /* the kernel reads 2T-1 either side */
  static float buf[4096];
  for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    uint32_t seed = 0x5eed1234u;
    for (uint32_t i = 0; i < 4096u; i++) {
      seed = seed * 1664525u + 1013904223u;
      const double t = (double)i / (double)OMX_FX_ORACLE_RATES[ri];
      buf[i] = (float)(((double)(seed >> 8) / 8388608.0 - 1.0) * 0.5 + 0.4 * sin(2.0 * M_PI * 997.0 * t) + 0.1 * sin(2.0 * M_PI * 31013.0 * t));
    }
    int eq_ovs = 1, eq_rta = 1;
    for (uint32_t c = reach; c + reach < 4096u; c++) {
      const float *cc = buf + c;
      const float shared = omx_halfband_dot(cc);
      float was_ovs = OMX_HALFBAND_CENTER * cc[0], was_rta = OMX_HALFBAND_CENTER * cc[0];
      for (uint32_t j = 0; j < OMX_HALFBAND_ODD_TAPS; j++) {
        const uint32_t k = 2u * j + 1u;
        was_ovs += OMX_HALFBAND_ODD_COEF[j] * (cc[-(int)k] + cc[k]);
      }
      for (uint32_t i = 0; i < OMX_HALFBAND_ODD_TAPS; i++) {
        const int32_t k = (int32_t)(2 * i + 1);
        was_rta += OMX_HALFBAND_ODD_COEF[i] * (cc[-k] + cc[k]);
      }
      if (memcmp(&shared, &was_ovs, sizeof shared) != 0) eq_ovs = 0;
      if (memcmp(&shared, &was_rta, sizeof shared) != 0) eq_rta = 0;
    }
    ok(eq_ovs, "omx_halfband_dot is BIT-identical to the oversampler's former dot", 0.0, OMX_FX_ORACLE_RATES[ri]);
    ok(eq_rta, "omx_halfband_dot is BIT-identical to the RTA decimator's former dot", 0.0, OMX_FX_ORACLE_RATES[ri]);
  }
  static float sym[4096];
  const uint32_t mid = 2048u;
  for (uint32_t i = 0; i < 4096u; i++) {
    const uint32_t d = (i > mid) ? (i - mid) : (mid - i);
    sym[i] = (float)(1.0 / (1.0 + 0.03 * (double)d)); /* even in d: sym[mid-k] == sym[mid+k] */
  }
  double want = (double)OMX_HALFBAND_CENTER * (double)sym[mid];
  for (uint32_t i = 0; i < OMX_HALFBAND_ODD_TAPS; i++) want += 2.0 * (double)OMX_HALFBAND_ODD_COEF[i] * (double)sym[mid + 2u * i + 1u];
  const double got = omx_halfband_dot(sym + mid);
  ok(fabs(got - want) <= 1e-6, "on a mirrored window the dot is centre + 2*one-sided sum", got, want);
}

/* ---- the shipped table's response, from its QUANTISED float32 taps ---------------------------
 * Moved from the engine's halfband-kernel probe, its positive control and its SHIPPED row
 * (the oversampler filter quality notes; the oversampling vs 192k notes §10g).
 * |H(f)| = |c + 2 * sum(odd_j * cos(2 pi f (2j+1)))|, f in cycles/sample of the rate the stage
 * runs at -- a half-band's edges are ratios, never Hz, so this arm has no rate loop. The probe's
 * Kaiser candidates are prototypes, not omx-dsp code, and are not moved.
 *   passband: worst |H| deviation on [0, 0.2083], 20001 points, = 0.00149 dB +- 5e-5
 *   stopband: worst |H| on [0.2917, 0.5], 40001 points, = -75.29 dB +- 0.05
 *   a x4 round trip at 20 kHz (up 2b, up 4b, down 4b, down 2b: 2|H(20k/2b)| + 2|H(20k/4b)| in dB)
 *   is -0.089 dB on a 44.1 kHz graph and +0.003 dB on a 48 kHz one, +- 0.0005 (the note's 3 places) */
static double ovs_mag_db(double f) {
  double acc = (double)OMX_HALFBAND_CENTER;
  for (int j = 0; j < OMX_HALFBAND_ODD_TAPS; j++) acc += 2.0 * (double)OMX_HALFBAND_ODD_COEF[j] * cos(2.0 * M_PI * f * (double)(2 * j + 1));
  return 20.0 * log10(fabs(acc) < 1e-30 ? 1e-30 : fabs(acc));
}

static void arm_oversampler_table(void) {
  double rip = 0.0, stop = -999.0;
  for (int i = 0; i <= 20000; i++) { const double d = fabs(ovs_mag_db(0.2083 * (double)i / 20000.0)); if (d > rip) rip = d; }
  for (int i = 0; i <= 40000; i++) { const double d = ovs_mag_db(0.2917 + (0.5 - 0.2917) * (double)i / 40000.0); if (d > stop) stop = d; }
  ok(fabs(rip - 0.00149) < 5e-5, "the shipped table's passband ripple to 0.2083 is the note's 0.00149 dB", rip, 0.00149);
  ok(fabs(stop + 75.29) < 0.05, "the shipped table's stopband from 0.2917 is the note's -75.29 dB", stop, -75.29);
  static const double base[2] = {44100.0, 48000.0}, note[2] = {-0.089, 0.003};
  for (int k = 0; k < 2; k++) {
    const double rt = 2.0 * ovs_mag_db(20000.0 / (2.0 * base[k])) + 2.0 * ovs_mag_db(20000.0 / (4.0 * base[k]));
    ok(fabs(rt - note[k]) <= 0.0005, "a x4 round trip at 20 kHz is the note's figure", rt, note[k]);
  }
}

static void arm_oversampler(void) {
  g_arm = "oversampler";
  /* the table: the centre and the odd taps sum to unity at DC; the dot is that sum on a DC window */
  double dc = OMX_HALFBAND_CENTER;
  for (int i = 0; i < OMX_HALFBAND_ODD_TAPS; i++) dc += 2.0 * OMX_HALFBAND_ODD_COEF[i];
  ok(fabs(dc - 1.0) < 1e-6, "the half-band is unity at DC", dc, 1.0);
  float win[4 * OMX_HALFBAND_ODD_TAPS - 1];
  for (int i = 0; i < 4 * OMX_HALFBAND_ODD_TAPS - 1; i++) win[i] = 0.5f;
  ok(fabsf(omx_halfband_dot(win + 2 * OMX_HALFBAND_ODD_TAPS - 1) - 0.5f) < 1e-6f, "the dot over a DC window is the level", omx_halfband_dot(win + 2 * OMX_HALFBAND_ODD_TAPS - 1), 0.5);
  ok(omx_oversampler_latency_for(1u) == 0u && omx_oversampler_latency_for(2u) == 48u && omx_oversampler_latency_for(4u) == 72u, "the declared latencies", omx_oversampler_latency_for(4u), 72.0);
  ok(omx_oversampler_latency_for(4u) == OMX_OVS_LATENCY_4X, "the macro and the function agree", OMX_OVS_LATENCY_4X, 72.0);
  ok(omx_oversampler_state_size() == sizeof(struct omx_oversampler), "the state layout is exported", (double)omx_oversampler_state_size(), sizeof(struct omx_oversampler));
  for (uint32_t factor = 1u; factor <= 4u; factor *= 2u) {
    struct omx_oversampler o;
    omx_oversampler_init(&o, factor);
    ok(o.factor == factor && omx_oversampler_latency(&o) == omx_oversampler_latency_for(factor), "init arms the factor", o.factor, factor);
    for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) {
      const float sr = OMX_DECLARED_RATES[ri];
      /* round trip of a tone: the output is the input delayed by the declared latency, within
       * the half-band's passband ripple; measured over a second of audio in uneven blocks */
      const uint32_t total = 4096u, lat = omx_oversampler_latency_for(factor);
      static float x[4096], up[4096 * 4], back[4096];
      const double w = 2.0 * M_PI * 1000.0 / sr;
      for (uint32_t i = 0; i < total; i++) x[i] = 0.5f * sinf((float)(w * i));
      omx_oversampler_init(&o, factor);
      uint32_t done = 0u;
      const uint32_t blocks[] = {64u, 37u, 128u, 1u, 256u, 100u};
      uint32_t bi = 0u;
      while (done < total) {
        uint32_t n = blocks[bi++ % 6u];
        if (n > total - done) n = total - done;
        omx_oversampler_up(&o, x + done, n, up);
        omx_oversampler_down(&o, up, n, back + done);
        done += n;
      }
      double worst = 0.0;
      for (uint32_t i = lat + 512u; i < total; i++) { const double e = fabs((double)back[i] - (double)x[i - lat]); if (e > worst) worst = e; }
      ok(worst < 0.5 * 0.0025, "up then down is the input delayed by the declared latency within the ripple", worst, 0.00125);
      /* quantum invariance: the same signal in one block equals the uneven blocks bit for bit */
      static float once[4096];
      omx_oversampler_init(&o, factor);
      omx_oversampler_up(&o, x, total, up);
      omx_oversampler_down(&o, up, total, once);
      ok(memcmp(once, back, sizeof once) == 0, "the block size does not touch a bit of the output", 0.0, 0.0);
      if (factor == 1u) ok(memcmp(once, x, sizeof once) == 0, "factor 1 is a copy", 0.0, 0.0);
      /* the interpolated tone is the tone: its level at the higher rate is the level in */
      if (factor > 1u) {
        omx_oversampler_init(&o, factor);
        omx_oversampler_up(&o, x, total, up);
        float peak = 0.0f;
        for (uint32_t i = 1024u; i < total * factor; i++) if (fabsf(up[i]) > peak) peak = fabsf(up[i]);
        ok(fabsf(peak - 0.5f) < 0.5f * 0.003f, "the interpolated tone keeps its level", peak, 0.5);
      }
    }
  }
  for (uint32_t ri = 0; ri < OMX_DECLARED_RATE_COUNT; ri++) arm_oversampler_parallel(OMX_DECLARED_RATES[ri]);
  for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) arm_oversampler_promises(OMX_FX_ORACLE_RATES[ri]);
  arm_oversampler_factor_and_dot();
  arm_oversampler_table();
  expect_clean();
}
