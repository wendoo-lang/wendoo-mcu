#include "doctest/doctest.h"

#include "core/runtime/bytecode.h"
#include "core/runtime/core-func-id.h"
#include "core/runtime/core-host-functions.h"
#include "core/runtime/core-type-atom-id.h"
#include "core/runtime/execution-state.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/program.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"
#include "vm-harness.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using wendoo::CoreTypeAtomId;
using wendoo::ErrorCode;
using wendoo::ExecutionState;
using wendoo::Frame;
using wendoo::FunctionBytecode;
using wendoo::Instr;
using wendoo::isTruthy;
using wendoo::kFalseValue;
using wendoo::kNilValue;
using wendoo::kNoCaptures;
using wendoo::kNoFuncId;
using wendoo::kNoTypeIdx;
using wendoo::kOperandSchema;
using wendoo::kTrueValue;
using wendoo::Op;
using wendoo::OperandEncoding;
using wendoo::operandSchemaFor;
using wendoo::OperandSpec;
using wendoo::OpOperandSchema;
using wendoo::ProgramImage;
using wendoo::RegionArena;
using wendoo::runExecution;
using wendoo::RunResult;
using wendoo::RunStatus;
using wendoo::Span;
using wendoo::startExecution;
using wendoo::Status;
using wendoo::Value;
using wendoo::ValueTag;
using wendoo::test::kDeviceProfileCaps;

namespace {

/** True for the opcodes the dispatch loop implements. */
bool isImplementedOp(Op op) {
  switch (op) {
  case Op::PUSH_CONST_VAL:
  case Op::POP:
  case Op::DUP:
  case Op::SWAP:
  case Op::PUSH_CONST_NUM:
  case Op::PUSH_CONST_STR:
  case Op::STACK_SET_REL:
  case Op::JMP:
  case Op::JMP_IF_FALSE:
  case Op::JMP_IF_TRUE:
  case Op::RET:
  case Op::SPAWN_RULE:
  case Op::WHEN_START:
  case Op::WHEN_END:
  case Op::WHEN_END_PRESENT:
  case Op::WHEN_END_CHAIN:
  case Op::WHEN_END_PRESENT_CHAIN:
  case Op::DO_START:
  case Op::DO_END:
  case Op::LOAD_LOCAL:
  case Op::STORE_LOCAL:
  case Op::LOAD_VAR_SLOT:
  case Op::STORE_VAR_SLOT:
  case Op::LOAD_SYSTEM_VAR:
  case Op::STORE_SYSTEM_VAR:
  case Op::CALL:
  case Op::HOST_CALL:
  case Op::ACTION_CALL:
  case Op::HOST_ACTION_CALL:
  case Op::LOAD_CALLSITE_VAR:
  case Op::STORE_CALLSITE_VAR:
  case Op::STRUCT_NEW:
  case Op::STRUCT_COPY_EXCEPT:
  case Op::STRUCT_GET_FIELD:
  case Op::STRUCT_SET_FIELD:
  case Op::STRUCT_DEEP_COPY:
  case Op::GET_FIELD:
  case Op::SET_FIELD:
  case Op::INSTANCE_OF:
  case Op::CALL_INDIRECT:
  case Op::CALL_INDIRECT_ARGS:
  case Op::MAKE_CLOSURE:
  case Op::LOAD_CAPTURE:
  case Op::LIST_NEW:
  case Op::LIST_PUSH:
  case Op::LIST_GET:
  case Op::LIST_SET:
  case Op::LIST_LEN:
  case Op::LIST_POP:
  case Op::LIST_SHIFT:
  case Op::LIST_REMOVE:
  case Op::LIST_INSERT:
  case Op::LIST_SWAP:
  case Op::MAP_NEW:
  case Op::MAP_SET:
  case Op::MAP_GET:
  case Op::MAP_HAS:
  case Op::MAP_DELETE:
  case Op::TYPE_CHECK:
  case Op::YIELD:
  case Op::TRY:
  case Op::END_TRY:
  case Op::THROW:
  case Op::HOST_CALL_ASYNC:
  case Op::HOST_ACTION_CALL_ASYNC:
  case Op::ACTION_CALL_ASYNC:
  case Op::AWAIT:
    return true;
  default:
    return false;
  }
}

} // namespace

TEST_CASE("PUSH_CONST_VAL pushes every inline constant kind") {
  ProgramBuilder b;
  b.poolString("hi");
  b.atomType(CoreTypeAtomId::Number);
  b.valueNil().valueVoid().valueUnknown().valueBool(true).valueNumber(2.5f);
  b.valueString(0).valueEnum(0, 2).valueFunction(1);
  const uint32_t kinds = 8;
  for (uint32_t k = 0; k < kinds; k++) {
    b.beginFunction().instr(Op::PUSH_CONST_VAL, static_cast<int32_t>(k)).instr(Op::RET);
  }
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  const auto resultOf = [&](uint32_t funcId) {
    Machine machine;
    REQUIRE(startExecution(machine.state, image, funcId, {}).isOk());
    machine.state.budget = 10;
    const RunResult result = runExecution(machine.state, image);
    REQUIRE(result.status == RunStatus::Done);
    return result.result;
  };

  CHECK(resultOf(0).tag() == ValueTag::Nil);
  CHECK(resultOf(1).tag() == ValueTag::Void);
  CHECK(resultOf(2).tag() == ValueTag::Unknown);
  CHECK(resultOf(3).asBoolean());
  CHECK(resultOf(4).asNumber() == 2.5f);
  const Value str = resultOf(5);
  CHECK(str.tag() == ValueTag::String);
  CHECK(str.borrowedStringIndex() == 0);
  const Value enumValue = resultOf(6);
  CHECK(enumValue.tag() == ValueTag::Enum);
  CHECK(enumValue.typeId() == 0);
  CHECK(enumValue.enumOrdinal() == 2);
  const Value fn = resultOf(7);
  CHECK(fn.tag() == ValueTag::Function);
  CHECK(fn.functionId() == 1);
  CHECK(fn.functionCaptures() == kNoCaptures);
}

TEST_CASE("PUSH_CONST_VAL of a container constant faults ScriptError") {
  ProgramBuilder b;
  b.atomType(CoreTypeAtomId::Number).listType(0);
  b.valueEmptyList(1);
  b.beginFunction().instr(Op::PUSH_CONST_VAL, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::ScriptError);
  CHECK(result.site.funcId == 0);
  CHECK(result.site.pc == 0);
}

