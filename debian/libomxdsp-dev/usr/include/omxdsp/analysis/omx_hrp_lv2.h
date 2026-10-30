/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * omx_hrp_lv2.h — HRP+ AS A LIVE INSERT: the third shell's core, with no LV2 in it.
 *
 * The third shell of one C core ("one C core, three shells": the console, a CLI, and an LV2
 * plugin for sound hosts), over the HRP design, whose laws bind here in FULL —
 * this is the live tier, so none of the three constraints the offline render lifts is
 * lifted here, and the header says at each one why not.
 *
 * ## What this file is, and what hrp_lv2.c is
 *
 * Everything below is plain C over `hrp_render.h` and `hrp_correct.h` and knows nothing about
 * LV2 — no `LV2_Handle`, no ports, no worker, no host. That is deliberate and it is the same
 * discipline `hrp_render.h` keeps: it makes the shell's core reachable by `hrp_lv2.test.c`
 * against closed-form oracles with a plain `cc`, and it keeps the LV2 boilerplate in
 * `hrp_lv2.c` down to what it actually is — a descriptor, six ports and two worker callbacks.
 *
 * NO BUFFER IS OWNED HERE either. {@link omx_hrp_lv2_init} takes the caller's arrays, so the
 * host's own pool, a malloc in `instantiate`, or a test's stack frame all work.
 *
 * ## §2 IS THE WHOLE ARCHITECTURE: analysis never runs in the audio callback
 *
 * "HRP analysis MUST NEVER execute inside the PipeWire audio callback" is not a PipeWire
 * statement, it is a statement about audio callbacks, and an LV2 `run()` is one. So the split
 * here is exactly the console's:
 *
 *     run()  (RT)   {@link omx_hrp_lv2_run}      memcpy into the ring, run the biquad cascade
 *                                                with the coefficients already published. No
 *                                                FFT, no detector, no design, no branch that
 *                                                depends on what the audio contains.
 *     work() (worker) {@link omx_hrp_lv2_analyse}  the whole chain — omx_rta_spectrum,
 *                                                omx_hrp_detect, omx_hrp_track_update,
 *                                                omx_hrp_attribute, omx_hrp_baseline_learn —
 *                                                then §29/§30's ranking and the curve design.
 *     work_response() (RT) {@link omx_hrp_lv2_adopt}  a fixed-size struct copy. Nothing else.
 *
 * The host provides the thread (LV2's `lv2:worker` extension), so this plugin starts no thread
 * of its own. A host with no worker gets a plugin that passes audio and never analyses — see
 * {@link omx_hrp_lv2_due}'s note. That is an honest absence, not a silent fallback into
 * analysing on the RT thread, which §2 forbids outright.
 *
 * §1's "do not create a second RT ring" is obeyed by using the existing one: `struct omx_ring`
 * from mix_dsp.h, the same primitive six RTA taps already run on.
 *
 * ## The three offline privileges, and why each stays refused here
 *
 *   1. THE RIPENING DELAY IS NOT LIFTED. The offline render reads the file twice so the take's
 *      opening can be measured against a model the whole performance taught. Live there is no
 *      second read, so {@link omx_hrp_lv2_analyse} feeds ONE pass with `learn = 1` and a note's
 *      first seconds go uncorrected, exactly as §15/§22 say they must. A live corrector that
 *      cut a note it had heard once would be guessing.
 *
 *   2. NO LOOK-AHEAD, AND NO CORRECTION IN FORCE FROM SAMPLE ZERO. A correction here is decided
 *      only from evidence that ends before the sample it is applied to. This is what §2's "HRP
 *      adds ZERO intentional audio latency" costs and buys: {@link OMX_HRP_LV2_LATENCY_FRAMES}
 *      is 0 because there is no delay line to compensate, and the price is that the first
 *      chorus cannot be corrected from the second.
 *
 *   3. THE BUDGET IS REAL. Offline the search runs at the full ceiling with the caller's own
 *      sweep step; here the family prior narrows it (§16) and one analysis pass does one
 *      window's work, so the cost per pass is bounded by the same numbers §57 measures.
 *
 * ## §17: PER-INSTRUMENT, and what that refuses
 *
 * "HRP operates on individual mixer channels. This is important." A sum is not an instrument:
 * its spectrum is many instruments' partials interleaved, so the harmonic grid §9 builds is not
 * a model of anything, and a cut placed on it lands on whichever voice happened to own that
 * partial. The plugin is therefore MONO, one in and one out, and says per-instrument in its own
 * metadata. That is the strongest refusal an LV2 plugin can make: a bus has no mono insert
 * point to rack it at, so the shape of the plugin refuses the use rather than a runtime check
 * hoping to detect it. {@link OMX_HRP_LV2_CHANNELS} is the number, and it is 1 for that reason.
 */
#ifndef OMX_HRP_LV2_H
#define OMX_HRP_LV2_H

#include <stdint.h>
#include <string.h>

#include <omxdsp/analysis/omx_hrp_correct.h>
#include <omxdsp/analysis/omx_hrp_render.h>
#include <omxdsp/analysis/omx_rta.h>
#include <omxdsp/omx_port_int.h>

/** Audio channels in and out. ONE, because §17 makes HRP per-instrument — see the header note. */
#define OMX_HRP_LV2_CHANNELS 1

/** Frames of latency this plugin introduces: NONE.
 *
 * §2: "HRP adds ZERO intentional audio latency." The RT path is a biquad cascade, which has
 * group delay but no delay LINE — there is nothing for a host to compensate — and the analysis
 * that decides the coefficients runs on the host's worker thread, where being late costs
 * lateness and never latency (§15). The plugin still PUBLISHES the number on an `lv2:latency`
 * port, because a host cannot tell "zero" from "never said" and only one of those is safe to
 * compensate for. */
#define OMX_HRP_LV2_LATENCY_FRAMES 0.0f

/** Bands one instance may plant — the channel's HRP budget (§29), not the cascade's capacity. */
#define OMX_HRP_LV2_MAX_BANDS OMX_HRP_DEFAULT_MAX_AUTO_BANDS

/* The ramp step is `hrp_correct.h`'s {@link OMX_HRP_GAIN_STEP_DB} — the console's own, shared
 * rather than restated, so a shell cannot smooth at a rate the desk does not. */

/** Analysis passes that make one evaluation window — after which the measurement is forgotten
 * and the MODEL is kept.
 *
 * The baseline is the long-term model (§21/§22) and must keep learning for as long as the show
 * runs. The report is a MEASUREMENT AGAINST it, and a measurement that accumulated for ever
 * would mean a resonance that stopped ringing an hour ago still authorised a cut — §47's
 * "resonance disappears -> fade/remove" could never happen. So the report is reset on this
 * cadence and the model is not.
 *
 * Eight times {@link OMX_HRP_RENDER_MIN_FRAMES}, derived rather than typed: a cell needs eight
 * hot windows before it is evidence at all, so an evaluation window has to be several times
 * that or nothing in it could ever ripen. At a quarter-window hop that is ~2.7 s at 48 kHz. */
#define OMX_HRP_LV2_EVAL_PASSES (OMX_HRP_RENDER_MIN_FRAMES * 8u)

/* The controls' travels are declared in data/kernels/hrp.json (OMX_HRP_FAMILY_RANGE_*,
 * OMX_HRP_AMOUNT_RANGE_*, OMX_HRP_BANDS_RANGE_*). Two of them the C can only carry as declared:
 * the family travel IS OmxHrpFamily, and the band ceiling IS the answer set's size. A declaration
 * moved past what the C holds is a compile error here, never a silent clamp. */
_Static_assert(OMX_HRP_FAMILY_RANGE_MIN == OMX_HRP_FAMILY_NONE,
               "the declared family floor is OMX_HRP_FAMILY_NONE");
_Static_assert(OMX_HRP_FAMILY_RANGE_MAX == OMX_HRP_FAMILY_COUNT - 1,
               "the declared family ceiling is the last OmxHrpFamily");
_Static_assert(OMX_HRP_FAMILY_RANGE_DEFAULT >= OMX_HRP_FAMILY_RANGE_MIN &&
                   OMX_HRP_FAMILY_RANGE_DEFAULT <= OMX_HRP_FAMILY_RANGE_MAX,
               "the declared family default lies inside its travel");
_Static_assert(OMX_HRP_BANDS_RANGE_MAX == OMX_HRP_DEFAULT_MAX_AUTO_BANDS,
               "the declared band ceiling is the answer set's size");
_Static_assert(OMX_HRP_BANDS_RANGE_MIN >= 1, "a band budget of zero is not a budget");
_Static_assert(OMX_HRP_BANDS_RANGE_DEFAULT >= OMX_HRP_BANDS_RANGE_MIN &&
                   OMX_HRP_BANDS_RANGE_DEFAULT <= OMX_HRP_BANDS_RANGE_MAX,
               "the declared band default lies inside its travel");

/**
 * @brief What the audio thread hands the analysis for ONE pass: the controls as they stood when
 * the pass was asked for.
 *
 * Fixed size, trivially copyable, no pointers. An LV2 face sends it as its worker message; a CLAP
 * face leaves it in a slot its analysis thread reads once `asked` has moved. Only the worker
 * applies it ({@link omx_hrp_lv2_install_ask}), so the analysis configuration has one writer.
 * Built by {@link omx_hrp_lv2_ask_of}.
 */
typedef struct {
  /** 1 when the pass runs bypassed (its bands walk out), else 0. */
  int bypass;
  /** An {@link OmxHrpFamily}, already inside the declared travel. */
  int family;
  /** The amount as the host wrote it; the worker clamps it ({@link omx_hrp_lv2_set_amount}). */
  float amount;
  /** The band budget, already inside the declared travel. */
  uint32_t max_bands;
} OmxHrpLv2Ask;

/**
 * A correction set crossing from the worker to the audio thread — fixed size, trivially
 * copyable, no pointers. This is the whole of what `work_response()` installs, which is why
 * that RT callback is a struct copy and not a decision.
 */
typedef struct {
  uint32_t count;
  float coeffs[OMX_HRP_LV2_MAX_BANDS][5];
  /** Reported, never acted on by the RT side: what the operator's surface would show. */
  float freq_hz[OMX_HRP_LV2_MAX_BANDS];
  float cut_db[OMX_HRP_LV2_MAX_BANDS];
} OmxHrpLv2Set;

/**
 * One band's persistent identity and ramp state, on the WORKER side only.
 *
 * §47's lifecycle needs a band to be the SAME band across passes, or a cut that merely moved a
 * hertz would leave and re-enter and the ramp would never arrive. The identity is the tracker's
 * voice id plus the harmonic index: voice ids are never reused, so a slot can never silently
 * reattach to a different note.
 */
typedef struct {
  int32_t voice_id;
  uint32_t harmonic;
  float freq_hz;
  /** Where the ramp is now, as a positive cut magnitude in dB. Zero means the slot is free. */
  float applied_db;
} OmxHrpLv2Slot;

/** One instance. No buffer here is owned; {@link omx_hrp_lv2_init} is given all of them. */
typedef struct {
  uint32_t rate;
  /** {@link OmxHrpFamily}, or {@link OMX_HRP_FAMILY_NONE} for the console's wide default. */
  int family;
  /** The operator's strength, 0..1 (§25's scale on what the excess authorises). */
  float amount;
  /** Bands this instance may plant, 1..{@link OMX_HRP_LV2_MAX_BANDS}. */
  uint32_t max_bands;

  /** THE SHARED ENGINE. Not a copy of it, not a variant of it — the same struct the offline
   *  render drives, running the same chain in the same order. */
  OmxHrpRender render;

  /** RT -> worker. The existing primitive (§1: no second RT ring). */
  struct omx_ring ring;
  /** Frames the audio thread has EVER written. Monotonic, and the only thing run() publishes. */
  _Atomic uint64_t written;
  /** Frames the worker has fed to the engine. Monotonic, advanced by the worker alone.
   *
   * THE TWO CURSORS ARE THE WHOLE POINT. An earlier shape had run() reset a "frames pending"
   * counter when it SCHEDULED a job, and the worker then read "the newest hop" whenever the
   * host got round to running it. Both halves were wrong in the same way: how much audio the
   * engine saw, and which audio, depended on thread scheduling. Measured through a real host,
   * that made the correction non-deterministic — five runs over identical audio put -0.68,
   * -4.20, 0.00, -0.70 and -2.42 dB on the same partial in the same half-second, because the
   * model ripened at a different point in the signal each time and hops were double-counted
   * or skipped at random. A cursor pair fixes the audio the engine consumes to exactly the
   * frames nobody has consumed yet, whenever the worker actually runs. */
  _Atomic uint64_t consumed;

  /** Passes asked for by the audio thread, and passes whose answer has been handed back. Each is
   *  advanced by one thread only ({@link omx_hrp_lv2_mark_asked} on the audio thread after a
   *  successful ask, {@link omx_hrp_lv2_mark_answered} on the worker once the set is where the
   *  audio thread will adopt it), so `asked != answered` reads "a pass is in flight" to any
   *  observer, whichever face produced it. Both wrap at 2^32 together. */
  _Atomic uint32_t asked;
  _Atomic uint32_t answered;

  /** The cascade the audio thread runs. Its STATE is never reset by an adopt — see
   *  {@link omx_hrp_lv2_adopt}. */
  OmxHrpCascade live;

  /** Worker-side scratch and state. */
  OmxHrpLv2Slot slot[OMX_HRP_LV2_MAX_BANDS];
  uint32_t passes;
  float *hop;

  int ready;
} OmxHrpLv2;

/** The analysis hop, in frames — how much audio one worker pass consumes. */
static inline uint32_t omx_hrp_lv2_hop(const OmxHrpLv2 *s) {
  return (s && s->ready) ? s->render.cfg.hop : 0u;
}

/**
 * Bind an instance to its rate and the caller's six buffers. Returns 1 when usable, 0 when not.
 *
 * `carry`, `work_re`, `work_im` and `hop` hold `fft_size` floats; `mags` and `residual` hold
 * `fft_size / 2`. The window length is the RTA's own for this rate ({@link
 * omx_rta_fft_size_for}), because §45 forbids hard-coding bin frequencies for one rate and
 * deriving the length is how every other tier already obeys it.
 *
 * A refused init leaves `ready` clear, and every entry below is a no-op on an instance that is
 * not ready: absence is a fact, not a crash. A plugin whose host asked for a rate the engine
 * cannot analyse still passes audio — it just never corrects it.
 */
static inline int omx_hrp_lv2_init(OmxHrpLv2 *s, uint32_t rate, float *carry, float *work_re,
                                   float *work_im, float *mags, float *residual, float *hop) {
  if (!s) return 0;
  memset(s, 0, sizeof(*s));
  if (rate == 0u || !hop) return 0;
  OmxHrpRenderConfig cfg = omx_hrp_render_config_default(rate);
  /* Live, the baseline moves at the LIVE step, not the render's fifth of it: a model that has
   * not converged by the second chorus is no use at a show (§22). The render lowers it because
   * its update count is the whole take's; this shell's is the last few seconds'. */
  cfg.baseline = omx_hrp_baseline_config_default();
  if (!omx_hrp_render_init(&s->render, &cfg, carry, work_re, work_im, mags, residual)) return 0;
  s->rate = rate;
  s->family = OMX_HRP_FAMILY_NONE;
  s->amount = OMX_HRP_DEFAULT_AMOUNT;
  s->max_bands = OMX_HRP_LV2_MAX_BANDS;
  s->hop = hop;
  omx_hrp_cascade_init(&s->live);
  s->ready = 1;
  return 1;
}

/**
 * @brief Floats the analysis buffers of {@link omx_hrp_lv2_init} take at `rate`, as ONE block.
 * @param rate The sample rate the instance will run at, in Hz.
 * @return Four windows (`carry`, `work_re`, `work_im`, `hop`) and two half-windows (`mags`,
 *         `residual`) of the RTA window for `rate`: `4 * fft + 2 * (fft / 2)`, so `5 * fft`.
 * @note Not for the audio thread only because a face sizes its allocation with it; it allocates
 *       nothing and is constant time. Pair it with {@link omx_hrp_lv2_init_block}.
 */
static inline uint32_t omx_hrp_lv2_block_floats(uint32_t rate) {
  const uint32_t fft = omx_rta_fft_size_for(rate);
  return 4u * fft + 2u * (fft / 2u);
}

/**
 * @brief {@link omx_hrp_lv2_init} over one block of {@link omx_hrp_lv2_block_floats} floats.
 * @param s The instance.
 * @param rate The sample rate, in Hz.
 * @param block The caller's block of `omx_hrp_lv2_block_floats(rate)` floats, or NULL.
 * @return 1 when usable, 0 when not (a NULL block is refused as init refuses a NULL buffer).
 * @note The partition lives here, once, for every face: `carry`, `work_re`, `work_im` take the
 *       first three windows, `mags` and `residual` the two half-windows after them, `hop` the
 *       last window. No two buffers overlap and the block ends exactly at the last float.
 */
static inline int omx_hrp_lv2_init_block(OmxHrpLv2 *s, uint32_t rate, float *block) {
  if (!block) return omx_hrp_lv2_init(s, rate, NULL, NULL, NULL, NULL, NULL, NULL);
  const uint32_t fft = omx_rta_fft_size_for(rate), half = fft / 2u;
  return omx_hrp_lv2_init(s, rate, block, block + fft, block + 2u * fft, block + 3u * fft,
                          block + 3u * fft + half, block + 3u * fft + 2u * half);
}

/**
 * Point the search at an instrument family (§16) — the plugin's `family` port, resolved.
 *
 * The family is a BROAD PRIOR and nothing more: it narrows where the detector looks and how
 * many voices it looks for, and it never moves the confidence floor. Changing it mid-show is
 * legal and cheap; it changes the next pass's search and touches neither the learned model nor
 * the corrections already in force, because the model is per NOTE and a note is a note whatever
 * family sounded it.
 */
static inline void omx_hrp_lv2_set_family(OmxHrpLv2 *s, int family) {
  if (!s || !s->ready) return;
  s->family = family;
  s->render.cfg.pitch = omx_hrp_family_search(family);
}

/**
 * @brief The operator's strength and band budget, clamped into their declared travels.
 * @param s The instance; a NULL or not-ready one is left alone.
 * @param amount The strength, held inside OMX_HRP_AMOUNT_RANGE_*; a NaN reads as its minimum.
 * @param max_bands The band budget, held inside OMX_HRP_BANDS_RANGE_MIN and the answer set's size.
 * @note Worker thread: it writes what a pass reads. A face calls it through
 *       {@link omx_hrp_lv2_install_ask}.
 */
static inline void omx_hrp_lv2_set_amount(OmxHrpLv2 *s, float amount, uint32_t max_bands) {
  if (!s || !s->ready) return;
  if (!(amount >= (float)OMX_HRP_AMOUNT_RANGE_MIN)) amount = (float)OMX_HRP_AMOUNT_RANGE_MIN;
  if (amount > (float)OMX_HRP_AMOUNT_RANGE_MAX) amount = (float)OMX_HRP_AMOUNT_RANGE_MAX;
  s->amount = amount;
  if (max_bands < (uint32_t)OMX_HRP_BANDS_RANGE_MIN) max_bands = (uint32_t)OMX_HRP_BANDS_RANGE_MIN;
  if (max_bands > OMX_HRP_LV2_MAX_BANDS) max_bands = OMX_HRP_LV2_MAX_BANDS;
  s->max_bands = max_bands;
}

/**
 * @brief The controls as a face read them, resolved into one pass's snapshot.
 * @param bypass The bypass word: above one half is bypassed.
 * @param family The family word, a float even though the control is an integer.
 * @param amount The amount word, carried as written.
 * @param bands The band budget word, a float even though the control is an integer.
 * @return The snapshot: the family and the band budget each held inside its declared travel and
 *         rounded half away from zero by omx_port_int(), a non-finite word reading as the
 *         declared default.
 * @note Audio thread, constant time, no state read or written. The amount's clamp is the
 *       worker's, in {@link omx_hrp_lv2_install_ask}, so a pass sees exactly what the host wrote.
 */
static inline OmxHrpLv2Ask omx_hrp_lv2_ask_of(float bypass, float family, float amount, float bands) {
  OmxHrpLv2Ask a;
  a.bypass = bypass > 0.5f ? 1 : 0;
  a.family = omx_port_int(family, OMX_HRP_FAMILY_RANGE_MIN, OMX_HRP_FAMILY_RANGE_MAX,
                          OMX_HRP_FAMILY_RANGE_DEFAULT);
  a.amount = amount;
  a.max_bands = (uint32_t)omx_port_int(bands, OMX_HRP_BANDS_RANGE_MIN, OMX_HRP_BANDS_RANGE_MAX,
                                       OMX_HRP_BANDS_RANGE_DEFAULT);
  return a;
}

/**
 * @brief Install the controls a pass was asked with, at the start of the pass.
 * @param s The instance; a NULL or not-ready one is left alone.
 * @param a The pass's snapshot; NULL installs nothing.
 * @note Worker thread only. The one writer of the analysis configuration (`family`, the search
 *       it points at, `amount`, `max_bands`), so the audio thread never writes what a pass reads.
 *       The family's search is re-pointed only when the family changed.
 */
static inline void omx_hrp_lv2_install_ask(OmxHrpLv2 *s, const OmxHrpLv2Ask *a) {
  if (!s || !s->ready || !a) return;
  if (a->family != s->family) omx_hrp_lv2_set_family(s, a->family);
  omx_hrp_lv2_set_amount(s, a->amount, a->max_bands);
}

/**
 * @brief Count one pass as asked for.
 * @param s The instance; NULL is a no-op.
 * @note Audio thread only, after the pass was successfully handed to the worker. One atomic add
 *       with release order, so the snapshot written before it is visible to whoever reads it.
 */
static inline void omx_hrp_lv2_mark_asked(OmxHrpLv2 *s) {
  if (s) atomic_fetch_add_explicit(&s->asked, 1u, memory_order_release);
}

/**
 * @brief Count one pass as answered.
 * @param s The instance; NULL is a no-op.
 * @note Worker thread only, once the pass's set is where the audio thread will adopt it. One
 *       atomic add with release order, so the set written before it is visible to the adopter.
 */
static inline void omx_hrp_lv2_mark_answered(OmxHrpLv2 *s) {
  if (s) atomic_fetch_add_explicit(&s->answered, 1u, memory_order_release);
}

/**
 * @brief Is a pass in flight: asked for and not yet answered?
 * @param s The instance; NULL has none.
 * @return 1 when `asked != answered`, else 0.
 * @note Any thread, constant time: two acquire loads. The same reading in every face.
 */
static inline int omx_hrp_lv2_in_flight(const OmxHrpLv2 *s) {
  if (!s) return 0;
  return atomic_load_explicit(&s->asked, memory_order_acquire) !=
         atomic_load_explicit(&s->answered, memory_order_acquire);
}

/**
 * @brief Drop every correction in force at once: the cascade empty and its state zero.
 * @param s The instance; NULL is a no-op.
 * @note Audio thread, constant time. The half of {@link omx_hrp_lv2_restart} that may run while a
 *       pass is in flight, since the worker never touches the live cascade. The worker's slots
 *       still hold their depths, so a pass answered after a clear plants them again.
 */
static inline void omx_hrp_lv2_clear(OmxHrpLv2 *s) {
  if (!s) return;
  omx_hrp_cascade_init(&s->live);
}

/**
 * @brief A new stream, at every activation, keeping the learned model.
 * @param s The instance; NULL is a no-op.
 * @note No correction in force, every band slot free, and both ring cursors back at zero, so no
 *       pass ever analyses a hop that straddles a stop. The baseline, the current report, the
 *       pass counter and the asked/answered counters are kept: a transport stop is not a new
 *       instrument. Never while a pass is in flight ({@link omx_hrp_lv2_in_flight}): the caller
 *       holds the worker idle (LV2: activate is in the instantiation class; CLAP: only when no
 *       pass is in flight). Constant time, no allocation.
 */
static inline void omx_hrp_lv2_restart(OmxHrpLv2 *s) {
  if (!s) return;
  omx_hrp_lv2_clear(s);
  memset(s->slot, 0, sizeof(s->slot));
  atomic_store_explicit(&s->written, 0u, memory_order_relaxed);
  atomic_store_explicit(&s->consumed, 0u, memory_order_relaxed);
}

/**
 * THE AUDIO CALLBACK'S WHOLE SHARE OF HRP. One memcpy into the ring, one biquad cascade.
 *
 * `in` and `out` may be the same buffer (LV2 hosts do connect a port in place). With no live
 * section the frames are copied and nothing else happens, byte for byte — which is what makes
 * a racked-but-idle corrector safe to leave in the chain, and it is the reason the default
 * control values ship the plugin inert.
 *
 * Note what is NOT here: no FFT, no allocation, no lock, no logging, no decision that depends
 * on what the audio contains. §2, and the reason it is a law rather than a preference.
 */
#define OMX_CONTRACT_STAGE "hrp/lv2-run"
static inline void omx_hrp_lv2_run(OmxHrpLv2 *s, const float *in, float *out, uint32_t n) {
  if (!in || !out || n == 0u) return;
  if (!s || !s->ready) {
    if (out != in) memcpy(out, in, (size_t)n * sizeof(float));
    /* R-056/laws: bypass-identity -- accepted 2026-09-17,
     * the contracts proposals notes. Not ready is not "processed a
     * little" -- it is the identity, byte for byte. */
    OMX_POST(out == in || memcmp(out, in, (size_t)n * sizeof(float)) == 0, "bypass-identity");
    return;
  }
  omx_ring_write(&s->ring, in, n);
  atomic_fetch_add_explicit(&s->written, n, memory_order_release);
  omx_hrp_cascade_apply(&s->live, in, out, n);
}
#undef OMX_CONTRACT_STAGE

/**
 * Is there a hop's worth of new audio for the worker, and is there any reason to look at it?
 *
 * Called from `run()`, so it decides in constant time and never blocks. Two reasons to say yes:
 * the corrector is engaged, or it is bypassed but still has corrections to walk out of the
 * audio (§49: "HRP corrections must be removed/faded" — a bypass that dropped them at depth
 * would click, which is the same defect the ramp exists for). A bypassed instance whose
 * cascade has already reached silence asks for nothing at all, so it costs a host exactly one
 * comparison per buffer.
 *
 * A HOST WITH NO WORKER never calls this and never analyses: the plugin then passes audio
 * through an empty cascade for ever. That is the honest reading of an absent feature, and it
 * is the only alternative §2 permits — analysing on the audio thread instead is forbidden
 * outright, so it is not on the table as a fallback.
 */
static inline int omx_hrp_lv2_due(const OmxHrpLv2 *s, int bypass) {
  if (!s || !s->ready) return 0;
  const uint64_t written = atomic_load_explicit(&s->written, memory_order_acquire);
  const uint64_t consumed = atomic_load_explicit(&s->consumed, memory_order_acquire);
  if (written - consumed < (uint64_t)s->render.cfg.hop) return 0;
  if (!bypass) return 1;
  return omx_hrp_cascade_live(&s->live) > 0u;
}

/* ---- the worker side ------------------------------------------------------------------ */

/** Find the slot already holding this (voice, harmonic), or -1. */
static inline int omx_hrp_lv2_slot_of(const OmxHrpLv2 *s, int32_t voice_id, uint32_t harmonic) {
  for (uint32_t i = 0; i < OMX_HRP_LV2_MAX_BANDS; i++)
    if (s->slot[i].applied_db > 0.0f && s->slot[i].voice_id == voice_id &&
        s->slot[i].harmonic == harmonic)
      return (int)i;
  return -1;
}

/** Move `now` toward `target` by at most one step. */
static inline float omx_hrp_lv2_ramp(float now, float target) {
  const float step = OMX_HRP_GAIN_STEP_DB;
  if (target > now) return (target - now > step) ? now + step : target;
  if (target < now) return (now - target > step) ? now - step : target;
  return now;
}

/**
 * ONE ANALYSIS PASS, on the host's worker thread. Never called from `run()`.
 *
 * Consumes one hop of the newest audio, runs the shared chain over it, re-decides what the
 * channel should be cutting, walks every band one step toward that answer, and writes the
 * resulting coefficient set into `out` for `work_response()` to install.
 *
 * LATE IS FINE; LATE IS NOT WRONG (§15). The hop read is the ring's NEWEST hop, so a worker
 * that fell behind SKIPS the audio it missed rather than replaying a backlog — the same
 * latest-wins discipline the console's telemetry runs under. A skipped window costs one
 * measurement out of an evaluation window's sixty-four; a backlog would cost the model its
 * grip on what is sounding NOW, which is the only thing it is for.
 *
 * `bypass` does not stop the ramp, it retargets it: every slot walks to exactly 0 dB and the
 * cascade arrives at the provable parked identity, from which removal is bit-identical (§49).
 */
static inline void omx_hrp_lv2_analyse(OmxHrpLv2 *s, int bypass, OmxHrpLv2Set *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  if (!s || !s->ready) return;

  const uint32_t hop = s->render.cfg.hop;
  const uint64_t written = atomic_load_explicit(&s->written, memory_order_acquire);
  uint64_t consumed = atomic_load_explicit(&s->consumed, memory_order_acquire);
  if (written - consumed < (uint64_t)hop) return; /* nothing whole to take yet */

  /* The oldest hop nobody has consumed, addressed as an offset back from the newest sample —
   * so WHEN the worker runs changes nothing about WHICH audio it analyses. */
  uint64_t back = written - (consumed + (uint64_t)hop);
  if (back + (uint64_t)hop > (uint64_t)OMX_RTA_RING_SIZE) {
    /* §15: late is fine, a BACKLOG is not. The writer has lapped the reader, so the audio in
     * between is gone — skip to the newest whole hop rather than replaying what is left of a
     * queue. Latest-wins, and it is the one place this shell drops audio, on purpose. */
    consumed = written - (uint64_t)hop;
    back = 0u;
  }
  omx_ring_read_at(&s->ring, s->hop, hop, (uint32_t)back);
  atomic_store_explicit(&s->consumed, consumed + (uint64_t)hop, memory_order_release);
  omx_hrp_render_feed(&s->render, s->hop, hop, OMX_HRP_RENDER_LIVE);

  if (++s->passes >= OMX_HRP_LV2_EVAL_PASSES) {
    /* Forget the MEASUREMENT, keep the MODEL — see OMX_HRP_LV2_EVAL_PASSES. */
    omx_hrp_render_reset_report(&s->render);
    s->passes = 0u;
  }

  /* Every stable voice's correctable partials, as candidates — §5's voice SET, never "the" f0. */
  OmxHrpCandidate cand[OMX_HRP_BASELINE_NOTES > 0 ? OMX_HRP_MAX_VOICES * OMX_HRP_HARMONICS : 1];
  uint32_t n = 0u;
  if (!bypass) {
    for (int note = 0; note < OMX_HRP_BASELINE_NOTES && n < (uint32_t)(sizeof(cand) / sizeof(cand[0]));
         note++) {
      for (uint32_t k = 1u; k < OMX_HRP_HARMONICS; k++) { /* from H2: the fundamental is the note */
        const OmxHrpRenderCell *c = &s->render.report.cell[note][k];
        const float cut = omx_hrp_cut_db(omx_hrp_render_excess_db(c), s->amount, OMX_HRP_MAX_CUT_DB);
        if (!(cut > 0.0f)) continue;
        const float f0 = omx_hrp_render_f0_hz(c);
        if (!(f0 > 0.0f)) continue;
        if (n >= (uint32_t)(sizeof(cand) / sizeof(cand[0]))) break;
        cand[n].freq_hz = f0 * (float)(k + 1u);
        cand[n].cut_db = cut;
        cand[n].score = omx_hrp_resonance_score(cut, omx_hrp_render_voice_confidence(c),
                                                omx_hrp_render_confidence(c));
        /* The note IS the voice's identity here: the model is per note, and a note is the same
         * band across passes whichever tracker slot happened to be carrying it. */
        cand[n].voice_id = note;
        cand[n].harmonic = k + 1u;
        n++;
      }
    }
    n = omx_hrp_select(cand, n, OMX_HRP_SELECT_COLLISION_CENTS, s->max_bands);
  }

  /* Retarget every slot: a selected partial keeps (or takes) its slot, everything else walks
   * out. §47's lifecycle, as arithmetic. */
  float target[OMX_HRP_LV2_MAX_BANDS];
  int taken[OMX_HRP_LV2_MAX_BANDS];
  for (uint32_t i = 0; i < OMX_HRP_LV2_MAX_BANDS; i++) {
    target[i] = 0.0f;
    taken[i] = 0;
  }
  for (uint32_t c = 0; c < n; c++) {
    int i = omx_hrp_lv2_slot_of(s, cand[c].voice_id, cand[c].harmonic);
    if (i < 0) {
      for (uint32_t j = 0; j < OMX_HRP_LV2_MAX_BANDS; j++)
        if (!taken[j] && s->slot[j].applied_db <= 0.0f) { i = (int)j; break; }
    }
    if (i < 0) continue; /* every slot is busy walking something else out: next pass */
    taken[i] = 1;
    s->slot[i].voice_id = cand[c].voice_id;
    s->slot[i].harmonic = cand[c].harmonic;
    s->slot[i].freq_hz = cand[c].freq_hz;
    target[i] = cand[c].cut_db;
  }

  for (uint32_t i = 0; i < OMX_HRP_LV2_MAX_BANDS; i++) {
    s->slot[i].applied_db = omx_hrp_lv2_ramp(s->slot[i].applied_db, target[i]);
    if (!(s->slot[i].applied_db > 0.0f)) {
      /* Arrived at exactly zero: the slot is free and plants nothing. */
      s->slot[i].applied_db = 0.0f;
      continue;
    }
    const uint32_t b = out->count;
    omx_hrp_peaking_coeffs(s->slot[i].freq_hz, OMX_HRP_BAND_Q, -s->slot[i].applied_db, s->rate,
                           out->coeffs[b]);
    out->freq_hz[b] = s->slot[i].freq_hz;
    out->cut_db[b] = s->slot[i].applied_db;
    out->count = b + 1u;
  }
}

/**
 * @brief Is a pass due under the controls a snapshot holds?
 * @param s The instance.
 * @param a The snapshot; NULL is never due.
 * @return {@link omx_hrp_lv2_due} with the snapshot's bypass.
 * @note Audio thread, constant time. A face asks with the snapshot it will send, never reaching
 *       into it.
 */
static inline int omx_hrp_lv2_wants(const OmxHrpLv2 *s, const OmxHrpLv2Ask *a) {
  return a ? omx_hrp_lv2_due(s, a->bypass) : 0;
}

/**
 * @brief One whole pass on the worker: install the snapshot, then analyse into `out`.
 * @param s The instance.
 * @param a The snapshot the pass was asked with; NULL installs nothing and analyses bypassed.
 * @param out Where the pass's correction set is written.
 * @note Worker thread only. The non-RT half of the shell as one call, the same in every face (an
 *       LV2 `work()`, a CLAP face's analysis thread). The face marks the pass answered once `out`
 *       is where the audio thread will adopt it.
 */
static inline void omx_hrp_lv2_answer(OmxHrpLv2 *s, const OmxHrpLv2Ask *a, OmxHrpLv2Set *out) {
  omx_hrp_lv2_install_ask(s, a);
  omx_hrp_lv2_analyse(s, a ? a->bypass : 1, out);
}

/**
 * Install a correction set, on the audio thread. A fixed-size copy and nothing else.
 *
 * THE FILTER STATE IS NOT RESET. §48 forbids interpolating coefficients in the biquad, and
 * DF-I is chosen precisely so a coefficient swap needs no interpolation: its state is the raw
 * input/output HISTORY, which stays meaningful under a retune (mix_dsp.h says so where the
 * kernel is defined). Zeroing it here would manufacture the discontinuity the ramp exists to
 * avoid. What bounds the move is the ramp, at the control level, which is where §48 puts it.
 */
static inline void omx_hrp_lv2_adopt(OmxHrpLv2 *s, const OmxHrpLv2Set *set) {
  if (!s || !s->ready || !set) return;
  uint32_t n = set->count;
  if (n > OMX_HRP_LV2_MAX_BANDS) n = OMX_HRP_LV2_MAX_BANDS;
  for (uint32_t b = 0; b < n; b++) {
    memcpy(s->live.coeffs[b], set->coeffs[b], 5u * sizeof(float));
    s->live.enabled[b] = 1u;
  }
  for (uint32_t b = n; b < s->live.count && b < OMX_EQ_MAX_BANDS; b++) s->live.enabled[b] = 0u;
  if (n > s->live.count) s->live.count = n;
}

#endif /* OMX_HRP_LV2_H */
