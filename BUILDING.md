<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# Building omx-dsp

Needs a C11 compiler, GNU make, binutils, awk, sed, grep and diffutils.

| command | does |
|---|---|
| `make` | `build/libomxdsp.a` |
| `make test` | the whole suite: every primitive's and kernel's oracle at every declared rate with contracts on, the negative, perturbation and thread arms, the writable-data and doc checks, and the golden digests |
| `make test-fx` | the effect kernels' oracles and golden digests alone |
| `make install PREFIX=/usr LIBDIR=/usr/lib64 DESTDIR=…` | headers, `libomxdsp.a` and `omxdsp.pc` |
| `make docs` | the API reference with doxygen |
| `make test-tsan` | the thread arm under ThreadSanitizer, where the toolchain has it |
| `make golden-write` | rewrites `test/golden/delay.sha256`; only in a commit that bumps the minor version |

Consumers compile the kernels through `pkg-config --cflags omxdsp`, which carries
`-ffp-contract=off`: the kernels are inline, and without it an architecture with fused
multiply-add would round differently from one without.

## Generated headers

`include/omxdsp/omx_contract_limits.h` and `include/omxdsp/params/*.h` are rendered from the
OpenMixer declaration and never edited here. `tools/render-check.sh <openmixer checkout>`
renders them again from the commit pinned in `.github/pins.txt` and compares byte for byte.

## Packages

`packaging/omx-dsp.spec` (RPM, `omx-dsp-devel`) and `debian/` (`libomxdsp-dev`). A `v<version>`
tag releases both through the FreeMixer release workflows; the version is
`OMXDSP_VERSION_*` in `include/omxdsp/omxdsp.h`, the spec's `Version:` and the top of
`debian/changelog`, and CI refuses them disagreeing.
