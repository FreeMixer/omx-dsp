// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <float.h>

#include "../fx/fdelay_oracle.h"
#include "../fx/fx_rates.h"

/* ---- the fractional delay line -------------------------------------------------------------- */

static void fdelay_moved_arms(void);
static void fd_arm_tick_pair(void);

static void arm_fdelay(void) {
  g_arm = "fdelay";
  static float ring[4096];
  for (int order = 3; order <= 5; order += 2) {
    /* the kernel is unity at DC at every fraction, and the identity at a zero fraction */
    for (int fi = 0; fi < 50; fi++) {
      float c[OMX_FDELAY_MAX_TAPS];
      omx_fdelay_lagrange(order, (float)fi / 50.0f, c);
      double sum = 0.0;
      for (int k = 0; k <= order; k++) sum += c[k];
      ok(fabs(sum - 1.0) < 1e-5, "the kernel sums to one", sum, 1.0);
      if (fi == 0) ok(c[omx_fdelay_lookbehind(order)] == 1.0f, "a zero fraction is the identity tap", c[omx_fdelay_lookbehind(order)], 1.0);
    }
    /* The kernel IS its documented product, bit for bit, whatever instantiation computes it (the
     * order-3 instantiation a constant order lets the compiler unroll is the chorus lane's cost
     * change, and it must not move one bit): c[k] = (float)(Π(j≠k)(f − (j − off)) / Π(j≠k)(k − j))
     * in double, left to right, over 2^20 grid fractions and 2^16 scattered ones. */
    {
      const int off = (int)omx_fdelay_lookbehind(order);
      long diff = 0, n = 0;
      uint32_t seed = 12345u;
      for (uint32_t i = 0; i < (1u << 20) + (1u << 16); i++) {
        float f;
        if (i < (1u << 20)) f = (float)i / (float)(1u << 20);
        else { seed = seed * 1664525u + 1013904223u; f = (float)(seed >> 8) / 16777216.0f; }
        float c[OMX_FDELAY_MAX_TAPS], want[OMX_FDELAY_MAX_TAPS];
        omx_fdelay_lagrange(order, f, c);
        for (int k = 0; k <= order; k++) {
          double num = 1.0, den = 1.0;
          for (int j = 0; j <= order; j++) {
            if (j == k) continue;
            num *= (double)f - (double)(j - off);
            den *= (double)(k - j);
          }
          want[k] = (float)(num / den);
        }
        n++;
        if (memcmp(c, want, (size_t)(order + 1) * sizeof(float)) != 0) diff++;
      }
      ok(diff == 0, "the kernel is its documented product, bit for bit", (double)diff, 0.0);
      printf("omxdsp_suite fdelay: order %d, %ld/%ld fractions bit-identical to the product\n", order, n - diff, n);
    }
    struct omx_fdelay l;
    ok(omx_fdelay_init(&l, ring, 4096u, order) == OMX_FDELAY_OK, "a legal order arms", order, 0.0);
    ok(l.order == order && l.cap == 4096u, "an armed line carries what it was given", l.order, order);
    /* a whole-sample delay is an exact copy, for any input, at each of the nine oracle rates (the
     * strip alignment line rides this primitive at every rate the interface clocks) */
    for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
      const float sr = OMX_FX_ORACLE_RATES[ri];
      memset(ring, 0, sizeof ring);
      l.wpos = 0u;
      const uint32_t n = 256u, d = (uint32_t)(sr * 0.001f); /* 1 ms */
      float x[512], y[512];
      for (uint32_t i = 0; i < 512u; i++) { x[i] = rnd(-1.0f, 1.0f) * (i % 7 == 0 ? 1e-25f : 1.0f); y[i] = x[i]; }
      omx_fdelay_process(y, 512u, &l, (float)d);
      int exact = 1;
      for (uint32_t i = d; i < 512u; i++) if (y[i] != x[i - d]) exact = 0;
      ok(exact, "a whole-sample delay is bit-exact, below the flush floor too", d, 0.0);
      (void)n;
      /* a fractional delay of a tone reads the tone at the delayed phase within the kernel's loss */
      memset(ring, 0, sizeof ring); l.wpos = 0u;
      const float delay = 10.5f;
      const double w = 2.0 * M_PI * 1000.0 / sr;
      double worst = 0.0;
      for (uint32_t i = 0; i < 2000u; i++) {
        const float out = omx_fdelay_tick(&l, sinf((float)(w * i)), delay);
        if (i > 100u) { const double want = sin(w * ((double)i - delay)); if (fabs(out - want) > worst) worst = fabs(out - want); }
      }
      ok(worst < 2e-3, "a fractional read of a 1 kHz tone lands on the delayed phase", worst, 2e-3);
      /* latency: the clamp's answer is what the line delivers */
      ok(omx_fdelay_latency(&l, -3.0f) == 0.0f, "a negative request delivers 0", omx_fdelay_latency(&l, -3.0f), 0.0);
      ok(omx_fdelay_latency(&l, 1e9f) == omx_fdelay_max_delay(4096u, order), "a huge request is clamped to the ring", omx_fdelay_latency(&l, 1e9f), omx_fdelay_max_delay(4096u, order));
      ok(omx_fdelay_latency(&l, 0.3f) == omx_fdelay_min_delay(order), "a fraction below the reach is raised to the shortest whole delay", omx_fdelay_latency(&l, 0.3f), omx_fdelay_min_delay(order));
    }
  }
  ok(omx_fdelay_cap_for(100.0f, 5) == 100u + 2u + 2u, "the cap is the delay plus the reach plus the slot", omx_fdelay_cap_for(100.0f, 5), 104.0);
  ok(omx_fdelay_max_delay(omx_fdelay_cap_for(100.0f, 3), 3) >= 100.0f, "a cap sized for a delay serves it", omx_fdelay_max_delay(omx_fdelay_cap_for(100.0f, 3), 3), 100.0);
  /* omx-dsp#250, found by fuzz_delay: an order-5 line over the smallest ring it arms on (5 slots) serves delays up to 1
   * while a fractional one is raised to its kernel's reach of 2, and the line delivered 2, past its ring, against its own
   * post `clamped-delay-is-inside-the-ring`. The clamp now follows the raise: such a ring delivers its longest whole delay. */
  {
    float tiny[5] = {0};
    struct omx_fdelay t;
    ok(omx_fdelay_init(&t, tiny, 5u, 5) == OMX_FDELAY_OK, "the smallest ring an order-5 line arms on", 5, 0.0);
    ok(omx_fdelay_max_delay(5u, 5) == 1.0f && omx_fdelay_min_delay(5) == 2.0f, "it serves less than the kernel's reach", omx_fdelay_max_delay(5u, 5), 1.0);
    static const float REQ[] = {0.3f, 1.5f, 2.0f, 1e-30f, 1e9f, 0.0f, 1.0f, NAN};
    int inside = 1;
    for (unsigned i = 0; i < sizeof REQ / sizeof REQ[0]; i++) {
      const float d = omx_fdelay_latency(&t, REQ[i]);
      if (!(d >= 0.0f && d <= 1.0f && d == floorf(d))) inside = 0;
    }
    ok(inside, "every request is delivered as a whole delay inside the ring", inside, 1.0);
    float x[64], y[64];
    for (uint32_t i = 0; i < 64u; i++) y[i] = x[i] = rnd(-1.0f, 1.0f);
    omx_fdelay_process(y, 64u, &t, 0.3f);
    int exact = y[0] == 0.0f;
    for (uint32_t i = 1; i < 64u; i++) if (y[i] != x[i - 1u]) exact = 0;
    ok(exact, "a fractional request on that ring is the whole delay of 1, bit for bit", exact, 1.0);
    expect_clean();
  }
  ok(omx_fdelay_state_size() == sizeof(struct omx_fdelay), "the state layout is exported", (double)omx_fdelay_state_size(), sizeof(struct omx_fdelay));
  expect_clean();
  fd_arm_tick_pair();
  fdelay_moved_arms();
}

