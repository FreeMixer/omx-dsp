# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
Name: omx-dsp
Version: 0.5.2
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
BuildRequires: pkgconfig
BuildRequires: omx-contract-devel >= 2.3.0
BuildRequires: omx-contract-devel < 3

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
Requires: omx-contract-devel >= 2.3.0
Requires: omx-contract-devel < 3

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
* Fri Oct 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.2-1
- A trim face, fx/omx_trim_instance.h: the console's input trim, a
  de-zippered gain; its resolve takes the trim kernel's trimDb, clamped
  into TRIM_RANGE
- With the EQ face's pass filters it is the channel strip plugin's input
  stage, bit for bit at every declared rate
- Built against omx-contract 2.3.0; any 2.x from 2.3.0 on

* Fri Oct 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.1-1
- The gate runs the contract's knee range (kneeStartDb, kneeEndDb): a rounded
  corner from the start to the end point around the threshold; a start equal
  to its end is the hard knee, the 0.5.0 gate bit for bit
- The gate face's resolve takes the knee start and end after the range, in
  the contract's order

* Fri Oct 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.0-1
- Built against omx-contract 2.2.0, and requires any 2.x from 2.2.0 on, not
  one exact release: a contract minor keeps every existing define unchanged
- The drive face takes the auto-gain, stereo-link and HF roll-off controls
- A delay instance face that owns its 2 s rings and takes the ping-pong switch
- A stereo EQ instance face taking the EQ's contract controls in order, one
  build per band count (8, 16 or 32 bands)
- The dynamics face is now the comp face (omx_comp_instance.h), taking the
  comp's mix, its kind (comp or limiter, RMS or peak detector) and its
  detector oversampling
- The gate face takes the hold and the hysteresis the kernel now runs
* Fri Oct 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.4.1-1
- Built against omx-contract 2.1.0

* Fri Oct 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.4.0-1
- pitch and limiter instance faces own their rings: init takes (instance, rate)
  and allocates nothing (breaking while 0.x: the caller-ring init is gone)

* Fri Oct 09 2026 Pau Aliagas <linuxnow@gmail.com> - 0.3.0-1
- Instance faces for tremolo, phaser, pitch, rotary and limiter (omx-contract 2.0.0)
- transient face: attack_time_ms, sustain_time_ms

* Thu Oct 08 2026 Pau Aliagas <linuxnow@gmail.com> - 0.2.0-1
- omx-dsp now builds against omx-contract 1.3.0 and no longer carries its own
  copy of the console's limits. Every limit, travel, default, list and choice
  the kernels read comes from the contract's header
  (`omxcontract/omx_contract_limits.h`, pinned in `.github/pins.txt`); the
  library installs and requires `omx-contract-devel` (or
  `libomx-contract-dev`) at exactly that version, and `omxdsp.pc` requires it.
- Breaking, while the library is 0.x: `omxdsp/omx_contract_limits.h` is gone.
  A program that read names from it now includes
  `<omxcontract/omx_contract_limits.h>`, and the console-only names the old
  header carried for the engine (about 460) are not in the contract and are
  not here.
- The kernels' own copies of contract values are gone: the chorus, flanger,
  phaser and reverb ceilings and base delays, the three fractional-delay read
  orders, the ring corrector's default amount, the 31 graphic EQ centres, the
  EQ band types, slopes and pass-filter defaults, and the oracle rate lists.
  Their names stay as definitions of the contract's. No sound changes: every
  golden digest is the one it was.
- An unconnected EQ band port now reads the contract's one default rule for
  the bank (the band's centre and type for its place among the bands) instead
  of a 1 kHz bell.
- The joint EQ band budget is checked by omx-contract itself, so the test that
  restated it is gone.
- `tools/contract-agree.sh`, its gap list and the render check against the
  console are gone with the header they compared. `tools/contract-include.sh`
  finds the contract (an installed package, else the pinned release), and a
  build that cannot find exactly the pinned version stops.

* Thu Oct 08 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.8-1
- Converting a level to decibels calls the system's log10f again, about five
  times faster than the correctly rounded version it replaced. That version,
  omx_log10f, is no longer part of the library: it lives in the tests, which
  use it so every reference output stays exact on any system.
- On glibc older than 2.41 (Debian bookworm) the result can differ from the
  correctly rounded one by up to 2 ulp of a dB value. glibc 2.41 rounds log10f
  correctly.

* Thu Oct 08 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.7-1
- The FFT the console's analyser runs on is now part of the library: the Hann
  window, a complex and a real single-precision transform, and a
  double-precision complex transform with its inverse for room and alignment
  measurements. The single-precision transforms give exactly the console's
  previous output; every transform is checked against a long-double reference.
- The dynamics, balance, rotor and limiter are now tested at all nine rates an
  RME interface runs at, from 32 to 192 kHz, and their reference outputs are
  kept for each of those rates.
- The rotor that drives the rotary speaker has its own test, brought over from
  the console.
- A new check builds the console engine's own copies of these four and
  confirms they give exactly the same output as the library, so the engine can
  switch to the library without a change in sound. Their code is unchanged.
- Two analysis engines arrive from the console, unchanged: the feedback
  detector, which finds a ring growing out of a spectrum and says where to
  notch it, and HRP, which follows the notes an instrument plays, learns how
  its harmonics normally sit and sizes the cuts for the ones that ring. Both
  read a spectrum the caller hands them, allocate nothing and run off the
  audio thread. Include them from `omxdsp/analysis/`.
- Both are tested at all nine sample rates an RME interface offers, from 32 to
  192 kHz, and their output is held bit for bit to what the console computed
  before they moved.

* Wed Oct 07 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.6-1
- The delay effect's instance header and the plugin parameter headers are no
  longer part of the library. They belong to the plugin, and omx-delay now
  carries its own. Nothing else changed.

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
