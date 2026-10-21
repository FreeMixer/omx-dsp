// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/** The twin of omx_eq_design.h omx_eq_octaves_q, moved verbatim from the console (its catalog package). */

/** Bandwidth in octaves → Q (the cookbook relation). */
export function octavesToQ(octaves: number): number {
  const bw = Math.max(octaves, 0.05);
  const t = Math.pow(2, bw);
  return Math.sqrt(t) / (t - 1);
}
