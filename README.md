<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com> -->
# omx-dsp

The DSP library of [OpenMixer](https://www.openmixer.org): the building blocks of
the console (biquads, one-poles, envelopes, gain computers, delay rings, the oversampler) and the
effect kernels built from them. The OpenMixer engine and the omx plugins compile the same kernels
from this library, so an effect sounds the same on the console and in any other host.

Plain C11 and `-lm`, with no allocation, lock or system call on the audio path. Build time only:
headers and static libraries, nothing a running program loads.

## Install

Fedora 44, x86_64 or aarch64:

```
sudo dnf install https://www.openmixer.org/rpm/openmixer-release.noarch.rpm
sudo dnf install omx-dsp-devel
```

Debian bookworm and trixie, amd64 or arm64 (also Raspberry Pi OS and Zynthian):

```
curl -fsSLO https://www.openmixer.org/deb/openmixer-release.deb && sudo apt install ./openmixer-release.deb
sudo apt install libomxdsp-dev
```

## Start

```c
#include <omxdsp/fx/omx_delay.h>
```

```
cc $(pkg-config --cflags omxdsp) -c my_plugin.c
cc -shared -o my_plugin.so my_plugin.o $(pkg-config --libs omxdsp)
```

Test builds use `omxdsp-contracts` (or `omxdsp-tsan`) in place of `omxdsp`.

## Documentation

<https://www.openmixer.org/projects/omx-dsp>, `man omx-dsp-copy-check`, and in this repository: how the
kernels are tested, the three library flavours and the copy check for consumers
([docs/overview.md](docs/overview.md)); building from source and running the tests
([BUILDING.md](BUILDING.md)).

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE).
