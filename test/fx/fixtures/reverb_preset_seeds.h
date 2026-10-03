/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 *
 * reverb_preset_seeds.h — GENERATED from OpenMixer's packages/core/src/reverb-presets.ts by
 * packages/core/src/reverb-presets-c-twin.test.ts (OMX_REGEN=1). Never edited by hand: that
 * test refuses any drift, so test/fx/reverb_math.test.c measures the seeds the console declares.
 */
#ifndef OMX_REVERB_PRESET_SEEDS_H
#define OMX_REVERB_PRESET_SEEDS_H

/** One declared reverb seed: every field but `on` and `mix`, in the kernel's units. */
struct omx_reverb_preset_seed {
  const char *id;
  int algorithm;
  float size, damping, predelay_ms, width, lowcut, highcut;
  float reverse_ms, hold_ms, release_ms, gate_threshold_db;
};

#define OMX_REVERB_PRESET_SEED_COUNT 16

static const struct omx_reverb_preset_seed OMX_REVERB_PRESET_SEEDS[OMX_REVERB_PRESET_SEED_COUNT] = {
  { "smallRoom", OMX_REVERB_ROOM, 0.35f, 0.45f, 8.0f, 0.9f, 120.0f, 8000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "vocalPlate", OMX_REVERB_PLATE, 0.6f, 0.35f, 20.0f, 1.0f, 200.0f, 12000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "concertHall", OMX_REVERB_HALL, 0.85f, 0.5f, 30.0f, 1.0f, 80.0f, 9000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "chamber", OMX_REVERB_HALL, 0.5f, 0.65f, 12.0f, 0.8f, 150.0f, 7000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "reverseSwell", OMX_REVERB_REVERSE, 0.7f, 0.3f, 0.0f, 1.0f, 100.0f, 12000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "gatedSnare", OMX_REVERB_GATED, 0.6f, 0.35f, 0.0f, 1.0f, 150.0f, 10000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "ambience", OMX_REVERB_ROOM, 0.2f, 0.3f, 0.0f, 1.0f, 150.0f, 14000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "revxRoom", OMX_REVERB_ROOM, 0.4f, 0.35f, 6.0f, 1.0f, 90.0f, 11000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "sslRoom", OMX_REVERB_ROOM, 0.45f, 0.4f, 10.0f, 0.9f, 100.0f, 10000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "vintageRoom", OMX_REVERB_ROOM, 0.5f, 0.2f, 10.0f, 1.0f, 100.0f, 12000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "platedShort", OMX_REVERB_PLATE, 0.1f, 0.7f, 5.0f, 0.8f, 150.0f, 9000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "vintagePlate", OMX_REVERB_PLATE, 0.33f, 0.2f, 0.0f, 1.0f, 80.0f, 10000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "revxPlate", OMX_REVERB_PLATE, 0.5f, 0.3f, 15.0f, 1.0f, 150.0f, 11000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "richPlate", OMX_REVERB_PLATE, 0.8f, 0.3f, 25.0f, 1.0f, 120.0f, 12000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "revxHall", OMX_REVERB_HALL, 0.6f, 0.4f, 20.0f, 1.0f, 80.0f, 11000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
  { "sslHall", OMX_REVERB_HALL, 0.7f, 0.45f, 25.0f, 1.0f, 100.0f, 10000.0f, 300.0f, 120.0f, 20.0f, -40.0f },
};

#endif
