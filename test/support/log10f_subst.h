// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/*
 * log10f_subst.h — makes every `log10f` after it in the translation unit omx_log10f(). The golden
 * builds compile with `-include test/support/log10f_subst.h` (the Makefile's FX golden rule), so
 * the kernels, which are static inline, call the correctly rounded function and their digests read
 * the same on glibc 2.36 as on 2.41 and 2.43. <math.h> comes first so its own declaration of
 * log10f is not rewritten.
 */
#ifndef OMXDSP_TEST_LOG10F_SUBST_H
#define OMXDSP_TEST_LOG10F_SUBST_H

#include <math.h>

#include "log10f_cr.h"

#define log10f omx_log10f
#define OMXDSP_LOG10F_SUBSTITUTED 1

#endif