TEST_CASE("PUSH_CONST_NUM and PUSH_CONST_STR push their pools' entries") {
  ProgramBuilder b;
  b.poolString("abc");
  b.number(-7.25f);
  b.beginFunction().instr(Op::PUSH_CONST_NUM, 0).instr(Op::RET);
  b.beginFunction().instr(Op::PUSH_CONST_STR, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine numbers;
  REQUIRE(startExecution(numbers.state, image, 0, {}).isOk());
  numbers.state.budget = 10;
  const RunResult numberResult = runExecution(numbers.state, image);
  REQUIRE(numberResult.status == RunStatus::Done);
  CHECK(numberResult.result.asNumber() == -7.25f);

  Machine strings;
  REQUIRE(startExecution(strings.state, image, 1, {}).isOk());
  strings.state.budget = 10;
  const RunResult stringResult = runExecution(strings.state, image);
  REQUIRE(stringResult.status == RunStatus::Done);
  CHECK(stringResult.result.tag() == ValueTag::String);
  CHECK(stringResult.result.borrowedStringIndex() == 0);
}

TEST_CASE("each PUSH_CONST_* faults ScriptError on an out-of-range index") {
  const Op pushes[3] = {Op::PUSH_CONST_VAL, Op::PUSH_CONST_NUM, Op::PUSH_CONST_STR};
  for (const Op op : pushes) {
    CAPTURE(static_cast<int>(op));
    ProgramBuilder b;
    b.beginFunction().instr(op, 0).instr(Op::RET);
    std::vector<uint8_t> storage(16 * 1024);
    const ProgramImage image = b.build(storage);

    Machine machine;
    const RunResult result = runProgram(machine, image);
    REQUIRE(result.status == RunStatus::Fault);
    CHECK(result.error == ErrorCode::ScriptError);
    CHECK(result.site.pc == 0);
  }
}

TEST_CASE("POP, DUP, and SWAP rearrange the operand stack") {
  ProgramBuilder b;
  b.number(1.0f).number(2.0f);
  // [1, 2] -> SWAP -> [2, 1] -> POP -> [2] -> RET returns 2.
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::SWAP)
      .instr(Op::POP)
      .instr(Op::RET);
  // [1] -> DUP -> [1, 1] -> POP -> [1] -> RET returns 1.
  b.beginFunction().instr(Op::PUSH_CONST_NUM, 0).instr(Op::DUP).instr(Op::POP).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine swapped;
  REQUIRE(startExecution(swapped.state, image, 0, {}).isOk());
  swapped.state.budget = 10;
  const RunResult swapResult = runExecution(swapped.state, image);
  REQUIRE(swapResult.status == RunStatus::Done);
  CHECK(swapResult.result.asNumber() == 2.0f);

  Machine duped;
  REQUIRE(startExecution(duped.state, image, 1, {}).isOk());
  duped.state.budget = 10;
  const RunResult dupResult = runExecution(duped.state, image);
  REQUIRE(dupResult.status == RunStatus::Done);
  CHECK(dupResult.result.asNumber() == 1.0f);
}

TEST_CASE("STACK_SET_REL fills an arg buffer below the top of stack") {
  ProgramBuilder b;
  b.number(5.0f).number(7.0f);
  // Build the [nil, nil] buffer, then overwrite slot 0 (offset 1) and slot 1
  // (offset 0) per the calling-convention fill pattern.
  b.valueNil();
  b.beginFunction()
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::STACK_SET_REL, 1)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::STACK_SET_REL, 0)
      .instr(Op::SWAP)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Done);
  // The buffer held [5, 7]; SWAP exposes slot 0, so RET returns 5.
  CHECK(result.result.asNumber() == 5.0f);
  REQUIRE(machine.state.stackDepth == 1);
  CHECK(machine.state.stack[0].asNumber() == 5.0f);
}

TEST_CASE("STACK_SET_REL faults ScriptError when the offset passes the bottom") {
  ProgramBuilder b;
  b.number(1.0f);
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::STACK_SET_REL, 5)
      .instr(Op::RET);
  // With an empty post-pop stack there is no slot to write at any offset.
  b.beginFunction().instr(Op::PUSH_CONST_NUM, 0).instr(Op::STACK_SET_REL, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine outOfRange;
  REQUIRE(startExecution(outOfRange.state, image, 0, {}).isOk());
  outOfRange.state.budget = 10;
  const RunResult farResult = runExecution(outOfRange.state, image);
  REQUIRE(farResult.status == RunStatus::Fault);
  CHECK(farResult.error == ErrorCode::ScriptError);
  CHECK(farResult.site.pc == 2);

  Machine empty;
  REQUIRE(startExecution(empty.state, image, 1, {}).isOk());
  empty.state.budget = 10;
  const RunResult emptyResult = runExecution(empty.state, image);
  REQUIRE(emptyResult.status == RunStatus::Fault);
  CHECK(emptyResult.error == ErrorCode::ScriptError);
  CHECK(emptyResult.site.pc == 1);
}

TEST_CASE("JMP applies signed relative offsets in both directions") {
  ProgramBuilder b;
  b.number(7.0f);
  // 0: JMP +3 -> 3: JMP -2 -> 1: push 7 -> 2: JMP +2 -> 4: RET.
  b.beginFunction()
      .instr(Op::JMP, 3)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::JMP, 2)
      .instr(Op::JMP, -2)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 7.0f);
}

TEST_CASE("conditional jumps branch on the contract truthiness table") {
  // Each function pushes one constant and routes through JMP_IF_FALSE:
  // truthy continues to push 1, falsy jumps to push 0.
  ProgramBuilder b;
  b.poolString("").poolString("x");
  b.atomType(CoreTypeAtomId::Number);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  b.valueNil().valueVoid().valueUnknown().valueBool(false).valueBool(true);
  b.valueNumber(0.0f).valueNumber(-0.0f).valueNumber(2.0f).valueNumber(nan);
  b.valueString(0).valueString(1).valueEnum(0, 0).valueFunction(0);
  b.number(1.0f).number(0.0f);
  const bool expected[13] = {false, false, false, false, true, false, false,
                             true,  true,  false, true,  true, true};
  for (uint32_t k = 0; k < 13; k++) {
    b.beginFunction()
        .instr(Op::PUSH_CONST_VAL, static_cast<int32_t>(k))
        .instr(Op::JMP_IF_FALSE, 3)
        .instr(Op::PUSH_CONST_NUM, 0)
        .instr(Op::RET)
        .instr(Op::PUSH_CONST_NUM, 1)
        .instr(Op::RET);
  }
  std::vector<uint8_t> storage(32 * 1024);
  const ProgramImage image = b.build(storage);

  for (uint32_t k = 0; k < 13; k++) {
    CAPTURE(k);
    Machine machine;
    REQUIRE(startExecution(machine.state, image, k, {}).isOk());
    machine.state.budget = 10;
    const RunResult result = runExecution(machine.state, image);
    REQUIRE(result.status == RunStatus::Done);
    CHECK(result.result.asNumber() == (expected[k] ? 1.0f : 0.0f));
  }
}

TEST_CASE("JMP_IF_TRUE is the symmetric truthy branch") {
  ProgramBuilder b;
  b.valueBool(true).valueBool(false);
  b.number(1.0f).number(0.0f);
  for (uint32_t k = 0; k < 2; k++) {
    b.beginFunction()
        .instr(Op::PUSH_CONST_VAL, static_cast<int32_t>(k))
        .instr(Op::JMP_IF_TRUE, 3)
        .instr(Op::PUSH_CONST_NUM, 1)
        .instr(Op::RET)
        .instr(Op::PUSH_CONST_NUM, 0)
        .instr(Op::RET);
  }
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  for (uint32_t k = 0; k < 2; k++) {
    CAPTURE(k);
    Machine machine;
    REQUIRE(startExecution(machine.state, image, k, {}).isOk());
    machine.state.budget = 10;
    const RunResult result = runExecution(machine.state, image);
    REQUIRE(result.status == RunStatus::Done);
    // Function 0 pushes true and takes the jump to the marker 1; function 1
    // pushes false and falls through to the marker 0.
    CHECK(result.result.asNumber() == (k == 0 ? 1.0f : 0.0f));
  }
}

TEST_CASE("isTruthy covers the VM-internal value kinds directly") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  CHECK(isTruthy(Value::handle(3), image));
  CHECK(!isTruthy(Value::error(ErrorCode::ScriptError), image));
  CHECK(!isTruthy(kNilValue, image));
  CHECK(isTruthy(kTrueValue, image));
  CHECK(!isTruthy(kFalseValue, image));
}

