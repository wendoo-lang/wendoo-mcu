#include "doctest/doctest.h"

#include "core/runtime/core-func-id.h"

#include <cstdint>

using wendoo::CoreFuncId;
using wendoo::kCoreFuncIdCount;
using wendoo::TARGET_FUNC_ID_BASE;

TEST_CASE("funcId partition constants match the TS declarations") {
  CHECK(TARGET_FUNC_ID_BASE == 1024);
}

TEST_CASE("CoreFuncId extremes and count are wire-stable") {
  CHECK(static_cast<uint32_t>(CoreFuncId::OpAndBoolean) == 0);
  CHECK(static_cast<uint32_t>(CoreFuncId::ContextGetWhenResult) == 101);
  CHECK(static_cast<uint32_t>(CoreFuncId::OpEqualToEnum) == 102);
  CHECK(static_cast<uint32_t>(CoreFuncId::OpNotEqualToEnum) == 103);
  CHECK(static_cast<uint32_t>(CoreFuncId::ConvEnumToString) == 104);
  CHECK(static_cast<uint32_t>(CoreFuncId::ConvEnumToNumber) == 105);
  CHECK(static_cast<uint32_t>(CoreFuncId::SensorOtherwise) == 106);
  CHECK(static_cast<uint32_t>(CoreFuncId::SensorRuleTrigger) == 107);
  CHECK(kCoreFuncIdCount == 108);
  CHECK(static_cast<uint32_t>(CoreFuncId::SensorRuleTrigger) == kCoreFuncIdCount - 1);
  CHECK(kCoreFuncIdCount <= TARGET_FUNC_ID_BASE);
}

TEST_CASE("CoreFuncId group boundaries are wire-stable") {
  CHECK(static_cast<uint32_t>(CoreFuncId::OpAddNumber) == 3);
  CHECK(static_cast<uint32_t>(CoreFuncId::OpBitwiseAndNumber) == 10);
  CHECK(static_cast<uint32_t>(CoreFuncId::OpEqualToBoolean) == 16);
  CHECK(static_cast<uint32_t>(CoreFuncId::OpAddString) == 24);
  CHECK(static_cast<uint32_t>(CoreFuncId::OpEqualToNil) == 27);
  CHECK(static_cast<uint32_t>(CoreFuncId::OpEqualToNumberNil) == 30);
  CHECK(static_cast<uint32_t>(CoreFuncId::OpNotEqualToNilString) == 41);
  CHECK(static_cast<uint32_t>(CoreFuncId::ConvNumberToString) == 42);
  CHECK(static_cast<uint32_t>(CoreFuncId::BrainContextGetVariable) == 48);
  CHECK(static_cast<uint32_t>(CoreFuncId::SensorRandom) == 52);
  CHECK(static_cast<uint32_t>(CoreFuncId::ActuatorSwitchPage) == 57);
  CHECK(static_cast<uint32_t>(CoreFuncId::ActuatorYield) == 59);
  CHECK(static_cast<uint32_t>(CoreFuncId::ListGet) == 60);
  CHECK(static_cast<uint32_t>(CoreFuncId::MapKeys) == 62);
  CHECK(static_cast<uint32_t>(CoreFuncId::MathAbs) == 66);
  CHECK(static_cast<uint32_t>(CoreFuncId::MathTan) == 83);
  CHECK(static_cast<uint32_t>(CoreFuncId::StrLength) == 84);
  CHECK(static_cast<uint32_t>(CoreFuncId::StrConcat) == 95);
  CHECK(static_cast<uint32_t>(CoreFuncId::BufferFrom) == 96);
}
