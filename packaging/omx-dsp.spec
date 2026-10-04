# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
Name: omx-dsp
Version: 0.1.4
Release: 1%{?dist}
License: GPL-3.0-or-later
Summary: DSP primitives and effect kernels of OpenMixer, for static linking
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
libomxdsp holds the DSP building blocks of the OpenMixer console (biquads,
one-poles, envelopes, gain computers, delay rings, the oversampler) and the
effect kernels built from them, as plain C with no allocation, lock or system
call on the audio path. The OpenMixer engine and the omx plugins compile and
link the same kernels from it, so a plugin sounds like the console.

%package devel
Summary: Headers, static library and pkg-config file of libomxdsp
Provides: %{name}-static = %{version}-%{release}

%description devel
The headers under include/omxdsp and the static library in three flavours, each
with its pkg-config file: libomxdsp.a (omxdsp), libomxdsp-contracts.a
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
* Mon Oct 05 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.4-1
- omx_mixmatrix: the summing matrix multiply, Y = G*X, dense and sparse, ramped where a
  coefficient moves inside the block.
- omx_fader_law: the one fader law -- fader dB, pan, send, mute and DCA resolve to a G entry --
  shared by the engine, the fader plugin and the channel-strip plugin.
- tools/bench-mixmatrix.c: ns per strip-output-frame, dense vs sparse, at 32/64/97 strips x 1024
  frames.

* Sun Oct 04 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.3-1
- omx_halfband_decimate: the one run of the half-band dot, sixteen outputs at a time,
  bit-identical to the loop it replaces; the oversampler's up pass runs output-parallel too.
- OMX_PRE_LEGS_FINITE: a stereo kernel's finite-in-l/finite-in-r entry pair as one contract word.
- tools/tsan-gate.sh keeps a red run's whole log and prints every report; no TSan is UNJUDGED.

* Fri Oct 02 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.2-1
- The library in three flavours: libomxdsp-contracts.a and libomxdsp-tsan.a
  beside libomxdsp.a, each with its pkg-config file.

* Thu Oct 01 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.1-1
- omx_param.h: the parameter clamp, one defined answer for NaN and +-Inf per word.

* Thu Oct 01 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- First package: the primitives and the FX delay kernel.