TEST_CASE("WHEN/DO boundaries gate the DO section on the WHEN result") {
  // Mirrors the compiled rule shape: WHEN_START, condition, WHEN_END +4,
  // DO_START, body, RET; the falsy path lands past the DO section.
  ProgramBuilder b;
  b.valueBool(true).valueBool(false).valueNil();
  b.number(42.0f);
  for (uint32_t k = 0; k < 2; k++) {
    b.beginFunction()
        .instr(Op::WHEN_START)
        .instr(Op::PUSH_CONST_VAL, static_cast<int32_t>(k))
        .instr(Op::WHEN_END, 4)
        .instr(Op::DO_START)
        .instr(Op::PUSH_CONST_NUM, 0)
        .instr(Op::RET)
        .instr(Op::PUSH_CONST_VAL, 2)
        .instr(Op::RET);
  }
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine truthy;
  REQUIRE(startExecution(truthy.state, image, 0, {}).isOk());
  truthy.state.budget = 10;
  const RunResult ranDo = runExecution(truthy.state, image);
  REQUIRE(ranDo.status == RunStatus::Done);
  CHECK(ranDo.result.asNumber() == 42.0f);

  Machine falsy;
  REQUIRE(startExecution(falsy.state, image, 1, {}).isOk());
  falsy.state.budget = 10;
  const RunResult skippedDo = runExecution(falsy.state, image);
  REQUIRE(skippedDo.status == RunStatus::Done);
  CHECK(skippedDo.result.tag() == ValueTag::Nil);
}

TEST_CASE("WHEN_END_PRESENT gates the DO section on presence, not truthiness") {
  // Same rule shape as WHEN_END: WHEN_START, value, WHEN_END_PRESENT +4,
  // DO_START, body, RET; the absent (nil) path lands past the DO section. The
  // present-but-falsy value 0 runs DO; nil skips it.
  ProgramBuilder b;
  b.valueNil();    // value pool index 0: nil (absent)
  b.number(0.0f);  // number pool index 0: 0 (present, falsy)
  b.number(42.0f); // number pool index 1: DO sentinel
  // func 0: a present falsy 0 runs DO and returns 42.
  b.beginFunction()
      .instr(Op::WHEN_START)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::WHEN_END_PRESENT, 4)
      .instr(Op::DO_START)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::RET)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::RET);
  // func 1: nil (absent) skips DO and returns nil.
  b.beginFunction()
      .instr(Op::WHEN_START)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::WHEN_END_PRESENT, 4)
      .instr(Op::DO_START)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::RET)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine present;
  REQUIRE(startExecution(present.state, image, 0, {}).isOk());
  present.state.budget = 10;
  const RunResult ranDo = runExecution(present.state, image);
  REQUIRE(ranDo.status == RunStatus::Done);
  CHECK(ranDo.result.asNumber() == 42.0f);

  Machine absent;
  REQUIRE(startExecution(absent.state, image, 1, {}).isOk());
  absent.state.budget = 10;
  const RunResult skippedDo = runExecution(absent.state, image);
  REQUIRE(skippedDo.status == RunStatus::Done);
  CHECK(skippedDo.result.tag() == ValueTag::Nil);
}

TEST_CASE("DO_END is a pure marker") {
  ProgramBuilder b;
  b.number(3.0f);
  b.beginFunction().instr(Op::PUSH_CONST_NUM, 0).instr(Op::DO_END).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 3.0f);
}

TEST_CASE("locals are seeded from args, nil-filled, and frame-indexed") {
  ProgramBuilder b;
  // params 2 + 1 extra local: copy arg 1 into the extra slot and return it.
  b.beginFunction(2, 1)
      .instr(Op::LOAD_LOCAL, 1)
      .instr(Op::STORE_LOCAL, 2)
      .instr(Op::LOAD_LOCAL, 2)
      .instr(Op::RET);
  // The extra local reads nil before any store.
  b.beginFunction(0, 1).instr(Op::LOAD_LOCAL, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const Value args[3] = {Value::number(10.0f), Value::number(20.0f), Value::number(30.0f)};
  // Excess args beyond numLocals are dropped.
  REQUIRE(startExecution(machine.state, image, 0, Span<const Value>(args, 3)).isOk());
  CHECK(machine.state.localsDepth == 3);
  machine.state.budget = 10;
  const RunResult result = runExecution(machine.state, image);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 20.0f);
  CHECK(machine.state.localsDepth == 0);

  Machine nilLocal;
  REQUIRE(startExecution(nilLocal.state, image, 1, {}).isOk());
  nilLocal.state.budget = 10;
  const RunResult nilResult = runExecution(nilLocal.state, image);
  REQUIRE(nilResult.status == RunStatus::Done);
  CHECK(nilResult.result.tag() == ValueTag::Nil);
}

TEST_CASE("LOAD_LOCAL and STORE_LOCAL fault ScriptError out of range") {
  ProgramBuilder b;
  b.number(1.0f);
  b.beginFunction(0, 1).instr(Op::LOAD_LOCAL, 1).instr(Op::RET);
  b.beginFunction(0, 1).instr(Op::PUSH_CONST_NUM, 0).instr(Op::STORE_LOCAL, 1).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine load;
  REQUIRE(startExecution(load.state, image, 0, {}).isOk());
  load.state.budget = 10;
  const RunResult loadResult = runExecution(load.state, image);
  REQUIRE(loadResult.status == RunStatus::Fault);
  CHECK(loadResult.error == ErrorCode::ScriptError);

  Machine store;
  REQUIRE(startExecution(store.state, image, 1, {}).isOk());
  store.state.budget = 10;
  const RunResult storeResult = runExecution(store.state, image);
  REQUIRE(storeResult.status == RunStatus::Fault);
  CHECK(storeResult.error == ErrorCode::ScriptError);
  CHECK(storeResult.site.pc == 1);
  // The bounds check precedes the pop, so the operand is still on the stack.
  CHECK(store.state.stackDepth == 1);
}

TEST_CASE("RET returns to the caller frame, cleaning stack and locals") {
  ProgramBuilder b;
  b.number(1.0f).number(2.0f).number(3.0f);
  // The "caller" body: a lone RET consuming the callee's pushed result.
  b.beginFunction(0, 1).instr(Op::RET);
  // The "callee" leaks two extra values below its return value.
  b.beginFunction(0, 2)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::PUSH_CONST_NUM, 2)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  REQUIRE(startExecution(machine.state, image, 0, {}).isOk());
  // Seed two caller operands so the callee's base sits above them, then
  // enter the callee as a call in progress (the caller pc already advanced).
  machine.state.stack[0] = Value::number(-1.0f);
  machine.state.stack[1] = Value::number(-2.0f);
  machine.state.stackDepth = 2;
  REQUIRE(startExecution(machine.state, image, 1, {}).isOk());
  CHECK(machine.state.frameDepth == 2);
  CHECK(machine.state.frames[1].base == 2);
  CHECK(machine.state.localsDepth == 3);

  machine.state.budget = 10;
  const RunResult result = runExecution(machine.state, image);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 3.0f);
  // The callee's leaked operands were truncated to its base before the
  // return value was pushed, and both frames released their locals.
  CHECK(machine.state.frameDepth == 0);
  CHECK(machine.state.stackDepth == 1);
  CHECK(machine.state.stack[0].asNumber() == 3.0f);
  CHECK(machine.state.localsDepth == 0);
}

