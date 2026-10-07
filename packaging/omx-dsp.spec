# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
Name: omx-dsp
Version: 0.1.5
Release: 1%{?dist}
License: GPL-3.0-or-later
Summary: The audio toolbox of the OpenMixer console, as a real-time-safe C library
URL: https://github.com/FreeMixer/omx-dsp

Source0: %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires: gcc
BuildRequires: make
BuildRequires: binutils
BuildRequires: gawk
BuildRequires: diffutils

# Headers and a static archive only: nothing of this package is loaded at run time.
%global debug_package %{nil}
# A static archive carries machine code only, never LTO bytecode a consumer's compiler cannot read.
%global _lto_cflags %{nil}

%description
libomxdsp is the sound of the OpenMixer console, as a library: the filters,
envelopes, delays, limiters, gates, compressors and other building blocks of a
mixer channel, written in plain C that never allocates, locks or calls the
system while audio is running. The console and the omx plugins are built from
this same code, so an effect sounds the same on the desk and in your DAW, sample
for sample. Link it into your own plugin, effect or audio tool and start from
parts that are already tested against exact results at every sample rate.

%package devel
Summary: Headers and static library to build on the OpenMixer audio toolbox
Provides: %{name}-static = %{version}-%{release}

%description devel
Everything you need to build against libomxdsp: the headers under
include/omxdsp and the static library in three flavours, each with its
pkg-config file: libomxdsp.a (omxdsp), libomxdsp-contracts.a
(omxdsp-contracts, contracts compiled in) and libomxdsp-tsan.a (omxdsp-tsan,
contracts and ThreadSanitizer). Programs link the library statically; no shared
library exists.

%prep
%autosetup

%build
%set_build_flags
%make_build lib

%install
%make_install PREFIX=%{_prefix} LIBDIR=%{_libdir} INCLUDEDIR=%{_includedir}

%check
%make_build test

%files devel
%license LICENSE
%doc README.md BUILDING.md
%{_includedir}/omxdsp/
%{_libdir}/libomxdsp.a
%{_libdir}/libomxdsp-contracts.a
%{_libdir}/libomxdsp-tsan.a
%{_libdir}/pkgconfig/omxdsp.pc
%{_libdir}/pkgconfig/omxdsp-contracts.pc
%{_libdir}/pkgconfig/omxdsp-tsan.pc

%changelog
* Wed Oct 07 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.5-1
- The arm64 and aarch64 packages are published again, for Raspberry Pi OS and
  Fedora on ARM. The library itself is unchanged since 0.1.4.

* Tue Oct 06 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.4-1
- A summing-matrix multiply that mixes many strips into many outputs at once,
  in a dense and a sparse form, and ramps a gain smoothly when it changes in
  the middle of a block.
- One fader law for the whole project: fader position, pan, sends, mute and
  DCA groups resolve to a single gain, so the console, the fader plugin and
  the channel-strip plugin agree.
- A small benchmark for the matrix multiply, at 32, 64 and 97 strips.
- More effects arrive from the console, each checked against a closed-form
  result at every sample rate it supports: limiter, band dynamics, de-esser,
  tremolo and rotary speaker.
- The building blocks of the chorus, drive, flanger, reverb, transient shaper,
  31-band graphic EQ, parametric EQ, gate and compressor are now in the
  library.
- The strip's dynamics and balance code moved over unchanged from the engine.
- The decibel conversion no longer depends on the system's log10f, so it gives
  the same answer on every machine.

* Sun Oct 04 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.3-1
- The half-band filter used by the oversampler runs sixteen outputs at a time,
  with results identical to the loop it replaces.
- A stereo effect can state in one value that both of its inputs are finite
  numbers.
- The thread-sanitizer gate keeps the whole log of a failed run and prints
  every report.

* Fri Oct 02 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.2-1
- The library comes in three builds side by side: the normal one, one with its
  safety checks compiled in, and one for thread-sanitizer runs. Each has its
  own pkg-config file.

* Thu Oct 01 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.1-1
- Parameter clamping gives one defined answer for NaN and for infinity.

* Thu Oct 01 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- First package: the DSP building blocks and the delay effect.
