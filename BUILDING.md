<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# Building omx-dsp

Needs a C11 compiler, GNU make, binutils, awk, sed, grep and diffutils.

| command | does |
|---|---|
| `make` | `build/libomxdsp.a`, `build/libomxdsp-contracts.a` and `build/libomxdsp-tsan.a` |
| `make test` | the whole suite: every primitive's and kernel's oracle at every declared rate with contracts on, the negative, perturbation and thread arms, the writable-data and doc checks, and the golden digests |
| `make test-fx` | the effect kernels' oracles and golden digests alone |
| `make consumer` | the stateful stages built the way a consumer builds them (release flags, `-Werror`, states on the stack that only their init or settle writes) and run |
| `make test-analysis` | the feedback detector's, the RTA ring and spectrum's and HRP's oracles (the HRP render and live core among them) at the nine oracle rates (32 to 192 kHz), their no-allocation arm, golden digests and one sabotage per kernel; the room analysis, Farina sweep and alignment fit against closed forms at the nine rates and against core's TypeScript (the parity table), and one sabotage per kernel that both must catch; alone |
| `make test-engine-kernels` | the golden digests of the sample-rate converter, the generators and the analyser's readings at the nine oracle rates, which the engine's own copies wrote, and a break in each that must move its own digests and no other's |
| `make install PREFIX=/usr LIBDIR=/usr/lib64 DESTDIR=…` | headers, the three archives and `omxdsp.pc`, `omxdsp-contracts.pc`, `omxdsp-tsan.pc` |
| `make flavours` | each archive carries its flavour, and a contracts consumer reads a violation raised inside the compiled code only through the contracts archive |
| `make docs` | the API reference with doxygen |
| `make exports` | `build/omx-dsp-exports.json`: every public function and macro with its signature, its body as normalised tokens, its distinctive literals, maths calls, inline kernels and `@twin` names; `make install` puts it in `$(DATADIR)/omxdsp/` beside `omx-dsp-copy-check` in `$(BINDIR)` |
| `make copy-check` | `tools/omx-dsp-copy-check.sh --self-test`: planted copies in a scratch consumer (an exact copy, a renamed one, a TypeScript twin, a literal copy, inline kernels) must be found, a legitimate caller, boilerplate, tests and fakes must pass, and a stale or reasonless allowlist entry must fail; then each rule is broken in turn and the self-test must go red (part of `make test`) |
| `make lint` | the doc check, the source scan, the log10f guard, tools/contract-single-source.sh (no name omx-contract defines is defined again here) and tools/face-conformance.sh (every instance face's resolve takes its kernel's contract controls by name, order and kind); both check their own sabotage |
| `make test-tsan` | the thread arm under ThreadSanitizer, where the toolchain has it |
| `make engine-kernels-identity OPENMIXER=<checkout>` | renders those digests again through the copies OpenMixer's engine still carries (`mix_src.h`, `mix_dsp.h`), and checks that each of its sabotages goes red; a desk check, not run in CI |
| `make test-mix` | the mix module's golden digests at the nine oracle rates (`test/golden/mix.sha256`, written by the engine's own copies), and a break in each kernel and in each of the bus stages' contract checks that must turn its arm red; alone |
| `make mix-engine-identity OPENMIXER=<checkout>` | renders the mix digests again through each set of copies OpenMixer's engine still carries (the bus stages' `omx_mix_strip`, `omx_bus_out_gain` and `omx_bus_out_stage` in `mix_dsp.h`, under the same names as `mix/omx_bus_stage.h`), and checks that each of its sabotages goes red; a desk check, not run in CI |
| `make strip-input-identity OMX_PLUGINS=<checkout>` | renders omx-plugins' channel strip's own input stage (`plugins/omx-strip/omx_strip.h`) and requires the trim face's golden digests, then its sabotage; a desk check, not run in CI |
| `make analysis-engine-identity OPENMIXER=<checkout>` | builds the analysis golden against the engine's own `mix_dsp.h`, `hrp_render.h` and `hrp_lv2.h` and requires the committed digests, then its sabotage; a desk check, not run in CI |
| `make room-ts-golden OPENMIXER=<checkout>` | runs core's room analysis and alignment fit from an OpenMixer checkout under node (22.6 or later) and requires the committed parity table `test/analysis/fixtures/room_ts.h`; a desk check, not run in CI |
| `make eq-sabotage` | the EQ bank, head-filter and drawn-curve arms (`eq-curve`, `eq-design-ts`) against nine sabotaged copies of the headers; each must fail both arms (part of `make test`) |
| `make golden-write` | rewrites every kernel's `test/golden/<kernel>.sha256`; only in a commit that bumps the minor version or adds a kernel |