TEST_CASE("budget exhaustion suspends at an instruction boundary and resumes") {
  ProgramBuilder b;
  b.number(1.0f).number(2.0f).number(3.0f);
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::POP)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::POP)
      .instr(Op::PUSH_CONST_NUM, 2)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  REQUIRE(startExecution(machine.state, image, 0, {}).isOk());
  machine.state.budget = 3;
  const RunResult slice = runExecution(machine.state, image);
  REQUIRE(slice.status == RunStatus::Yielded);
  CHECK(machine.state.budget == 0);
  CHECK(machine.state.frames[0].pc == 3);
  REQUIRE(machine.state.stackDepth == 1);
  CHECK(machine.state.stack[0].asNumber() == 2.0f);

  machine.state.budget = 100;
  const RunResult resumed = runExecution(machine.state, image);
  REQUIRE(resumed.status == RunStatus::Done);
  CHECK(resumed.result.asNumber() == 3.0f);
  CHECK(machine.state.budget == 97);
}

TEST_CASE("entering the loop without a positive budget is a host-contract fault") {
  ProgramBuilder b;
  b.number(1.0f);
  b.beginFunction().instr(Op::PUSH_CONST_NUM, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  REQUIRE(startExecution(machine.state, image, 0, {}).isOk());
  machine.state.budget = 0;
  const RunResult guarded = runExecution(machine.state, image);
  REQUIRE(guarded.status == RunStatus::Fault);
  CHECK(guarded.error == ErrorCode::HostError);
  CHECK(guarded.site.funcId == kNoFuncId);
  // The state is untouched and still runnable.
  CHECK(machine.state.frames[0].pc == 0);
  CHECK(machine.state.stackDepth == 0);
  machine.state.budget = 10;
  const RunResult result = runExecution(machine.state, image);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 1.0f);
}

TEST_CASE("popping opcodes fault StackUnderflow on an empty stack") {
  const Op poppers[8] = {Op::POP,          Op::DUP,         Op::SWAP,     Op::STACK_SET_REL,
                         Op::JMP_IF_FALSE, Op::JMP_IF_TRUE, Op::WHEN_END, Op::WHEN_END_PRESENT};
  for (const Op op : poppers) {
    CAPTURE(static_cast<int>(op));
    ProgramBuilder b;
    b.beginFunction().instr(op, 1).instr(Op::RET);
    std::vector<uint8_t> storage(16 * 1024);
    const ProgramImage image = b.build(storage);

    Machine machine;
    const RunResult result = runProgram(machine, image);
    REQUIRE(result.status == RunStatus::Fault);
    CHECK(result.error == ErrorCode::StackUnderflow);
    CHECK(result.site.funcId == 0);
    CHECK(result.site.pc == 0);
  }
}

TEST_CASE("RET on an empty stack faults StackUnderflow") {
  ProgramBuilder b;
  b.beginFunction().instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::StackUnderflow);
}

TEST_CASE("SWAP with one value faults StackUnderflow") {
  ProgramBuilder b;
  b.number(1.0f);
  b.beginFunction().instr(Op::PUSH_CONST_NUM, 0).instr(Op::SWAP).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::StackUnderflow);
  CHECK(result.site.pc == 1);
}

TEST_CASE("pushing past the operand-stack limit faults StackOverflow") {
  ProgramBuilder b;
  b.number(1.0f);
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine(2);
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::StackOverflow);
  CHECK(result.site.pc == 2);
  CHECK(machine.state.stackDepth == 2);
}

TEST_CASE("startExecution rejects bad funcIds and exhausted regions") {
  ProgramBuilder b;
  b.beginFunction(0, 4).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const Status badFunc = startExecution(machine.state, image, 9, {});
  REQUIRE(!badFunc.isOk());
  CHECK(badFunc.error() == ErrorCode::HostError);

  Machine noLocals(kDeviceProfileCaps.maxStackSize, 2);
  const Status localsFull = startExecution(noLocals.state, image, 0, {});
  REQUIRE(!localsFull.isOk());
  CHECK(localsFull.error() == ErrorCode::StackOverflow);

  Machine noFrames(kDeviceProfileCaps.maxStackSize, kDeviceProfileCaps.maxLocalsSize, 1);
  REQUIRE(startExecution(noFrames.state, image, 0, {}).isOk());
  const Status framesFull = startExecution(noFrames.state, image, 0, {});
  REQUIRE(!framesFull.isOk());
  CHECK(framesFull.error() == ErrorCode::StackOverflow);
}

TEST_CASE("a jump landing outside the function faults ScriptError at the bad pc") {
  ProgramBuilder b;
  b.beginFunction().instr(Op::JMP, 5).instr(Op::RET);
  b.beginFunction().instr(Op::JMP, -1).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine forward;
  REQUIRE(startExecution(forward.state, image, 0, {}).isOk());
  forward.state.budget = 10;
  const RunResult pastEnd = runExecution(forward.state, image);
  REQUIRE(pastEnd.status == RunStatus::Fault);
  CHECK(pastEnd.error == ErrorCode::ScriptError);
  CHECK(pastEnd.site.funcId == 0);
  CHECK(pastEnd.site.pc == 5);

  // A negative pc wraps the unsigned counter and lands out of bounds.
  Machine backward;
  REQUIRE(startExecution(backward.state, image, 1, {}).isOk());
  backward.state.budget = 10;
  const RunResult beforeStart = runExecution(backward.state, image);
  REQUIRE(beforeStart.status == RunStatus::Fault);
  CHECK(beforeStart.error == ErrorCode::ScriptError);
}

TEST_CASE("running with no live frame faults ScriptError") {
  ProgramBuilder b;
  b.valueNil();
  b.beginFunction().instr(Op::PUSH_CONST_VAL, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine never;
  never.state.budget = 10;
  const RunResult unstarted = runExecution(never.state, image);
  REQUIRE(unstarted.status == RunStatus::Fault);
  CHECK(unstarted.error == ErrorCode::ScriptError);
  CHECK(unstarted.site.funcId == kNoFuncId);

  Machine machine;
  const RunResult completed = runProgram(machine, image);
  REQUIRE(completed.status == RunStatus::Done);
  machine.state.budget = 10;
  const RunResult reentered = runExecution(machine.state, image);
  REQUIRE(reentered.status == RunStatus::Fault);
  CHECK(reentered.error == ErrorCode::ScriptError);
}

// The two reserved opcode numbers carry no VM handler; every other declared
// opcode is a contract opcode the dispatch loop must implement.
bool isReservedOp(Op op) { return op == Op::RESERVED_111 || op == Op::RESERVED_112; }

TEST_CASE("every contract opcode has an implemented dispatch arm") {
  // Conformance: with ACTION_CALL_ASYNC landed, the only declared opcodes the
  // dispatch loop does not implement are the reserved (handler-free) numbers.
  // Every other opcode must have a dispatch arm - none faults as unimplemented.
  for (const OpOperandSchema& row : kOperandSchema) {
    CAPTURE(static_cast<int>(row.op));
    if (isReservedOp(row.op)) {
      CHECK_FALSE(isImplementedOp(row.op));
    } else {
      CHECK(isImplementedOp(row.op));
    }
  }
}

TEST_CASE("the reserved opcodes fault ScriptError deterministically") {
  for (const OpOperandSchema& row : kOperandSchema) {
    if (!isReservedOp(row.op)) {
      continue;
    }
    CAPTURE(static_cast<int>(row.op));
    ProgramBuilder b;
    b.beginFunction().instr(row.op).instr(Op::RET);
    std::vector<uint8_t> storage(16 * 1024);
    const ProgramImage image = b.build(storage);

    Machine machine;
    const RunResult result = runProgram(machine, image);
    REQUIRE(result.status == RunStatus::Fault);
    CHECK(result.error == ErrorCode::ScriptError);
    CHECK(result.site.funcId == 0);
    CHECK(result.site.pc == 0);
  }
}

TEST_CASE("HOST_ACTION_CALL of an unregistered action id faults ScriptError") {
  // The committed device fixture's call shape: two nil args, then
  // HOST_ACTION_CALL with the stable action id, argc, and callSiteId. No
  // binding table is supplied, so the existence check fails.
  ProgramBuilder b;
  b.valueNil();
  b.beginFunction()
      .instr(Op::WHEN_START)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::HOST_ACTION_CALL, 0x400, 2, 0)
      .instr(Op::WHEN_END, 2)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::ScriptError);
  CHECK(result.site.pc == 3);
  // The args stay where the fault left them; nothing was popped.
  CHECK(machine.state.stackDepth == 2);
}

