<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# Building omx-dsp

Needs a C11 compiler, GNU make, binutils, awk, sed, grep and diffutils.

| command | does |
|---|---|
| `make` | `build/libomxdsp.a`, `build/libomxdsp-contracts.a` and `build/libomxdsp-tsan.a` |
| `make test` | the whole suite: every primitive's and kernel's oracle at every declared rate with contracts on, the negative, perturbation and thread arms, the writable-data and doc checks, and the golden digests |
| `make test-fx` | the effect kernels' oracles and golden digests alone |
| `make install PREFIX=/usr LIBDIR=/usr/lib64 DESTDIR=…` | headers, the three archives and `omxdsp.pc`, `omxdsp-contracts.pc`, `omxdsp-tsan.pc` |
| `make flavours` | each archive carries its flavour, and a contracts consumer reads a violation raised inside the compiled code only through the contracts archive |
| `make docs` | the API reference with doxygen |
| `make test-tsan` | the thread arm under ThreadSanitizer, where the toolchain has it |
| `make golden-write` | rewrites every kernel's `test/golden/<kernel>.sha256`; only in a commit that bumps the minor version or adds a kernel |

Consumers compile the kernels through `pkg-config --cflags omxdsp`, which carries
`-ffp-contract=off`: the kernels are inline, and without it an architecture with fused
multiply-add would round differently from one without.

## Flavours

The compiled part of the library (today only the oversampler; everything else is inline in the
headers) is built once per flavour into `build/`:

- `libomxdsp.a`: `CFLAGS` only, contracts compiled out.
- `libomxdsp-contracts.a`: objects in `build/contracts/`, with `-DOMX_CONTRACTS`.
- `libomxdsp-tsan.a`: objects in `build/tsan-lib/`, with `-DOMX_CONTRACTS -fsanitize=thread`.

Each has a pkg-config file of the same name (`omxdsp`, `omxdsp-contracts`, `omxdsp-tsan`) whose
cflags carry the flavour's flags, so the inline headers and the archive are always built the same
way. The suite, the thread arm and the contract-checked kernel tests link `libomxdsp-contracts.a`,
`make test-tsan` links `libomxdsp-tsan.a`, and the release-flag kernel tests link `libomxdsp.a`.
`make flavours` (`tools/flavour-check.sh`, part of `make test`) checks each archive's symbols and
builds `test/omxdsp_flavour.c`, a contracts consumer that feeds a NaN to the compiled oversampler:
against `libomxdsp-contracts.a` it must find the violation in its own ledger, and against
`libomxdsp.a` it must fail.

## GCC 12.2 on arm64

Debian bookworm's GCC 12.2 on arm64 (Raspberry Pi OS, Zynthian) crashes with an internal compiler
error in `vect_transform_reduction` on a loop that keeps a `double` running maximum or minimum with
`fmax`/`fmin` over values widened from `float`. Write such a reduction as a comparison instead,
`if (!(e <= worst)) worst = e;`, which also carries a NaN into the result. `make lint` refuses the
shape on any host (`tools/reduction-check.sh`), and CI builds the suite on bookworm arm64.

## Golden digests and libm

A golden digest covers the kernel's output bit for bit, so it also covers every libm call the
kernel makes per sample. A libm's `log10f` is not one function: glibc 2.36 (debian:bookworm)
misrounds 1.39% of the non-negative floats, 62 225 of them by 2 ulp, where glibc 2.41 (debian:trixie)
and 2.43 (Fedora 44) round every one correctly, since 2.41 took `log10f` from CORE-MATH. The limiter's
48 kHz digest (the render of FreeMixer/omx-dsp#21) read `5fe47250…` on bookworm against `69a449ce…`
on 2.41 and 2.43. The compiler played no part: the same binary flips with the glibc it runs on.

Production calls the libm: `omx_lin_to_db()` is `20·log10f(max(lin, 1e-9))`, about 2 ns per call
against 10 ns for a libm-free `log10f` on glibc 2.43. The goldens do not: `omx_log10f()` lives in
`test/support/log10f_cr.h`, a correctly rounded `log10` in IEEE double arithmetic (it needs
`FLT_EVAL_METHOD == 0` and `-ffp-contract=off`, which the Makefile's `FPFLAGS` carry), and every
golden TU is compiled with `-include test/support/log10f_subst.h`, which defines `log10f` as
`omx_log10f`. The kernels are static inline, so the substitution reaches them, and every kernel's
golden is an exact digest on every toolchain. Two guards hold it: `golden.h` refuses to compile a
golden without the substitution and the golden driver exits 2 when `log10f` there is not
`omx_log10f`, and `tools/log10f-guard.sh` (part of `make lint`) fails when `omx_log10f` appears
under `include/` or `src/`. `make check-log10f` compares `omx_log10f` with the host libm's `log10f`
on all 2³¹ non-negative floats; against glibc 2.43 none differ. `test/kernels/units.c` tests it
against fixed bit patterns, including `0x0efeee7a`, 7.8e-10 ulp from a float midpoint, which is
answered from its exact value.

On glibc older than 2.41 a production build may therefore differ from the golden by up to 2 ulp of
a dB value in the units that call `log10f`. That is accepted.

The limiter's other per-sample call, `powf` in `omx_db_to_lin()`, is the same implementation in
every glibc since 2.28 and returned the same bits for equal arguments on 2.36, 2.41 and 2.43. A
kernel that adds a per-sample libm call adds it to the RT-safe allowlist named in its `@note`, and
checks that the goldens agree across glibc versions, or uses a libm-free word.

## Generated headers

`include/omxdsp/omx_contract_limits.h` and `include/omxdsp/params/*.h` are rendered from the
OpenMixer declaration and never edited here. `tools/render-check.sh <openmixer checkout>`
renders them again from the commit pinned in `.github/pins.txt` and compares byte for byte.

## Packages

`packaging/omx-dsp.spec` (RPM, `omx-dsp-devel`) and `debian/` (`libomxdsp-dev`). A `v<version>`
tag releases both through the FreeMixer release workflows; the version is
`OMXDSP_VERSION_*` in `include/omxdsp/omxdsp.h`, the spec's `Version:` and the top of
`debian/changelog`, and CI refuses them disagreeing.
