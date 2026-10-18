// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

#include <omxdsp/mix/omx_key.h>

/* ---- a dynamics slot's key (mix/omx_key.h). At each of the nine oracle rates, one second of
 * noise on two legs: no key (a NULL key, a NULL buffer) returns NULL and writes nothing; the self
 * key of a stereo lane is 0.5·(l + r) and of a mono lane the leg, bit for bit, and another lane's
 * block is copied as it is; a listen-only key fills the buffer and returns NULL; the high-pass and
 * the low-pass run in that order, each in its own history and only when its edge is on, the same
 * bits as omx_biquad() run by hand; a 1 kHz high-pass takes a 50 Hz tone down by more than 40 dB
 * and passes a 5 kHz one within 0.5 dB; a 1 kHz low-pass takes a tone at 0.4 of the rate down by
 * more than 40 dB and passes 50 Hz within 0.5 dB. A NaN in the self leg
 * is recorded as `lane/key-build pre finite-in-l`, a NaN in another lane's block as
 * `lane/key-build post key-finite`. -------------------------------------------------------------- */

static const float MIX_KEY_RATES[OMX_ORACLE_FULL_RATES_COUNT] = OMX_ORACLE_FULL_RATES_INIT;

/* An RBJ section, Butterworth Q, normalised {b0,b1,b2,a1,a2}: `hp` 1 for a high-pass. */
static void mix_key_rbj(int hp, double f, double sr, float c[5]) {
  const double w = 2.0 * M_PI * f / sr, cw = cos(w), alpha = sin(w) / (2.0 * M_SQRT1_2);
  const double a0 = 1.0 + alpha;
  const double b0 = hp ? (1.0 + cw) / 2.0 : (1.0 - cw) / 2.0;
  c[0] = (float)(b0 / a0);
  c[1] = (float)((hp ? -(1.0 + cw) : 1.0 - cw) / a0);
  c[2] = (float)(b0 / a0);
  c[3] = (float)(-2.0 * cw / a0);
  c[4] = (float)((1.0 - alpha) / a0);
}

/* The steady-state gain, dB, of a key whose source is a unit tone at `f`: the peak over the
 * second half of one second. */
static double mix_key_tone_db(struct omx_key *k, float *tone, uint32_t n, double f, double sr) {
  for (uint32_t i = 0; i < n; i++) tone[i] = (float)sin(2.0 * M_PI * f * (double)i / sr);
  float hs[4] = {0}, ls[4] = {0};
  k->src = tone;
  omx_key_build(k, tone, NULL, n, hs, ls);
  float peak = 0.0f;
  for (uint32_t i = n / 2u; i < n; i++) if (fabsf(k->buf[i]) > peak) peak = fabsf(k->buf[i]);
  k->src = NULL;
  return 20.0 * log10((double)peak);
}

/* The ledger holds exactly `count` violations, the first of this stage, kind and token, the rest
 * of this stage; otherwise each record is printed (test/mix/sabotage.sh reads them). Then cleared. */
static int mix_key_recorded(uint32_t count, const char *kind, const char *token) {
  int match = omx_contract_log.count == count;
  for (uint32_t i = 0; match && i < count; i++)
    match = omx_contract_record_ready(&omx_contract_log.rec[i]) &&
            strcmp(omx_contract_log.rec[i].stage, "lane/key-build") == 0 &&
            (i > 0u || (strcmp(omx_contract_log.rec[i].kind, kind) == 0 &&
                        strcmp(omx_contract_log.rec[i].token, token) == 0));
  if (!match) {
    const uint32_t kept = omx_contract_log.count < OMX_CONTRACT_MAX ? omx_contract_log.count : OMX_CONTRACT_MAX;
    for (uint32_t i = 0; i < kept; i++)
      if (omx_contract_record_ready(&omx_contract_log.rec[i]))
        printf("  got [%s] %s %s\n", omx_contract_log.rec[i].stage, omx_contract_log.rec[i].kind,
               omx_contract_log.rec[i].token);
  }
  omx_contract_reset();
  return match;
}

