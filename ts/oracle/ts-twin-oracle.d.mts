// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
export interface Tolerance {
  /** Allowed relative gap, a fraction of the kernel's number. */
  readonly rel?: number;
  /** Allowed absolute gap. */
  readonly abs?: number;
}
export declare const oraclePath: () => string;
export declare const oracleKernels: () => string[];
/** The contract's full oracle rate grid, as the library was built with it. */
export declare const oracleRates: () => number[];
/** The flags the oracle is built with and where its library came from. */
export declare const linkage: () => { from: string; flags: string };
/** One answer row per question row, in order. */
export declare const oracle: (kernel: string, rows: readonly (readonly number[])[]) => number[][];
/** '' when `twin` agrees with `kernel` inside `tol`, else a sentence saying how far off it is. */
export declare const gap: (twin: number, kernel: number, tol: Tolerance) => string;
export declare const compare: (
  twin: readonly number[],
  kernel: readonly number[],
  tol: Tolerance,
  label?: string,
) => { failures: string[]; worst: number };
export interface Asked {
  readonly kernel: string;
  readonly rows: readonly (readonly number[])[];
  readonly answers: readonly (readonly number[])[];
}
export declare const ask: (kernel: string, rows: readonly (readonly number[])[]) => Asked;
export declare const against: (
  asked: Asked,
  twin: (row: readonly number[]) => readonly number[],
  tol: Tolerance,
) => { rows: number; failures: string[]; worst: number };
