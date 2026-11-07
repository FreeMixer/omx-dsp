// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
/**
 * @openmixer/omx-dsp: the TypeScript twins of omx-dsp kernels, the ones that stay in TypeScript because doing
 * the work in C would be silly (a display, a unit conversion a browser runs). Each is pinned to its kernel by an
 * agreement test in test/ that feeds both the same grid.
 */
export { ANALYSIS_FLOOR_DB, dbToLinearGain, linearGainToDb, linearToDbFloored } from './units.js';
export {
  biquadMagnitudeDb,
  clampEqQFor,
  lowpassHandover,
  magnitudeSumDb,
  matchedPair,
  octavesToQ,
  rbjSection,
  sectionMagnitudeDb,
  type BiquadBand,
  type BiquadKind,
  type BiquadSection,
  type BiquadType,
} from './eq.js';
export {
  COMP_KNEE_DB_RANGE,
  COMP_RATIO_RANGE,
  CURVE_CODES,
  CURVE_DISPLAYS,
  CURVE_EXTENTS,
  CURVE_FAMILIES,
  CURVE_POINTS_MAX,
  CURVE_ORIGINS,
  CURVE_ROLES,
  CURVE_SHAPES,
  CURVE_UNITS,
  EQ_NOTCH_Q_RANGE,
  EQ_Q_RANGE,
  GATE_RANGE_DB_RANGE,
  GATE_RATIO_RANGE,
  LIN_TO_DB_FLOOR,
  LOWPASS_HANDOVER,
  METER_DECIMATION,
  METER_KINDS,
  METER_MEASUREMENTS,
  METER_UNITS,
  METER_UNITS_LABELS,
  METER_VIEW,
  RTA_FFT_SIZE_MAX,
  RTA_TARGET_BIN_HZ,
} from './definitions.js';
export { fftSizeForRate } from './rta.js';
export { gaincompDb, gaincompDbShifted, gateKnee, type GaincompMode, type GaincompParams } from './gaincomp.js';
export {
  CURVE_WORDS,
  butterworthQs,
  curvePoints,
  type CurveAnswer,
  type CurveCode,
  type CurveMark,
  type CurvePoint,
  type CurveRequest,
  type CurveTerm,
} from './curve.js';
export {
  FBS_ACTIONS,
  FBS_HOLD_NEVER,
  FBS_PLACE_SLOTS,
  FbsPlace,
  ORIGIN,
  ORIGIN_WORDS,
  SHAPE,
  SHAPE_WORDS,
  fbsCents,
  fbsGainCap,
  sameRingFreq,
  type BandReport,
  type FbsAction,
  type FbsEvent,
  type FbsFind,
  type FbsPool,
} from './fbs-place.js';
export { EQ_GAIN_RANGE, FBS_MAX_CUT_DB } from './definitions.js';
export {
  METER_GR_EMPTY,
  METER_GR_HOLD_MS,
  METER_MARKER_EMPTY,
  METER_WORDS,
  meterBarStep,
  meterClip,
  meterDecimate,
  meterFill,
  meterFillDb,
  meterGrStep,
  meterGrView,
  meterMarker,
  meterStep,
  meterUnitOf,
  type MeterGr,
  type MeterKind,
  type MeterMarker,
  type MeterMeasurement,
  type MeterUnit,
} from './meter.js';