/* ---- tick_pair against two ticks --------------------------------------------------------
 * omx_fdelay_tick_pair splits the delay once and computes one kernel for both lines; its reads
 * must be the bits two omx_fdelay_tick calls give. Orders 3 and 5, 20 000 samples each of a swept
 * fractional delay and of whole delays, the two legs fed different signals (one of them partly
 * below the flush floor), every read compared by memcmp. */
#define FD_TP_CAP 2048u
#define FD_TP_LEN 20000u
static void fd_arm_tick_pair(void) {
  g_arm = "fdelay tick-pair";
  static float ra[FD_TP_CAP], rb[FD_TP_CAP], rc[FD_TP_CAP], rd[FD_TP_CAP];
  for (int oi = 0; oi < 2; oi++) {
    const int order = oi == 0 ? 3 : 5;
    for (int whole = 0; whole <= 1; whole++) {
      memset(ra, 0, sizeof ra); memset(rb, 0, sizeof rb);
      memset(rc, 0, sizeof rc); memset(rd, 0, sizeof rd);
      struct omx_fdelay a, b, c, d;
      omx_fdelay_init(&a, ra, FD_TP_CAP, order);
      omx_fdelay_init(&b, rb, FD_TP_CAP, order);
      omx_fdelay_init(&c, rc, FD_TP_CAP, order);
      omx_fdelay_init(&d, rd, FD_TP_CAP, order);
      const float lo = omx_fdelay_min_delay(order);
      const float hi = omx_fdelay_max_delay(FD_TP_CAP, order);
      long diff = 0;
      for (uint32_t i = 0; i < FD_TP_LEN; i++) {
        const float xa = rnd(-1.0f, 1.0f);
        const float xb = rnd(-1.0f, 1.0f) * (i % 5 == 0 ? 1e-30f : 1.0f);
        const float ph = (float)i / (float)FD_TP_LEN;
        float delay = lo + (hi - lo) * 0.5f * (1.0f - cosf(6.2831853f * 3.0f * ph));
        if (whole) delay = floorf(delay);
        float ya, yb;
        omx_fdelay_tick_pair(&a, &b, xa, xb, delay, &ya, &yb);
        const float wa = omx_fdelay_tick(&c, xa, delay);
        const float wb = omx_fdelay_tick(&d, xb, delay);
        if (memcmp(&ya, &wa, sizeof ya) != 0 || memcmp(&yb, &wb, sizeof yb) != 0) diff++;
      }
      ok(diff == 0, whole ? "tick_pair reads two ticks' bits at whole delays"
                          : "tick_pair reads two ticks' bits at swept delays",
         (double)diff, 0.0);
      ok(a.wpos == c.wpos && b.wpos == d.wpos, "tick_pair advances both cursors as two ticks do",
         (double)a.wpos, (double)c.wpos);
      printf("omxdsp_suite fdelay: tick_pair order %d %s, %ld/%u samples bit-identical to two ticks\n",
             order, whole ? "whole" : "swept", (long)FD_TP_LEN - diff, FD_TP_LEN);
    }
  }
  expect_clean();
  g_arm = "fdelay";
}

/* ---- the fractional delay line against a CLOSED-FORM oracle (moved from openmixer) -----------
 *
 * These lived in the engine's
 * mix_fdelay.test.c. The oracle is
 * ../fx/fdelay_oracle.h's omx_oracle_fdelay: the same unique interpolating polynomial built by
 * NEWTON'S DIVIDED DIFFERENCES over the complex-exponential samples, never forming a Lagrange
 * coefficient, so the kernel is never compared with itself. Nothing below is a recorded number
 * except the ruled figures, which the MEASURED worst cases are compared against.
 *
 *   A — EXACT COPY: orders 3 and 5, every integer delay 0..64, BOTH doors, input below the flush
 *       floor; plus the memcmp ruler's positive control.
 *   B — THE §2 TABLE at the nine oracle rates: a 20 kHz tone (15 kHz at 32 kHz, whose Nyquist is
 *       16 kHz), 101 fractions, loss and GROUP DELAY measured by DFT against the closed form
 *       within 0.01 dB / 0.001 sample; the ruled figures at the six rates it quotes, 0.01 dB /
 *       0.01 µs; the band sweep 1-20 kHz; and the REFUSED linear row from the closed form.
 *   C — NO CLICK: a 1 kHz tone under a slewed delay steps no more than the derived bound, with a
 *       nearest-sample slew as the positive control, at the nine oracle rates.
 *   D — THE RING: geometry helpers, the shortest legal delay over many laps of a short ring
 *       between guard words, presence AND absence of a poisoned slot, a zero tap skipped over a
 *       non-finite slot, the refusals with their codes, and cap_for's promise.
 */

