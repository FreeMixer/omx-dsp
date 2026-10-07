/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * hrp_peaking_corpus.h — GENERATED. The frozen contract between the two languages that design
 * HRP's one curve.
 *
 * HRP plants exactly one section shape, the peaking bell, and two implementations design it:
 * `@freemixer/core`'s `rbjCoeffs('peaking', …)`, which the console and the `omx-hrp` CLI reach
 * across N-API, and {@link omx_hrp_peaking_coeffs} in hrp_correct.h, which is what a live LV2
 * insert links because there is no TypeScript in a foreign host's process. Two languages, one
 * curve — so the two must be held equal by something mechanical.
 *
 * THIS IS THAT SOMETHING. Every row below was computed by a THIRD transcription of the design
 * (the generator in this commit's message), so a mistake shared by both implementations would
 * still show. Both sides check every row:
 *
 *   - `hrp_correct.test.c` runs {@link omx_hrp_peaking_coeffs} over it, and separately
 *     recomputes the design in double precision, so the corpus cannot be quietly wrong.
 *   - `packages/audio-engine/src/hrp-native-twins.test.ts` PARSES THIS FILE and runs
 *     `rbjCoeffs` over the same rows.
 *
 * Editing either implementation's arithmetic turns one of those red. Regenerating this file
 * to make a red go green is changing the contract, and is a decision, not a fix.
 *
 * THE DESIGN IS MATCHED, NOT BILINEAR (operator ruling, 2026-09-14), which is why this file
 * was regenerated and why it is no longer called `hrp_rbj_corpus.h`. The curve is the analogue
 * peaking prototype placed by the matched-Z map `z = exp(sT)`; the RBJ cookbook it replaced
 * was a bilinear image of the same prototype, 1.28 dB low at 20 kHz on a +12 dB bell at 16 kHz
 * Q 2 at the 96 kHz the graph runs.
 *
 * Tolerance is float32's, not double's: the C returns `float` and the TS returns `double`,
 * so the two agree to about seven significant figures and no further.
 */
#ifndef OMX_HRP_PEAKING_CORPUS_H
#define OMX_HRP_PEAKING_CORPUS_H

#include <stdint.h>

typedef struct {
  double freq_hz;
  double q;
  double gain_db;
  uint32_t rate;
  /** {b0, b1, b2, a1, a2}, normalised by a0. */
  double coeffs[5];
  const char *why;
} OmxHrpPeakingRow;

/* BEGIN CORPUS (parsed by hrp-native-twins.test.ts — one row per line, do not reflow) */
static const OmxHrpPeakingRow OMX_HRP_PEAKING_CORPUS[] = {
  { 40, 5, -3, 44100, { 0.99980224017930297, -1.9986136447772092, 0.99884386151280924, -1.9986137972367095, 0.99864625415161223 }, "the planted bell" },
  { 110, 5, -3, 44100, { 0.9994564380489579, -1.9960352951658389, 0.99682401753094474, -1.9960364467710163, 0.99628160718507996 }, "the planted bell" },
  { 440, 5, -3, 44100, { 0.99783097864889847, -1.9812913151536444, 0.98735988135904673, -1.9813096370993291, 0.98520918195362983 }, "the planted bell" },
  { 867.20000000000005, 5, -3, 44100, { 0.99573835879875305, -1.955962609299176, 0.97524898251802283, -1.9560332549841213, 0.97105798700172108 }, "the planted bell" },
  { 2000, 5, -3, 44100, { 0.990252546589937, -1.8561626818410404, 0.94388903399961932, -1.856530915964977, 0.93450981471349315 }, "the planted bell" },
  { 7500, 5, -3, 44100, { 0.9649317474632666, -0.85496801643103426, 0.80612565480084009, -0.85960295094009598, 0.7756923367731684 }, "the planted bell" },
  { 40, 5, -3, 48000, { 0.99981830385828907, -1.9987286504170099, 0.99893774501032029, -1.9987287791153949, 0.99875617756699453 }, "the planted bell" },
  { 110, 5, -3, 48000, { 0.99950056990626535, -1.9963752653762992, 0.99708166772137541, -1.996376237595737, 0.99658320984707849 }, "the planted bell" },
  { 440, 5, -3, 48000, { 0.99800669195901182, -1.9830934830444296, 0.98838055964134741, -1.9831089581417207, 0.98640272669765039 }, "the planted bell" },
  { 867.20000000000005, 5, -3, 48000, { 0.9960826034196324, -1.9606182363076043, 0.97723562119020724, -1.9606779418689757, 0.97337793017121099 }, "the planted bell" },
  { 2000, 5, -3, 48000, { 0.99103386245527825, -1.8732839007453126, 0.9483212888586483, -1.8735956467185417, 0.93966689728715569 }, "the planted bell" },
  { 7500, 5, -3, 48000, { 0.96762708903174122, -0.99507737020909315, 0.82027465123984178, -0.99904260135637013, 0.79186697141886009 }, "the planted bell" },
  { 40, 5, -3, 96000, { 0.99990913975413898, -1.9993710113358356, 0.99946872333758807, -1.9993710435204624, 0.99937789527635379 }, "the planted bell" },
  { 110, 5, -3, 96000, { 0.99975019291303557, -1.9982381118463679, 0.99853970694170835, -1.9982383551098852, 0.99829014311826125 }, "the planted bell" },
  { 440, 5, -3, 96000, { 0.99900187572374299, -1.9923477801523977, 0.99417233623500711, -1.9923516623427535, 0.99317809414910607 }, "the planted bell" },
  { 867.20000000000005, 5, -3, 96000, { 0.99803560128852564, -1.9833851542972465, 0.98854854302280715, -1.9834001840030071, 0.98659917401709341 }, "the planted bell" },
  { 2000, 5, -3, 96000, { 0.99548672734037236, -1.9524381241695314, 0.97379822156256279, -1.9525173469209627, 0.96936417165436639 }, "the planted bell" },
  { 7500, 5, -3, 96000, { 0.98338618194235039, -1.6659039178735138, 0.90541927952162182, -1.6669675318464061, 0.88986907543686444 }, "the planted bell" },
  { 40, 5, -3, 192000, { 0.99995456683301265, -1.9996871779927547, 0.99973432436593845, -1.9996871860401408, 0.99968889924633741 }, "the planted bell" },
  { 110, 5, -3, 192000, { 0.99987507343994309, -1.999131692369907, 0.99926957151313489, -1.9991317532118713, 0.99914470579504211 }, "the planted bell" },
  { 440, 5, -3, 192000, { 0.99950056990626535, -1.9963752653762992, 0.99708166772137541, -1.996376237595737, 0.99658320984707849 }, "the planted bell" },
  { 867.20000000000005, 5, -3, 192000, { 0.99901637280556221, -1.9924706107814205, 0.99425684449047513, -1.9924743810457983, 0.99327698756041527 }, "the planted bell" },
  { 2000, 5, -3, 192000, { 0.9977357877558235, -1.9802939540527058, 0.98680718218286778, -1.980313918442931, 0.98456293432891639 }, "the planted bell" },
  { 7500, 5, -3, 192000, { 0.99158717740613966, -1.8848315258571915, 0.95146697847189821, -1.8851060907530228, 0.94332872077386909 }, "the planted bell" },
  { 1000, 5, 0, 48000, { 1, -1.9572715383840324, 0.97415978471404419, -1.9572715383840324, 0.97415978471404419 }, "the cut travel" },
  { 1000, 5, -0.5, 48000, { 0.99924994968221903, -1.9565205554620988, 0.97415242666409974, -1.9565335344160937, 0.97341535530031364 }, "the cut travel" },
  { 1000, 5, -2.8300000000000001, 48000, { 0.99574422419405895, -1.952733991909664, 0.97383913179868298, -1.9528085743902763, 0.96965793847335424 }, "the cut travel" },
  { 1000, 5, -6, 48000, { 0.99086161797460037, -1.9467299957383071, 0.97266614930143647, -1.9468974964007595, 0.96369526793848936 }, "the cut travel" },
  { 1000, 0.5, -6, 48000, { 0.91579043338230071, -1.662346164344068, 0.76085878913807869, -1.6765684214121903, 0.69087147958850192 }, "a wide bell" },
  { 1000, 12, -6, 48000, { 0.99617180617115564, -1.9677011821081769, 0.98850850678539259, -1.9677305767436224, 0.98470970759199372 }, "a narrow bell" },
  { 1000, 5, 6, 48000, { 1.0092226622362053, -1.9648530744185773, 0.97258310389331537, -1.9646840289540908, 0.98163672066503405 }, "a boost the designer must still get right" },
  { 30000, 5, -6, 48000, { 0.83113034872507319, 1.3310026655913108, 0.53294381220601039, 1.2830372979535931, 0.41203952856880083 }, "above Nyquist -> clamped to rate*0.4995" },
  { 0.25, 5, -6, 48000, { 0.99999077700087335, -1.9999630032327296, 0.99997224336626911, -1.9999630034029015, 0.99996302053731467 }, "below 1 Hz -> clamped to 1" },
  { 1000, 0, -6, 48000, { 0.50120959731915937, -0.50111693167996663, 2.844454848777187e-41, -0.99990733436080725, 4.9961135477753125e-81 }, "Q = 0 -> 1e-3, never a division by zero" },
  { 1000, -4, -6, 48000, { 0.50120959731915937, -0.50111693167996663, 2.844454848777187e-41, -0.99990733436080725, 4.9961135477753125e-81 }, "Q < 0 -> 1e-3" },
};
/* END CORPUS */

#define OMX_HRP_PEAKING_CORPUS_ROWS 35u

#endif /* OMX_HRP_PEAKING_CORPUS_H */