static void arm_mix_key(void) {
  g_arm = "mix_key";
  static float l[192000], r[192000], src[192000], buf[192000], want[192000];
  uint32_t seed = 0x4e7u;
  for (uint32_t ri = 0; ri < OMX_ORACLE_FULL_RATES_COUNT; ri++) {
    const float sr = MIX_KEY_RATES[ri];
    const uint32_t n = (uint32_t)sr;
    for (uint32_t i = 0; i < n; i++) {
      seed = seed * 1664525u + 1013904223u;
      l[i] = (float)(int32_t)(seed >> 8) / 8388608.0f - 1.0f;
      seed = seed * 1664525u + 1013904223u;
      r[i] = 0.5f * ((float)(int32_t)(seed >> 8) / 8388608.0f - 1.0f);
      src[i] = 0.25f * l[i] - 0.5f * r[i];
    }
    float hs[4] = {0}, ls[4] = {0};
    struct omx_key k;
    memset(&k, 0, sizeof k);

    /* no key */
    ok(omx_key_build(NULL, l, r, n, hs, ls) == NULL, "a NULL key returns NULL", 0, 0);
    buf[0] = 7.0f;
    ok(omx_key_build(&k, l, r, n, hs, ls) == NULL && !k.built && buf[0] == 7.0f,
       "a NULL buffer returns NULL, builds nothing and writes nothing", k.built, 0);

    /* the sources, no edge on */
    k.buf = buf;
    k.to_detector = 1;
    uint32_t bad = 0;
    ok(omx_key_build(&k, l, r, n, hs, ls) == buf && k.built == 1, "a detector key returns its buffer, built", k.built, 1);
    for (uint32_t i = 0; i < n; i++) if (buf[i] != 0.5f * (l[i] + r[i])) bad++;
    ok(bad == 0, "the self key of a stereo lane is 0.5 (l + r), bit for bit", bad, 0);
    k.built = 0;
    omx_key_build(&k, l, NULL, n, hs, ls);
    ok(memcmp(buf, l, n * sizeof(float)) == 0 && k.built == 1, "the self key of a mono lane is the leg", 0, 0);
    k.src = src;
    omx_key_build(&k, l, r, n, hs, ls);
    ok(memcmp(buf, src, n * sizeof(float)) == 0, "another lane's block is copied as it is", 0, 0);
    k.src = NULL;
    k.to_detector = 0;
    k.built = 0;
    memset(buf, 0, n * sizeof(float));
    ok(omx_key_build(&k, l, r, n, hs, ls) == NULL && k.built == 1 && buf[n / 2u] == 0.5f * (l[n / 2u] + r[n / 2u]),
       "a listen-only key fills its buffer and returns NULL", k.built, 1);
    bad = 0;
    for (uint32_t w = 0; w < 4u; w++) if (hs[w] != 0.0f || ls[w] != 0.0f) bad++;
    ok(bad == 0, "no edge on leaves both histories untouched", bad, 0);
    expect_clean();

    /* the edges: high-pass then low-pass, each in its own history, as omx_biquad by hand */
    k.to_detector = 1;
    mix_key_rbj(1, 200.0, sr, k.hp_c);
    mix_key_rbj(0, 5000.0, sr, k.lp_c);
    for (int edges = 1; edges <= 3; edges++) {
      k.hp_on = edges & 1;
      k.lp_on = edges >> 1;
      float h1[4] = {0}, l1[4] = {0}, h2[4] = {0}, l2[4] = {0};
      omx_key_build(&k, l, r, n, h1, l1);
      for (uint32_t i = 0; i < n; i++) {
        float x = 0.5f * (l[i] + r[i]);
        if (k.hp_on) x = omx_biquad(x, k.hp_c, h2);
        if (k.lp_on) x = omx_biquad(x, k.lp_c, l2);
        want[i] = x;
      }
      ok(memcmp(buf, want, n * sizeof(float)) == 0, "the edges are omx_biquad high-pass then low-pass, bit for bit", edges, 0);
      ok(memcmp(h1, h2, sizeof h1) == 0 && memcmp(l1, l2, sizeof l1) == 0,
         "each edge runs in its own history, and only when it is on", edges, 0);
    }
    expect_clean();

    /* the edges' responses */
    k.hp_on = 1;
    k.lp_on = 0;
    mix_key_rbj(1, 1000.0, sr, k.hp_c);
    const double hp_low = mix_key_tone_db(&k, want, n, 50.0, sr);
    const double hp_high = mix_key_tone_db(&k, want, n, 5000.0, sr);
    ok(hp_low < -40.0, "a 1 kHz high-pass takes a 50 Hz tone down by more than 40 dB", hp_low, -40.0);
    ok(fabs(hp_high) < 0.5, "and passes a 5 kHz tone within 0.5 dB", hp_high, 0.5);
    k.hp_on = 0;
    k.lp_on = 1;
    mix_key_rbj(0, 1000.0, sr, k.lp_c);
    const double lp_high = mix_key_tone_db(&k, want, n, 0.4 * sr, sr);
    const double lp_low = mix_key_tone_db(&k, want, n, 50.0, sr);
    ok(lp_high < -40.0, "a 1 kHz low-pass takes a tone at 0.4 of the rate down by more than 40 dB", lp_high, -40.0);
    ok(fabs(lp_low) < 0.5, "and passes a 50 Hz tone within 0.5 dB", lp_low, 0.5);
    k.lp_on = 0;
    expect_clean();

    /* the contract: a NaN self leg is refused on the way in, and the key it makes on the way out */
    const float keep = l[n / 3u];
    l[n / 3u] = NAN;
    omx_key_build(&k, l, r, n, hs, ls);
    ok(mix_key_recorded(2u, "pre", "finite-in-l"),
       "a NaN in the self leg is recorded as lane/key-build pre finite-in-l", 0, 0);
    memset(hs, 0, sizeof hs);
    l[n / 3u] = keep;
    src[n / 4u] = NAN;
    k.src = src;
    omx_key_build(&k, l, r, n, hs, ls);
    ok(mix_key_recorded(1u, "post", "key-finite"),
       "a NaN in another lane's block is recorded as lane/key-build post key-finite", 0, 0);
    k.src = NULL;
    expect_clean();
  }
}