static const int FD_ORDER[2] = {3, 5};

/** Wrap a phase difference into (−π, π]. */
static double fd_wrap_pi(double p) {
  while (p > M_PI) p -= 2.0 * M_PI;
  while (p <= -M_PI) p += 2.0 * M_PI;
  return p;
}

/** The oracle's group delay at `w`, by the SAME centred difference the measurement uses, so the
 *  two are comparable without a finite-difference error between them. */
static double fd_oracle_gd(int order, double d, double w, double dw) {
  const double p1 = carg(omx_oracle_fdelay(order, d, w - dw));
  const double p2 = carg(omx_oracle_fdelay(order, d, w + dw));
  return -fd_wrap_pi(p2 - p1) / (2.0 * dw);
}

/** The arm-B tone at `sr`: 20 kHz (the ruled column) wherever it is below Nyquist; at 32 kHz
 *  the band's next tone, 15 kHz — a 20 kHz tone there is a 12 kHz alias, not a 20 kHz point. */
static double fd_b_tone(double sr) { return 20000.0 < 0.5 * sr ? 20000.0 : 15000.0; }

/* ---- arm A: a whole-sample delay is an EXACT COPY ------------------------------------------ */

#define FD_ACAP 256u
#define FD_ALEN 512u

/** A deterministic block that deliberately spans the flush floor: a line that flushed its exact
 *  branch would turn the tiny words into zeros and `memcmp` would see it. */
static void fd_fill_spanning_the_flush_floor(float *b, uint32_t n) {
  uint32_t s = 0x9e3779b9u;
  for (uint32_t i = 0; i < n; i++) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    float x = ((float)(s >> 8) / 16777216.0f) * 2.0f - 1.0f;
    if ((i % 7u) == 3u) x *= 1e-25f; /* below omx_flush's 1e-20 floor, above subnormal */
    if ((i % 13u) == 5u) x *= 1e-30f;
    b[i] = x;
  }
}

/* The delay is in SAMPLES and the exact branch has no rate in it: one sweep of every integer
 * delay 0..64 covers every rate (the rate-loop arm above checks the 1 ms delay at each). */
static void fd_arm_a_exact_copy(void) {
  g_arm = "fdelay A exact copy";
  static float ring[FD_ACAP], in[FD_ALEN], out[FD_ALEN];
  for (int oi = 0; oi < 2; oi++) {
    const int order = FD_ORDER[oi];
    int worst_block = 1, worst_tick = 1, head_ok = 1, armed = 1;
    for (uint32_t D = 0; D <= 64u; D++) {
      fd_fill_spanning_the_flush_floor(in, FD_ALEN);
      /* THE BLOCK DOOR. */
      memset(ring, 0, sizeof ring);
      struct omx_fdelay l;
      if (omx_fdelay_init(&l, ring, FD_ACAP, order) != OMX_FDELAY_OK) armed = 0;
      memcpy(out, in, sizeof out);
      omx_fdelay_process(out, FD_ALEN, &l, (float)D);
      if (memcmp(out + D, in, (size_t)(FD_ALEN - D) * sizeof(float)) != 0) worst_block = 0;
      for (uint32_t i = 0; i < D; i++)
        if (out[i] != 0.0f) head_ok = 0; /* a zeroed ring is silence, exactly */
      /* THE PER-SAMPLE DOOR — the same law, sample by sample. */
      memset(ring, 0, sizeof ring);
      struct omx_fdelay t;
      if (omx_fdelay_init(&t, ring, FD_ACAP, order) != OMX_FDELAY_OK) armed = 0;
      for (uint32_t i = 0; i < FD_ALEN; i++) out[i] = omx_fdelay_tick(&t, in[i], (float)D);
      if (memcmp(out + D, in, (size_t)(FD_ALEN - D) * sizeof(float)) != 0) worst_tick = 0;
    }
    char what[160];
    snprintf(what, sizeof what, "order %d: the line arms for every delay", order);
    ok(armed, what, (double)!armed, 0.0);
    snprintf(what, sizeof what,
             "order %d: every integer delay 0..64 is a BIT-IDENTICAL copy (block door)", order);
    ok(worst_block, what, (double)!worst_block, 0.0);
    snprintf(what, sizeof what, "order %d: and through the per-sample door too", order);
    ok(worst_tick, what, (double)!worst_tick, 0.0);
    snprintf(what, sizeof what, "order %d: the samples before the first tap are exact zero", order);
    ok(head_ok, what, (double)!head_ok, 0.0);
  }
  /* THE POSITIVE CONTROL for this arm's ruler: `memcmp` must SEE a one-ulp difference. */
  static float a[FD_ALEN], b[FD_ALEN];
  fd_fill_spanning_the_flush_floor(a, FD_ALEN);
  memcpy(b, a, sizeof b);
  ok(memcmp(a, b, sizeof a) == 0, "control: memcmp agrees with an identical block", 0.0, 0.0);
  b[FD_ALEN / 2] = nextafterf(b[FD_ALEN / 2], 1.0f);
  ok(memcmp(a, b, sizeof a) != 0, "control: and SEES a one-ulp difference", 0.0, 0.0);
  expect_clean();
}

/* ---- arm B: the §2 table, measured --------------------------------------------------------- */

/* A 5 Hz bin is a whole number of samples at every oracle rate (all nine are multiples of 5 Hz),
 * and every tone injected here (the kHz-round tones and the 50 Hz group-delay offsets) is a
 * multiple of 5 Hz: the window holds whole cycles and the DFT leaks nothing. The window length
 * is computed per rate; the arrays are sized for 192 kHz's 38 400 and a longer one is refused. */
