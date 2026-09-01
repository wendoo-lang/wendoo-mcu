#pragma once

#include <cstdint>

namespace wendoo {

/**
 * Brain VM bytecode opcodes. Mirrors the Op enum in
 * external/wendoo-lang/packages/core/src/runtime/bytecode.ts. Serialized
 * programs record these values as the instruction opcode byte, so they are
 * wire-stable: never renumber or reuse a value. `RESERVED_111` and
 * `RESERVED_112` are reserved opcode numbers with no VM handler, kept so the
 * remaining struct opcodes keep their numbers.
 */
enum class Op : uint8_t {
  // Stack manipulation
  PUSH_CONST_VAL = 0,
  POP = 1,
  DUP = 2,
  SWAP = 3,
  PUSH_CONST_NUM = 4,
  PUSH_CONST_STR = 5,
  STACK_SET_REL = 6,

  // Variables (stored in the brain by slot index from the program's variableNames pool)
  LOAD_VAR_SLOT = 10,
  STORE_VAR_SLOT = 11,

  // System variables: a brain-global, internal store separate from the
  // variableNames pool. One value slot per registered System; shared across all
  // callsites and not reachable from brain code. STORE_SYSTEM_VAR writes by
  // reference (no deep copy), so a System's state struct mutates in place.
  LOAD_SYSTEM_VAR = 12,
  STORE_SYSTEM_VAR = 13,

  // Control flow
  JMP = 20,
  JMP_IF_FALSE = 21,
  JMP_IF_TRUE = 22,

  // Function calls
  CALL = 30,
  RET = 31,
  SPAWN_RULE = 32,

  // Host calls
  HOST_CALL = 40,
  HOST_CALL_ASYNC = 41,

  // Action calls
  ACTION_CALL = 42,
  ACTION_CALL_ASYNC = 43,

  // Host action calls by stable registry id
  HOST_ACTION_CALL = 44,
  HOST_ACTION_CALL_ASYNC = 45,

  // Async operations and cooperative scheduling
  AWAIT = 50,
  YIELD = 51,

  // Exception handling
  TRY = 60,
  END_TRY = 61,
  THROW = 62,

  // Boundaries
  WHEN_START = 70,
  WHEN_END = 71,
  DO_START = 72,
  DO_END = 73,
  WHEN_END_PRESENT = 74,

  // List operations
  LIST_NEW = 90,
  LIST_PUSH = 91,
  LIST_GET = 92,
  LIST_SET = 93,
  LIST_LEN = 94,
  LIST_POP = 95,
  LIST_SHIFT = 96,
  LIST_REMOVE = 97,
  LIST_INSERT = 98,
  LIST_SWAP = 99,

  // Map operations
  MAP_NEW = 100,
  MAP_SET = 101,
  MAP_GET = 102,
  MAP_HAS = 103,
  MAP_DELETE = 104,

  // Struct operations
  STRUCT_NEW = 110,
  RESERVED_111 = 111,
  RESERVED_112 = 112,
  STRUCT_COPY_EXCEPT = 113,
  STRUCT_GET_FIELD = 114,
  STRUCT_SET_FIELD = 115,
  STRUCT_DEEP_COPY = 116,

  // Generic field access
  GET_FIELD = 120,
  SET_FIELD = 121,

  // Frame-local variables (indexed slots on the current call frame)
  LOAD_LOCAL = 130,
  STORE_LOCAL = 131,

  // Action instance state slots
  LOAD_CALLSITE_VAR = 140,
  STORE_CALLSITE_VAR = 141,

  // Type introspection
  TYPE_CHECK = 150,
  INSTANCE_OF = 151,

  // Indirect function calls
  CALL_INDIRECT = 160,
  CALL_INDIRECT_ARGS = 161,

  // Closure operations
  MAKE_CLOSURE = 170,
  LOAD_CAPTURE = 171,

