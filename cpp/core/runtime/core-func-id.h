#pragma once

#include <cstdint>

namespace wendoo {

/**
 * First funcId owned by the active target; core funcIds are below this.
 * Mirrors TARGET_FUNC_ID_BASE in
 * external/wendoo-lang/packages/core/src/runtime/abi-ids.ts.
 */
inline constexpr uint32_t TARGET_FUNC_ID_BASE = 1024;

/**
 * Stable funcIds of the core host functions: operator overloads, conversions,
 * builtins, context methods, and the sensor/actuator function entries.
 * Mirrors the CoreFuncId enum in
 * external/wendoo-lang/packages/core/src/runtime/abi-ids.ts. Serialized
 * programs record these ids verbatim, so the values are wire-stable: never
 * renumber or reuse a value; append new members at the next free id.
 */
enum class CoreFuncId : uint32_t {
  OpAndBoolean = 0,
  OpOrBoolean = 1,
  OpNotBoolean = 2,
  OpAddNumber = 3,
  OpSubtractNumber = 4,
  OpMultiplyNumber = 5,
  OpDivideNumber = 6,
  OpModuloNumber = 7,
  OpPowerNumber = 8,
  OpNegateNumber = 9,
  OpBitwiseAndNumber = 10,
  OpBitwiseOrNumber = 11,
  OpBitwiseXorNumber = 12,
  OpBitwiseNotNumber = 13,
  OpLeftShiftNumber = 14,
  OpRightShiftNumber = 15,
  OpEqualToBoolean = 16,
  OpNotEqualToBoolean = 17,
  OpEqualToNumber = 18,
  OpNotEqualToNumber = 19,
  OpLessThanNumber = 20,
  OpLessThanOrEqualToNumber = 21,
  OpGreaterThanNumber = 22,
  OpGreaterThanOrEqualToNumber = 23,
  OpAddString = 24,
  OpEqualToString = 25,
  OpNotEqualToString = 26,
  OpEqualToNil = 27,
  OpNotEqualToNil = 28,
  OpNotNil = 29,
  OpEqualToNumberNil = 30,
  OpEqualToNilNumber = 31,
  OpNotEqualToNumberNil = 32,
  OpNotEqualToNilNumber = 33,
  OpEqualToBooleanNil = 34,
  OpEqualToNilBoolean = 35,
  OpNotEqualToBooleanNil = 36,
  OpNotEqualToNilBoolean = 37,
  OpEqualToStringNil = 38,
  OpEqualToNilString = 39,
  OpNotEqualToStringNil = 40,
  OpNotEqualToNilString = 41,
  ConvNumberToString = 42,
  ConvStringToNumber = 43,
  ConvNumberToBoolean = 44,
  ConvBooleanToNumber = 45,
  ConvStringToBoolean = 46,
  ConvBooleanToString = 47,
  BrainContextGetVariable = 48,
  BrainContextSetVariable = 49,
  RuleContextGetVariable = 50,
  RuleContextSetVariable = 51,
  SensorRandom = 52,
  SensorOnPageEntered = 53,
  SensorTimeout = 54,
  SensorCurrentPage = 55,
  SensorPreviousPage = 56,
  ActuatorSwitchPage = 57,
  ActuatorRestartPage = 58,
  ActuatorYield = 59,
  ListGet = 60,
  StringGet = 61,
  MapKeys = 62,
  MapValues = 63,
  MapSize = 64,
  MapClear = 65,
  MathAbs = 66,
  MathAcos = 67,
  MathAsin = 68,
  MathAtan = 69,
  MathAtan2 = 70,
  MathCeil = 71,
  MathCos = 72,
  MathExp = 73,
  MathFloor = 74,
  MathLog = 75,
  MathMax = 76,
  MathMin = 77,
  MathPow = 78,
  MathRandom = 79,
  MathRound = 80,
  MathSin = 81,
  MathSqrt = 82,
  MathTan = 83,
  StrLength = 84,
  StrCharAt = 85,
  StrCharCodeAt = 86,
  StrIndexOf = 87,
  StrLastIndexOf = 88,
  StrSlice = 89,
  StrSubstring = 90,
  StrToLowerCase = 91,
  StrToUpperCase = 92,
  StrTrim = 93,
  StrSplit = 94,
  StrConcat = 95,
  BufferFrom = 96,
  BufferFromHex = 97,
  BufferFromString = 98,
  BufferLength = 99,
  BufferGet = 100,
  ContextGetWhenResult = 101,
  OpEqualToEnum = 102,
  OpNotEqualToEnum = 103,
  ConvEnumToString = 104,
  ConvEnumToNumber = 105,
  SensorOtherwise = 106,
  SensorRuleTrigger = 107,
};

/** Number of declared {@link CoreFuncId} members; ids are dense from 0. */
inline constexpr uint32_t kCoreFuncIdCount = 108;

} // namespace wendoo