#define FD_BWIN_MAX 38400u
#define FD_BWARM 2048u
#define FD_BLEN_MAX (FD_BWIN_MAX + FD_BWARM)
#define FD_BCAP 64u
#define FD_BDELTA 50.0 /* the centred difference the group delay is measured over, in Hz */

static float fd_tone[3][FD_BLEN_MAX], fd_bout[FD_BLEN_MAX];
static double fd_cs[3][FD_BWIN_MAX], fd_sn[3][FD_BWIN_MAX], fd_tone_mag[3], fd_tone_ph[3];

/** Magnitude and phase of `b`'s last `win` samples at bin `bi`, by direct DFT in double. */
static void fd_dft_bin(const float *b, uint32_t win, int bi, double *mag, double *ph) {
  double re = 0.0, im = 0.0;
  const float *w = b + FD_BWARM;
  for (uint32_t i = 0; i < win; i++) {
    re += (double)w[i] * fd_cs[bi][i];
    im -= (double)w[i] * fd_sn[bi][i];
  }
  *mag = hypot(re, im) * 2.0 / (double)win;
  *ph = atan2(im, re);
}

/** Bin `bi` at `w`: its DFT basis, the 0.5-amplitude input tone, and that tone's own DFT — the
 *  input is the same for every fraction, so it is built and measured once per bin. */
static void fd_set_bin(int bi, double w, uint32_t win) {
  for (uint32_t i = 0; i < win; i++) {
    const double a = w * (double)(i + FD_BWARM);
    fd_cs[bi][i] = cos(a);
    fd_sn[bi][i] = sin(a);
  }
  for (uint32_t i = 0; i < win + FD_BWARM; i++) fd_tone[bi][i] = 0.5f * sinf((float)(w * (double)i));
  fd_dft_bin(fd_tone[bi], win, bi, &fd_tone_mag[bi], &fd_tone_ph[bi]);
}

/** Play bin `bi`'s tone through a fresh line at delay `d`, and DFT both ends. */
static void fd_measure(int order, double d, uint32_t win, int bi, double *mi, double *pi_, double *mo,
                       double *po) {
  static float ring[FD_BCAP];
  const uint32_t winlen = win + FD_BWARM;
  memcpy(fd_bout, fd_tone[bi], winlen * sizeof(float));
  memset(ring, 0, sizeof ring);
  struct omx_fdelay l;
  omx_fdelay_init(&l, ring, FD_BCAP, order);
  omx_fdelay_process(fd_bout, winlen, &l, (float)d);
  *mi = fd_tone_mag[bi];
  *pi_ = fd_tone_ph[bi];
  fd_dft_bin(fd_bout, win, bi, mo, po);
}

/* The loss table, ruled 2026-09-20, to two decimals:
 * worst loss (dB) and worst group-delay error (µs) at 20 kHz over 101 fractions, {order 3,
 * order 5}. It quotes the six declared rates only; 32, 64 and 128 kHz have no row and are held to
 * the closed form alone. ONE TOLERANCE FOR EVERY ROW — 0.01 dB, 0.01 µs: a looser row is a rate
 * the kernel may drift in while the battery stays green. */
static const struct {
  float sr;
  double loss[2], err_us[2];
} FD_TABLE[] = {
  {44100.0f, {-13.28, -11.40}, {37.00, 36.31}}, {48000.0f, {-8.41, -6.67}, {17.10, 16.16}},
  {88200.0f, {-0.74, -0.26}, {0.67, 0.33}},     {96000.0f, {-0.53, -0.17}, {0.45, 0.20}},
  {176400.0f, {-0.05, -0.01}, {0.02, 0.00}},    {192000.0f, {-0.04, 0.00}, {0.02, 0.00}},
};
#define FD_TABLE_ROWS (sizeof FD_TABLE / sizeof FD_TABLE[0])

