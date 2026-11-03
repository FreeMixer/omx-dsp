<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# recipes/answers

One file per kernel of `data/kernels/`: where each value of its definitions came from. They are provenance, kept in
the tree and not installed. `make contract-proof OMX_CONTRACT=<omx-contract checkout>` reads them twice: the
completeness check holds every item of a kernel file to its answer (the same value, unit, default and labels, and no
answer for an item the file does not declare), and the round-trip against the v0.1.3 header reads each item's
`since` to tell an item of the first release from a later one.

## An answer

- `kernel`, the file's kernel; `engine`, the repository and the 40-hex commit the values were read at.
- `items`, one per declared item: `name`, `source`, `value` (and `unit`, or `default` and `labels` for a set),
  `doc`, and `since` when the item came later than its kernel.
- `source` is either a path with an `export` or `define` at the engine commit, or one `origin` sentence: the
  decision, or the commit that first declared the value and why it moved since.

## `since`

`since` is read in **omx-contract's release line, frozen at the move**. The definitions lived in omx-contract until
2.14.0 and live here since; so:

- an item first shipped by omx-contract says the omx-contract release that first carried it, 1.0.0 to 2.14.0;
- an item first shipped after the move, in a kernel of its own or added to an older one, says **3.0.0**, the
  contract schema its definitions install under. The exact omx-dsp release and commit are in its `origin`.

The precise release never goes in `since`: omx-dsp's releases (0.x) would read as earlier than the first release.
`make contract-proof` is red on a `since` outside that line, in an answer or in `data/`.
