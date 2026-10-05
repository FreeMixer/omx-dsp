// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @file omx_dyn.h
 * @brief The dynamics atom and its detector parameters: what a comp, a gate, a band-dyn band or a
 *        de-esser resolves its controls to, and the one derivation of the envelope's parameters
 *        from it.
 *
 * Moved unchanged from openmixer packages/pipewire-native/src/mix_dsp.h (omx-dsp-dev#32): the
 * band-dyn and the de-esser kernels derive their detector from the atom here and nowhere else, so
 * the engine's slot and a kernel cannot read the attack and release poles differently. Pure.
 *
 * Not in the omxdsp.h umbrella, as the fx/ headers are not: it depends on omx_envelope.h, which
 * includes the umbrella ahead of its own definitions. Include it by name.
 */
#ifndef OMX_DYN_H
#define OMX_DYN_H

#include "omx_envelope.h"
#include "omx_gaincomp.h"

/** @brief One resolved dynamics atom: a per-block snapshot of the operator's controls. */
struct omx_dyn {
  int enabled;         /* 0 → the atom is a no-op (the whole slot bypassed) */
  struct omx_gaincomp_params gc; /* the gain computer's mode, threshold, ratio, knee, range, make-up */
  int detect;          /* OMX_DETECT_PEAK | OMX_DETECT_RMS */
  float attack_coeff;  /* one-pole coeff when the detector RISES (0 = instant, →1 = slow) */
  float release_coeff; /* one-pole coeff when the detector FALLS */
  int ovs_mode;        /* OMX_DYN_OVS_AUTO | _OFF | _X4 — the operator's choice, not a factor */
  /* The attack in MILLISECONDS, the operator's own number. `attack_coeff` is the derivation of
   * it (and of the rate); this is the value `auto` compares against a millisecond threshold,
   * which a coefficient cannot answer without knowing the rate it was made at. */
  float attack_ms;
};

/**
 * @brief The detector parameters of a dynamics atom: its attack and release poles and its domain.
 * @param p The atom.
 * @return `{attack_coeff, release_coeff, detect}` as the envelope's parameters, copied unchanged.
 * @note RT-safe: three loads. Thread-safe: pure.
 */
static inline struct omx_env_params omx_dyn_env_params(const struct omx_dyn *p) {
  const struct omx_env_params e = {p->attack_coeff, p->release_coeff, p->detect};
  return e;
}

#endif /* OMX_DYN_H */
