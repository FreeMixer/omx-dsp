# Changelog

What changed in each release of omx-dsp, in plain words. The RPM and Debian changelogs and the
GitHub release notes are generated from this file.

## Unreleased

- The dynamics, balance, rotor and limiter are now tested at all nine rates an RME interface
  runs at, from 32 to 192 kHz, and their reference outputs are kept for each of those rates.
- The rotor that drives the rotary speaker has its own test, brought over from the console.
- A new check builds the console engine's own copies of these four and confirms they give exactly
  the same output as the library, so the engine can switch to the library without a change in
  sound. The library's code itself is unchanged.

## 0.1.5 - 2026-10-07

- The arm64 and aarch64 packages are published again, for Raspberry Pi OS and Fedora on ARM.
  The library itself is unchanged since 0.1.4.

## 0.1.4 - 2026-10-06

- A summing-matrix multiply that mixes many strips into many outputs at once, in a dense and a
  sparse form, and ramps a gain smoothly when it changes in the middle of a block.
- One fader law for the whole project: fader position, pan, sends, mute and DCA groups resolve to
  a single gain, so the console, the fader plugin and the channel-strip plugin agree.
- A small benchmark for the matrix multiply, at 32, 64 and 97 strips.
- More effects arrive from the console, each checked against a closed-form result at every
  sample rate it supports: limiter, band dynamics, de-esser, tremolo and rotary speaker.
- The building blocks of the chorus, drive, flanger, reverb, transient shaper, 31-band graphic
  EQ, parametric EQ, gate and compressor are now in the library.
- The strip's dynamics and balance code moved over unchanged from the engine.
- The decibel conversion no longer depends on the system's log10f, so it gives the same
  answer on every machine.

## 0.1.3 - 2026-10-04

- The half-band filter used by the oversampler runs sixteen outputs at a time, with results
  identical to the loop it replaces.
- A stereo effect can state in one value that both of its inputs are finite numbers.
- The thread-sanitizer gate keeps the whole log of a failed run and prints every report.

## 0.1.2 - 2026-10-02

- The library comes in three builds side by side: the normal one, one with its safety checks
  compiled in, and one for thread-sanitizer runs. Each has its own pkg-config file.

## 0.1.1 - 2026-10-01

- Parameter clamping gives one defined answer for NaN and for infinity.

## 0.1.0 - 2026-10-01

- First package: the DSP building blocks and the delay effect.