static void fd_arm_b_the_table(double sr) {
  const uint32_t win = (uint32_t)llround(sr / 5.0); /* a 5 Hz bin, exact at every oracle rate */
  ok(win <= FD_BWIN_MAX, "the 5 Hz window fits the arrays", (double)win, (double)FD_BWIN_MAX);
  if (win > FD_BWIN_MAX) return;
  const double f0 = fd_b_tone(sr);
  const double freq[3] = {f0 - FD_BDELTA, f0, f0 + FD_BDELTA};
  double w[3];
  for (int bi = 0; bi < 3; bi++) {
    w[bi] = 2.0 * M_PI * freq[bi] / sr;
    fd_set_bin(bi, w[bi], win);
  }
  const double dw = w[2] - w[1];
  char what[200];

  /* THE RULER, FIRST. A pure INTEGER delay has no loss and no delay error at all; if the DFT or
   * the phase difference were broken, this would not read zero. */
  {
    double mi = 0, pin[3], mo[3], po[3], m1 = 0;
    for (int bi = 0; bi < 3; bi++) {
      fd_measure(5, 8.0, win, bi, &mi, &pin[bi], &mo[bi], &po[bi]);
      if (bi == 1) m1 = mi;
    }
    const double loss = 20.0 * log10(mo[1] / m1);
    snprintf(what, sizeof what, "%.0f Hz ruler: an INTEGER delay measures 0.0000 dB of loss", sr);
    ok(fabs(loss) < 1e-4, what, loss, 1e-4);
    const double gd = -fd_wrap_pi((po[2] - pin[2]) - (po[0] - pin[0])) / (2.0 * dw);
    snprintf(what, sizeof what, "%.0f Hz ruler: and its group delay measures exactly 8 samples", sr);
    ok(fabs(gd - 8.0) < 1e-3, what, gd, 8.0);
  }

  int row = -1;
  for (unsigned r = 0; r < FD_TABLE_ROWS; r++)
    if ((double)FD_TABLE[r].sr == sr) row = (int)r;

  /* THE TABLE. 101 fractions on an 8-sample base, both orders. */
  for (int oi = 0; oi < 2; oi++) {
    const int order = FD_ORDER[oi];
    double worst_loss = 0.0, worst_err = 0.0, worst_dloss = 0.0, worst_dgd = 0.0;
    for (int fi = 0; fi <= 100; fi++) {
      const double d = 8.0 + (double)fi / 100.0;
      double mi[3], phi[3], mo[3], pho[3];
      for (int bi = 0; bi < 3; bi++) fd_measure(order, d, win, bi, &mi[bi], &phi[bi], &mo[bi], &pho[bi]);
      /* MEASURED: the magnitude at f0, and the group delay by the centred difference. */
      const double loss_db = 20.0 * log10(mo[1] / mi[1]);
      const double gd = -fd_wrap_pi((pho[2] - phi[2]) - (pho[0] - phi[0])) / (2.0 * dw);
      /* THE ORACLE, for the same three frequencies. */
      const double o_loss = 20.0 * log10(cabs(omx_oracle_fdelay(order, d, w[1])));
      const double o_gd = fd_oracle_gd(order, d, w[1], dw);
      if (fabs(loss_db - o_loss) > worst_dloss) worst_dloss = fabs(loss_db - o_loss);
      if (fabs(gd - o_gd) > worst_dgd) worst_dgd = fabs(gd - o_gd);
      if (loss_db < worst_loss) worst_loss = loss_db;
      if (fabs(gd - d) > worst_err) worst_err = fabs(gd - d);
    }
    const double worst_err_us = worst_err / sr * 1e6;
    printf("omxdsp_suite fdelay B: Lagrange-%d at %.1f kHz / %.0f kHz over 101 fractions: worst loss "
           "%.4f dB, worst group-delay error %.5f samples = %.4f us\n",
           order, sr / 1000.0, f0 / 1000.0, worst_loss, worst_err, worst_err_us);
    snprintf(what, sizeof what, "%.0f Hz, order %d: measured loss matches the closed form", sr, order);
    ok(worst_dloss <= 0.01, what, worst_dloss, 0.01);
    snprintf(what, sizeof what, "%.0f Hz, order %d: measured group delay matches the closed form", sr,
             order);
    ok(worst_dgd <= 0.001, what, worst_dgd, 0.001);
    if (row >= 0) {
      /* AND THE RULING'S OWN NUMBERS, to the hundredth the ruling quotes them at. */
      snprintf(what, sizeof what, "%.0f Hz, order %d: the worst loss IS the ruled figure", sr, order);
      ok(fabs(worst_loss - FD_TABLE[row].loss[oi]) <= 0.01, what, worst_loss, FD_TABLE[row].loss[oi]);
      snprintf(what, sizeof what, "%.0f Hz, order %d: and so is the worst delay error", sr, order);
      ok(fabs(worst_err_us - FD_TABLE[row].err_us[oi]) <= 0.01, what, worst_err_us,
         FD_TABLE[row].err_us[oi]);
    }
  }
  /* The ORDERING the ruling turns on: 5 beats 3 at 20 kHz, at every rate the table quotes. */
  if (row >= 0) {
    snprintf(what, sizeof what, "%.0f Hz: the table the ruling read prefers order 5", sr);
    ok(FD_TABLE[row].loss[1] > FD_TABLE[row].loss[0], what, FD_TABLE[row].loss[1],
       FD_TABLE[row].loss[0]);
  }
}

/* §2's table is two points at 20 kHz; this walks the band a strip actually carries — 1, 2, 5,
 * 10, 15 and 20 kHz — at both orders, worst loss over the same 101 fractions against the same
 * closed form at the same 0.01 dB. The engine ran it at 96 kHz (the graph rate); it runs here at
 * every oracle rate, each tone below that rate's Nyquist. No delay-error column: the ruling only
 * ever quoted delay error at 20 kHz, and this sweep does not widen that claim. */
static void fd_arm_b_band_sweep(double sr) {
  const double freqs[6] = {1000.0, 2000.0, 5000.0, 10000.0, 15000.0, 20000.0};
  const uint32_t win = (uint32_t)llround(sr / 5.0);
  if (win > FD_BWIN_MAX) return; /* refused, with its FAIL, by fd_arm_b_the_table */
  char what[200];
  for (int fx = 0; fx < 6; fx++) {
    if (freqs[fx] >= 0.5 * sr) continue;
    const double w0 = 2.0 * M_PI * freqs[fx] / sr;
    fd_set_bin(0, w0, win);
    for (int oi = 0; oi < 2; oi++) {
      const int order = FD_ORDER[oi];
      double worst_loss = 0.0, worst_dloss = 0.0;
      for (int fi = 0; fi <= 100; fi++) {
        const double d = 8.0 + (double)fi / 100.0;
        double mi, phi, mo, pho;
        fd_measure(order, d, win, 0, &mi, &phi, &mo, &pho);
        const double loss_db = 20.0 * log10(mo / mi);
        const double o_loss = 20.0 * log10(cabs(omx_oracle_fdelay(order, d, w0)));
        if (loss_db < worst_loss) worst_loss = loss_db;
        if (fabs(loss_db - o_loss) > worst_dloss) worst_dloss = fabs(loss_db - o_loss);
      }
      if (sr == 96000.0)
        printf("omxdsp_suite fdelay B band: Lagrange-%d at 96 kHz / %.0f Hz: worst loss %.4f dB\n", order,
               freqs[fx], worst_loss);
      snprintf(what, sizeof what, "band sweep %.0f Hz at %.0f Hz, order %d: loss matches the closed form",
               freqs[fx], sr, order);
      ok(worst_dloss <= 0.01, what, worst_dloss, 0.01);
    }
  }
}

/* §2's table has three rows and the line can only run two: order 1 IS linear interpolation and
 * omx_fdelay_init refuses it by code (§1). A kernel refused for a reason nobody measures is a
 * preference, so the linear row is measured HERE from the same closed form — same 101 fractions,
 * same 8-sample base, same tone — and the refusal is asserted in the same breath. It is a CLOSED
 * FORM and not a running line, on purpose: the running line for this order does not exist. */
