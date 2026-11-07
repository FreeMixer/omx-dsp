# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# libomxdsp — the DSP primitives and effect kernels the OpenMixer engine and the omx plugins
# link statically.
#   make lib    the compiled unit (every primitive and kernel is header-only) once per flavour:
#               build/libomxdsp.a, build/libomxdsp-contracts.a, build/libomxdsp-tsan.a
#   make test   the library suite alone: contracts ON, every oracle at every declared rate, the
#               negative arms, the perturbation arm, the N-thread byte-identity, the
#               writable-data check, the doc check and the effect kernels' oracles and golden
#               digests — pure C + -lm (+ -pthread), no node
#   make install   headers, the three archives and their .pc files under PREFIX/LIBDIR/INCLUDEDIR,
#               and data/ as the package omx-dsp of omx-contract's contract directory, into DESTDIR
#   make flavours  each archive carries its flavour, and a contracts consumer sees the compiled
#               unit's contracts only through the contracts archive
#   make test-fx   the effect kernels' oracles and golden digests alone
#   make consumer  the stateful stages built and run the way a consumer drives them, release flags
#   make test-analysis  the analysis engines' oracles (FBS feedback detection, HRP) alone
#   make test-engine-kernels  the golden digests of the converter, generators and analyser readings
#               moved from the engine, and a sabotage of each
#   make engine-kernels-identity OPENMIXER=<checkout>  the same digests rendered through the engine's
#               own copies (mix_src.h, mix_dsp.h), and the check's sabotages (tools/engine-kernels-identity.sh)
#   make exports  build/omx-dsp-exports.json: every public export with its fingerprint, literals,
#               kernels and twins, which omx-dsp-copy-check reads in a consumer (make install ships both)
#   make copy-check  omx-dsp-copy-check's self-test over planted copies, then each rule broken in turn
#   make lint   the doc check and the source scan only
#   make fuzz   the libFuzzer harnesses of fuzz/ (clang, ASan, UBSan, contracts); make fuzz-run FUZZ_TIME=60 runs
#               each from its seed corpus, make fuzz-corpus regenerates the seeds (fuzz/README.md)
#   make contract-dir OMX_CONTRACT=<checkout>  data/ installed beside omx-contract's own data as a contract
#               directory and read back through the checkout's reader, and three refusals
#               (tools/contract-dir-check.sh)
#   make definitions OMX_CONTRACT=<checkout>  include/omxdsp/omxdsp_definitions.h rendered from data/ by the
#               checkout's `render --package omx-dsp`; make definitions-check holds the committed header equal
#               to that render (tools/definitions-render.sh)
#   make contract-proof OMX_CONTRACT=<checkout>  the round-trip against this library's v0.1.3 header and the kernel
#               completeness over data/ and recipes/answers/, after the proof's own self-test (tools/contract-proof.mjs)
#   make contract-tests OMX_CONTRACT=<checkout>  the declaration tests (test/contract/): each fact of data/ they hold,
#               over this live data/ with omx-contract's reader and renderer
#   make magnitude-sabotage  the magnitude arms fail against a sabotaged evaluator
#   make eq-sabotage  the EQ bank, head filter and curve arms fail against nine sabotages
#   make curve-sabotage  the one curve function's arm (omx_curve.h) fails against twelve sabotages
#   make curve-golden  test/kernels/fixtures/curve_golden.h rewritten from omx_curve_points() (tools/curve-golden.c)
#   make meter-view-sabotage  the meter view arm (mix/omx_meter_view.h) fails against seven sabotages
#   make meter-view-golden  vectors/meter-view.json rewritten from the meter view law (tools/meter-view-golden.c)
#   make geq-true-sabotage  the graphic EQ's true-mode oracle fails against seven sabotages
#   make delay-taps-sabotage  the FX delay's read-tap oracle fails against ten sabotages
#   make align-fdelay-sabotage  the strip alignment stage's oracle fails against five sabotages
#   make pitch-rates-sabotage  the pitch oracles fail at 32, 64 and 128 kHz against two sabotages
#   make instance-rates-sabotage  the nine instance faces' oracles fail at 32, 64 and 128 kHz against the declared-only gate put back, and the deesser's against its Nyquist cap removed
#   make bandwidth-q-table  test/golden/eq_bandwidth_q.json is omx_eq_bandwidth_q()'s
#   make eq-default-bands  omx_eq_default_centres() and omx_eq_default_type() give test/golden/eq_default_bands.txt
#               for 1..62 bands, and every count EQ_BAND_COUNTS declares has its default bands
#   make eq-defaults-table  test/golden/eq_default_bands.txt is the rule's JavaScript spelling over data/
#               (tools/eq-defaults-golden.mjs, node)
#   make public-sources  no tracked file names a private repository, path or issue (FreeMixer/.github's
#               private-refs-check, from a checkout of it: DOTGITHUB=<dir>)
#   make docs   the API reference by doxygen (build-time only)
#   make test-tsan  the thread arm under -fsanitize=thread where the toolchain has it
#   make rt-gate   omx-rt-gate, the one static real-time gate (tools/rt-gate, omx-rt-gate(1)): its class
#               table measured by the externals arm, its planted fixtures red and clean twins green, its
#               load refusals;  make rt-gate-sabotage  the guards broken one at a time, the -O2 arm
#   make cost-prel  omx_env_program_release's ns/sample at every declared rate (its cost row)
#   make cost-deesser  the de-esser's ns/frame at every declared rate, default and stereo, by the
#               plugin cost method (1 kHz sine at -6 dBFS, 512-frame blocks, 64 discarded, p50/p95)
#   make bench-mixmatrix  omx_mixmatrix dense/sparse ns per strip-output-frame, 32/64/97 strips x
#               1024 frames, then 64 x 32 held to MIXMATRIX_PERIOD_SHARE of the period at the nine rates
#   make bench-gate  omx_dynamics as a gate, ns/sample/lane at every declared rate (open, slope, floor);
#               tools/gate-cost-ab.sh <rev> compares a revision's kernel with this tree's, interleaved
#   make bench-drive  omx_drive_process at the come-up atom, and its oversampler and shaper halves, ns/sample
#               at every declared rate;  make bench-flanger  omx_flanger_process, default and max
#               tools/kernel-cost-ab.sh <bench> <rev> [rounds] [quantum] [blocks] is the same
#               interleaved before/after for any of the three benches (gate-cost-bench, drive-cost, flanger-cost)
#   make strip-input-identity OMX_PLUGINS=<checkout>  the trim and eq faces against omx-plugins'
#               omx_strip.h input stage, and the check's sabotage (tools/strip-input-identity.sh)
#   make analysis-engine-identity OPENMIXER=<checkout>  the RTA, HRP render and HRP live core against
#               the engine's own copies, and the check's sabotage (tools/analysis-engine-identity.sh)
#   make room-ts-golden OPENMIXER=<checkout>  core's room analysis and alignment fit, run under node from
#               the checkout, against the committed parity table (tools/room-ts-golden.mjs)
#   make test-mix  the mix module's golden digests (pan law, LCR, strip and fold coefficients, bus
#               out stage, peak, clip, peak hold, gain-reduction reading, analyser tap, bus sum,
#               real-time load meter, factored strip coefficient, route gain stages, strip meter
#               reading, settled silence, LCR fold level, bus stages, a dynamics slot's key, the
#               source switch, a level meter's dBFS reading) and a sabotage of each kernel against
#               its oracle and digests
#   make mix-engine-identity OPENMIXER=<checkout>  the same digests rendered through the engine's own
#               copies, each set the checkout carries (mix_dsp.h, mix_lcr.h, mix_bus.h,
#               mix_gr_meter.h; mix_dsp.h, mix_sum.h; mix_dsp.h; mix_lane.h; mix_pb.h), and the
#               check's sabotages (tools/mix-engine-identity.sh)