`make -jN test` is supported. `tools/kernels-gen.sh` runs once, as the Make rule that produces
`build/kernels-suite.inc.c` and `build/kernels-perturb.inc.c`; every target that reads them lists
them as a prerequisite and no recipe or script calls the generator into `build/`. `make test-fx`
is one target per kernel, instance, oracle, golden and perturbation script (`fx-run-*`), so `-j`
runs them side by side; the set run and each assertion are the same as serially.
`make gen-once` (`test/gen-once.sh`, part of `make test`) counts the generations under `-j8` and
must see exactly one; it then re-adds the generator call to a recipe in a scratch copy of the
Makefile, which must turn it red.

Consumers compile the kernels through `pkg-config --cflags omxdsp`, which carries
`-ffp-contract=off`: the kernels are inline, and without it an architecture with fused
multiply-add would round differently from one without.

## Flavours

The compiled part of the library (the oversampler, the FBS detector, the room analysis, the Farina
sweep and the alignment fit; everything else is inline in the headers) is built once per flavour
into `build/`:

- `libomxdsp.a`: `CFLAGS` only, contracts compiled out.
- `libomxdsp-contracts.a`: objects in `build/contracts/`, with `-DOMX_CONTRACTS`.
- `libomxdsp-tsan.a`: objects in `build/tsan-lib/`, with `-DOMX_CONTRACTS -fsanitize=thread`.

Each has a pkg-config file of the same name (`omxdsp`, `omxdsp-contracts`, `omxdsp-tsan`) whose
cflags carry the flavour's flags, so the inline headers and the archive are always built the same
way. The suite, the thread arm and the contract-checked kernel tests link `libomxdsp-contracts.a`,
`make test-tsan` links `libomxdsp-tsan.a`, and the release-flag kernel tests link `libomxdsp.a`.
`make flavours` (`tools/flavour-check.sh`, part of `make test`) checks each archive's symbols and
builds `test/omxdsp_flavour.c`, a contracts consumer that feeds a NaN to the compiled oversampler:
against `libomxdsp-contracts.a` it must find the violation in its own ledger, and against
`libomxdsp.a` it must fail.

## GCC 12.2 on arm64

Debian bookworm's GCC 12.2 on arm64 (Raspberry Pi OS, Zynthian) crashes with an internal compiler
error in `vect_transform_reduction` on a loop that keeps a `double` running maximum or minimum with
`fmax`/`fmin` over values widened from `float`. Write such a reduction as a comparison instead,
`if (!(e <= worst)) worst = e;`, which also carries a NaN into the result. `make lint` refuses the
shape on any host (`tools/reduction-check.sh`), and CI builds the suite on bookworm arm64.

## Golden digests and libm