TEST_CASE("an opcode value outside the declared set faults, never UB") {
  const Instr code[2] = {{static_cast<Op>(200), 0, 0, 0}, {Op::RET, 0, 0, 0}};
  const FunctionBytecode fn{0, 2, 0, 0, kNoTypeIdx};
  ProgramImage image{};
  image.functions = Span<const FunctionBytecode>(&fn, 1);
  image.instructions = Span<const Instr>(code, 2);

  Machine machine;
  REQUIRE(startExecution(machine.state, image, 0, {}).isOk());
  machine.state.budget = 10;
  const RunResult result = runExecution(machine.state, image);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::ScriptError);
  CHECK(result.site.pc == 0);
}

TEST_CASE("a suspended state survives while an independent state runs to completion") {
  ProgramBuilder b;
  b.number(1.0f).number(2.0f).number(3.0f);
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::PUSH_CONST_NUM, 2)
      .instr(Op::SWAP)
      .instr(Op::POP)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  std::array<uint8_t, 32 * sizeof(Value) + 8 * sizeof(Frame)> arenaBytes;
  RegionArena arena(Span<uint8_t>(arenaBytes.data(), arenaBytes.size()));

  // Each state's regions come from independent arena allocations, mirroring the
  // per-fiber regions the scheduler draws on. They are pre-allocated to their
  // full capacity with no grow allocator, so the caps double as fixed sizes.
  const auto bindState = [&](uint32_t stackSlots, uint32_t localSlots, uint32_t frameSlots) {
    ExecutionState state{};
    state.stack =
        static_cast<Value*>(arena.allocateBytes(stackSlots * sizeof(Value), alignof(Value)));
    state.stackLimit = stackSlots;
    state.stackCapacity = stackSlots;
    state.locals =
        static_cast<Value*>(arena.allocateBytes(localSlots * sizeof(Value), alignof(Value)));
    state.localsLimit = localSlots;
    state.localsCapacity = localSlots;
    state.frames =
        static_cast<Frame*>(arena.allocateBytes(frameSlots * sizeof(Frame), alignof(Frame)));
    state.frameLimit = frameSlots;
    state.frameCapacity = frameSlots;
    REQUIRE(state.stack != nullptr);
    REQUIRE(state.locals != nullptr);
    REQUIRE(state.frames != nullptr);
    return state;
  };

  // Suspend the first state two instructions in.
  ExecutionState suspended = bindState(8, 4, 2);
  REQUIRE(startExecution(suspended, image, 0, {}).isOk());
  suspended.budget = 2;
  REQUIRE(runExecution(suspended, image).status == RunStatus::Yielded);

  // A second, independent state runs to completion.
  ExecutionState transient = bindState(8, 4, 2);
  REQUIRE(startExecution(transient, image, 0, {}).isOk());
  transient.budget = 100;
  const RunResult transientResult = runExecution(transient, image);
  REQUIRE(transientResult.status == RunStatus::Done);
  CHECK(transientResult.result.asNumber() == 3.0f);

  // The suspended state's live range was untouched; it resumes and finishes.
  CHECK(suspended.stack[0].asNumber() == 1.0f);
  CHECK(suspended.stack[1].asNumber() == 2.0f);
  suspended.budget = 100;
  const RunResult resumed = runExecution(suspended, image);
  REQUIRE(resumed.status == RunStatus::Done);
  CHECK(resumed.result.asNumber() == 3.0f);
}

namespace {

/** Observer recording host-action dispatches and faults for assertions. */
struct RecordingObserver : wendoo::VmObserver {
  struct ActionCall {
    uint32_t actionId;
    uint32_t callSiteId;
    std::vector<Value> args;
    Value result;
  };
  struct Fault {
    uint32_t fiberId;
    ErrorCode code;
  };
  std::vector<ActionCall> actionCalls;
  std::vector<Fault> faults;

  void onHostActionCall(uint32_t actionId, uint32_t callSiteId, Span<const Value> args,
                        const Value& result) override {
    ActionCall call{actionId, callSiteId, {}, result};
    for (size_t i = 0; i < args.size(); i++) {
      call.args.push_back(args[i]);
    }
    actionCalls.push_back(call);
  }

  void onFiberFault(uint32_t fiberId, ErrorCode code) override {
    faults.push_back(Fault{fiberId, code});
  }
};

/** Body summing its numeric args and stashing the call's arg count in callsite state. */
Value execSumAction(void* hostData, wendoo::ExecutionContext& ctx, Span<const Value> args) {
  static_cast<void>(hostData);
  float sum = 0.0f;
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i].isNumber()) {
      sum += args[i].asNumber();
    }
  }
  ctx.setCallSiteState(Value::number(static_cast<float>(args.size())));
  return Value::number(sum);
}

} // namespace

