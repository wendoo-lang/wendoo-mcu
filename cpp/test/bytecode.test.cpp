#include "doctest/doctest.h"

#include "core/runtime/bytecode.h"

#include <cstdint>
#include <iterator>

using wendoo::kOpCount;
using wendoo::kOperandSchema;
using wendoo::Op;
using wendoo::OperandEncoding;
using wendoo::operandSchemaFor;
using wendoo::OpOperandSchema;

TEST_CASE("Op values are wire-stable at the group boundaries") {
  CHECK(static_cast<uint8_t>(Op::PUSH_CONST_VAL) == 0);
  CHECK(static_cast<uint8_t>(Op::STACK_SET_REL) == 6);
  CHECK(static_cast<uint8_t>(Op::LOAD_VAR_SLOT) == 10);
  CHECK(static_cast<uint8_t>(Op::JMP) == 20);
  CHECK(static_cast<uint8_t>(Op::CALL) == 30);
  CHECK(static_cast<uint8_t>(Op::SPAWN_RULE) == 32);
  CHECK(static_cast<uint8_t>(Op::HOST_CALL) == 40);
  CHECK(static_cast<uint8_t>(Op::ACTION_CALL) == 42);
  CHECK(static_cast<uint8_t>(Op::HOST_ACTION_CALL) == 44);
  CHECK(static_cast<uint8_t>(Op::AWAIT) == 50);
  CHECK(static_cast<uint8_t>(Op::TRY) == 60);
  CHECK(static_cast<uint8_t>(Op::WHEN_START) == 70);
  CHECK(static_cast<uint8_t>(Op::WHEN_END_PRESENT) == 74);
  CHECK(static_cast<uint8_t>(Op::LIST_NEW) == 90);
  CHECK(static_cast<uint8_t>(Op::LIST_SWAP) == 99);
  CHECK(static_cast<uint8_t>(Op::MAP_NEW) == 100);
  CHECK(static_cast<uint8_t>(Op::STRUCT_NEW) == 110);
  CHECK(static_cast<uint8_t>(Op::RESERVED_111) == 111);
  CHECK(static_cast<uint8_t>(Op::RESERVED_112) == 112);
  CHECK(static_cast<uint8_t>(Op::STRUCT_COPY_EXCEPT) == 113);
  CHECK(static_cast<uint8_t>(Op::STRUCT_DEEP_COPY) == 116);
  CHECK(static_cast<uint8_t>(Op::GET_FIELD) == 120);
  CHECK(static_cast<uint8_t>(Op::LOAD_LOCAL) == 130);
  CHECK(static_cast<uint8_t>(Op::LOAD_CALLSITE_VAR) == 140);
  CHECK(static_cast<uint8_t>(Op::TYPE_CHECK) == 150);
  CHECK(static_cast<uint8_t>(Op::CALL_INDIRECT) == 160);
  CHECK(static_cast<uint8_t>(Op::MAKE_CLOSURE) == 170);
  CHECK(static_cast<uint8_t>(Op::LOAD_CAPTURE) == 171);
  CHECK(static_cast<uint8_t>(Op::WHEN_END_CHAIN) == 172);
  CHECK(static_cast<uint8_t>(Op::WHEN_END_PRESENT_CHAIN) == 173);
}

TEST_CASE("schema covers every declared opcode in declaration order") {
  REQUIRE(std::size(kOperandSchema) == kOpCount);
  CHECK(kOperandSchema[0].op == Op::PUSH_CONST_VAL);
  CHECK(kOperandSchema[kOpCount - 1].op == Op::WHEN_END_PRESENT_CHAIN);
}