static void fd_arm_b_the_refused_row(double sr) {
  const double w0 = 2.0 * M_PI * fd_b_tone(sr) / sr;
  const double dw = 2.0 * M_PI * FD_BDELTA / sr;
  const int ORDERS[3] = {1, 3, 5};
  double worst[3] = {0.0, 0.0, 0.0}, worst_err[3] = {0.0, 0.0, 0.0};
  for (int oi = 0; oi < 3; oi++)
    for (int fi = 0; fi <= 100; fi++) {
      const double d = 8.0 + (double)fi / 100.0;
      const double loss = 20.0 * log10(cabs(omx_oracle_fdelay(ORDERS[oi], d, w0)));
      const double err = fabs(fd_oracle_gd(ORDERS[oi], d, w0, dw) - d);
      if (loss < worst[oi]) worst[oi] = loss;
      if (err > worst_err[oi]) worst_err[oi] = err;
    }
  const double err_us = worst_err[0] / sr * 1e6;
  char what[200];
  /* §2's linear row: 96 kHz loss and delay error, 48 kHz loss (no delay-error cell there). */
  if (sr == 96000.0) {
    ok(fabs(worst[0] - (-2.01)) <= 0.01, "96 kHz: linear's loss IS the ruled figure", worst[0], -2.01);
    ok(fabs(err_us - 1.00) <= 0.01, "96 kHz: and so is its delay error", err_us, 1.00);
  }
  if (sr == 48000.0)
    ok(fabs(worst[0] - (-11.74)) <= 0.01, "48 kHz: linear's loss IS the ruled figure", worst[0], -11.74);
  /* THE ORDERING THE RULING TURNS ON: linear the worst row of the three and order 5 the best, in
   * loss AND in delay error (the engine asserted it at 96 and 48 kHz; here at every oracle rate). */
  snprintf(what, sizeof what, "%.0f Hz: linear loses more than Lagrange-3, which loses more than Lagrange-5",
           sr);
  ok(worst[0] < worst[1] && worst[1] < worst[2], what, worst[0], worst[2]);
  snprintf(what, sizeof what, "%.0f Hz: and the same order holds for the delay error", sr);
  ok(worst_err[0] > worst_err[1] && worst_err[1] > worst_err[2], what, worst_err[0], worst_err[2]);
}

static void fd_arm_b(void) {
  g_arm = "fdelay B the table";
  /* Every row the ruling quotes is compared: a table rate that left the RME set would drop a row. */
  for (unsigned r = 0; r < FD_TABLE_ROWS; r++) {
    int found = 0;
    for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++)
      if (OMX_FX_ORACLE_RATES[ri] == FD_TABLE[r].sr) found = 1;
    ok(found, "every §2 table rate is an oracle rate this arm runs", FD_TABLE[r].sr, 0.0);
  }
  for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    const double sr = (double)OMX_FX_ORACLE_RATES[ri];
    fd_arm_b_the_table(sr);
    fd_arm_b_band_sweep(sr);
    fd_arm_b_the_refused_row(sr);
  }
  expect_clean();
  /* AND THE OTHER HALF OF THE FACT: the row measured above is one the line will not build. */
  static float ring[64];
  struct omx_fdelay l;
  ok(omx_fdelay_init(&l, ring, 64u, 1) == OMX_FDELAY_BAD_ORDER,
     "the line refuses the very order just measured (§1, with arm D)", 0.0, 0.0);
  ok(l.order == 0, "and is left unarmed", (double)l.order, 0.0);
  omx_contract_reset(); /* the refusal above trips its own PRE, by design */
}

/* ---- arm C: a slewed fraction never steps -------------------------------------------------- */

#define FD_CLEN 4096u
#define FD_CCAP 64u

static void fd_arm_c_no_click(void) {
  g_arm = "fdelay C no click";
  static float ring[FD_CCAP], out[FD_CLEN], zoh[FD_CLEN];
  const double f = 1000.0, A = 0.5;
  /* d0..d1 spans 32 samples. A faster slew shrinks `ideal_step` while the discrete worst step the
   * window's grid can find does not, which gives the positive control a margin over its own bound
   * without changing what either bound MEANS. The engine's copy spanned 16, enough at the six
   * declared rates (8 left the margin negative at 48 kHz); at 32 kHz, where ε is largest, 16 and
   * 20 leave the CONTROL short of its bound by 0.13% and 0.03%, and 32 clears it by >= 0.26% at
   * all nine rates. The no-click assertion's own margin is the same at every span tried. */
  const double d0 = 4.0, d1 = 36.0;
  const double dd = (d1 - d0) / (double)FD_CLEN; /* the slew, in samples per sample — rate-free */
  for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
    const double sr = (double)OMX_FX_ORACLE_RATES[ri];
    const double wt = 2.0 * M_PI * f / sr;
    for (int oi = 0; oi < 2; oi++) {
      const int order = FD_ORDER[oi];
      /* THE BOUND, derived rather than chosen. A delay slewing by `dd` per sample is a
       * resampling: the ideal output is the same tone at ω' = ω(1 − dd), whose largest step is
       * 2A·sin(ω'/2) — the TONE, not a click. Each output sample may differ from that ideal by
       * at most A·ε, ε the interpolator's worst deviation from an exact delay at this frequency
       * over the fractions the slew crosses (the oracle answers it); two consecutive samples can
       * be off in opposite directions, so 2A·ε; and float rounding, 8·FLT_EPSILON·A per sample,
       * twice for a step. The bound carries no rate constant beyond ω. */
      double eps = 0.0;
      for (int i = 0; i <= 200; i++) {
        const double d = d0 + (d1 - d0) * (double)i / 200.0;
        const double e = cabs(omx_oracle_fdelay(order, d, wt) * cexp(I * wt * d) - 1.0);
        if (e > eps) eps = e;
      }
      const double ideal_step = 2.0 * A * sin(wt * (1.0 - dd) / 2.0);
      const double bound = ideal_step + 2.0 * A * (eps + 8.0 * (double)FLT_EPSILON);
      memset(ring, 0, sizeof ring);
      struct omx_fdelay l;
      ok(omx_fdelay_init(&l, ring, FD_CCAP, order) == OMX_FDELAY_OK, "the line arms", 0.0, 0.0);
      /* WARM FIRST: a line reading its own zeroed ring is not the stage under test. */
      for (uint32_t i = 0; i < FD_CCAP * 4u; i++) omx_fdelay_tick(&l, (float)(A * sin(wt * (double)i)), (float)d0);
      double worst = 0.0;
      for (uint32_t i = 0; i < FD_CLEN; i++) {
        const double x = A * sin(wt * (double)(i + FD_CCAP * 4u));
        out[i] = omx_fdelay_tick(&l, (float)x, (float)(d0 + dd * (double)i));
        if (i > 0 && fabs((double)out[i] - (double)out[i - 1]) > worst)
          worst = fabs((double)out[i] - (double)out[i - 1]);
      }
      char what[200];
      snprintf(what, sizeof what, "%.0f Hz, order %d: a slewed fraction steps no more than the tone does",
               sr, order);
      ok(worst <= bound, what, worst, bound);
      /* THE POSITIVE CONTROL: the same slew read at the NEAREST sample — what this primitive
       * exists not to be. Each time the rounded delay ticks over the output jumps a whole sample
       * of tone, and the measure must SEE it; without this "no step" fits a broken ruler. */
      double zworst = 0.0;
      for (uint32_t i = 0; i < FD_CLEN; i++) {
        const double nearest = floor(d0 + dd * (double)i + 0.5);
        zoh[i] = (float)(A * sin(wt * ((double)(i + FD_CCAP * 4u) - nearest)));
        if (i > 0 && fabs((double)zoh[i] - (double)zoh[i - 1]) > zworst)
          zworst = fabs((double)zoh[i] - (double)zoh[i - 1]);
      }
      snprintf(what, sizeof what, "control, %.0f Hz, order %d: a NEAREST-SAMPLE slew breaks the bound", sr,
               order);
      ok(zworst > bound, what, zworst, bound);
    }
  }
  expect_clean();
}