TEST_CASE("bindSlots seeds a variable slot from the program's starting value") {
  ProgramBuilder b;
  b.poolString("count");
  b.poolString("label");
  b.valueNumber(0.0f); // value slot 0
  b.valueString(1);    // value slot 1
  b.valueBool(false);  // value slot 2
  b.brainVariable(0, 0);
  b.brainVariable(1, 1);
  b.brainVariable(0, 2);
  // A fourth slot declares no starting value.
  b.brainVariable(1);
  b.beginFunction().instr(Op::LOAD_VAR_SLOT, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  REQUIRE(image.variableInitValues.size() == 4);
  CHECK(image.variableInitValues[0] == 0);
  CHECK(image.variableInitValues[3] == wendoo::kNoVariableInit);

  std::array<uint8_t, 512> ctxStorage;
  wendoo::RegionArena ctxArena(Span<uint8_t>(ctxStorage.data(), ctxStorage.size()));
  wendoo::ExecutionContext ctx;
  REQUIRE(ctx.bindSlots(ctxArena, 4, 0, 0, 0, 0, image.variableInitValues, image.constValues));

  CHECK(ctx.variables[0].tag() == ValueTag::Number);
  CHECK(ctx.variables[0].asNumber() == 0.0f);
  CHECK(ctx.variables[1].tag() == ValueTag::String);
  CHECK(ctx.variables[2].tag() == ValueTag::Boolean);
  CHECK(ctx.variables[2].asBoolean() == false);
  CHECK(ctx.variables[3].tag() == ValueTag::Nil);

  // The seeded value is what LOAD_VAR_SLOT observes before any store.
  wendoo::RuntimeSurface surface;
  surface.context = &ctx;
  Machine machine;
  const RunResult loaded = runProgram(machine, image, {}, 1000, surface);
  REQUIRE(loaded.status == RunStatus::Done);
  CHECK(loaded.result.tag() == ValueTag::Number);
  CHECK(loaded.result.asNumber() == 0.0f);
}

TEST_CASE("LOAD_VAR_SLOT and STORE_VAR_SLOT round-trip brain variables") {
  ProgramBuilder b;
  b.poolString("counter");
  b.number(9.0f);
  b.brainVariable(0);
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::STORE_VAR_SLOT, 0)
      .instr(Op::LOAD_VAR_SLOT, 0)
      .instr(Op::RET);
  // An unstored slot loads nil.
  b.beginFunction().instr(Op::LOAD_VAR_SLOT, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  std::array<uint8_t, 256> ctxStorage;
  wendoo::RegionArena ctxArena(Span<uint8_t>(ctxStorage.data(), ctxStorage.size()));
  wendoo::ExecutionContext ctx;
  REQUIRE(ctx.bindSlots(ctxArena, 1, 0));
  wendoo::RuntimeSurface surface;
  surface.context = &ctx;

  Machine machine;
  const RunResult stored = runProgram(machine, image, {}, 1000, surface);
  REQUIRE(stored.status == RunStatus::Done);
  CHECK(stored.result.asNumber() == 9.0f);
  CHECK(ctx.variables[0].asNumber() == 9.0f);

  std::array<uint8_t, 256> freshStorage;
  wendoo::RegionArena freshArena(Span<uint8_t>(freshStorage.data(), freshStorage.size()));
  wendoo::ExecutionContext fresh;
  REQUIRE(fresh.bindSlots(freshArena, 1, 0));
  surface.context = &fresh;
  Machine unstored;
  REQUIRE(startExecution(unstored.state, image, 1, {}).isOk());
  unstored.state.budget = 10;
  const RunResult nilLoad = runExecution(unstored.state, image, surface);
  REQUIRE(nilLoad.status == RunStatus::Done);
  CHECK(nilLoad.result.tag() == ValueTag::Nil);
}

TEST_CASE("LOAD_SYSTEM_VAR and STORE_SYSTEM_VAR round-trip the System store") {
  ProgramBuilder b;
  b.number(9.0f);
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::STORE_SYSTEM_VAR, 0)
      .instr(Op::LOAD_SYSTEM_VAR, 0)
      .instr(Op::RET);
  // An unwritten / out-of-range System slot loads nil.
  b.beginFunction().instr(Op::LOAD_SYSTEM_VAR, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  std::array<uint8_t, 256> ctxStorage;
  wendoo::RegionArena ctxArena(Span<uint8_t>(ctxStorage.data(), ctxStorage.size()));
  wendoo::ExecutionContext ctx;
  REQUIRE(ctx.bindSlots(ctxArena, 0, 0, 0, 1));
  wendoo::RuntimeSurface surface;
  surface.context = &ctx;

  Machine machine;
  const RunResult stored = runProgram(machine, image, {}, 1000, surface);
  REQUIRE(stored.status == RunStatus::Done);
  CHECK(stored.result.asNumber() == 9.0f);
  CHECK(ctx.systemStore[0].asNumber() == 9.0f);

  // A context with no System slots reads nil for any slot (out of range).
  std::array<uint8_t, 256> freshStorage;
  wendoo::RegionArena freshArena(Span<uint8_t>(freshStorage.data(), freshStorage.size()));
  wendoo::ExecutionContext fresh;
  REQUIRE(fresh.bindSlots(freshArena, 0, 0, 0, 0));
  surface.context = &fresh;
  Machine unstored;
  REQUIRE(startExecution(unstored.state, image, 1, {}).isOk());
  unstored.state.budget = 10;
  const RunResult nilLoad = runExecution(unstored.state, image, surface);
  REQUIRE(nilLoad.status == RunStatus::Done);
  CHECK(nilLoad.result.tag() == ValueTag::Nil);
}

TEST_CASE("var-slot opcodes fault ScriptError past the variable table") {
  ProgramBuilder b;
  b.poolString("v");
  b.number(1.0f);
  b.brainVariable(0);
  b.beginFunction().instr(Op::LOAD_VAR_SLOT, 1).instr(Op::RET);
  b.beginFunction().instr(Op::PUSH_CONST_NUM, 0).instr(Op::STORE_VAR_SLOT, 1).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  wendoo::ExecutionContext ctx;
  wendoo::RuntimeSurface surface;
  surface.context = &ctx;

  for (uint32_t funcId = 0; funcId < 2; funcId++) {
    CAPTURE(funcId);
    Machine machine;
    REQUIRE(startExecution(machine.state, image, funcId, {}).isOk());
    machine.state.budget = 10;
    const RunResult result = runExecution(machine.state, image, surface);
    REQUIRE(result.status == RunStatus::Fault);
    CHECK(result.error == ErrorCode::ScriptError);
  }
}

TEST_CASE("var-slot opcodes without a bound context fault HostError") {
  ProgramBuilder b;
  b.poolString("v");
  b.brainVariable(0);
  b.beginFunction().instr(Op::LOAD_VAR_SLOT, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::HostError);
}

TEST_CASE("HOST_ACTION_CALL dispatches the registered body per the calling convention") {
  ProgramBuilder b;
  b.number(2.5f).number(4.0f);
  b.valueNil();
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::HOST_ACTION_CALL, 7, 3, 5)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  const wendoo::HostActionBinding bindings[1] = {{7, &execSumAction, nullptr, nullptr}};
  std::array<uint8_t, 256> ctxStorage;
  wendoo::RegionArena ctxArena(Span<uint8_t>(ctxStorage.data(), ctxStorage.size()));
  wendoo::ExecutionContext ctx;
  REQUIRE(ctx.bindSlots(ctxArena, 0, 6));
  RecordingObserver observer;
  wendoo::RuntimeSurface surface;
  surface.context = &ctx;
  surface.actions = Span<const wendoo::HostActionBinding>(bindings, 1);
  surface.observer = &observer;

  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, surface);
  REQUIRE(result.status == RunStatus::Done);
  // The body summed the numeric args; the nil slot contributed nothing.
  CHECK(result.result.asNumber() == 6.5f);
  // The args were popped and the result pushed in their place.
  CHECK(machine.state.stackDepth == 1);

  // The binding observed the positional buffer and the bound call site.
  REQUIRE(observer.actionCalls.size() == 1);
  CHECK(observer.actionCalls[0].actionId == 7);
  CHECK(observer.actionCalls[0].callSiteId == 5);
  REQUIRE(observer.actionCalls[0].args.size() == 3);
  CHECK(observer.actionCalls[0].args[0].asNumber() == 2.5f);
  CHECK(observer.actionCalls[0].args[1].tag() == ValueTag::Nil);
  CHECK(observer.actionCalls[0].args[2].asNumber() == 4.0f);
  CHECK(observer.actionCalls[0].result.asNumber() == 6.5f);

  // The callsite binding was keyed by the instruction's callSiteId and reset
  // after the dispatch returned.
  CHECK(ctx.callSiteStatePresent[5]);
  CHECK(ctx.callSiteStates[5].asNumber() == 3.0f);
  CHECK(ctx.currentCallSiteId == wendoo::kNoCallSiteId);
}