CC      ?= cc
AR      ?= ar
NM      ?= nm
CFLAGS  ?= -Wall -Wextra -Werror -O2
# Every consumer compiles the inline kernels without floating-point contraction (omxdsp.pc carries
# the same flag), so a kernel's output does not depend on the architecture's FMA or the consumer's
# build flags: the engine and a plugin built at one version produce the same bits.
FPFLAGS  = -ffp-contract=off
# The flags this file adds are taken out of what it is given first. CFLAGS from the environment (an rpm or deb
# build's set_build_flags) goes on to a sub-make with them already in it, so `make test`'s second make would read
# libmvec in CFLAGS_PLAIN, and FASTMATH=off would not take it out.
override CFLAGS := $(filter-out $(FPFLAGS) -fopenmp-simd -fno-math-errno -DOMX_FASTMATH_MVEC_%,$(CFLAGS))
override CFLAGS += $(FPFLAGS)
# libmvec, found by a build-time probe (tools/fastmath-probe.sh): the functions this toolchain can call
# vectorised, each declared to the compiler by omx_fastmath.h under its OMX_FASTMATH_MVEC_<F> macro. Never
# -ffast-math. FASTMATH=off keeps the libm; FASTMATH=auto (the default) takes what the probe finds.
# CFLAGS_PLAIN is the flags without it: the golden digests are built with it, on the libm, so they read
# the same on every glibc, with or without libmvec.
FASTMATH ?= auto
CFLAGS_PLAIN := $(CFLAGS)
ifeq ($(FASTMATH),off)
MVEC_FUNCS :=
else
MVEC_FUNCS := $(shell sh tools/fastmath-probe.sh "$(CC)")
endif
ifneq ($(MVEC_FUNCS),)
MVEC_CFLAGS = -fopenmp-simd -fno-math-errno $(foreach f,$(MVEC_FUNCS),-DOMX_FASTMATH_MVEC_$(shell echo $(f) | tr a-z A-Z)=1)
MVEC_LIBS   = -lmvec
override CFLAGS += $(MVEC_CFLAGS)
endif
# The limits come from FreeMixer/omx-contract: the installed omx-contract-devel / libomx-contract-dev
# at or above the floor packaging/omx-dsp.spec declares, found by pkg-config in tools/contract-include.sh
# (nothing is downloaded); this tree commits no copy of them. Every sub-script that compiles gets the
# directory as CONTRACT_INC.
ifeq ($(filter version clean docs public-sources private-refs exports copy-check deps deps-check,$(MAKECMDGOALS)),)
CONTRACT_INC := $(shell sh tools/contract-include.sh)
ifeq ($(CONTRACT_INC),)
$(error tools/contract-include.sh found no omx-contract: see its message above)
endif
export CONTRACT_INC
endif
INC      = -Iinclude -isystem $(CONTRACT_INC)
# The floor the spec declares, rendered into omxdsp.pc's Requires, beside its ceiling, the next major.
CONTRACT_FLOOR   := $(shell sh tools/contract-include.sh --floor)
BUILD    = build
LIB      = $(BUILD)/libomxdsp.a
HEADERS  = $(wildcard include/omxdsp/*.h)
ANALYSIS_HEADERS = $(wildcard include/omxdsp/analysis/*.h)
MIX_HEADERS = $(wildcard include/omxdsp/mix/*.h)
SRC      = $(wildcard src/*.c)
OBJ      = $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
TESTFLAGS = $(CFLAGS) $(INC) -DOMX_CONTRACTS

# The flavours. The compiled unit is built once per flavour, and a consumer links the archive whose
# flags match its own build, through the .pc file of the same name: a contracts or TSan build of a
# consumer then checks the library's own code too, not only the inline headers.
#   omxdsp            build/libomxdsp.a            contracts compiled out (the release build)
#   omxdsp-contracts  build/libomxdsp-contracts.a  -DOMX_CONTRACTS: the unit's PRE/POST record into
#                                                  the consumer's ledger
#   omxdsp-tsan       build/libomxdsp-tsan.a       -DOMX_CONTRACTS -fsanitize=thread
# The .pc file's Cflags carry the flavour's defines, so the consumer's inline kernels and the
# archive agree.
CONTRACTS_CFLAGS = -DOMX_CONTRACTS
TSAN_CFLAGS      = -DOMX_CONTRACTS -fsanitize=thread
TSAN_LIBS        = -fsanitize=thread
LIB_CONTRACTS    = $(BUILD)/libomxdsp-contracts.a
LIB_TSAN         = $(BUILD)/libomxdsp-tsan.a
OBJ_CONTRACTS    = $(patsubst src/%.c,$(BUILD)/contracts/%.o,$(SRC))
OBJ_TSAN         = $(patsubst src/%.c,$(BUILD)/tsan-lib/%.o,$(SRC))
LIBS_ALL         = $(LIB) $(LIB_CONTRACTS) $(LIB_TSAN)
PC_ALL           = $(BUILD)/omxdsp.pc $(BUILD)/omxdsp-contracts.pc $(BUILD)/omxdsp-tsan.pc
KERNELS  = $(wildcard test/kernels/*.c) $(wildcard test/kernels/fixtures/*.h)
FX_HEADERS = $(wildcard include/omxdsp/fx/*.h)
FX_TESTS   = $(wildcard test/fx/*.c) $(wildcard test/fx/*.h) $(wildcard test/fx/fixtures/*.h)
SUPPORT_HEADERS = $(wildcard test/support/*.h)
GOLDEN_SUBST    = -include test/support/log10f_subst.h

PREFIX     ?= /usr/local
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
BINDIR     ?= $(PREFIX)/bin
DATADIR    ?= $(PREFIX)/share
MANDIR     ?= $(DATADIR)/man
DESTDIR    ?=
# The definitions this library implements (data/) install as the package omx-dsp of the installed
# omx-contract's contract directory, <contractdir>/<schema major>/omx-dsp/, with the installer that
# omx-contract ships beside its schema: plain shell, no node. Read when `make install` runs, only.
CONTRACTDIR        ?= $(shell pkg-config --variable=contractdir omx-contract 2>/dev/null)
CONTRACT_DATADIR    = $(shell pkg-config --variable=datadir omx-contract 2>/dev/null)
CONTRACT_INSTALLER ?= $(CONTRACT_DATADIR)/install-contract-dir.sh
CONTRACT_MAJOR      = $(shell sed -n 's/^  "x-omx-schema-version": "\([0-9]*\)\..*",$$/\1/p' $(CONTRACT_DATADIR)/omx-contract.schema.json 2>/dev/null)
VERSION    := $(shell sed -n 's/^\#define OMXDSP_VERSION_\(MAJOR\|MINOR\|PATCH\) \([0-9]*\)$$/\2/p' include/omxdsp/omxdsp.h | paste -sd.)



.PHONY: fuzz fuzz-run fuzz-corpus all lib test lint docs clean contract-dir definitions definitions-check contract-proof contract-tests test-tsan suite negative consumer gen-once perturb fdelay-sabotage doppler-sabotage onepole-sabotage threads threads-sabotage checks cost-prel test-fx test-analysis install version golden-write flavours bench-mixmatrix bench-gate bench-drive bench-flanger check-log10f strip-input-identity analysis-engine-identity test-mix mix-engine-identity test-engine-kernels engine-kernels-identity magnitude-sabotage eq-sabotage curve-golden curve-sabotage meter-view-golden meter-view-sabotage geq-true-sabotage delay-taps-sabotage align-fdelay-sabotage pitch-rates-sabotage instance-rates-sabotage bandwidth-q-table bandwidth-q-sabotage eq-default-bands eq-defaults-table room-ts-golden public-sources private-refs deps deps-check cost-deesser note-map-sabotage exports copy-check

all: lib

lib: $(LIBS_ALL)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(HEADERS) $(ANALYSIS_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -fPIC $(INC) -c -o $@ $<

$(LIB): $(OBJ) | $(BUILD)
	$(AR) rcs $@ $(OBJ)

$(BUILD)/contracts/%.o: src/%.c $(HEADERS) $(ANALYSIS_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CONTRACTS_CFLAGS) -fPIC $(INC) -c -o $@ $<

$(LIB_CONTRACTS): $(OBJ_CONTRACTS)
	$(AR) rcs $@ $(OBJ_CONTRACTS)

$(BUILD)/tsan-lib/%.o: src/%.c $(HEADERS) $(ANALYSIS_HEADERS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(TSAN_CFLAGS) -fPIC $(INC) -c -o $@ $<

$(LIB_TSAN): $(OBJ_TSAN)
	$(AR) rcs $@ $(OBJ_TSAN)

# ---- the suite ---------------------------------------------------------------------------------
# Each primitive's arm lives in its own test/kernels/<name>.c; tools/kernels-gen.sh globs them
# (sorted by filename, never a hand list) into $(BUILD)/kernels-suite.inc.c, which
# omxdsp_suite.c #includes — a kernel lane adds ONE file and never touches this Makefile or
# omxdsp_suite.c's main().

# The generation is ONE rule: both files come from one run (a grouped target), and every target that
# reads either lists them, so `make -jN` runs the generator once and never reads a half-written file.
# No recipe and no script calls the generator into $(BUILD) (make gen-once, test/gen-once.sh).
# KERNELS_GEN is the command, overridable so the test can count its runs.
KERNELS_GEN = bash tools/kernels-gen.sh
KERNELS_INC = $(BUILD)/kernels-suite.inc.c $(BUILD)/kernels-perturb.inc.c

$(KERNELS_INC) &: $(wildcard test/kernels/*.c) tools/kernels-gen.sh | $(BUILD)
	$(KERNELS_GEN) $(BUILD)

$(BUILD)/omxdsp_suite: test/omxdsp_suite.c $(KERNELS) $(KERNELS_INC) $(LIB_CONTRACTS) $(HEADERS) $(MIX_HEADERS)
	$(CC) $(TESTFLAGS) -I$(BUILD) -o $@ test/omxdsp_suite.c $(LIB_CONTRACTS) -lm

$(BUILD)/omxdsp_threads: test/omxdsp_threads.c $(LIB_CONTRACTS) $(HEADERS) | $(BUILD)
	$(CC) $(TESTFLAGS) -pthread -o $@ test/omxdsp_threads.c $(LIB_CONTRACTS) -lm

# The POST arm of the negative suite runs against a SABOTAGED COPY of one header, placed ahead of
# the real include directory: the tree is never edited.
$(BUILD)/omxdsp_negative: test/omxdsp_negative.c test/sabotage.sh $(SRC) $(HEADERS) | $(BUILD)
	bash test/sabotage.sh $(BUILD)/sabotage
	$(CC) $(CFLAGS) -I$(BUILD)/sabotage $(INC) -DOMX_CONTRACTS -o $@ test/omxdsp_negative.c $(SRC) -lm

suite: $(BUILD)/omxdsp_suite
	./$(BUILD)/omxdsp_suite

negative: $(BUILD)/omxdsp_negative
	./$(BUILD)/omxdsp_negative

# The stateful stages built the way a consumer builds them: release flags, no contracts, states on
# the stack that only their init or settle writes; one stage per translation unit, as a consumer's.
CONSUMER_ARMS = pitch chorus flanger rotor
CONSUMER_BINS = $(addprefix $(BUILD)/consumer_,$(CONSUMER_ARMS))

$(BUILD)/consumer_%: test/omxdsp_consumer.c $(LIB) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -DARM_$* -o $@ test/omxdsp_consumer.c $(LIB) -lm

consumer: $(CONSUMER_BINS)
	@set -e; for b in $(CONSUMER_BINS); do echo "./$$b"; ./$$b; done

threads: $(BUILD)/omxdsp_threads
	./$(BUILD)/omxdsp_threads

# The thread arm's gate against a gate whose state is carried between runs: the arm must go red,
# naming a worker whose output differs from the reference (test/omxdsp_threads.c).
threads-sabotage: test/omxdsp_threads.c $(LIB_CONTRACTS) $(HEADERS) | $(BUILD)
	$(CC) $(TESTFLAGS) -pthread -DOMXDSP_THREADS_SABOTAGE_GATE_CARRIES_STATE -o $(BUILD)/omxdsp_threads_sabotage \
	  test/omxdsp_threads.c $(LIB_CONTRACTS) -lm
	@if ./$(BUILD)/omxdsp_threads_sabotage > $(BUILD)/omxdsp_threads_sabotage.log 2>&1; then \
	  echo "threads-sabotage: FAIL — a gate carrying state between runs left the thread arm green"; exit 1; fi
	@grep -q "output differs from the single-threaded reference" $(BUILD)/omxdsp_threads_sabotage.log || \
	  { echo "threads-sabotage: FAIL — red, but not on an output that differs"; tail -3 $(BUILD)/omxdsp_threads_sabotage.log; exit 1; }
	@echo "threads-sabotage: the thread arm sees a gate that carries state between runs ($$(grep -c 'output differs' $(BUILD)/omxdsp_threads_sabotage.log) worker outputs differ)"

# The magnitude arms fail against a sabotaged evaluator (test/magnitude-sabotage.sh); the real
# suite above is the control.
magnitude-sabotage: test/omxdsp_suite.c test/magnitude-sabotage.sh $(KERNELS) $(LIB_CONTRACTS) $(HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/magnitude-sabotage.sh $(BUILD)/magnitude-sabotage $(LIB_CONTRACTS)

# The EQ bank's, head filters' and drawn curve's arms fail against nine sabotages
# (test/eq-sabotage.sh); the real suite above is the control.
eq-sabotage: test/omxdsp_suite.c test/eq-sabotage.sh $(KERNELS) $(LIB_CONTRACTS) $(HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/eq-sabotage.sh $(BUILD)/eq-sabotage $(LIB_CONTRACTS)

# The bandwidth-to-Q law's table (FreeMixer/omx-dsp#41), held equal to what omx_eq_bandwidth_q()
# computes; openmixer's TypeScript copy is tested against the committed file.
$(BUILD)/eq_bandwidth_q_table: test/eq_bandwidth_q_table.c $(LIB) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIB) -lm

# q is held within 4 ULP, not bit for bit: the law goes through libm's sin() and sinh(), which glibc does not
# round correctly, so libm versions differ in the last digits (test/eq_bandwidth_q_table.c says more).
bandwidth-q-table: $(BUILD)/eq_bandwidth_q_table
	./$(BUILD)/eq_bandwidth_q_table --check test/golden/eq_bandwidth_q.json

# The bound proves itself: the golden and Fedora 45's table (glibc 2.44) pass, a q moved 4 ULP passes and one
# moved 5 ULP either way fails.
bandwidth-q-sabotage: $(BUILD)/eq_bandwidth_q_table
	./$(BUILD)/eq_bandwidth_q_table --check test/golden/eq_bandwidth_q.json test/golden/eq_bandwidth_q.json
	./$(BUILD)/eq_bandwidth_q_table --check test/golden/eq_bandwidth_q.json test/eq_bandwidth_q/fedora45-glibc-2.44.json
	./$(BUILD)/eq_bandwidth_q_table --nudge 4 test/golden/eq_bandwidth_q.json | ./$(BUILD)/eq_bandwidth_q_table --check test/golden/eq_bandwidth_q.json -
	@for n in 5 -5; do \
	  if ./$(BUILD)/eq_bandwidth_q_table --nudge $$n test/golden/eq_bandwidth_q.json | ./$(BUILD)/eq_bandwidth_q_table --check test/golden/eq_bandwidth_q.json - > $(BUILD)/bandwidth-q-nudge.out; then \
	    echo "bandwidth-q-sabotage: a q moved $$n ULP passed"; exit 1; fi; \
	  grep -q '^row 7 .*(5 ULP apart, bound 4)' $(BUILD)/bandwidth-q-nudge.out || { cat $(BUILD)/bandwidth-q-nudge.out; echo "bandwidth-q-sabotage: the refusal does not name row 7"; exit 1; }; \
	done
	@echo "bandwidth-q-sabotage: 4 ULP passes, 5 ULP either way is refused, Fedora 45's table passes"

# THE default EQ rule (omx_eq_defaults.h, moved from omx-contract's C render at 3.0.0) against its
# JavaScript spelling's answers (tools/eq-defaults-golden.mjs writes the golden; no node here).
$(BUILD)/eq_default_bands: test/eq_default_bands.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< -lm

eq-default-bands: $(BUILD)/eq_default_bands
	./$(BUILD)/eq_default_bands test/golden/eq_default_bands.txt

# The one curve function's golden vectors (test/kernels/fixtures/curve_golden.h, read back by the suite's curve arm)
# are written by tools/curve-golden.c from test/kernels/fixtures/curve_requests.h: `make curve-golden` rewrites
# them, in the commit that changes the law, and the diff is the review.
$(BUILD)/curve_golden: tools/curve-golden.c test/kernels/fixtures/curve_requests.h $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS_PLAIN) $(INC) -o $@ $< -lm

curve-golden: $(BUILD)/curve_golden
	./$(BUILD)/curve_golden > test/kernels/fixtures/curve_golden.h

# The meter view law's vectors (vectors/meter-view.json, read back by the suite's mix_meter_view arm, the TypeScript
# twin's test and any other copy of the law; installed beside the exports) are written by tools/meter-view-golden.c
# from test/kernels/fixtures/meter_view_scenarios.h: `make meter-view-golden` rewrites them, in the commit that changes
# the law, and the diff is the review.
$(BUILD)/meter_view_golden: tools/meter-view-golden.c test/kernels/fixtures/meter_view_scenarios.h $(HEADERS) $(MIX_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS_PLAIN) $(INC) -o $@ $< -lm

meter-view-golden: $(BUILD)/meter_view_golden
	./$(BUILD)/meter_view_golden > vectors/meter-view.json

# The meter view arm fails against seven sabotages of omx_meter_view.h, omx_meter.h and the declared numbers
# (test/meter-view-sabotage.sh); the real suite is the control.
meter-view-sabotage: test/omxdsp_suite.c test/meter-view-sabotage.sh $(KERNELS) vectors/meter-view.json $(LIB_CONTRACTS) $(HEADERS) $(MIX_HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/meter-view-sabotage.sh $(BUILD)/meter-view-sabotage $(LIB_CONTRACTS)

# The curve arm fails against twelve sabotages of omx_curve.h (test/curve-sabotage.sh); the real suite is the control.
curve-sabotage: test/omxdsp_suite.c test/curve-sabotage.sh $(KERNELS) $(LIB_CONTRACTS) $(HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/curve-sabotage.sh $(BUILD)/curve-sabotage $(LIB_CONTRACTS)

eq-defaults-table:
	node tools/eq-defaults-golden.mjs --check

# The generator runs once under make -j8 (test/gen-once.sh), and a Makefile that calls it from a recipe again turns it red.
gen-once: $(HEADERS) test/gen-once.sh test/gen-once-loud.sh | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/gen-once.sh $(BUILD)/gen-once
	bash test/gen-once-loud.sh $(BUILD)/gen-once-loud

perturb: $(HEADERS) $(KERNELS) $(KERNELS_INC) test/perturb.sh test/omxdsp_perturb.c | $(BUILD)
	OMXDSP_GEN="$(CURDIR)/$(BUILD)" CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/perturb.sh $(BUILD)/perturb

# The graphic EQ's true-mode oracle against seven sabotaged copies of omx_geq_instance.h, each red on
# its named arm (test/fx/geq-true-sabotage.sh); the real oracle in test-fx is the control.
geq-true-sabotage: test/fx/geq_true.test.c test/fx/geq-true-sabotage.sh $(LIB_CONTRACTS) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/fx/geq-true-sabotage.sh $(BUILD)/geq-true-sabotage $(LIB_CONTRACTS)

# The FX delay's read-tap oracle against ten sabotaged copies of omx_delay.h, each red on its named
# arm (test/fx/delay-taps-sabotage.sh); the real oracle in test-fx is the control.
delay-taps-sabotage: test/fx/delay_taps.test.c test/fx/delay-taps-sabotage.sh $(LIB_CONTRACTS) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/fx/delay-taps-sabotage.sh $(BUILD)/delay-taps-sabotage $(LIB_CONTRACTS)

# The strip alignment stage's oracle against five sabotaged copies of omx_align.h (the target's
# rounding, its 88.2 kHz edge, the slew, the moving read point, the exact-copy branch), each red on
# its named arm (test/fx/align-fdelay-sabotage.sh); the real oracle in test-fx is the control.
align-fdelay-sabotage: test/fx/align_fdelay.test.c test/fx/align-fdelay-sabotage.sh $(LIB_CONTRACTS) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/fx/align-fdelay-sabotage.sh $(BUILD)/align-fdelay-sabotage $(LIB_CONTRACTS)

# The pitch oracles against two sabotaged copies of the pitch headers (the instance face's rate
# gate, the pre-filter's Nyquist term), each red on its named arm at 32, 64 or 128 kHz
# (test/fx/pitch-rates-sabotage.sh); the real oracles in test-fx are the control.
pitch-rates-sabotage: test/fx/pitch_instance.test.c test/fx/pitch_math.test.c test/fx/pitch-rates-sabotage.sh $(LIB_CONTRACTS) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/fx/pitch-rates-sabotage.sh $(BUILD)/pitch-rates-sabotage $(LIB_CONTRACTS)

# The nine instance faces' oracles against a sabotaged copy of each face with the declared-only rate gate
# put back, and the deesser's against one without its Nyquist cap, each red on its named arm at 32 kHz
# (test/fx/instance-rates-sabotage.sh); the real oracles in test-fx are the control.
instance-rates-sabotage: $(foreach k,drive rotary reverb chorus deesser delay eq flanger limiter,test/fx/$(k)_instance.test.c) test/fx/instance-rates-sabotage.sh $(LIB_CONTRACTS) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/fx/instance-rates-sabotage.sh $(BUILD)/instance-rates-sabotage $(LIB_CONTRACTS)

# The fractional delay line's suite arm against four sabotaged copies of omx_fdelay.h: each must
# go red on that arm (test/fdelay-sabotage.sh). The tree is never edited.
fdelay-sabotage: $(HEADERS) $(KERNELS) $(SRC) test/fdelay-sabotage.sh tools/kernels-gen.sh | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/fdelay-sabotage.sh $(BUILD)/fdelay-sabotage

# The Doppler head's suite arm against three sabotaged copies of omx_doppler.h, its fade and its
# hand-over: each must go red on that arm (test/doppler-sabotage.sh). The tree is never edited.
doppler-sabotage: $(HEADERS) $(KERNELS) $(SRC) test/doppler-sabotage.sh tools/kernels-gen.sh | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/doppler-sabotage.sh $(BUILD)/doppler-sabotage

# The note map's suite arm against twelve sabotaged copies of omx_note_map.h: each must go red on
# that arm (test/note-map-sabotage.sh). The tree is never edited.
note-map-sabotage: $(HEADERS) $(KERNELS) $(SRC) test/note-map-sabotage.sh tools/kernels-gen.sh | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/note-map-sabotage.sh $(BUILD)/note-map-sabotage

# omx_pole_rescale's checks in the onepole arm against five sabotaged copies of the include tree (the
# law, its running-rate and reference-rate <= 0 fixed points, the delay's and the reverb's call): each
# must go red on that arm (test/onepole-sabotage.sh). The tree is never edited.
onepole-sabotage: $(HEADERS) $(FX_HEADERS) $(KERNELS) $(SRC) test/onepole-sabotage.sh tools/kernels-gen.sh | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/onepole-sabotage.sh $(BUILD)/onepole-sabotage

checks:
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/writable-data-check.sh $(BUILD)/wd
	bash tools/doc-check.sh
	bash tools/reduction-check.sh --self-test
	bash tools/reduction-check.sh
	bash tools/log10f-guard.sh include src
	bash tools/pole-rescale-guard.sh include src
	bash tools/pole-rescale-guard.sh --self-test
	sh tools/contract-include-selftest.sh
	bash tools/contract-single-source.sh
	bash tools/contract-single-source.sh --self-test
	bash tools/travel-grid-check.sh
	bash tools/travel-grid-check.sh --self-test
	bash tools/check-package-check.sh
	bash tools/check-package-check.sh --self-test
	CC="$(CC)" TMPDIR="$(abspath $(BUILD))" bash tools/clone-check.sh
	CC="$(CC)" TMPDIR="$(abspath $(BUILD))" bash tools/clone-check.sh --self-test
	bash tools/umbrella-check.sh
	bash tools/umbrella-check.sh --self-test
	bash tools/face-conformance.sh
	bash tools/face-conformance.sh --self-test
	bash tools/rate-list-check.sh
	bash tools/rate-list-check.sh --self-test
	bash tools/patch-names-check.sh
	bash tools/patch-names-check.sh --self-test
	bash tools/gen-scratch-check.sh
	bash tools/gen-scratch-check.sh --self-test

lint: checks

OMX_CONTRACT ?=
contract-dir:
	@test -n "$(OMX_CONTRACT)" || { echo "contract-dir: name an omx-contract checkout: make contract-dir OMX_CONTRACT=<dir>" >&2; exit 2; }
	bash tools/contract-dir-check.sh $(OMX_CONTRACT) $(BUILD)/contract-dir

definitions definitions-check:
	@test -n "$(OMX_CONTRACT)" || { echo "$@: name an omx-contract checkout: make $@ OMX_CONTRACT=<dir>" >&2; exit 2; }
	bash tools/definitions-render.sh $(if $(filter definitions-check,$@),--check) $(OMX_CONTRACT) $(BUILD)/definitions

contract-proof:
	@test -n "$(OMX_CONTRACT)" || { echo "$@: name an omx-contract checkout: make $@ OMX_CONTRACT=<dir>" >&2; exit 2; }
	@git rev-parse -q --verify 'v0.1.3^{commit}' >/dev/null || git fetch -q --depth 1 origin tag v0.1.3
	node tools/contract-proof.mjs $(OMX_CONTRACT) --self-test
	node tools/contract-proof.mjs $(OMX_CONTRACT)

contract-tests:
	@test -n "$(OMX_CONTRACT)" || { echo "$@: name an omx-contract checkout: make $@ OMX_CONTRACT=<dir>" >&2; exit 2; }
	OMX_CONTRACT=$(abspath $(OMX_CONTRACT)) node --test --test-reporter=tap test/contract/*.test.mjs

# The exports file a consumer's copy check reads, generated from the headers and the compiled unit.
EXPORTS = $(BUILD)/omx-dsp-exports.json
exports: $(EXPORTS)
$(EXPORTS): $(HEADERS) $(FX_HEADERS) $(ANALYSIS_HEADERS) $(MIX_HEADERS) $(SRC) tools/omx-dsp-copy-check.sh include/omxdsp/omxdsp.h | $(BUILD)
	bash tools/omx-dsp-copy-check.sh --generate --version $(VERSION) . > $@.tmp
	mv $@.tmp $@

# omx-dsp-copy-check finds every planted copy and passes every legitimate use, and each of its
# rules, broken in turn, turns its self-test red.
copy-check: | $(BUILD)
	TMPDIR="$(abspath $(BUILD))" bash tools/omx-dsp-copy-check.sh --self-test
	TMPDIR="$(abspath $(BUILD))" bash tools/omx-dsp-copy-check.sh --self-test --sabotage

# No tracked file points into a private repository, issue, branch or path, with no exemption. The engine and its
# self-test are FreeMixer/.github's private-refs-check (CI runs it in that repository's public-sources-check
# workflow, with the commit message check and no-local-copies); locally name a checkout of it:
# make private-refs DOTGITHUB=<checkout>.
public-sources: private-refs

# make deps renders the floors of packaging/deps.txt with that checkout's deps-render, make deps-check refuses drift.
DOTGITHUB ?=
.PHONY: private-refs deps deps-check
private-refs:
	@test -n "$(DOTGITHUB)" || { echo "$@: name a checkout of FreeMixer/.github: make $@ DOTGITHUB=<dir>" >&2; exit 2; }
	$(DOTGITHUB)/.github/actions/private-refs-check/check.sh --self-test
	$(DOTGITHUB)/.github/actions/private-refs-check/check.sh .
deps:
	@test -n "$(DOTGITHUB)" || { echo "$@: name a checkout of FreeMixer/.github: make $@ DOTGITHUB=<dir>" >&2; exit 2; }
	$(DOTGITHUB)/.github/actions/deps-render/render.sh --write .
deps-check:
	@test -n "$(DOTGITHUB)" || { echo "$@: name a checkout of FreeMixer/.github: make $@ DOTGITHUB=<dir>" >&2; exit 2; }
	$(DOTGITHUB)/.github/actions/deps-render/render.sh --check .

# omx_log10f() (test/support) against the libm's log10f on all 2^31 non-negative floats: proof of correct rounding
# where the libm's log10f is correctly rounded (glibc 2.43), so not part of make test.
check-log10f: tools/log10f-check.c test/support/log10f_cr.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -pthread -o $(BUILD)/log10f-check tools/log10f-check.c -lm
	./$(BUILD)/log10f-check

# ---- the effect kernels ------------------------------------------------------------------------
# Each kernel's oracle runs every arm at every rate in OMX_DECLARED_RATES and refuses to run if
# 44.1, 48, 96 or 192 kHz is missing; the golden digests hold the kernel's output bit for bit.
# The dynamics, balance, rotor, limiter, loudness and multiband oracles and digests, the pitch kernel's and
# instance face's oracles, and the fractional delay line's exact-copy and tone arms, run at the nine oracle rates instead
# (OMX_FX_ORACLE_RATES in test/fx/fx_rates.h), 32, 64 and 128 kHz included.

# One list, one set of rules: a kernel lane adds its name to FX_KERNELS and its three sources,
#   test/fx/<k>.test.c         the oracle, release flags against the plain archive unless the
#                              kernel names contracts below
#   test/fx/<k>_math.test.c    the contracts battery, contracts compiled in, the ledger read;
#                              a kernel whose oracle reads the ledger itself may have none
#   test/fx/<k>_golden.test.c  the golden digests, compared with test/golden/<k>.sha256
FX_KERNELS = delay geq pitch transient drive chorus flanger phaser reverb tremolo rotor rotary transient_instance geq_instance eq_instance limiter dynamics_keyed dynamics gate gate_instance gate_knee gate_keyfilter comp_instance band_dyn deesser dyn_controls trim_instance align reverse_render dynamics_feedback idelay loudness mbcomp dyneq_instance mbcomp_instance doubler_instance tone_instance leveler_instance plosive_instance width_instance suboct_instance clipper_instance upward_instance glue_instance loudlevel_instance spring_instance balancer_instance
# Oracles that read the contract ledger themselves build with contracts and threads.
FX_CONTRACT_ORACLES = delay_taps geq pitch transient chorus flanger phaser tremolo rotor rotary transient_instance geq_instance eq_instance limiter dynamics_keyed dynamics gate gate_instance gate_knee gate_keyfilter comp_instance band_dyn dyn_controls trim_instance align_fdelay dynamics_feedback idelay loudness mbcomp geq_true dyneq_instance comp_key mbcomp_instance delay_color gate_key reverb_return doubler_instance tone_instance dyn_upward leveler_instance comp_release plosive_instance transient_readouts width_instance suboct_instance clipper_instance upward_instance glue_instance loudlevel_instance spring_instance balancer_instance $(addsuffix _instance,$(FX_INSTANCES))
# Further oracles of a kernel in FX_KERNELS, each test/fx/<o>.test.c, built by the same rule.
FX_ORACLES = align_fdelay delay_taps dyn_flat gaincomp_fpflags geq_true band_dyn_poly comp_key delay_color gate_key reverb_return dyn_upward comp_release transient_readouts
# The effects that carry a host-agnostic instance core, include/omxdsp/fx/omx_<k>_instance.h: each
# adds test/fx/<k>_instance.test.c, its oracle at every declared rate, contracts compiled in.
FX_INSTANCES = chorus flanger drive reverb tremolo phaser pitch rotary limiter deesser delay
# The instance faces with golden digests of their own: test/fx/<k>_instance_golden.test.c,
# compared with test/golden/<k>_instance.sha256.
FX_INSTANCE_GOLDENS = pitch
# The further oracles with golden digests of their own: test/fx/<o>_golden.test.c, compared with
# test/golden/<o>.sha256.
FX_ORACLE_GOLDENS = delay_taps comp_key delay_color gate_key reverb_return
# A kernel's perturbation arm: test/fx/<k>-perturb.sh builds its oracle against a moved declaration.
FX_PERTURB = $(wildcard test/fx/*-perturb.sh)
FX_DEPS = $(LIB) $(LIB_CONTRACTS) $(HEADERS) $(FX_HEADERS) $(FX_TESTS) | $(BUILD)

$(BUILD)/fx_%_math: test/fx/%_math.test.c $(FX_DEPS)
	$(CC) $(TESTFLAGS) -pthread -Itest/fx -o $@ $< $(LIB_CONTRACTS) -lm

# A golden TU is compiled with log10f replaced by the correctly rounded omx_log10f (test/support),
# so its digest is exact on every glibc; production calls the libm's log10f.
$(BUILD)/fx_%_golden: test/fx/%_golden.test.c $(FX_DEPS) $(SUPPORT_HEADERS)
	$(CC) $(CFLAGS_PLAIN) $(INC) -Itest/fx $(GOLDEN_SUBST) -o $@ $< $(LIB) -lm

# An oracle that proves a face allocates nothing links with the allocators wrapped (FX_LDFLAGS_<k>).
FX_NO_ALLOC_LDFLAGS = -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
FX_LDFLAGS_pitch_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_limiter_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_geq_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_geq_true = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_drive_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_chorus_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_flanger_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_reverb_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_deesser_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_delay_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_eq_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_comp_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_gate_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_gate_knee = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_gate_keyfilter = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_trim_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_loudness = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_mbcomp = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_dyneq_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_comp_key = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_mbcomp_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_delay_color = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_gate_key = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_reverb_return = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_doubler_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_tone_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_leveler_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_plosive_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_width_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_clipper_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_upward_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_glue_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_loudlevel_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_spring_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_balancer_instance = $(FX_NO_ALLOC_LDFLAGS)
FX_LDFLAGS_suboct_instance = $(FX_NO_ALLOC_LDFLAGS)

$(BUILD)/fx_%: test/fx/%.test.c $(FX_DEPS)
	$(if $(filter $*,$(FX_CONTRACT_ORACLES)),$(CC) $(TESTFLAGS) -pthread -Itest/fx -o $@ $< $(LIB_CONTRACTS) -lm $(FX_LDFLAGS_$*),$(CC) $(CFLAGS) $(INC) -Itest/fx -o $@ $< $(LIB) -lm $(FX_LDFLAGS_$*))

FX_BINS = $(foreach k,$(FX_KERNELS),$(BUILD)/fx_$(k) $(if $(wildcard test/fx/$(k)_math.test.c),$(BUILD)/fx_$(k)_math) $(BUILD)/fx_$(k)_golden) \
          $(foreach k,$(FX_INSTANCES),$(BUILD)/fx_$(k)_instance) \
          $(foreach o,$(FX_ORACLES),$(BUILD)/fx_$(o)) \
          $(foreach k,$(FX_INSTANCE_GOLDENS),$(BUILD)/fx_$(k)_instance_golden) \
          $(foreach o,$(FX_ORACLE_GOLDENS),$(BUILD)/fx_$(o)_golden)

# test-fx is one phony target per thing it runs, so `make -jN test-fx` runs them side by side. The
# targets and the order of the prerequisites are the loops' that stood here: the same binaries, with
# the same arguments, and a binary that exits non-zero fails its target and so test-fx.
FX_RUN_KERNELS  = $(addprefix fx-run-kernel-,$(FX_KERNELS))
FX_RUN_MATHS    = $(foreach k,$(FX_KERNELS),$(if $(wildcard test/fx/$(k)_math.test.c),fx-run-math-$(k)))
FX_RUN_INSTANCES = $(addprefix fx-run-instance-,$(FX_INSTANCES))
FX_RUN_ORACLES  = $(addprefix fx-run-oracle-,$(FX_ORACLES))
FX_RUN_INSTGOLD = $(addprefix fx-run-instance-golden-,$(FX_INSTANCE_GOLDENS))
FX_RUN_ORAGOLD  = $(addprefix fx-run-oracle-golden-,$(FX_ORACLE_GOLDENS))
FX_RUN_PERTURB  = $(addprefix fx-run-perturb-,$(notdir $(basename $(FX_PERTURB))))
FX_RUN_ALL      = $(FX_RUN_KERNELS) $(FX_RUN_MATHS) $(FX_RUN_INSTANCES) $(FX_RUN_ORACLES) $(FX_RUN_INSTGOLD) $(FX_RUN_ORAGOLD) $(FX_RUN_PERTURB)
.PHONY: $(FX_RUN_ALL)

# fx-oracles is test-fx without the perturbation scripts: what make check-package runs
FX_RUN_ORACLES_ALL = $(filter-out $(FX_RUN_PERTURB),$(FX_RUN_ALL))
fx-oracles: $(FX_RUN_ORACLES_ALL)

test-fx: fx-oracles $(FX_RUN_PERTURB)

$(FX_RUN_KERNELS): fx-run-kernel-%: $(BUILD)/fx_% $(BUILD)/fx_%_golden
	@echo "./$(BUILD)/fx_$*"; ./$(BUILD)/fx_$*
	@echo "./$(BUILD)/fx_$*_golden test/golden/$*.sha256"; ./$(BUILD)/fx_$*_golden test/golden/$*.sha256

$(FX_RUN_MATHS): fx-run-math-%: $(BUILD)/fx_%_math
	@echo "./$(BUILD)/fx_$*_math"; ./$(BUILD)/fx_$*_math

$(FX_RUN_INSTANCES): fx-run-instance-%: $(BUILD)/fx_%_instance
	@echo "./$(BUILD)/fx_$*_instance"; ./$(BUILD)/fx_$*_instance

$(FX_RUN_ORACLES): fx-run-oracle-%: $(BUILD)/fx_%
	@echo "./$(BUILD)/fx_$*"; ./$(BUILD)/fx_$*

$(FX_RUN_INSTGOLD): fx-run-instance-golden-%: $(BUILD)/fx_%_instance_golden
	@echo "./$(BUILD)/fx_$*_instance_golden test/golden/$*_instance.sha256"; \
	  ./$(BUILD)/fx_$*_instance_golden test/golden/$*_instance.sha256

$(FX_RUN_ORAGOLD): fx-run-oracle-golden-%: $(BUILD)/fx_%_golden
	@echo "./$(BUILD)/fx_$*_golden test/golden/$*.sha256"; ./$(BUILD)/fx_$*_golden test/golden/$*.sha256

$(foreach p,$(FX_PERTURB),$(eval fx-run-perturb-$(notdir $(basename $(p))): $(p) $(LIB) $(HEADERS) $(FX_HEADERS) $(FX_TESTS) | $(BUILD)))
$(FX_RUN_PERTURB): fx-run-perturb-%:
	@echo "bash test/fx/$*.sh"; CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/fx/$*.sh $(BUILD)/fx-perturb/$*

# The trim and eq faces against omx-plugins' channel strip's own input stage (omx_strip.h): a desk
# check, the strip lives in another repository (tools/strip-input-identity.sh).
strip-input-identity: $(LIB)
	@test -n "$(OMX_PLUGINS)" || { echo "make strip-input-identity OMX_PLUGINS=<omx-plugins checkout>"; exit 2; }
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/strip-input-identity.sh "$(OMX_PLUGINS)"
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/strip-input-identity.sh --self-test "$(OMX_PLUGINS)"

golden-write: $(foreach k,$(FX_KERNELS),$(BUILD)/fx_$(k)_golden) $(foreach k,$(FX_INSTANCE_GOLDENS),$(BUILD)/fx_$(k)_instance_golden) $(foreach o,$(FX_ORACLE_GOLDENS),$(BUILD)/fx_$(o)_golden) $(BUILD)/eq_bandwidth_q_table
	./$(BUILD)/eq_bandwidth_q_table > test/golden/eq_bandwidth_q.json
	@set -e; for k in $(FX_KERNELS); do ./$(BUILD)/fx_$${k}_golden --write > test/golden/$$k.sha256; done
	@set -e; for k in $(FX_INSTANCE_GOLDENS); do \
	  ./$(BUILD)/fx_$${k}_instance_golden --write > test/golden/$${k}_instance.sha256; \
	done
	@set -e; for o in $(FX_ORACLE_GOLDENS); do ./$(BUILD)/fx_$${o}_golden --write > test/golden/$$o.sha256; done

# ---- the analysis engines ---------------------------------------------------------------------
# FBS feedback detection and HRP read a spectrum and answer what rings and what to cut; they run on
# a control thread, never in the audio callback. omx_rta.h is the capture ring
# and the spectrum they read. Each test/analysis/<t>.test.c is one program at
# release flags against the plain archive; -Isrc lets a test include the compiled unit it checks.
# rates runs the HRP chain at the nine oracle rates and counts allocations through wrapped allocators;
# golden compares every answer of both engines with test/golden/analysis.sha256, which the engine's
# own copy of this code wrote before it moved here; its log10f is the correctly rounded omx_log10f of test/support, wrapped in at link time.
# make analysis-engine-identity OPENMIXER=<checkout> writes the rta, render and live digests again from
# the engine's own copies and compares them (tools/analysis-engine-identity.sh), a desk check.
# test/analysis/sabotage.sh breaks one line of each kernel (fbs, rta, render, live) in a scratch copy
# and requires the golden digests of that kernel to move.
# hrp_lv2_rates holds the live HRP shell's own decisions (the hop, when the worker is asked for, the
# cursor, the adopted band's transfer function, the walk out) and the calls a plugin face drives it
# through (the analysis block, the control snapshot, the pass counters, wants and answer, clear and
# restart) to closed forms at the nine oracle rates; test/analysis/hrp-lv2-rates-sabotage.sh breaks each
# of them once and requires its arm to fail.
# The room analysis, the Farina sweep and the alignment fit (moved from core's TypeScript) run on the same
# thread: room_ts holds them to core's TypeScript through the parity table test/analysis/fixtures/room_ts.h;
# align_fit, room and sweep hold them to closed forms at the nine oracle rates and count allocations;
# test/analysis/room-sabotage.sh breaks each of their kernels once and requires both kinds to fail.
ANALYSIS_TESTS = fbs_detect rta hrp_pitch hrp_track hrp_attribute hrp_baseline hrp_correct hrp_render hrp_lv2 hrp_lv2_rates rates golden \
                 room_ts align_fit room sweep
ANALYSIS_LDFLAGS_rates = -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
ANALYSIS_LDFLAGS_golden = -Wl,--wrap=log10f
ANALYSIS_LDFLAGS_sweep = -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
ANALYSIS_LDFLAGS_room = -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
ANALYSIS_LDFLAGS_align_fit = -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
ANALYSIS_DEPS = $(LIB) $(HEADERS) $(ANALYSIS_HEADERS) $(wildcard test/analysis/*.h) $(wildcard test/analysis/fixtures/*.h) \
                test/fx/sha256.h $(SUPPORT_HEADERS) | $(BUILD)

$(BUILD)/analysis_%: test/analysis/%.test.c $(ANALYSIS_DEPS)
	$(CC) $(CFLAGS) $(INC) -Isrc -Itest/analysis -Itest/fx $(ANALYSIS_LDFLAGS_$*) -o $@ $< $(LIB) -lm

analysis-oracles: $(foreach t,$(ANALYSIS_TESTS),$(BUILD)/analysis_$(t))
	@set -e; for t in $(ANALYSIS_TESTS); do echo "./$(BUILD)/analysis_$$t"; ./$(BUILD)/analysis_$$t; done

test-analysis: analysis-oracles
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/analysis/sabotage.sh $(BUILD)/analysis-sabotage
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/analysis/room-sabotage.sh $(BUILD)/room-sabotage
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/analysis/hrp-lv2-rates-sabotage.sh $(BUILD)/hrp-lv2-rates-sabotage

analysis-engine-identity: $(LIB)
	@test -n "$(OPENMIXER)" || { echo "make analysis-engine-identity OPENMIXER=<openmixer checkout>"; exit 2; }
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/analysis-engine-identity.sh "$(OPENMIXER)"
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/analysis-engine-identity.sh --self-test "$(OPENMIXER)"

# core's TypeScript room analysis and alignment fit, run from an OpenMixer
# checkout under node 22.6 or later with omx-contract's TypeScript render (OMX_CONTRACT_TS, the dist/ts of
# the installed release), must write the committed parity table again, its
# provenance line aside: a desk check, not part of make test and not run in CI.
OMX_CONTRACT_TS ?=
room-ts-golden: | $(BUILD)
	@test -n "$(OPENMIXER)" || { echo "make room-ts-golden OPENMIXER=<openmixer checkout>"; exit 2; }
	@test -f "$(OMX_CONTRACT_TS)/omx-contract.js" || { echo "room-ts-golden: no omx-contract TypeScript render in $(OMX_CONTRACT_TS); set OMX_CONTRACT_TS"; exit 2; }
	node --experimental-strip-types tools/room-ts-golden.mjs "$(OPENMIXER)" "$(OMX_CONTRACT_TS)" > $(BUILD)/room_ts.h
	diff -u -I '^/\* GENERATED by' test/analysis/fixtures/room_ts.h $(BUILD)/room_ts.h
	@echo "room-ts-golden: core's TypeScript writes the committed parity table"

# ---- the kernels moved from the engine ---------------------------------------------------------
# The sample-rate converter, the generators and the analyser's readings (moved from the engine):
# their oracles are suite arms (test/kernels/src.c, gen.c, rta.c); here are their golden digests at
# the nine oracle rates, which the engine's own copies wrote (test/golden/engine_kernels.sha256), and
# a break in each kernel that must move its own digests and no other's.
$(BUILD)/engine_kernels_golden: test/engine_kernels/golden.test.c test/fx/sha256.h $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< -lm

engine-kernels-golden: $(BUILD)/engine_kernels_golden
	./$(BUILD)/engine_kernels_golden test/golden/engine_kernels.sha256

test-engine-kernels: engine-kernels-golden
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/engine_kernels/sabotage.sh $(BUILD)/engine-kernels-sabotage

# The same digests through the engine's copies: needs a checkout of the private OpenMixer repository,
# so it is a desk check, not part of make test and not run in CI (BUILDING.md, "The engine's copies").
# The console checkout's package directory, where the identity recipes read the engine's native sources.
CONSOLE_PACKAGES = $(OPENMIXER)/packages
engine-kernels-identity:
	@test -n "$(OPENMIXER)" || { echo "make engine-kernels-identity OPENMIXER=<openmixer checkout>"; exit 2; }
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/engine-kernels-identity.sh "$(CONSOLE_PACKAGES)/pipewire-native/src"
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/engine-kernels-identity.sh --self-test "$(CONSOLE_PACKAGES)/pipewire-native/src"

# ---- the mix module -------------------------------------------------------------------------
# The pan law, the strip and fold coefficients of G, a bus's out stage, the meters' measure half, one
# analyser tap's reading, one source's contribution to a summing bus, the real-time load meter, the
# factored strip coefficient, an output route's gain stages, the settled-silence test, the LCR
# fold's centre level, the bus stages, a dynamics slot's key, the source switch and a level meter's
# dBFS reading (include/omxdsp/mix/, FreeMixer/omx-dsp#98 and #140): their oracles are suite arms (test/kernels/mix_*.c);
# here are their golden digests at the nine oracle rates, which the engine's own copies wrote (the
# level reading's, which has no engine copy, the library's own)
# (test/golden/mix.sha256), and a break in each kernel that must turn its arm and its digests red.
# Compiled with log10f replaced by the correctly rounded omx_log10f (test/support), as the FX goldens
# are: the gain-reduction reading's log10f otherwise moves its digests on glibc 2.36 and 2.39.
$(BUILD)/mix_golden: test/mix/golden.test.c test/fx/sha256.h $(HEADERS) $(MIX_HEADERS) $(SUPPORT_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS_PLAIN) $(INC) $(GOLDEN_SUBST) -o $@ $< -lm

mix-golden: $(BUILD)/mix_golden
	./$(BUILD)/mix_golden test/golden/mix.sha256

test-mix: mix-golden $(LIB_CONTRACTS)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/mix/sabotage.sh $(BUILD)/mix-sabotage $(LIB_CONTRACTS)

# The same digests through the engine's copies: a desk check, as engine-kernels-identity is.
mix-engine-identity:
	@test -n "$(OPENMIXER)" || { echo "make mix-engine-identity OPENMIXER=<openmixer checkout>"; exit 2; }
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/mix-engine-identity.sh "$(CONSOLE_PACKAGES)/pipewire-native/src"
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/mix-engine-identity.sh --self-test "$(CONSOLE_PACKAGES)/pipewire-native/src"

# Each archive carries its flavour (its symbols say so), and the contracts consumer reads a
# violation raised inside the compiled unit only when linked against the contracts archive: linked
# against the plain one it must fail, or the check is blind.
flavours: $(LIBS_ALL) test/omxdsp_flavour.c $(HEADERS) | $(BUILD)
	CC="$(CC)" NM="$(NM)" bash tools/flavour-check.sh $(BUILD)/flavour "$(TESTFLAGS)" \
	  $(LIB) $(LIB_CONTRACTS) $(LIB_TSAN)

# copy-check runs alone, before the rest: under the test job's -j it once missed a planted copy
# while the other checks ran beside it (#227), and a check that can miss a planted copy can pass a
# real one. The rest runs in parallel from a second make, which shares the parent's job slots.
test:
	$(MAKE) --no-print-directory copy-check
	$(MAKE) --no-print-directory test-parallel
	@echo "omxdsp: make test green"

# ---- libmvec (include/omxdsp/omx_fastmath.h) --------------------------------------------------------
# fastmath          the oracle of tanhf, expf, log10f and powf over the library's range, and the check that every
#                   hot loop (the drive's shaper, the dynamics gain curves) really calls the libmvec symbols and no
#                   libm one, and the block size is not observable through those loops (fastmath_blocks)
# fastmath-diff     each kernel that uses them, rendered on the libm and on libmvec: the largest sample difference in dB
# fastmath-sabotage the oracle goes red on a result moved by 5 ULP, the loop check on a header without the pragma,
#                   the probe on a toolchain with no libmvec, no pragma or no vector symbol, the loop check on a
#                   loop whose remainder runs on the libm
# fastmath-plain    the same oracle, the drive and a dynamics oracle again with FASTMATH=off: the libm path
# fastmath-partial  the flags of a libmvec with log10f and expf but no powf and no tanhf (aarch64 glibc before 2.40),
#                   on any host: no loop that needs a missing function takes its lane, every render is the plain
#                   libm build's bit for bit, and the loop check passes on that set
# Without libmvec in the build (the probe found nothing, or FASTMATH=off) each says so and passes.
FM_DIFF_LIMIT_DBFS ?= -96

$(BUILD)/fastmath: test/fx/fastmath.test.c include/omxdsp/omx_fastmath.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< -lm $(MVEC_LIBS)

# the block size through the libmvec path: built as a consumer builds the kernels, contracts out, libmvec flags in
$(BUILD)/fastmath_blocks: test/fx/fastmath_blocks.test.c $(LIB) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $< $(LIB) -lm $(MVEC_LIBS)

$(BUILD)/fastmath_sabotage: test/fx/fastmath.test.c include/omxdsp/omx_fastmath.h | $(BUILD)
	$(CC) $(CFLAGS) -DOMX_FM_ORACLE_SABOTAGE=1 $(INC) -o $@ $< -lm $(MVEC_LIBS)

fastmath: $(BUILD)/fastmath $(BUILD)/fastmath_blocks
	./$(BUILD)/fastmath
	./$(BUILD)/fastmath_blocks
	sh tools/fastmath-vec-check.sh "$(CC)" "$(INC) $(FPFLAGS)" $(MVEC_CFLAGS)

fastmath-diff: $(LIB)
	sh tools/fastmath-diff.sh "$(CC)" "$(CFLAGS_PLAIN)" $(LIB) $(FM_DIFF_LIMIT_DBFS) $(MVEC_CFLAGS)

fastmath-sabotage: $(BUILD)/fastmath_sabotage
	@if ./$(BUILD)/fastmath_sabotage > $(BUILD)/fastmath_sabotage.log 2>&1; then \
	  echo "fastmath-sabotage: FAIL, results moved by 5 ULP left the oracle green"; exit 1; fi
	@grep -q "FAIL tanhf error" $(BUILD)/fastmath_sabotage.log && grep -q "FAIL log10f error" $(BUILD)/fastmath_sabotage.log || \
	  { echo "fastmath-sabotage: FAIL, red, but not on the error arms"; tail -3 $(BUILD)/fastmath_sabotage.log; exit 1; }
	@echo "fastmath-sabotage: the oracle goes red on results moved by 5 ULP"
	sh tools/fastmath-vec-check.sh --self-test "$(CC)" "$(INC) $(FPFLAGS)" $(MVEC_CFLAGS)
	sh tools/fastmath-probe-selftest.sh "$(CC)"

fastmath-plain:
	$(MAKE) --no-print-directory FASTMATH=off BUILD=$(BUILD)/plain fastmath fx-run-kernel-drive fx-run-oracle-dyn_flat fx-run-kernel-dynamics

FM_PARTIAL_CFLAGS = -fopenmp-simd -fno-math-errno -DOMX_FASTMATH_MVEC_LOG10F=1 -DOMX_FASTMATH_MVEC_EXPF=1
$(BUILD)/fastmath_partial: test/fx/fastmath_partial.test.c test/fx/fastmath_partial_render.c $(LIB) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS_PLAIN) $(FM_PARTIAL_CFLAGS) -DOMX_PARTIAL_SIDE=partial $(INC) -c -o $(BUILD)/fastmath_partial_p.o test/fx/fastmath_partial_render.c
	$(CC) $(CFLAGS_PLAIN) -DOMX_PARTIAL_SIDE=plain $(INC) -c -o $(BUILD)/fastmath_partial_l.o test/fx/fastmath_partial_render.c
	$(CC) $(CFLAGS_PLAIN) $(INC) -o $@ $< $(BUILD)/fastmath_partial_p.o $(BUILD)/fastmath_partial_l.o $(LIB) -lm $(MVEC_LIBS)

fastmath-partial: $(BUILD)/fastmath_partial
	./$(BUILD)/fastmath_partial
	sh tools/fastmath-vec-check.sh "$(CC)" "$(INC) $(FPFLAGS)" $(FM_PARTIAL_CFLAGS)

.PHONY: fastmath fastmath-diff fastmath-sabotage fastmath-plain fastmath-partial

# ---- omx-rt-gate: the one static real-time gate (tools/rt-gate, omx-rt-gate(1)) -----------------------
# rt-gate           the gate built; the externals arm measures every row of the class table (each row names
#                   its case, and every case must read zero allocations); the self-test judges every planted
#                   fixture red, at its place and through its path, every clean twin green, and refuses at
#                   load every waiver-shaped, unreasoned, unmeasured, duplicate or forbidden row
# rt-gate-sabotage  the forbidden list and the class table emptied (the self-test must go red), the -O2 arm
#                   (dead-code removal takes the planted malloc out of the graph: why the input is -O0), the
#                   externals arm's own sabotage
RT_GATE_DIR    = $(BUILD)/rt-gate
RT_GATE        = $(RT_GATE_DIR)/omx-rt-gate
RT_GATE_ARM    = $(RT_GATE_DIR)/externals-arm
RT_GATE_TABLES = tools/rt-gate/tables
RT_GATE_FIXTURES = $(wildcard tools/rt-gate/fixtures/*)

$(RT_GATE): tools/rt-gate/omx-rt-gate.c | $(BUILD)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_PLAIN) -DOMX_RT_GATE_DATADIR='"$(DATADIR)/omxdsp/rt-gate"' -o $@ $< $(LDFLAGS)

# -O0 -fno-builtin: every case calls the real symbol, none folded or inlined away
$(RT_GATE_ARM): tools/rt-gate/externals-arm.c | $(BUILD)
	@mkdir -p $(@D)
	$(CC) -O0 -fno-builtin -Wall -Wextra -Werror -o $@ $< -lm

# the thread tags: every function the shipped headers declare carries exactly one `@thread audio|worker|control`
# in the comment above it, under each set of flags a consumer builds with (the plain libm, this toolchain's
# libmvec, contracts compiled in); the shipped tags are their union. The header omx-contract renders from data/
# is left out: it is the render, and its items are documented in data/
RT_GATE_HEADERS_UNIT = $(RT_GATE_DIR)/headers.c
RT_GATE_TAGS         = $(RT_GATE_DIR)/omx-dsp.tags
RT_GATE_FLAVOURS     = plain release contracts
RT_GATE_FLAGS_plain     = $(CFLAGS_PLAIN) $(INC)
RT_GATE_FLAGS_release   = $(CFLAGS) $(INC)
RT_GATE_FLAGS_contracts = $(CFLAGS) $(CONTRACTS_CFLAGS) $(INC)

$(RT_GATE_HEADERS_UNIT): $(HEADERS) $(FX_HEADERS) $(ANALYSIS_HEADERS) $(MIX_HEADERS) | $(BUILD)
	@mkdir -p $(@D)
	@for h in $(sort $(patsubst include/%,%,$^)); do echo "#include <$$h>"; done > $@

$(RT_GATE_DIR)/%.tags: $(RT_GATE) $(RT_GATE_HEADERS_UNIT)
	./$(RT_GATE) --thread-tags $@ --selfrun $(RT_GATE_DIR)/selfrun-$*.c --under include/omxdsp \
	  --except include/omxdsp/omxdsp_definitions.h --work $(RT_GATE_DIR)/tags-$* \
	  -- $(CC) $(RT_GATE_FLAGS_$*) $(RT_GATE_HEADERS_UNIT)

$(RT_GATE_TAGS): $(foreach f,$(RT_GATE_FLAVOURS),$(RT_GATE_DIR)/$(f).tags)
	awk '/^#/ { next } !seen[$$1]++' $^ | sort > $@

rt-gate-tags: $(RT_GATE_TAGS)
	@echo "rt-gate-tags: $$(wc -l < $(RT_GATE_TAGS)) functions, each with exactly one @thread tag"

# the call graphs of the compiled units, per set of flags (-O0 -fno-inline: the gate's input, never -O2); the
# release ones ship, for the consumers' join
RT_GATE_GRAPHS = $(RT_GATE_DIR)/graphs-release
# the gate compiles them itself, so a graph is read the way a consumer's unit is (-O0 -fno-inline, the
# fortification and the stack protector taken out), whatever flags the packaging build adds
$(RT_GATE_DIR)/graphs-%.stamp: $(RT_GATE) $(SRC) $(HEADERS) $(ANALYSIS_HEADERS) | $(BUILD)
	./$(RT_GATE) --write-graphs $(RT_GATE_DIR)/graphs-$* -- $(CC) $(RT_GATE_FLAGS_$*) $(SRC)
	@touch $@

# the self-run: the generated unit takes the address of every tagged function, each audio one an audio root
# and each worker one a worker root (omxdsp.abi), joined to the compiled units' graphs; every tagged audio
# and worker function must be a root walked, and nothing red
rt-gate-selfrun-%: $(RT_GATE) $(RT_GATE_DIR)/%.tags $(RT_GATE_DIR)/graphs-%.stamp
	./$(RT_GATE) --tables $(RT_GATE_TABLES) --abi omxdsp --tags $(RT_GATE_DIR)/$*.tags --graphs $(RT_GATE_DIR)/graphs-$* \
	  --work $(RT_GATE_DIR)/selfrun-$*.work -- $(CC) $(RT_GATE_FLAGS_$*) $(RT_GATE_DIR)/selfrun-$*.c > $(RT_GATE_DIR)/selfrun-$*.out
	@want=$$(awk '$$2 == "audio" || $$2 == "worker"' $(RT_GATE_DIR)/$*.tags | wc -l); \
	  got=$$(sed -n 's/^SUMMARY roots=\([0-9]*\) .*/\1/p' $(RT_GATE_DIR)/selfrun-$*.out); \
	  [ "$$got" = "$$want" ] || { echo "rt-gate-selfrun-$*: $$got roots walked, $$want tagged audio or worker"; exit 1; }; \
	  echo "rt-gate-selfrun-$*: $$(sed -n 's/^SUMMARY //p' $(RT_GATE_DIR)/selfrun-$*.out)"

rt-gate-selfrun: $(foreach f,$(RT_GATE_FLAVOURS),rt-gate-selfrun-$(f))

.SECONDARY: $(foreach f,$(RT_GATE_FLAVOURS),$(RT_GATE_DIR)/graphs-$(f).stamp $(RT_GATE_DIR)/$(f).tags)

# what make install ships of the gate, and the installed gate judged where it lies: with no --tables, --tags or
# --graphs it reads its own share directory, joins a consumer to omx-dsp's graph and reads it red and green
rt-gate-ship: $(RT_GATE) $(RT_GATE_TAGS) $(RT_GATE_DIR)/graphs-release.stamp

install-rt-gate: rt-gate-ship
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1 $(DESTDIR)$(DATADIR)/omxdsp/rt-gate/tables $(DESTDIR)$(DATADIR)/omxdsp/rt-gate/graphs
	install -m 0755 $(RT_GATE) $(DESTDIR)$(BINDIR)/omx-rt-gate
	install -m 0644 tools/rt-gate/omx-rt-gate.1 $(DESTDIR)$(MANDIR)/man1/
	install -m 0644 $(RT_GATE_TABLES)/*.tbl $(RT_GATE_TABLES)/*.abi $(DESTDIR)$(DATADIR)/omxdsp/rt-gate/tables/
	install -m 0644 $(RT_GATE_TAGS) $(DESTDIR)$(DATADIR)/omxdsp/rt-gate/
	install -m 0644 $(RT_GATE_GRAPHS)/*.ci $(DESTDIR)$(DATADIR)/omxdsp/rt-gate/graphs/

rt-gate-installed: rt-gate-ship | $(BUILD)
	rm -rf $(RT_GATE_DIR)/root
	$(MAKE) --no-print-directory install-rt-gate DESTDIR=$(abspath $(RT_GATE_DIR)/root) PREFIX=/usr
	@cd tools/rt-gate && for f in join.c join.clean.c; do \
	  want=$$(sed -n 's|^// expect: ||p' fixtures/$$f); \
	  $(abspath $(RT_GATE_DIR)/root)/usr/bin/omx-rt-gate --abi lv2 --work $(abspath $(RT_GATE_DIR))/installed-$$f -- \
	    $(CC) -Wall -Wextra -Werror -O2 -Ifixtures -I$(abspath include) -isystem $(CONTRACT_INC) $(FPFLAGS) fixtures/$$f \
	    > $(abspath $(RT_GATE_DIR))/installed-$$f.out 2>&1; rc=$$?; \
	  [ "$$rc" = "$$want" ] || { cat $(abspath $(RT_GATE_DIR))/installed-$$f.out; echo "rt-gate-installed: $$f exit $$rc, want $$want"; exit 1; }; \
	  sed -n 's|^// expect-red: ||p' fixtures/$$f | sort > $(abspath $(RT_GATE_DIR))/installed-$$f.want; \
	  grep '^RT-' $(abspath $(RT_GATE_DIR))/installed-$$f.out | sort > $(abspath $(RT_GATE_DIR))/installed-$$f.got; \
	  diff $(abspath $(RT_GATE_DIR))/installed-$$f.want $(abspath $(RT_GATE_DIR))/installed-$$f.got || \
	    { echo "rt-gate-installed: $$f reds differ"; exit 1; }; \
	  echo "rt-gate-installed: the installed gate judges fixtures/$$f exit $$rc"; done

.PHONY: rt-gate rt-gate-arm rt-gate-selftest rt-gate-sabotage rt-gate-tags rt-gate-selfrun rt-gate-ship install-rt-gate rt-gate-installed
rt-gate-arm: $(RT_GATE_ARM) $(RT_GATE_TABLES)/classes.tbl
	@awk '!/^#/ && NF && $$3 !~ /^externals-arm:/ { print "rt-gate-arm: " FILENAME ":" FNR ": " $$1 " names no externals-arm case"; bad = 1 } END { exit bad }' $(RT_GATE_TABLES)/classes.tbl
	./$(RT_GATE_ARM) $$(awk '!/^#/ && NF { sub(/^externals-arm:/, "", $$3); print $$3 }' $(RT_GATE_TABLES)/classes.tbl)

rt-gate-selftest: $(RT_GATE) $(RT_GATE_FIXTURES) $(wildcard $(RT_GATE_TABLES)/*) $(RT_GATE_TAGS) $(RT_GATE_DIR)/graphs-release.stamp
	CC="$(CC)" TMPDIR="$(abspath $(BUILD))" bash tools/rt-gate/selftest.sh $(abspath $(RT_GATE)) $(abspath $(RT_GATE_TABLES)) \
	  --graphs $(abspath $(RT_GATE_GRAPHS)) --tags $(abspath $(RT_GATE_TAGS)) --cflags "-I$(abspath include) -isystem $(CONTRACT_INC) $(FPFLAGS)"

rt-gate: rt-gate-arm rt-gate-selftest rt-gate-tags rt-gate-selfrun rt-gate-installed
	@echo "rt-gate: the gate reads every planted red, passes every clean twin and refuses every bad row"

rt-gate-sabotage: $(RT_GATE) $(RT_GATE_ARM) $(RT_GATE_FIXTURES)
	CC="$(CC)" TMPDIR="$(abspath $(BUILD))" bash tools/rt-gate/sabotage.sh $(abspath $(RT_GATE)) $(abspath $(RT_GATE_TABLES)) $(abspath $(RT_GATE_ARM)) \
	  "-I$(abspath include) -isystem $(CONTRACT_INC) $(CFLAGS)"

# ---- the package build's check --------------------------------------------------------------------
# What debian/rules and the RPM's %check run: the tests that prove the library just built, at every
# declared rate (the suite, the negative arm, the consumer and thread arms, every oracle and golden
# digest, the flavours and the libmvec oracles). The sabotage and perturbation arms, which prove the
# tests rather than the library, and the source checks run in CI's make test on every pull request:
# under make -j4 test the package build went past its 60-minute job on a 4-core runner.
# tools/check-package-check.sh holds this list to make test's (part of make checks).
CHECK_PACKAGE = lib suite negative consumer threads bandwidth-q-table eq-default-bands fx-oracles \
                analysis-oracles engine-kernels-golden mix-golden flavours fastmath fastmath-diff fastmath-partial rt-gate

.PHONY: check-package fx-oracles analysis-oracles engine-kernels-golden mix-golden
check-package: $(CHECK_PACKAGE)
	@echo "omxdsp: make check-package green"

.PHONY: test-parallel
test-parallel: lib checks gen-once suite negative magnitude-sabotage eq-sabotage curve-sabotage meter-view-sabotage geq-true-sabotage delay-taps-sabotage align-fdelay-sabotage pitch-rates-sabotage instance-rates-sabotage bandwidth-q-table bandwidth-q-sabotage eq-default-bands consumer perturb fdelay-sabotage doppler-sabotage onepole-sabotage note-map-sabotage threads threads-sabotage test-fx test-analysis test-engine-kernels test-mix flavours fastmath fastmath-diff fastmath-sabotage fastmath-plain fastmath-partial rt-gate rt-gate-sabotage

# ---- install ------------------------------------------------------------------------------------

version:
	@echo $(VERSION)

PC_SED = -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|' -e 's|@DATADIR@|$(DATADIR)|' \
	 -e 's|@VERSION@|$(VERSION)|' -e 's|@CONTRACTFLOOR@|$(CONTRACT_FLOOR)|' -e 's|@FPFLAGS@|$(FPFLAGS)|'

$(BUILD)/omxdsp.pc: omxdsp.pc.in include/omxdsp/omxdsp.h Makefile packaging/omx-dsp.spec | $(BUILD)
	sed $(PC_SED) -e 's|@NAME@|omxdsp|' -e 's|@FLAVOUR@|release, contracts compiled out|' \
	    -e 's|@CFLAGS@| $(MVEC_CFLAGS)|' -e 's|@LIBS@| $(MVEC_LIBS)|' $< | sed 's/ *$$//' > $@

$(BUILD)/omxdsp-contracts.pc: omxdsp.pc.in include/omxdsp/omxdsp.h Makefile packaging/omx-dsp.spec | $(BUILD)
	sed $(PC_SED) -e 's|@NAME@|omxdsp-contracts|' -e 's|@FLAVOUR@|contracts compiled in|' \
	    -e 's|@CFLAGS@| $(CONTRACTS_CFLAGS) $(MVEC_CFLAGS)|' -e 's|@LIBS@| $(MVEC_LIBS)|' $< | sed 's/ *$$//' > $@

$(BUILD)/omxdsp-tsan.pc: omxdsp.pc.in include/omxdsp/omxdsp.h Makefile packaging/omx-dsp.spec | $(BUILD)
	sed $(PC_SED) -e 's|@NAME@|omxdsp-tsan|' -e 's|@FLAVOUR@|contracts compiled in, under ThreadSanitizer|' \
	    -e 's|@CFLAGS@| $(TSAN_CFLAGS) $(MVEC_CFLAGS)|' -e 's|@LIBS@| $(TSAN_LIBS) $(MVEC_LIBS)|' $< | sed 's/ *$$//' > $@

install: $(LIBS_ALL) $(PC_ALL) $(EXPORTS) install-rt-gate
	install -d $(DESTDIR)$(INCLUDEDIR)/omxdsp/fx $(DESTDIR)$(INCLUDEDIR)/omxdsp/analysis $(DESTDIR)$(INCLUDEDIR)/omxdsp/mix $(DESTDIR)$(LIBDIR)/pkgconfig \
	  $(DESTDIR)$(BINDIR) $(DESTDIR)$(DATADIR)/omxdsp $(DESTDIR)$(MANDIR)/man1
	install -m 0644 include/omxdsp/*.h $(DESTDIR)$(INCLUDEDIR)/omxdsp/
	install -m 0644 include/omxdsp/fx/*.h $(DESTDIR)$(INCLUDEDIR)/omxdsp/fx/
	install -m 0644 include/omxdsp/analysis/*.h $(DESTDIR)$(INCLUDEDIR)/omxdsp/analysis/
	install -m 0644 include/omxdsp/mix/*.h $(DESTDIR)$(INCLUDEDIR)/omxdsp/mix/
	install -m 0644 $(LIBS_ALL) $(DESTDIR)$(LIBDIR)/
	install -m 0644 $(PC_ALL) $(DESTDIR)$(LIBDIR)/pkgconfig/
	@test -n "$(CONTRACTDIR)" && test -f "$(CONTRACT_INSTALLER)" && test -n "$(CONTRACT_MAJOR)" || \
	  { echo "install: the installed omx-contract names no contract directory, installer or schema major (contractdir and datadir of omx-contract.pc): data/ cannot be installed" >&2; exit 1; }
	sh $(CONTRACT_INSTALLER) omx-dsp $(VERSION) $(CONTRACT_MAJOR) data $(DESTDIR)$(CONTRACTDIR) data
	install -m 0644 $(EXPORTS) $(DESTDIR)$(DATADIR)/omxdsp/
	install -m 0644 test/analysis/hrp_peaking_corpus.h $(DESTDIR)$(DATADIR)/omxdsp/
	install -m 0644 vectors/meter-view.json $(DESTDIR)$(DATADIR)/omxdsp/meter-view-vectors.json
	install -m 0755 tools/omx-dsp-copy-check.sh $(DESTDIR)$(BINDIR)/omx-dsp-copy-check
	install -m 0644 omx-dsp-copy-check.1 $(DESTDIR)$(MANDIR)/man1/

# §4.3 (e): UNJUDGED (exit 0, never a pass) where the toolchain has no TSan; where it has, the
# thread arm must be report-free and its shared-state sabotage must race. The gate's own report
# handling is held by tools/tsan-gate-selftest.sh first.
test-tsan: test/omxdsp_threads.c tools/tsan-gate.sh tools/tsan-gate-selftest.sh $(LIB_TSAN) $(HEADERS) | $(BUILD)
	TMPDIR="$(abspath $(BUILD))" bash tools/tsan-gate-selftest.sh
	CC="$(CC)" bash tools/tsan-gate.sh $(BUILD)/tsan omxdsp_threads OMXDSP_THREADS_SABOTAGE_SHARED_STATE \
	  $(TESTFLAGS) -pthread test/omxdsp_threads.c $(LIB_TSAN) -lm

docs: $(HEADERS) | $(BUILD)
	doxygen Doxyfile

clean:
	rm -rf $(BUILD)

cost-prel: tools/prel-cost.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $(BUILD)/prel-cost tools/prel-cost.c -lm
	./$(BUILD)/prel-cost

cost-deesser: tools/deesser-cost.c $(HEADERS) $(FX_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $(BUILD)/deesser-cost tools/deesser-cost.c -lm
	./$(BUILD)/deesser-cost

bench-mixmatrix: tools/bench-mixmatrix.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $(BUILD)/bench-mixmatrix tools/bench-mixmatrix.c -lm
	./$(BUILD)/bench-mixmatrix
	@if ./$(BUILD)/bench-mixmatrix --share 0.000001 > /dev/null 2>&1; then \
	  echo "bench-mixmatrix: FAIL: a budget nothing can meet left the bench green"; exit 1; fi
	@echo "bench-mixmatrix: inside MIXMATRIX_PERIOD_SHARE at the nine rates, and a budget nothing can meet goes red"

bench-gate: tools/gate-cost-bench.c $(LIB) $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $(BUILD)/gate-cost-bench tools/gate-cost-bench.c $(LIB) -lm
	./$(BUILD)/gate-cost-bench

bench-drive: tools/drive-cost.c $(LIB) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $(BUILD)/drive-cost tools/drive-cost.c $(LIB) -lm
	./$(BUILD)/drive-cost

bench-flanger: tools/flanger-cost.c $(LIB) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $(BUILD)/flanger-cost tools/flanger-cost.c $(LIB) -lm
	./$(BUILD)/flanger-cost

# ---- fuzzing: libFuzzer harnesses over the entry points that take outside data (fuzz/README.md) ----
# One harness per fuzz/fuzz_<name>.c, a plain C LLVMFuzzerTestOneInput, linked against a libomxdsp.a of its
# own: the same src/*.c compiled with clang, ASan, UBSan, the library's contracts (-DOMX_CONTRACTS) and
# libFuzzer's coverage hooks into build/fuzz/obj, so the ordinary build above is never touched. None of these
# targets is part of `make test`. The only addition to the toolchain is clang with libFuzzer's runtime
# (apt-get install -y clang libclang-rt-dev).
#
#   make fuzz                   build every harness
#   make fuzz-run FUZZ_TIME=60  run each for the budget from its seed corpus; red on any crash, the
#                               crashing input left in fuzz/findings/
#   make fuzz-corpus            regenerate fuzz/corpus/ from the library's own tables (fuzz/gen-corpus.c)
FUZZ_CC        ?= clang
FUZZ_TIME      ?= 60
FUZZ_BUILD     := $(BUILD)/fuzz
FUZZ_HARNESSES := fx laws eq src delay note_map
FUZZ_SAN       := -fsanitize=address,undefined -fno-sanitize-recover=all
FUZZ_CFLAGS    := -O1 -g -std=gnu11 -Wall -Wextra -fno-omit-frame-pointer $(FUZZ_SAN) -DOMX_CONTRACTS $(FPFLAGS)
FUZZ_BINS      := $(FUZZ_HARNESSES:%=$(FUZZ_BUILD)/fuzz_%)
FUZZ_LIB_OBJS  := $(patsubst src/%.c,$(FUZZ_BUILD)/obj/%.o,$(SRC))
FUZZ_DEPS      := fuzz/fuzz.h $(HEADERS) $(FX_HEADERS) $(MIX_HEADERS) $(ANALYSIS_HEADERS)

$(FUZZ_BUILD)/obj/%.o: src/%.c $(HEADERS) $(ANALYSIS_HEADERS)
	@mkdir -p $(@D)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -fsanitize=fuzzer-no-link $(INC) -c -o $@ $<

$(FUZZ_BUILD)/libomxdsp.a: $(FUZZ_LIB_OBJS)
	$(AR) rcs $@ $(FUZZ_LIB_OBJS)

$(FUZZ_BUILD)/fuzz_%: fuzz/fuzz_%.c $(FUZZ_DEPS) $(FUZZ_BUILD)/libomxdsp.a
	$(FUZZ_CC) $(FUZZ_CFLAGS) -fsanitize=fuzzer $(INC) -Ifuzz $< $(FUZZ_BUILD)/libomxdsp.a -lm -o $@

fuzz: $(FUZZ_BINS)

fuzz-run: fuzz
	FUZZ_BUILD=$(FUZZ_BUILD) FUZZ_TIME=$(FUZZ_TIME) fuzz/run.sh $(FUZZ_HARNESSES)

$(FUZZ_BUILD)/fuzz-gen-corpus: fuzz/gen-corpus.c fuzz/fuzz_fx.c $(FUZZ_DEPS) $(LIB_CONTRACTS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DOMX_CONTRACTS $(INC) -Ifuzz -o $@ fuzz/gen-corpus.c $(LIB_CONTRACTS) -lm

fuzz-corpus: $(FUZZ_BUILD)/fuzz-gen-corpus
	$(FUZZ_BUILD)/fuzz-gen-corpus fuzz/corpus
