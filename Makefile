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
#               into DESTDIR
#   make flavours  each archive carries its flavour, and a contracts consumer sees the compiled
#               unit's contracts only through the contracts archive
#   make test-fx   the effect kernels' oracles and golden digests alone
#   make test-analysis  the analysis engines' oracles (FBS feedback detection, HRP) alone
#   make lint   the doc check and the source scan only
#   make docs   the API reference by doxygen (build-time only)
#   make test-tsan  the thread arm under -fsanitize=thread where the toolchain has it
#   make cost-prel  omx_env_program_release's ns/sample at every declared rate (its cost row)
#   make cost-kernel [PART=gate|flanger|drive|ovs|shape|deesser|delay]  one effect kernel's
#               ns/sample alone (every part by default) at every declared rate, at its defaults and
#               with every optional path engaged, with a digest of its output
#   make bench-mixmatrix  omx_mixmatrix dense/sparse ns per strip-output-frame, 32/64/97 strips x
#               1024 frames
#   make engine-identity OPENMIXER=<checkout>  the golden digests rendered again through the
#               dynamics, balance, rotor and limiter copies OpenMixer's engine still carries, and
#               the check's own sabotages (tools/engine-identity.sh)

CC      ?= cc
AR      ?= ar
NM      ?= nm
CFLAGS  ?= -Wall -Wextra -Werror -O2
# Every consumer compiles the inline kernels without floating-point contraction (omxdsp.pc carries
# the same flag), so a kernel's output does not depend on the architecture's FMA or the consumer's
# build flags: the engine and a plugin built at one version produce the same bits.
FPFLAGS  = -ffp-contract=off
override CFLAGS += $(FPFLAGS)
# The limits come from FreeMixer/omx-contract, the version .github/pins.txt names, resolved by
# tools/contract-include.sh (installed omx-contract-devel, else the release tarball); this tree
# commits no copy of them. Every sub-script that compiles gets the directory as CONTRACT_INC.
ifeq ($(filter version clean docs,$(MAKECMDGOALS)),)
CONTRACT_INC := $(shell sh tools/contract-include.sh)
ifeq ($(CONTRACT_INC),)
$(error tools/contract-include.sh found no omx-contract: see its message above)
endif
export CONTRACT_INC
endif
INC      = -Iinclude -isystem $(CONTRACT_INC)
CONTRACT_VERSION := $(shell awk '$$1 == "omx-contract" { print $$3 }' .github/pins.txt)
BUILD    = build
LIB      = $(BUILD)/libomxdsp.a
HEADERS  = $(wildcard include/omxdsp/*.h)
ANALYSIS_HEADERS = $(wildcard include/omxdsp/analysis/*.h)
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
KERNELS  = $(wildcard test/kernels/*.c)
FX_HEADERS = $(wildcard include/omxdsp/fx/*.h)
FX_TESTS   = $(wildcard test/fx/*.c) $(wildcard test/fx/*.h) $(wildcard test/fx/fixtures/*.h)
SUPPORT_HEADERS = $(wildcard test/support/*.h)
GOLDEN_SUBST    = -include test/support/log10f_subst.h

PREFIX     ?= /usr/local
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
DESTDIR    ?=
VERSION    := $(shell sed -n 's/^\#define OMXDSP_VERSION_\(MAJOR\|MINOR\|PATCH\) \([0-9]*\)$$/\2/p' include/omxdsp/omxdsp.h | paste -sd.)



.PHONY: all lib test lint docs clean test-tsan suite negative perturb threads checks cost-prel cost-kernel test-fx test-analysis install version golden-write flavours bench-mixmatrix check-log10f engine-identity

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

$(BUILD)/omxdsp_suite: test/omxdsp_suite.c $(KERNELS) $(LIB_CONTRACTS) $(HEADERS) tools/kernels-gen.sh | $(BUILD)
	bash tools/kernels-gen.sh $(BUILD)
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

threads: $(BUILD)/omxdsp_threads
	./$(BUILD)/omxdsp_threads

perturb: $(HEADERS) $(KERNELS) tools/kernels-gen.sh | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/perturb.sh $(BUILD)/perturb

checks:
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/writable-data-check.sh $(BUILD)/wd
	bash tools/doc-check.sh
	bash tools/reduction-check.sh
	bash tools/log10f-guard.sh include src
	bash tools/contract-single-source.sh
	bash tools/contract-single-source.sh --self-test

lint: checks

# omx_log10f() (test/support) against the libm's log10f on all 2^31 non-negative floats: proof of correct rounding
# where the libm's log10f is correctly rounded (glibc 2.43), so not part of make test.
check-log10f: tools/log10f-check.c test/support/log10f_cr.h | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -pthread -o $(BUILD)/log10f-check tools/log10f-check.c -lm
	./$(BUILD)/log10f-check

# ---- the effect kernels ------------------------------------------------------------------------
# Each kernel's oracle runs every arm at every rate in OMX_DECLARED_RATES and refuses to run if
# 44.1, 48, 96 or 192 kHz is missing; the golden digests hold the kernel's output bit for bit.
# The dynamics, balance, rotor and limiter oracles and digests run at the nine RME rates instead
# (OMX_FX_RME_RATES in test/fx/fx_rates.h), 32, 64 and 128 kHz included.

# One list, one set of rules: a kernel lane adds its name to FX_KERNELS and its three sources,
#   test/fx/<k>.test.c         the oracle, release flags against the plain archive unless the
#                              kernel names contracts below
#   test/fx/<k>_math.test.c    the contracts battery, contracts compiled in, the ledger read;
#                              a kernel whose oracle reads the ledger itself may have none
#   test/fx/<k>_golden.test.c  the golden digests, compared with test/golden/<k>.sha256
FX_KERNELS = delay geq pitch transient drive chorus flanger phaser reverb tremolo rotor rotary transient_instance geq_instance eq_instance limiter dynamics_keyed dynamics gate gate_instance dynamics_instance band_dyn deesser
# Oracles that read the contract ledger themselves build with contracts and threads.
FX_CONTRACT_ORACLES = geq pitch transient chorus flanger phaser tremolo rotor rotary transient_instance geq_instance eq_instance limiter dynamics_keyed dynamics gate gate_instance dynamics_instance band_dyn $(addsuffix _instance,$(FX_INSTANCES))
# The effects that carry a host-agnostic instance core, include/omxdsp/fx/omx_<k>_instance.h: each
# adds test/fx/<k>_instance.test.c, its oracle at every declared rate, contracts compiled in.
FX_INSTANCES = chorus flanger drive reverb
# A kernel's perturbation arm: test/fx/<k>-perturb.sh builds its oracle against a moved declaration.
FX_PERTURB = $(wildcard test/fx/*-perturb.sh)
FX_DEPS = $(LIB) $(LIB_CONTRACTS) $(HEADERS) $(FX_HEADERS) $(FX_TESTS) | $(BUILD)

$(BUILD)/fx_%_math: test/fx/%_math.test.c $(FX_DEPS)
	$(CC) $(TESTFLAGS) -pthread -Itest/fx -o $@ $< $(LIB_CONTRACTS) -lm

# A golden TU is compiled with log10f replaced by the correctly rounded omx_log10f (test/support),
# so its digest is exact on every glibc; production calls the libm's log10f.
$(BUILD)/fx_%_golden: test/fx/%_golden.test.c $(FX_DEPS) $(SUPPORT_HEADERS)
	$(CC) $(CFLAGS) $(INC) -Itest/fx $(GOLDEN_SUBST) -o $@ $< $(LIB) -lm

$(BUILD)/fx_%: test/fx/%.test.c $(FX_DEPS)
	$(if $(filter $*,$(FX_CONTRACT_ORACLES)),$(CC) $(TESTFLAGS) -pthread -Itest/fx -o $@ $< $(LIB_CONTRACTS) -lm,$(CC) $(CFLAGS) $(INC) -Itest/fx -o $@ $< $(LIB) -lm)

FX_BINS = $(foreach k,$(FX_KERNELS),$(BUILD)/fx_$(k) $(if $(wildcard test/fx/$(k)_math.test.c),$(BUILD)/fx_$(k)_math) $(BUILD)/fx_$(k)_golden) \
          $(foreach k,$(FX_INSTANCES),$(BUILD)/fx_$(k)_instance)

test-fx: $(FX_BINS)
	@set -e; for k in $(FX_KERNELS); do \
	  echo "./$(BUILD)/fx_$$k"; ./$(BUILD)/fx_$$k; \
	  if [ -f test/fx/$${k}_math.test.c ]; then echo "./$(BUILD)/fx_$${k}_math"; ./$(BUILD)/fx_$${k}_math; fi; \
	  echo "./$(BUILD)/fx_$${k}_golden test/golden/$$k.sha256"; ./$(BUILD)/fx_$${k}_golden test/golden/$$k.sha256; \
	done
	@set -e; for k in $(FX_INSTANCES); do echo "./$(BUILD)/fx_$${k}_instance"; ./$(BUILD)/fx_$${k}_instance; done
	@set -e; for p in $(FX_PERTURB); do \
	  echo "bash $$p"; CC="$(CC)" CFLAGS="$(CFLAGS)" bash $$p $(BUILD)/fx-perturb/$$(basename $$p .sh); \
	done

# The engine's copies of the dynamics, balance, rotor and limiter kernels against this library's
# golden digests: needs a checkout of the private OpenMixer repository, so it is a desk check, not part of
# make test and not run in CI (BUILDING.md, "The engine's copies").
engine-identity: $(LIB)
	@test -n "$(OPENMIXER)" || { echo "make engine-identity OPENMIXER=<openmixer checkout>"; exit 2; }
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/engine-identity.sh "$(OPENMIXER)/packages/pipewire-native/src"
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/engine-identity.sh --self-test "$(OPENMIXER)/packages/pipewire-native/src"

golden-write: $(foreach k,$(FX_KERNELS),$(BUILD)/fx_$(k)_golden)
	@set -e; for k in $(FX_KERNELS); do ./$(BUILD)/fx_$${k}_golden --write > test/golden/$$k.sha256; done

# ---- the analysis engines ---------------------------------------------------------------------
# FBS feedback detection and HRP read a spectrum and answer what rings and what to cut; they run on
# a control thread, never in the audio callback. Each test/analysis/<t>.test.c is one program at
# release flags against the plain archive; -Isrc lets a test include the compiled unit it checks.
# rates runs the HRP chain at the nine RME rates and counts allocations through wrapped allocators;
# golden compares every answer of both engines with test/golden/analysis.sha256, which the engine's
# own copy of this code wrote before it moved here; its log10f is the correctly rounded omx_log10f of test/support, wrapped in at link time.
ANALYSIS_TESTS = fbs_detect hrp_pitch hrp_track hrp_attribute hrp_baseline hrp_correct rates golden
ANALYSIS_LDFLAGS_rates = -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
ANALYSIS_LDFLAGS_golden = -Wl,--wrap=log10f
ANALYSIS_DEPS = $(LIB) $(HEADERS) $(ANALYSIS_HEADERS) $(wildcard test/analysis/*.h) test/fx/sha256.h $(SUPPORT_HEADERS) | $(BUILD)

$(BUILD)/analysis_%: test/analysis/%.test.c $(ANALYSIS_DEPS)
	$(CC) $(CFLAGS) $(INC) -Isrc -Itest/analysis -Itest/fx $(ANALYSIS_LDFLAGS_$*) -o $@ $< $(LIB) -lm

test-analysis: $(foreach t,$(ANALYSIS_TESTS),$(BUILD)/analysis_$(t))
	@set -e; for t in $(ANALYSIS_TESTS); do echo "./$(BUILD)/analysis_$$t"; ./$(BUILD)/analysis_$$t; done

# Each archive carries its flavour (its symbols say so), and the contracts consumer reads a
# violation raised inside the compiled unit only when linked against the contracts archive: linked
# against the plain one it must fail, or the check is blind.
flavours: $(LIBS_ALL) test/omxdsp_flavour.c $(HEADERS) | $(BUILD)
	CC="$(CC)" NM="$(NM)" bash tools/flavour-check.sh $(BUILD)/flavour "$(TESTFLAGS)" \
	  $(LIB) $(LIB_CONTRACTS) $(LIB_TSAN)

test: lib checks suite negative perturb threads test-fx test-analysis flavours
	@echo "omxdsp: make test green"

# ---- install ------------------------------------------------------------------------------------

version:
	@echo $(VERSION)

PC_SED = -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|' \
	 -e 's|@VERSION@|$(VERSION)|' -e 's|@CONTRACTVERSION@|$(CONTRACT_VERSION)|' -e 's|@FPFLAGS@|$(FPFLAGS)|'

$(BUILD)/omxdsp.pc: omxdsp.pc.in include/omxdsp/omxdsp.h Makefile | $(BUILD)
	sed $(PC_SED) -e 's|@NAME@|omxdsp|' -e 's|@FLAVOUR@|release, contracts compiled out|' \
	    -e 's|@CFLAGS@||' -e 's|@LIBS@||' $< | sed 's/ *$$//' > $@

$(BUILD)/omxdsp-contracts.pc: omxdsp.pc.in include/omxdsp/omxdsp.h Makefile | $(BUILD)
	sed $(PC_SED) -e 's|@NAME@|omxdsp-contracts|' -e 's|@FLAVOUR@|contracts compiled in|' \
	    -e 's|@CFLAGS@| $(CONTRACTS_CFLAGS)|' -e 's|@LIBS@||' $< | sed 's/ *$$//' > $@

$(BUILD)/omxdsp-tsan.pc: omxdsp.pc.in include/omxdsp/omxdsp.h Makefile | $(BUILD)
	sed $(PC_SED) -e 's|@NAME@|omxdsp-tsan|' -e 's|@FLAVOUR@|contracts compiled in, under ThreadSanitizer|' \
	    -e 's|@CFLAGS@| $(TSAN_CFLAGS)|' -e 's|@LIBS@| $(TSAN_LIBS)|' $< | sed 's/ *$$//' > $@

install: $(LIBS_ALL) $(PC_ALL)
	install -d $(DESTDIR)$(INCLUDEDIR)/omxdsp/fx $(DESTDIR)$(INCLUDEDIR)/omxdsp/analysis $(DESTDIR)$(LIBDIR)/pkgconfig
	install -m 0644 include/omxdsp/*.h $(DESTDIR)$(INCLUDEDIR)/omxdsp/
	install -m 0644 include/omxdsp/fx/*.h $(DESTDIR)$(INCLUDEDIR)/omxdsp/fx/
	install -m 0644 include/omxdsp/analysis/*.h $(DESTDIR)$(INCLUDEDIR)/omxdsp/analysis/
	install -m 0644 $(LIBS_ALL) $(DESTDIR)$(LIBDIR)/
	install -m 0644 $(PC_ALL) $(DESTDIR)$(LIBDIR)/pkgconfig/

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

PART   ?= all
cost-kernel: tools/kernel-cost.c $(LIB) $(HEADERS) $(FX_HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $(BUILD)/kernel-cost tools/kernel-cost.c $(LIB) -lm
	./$(BUILD)/kernel-cost $(PART)

bench-mixmatrix: tools/bench-mixmatrix.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $(BUILD)/bench-mixmatrix tools/bench-mixmatrix.c -lm
	./$(BUILD)/bench-mixmatrix