TEST_CASE("HOST_ACTION_CALL faults StackUnderflow when argc exceeds the stack") {
  ProgramBuilder b;
  b.valueNil();
  b.beginFunction()
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::HOST_ACTION_CALL, 7, 2, 0)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  const wendoo::HostActionBinding bindings[1] = {{7, &execSumAction, nullptr, nullptr}};
  wendoo::ExecutionContext ctx;
  wendoo::RuntimeSurface surface;
  surface.context = &ctx;
  surface.actions = Span<const wendoo::HostActionBinding>(bindings, 1);

  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, surface);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::StackUnderflow);
  CHECK(result.site.pc == 1);
}

TEST_CASE("HOST_ACTION_CALL with a registered action but no context faults HostError") {
  ProgramBuilder b;
  b.beginFunction().instr(Op::HOST_ACTION_CALL, 7, 0, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  const wendoo::HostActionBinding bindings[1] = {{7, &execSumAction, nullptr, nullptr}};
  wendoo::RuntimeSurface surface;
  surface.actions = Span<const wendoo::HostActionBinding>(bindings, 1);

  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, surface);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::HostError);
}

// ---- Structs, closures, and function calls ----

namespace {

/** A heap-backed surface for the struct/closure opcodes; no collection fires at
 * these test sizes, so the heap needs no root source. */
struct HeapHarness {
  std::vector<uint8_t> storage = std::vector<uint8_t>(16 * 1024);
  RegionArena arena{Span<uint8_t>(storage.data(), storage.size())};
  wendoo::ManagedHeap heap{arena};
  wendoo::RuntimeSurface surface{nullptr, {}, nullptr, &heap};
};

} // namespace

TEST_CASE("STRUCT_SET_FIELD is a pure store visible through every struct reference") {
  ProgramBuilder b;
  b.poolString("Pair");
  b.structType(0, 2);
  b.number(7.0f);
  // local0 = new Pair; local1 = the same handle (an alias); write field 0 of one
  // and read it back through the other.
  b.beginFunction(0, 2)
      .instr(Op::STRUCT_NEW, 0, 0)
      .instr(Op::STORE_LOCAL, 0)
      .instr(Op::LOAD_LOCAL, 0)
      .instr(Op::STORE_LOCAL, 1)
      .instr(Op::LOAD_LOCAL, 0)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::STRUCT_SET_FIELD, 0)
      .instr(Op::POP)
      .instr(Op::LOAD_LOCAL, 1)
      .instr(Op::STRUCT_GET_FIELD, 0)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  HeapHarness h;
  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, h.surface);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 7.0f);
}

TEST_CASE("STRUCT_DEEP_COPY breaks aliasing so the original mutation is invisible") {
  ProgramBuilder b;
  b.poolString("Pair");
  b.structType(0, 2);
  b.number(7.0f).number(9.0f);
  // local0 = new Pair{0: 7}; local1 = deep copy; mutate the original's field 0
  // to 9; the copy still reads 7.
  b.beginFunction(0, 2)
      .instr(Op::STRUCT_NEW, 0, 0)
      .instr(Op::STORE_LOCAL, 0)
      .instr(Op::LOAD_LOCAL, 0)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::STRUCT_SET_FIELD, 0)
      .instr(Op::POP)
      .instr(Op::LOAD_LOCAL, 0)
      .instr(Op::STRUCT_DEEP_COPY)
      .instr(Op::STORE_LOCAL, 1)
      .instr(Op::LOAD_LOCAL, 0)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::STRUCT_SET_FIELD, 0)
      .instr(Op::POP)
      .instr(Op::LOAD_LOCAL, 1)
      .instr(Op::STRUCT_GET_FIELD, 0)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  HeapHarness h;
  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, h.surface);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 7.0f);
}

TEST_CASE("STRUCT_NEW with a non-zero reserved operand faults ScriptError") {
  ProgramBuilder b;
  b.poolString("Pair");
  b.structType(0, 2);
  b.beginFunction().instr(Op::STRUCT_NEW, 1, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  HeapHarness h;
  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, h.surface);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::ScriptError);
  CHECK(result.site.pc == 0);
}

TEST_CASE("GET_FIELD and SET_FIELD degrade to nil and a no-op on the binary path") {
  ProgramBuilder b;
  b.poolString("Pair");
  b.poolString("x");
  b.structType(0, 1);
  b.number(5.0f).number(8.0f);
  // s.0 = 5; SET_FIELD "x" = 8 (dropped); read field 0 by id -> still 5;
  // GET_FIELD "x" -> nil (proven by TYPE_CHECK Nil -> true). Return field 0.
  b.beginFunction(0, 1)
      .instr(Op::STRUCT_NEW, 0, 0)
      .instr(Op::STORE_LOCAL, 0)
      .instr(Op::LOAD_LOCAL, 0)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::STRUCT_SET_FIELD, 0)
      .instr(Op::POP)
      .instr(Op::LOAD_LOCAL, 0)
      .instr(Op::PUSH_CONST_STR, 1)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::SET_FIELD)
      .instr(Op::POP)
      .instr(Op::LOAD_LOCAL, 0)
      .instr(Op::STRUCT_GET_FIELD, 0)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  HeapHarness h;
  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, h.surface);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 5.0f); // SET_FIELD never wrote
}

TEST_CASE("GET_FIELD reads nil for an absent name-keyed field") {
  ProgramBuilder b;
  b.poolString("Pair");
  b.poolString("x");
  b.structType(0, 1);
  b.beginFunction(0, 1)
      .instr(Op::STRUCT_NEW, 0, 0)
      .instr(Op::PUSH_CONST_STR, 1)
      .instr(Op::GET_FIELD)
      .instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  HeapHarness h;
  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, h.surface);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.isNil());
}

TEST_CASE("INSTANCE_OF matches a struct of the operand type and rejects others") {
  ProgramBuilder b;
  b.poolString("Pair");
  b.structType(0, 1);
  b.number(3.0f);
  // A Pair value is an instance of type 0; a number is not.
  b.beginFunction().instr(Op::STRUCT_NEW, 0, 0).instr(Op::INSTANCE_OF, 0).instr(Op::RET);
  b.beginFunction().instr(Op::PUSH_CONST_NUM, 0).instr(Op::INSTANCE_OF, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  HeapHarness h;
  Machine matchMachine;
  REQUIRE(startExecution(matchMachine.state, image, 0, {}).isOk());
  matchMachine.state.budget = 100;
  const RunResult matched = runExecution(matchMachine.state, image, h.surface);
  REQUIRE(matched.status == RunStatus::Done);
  CHECK(matched.result.asBoolean());

  HeapHarness h2;
  Machine missMachine;
  REQUIRE(startExecution(missMachine.state, image, 1, {}).isOk());
  missMachine.state.budget = 100;
  const RunResult missed = runExecution(missMachine.state, image, h2.surface);
  REQUIRE(missed.status == RunStatus::Done);
  CHECK_FALSE(missed.result.asBoolean());
}

TEST_CASE("INSTANCE_OF with an out-of-range type index faults ScriptError") {
  ProgramBuilder b;
  b.valueNil();
  b.beginFunction().instr(Op::PUSH_CONST_VAL, 0).instr(Op::INSTANCE_OF, 5).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::ScriptError);
}

