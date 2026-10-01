# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# libomxdsp — the DSP primitives and effect kernels the OpenMixer engine and the omx plugins
# link statically.
#   make lib    build/libomxdsp.a (the one compiled unit; every primitive and kernel is header-only)
#   make test   the library suite alone: contracts ON, every oracle at every declared rate, the
#               negative arms, the perturbation arm, the N-thread byte-identity, the
#               writable-data check, the doc check and the effect kernels' oracles and golden
#               digests — pure C + -lm (+ -pthread), no node
#   make install   headers, libomxdsp.a and omxdsp.pc under PREFIX/LIBDIR/INCLUDEDIR, into DESTDIR
#   make test-fx   the effect kernels' oracles and golden digests alone
#   make lint   the doc check and the source scan only
#   make docs   the API reference by doxygen (build-time only)
#   make test-tsan  the thread arm under -fsanitize=thread where the toolchain has it
#   make cost-prel  omx_env_program_release's ns/sample at every declared rate (its cost row)

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -Wall -Wextra -Werror -O2
# Every consumer compiles the inline kernels without floating-point contraction (omxdsp.pc carries
# the same flag), so a kernel's output does not depend on the architecture's FMA or the consumer's
# build flags: the engine and a plugin built at one version produce the same bits.
FPFLAGS  = -ffp-contract=off
override CFLAGS += $(FPFLAGS)
INC      = -Iinclude
BUILD    = build
LIB      = $(BUILD)/libomxdsp.a
HEADERS  = $(wildcard include/omxdsp/*.h)
SRC      = $(wildcard src/*.c)
OBJ      = $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
TESTFLAGS = $(CFLAGS) $(INC) -DOMX_CONTRACTS
KERNELS  = $(wildcard test/kernels/*.c)
FX_HEADERS = $(wildcard include/omxdsp/fx/*.h include/omxdsp/params/*.h)
FX_TESTS   = $(wildcard test/fx/*.c) $(wildcard test/fx/*.h)

PREFIX     ?= /usr/local
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
DESTDIR    ?=
VERSION    := $(shell sed -n 's/^\#define OMXDSP_VERSION_\(MAJOR\|MINOR\|PATCH\) \([0-9]*\)$$/\2/p' include/omxdsp/omxdsp.h | paste -sd.)



.PHONY: all lib test lint docs clean test-tsan suite negative perturb threads checks cost-prel test-fx install version golden-write

all: lib

lib: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -fPIC $(INC) -c -o $@ $<

$(LIB): $(OBJ) | $(BUILD)
	$(AR) rcs $@ $(OBJ)

# ---- the suite ---------------------------------------------------------------------------------
# Each primitive's arm lives in its own test/kernels/<name>.c; tools/kernels-gen.sh globs them
# (sorted by filename, never a hand list) into $(BUILD)/kernels-suite.inc.c, which
# omxdsp_suite.c #includes — a kernel lane adds ONE file and never touches this Makefile or
# omxdsp_suite.c's main().

$(BUILD)/omxdsp_suite: test/omxdsp_suite.c $(KERNELS) $(SRC) $(HEADERS) tools/kernels-gen.sh | $(BUILD)
	bash tools/kernels-gen.sh $(BUILD)
	$(CC) $(TESTFLAGS) -I$(BUILD) -o $@ test/omxdsp_suite.c $(SRC) -lm

$(BUILD)/omxdsp_threads: test/omxdsp_threads.c $(SRC) $(HEADERS) | $(BUILD)
	$(CC) $(TESTFLAGS) -pthread -o $@ test/omxdsp_threads.c $(SRC) -lm

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

lint: checks

# ---- the effect kernels ------------------------------------------------------------------------
# Each kernel's oracle runs every arm at every rate in OMX_DECLARED_RATES and refuses to run if
# 44.1, 48, 96 or 192 kHz is missing; the golden digests hold the kernel's output bit for bit.

$(BUILD)/fx_delay: test/fx/delay.test.c $(HEADERS) $(FX_HEADERS) $(FX_TESTS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -Itest/fx -o $@ $< $(SRC) -lm

$(BUILD)/fx_delay_math: test/fx/delay_math.test.c $(HEADERS) $(FX_HEADERS) $(FX_TESTS) | $(BUILD)
	$(CC) $(TESTFLAGS) -Itest/fx -o $@ $< $(SRC) -lm

$(BUILD)/fx_delay_golden: test/fx/delay_golden.test.c $(HEADERS) $(FX_HEADERS) $(FX_TESTS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -Itest/fx -o $@ $< $(SRC) -lm

test-fx: $(BUILD)/fx_delay $(BUILD)/fx_delay_math $(BUILD)/fx_delay_golden
	./$(BUILD)/fx_delay
	./$(BUILD)/fx_delay_math
	./$(BUILD)/fx_delay_golden test/golden/delay.sha256

golden-write: $(BUILD)/fx_delay_golden
	./$(BUILD)/fx_delay_golden --write > test/golden/delay.sha256

test: lib checks suite negative perturb threads test-fx
	@echo "omxdsp: make test green"

# ---- install ------------------------------------------------------------------------------------

version:
	@echo $(VERSION)

$(BUILD)/omxdsp.pc: omxdsp.pc.in include/omxdsp/omxdsp.h | $(BUILD)
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|' \
	    -e 's|@VERSION@|$(VERSION)|' -e 's|@FPFLAGS@|$(FPFLAGS)|' $< > $@

install: $(LIB) $(BUILD)/omxdsp.pc
	install -d $(DESTDIR)$(INCLUDEDIR)/omxdsp/fx $(DESTDIR)$(INCLUDEDIR)/omxdsp/params $(DESTDIR)$(LIBDIR)/pkgconfig
	install -m 0644 include/omxdsp/*.h $(DESTDIR)$(INCLUDEDIR)/omxdsp/
	install -m 0644 include/omxdsp/fx/*.h $(DESTDIR)$(INCLUDEDIR)/omxdsp/fx/
	install -m 0644 include/omxdsp/params/*.h $(DESTDIR)$(INCLUDEDIR)/omxdsp/params/
	install -m 0644 $(LIB) $(DESTDIR)$(LIBDIR)/libomxdsp.a
	install -m 0644 $(BUILD)/omxdsp.pc $(DESTDIR)$(LIBDIR)/pkgconfig/omxdsp.pc

# §4.3 (e): NOT RUN (exit 0, never a pass) where the toolchain has no TSan; where it has, the
# thread arm must be report-free and its shared-state sabotage must race.
test-tsan: test/omxdsp_threads.c tools/tsan-gate.sh $(SRC) $(HEADERS) | $(BUILD)
	CC="$(CC)" bash tools/tsan-gate.sh $(BUILD)/tsan omxdsp_threads OMXDSP_THREADS_SABOTAGE_SHARED_STATE \
	  $(TESTFLAGS) -pthread test/omxdsp_threads.c $(SRC) -lm

docs: $(HEADERS) | $(BUILD)
	doxygen Doxyfile

clean:
	rm -rf $(BUILD)

cost-prel: tools/prel-cost.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $(BUILD)/prel-cost tools/prel-cost.c -lm
	./$(BUILD)/prel-cost
