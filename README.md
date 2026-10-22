<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# omx-dsp

The DSP library of [OpenMixer](https://www.openmixer.org): the building blocks of
the console (biquads, one-poles, envelopes, gain computers, delay rings, the oversampler) and the
effect kernels built from them. The OpenMixer engine and the omx plugins compile the same kernels
from this library, so an effect sounds the same on the console and in any other host.

- Plain C11 and `-lm`. No allocation, lock or system call on the audio path; every state is
  owned by the caller, so any number of threads can run the kernels on their own states.
- Pre- and postconditions on every function, compiled in with `-DOMX_CONTRACTS` for testing and
  absent from a release build.
- Every kernel is tested against its closed form at 44.1, 48, 88.2, 96, 176.4 and 192 kHz, and
  its output is held bit for bit by golden digests. The dynamics, balance, rotor, limiter and
  loudness are also tested at 32, 64 and 128 kHz, the other rates of the oracle set.
- Build time only: headers and a static library. Nothing is installed that a running program
  loads.

## Install

Fedora 44, x86_64 or aarch64:

```
sudo dnf config-manager addrepo --from-repofile=https://freemixer.github.io/rpm/freemixer.repo
sudo dnf install omx-dsp-devel
```

Debian bookworm and trixie, amd64 or arm64 (also Raspberry Pi OS and Zynthian):

```
curl -fsSL https://freemixer.github.io/deb/freemixer.asc | sudo tee /usr/share/keyrings/freemixer.asc >/dev/null
echo "deb [signed-by=/usr/share/keyrings/freemixer.asc] https://freemixer.github.io/deb/debian/$(. /etc/os-release; echo $VERSION_CODENAME) ./" | sudo tee /etc/apt/sources.list.d/freemixer.list
sudo apt update && sudo apt install libomxdsp-dev
```

## Use

```
cc $(pkg-config --cflags omxdsp) -c my_plugin.c
cc -shared -o my_plugin.so my_plugin.o $(pkg-config --libs omxdsp)
```

```c
#include <omxdsp/fx/omx_delay.h>
```

A consumer's CI can check that it calls the library instead of carrying its own copies of
what the library exports. The devel package installs `omx-dsp-copy-check` and the exports file it
reads; from the root of the consumer's checkout:

```
omx-dsp-copy-check --enforce
```

It scans the tracked C and TypeScript sources (tests, fakes, fixtures, oracles and generated
directories excluded) and names each function that defines an exported name or a twin the
library's documentation declares, repeats an export's body, carries two of its distinctive
constants with the same maths calls, or writes a short export inline, such as `20 * log10(x)` or
the mono sum `0.5f * (l[i] + r[i])`. Without `--enforce` it only reports. Copies kept on purpose
go in `omx-dsp-copies.allow` at the root, one per line:

```
tools/measure.c:main | omx_lin_to_db_d | an independent measurement, kept on purpose | owner#12
```

An entry that no longer covers a copy, or that has no reason or owner, fails the check, so the
list only shrinks. `omx-dsp-copy-check --help` describes every rule.

The library comes in three flavours, each with its own archive and pkg-config file. A program
builds against the one that matches its own build, so that its contract and TSan builds also
check the library's compiled code:

| pkg-config | archive | for |
|---|---|---|
| `omxdsp` | `libomxdsp.a` | release builds; contracts compiled out |
| `omxdsp-contracts` | `libomxdsp-contracts.a` | test builds with `-DOMX_CONTRACTS` |
| `omxdsp-tsan` | `libomxdsp-tsan.a` | test builds under ThreadSanitizer, contracts on |

The contracts and TSan flavours put `-DOMX_CONTRACTS` (and `-fsanitize=thread`) in the cflags.
The program defines the violation ledger in one of its files with `#define OMX_CONTRACT_STORAGE`
before including the headers.

Building from source and running the tests: [BUILDING.md](BUILDING.md).

## Licence

GPL-3.0-or-later.
