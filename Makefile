# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# libomxdsp — docs/design/specs/2026-09-26-dsp-primitives.md §3.6.
#   make lib    build/libomxdsp.a (the one compiled unit; every primitive is header-only)
#   make test   the library suite alone: contracts ON, every oracle at every declared rate, the
#               negative arms, the perturbation arm, the N-thread byte-identity, the
#               writable-data check and the doc check — pure C + -lm (+ -pthread), no node
#   make lint   the doc check and the source scan only
#   make docs   the API reference by doxygen (build-time only)
#   make test-tsan  the thread arm under -fsanitize=thread where the toolchain has it

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

SUITE_BINS = $(BUILD)/omxdsp_suite $(BUILD)/omxdsp_negative $(BUILD)/omxdsp_threads

.PHONY: all lib test lint docs clean test-tsan suite negative perturb threads checks

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

# The POST arm of the negative suite runs against a SABOTAGED COPY of one header, placed ahead of
# the real include directory: the tree is never edited.
$(BUILD)/omxdsp_negative: test/omxdsp_negative.c test/sabotage.sh $(SRC) $(HEADERS) | $(BUILD)
	bash test/sabotage.sh $(BUILD)/sabotage
	$(CC) $(TESTFLAGS) -I$(BUILD)/sabotage -o $@ test/omxdsp_negative.c $(SRC) -lm

suite: $(BUILD)/omxdsp_suite
	./$(BUILD)/omxdsp_suite

negative: $(BUILD)/omxdsp_negative
	./$(BUILD)/omxdsp_negative

threads: $(BUILD)/omxdsp_threads
	./$(BUILD)/omxdsp_threads

perturb: $(HEADERS) | $(BUILD)
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash test/perturb.sh $(BUILD)/perturb

checks:
	CC="$(CC)" CFLAGS="$(CFLAGS)" bash tools/writable-data-check.sh $(BUILD)/wd
	bash tools/doc-check.sh

lint: checks

test: lib checks suite $(MORE_TEST_TARGETS)
	@echo "omxdsp: make test green"

test-tsan: test/omxdsp_threads.c $(SRC) $(HEADERS) | $(BUILD)
	$(CC) $(TESTFLAGS) -fsanitize=thread -g -pthread -o $(BUILD)/omxdsp_threads_tsan test/omxdsp_threads.c $(SRC) -lm
	./$(BUILD)/omxdsp_threads_tsan

docs: $(HEADERS) | $(BUILD)
	doxygen Doxyfile

clean:
	rm -rf $(BUILD)