A golden digest covers the kernel's output bit for bit, so it also covers every libm call the
kernel makes per sample. A libm's `log10f` is not one function: glibc 2.36 (debian:bookworm)
misrounds 1.39% of the non-negative floats, 62 225 of them by 2 ulp, where glibc 2.41 (debian:trixie)
and 2.43 (Fedora 44) round every one correctly, since 2.41 took `log10f` from CORE-MATH. The limiter's
48 kHz digest (the render of FreeMixer/omx-dsp#21) read `5fe47250…` on bookworm against `69a449ce…`
on 2.41 and 2.43. The compiler played no part: the same binary flips with the glibc it runs on.

Production calls the libm: `omx_lin_to_db()` is `20·log10f(max(lin, 1e-9))`, about 2 ns per call
against 10 ns for a libm-free `log10f` on glibc 2.43. The goldens do not: `omx_log10f()` lives in
`test/support/log10f_cr.h`, a correctly rounded `log10` in IEEE double arithmetic (it needs
`FLT_EVAL_METHOD == 0` and `-ffp-contract=off`, which the Makefile's `FPFLAGS` carry), and every
effect-kernel golden TU is compiled with `-include test/support/log10f_subst.h`, which defines `log10f` as
`omx_log10f`. The kernels are static inline, so the substitution reaches them, and every kernel's
golden is an exact digest on every toolchain. Two guards hold it: `golden.h` refuses to compile a
golden without the substitution and the golden driver exits 2 when `log10f` there is not
`omx_log10f`, and `tools/log10f-guard.sh` (part of `make lint`) fails when `omx_log10f` appears
under `include/` or `src/`. `make check-log10f` compares `omx_log10f` with the host libm's `log10f`
on all 2³¹ non-negative floats; against glibc 2.43 none differ. `test/kernels/units.c` tests it
against fixed bit patterns, including `0x0efeee7a`, 7.8e-10 ulp from a float midpoint, which is
answered from its exact value. The analysis golden (`test/analysis/golden.test.c`), whose HRP
code calls `log10f` through the compiled headers, links with `-Wl,--wrap=log10f` and answers each
call with `omx_log10f`; it exits 2 if the wrap was never reached.

On glibc older than 2.41 a production build may therefore differ from the golden by up to 2 ulp of
a dB value in the units that call `log10f`. That is accepted.

The limiter's other per-sample call, `powf` in `omx_db_to_lin()`, is the same implementation in
every glibc since 2.28 and returned the same bits for equal arguments on 2.36, 2.41 and 2.43. A
kernel that adds a per-sample libm call adds it to the RT-safe allowlist named in its `@note`, and
checks that the goldens agree across glibc versions, or uses a libm-free word.

## Rates outside the declaration

The console declares six rates (`OMX_DECLARED_RATES`), and the kernels' contracts refuse any other
with a `rate-is-declared` precondition. The dynamics, balance, rotor and limiter oracles also run at
32, 64 and 128 kHz, the other rates of the oracle set (`OMX_FX_ORACLE_RATES` in
`test/fx/fx_rates.h`), with contracts still on: there the `rate-is-declared` precondition is
expected, and every other law must hold. Their golden digest files carry all nine rates.

## The engine's copies

The sample-rate converter (`omx_src.h`), the generators (`omx_gen.h`) and the analyser's spectrum
readings and half-band decimator (`analysis/omx_rta.h`) moved here from OpenMixer's engine unchanged in
behaviour. Their oracles are suite arms (`test/kernels/src.c`, `gen.c`,
`rta.c`); `test/engine_kernels/golden.test.c` hashes their output per kernel at the nine oracle rates,
and `test/golden/engine_kernels.sha256` holds the digests the engine's own copies gave, written by
`tools/engine-kernels-identity.sh --write`. `make engine-kernels-identity OPENMIXER=<checkout>`
compiles the program again with those copies in place of the three headers, against the
omx-contract release the engine pins, and requires the same digests (set
`ENGINE_IDENTITY_INCLUDE` to the include directory of the omx-dsp the engine pins, e.g. `git archive
<its commit> include`, when this tree's other headers need a newer contract than the engine's); its self-test changes one line
in each copy and requires a failure each time. It is a desk check and CI does not run it: the copies
live in OpenMixer's private repository. It ends when the engine builds from these headers instead
of its copies.

The engine spells the converter's design rolloff `OMX_SRC_ROLLOFF`, a double; omx-contract renders
it as the float `OMX_SRC_ROLLOFF` and the double `OMX_SRC_ROLLOFF_DOUBLE`. A caller that wants the
engine's coefficients passes `OMX_SRC_ROLLOFF_DOUBLE` to `omx_src_design`.

## The channel strip's input stage

omx-plugins' channel strip ran its input trim and the trim's ramp as arithmetic of its own. That
stage is now two faces: `fx/omx_trim_instance.h` (the trim kernel's one control, the console's
de-zippered head gain) and `fx/omx_eq_instance.h` with every band off (the HPF and the LPF, which
the console runs as sections of its channel EQ). `test/golden/trim_instance.sha256` was written by
`make strip-input-identity OMX_PLUGINS=<checkout>` (`tools/strip-input-identity.sh`) from the
plugin's own `omx_strip.h`, and `test/fx/trim_instance_golden.test.c` holds the two faces to it at
every declared rate. Run it again whenever either face or the strip's input stage changes; it ends
when the strip is generated from these faces and holds no arithmetic of its own.

## Room analysis and the alignment fit

OpenMixer's room analysis and Smart Alignment's fit moved here from TypeScript:
`analysis/omx_align_fit.h` (the cross-spectrum accumulator, the
delay and polarity fit, the correction), `analysis/omx_room.h` (the H1 transfer and coherence,
the impulse response and its early reflections, RT60 per octave band, the mode finder and the
mode-EQ proposal) and `analysis/omx_sweep.h` (the Farina sweep, its inverse and the
deconvolution), compiled in `src/omx_align_fit.c`, `src/omx_room.c` and `src/omx_sweep.c`. They
run on a control thread, never in the audio callback: every buffer and scratch array is the
caller's, sized as each function states, and nothing is allocated, locked or kept between calls.
A measurement that cannot conclude returns a refusal code (`enum omx_analysis_refusal`), one per
word of core's refusal lists.

The numbers core declares for these readers and omx-contract does not yet carry (the frame
floor, the default and mode bands, the trust coherence, the analysis floor, the early-reflection
horizon and floor, the T20 span, the octave centres, the mode margin and position floor, the
sweep's fade fraction) are the caller's to pass; this tree types none of them. The proposal's
bounds are omx-contract's (`OMX_FBS_MAX_NOTCH_DEPTH_DB`, `OMX_EQ_GAIN_RANGE_*`,
`OMX_EQ_Q_RANGE_*`).

Two kinds of oracle hold them. `test/analysis/align_fit.test.c`, `room.test.c` and
`sweep.test.c` check closed forms at the nine oracle rates (a known delay, gain, reflection,
decay, set of resonances, sweep law) and count allocations. `test/analysis/room_ts.test.c` holds
every result to what core's TypeScript computes on fixed inputs, from the parity table
`test/analysis/fixtures/room_ts.h`: integers exactly, reals within 1e-9 relative (measured gap
about 1e-12; V8's libm against the host's). `tools/room-ts-golden.mjs` wrote the table from an
OpenMixer checkout, with omx-contract's TypeScript render from the pinned release; every input
is built from an integer PRNG and exact arithmetic, so the C test rebuilds the same bits.
`make room-ts-golden OPENMIXER=<checkout>` writes it again and requires the committed file; run
it whenever core's TypeScript or these kernels change, until the TypeScript is deleted. `test/analysis/room-sabotage.sh` breaks each kernel once in a
scratch copy and requires its closed-form oracle and the parity test to fail.
## The EQ maths openmixer's TypeScript computes

openmixer designs its console EQ and draws its curve in TypeScript today. The C
here computes the same results, so the engine and the server can take them from one place:

| TypeScript | C |
|---|---|
| core `rbjSection`, `rbjCoeffs` (core `eq.ts`) | `omx_eq_design`, `omx_eq_design_f` |
| core `bandwidthOctavesQ` | `omx_eq_bandwidth_q` |
| core `butterworthSlopeQs`, web `butterworthSectionQs` | `omx_eq_butterworth_qs` |
| core `passFilterMaxSections` | `OMX_EQ_PASS_MAX_SECTIONS` |
| core `complementSection` | `omx_eq_complement` |
| core `eqBandIsIdentity` | `omx_eq_band_is_identity` |
| core `eqCoeffs` | `omx_eq_bank_design` |
| core `passFilterCoeffs` | `omx_eq_pass_design` |
| core `sectionMagnitudeDb` | `omx_biquad_magnitude_db_d` |
| core `biquadMagnitudeDb` | `omx_eq_band_magnitude_db` |
| core `eqCurveDb` | `omx_eq_curve_db` (its frequencies: `omx_eq_curve_freqs`) |
| web `magnitudeSumDb`, `seatY`'s value (web `eqCurveGeom.ts`) | `omx_eq_magnitude_sum_db` |
| web `eqStateBands`, `filterBands`, `passFilterBands` | `omx_eq_curve_bands` |
| web `eqStateCurveDb`, `compositeCurveDb` | `omx_eq_curve_bands`, then `omx_eq_curve_db` |
| catalog `octavesToQ` (catalog `eq.ts`) | `omx_eq_octaves_q` |

`test/kernels/fixtures/eq_design_ts.h` is what that TypeScript computes on fixed inputs at the nine
oracle rates, written by `tools/ts-golden.mjs design <openmixer> <contract-ts>` (Node 22.7 or later;
`<contract-ts>` is omx-contract's TypeScript render, `omx-contract render --target ts --out <dir>`
from a checkout of the installed release). The `eq-design-ts` arm holds the C to it: a coefficient, a Q or
a frequency of the drawn grid within 4 ulp, a magnitude within 1e-9 dB, the bank's order, count and
parked sections exactly. Two families of coefficient pass 4 ulp, both a cancellation that turns one
ulp of V8's `exp` or `cos` into many (a matched bell or shelf at a low corner or a wide Q, and the
fitted low-pass's b2 far below Nyquist); they are held to 1e-13 absolute, and the arm prints the
largest gap of each kind. Regenerate the fixture when openmixer's TypeScript changes, until it is
deleted. The `eq-curve` arm holds the same functions to closed forms.

## Loudness

`omx_loudness.h` measures EBU R 128 loudness the way ITU-R BS.1770-4 defines it
(FreeMixer/omx-dsp#64). The engine measures and accumulates; it reads the figures and publishes
them, and no other code computes them.

- **K-weighting.** Two sections designed for the rate at init, in double, from the analogue
  prototype of BS.1770's 48 kHz table (a high shelf at 1681.97 Hz, +4.0 dB, and the RLB high-pass
  at 38.135 Hz), by the bilinear transform with `tan` prewarping. At 48 kHz they reproduce the
  standard's printed coefficients; at the other rates they are the same filter. Each leg runs
  through omx_biquad_d(), double history.
- **Blocks.** The K-weighted squares, each leg weighted (1.0 for L, R, C; 1.41 for the surrounds;
  0 drops a leg, the LFE), are summed in 10 ms sub-blocks (`rate / 100` samples, a whole number at
  each of the nine oracle rates). Momentary loudness is the last 40 sub-blocks (400 ms), short-term
  the last 300 (3 s); a window that has not yet filled counts the missing time as silence.
- **Gating.** Every 100 ms a full 400 ms block joins the integrated measurement and, once 3 s have
  passed, the short-term value joins the range. Neither keeps the blocks: each keeps a histogram of
  0.1 LU bins from −70 LUFS up, every bin holding its count and the exact sum of its energies. The
  absolute gate (−70 LUFS) is a bin edge; the relative gate (−10 LU for the integrated loudness,
  −20 LU for the range) is placed in energy, and the bin it falls in is kept when its own mean is
  above the gate. So a programme of any length takes the same state, and the integrated value is
  exact except for blocks within 0.1 LU of the relative gate.
- **Loudness range.** EBU Tech 3342: the 10th to the 95th percentile of the gated short-term
  values, each percentile read as the energy mean of the bin that holds it.
- **True peak.** The largest reading of omx_truepeak_block() over every leg: the shared ×4
  half-band interpolator, BS.1770-4's four, not a second one.

Nothing on the process path allocates, locks or calls libm: the bin edges and coefficients are
computed at init, and a reading calls `pow` and `log10` at most twice. The state is one caller-owned struct
without pointers, exported with OMXDSP_STATE_LAYOUT.

The oracle (`test/fx/loudness.test.c`) generates EBU Tech 3341's and 3342's test signals from
their closed forms at each of the nine oracle rates and holds the readings to the tolerances the
documents give: momentary, short-term and integrated ±0.1 LU, true peak +0.2/−0.4 dB, loudness
range ±1 LU. Generated: 3341 signals 1 to 6, 9, 12 and 15 to 19, and 3342 signals 1 to 4. In
place of the burst signals 10 and 13, a burst of exactly one window (3 s, 400 ms) at −23 dBFS is
placed off the 10 ms grid and the largest reading must be −23.0 LUFS ±0.1. Not generated: the
programme recordings (3341 signals 7 and 8, 3342 signals 5 and 6) and the signals published only
as files (3341 signals 11, 14 and 20 to 23). The true-peak tones are faded in over 20 ms: a sine
switched on at full level is a step, and its reconstruction overshoots. The golden digests
(`test/golden/loudness.sha256`) hold the readings in energy, so no libm `log10` enters them;
`test/fx/loudness-perturb.sh` breaks the shelf, the relative gate, the range percentile, the true
peak and the momentary window one at a time, and each must turn the oracle red and move every
digest.

## The definitions and the limits

The kernels and DSP constants this library implements are defined in `data/`: one file per kernel
under `data/kernels/`, one per constant under `data/primitives/`, in omx-contract's schema. The C
code reads them through `<omxdsp/omxdsp_definitions.h>`, rendered from `data/` by omx-contract's tool
(`make definitions OMX_CONTRACT=<checkout>`, which runs `omx-contract render --target c --package
omx-dsp`) and committed, so a build needs no node. Never edit the header: change the definition and
render again; `make definitions-check` fails while the two differ, and `tools/contract-single-source.sh`
fails on a second definition of any of its names. `make install` installs `data/` into omx-contract's
contract directory as the package `omx-dsp`, with a `MANIFEST.json`, where the other packages read
it.

The facts no single package implements (the declared sample rates, the parameter scales) stay in
FreeMixer/omx-contract, read through `<omxcontract/omx_contract_limits.h>`, which the definitions
header includes. That dependency is declared once, in the packaging: `packaging/omx-dsp.spec`
(`BuildRequires`, `Requires`) and `debian/control` (`Build-Depends`, `Depends`) carry the same floor
and no upper bound (`>= 3.0.0`), and `omxdsp.pc` requires the same floor. Install
it from the FreeMixer channel with the rest of the build dependencies:

    dnf builddep packaging/omx-dsp.spec     # Fedora, with the FreeMixer repository added
    apt-get build-dep ./                    # Debian, with the FreeMixer source list added

`tools/contract-include.sh` finds the installed `omx-contract` with `pkg-config`: at or above the
floor the spec declares, it prints the package's include directory; otherwise the build stops, naming
the package, the floor and the version installed. The dependency is a floor only, never a ceiling.
Nothing is downloaded and no version is pinned anywhere else. `tools/contract-include-selftest.sh`
(`make checks`) holds the script to that floor, `debian/control` and `omxdsp.pc.in` to the spec's, and
refuses an upper bound in any of them.

`OMX_CONTRACT_INC=<dir>` names another include directory (one that holds `omxcontract/`) and skips the
version check. The perturbation arms use it, and so can you: render a contract with one value moved
into a scratch directory, `make test`, and the kernel test that reads that value must go red.
Moving the range is one commit of its own, in the spec and `debian/control` together. A value the
kernels need is added to its definition in `data/` and the header rendered again; it is never typed
in the code.

## Packages

`packaging/omx-dsp.spec` (RPM, `omx-dsp-devel`) and `debian/` (`libomxdsp-dev`). A `v<version>`
tag releases both through the FreeMixer release workflows; the version is
`OMXDSP_VERSION_*` in `include/omxdsp/omxdsp.h`, the spec's `Version:` and the top of
`debian/changelog`, and CI refuses them disagreeing.