TEST_CASE("schema rows for known opcodes match the TS operand layout") {
  const OpOperandSchema* ret = operandSchemaFor(Op::RET);
  REQUIRE(ret != nullptr);
  CHECK(ret->operandCount == 0);

  const OpOperandSchema* pushNum = operandSchemaFor(Op::PUSH_CONST_NUM);
  REQUIRE(pushNum != nullptr);
  CHECK(pushNum->operandCount == 1);
  CHECK(pushNum->operands[0].encoding == OperandEncoding::UVar);
  CHECK_FALSE(pushNum->operands[0].optional);

  const OpOperandSchema* jmp = operandSchemaFor(Op::JMP);
  REQUIRE(jmp != nullptr);
  CHECK(jmp->operandCount == 1);
  CHECK(jmp->operands[0].encoding == OperandEncoding::SVar);

  const OpOperandSchema* call = operandSchemaFor(Op::CALL);
  REQUIRE(call != nullptr);
  CHECK(call->operandCount == 2);
  CHECK(call->operands[0].encoding == OperandEncoding::UVar);
  CHECK(call->operands[1].encoding == OperandEncoding::UVar);
  CHECK_FALSE(call->operands[1].optional);

  const OpOperandSchema* spawnRule = operandSchemaFor(Op::SPAWN_RULE);
  REQUIRE(spawnRule != nullptr);
  CHECK(spawnRule->operandCount == 1);
  CHECK(spawnRule->operands[0].encoding == OperandEncoding::UVar);
  CHECK_FALSE(spawnRule->operands[0].optional);

  const OpOperandSchema* hostCall = operandSchemaFor(Op::HOST_CALL);
  REQUIRE(hostCall != nullptr);
  CHECK(hostCall->operandCount == 3);
  for (uint8_t i = 0; i < 3; i++) {
    CHECK(hostCall->operands[i].encoding == OperandEncoding::UVar);
    CHECK_FALSE(hostCall->operands[i].optional);
  }

  const OpOperandSchema* structNew = operandSchemaFor(Op::STRUCT_NEW);
  REQUIRE(structNew != nullptr);
  CHECK(structNew->operandCount == 2);
  CHECK(structNew->operands[0].encoding == OperandEncoding::UVar);
  CHECK_FALSE(structNew->operands[0].optional);
  CHECK(structNew->operands[1].encoding == OperandEncoding::UVar);
  CHECK(structNew->operands[1].optional);

  const OpOperandSchema* reserved = operandSchemaFor(Op::RESERVED_111);
  REQUIRE(reserved != nullptr);
  CHECK(reserved->operandCount == 0);
}

TEST_CASE("exactly the eight rel-offset opcodes use the signed encoding") {
  uint32_t svarCount = 0;
  for (const OpOperandSchema& row : kOperandSchema) {
    for (uint8_t i = 0; i < row.operandCount; i++) {
      if (row.operands[i].encoding == OperandEncoding::SVar) {
        svarCount++;
        const bool isRelOffsetOp = row.op == Op::JMP || row.op == Op::JMP_IF_FALSE ||
                                   row.op == Op::JMP_IF_TRUE || row.op == Op::WHEN_END ||
                                   row.op == Op::WHEN_END_PRESENT || row.op == Op::WHEN_END_CHAIN ||
                                   row.op == Op::WHEN_END_PRESENT_CHAIN || row.op == Op::TRY;
        CHECK(isRelOffsetOp);
      }
    }
  }
  CHECK(svarCount == 8);
}

TEST_CASE("exactly the four typed constructors carry an optional trailing operand") {
  uint32_t optionalCount = 0;
  for (const OpOperandSchema& row : kOperandSchema) {
    for (uint8_t i = 0; i < row.operandCount; i++) {
      if (row.operands[i].optional) {
        optionalCount++;
        // Optional operands are allowed only on the trailing slot.
        CHECK(i == row.operandCount - 1);
        const bool isTypedConstructor = row.op == Op::LIST_NEW || row.op == Op::MAP_NEW ||
                                        row.op == Op::STRUCT_NEW ||
                                        row.op == Op::STRUCT_COPY_EXCEPT;
        CHECK(isTypedConstructor);
      }
    }
  }
  CHECK(optionalCount == 4);
}

TEST_CASE("operandSchemaFor returns nullptr for undeclared opcode values") {
  CHECK(operandSchemaFor(static_cast<Op>(7)) == nullptr);
  CHECK(operandSchemaFor(static_cast<Op>(117)) == nullptr);
  CHECK(operandSchemaFor(static_cast<Op>(255)) == nullptr);
}
