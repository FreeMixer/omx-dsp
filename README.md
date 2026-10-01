<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# omx-dsp

The DSP library of [OpenMixer](https://github.com/FreeMixer/openmixer): the building blocks of
the console (biquads, one-poles, envelopes, gain computers, delay rings, the oversampler) and the
effect kernels built from them. The OpenMixer engine and the omx plugins compile the same kernels
from this library, so an effect sounds the same on the console and in any other host.

- Plain C11 and `-lm`. No allocation, lock or system call on the audio path; every state is
  owned by the caller, so any number of threads can run the kernels on their own states.
- Pre- and postconditions on every function, compiled in with `-DOMX_CONTRACTS` for testing and
  absent from a release build.
- Every kernel is tested against its closed form at 44.1, 48, 88.2, 96, 176.4 and 192 kHz, and
  its output is held bit for bit by golden digests.
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

Building from source and running the tests: [BUILDING.md](BUILDING.md).

## Licence

GPL-3.0-or-later.