/* ---- arm D: the ring, its shortest legal delay, and its wrap ------------------------------- */

#define FD_DGUARD 8u

static void fd_arm_d_the_ring(void) {
  g_arm = "fdelay D the ring";
  char what[200];
  /* The helpers first: what the header says the geometry IS. */
  ok(omx_fdelay_lookbehind(3) == 1u && omx_fdelay_lookahead(3) == 2u,
     "order 3 reads 1 sample behind the point and 2 ahead", 0.0, 0.0);
  ok(omx_fdelay_lookbehind(5) == 2u && omx_fdelay_lookahead(5) == 3u, "order 5 reads 2 behind and 3 ahead",
     0.0, 0.0);
  ok(omx_fdelay_min_delay(3) == 1.0f && omx_fdelay_min_delay(5) == 2.0f,
     "the shortest fractional delay is 1 sample at order 3, 2 at order 5", 0.0, 0.0);

  for (int oi = 0; oi < 2; oi++) {
    const int order = FD_ORDER[oi];
    const double d = (double)omx_fdelay_min_delay(order) + 0.5; /* worst case for lookahead */
    /* A SHORT ring with POISON on both sides: a tap that walked out of the line's own slots would
     * pick up 1e20 and miss the closed form by fifteen orders of magnitude. 4096 samples over a
     * ring of a few floats is hundreds of laps. The geometry is in samples; the tone is played
     * at every oracle rate only because it is cheap. */
    const uint32_t cap = omx_fdelay_cap_for((float)d, order);
    static float buf[FD_DGUARD + 64u + FD_DGUARD];
    float *ring = buf + FD_DGUARD;
    for (uint32_t ri = 0; ri < OMX_FX_ORACLE_RATE_COUNT; ri++) {
      const double sr = (double)OMX_FX_ORACLE_RATES[ri];
      for (uint32_t i = 0; i < FD_DGUARD + 64u + FD_DGUARD; i++) buf[i] = 1e20f;
      memset(ring, 0, cap * sizeof(float));
      struct omx_fdelay l;
      ok(omx_fdelay_init(&l, ring, cap, order) == OMX_FDELAY_OK,
         "a line arms over a ring sized by omx_fdelay_cap_for", 0.0, 0.0);
      const double wt = 2.0 * M_PI * 997.0 / sr;
      const double complex H = omx_oracle_fdelay(order, d, wt);
      double worst = 0.0;
      uint32_t laps = 0, prev = l.wpos;
      for (uint32_t i = 0; i < 4096u; i++) {
        const float y = omx_fdelay_tick(&l, (float)(0.5 * sin(wt * (double)i)), (float)d);
        if (l.wpos < prev) laps++;
        prev = l.wpos;
        if (i > 64u) {
          /* The closed form for a steady tone: amplitude × |H|, phase shifted by arg H. */
          const double e = fabs((double)y - 0.5 * cabs(H) * sin(wt * (double)i + carg(H)));
          if (e > worst) worst = e;
        }
      }
      snprintf(what, sizeof what,
               "%.0f Hz, order %d: at the shortest legal delay (%.1f) the line tracks the closed form "
               "over %u ring laps", sr, order, d, laps);
      ok(worst < 2e-4, what, worst, 2e-4);
      snprintf(what, sizeof what, "order %d: and the ring wrapped many times", order);
      ok(laps > 100u, what, (double)laps, 100.0);
      int guards = 1;
      for (uint32_t i = 0; i < FD_DGUARD; i++)
        if (buf[i] != 1e20f || buf[FD_DGUARD + 64u + i] != 1e20f) guards = 0;
      snprintf(what, sizeof what, "order %d: the guard words either side are untouched", order);
      ok(guards, what, (double)!guards, 0.0);
    }
    /* PRESENCE AND ABSENCE, in the same run. A poison word in the slot the kernel MUST read
     * changes the answer; the same word one slot further back — outside the tap span — must not.
     * A test that only showed the second would be satisfied by a line that read nothing. */
    const uint32_t oldest = (uint32_t)d + omx_fdelay_lookbehind(order) + 1u;
    for (uint32_t probe = 0; probe < 2u; probe++) {
      memset(ring, 0, cap * sizeof(float));
      struct omx_fdelay p;
      omx_fdelay_init(&p, ring, cap, order);
      for (uint32_t i = 0; i < 4u * cap; i++) omx_fdelay_tick(&p, 0.25f, (float)d);
      /* distance `oldest` is tap 0; distance `oldest + 1` is one slot older than any tap. */
      const uint32_t dist = oldest + probe;
      const uint32_t last = (p.wpos == 0u) ? cap - 1u : p.wpos - 1u;
      ring[(last >= dist) ? last - dist : last + cap - dist] = 1000.0f;
      const float y = omx_fdelay_read(&p, (float)d);
      snprintf(what, sizeof what, "order %d: a word at distance %u %s the read", order, dist,
               probe == 0u ? "REACHES" : "is outside");
      if (probe == 0u) ok(fabsf(y - 0.25f) > 1.0f, what, (double)y, 0.25);
      else ok(fabsf(y - 0.25f) < 1e-5f, what, (double)y, 0.25);
    }
  }

  /* §1's ZERO TAP, exercised where it can be seen. The kernel never hands the read an exact zero
   * for a fraction INSIDE a sample, so the header's "a zero coefficient is SKIPPED, never
   * multiplied" guard sits on a branch no tone reaches; this builds the situation directly — a
   * NON-FINITE word in the slot a zeroed coefficient multiplies. 0 × inf is NaN and would spread
   * through the sum; a skipped zero leaves the read finite and equal to the other taps. */
  for (int oi = 0; oi < 2; oi++) {
    const int order = FD_ORDER[oi];
    const uint32_t cap = 64u;
    static float ring[64];
    struct omx_fdelay l;
    memset(ring, 0, sizeof ring);
    omx_fdelay_init(&l, ring, cap, order);
    for (uint32_t i = 0; i < 3u * cap; i++) omx_fdelay_write(&l, 0.25f);
    const uint32_t id = 8u;
    const uint32_t oldest = id + omx_fdelay_lookbehind(order) + 1u;
    const uint32_t last = (l.wpos == 0u) ? cap - 1u : l.wpos - 1u;
    ring[(last >= oldest) ? last - oldest : last + cap - oldest] = (float)INFINITY; /* tap 0's slot */
    float c[OMX_FDELAY_MAX_TAPS];
    omx_fdelay_lagrange(order, 0.5f, c);
    double rest = 0.0;
    for (int k = 1; k <= order; k++) rest += (double)c[k] * 0.25;
    c[0] = 0.0f; /* the tap over the poisoned slot, zeroed by hand */
    const float y = omx_fdelay_read_at(&l, id, c);
    snprintf(what, sizeof what,
             "order %d: a ZERO coefficient over a non-finite slot is skipped, not multiplied", order);
    ok(y - y == 0.0f, what, (double)y, 0.0);
    snprintf(what, sizeof what, "order %d: and carries exactly the other taps", order);
    ok(fabs((double)y - rest) < 1e-6, what, (double)y, rest);
  }

  /* The geometry the ring sizing promises: cap_for(D) holds D, and one float less does not. */
  for (int oi = 0; oi < 2; oi++) {
    const int order = FD_ORDER[oi];
    const uint32_t cap = omx_fdelay_cap_for(37.0f, order);
    snprintf(what, sizeof what, "order %d: cap_for(37) serves a delay of 37 samples", order);
    ok(omx_fdelay_max_delay(cap, order) >= 37.0f, what, (double)omx_fdelay_max_delay(cap, order), 37.0);
    snprintf(what, sizeof what, "order %d: and one float less does not", order);
    ok(omx_fdelay_max_delay(cap - 1u, order) < 37.0f, what, (double)omx_fdelay_max_delay(cap - 1u, order),
       37.0);
  }
  expect_clean();

  /* THE REFUSALS. Each answers with its code, leaves the line unarmed, and is recorded in the
   * ledger under the contract it broke — a refusal nobody can see is a silent clamp. */
  static float ring[64];
  struct omx_fdelay l;
  ok(omx_fdelay_init(&l, ring, 64u, 4) == OMX_FDELAY_BAD_ORDER, "an EVEN order is refused by code", 0.0,
     0.0);
  ok(l.order == 0 && l.ring == 0, "and the line is left unarmed", 0.0, 0.0);
  ok(omx_fdelay_init(&l, ring, 64u, 1) == OMX_FDELAY_BAD_ORDER,
     "order 1 — LINEAR interpolation — is refused by code", 0.0, 0.0);
  ok(omx_fdelay_init(&l, ring, 64u, 7) == OMX_FDELAY_BAD_ORDER,
     "an order past the kernel's tap array is refused by code", 0.0, 0.0);
  ok(omx_fdelay_init(&l, ring, 3u, 5) == OMX_FDELAY_RING_TOO_SMALL,
     "a ring too small for the kernel's reach is refused by code", 0.0, 0.0);
  ok(omx_fdelay_init(&l, 0, 64u, 5) == OMX_FDELAY_RING_TOO_SMALL, "and so is no ring at all", 0.0, 0.0);
  /* Those five broke preconditions on purpose: the ledger must have SEEN them. */
  ok(omx_contract_log.count >= 5u, "every refusal is recorded in the contract ledger",
     (double)omx_contract_log.count, 5.0);
  omx_contract_reset();
  /* An unarmed line is an announced passthrough, never a read of nothing. */
  ok(omx_fdelay_tick(&l, 0.75f, 2.5f) == 0.75f, "an unarmed line passes its input through", 0.0, 0.0);
  omx_contract_reset();
}

static void fdelay_moved_arms(void) {
  const uint32_t before = omx_contract_log.checks;
  fd_arm_a_exact_copy();
  fd_arm_b();
  fd_arm_c_no_click();
  fd_arm_d_the_ring();
  /* A FLOOR THAT MEASURES THE SCAN, not the population: arms that evaluated a handful of
   * contracts ran nothing at all, and would print the same clean sheet as a clean kernel. */
  g_arm = "fdelay A-D";
  const uint32_t evaluated = omx_contract_log.checks - before;
  ok(evaluated >= 10000u, "arms A-D evaluated the line's contracts", (double)evaluated, 10000.0);
  g_arm = "fdelay";
}
