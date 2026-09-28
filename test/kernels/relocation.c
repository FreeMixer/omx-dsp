
/* ---- state relocation ----------------------------------------------------------------------- */

/* A block sized and aligned as the primitive exports, the size rounded up to the alignment. */
static void *state_block(size_t size, size_t align) {
  const size_t rounded = (size + align - 1u) / align * align;
  return aligned_alloc(align, rounded);
}

/* Every stateful primitive: a reference run in one state, then a second run whose state is
 * memcpy'd to a fresh aligned block halfway (the old block poisoned, so nothing reads it) —
 * the two outputs bit-identical. The line's ring is caller memory the state points at: it
 * stays where it is and only the state moves. */
static void arm_relocation(void) {
  g_arm = "relocation";
  enum { N = 512, HALF = 256, BLOCK = 64 };
  static float x[N], ref[N], out[N], up[BLOCK * 4];
  for (int i = 0; i < N; i++) x[i] = rnd(-0.5f, 0.5f);
  ok(omx_lfo_state_align() > 0u && (omx_lfo_state_align() & (omx_lfo_state_align() - 1u)) == 0u, "the LFO's alignment is a power of two", (double)omx_lfo_state_align(), 0.0);
  ok(omx_fdelay_state_align() > 0u && (omx_fdelay_state_align() & (omx_fdelay_state_align() - 1u)) == 0u, "the line's alignment is a power of two", (double)omx_fdelay_state_align(), 0.0);
  ok(omx_env_state_align() > 0u && (omx_env_state_align() & (omx_env_state_align() - 1u)) == 0u, "the envelope's alignment is a power of two", (double)omx_env_state_align(), 0.0);
  ok(omx_oversampler_state_align() > 0u && (omx_oversampler_state_align() & (omx_oversampler_state_align() - 1u)) == 0u, "the oversampler's alignment is a power of two", (double)omx_oversampler_state_align(), 0.0);
  {
    struct omx_lfo l = {0.0f, omx_lfo_inc(3.0f, 48000.0f), 0.0f};
    for (int i = 0; i < N; i++) { ref[i] = omx_lfo_at(&l, 0.25f); omx_lfo_advance(&l); }
    struct omx_lfo *a = state_block(omx_lfo_state_size(), omx_lfo_state_align());
    struct omx_lfo *b = state_block(omx_lfo_state_size(), omx_lfo_state_align());
    ok(a != 0 && b != 0, "two aligned blocks for the LFO", 0.0, 0.0);
    a->phase = 0.0f; a->inc = omx_lfo_inc(3.0f, 48000.0f);
    for (int i = 0; i < HALF; i++) { out[i] = omx_lfo_at(a, 0.25f); omx_lfo_advance(a); }
    memcpy(b, a, omx_lfo_state_size());
    memset(a, 0xAA, omx_lfo_state_size());
    for (int i = HALF; i < N; i++) { out[i] = omx_lfo_at(b, 0.25f); omx_lfo_advance(b); }
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated LFO continues bit for bit", 0.0, 0.0);
    free(a); free(b);
  }
  {
    static float ring[1024];
    struct omx_fdelay l;
    memset(ring, 0, sizeof ring);
    ok(omx_fdelay_init(&l, ring, 1024u, 5) == OMX_FDELAY_OK, "the reference line arms", 0.0, 0.0);
    for (int i = 0; i < N; i++) ref[i] = omx_fdelay_tick(&l, x[i], 7.25f + 3.0f * (float)(i % 5));
    memset(ring, 0, sizeof ring);
    struct omx_fdelay *a = state_block(omx_fdelay_state_size(), omx_fdelay_state_align());
    struct omx_fdelay *b = state_block(omx_fdelay_state_size(), omx_fdelay_state_align());
    ok(a != 0 && b != 0, "two aligned blocks for the line", 0.0, 0.0);
    ok(omx_fdelay_init(a, ring, 1024u, 5) == OMX_FDELAY_OK, "the relocating line arms", 0.0, 0.0);
    for (int i = 0; i < HALF; i++) out[i] = omx_fdelay_tick(a, x[i], 7.25f + 3.0f * (float)(i % 5));
    memcpy(b, a, omx_fdelay_state_size());
    memset(a, 0xAA, omx_fdelay_state_size());
    for (int i = HALF; i < N; i++) out[i] = omx_fdelay_tick(b, x[i], 7.25f + 3.0f * (float)(i % 5));
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated line over the same ring continues bit for bit", 0.0, 0.0);
    ok(b->ring == ring, "the relocated line still points at the caller's ring", 0.0, 0.0);
    free(a); free(b);
  }
  {
    const struct omx_env_params e = {omx_pole_from_time_ms(1.0f, 48000.0f), omx_pole_from_time_ms(30.0f, 48000.0f), OMX_DETECT_RMS};
    float ac, rc;
    omx_env_stage_poles(&e, 1u, &ac, &rc);
    struct omx_env ref_env;
    memset(&ref_env, 0, sizeof ref_env);
    for (int i = 0; i < N; i++) ref[i] = omx_env_step(&ref_env, &e, x[i] * x[i], ac, rc);
    struct omx_env *a = state_block(omx_env_state_size(), omx_env_state_align());
    struct omx_env *b = state_block(omx_env_state_size(), omx_env_state_align());
    ok(a != 0 && b != 0, "two aligned blocks for the envelope", 0.0, 0.0);
    memset(a, 0, omx_env_state_size());
    for (int i = 0; i < HALF; i++) out[i] = omx_env_step(a, &e, x[i] * x[i], ac, rc);
    memcpy(b, a, omx_env_state_size());
    memset(a, 0xAA, omx_env_state_size());
    for (int i = HALF; i < N; i++) out[i] = omx_env_step(b, &e, x[i] * x[i], ac, rc);
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated envelope continues bit for bit", 0.0, 0.0);
    free(a); free(b);
  }
  {
    struct omx_oversampler o;
    omx_oversampler_init(&o, 4u);
    for (int i = 0; i < N; i += BLOCK) { omx_oversampler_up(&o, x + i, BLOCK, up); omx_oversampler_down(&o, up, BLOCK, ref + i); }
    struct omx_oversampler *a = state_block(omx_oversampler_state_size(), omx_oversampler_state_align());
    struct omx_oversampler *b = state_block(omx_oversampler_state_size(), omx_oversampler_state_align());
    ok(a != 0 && b != 0, "two aligned blocks for the oversampler", 0.0, 0.0);
    omx_oversampler_init(a, 4u);
    for (int i = 0; i < HALF; i += BLOCK) { omx_oversampler_up(a, x + i, BLOCK, up); omx_oversampler_down(a, up, BLOCK, out + i); }
    memcpy(b, a, omx_oversampler_state_size());
    memset(a, 0xAA, omx_oversampler_state_size());
    for (int i = HALF; i < N; i += BLOCK) { omx_oversampler_up(b, x + i, BLOCK, up); omx_oversampler_down(b, up, BLOCK, out + i); }
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated oversampler continues bit for bit", 0.0, 0.0);
    free(a); free(b);
  }
  {
    const float a = (float)omx_allpass1_coef_d(700.0, 48000.0);
    struct omx_allpass1 r0 = {0.0f};
    memcpy(ref, x, sizeof ref);
    omx_allpass1_block(&r0, ref, N, a);
    struct omx_allpass1 *a1 = state_block(omx_allpass1_state_size(), omx_allpass1_state_align());
    struct omx_allpass1 *b1 = state_block(omx_allpass1_state_size(), omx_allpass1_state_align());
    ok(a1 != 0 && b1 != 0, "two aligned blocks for the all-pass", 0.0, 0.0);
    a1->s = 0.0f;
    memcpy(out, x, sizeof out);
    omx_allpass1_block(a1, out, HALF, a);
    memcpy(b1, a1, omx_allpass1_state_size());
    memset(a1, 0xAA, omx_allpass1_state_size());
    omx_allpass1_block(b1, out + HALF, N - HALF, a);
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated all-pass continues bit for bit", 0.0, 0.0);
    free(a1); free(b1);
  }
  {
    static float hr[N], ho[N];
    struct omx_xover c;
    ok(omx_xover_design(&c, 4u, 300.0, 96000.0) == OMX_XOVER_OK, "the relocating crossover designs", 0.0, 0.0);
    struct omx_xover_state r0;
    memset(&r0, 0, sizeof r0);
    omx_xover_process(x, ref, hr, N, &c, &r0);
    struct omx_xover_state *a1 = state_block(omx_xover_state_size(), omx_xover_state_align());
    struct omx_xover_state *b1 = state_block(omx_xover_state_size(), omx_xover_state_align());
    ok(a1 != 0 && b1 != 0, "two aligned blocks for the crossover", 0.0, 0.0);
    ok((omx_xover_state_align() & (omx_xover_state_align() - 1u)) == 0u, "the crossover's alignment is a power of two", (double)omx_xover_state_align(), 0.0);
    memset(a1, 0, omx_xover_state_size());
    omx_xover_process(x, out, ho, HALF, &c, a1);
    memcpy(b1, a1, omx_xover_state_size());
    memset(a1, 0xAA, omx_xover_state_size());
    omx_xover_process(x + HALF, out + HALF, ho + HALF, N - HALF, &c, b1);
    ok(memcmp(ref, out, sizeof ref) == 0 && memcmp(hr, ho, sizeof hr) == 0, "a relocated crossover continues bit for bit", 0.0, 0.0);
    free(a1); free(b1);
    struct omx_xover_ap_state *p = state_block(omx_xover_ap_state_size(), omx_xover_ap_state_align());
    ok(p != 0 && omx_xover_ap_state_size() == sizeof(struct omx_xover_ap_state), "the tree all-pass exports its layout", 0.0, 0.0);
    free(p);
  }
  {
    struct omx_divider ref_d;
    omx_divider_init(&ref_d);
    omx_divider_block(&ref_d, x, ref, N, 0.1f);
    struct omx_divider *a = state_block(omx_divider_state_size(), omx_divider_state_align());
    struct omx_divider *b = state_block(omx_divider_state_size(), omx_divider_state_align());
    ok(a != 0 && b != 0, "two aligned blocks for the divider", 0.0, 0.0);
    omx_divider_init(a);
    omx_divider_block(a, x, out, HALF, 0.1f);
    memcpy(b, a, omx_divider_state_size());
    memset(a, 0xAA, omx_divider_state_size());
    omx_divider_block(b, x + HALF, out + HALF, N - HALF, 0.1f);
    ok(memcmp(ref, out, sizeof ref) == 0, "a relocated divider continues bit for bit", 0.0, 0.0);
    free(a); free(b);
  }
  expect_clean();
}