  // Chain-gate WHEN boundaries: the truthiness and presence gates of a rule
  // whose trigger mode chains onto the rule above it at its own nesting level.
  WHEN_END_CHAIN = 172,
  WHEN_END_PRESENT_CHAIN = 173,
};

/** Number of declared {@link Op} members, including the reserved opcodes. */
inline constexpr uint32_t kOpCount = 69;

/**
 * Var-int encoding of a single instruction operand: `UVar` is an unsigned
 * ULEB128 var-int; `SVar` is a zigzag + ULEB128 signed var-int.
 */
enum class OperandEncoding : uint8_t {
  UVar = 0,
  SVar = 1,
};

/** One operand slot in an opcode's {@link kOperandSchema} row. */
struct OperandSpec {
  /** Var-int encoding used for this operand. */
  OperandEncoding encoding;
  /**
   * When true, the operand may be absent. Allowed only on the trailing slot.
   * Absence is sentinel-biased on the wire (`0` = absent, `value + 1` =
   * present), so an optional operand always occupies exactly one var-uint.
   */
  bool optional;
};

/** One {@link kOperandSchema} row: an opcode and its operand slots. */
struct OpOperandSchema {
  Op op;
  /** Number of entries used in {@link operands}. */
  uint8_t operandCount;
  /** Operand slots, a contiguous `a, b, c` prefix in order. */
  OperandSpec operands[3];
};

namespace detail {
inline constexpr OperandSpec kUVar{OperandEncoding::UVar, false};
inline constexpr OperandSpec kSVar{OperandEncoding::SVar, false};
inline constexpr OperandSpec kUVarOpt{OperandEncoding::UVar, true};
} // namespace detail

/**
 * Per-opcode operand layout for binary instruction serialization. Mirrors
 * OPERAND_SCHEMA in
 * external/wendoo-lang/packages/core/src/runtime/bytecode.ts, one row per
 * declared {@link Op} member in declaration order. `SVar` marks the eight
 * signed rel-offset opcodes (`JMP`, `JMP_IF_FALSE`, `JMP_IF_TRUE`,
 * `WHEN_END`, `WHEN_END_PRESENT`, `WHEN_END_CHAIN`, `WHEN_END_PRESENT_CHAIN`,
 * `TRY`); the optional trailing `b` marks the four
 * typeId-carrying constructors (`LIST_NEW`, `MAP_NEW`, `STRUCT_NEW`,
 * `STRUCT_COPY_EXCEPT`); every other operand is `UVar`. `STRUCT_NEW`'s `a`
 * operand is reserved and must be 0.
 */
inline constexpr OpOperandSchema kOperandSchema[] = {
    {Op::PUSH_CONST_VAL, 1, {detail::kUVar}},
    {Op::POP, 0, {}},
    {Op::DUP, 0, {}},
    {Op::SWAP, 0, {}},
    {Op::PUSH_CONST_NUM, 1, {detail::kUVar}},
    {Op::PUSH_CONST_STR, 1, {detail::kUVar}},
    {Op::STACK_SET_REL, 1, {detail::kUVar}},
    {Op::LOAD_VAR_SLOT, 1, {detail::kUVar}},
    {Op::STORE_VAR_SLOT, 1, {detail::kUVar}},
    {Op::LOAD_SYSTEM_VAR, 1, {detail::kUVar}},
    {Op::STORE_SYSTEM_VAR, 1, {detail::kUVar}},
    {Op::JMP, 1, {detail::kSVar}},
    {Op::JMP_IF_FALSE, 1, {detail::kSVar}},
    {Op::JMP_IF_TRUE, 1, {detail::kSVar}},
    {Op::CALL, 2, {detail::kUVar, detail::kUVar}},
    {Op::RET, 0, {}},
    {Op::SPAWN_RULE, 1, {detail::kUVar}},
    {Op::HOST_CALL, 3, {detail::kUVar, detail::kUVar, detail::kUVar}},
    {Op::HOST_CALL_ASYNC, 3, {detail::kUVar, detail::kUVar, detail::kUVar}},
    {Op::ACTION_CALL, 3, {detail::kUVar, detail::kUVar, detail::kUVar}},
    {Op::ACTION_CALL_ASYNC, 3, {detail::kUVar, detail::kUVar, detail::kUVar}},
    {Op::HOST_ACTION_CALL, 3, {detail::kUVar, detail::kUVar, detail::kUVar}},
    {Op::HOST_ACTION_CALL_ASYNC, 3, {detail::kUVar, detail::kUVar, detail::kUVar}},
    {Op::AWAIT, 0, {}},
    {Op::YIELD, 0, {}},
    {Op::TRY, 1, {detail::kSVar}},
    {Op::END_TRY, 0, {}},
    {Op::THROW, 0, {}},
    {Op::WHEN_START, 0, {}},
    {Op::WHEN_END, 1, {detail::kSVar}},
    {Op::DO_START, 0, {}},
    {Op::DO_END, 0, {}},
    {Op::WHEN_END_PRESENT, 1, {detail::kSVar}},
    {Op::LIST_NEW, 2, {detail::kUVar, detail::kUVarOpt}},
    {Op::LIST_PUSH, 0, {}},
    {Op::LIST_GET, 0, {}},
    {Op::LIST_SET, 0, {}},
    {Op::LIST_LEN, 0, {}},
    {Op::LIST_POP, 0, {}},
    {Op::LIST_SHIFT, 0, {}},
    {Op::LIST_REMOVE, 0, {}},
    {Op::LIST_INSERT, 0, {}},
    {Op::LIST_SWAP, 0, {}},
    {Op::MAP_NEW, 2, {detail::kUVar, detail::kUVarOpt}},
    {Op::MAP_SET, 0, {}},
    {Op::MAP_GET, 0, {}},
    {Op::MAP_HAS, 0, {}},
    {Op::MAP_DELETE, 0, {}},
    {Op::STRUCT_NEW, 2, {detail::kUVar, detail::kUVarOpt}},
    {Op::RESERVED_111, 0, {}},
    {Op::RESERVED_112, 0, {}},
    {Op::STRUCT_COPY_EXCEPT, 2, {detail::kUVar, detail::kUVarOpt}},
    {Op::STRUCT_GET_FIELD, 1, {detail::kUVar}},
    {Op::STRUCT_SET_FIELD, 1, {detail::kUVar}},
    {Op::STRUCT_DEEP_COPY, 0, {}},
    {Op::GET_FIELD, 0, {}},
    {Op::SET_FIELD, 0, {}},
    {Op::LOAD_LOCAL, 1, {detail::kUVar}},
    {Op::STORE_LOCAL, 1, {detail::kUVar}},
    {Op::LOAD_CALLSITE_VAR, 1, {detail::kUVar}},
    {Op::STORE_CALLSITE_VAR, 1, {detail::kUVar}},
    {Op::TYPE_CHECK, 1, {detail::kUVar}},
    {Op::INSTANCE_OF, 1, {detail::kUVar}},
    {Op::CALL_INDIRECT, 1, {detail::kUVar}},
    {Op::CALL_INDIRECT_ARGS, 1, {detail::kUVar}},
    {Op::MAKE_CLOSURE, 2, {detail::kUVar, detail::kUVar}},
    {Op::LOAD_CAPTURE, 1, {detail::kUVar}},
    {Op::WHEN_END_CHAIN, 1, {detail::kSVar}},
    {Op::WHEN_END_PRESENT_CHAIN, 1, {detail::kSVar}},
};

/**
 * Return the operand-schema row for `op`, or nullptr when the value is not a
 * declared opcode.
 */
const OpOperandSchema* operandSchemaFor(Op op);

} // namespace wendoo
