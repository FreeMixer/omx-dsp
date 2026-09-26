# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# libomxdsp — docs/design/specs/2026-09-26-dsp-primitives.md §3.6, §8b.
#   make lib    build/libomxdsp.a (the one compiled unit, src/omx_oversampler.c; every other
#               primitive is header-only)
#   make test   the library suite alone, contracts ON, pure C + -lm (+ -pthread), no node:
#                 checks    tools/writable-data-check.sh, tools/doc-check.sh       (§4.3 a, b; §6)
#                 suite     test/omxdsp_suite.c — every oracle at every declared rate, the state
#                           relocation arm, the ledger empty at the end            (§8b 1, 6)
#                 negative  test/omxdsp_negative.c against test/sabotage.sh's kernel copy — one
#                           arm per contract kind, exactly its violation           (§8b 2)
#                 perturb   test/perturb.sh — the perturbed limits header followed, a literal
#                           copy red                                               (§7, §8b 3)
#                 threads   test/omxdsp_threads.c — N workers byte-identical to the single-threaded
#                           reference, the ledger grown by exactly N x; then RED against
#                           test/sabotage.sh's ledger copy (the count made non-atomic) (§4.3 c)
#   make lint   the doc check and the source scan only
#   make docs   the API reference by doxygen (build-time only)
#   make test-tsan  the thread arm under -fsanitize=thread; SKIP, reported, where the toolchain
#               cannot link it (the settle's container can)

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -Wall -Wextra -Werror -O2
INC      = -Iinclude
BUILD    = build
LIB      = $(BUILD)/libomxdsp.a
HEADERS  = $(wildcard include/omxdsp/*.h)
SRC      = $(wildcard src/*.c)
OBJ      = $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
TESTFLAGS = $(CFLAGS) $(INC) -DOMX_CONTRACTS

.PHONY: all lib test lint docs clean test-tsan suite negative perturb threads threads-sabotage checks

all: lib

lib: $(LIB)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(LIB): $(OBJ) | $(BUILD)
	$(AR) rcs $@ $(OBJ)

# ---- the suite ---------------------------------------------------------------------------------

$(BUILD)/omxdsp_suite: test/omxdsp_suite.c $(SRC) $(HEADERS) | $(BUILD)
	$(CC) $(TESTFLAGS) -o $@ test/omxdsp_suite.c $(SRC) -lm

$(BUILD)/omxdsp_threads: test/omxdsp_threads.c $(SRC) $(HEADERS) | $(BUILD)
	$(CC) $(TESTFLAGS) -pthread -o $@ test/omxdsp_threads.c $(SRC) -lm

# The negative suite runs against a SABOTAGED COPY of the headers, placed AHEAD of the real
# include directory (the copy's -I comes first): the tree is never edited.
$(BUILD)/omxdsp_negative: test/omxdsp_negative.c test/sabotage.sh $(SRC) $(HEADERS) | $(BUILD)
	bash test/sabotage.sh $(BUILD)/sabotage-kernel kernel
	$(CC) -I$(BUILD)/sabotage-kernel $(TESTFLAGS) -o $@ test/omxdsp_negative.c $(SRC) -lm

# The thread arm against the ledger made non-atomic: it must go RED.
$(BUILD)/omxdsp_threads_sabotaged: test/omxdsp_threads.c test/sabotage.sh $(SRC) $(HEADERS) | $(BUILD)
	bash test/sabotage.sh $(BUILD)/sabotage-ledger ledger
	$(CC) -I$(BUILD)/sabotage-ledger $(TESTFLAGS) -pthread -o $@ test/omxdsp_threads.c $(SRC) -lm

suite: $(BUILD)/omxdsp_suite
	./$(BUILD)/omxdsp_suite

negative: $(BUILD)/omxdsp_negative
	./$(BUILD)/omxdsp_negative

threads: $(BUILD)/omxdsp_threads
	./$(BUILD)/omxdsp_threads

threads-sabotage: $(BUILD)/omxdsp_threads_sabotaged
	@if ./$(BUILD)/omxdsp_threads_sabotaged > $(BUILD)/threads_sabotaged.log 2>&1; then \
	  echo "omxdsp: FAIL the thread arm stayed GREEN with a non-atomic ledger"; cat $(BUILD)/threads_sabotaged.log; exit 1; \
	else echo "omxdsp: the thread arm went red against the non-atomic ledger: $$(tail -1 $(BUILD)/threads_sabotaged.log)"; fi

perturb: $(HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/perturb.sh $(BUILD)/perturb

checks:
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/writable-data-check.sh $(BUILD)/wd
	bash tools/doc-check.sh

lint: checks

test: lib checks suite negative perturb threads threads-sabotage
	@echo "omxdsp: make test green"

test-tsan: test/omxdsp_threads.c $(SRC) $(HEADERS) | $(BUILD)
	@if $(CC) $(TESTFLAGS) -fsanitize=thread -g -pthread -o $(BUILD)/omxdsp_threads_tsan test/omxdsp_threads.c $(SRC) -lm 2> $(BUILD)/tsan_build.log; then \
	  ./$(BUILD)/omxdsp_threads_tsan && echo "omxdsp: test-tsan ran"; \
	else echo "omxdsp: test-tsan SKIP — this toolchain cannot link -fsanitize=thread: $$(tail -1 $(BUILD)/tsan_build.log)"; fi

docs: $(HEADERS) | $(BUILD)
	doxygen Doxyfile

clean:
	rm -rf $(BUILD)
