# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
Name: omx-dsp
Version: 0.2.0
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
The headers under include/omxdsp, libomxdsp.a and omxdsp.pc. Programs link the
library statically; no shared library exists.

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
%{_libdir}/pkgconfig/omxdsp.pc

%changelog
* Thu Oct 01 2026 Pau Aliagas <linuxnow@gmail.com> - 0.2.0-1
- First package: the primitives and the FX delay kernel.