TEST_CASE("CALL invokes a function and RET resumes the caller") {
  ProgramBuilder b;
  b.number(21.0f);
  // func 0: CALL func 1 with one arg (21); func 1 returns its param.
  b.beginFunction(0, 0).instr(Op::PUSH_CONST_NUM, 0).instr(Op::CALL, 1, 1).instr(Op::RET);
  b.beginFunction(1, 1).instr(Op::LOAD_LOCAL, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 21.0f);
}

TEST_CASE("CALL with an argc that mismatches numParams faults ScriptError") {
  ProgramBuilder b;
  b.number(1.0f);
  b.beginFunction(0, 0).instr(Op::PUSH_CONST_NUM, 0).instr(Op::CALL, 1, 1).instr(Op::RET);
  b.beginFunction(2, 2).instr(Op::LOAD_LOCAL, 0).instr(Op::RET); // wants two params
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::ScriptError);
  CHECK(result.site.funcId == 0);
  CHECK(result.site.pc == 1); // the CALL site, recorded before any pc advance
}

TEST_CASE("a closure captures a value and CALL_INDIRECT loads it") {
  ProgramBuilder b;
  b.number(7.0f);
  // func 0: capture 7 into a closure over func 1, then call it with no args.
  b.beginFunction(0, 0)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::MAKE_CLOSURE, 1, 1)
      .instr(Op::CALL_INDIRECT, 0)
      .instr(Op::RET);
  b.beginFunction(0, 0).instr(Op::LOAD_CAPTURE, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  HeapHarness h;
  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, h.surface);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 7.0f);
}

TEST_CASE("CALL_INDIRECT_ARGS truncates surplus args and nil-pads missing ones") {
  ProgramBuilder b;
  b.number(3.0f).number(4.0f);
  // func 0: closure over func 1 (two params, returns the second); call it with
  // one arg, so the second param is nil-padded and returned.
  b.beginFunction(0, 0)
      .instr(Op::MAKE_CLOSURE, 1, 0)
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::CALL_INDIRECT_ARGS, 1)
      .instr(Op::RET);
  b.beginFunction(2, 2).instr(Op::LOAD_LOCAL, 1).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  HeapHarness h;
  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, h.surface);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.isNil());
}

TEST_CASE("CALL_INDIRECT on a non-function value faults ScriptError") {
  ProgramBuilder b;
  b.number(5.0f);
  b.beginFunction(0, 0).instr(Op::PUSH_CONST_NUM, 0).instr(Op::CALL_INDIRECT, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  HeapHarness h;
  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, h.surface);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::ScriptError);
}

TEST_CASE("LOAD_CAPTURE in a frame with no captures faults ScriptError") {
  ProgramBuilder b;
  b.beginFunction(0, 0).instr(Op::LOAD_CAPTURE, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  HeapHarness h;
  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, h.surface);
  REQUIRE(result.status == RunStatus::Fault);
  CHECK(result.error == ErrorCode::ScriptError);
}

// ---- HOST_CALL opcode dispatch (the core host-function library) ----

TEST_CASE("HOST_CALL dispatches a core numeric operator body") {
  ProgramBuilder b;
  b.number(2.0f).number(3.0f);
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::PUSH_CONST_NUM, 1)
      .instr(Op::HOST_CALL, static_cast<int32_t>(wendoo::CoreFuncId::OpAddNumber), 2, 0)
      .instr(Op::RET);
  std::vector<uint8_t> storage(8 * 1024);
  const ProgramImage image = b.build(storage);

  Machine machine;
  const RunResult result = runProgram(machine, image);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == 5.0f);
}

TEST_CASE("HOST_CALL produces a managed string from a core string body") {
  ProgramBuilder b;
  b.poolString("a").poolString("b");
  b.beginFunction()
      .instr(Op::PUSH_CONST_STR, 0)
      .instr(Op::PUSH_CONST_STR, 1)
      .instr(Op::HOST_CALL, static_cast<int32_t>(wendoo::CoreFuncId::OpAddString), 2, 0)
      .instr(Op::RET);
  std::vector<uint8_t> storage(8 * 1024);
  const ProgramImage image = b.build(storage);

  std::vector<uint8_t> heapStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(heapStorage.data(), heapStorage.size()));
  // The heap resolves the borrowed operands' content through the program.
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::RuntimeSurface surface{nullptr, {}, nullptr, &heap};

  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, surface);
  REQUIRE(result.status == RunStatus::Done);
  REQUIRE(result.result.isManagedString());
  const char* bytes = nullptr;
  uint32_t length = 0;
  REQUIRE(heap.stringContent(result.result, bytes, length));
  CHECK(std::string(bytes, length) == "ab");
}

TEST_CASE("HOST_CALL draws MathRandom from the VM-global RNG") {
  ProgramBuilder b;
  b.beginFunction()
      .instr(Op::HOST_CALL, static_cast<int32_t>(wendoo::CoreFuncId::MathRandom), 0, 0)
      .instr(Op::RET);
  std::vector<uint8_t> storage(8 * 1024);
  const ProgramImage image = b.build(storage);

  wendoo::VmRng rng;
  wendoo::RuntimeSurface surface;
  surface.rng = &rng;

  wendoo::VmRng expected;
  const float expectedDraw = expected.next();

  Machine machine;
  const RunResult result = runProgram(machine, image, {}, 1000, surface);
  REQUIRE(result.status == RunStatus::Done);
  CHECK(result.result.asNumber() == expectedDraw);
}

TEST_CASE("HOST_CALL host-call failures are ScriptError; absent capabilities are HostError") {
  SUBCASE("a core funcId with no host-call body faults ScriptError (the host-failure code)") {
    // Context/sensor/actuator ids are not core host-call bodies; dispatching one
    // through HOST_CALL hits the unserviceable-id path.
    ProgramBuilder b;
    b.beginFunction()
        .instr(Op::HOST_CALL, static_cast<int32_t>(wendoo::CoreFuncId::SensorCurrentPage), 0, 0)
        .instr(Op::RET);
    std::vector<uint8_t> storage(8 * 1024);
    const ProgramImage image = b.build(storage);

    Machine machine;
    const RunResult result = runProgram(machine, image);
    REQUIRE(result.status == RunStatus::Fault);
    CHECK(result.error == ErrorCode::ScriptError);
  }

  SUBCASE("a target funcId has no registered body and faults ScriptError") {
    ProgramBuilder b;
    b.beginFunction()
        .instr(Op::HOST_CALL, static_cast<int32_t>(wendoo::TARGET_FUNC_ID_BASE), 0, 0)
        .instr(Op::RET);
    std::vector<uint8_t> storage(8 * 1024);
    const ProgramImage image = b.build(storage);

    Machine machine;
    const RunResult result = runProgram(machine, image);
    REQUIRE(result.status == RunStatus::Fault);
    CHECK(result.error == ErrorCode::ScriptError);
  }

  SUBCASE("a body whose capability is absent faults HostError") {
    // MathRandom needs the rng; a surface without one cannot service it.
    ProgramBuilder b;
    b.beginFunction()
        .instr(Op::HOST_CALL, static_cast<int32_t>(wendoo::CoreFuncId::MathRandom), 0, 0)
        .instr(Op::RET);
    std::vector<uint8_t> storage(8 * 1024);
    const ProgramImage image = b.build(storage);

    Machine machine;
    const RunResult result = runProgram(machine, image); // default surface: no rng
    REQUIRE(result.status == RunStatus::Fault);
    CHECK(result.error == ErrorCode::HostError);
  }
}
