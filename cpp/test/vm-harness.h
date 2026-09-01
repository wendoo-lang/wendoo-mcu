#pragma once

#include "doctest/doctest.h"

#include "core/runtime/core-type-atom-id.h"
#include "core/runtime/execution-state.h"
#include "core/runtime/program.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"
#include "device-profile-caps.h"
#include "wire-builder.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

/**
 * Composes a synthetic program section by section and decodes it into a
 * {@link wendoo::ProgramImage} for dispatch-loop and scheduler tests.
 */
class ProgramBuilder {
public:
  /** Appends a constant-pool string and returns the builder. */
  ProgramBuilder& poolString(const char* value) {
    strings_.push_back(value);
    return *this;
  }

  /** Appends an atom type-table entry. */
  ProgramBuilder& atomType(wendoo::CoreTypeAtomId atomId) {
    typeCount_++;
    types_.u8(0).varUint(static_cast<uint32_t>(atomId));
    return *this;
  }

  /** Appends a list type-table entry over element type `elemTypeIdx`. */
  ProgramBuilder& listType(uint32_t elemTypeIdx) {
    typeCount_++;
    types_.u8(1).varUint(elemTypeIdx);
    return *this;
  }

  /**
   * Appends a program-local struct type-table entry: its name (string-table
   * entry `nameStringIdx`), `slotCount` (`maxFieldId + 1`) field slots, and an
   * optional field name->id map (`{nameStringIdx, fieldId}` pairs) used by the
   * dynamic computed-key opcodes.
   */
  ProgramBuilder& structType(uint32_t nameStringIdx, uint32_t slotCount,
                             std::initializer_list<std::pair<uint32_t, uint32_t>> fields = {}) {
    typeCount_++;
    types_.u8(6)
        .varUint(nameStringIdx)
        .varUint(slotCount)
        .varUint(static_cast<uint32_t>(fields.size()));
    for (const std::pair<uint32_t, uint32_t>& field : fields) {
      types_.varUint(field.first).varUint(field.second);
    }
    return *this;
  }

  /** Appends a number constant-pool entry. */
  ProgramBuilder& number(float value) {
    numberCount_++;
    numbers_.u8(1).f32(value);
    return *this;
  }

  /** Appends a nil value constant. */
  ProgramBuilder& valueNil() {
    valueCount_++;
    values_.u8(1);
    return *this;
  }

  /** Appends a void value constant. */
  ProgramBuilder& valueVoid() {
    valueCount_++;
    values_.u8(0);
    return *this;
  }

  /** Appends an unknown value constant. */
  ProgramBuilder& valueUnknown() {
    valueCount_++;
    values_.u8(0xff);
    return *this;
  }

  /**
   * Appends a program-local enum type-table entry: its name (string-table entry
   * `nameStringIdx`) and its string-valued symbols in declared order, each a
   * `{symbolStringIdx, valueStringIdx}` pair. An enum value's ordinal indexes
   * this list.
   */
  ProgramBuilder& enumType(uint32_t nameStringIdx,
                           std::initializer_list<std::pair<uint32_t, uint32_t>> symbols) {
    typeCount_++;
    types_.u8(7).varUint(nameStringIdx).varUint(static_cast<uint32_t>(symbols.size()));
    if (symbols.size() > 0) {
      types_.u8(1); // string-valued symbols
      for (const std::pair<uint32_t, uint32_t>& symbol : symbols) {
        types_.varUint(symbol.first).varUint(symbol.second);
      }
    }
    return *this;
  }

  /** Appends a boolean value constant. */
  ProgramBuilder& valueBool(bool value) {
    valueCount_++;
    values_.u8(2).u8(value ? 1 : 0);
    return *this;
  }

  /** Appends a number value constant. */
  ProgramBuilder& valueNumber(float value) {
    valueCount_++;
    values_.u8(3).u8(1).f32(value);
    return *this;
  }

  /** Appends a string value constant referencing string-table entry `stringIdx`. */
  ProgramBuilder& valueString(uint32_t stringIdx) {
    valueCount_++;
    values_.u8(4).varUint(stringIdx);
    return *this;
  }

  /** Appends an enum value constant. */
  ProgramBuilder& valueEnum(uint32_t typeIdx, uint32_t ordinal) {
    valueCount_++;
    values_.u8(5).varUint(typeIdx).varUint(ordinal);
    return *this;
  }

  /** Appends a captureless function value constant. */
  ProgramBuilder& valueFunction(uint32_t funcId) {
    valueCount_++;
    values_.u8(11).varUint(funcId).varUint(0);
    return *this;
  }

  /** Appends a borrowed buffer value constant carrying `bytes` inline. */
  ProgramBuilder& valueBuffer(std::initializer_list<uint8_t> bytes) {
    valueCount_++;
    values_.u8(12).varUint(static_cast<uint32_t>(bytes.size()));
    for (const uint8_t byte : bytes) {
      values_.u8(byte);
    }
    return *this;
  }

  /** Appends an empty list value constant of list type `typeIdx`. */
  ProgramBuilder& valueEmptyList(uint32_t typeIdx) {
    valueCount_++;
    values_.u8(6).varUint(typeIdx).varUint(0);
    return *this;
  }

  /** Opens a new function; subsequent {@link instr} calls fill its body. */
  ProgramBuilder& beginFunction(uint32_t numParams = 0, uint32_t extraLocals = 0) {
    functions_.push_back(FunctionSpec{numParams, extraLocals, 0, WireBuilder()});
    return *this;
  }

  /**
   * Appends one instruction to the open function, encoding the operands the
   * opcode's schema requires. An optional trailing operand is encoded present
   * (sentinel-biased) for any value other than {@link wendoo::kNoTypeIdx},
   * which encodes it absent.
   */
  ProgramBuilder& instr(wendoo::Op op, int32_t a = 0, int32_t b = 0, int32_t c = 0) {
    const wendoo::OpOperandSchema* schema = wendoo::operandSchemaFor(op);
    REQUIRE(schema != nullptr);
    FunctionSpec& fn = functions_.back();
    fn.body.u8(static_cast<uint8_t>(op));
    const int32_t operands[3] = {a, b, c};
    for (uint8_t i = 0; i < schema->operandCount; i++) {
      const wendoo::OperandSpec& spec = schema->operands[i];
      if (spec.optional) {
        const uint32_t value = static_cast<uint32_t>(operands[i]);
        fn.body.varUint(value == wendoo::kNoTypeIdx ? 0 : value + 1);
      } else if (spec.encoding == wendoo::OperandEncoding::SVar) {
        fn.body.varInt(operands[i]);
      } else {
        fn.body.varUint(static_cast<uint32_t>(operands[i]));
      }
    }
    fn.instrCount++;
    return *this;
  }

  /**
   * Appends a brain variable slot named by string-table entry `nameStringIdx`.
   * `initValueIdx` is the slot's starting value as a value-pool index, or
   * {@link wendoo::kNoVariableInit} for a slot that starts holding no value.
   */
  ProgramBuilder& brainVariable(uint32_t nameStringIdx,
                                uint32_t initValueIdx = wendoo::kNoVariableInit) {
    variables_.push_back({nameStringIdx, initValueIdx});
    return *this;
  }

  /**
   * Appends a bytecode action whose body is `entryFuncId`, taking the next
   * action slot. Actions are emitted in append order, so the first call takes
   * slot 0 -- the operand `ACTION_CALL` / `ACTION_CALL_ASYNC` carries.
   */
  ProgramBuilder& bytecodeAction(uint32_t entryFuncId) {
    actions_.push_back(entryFuncId);
    return *this;
  }

  /** Marks `funcId` as a rule entry in the program's rule-funcId set. */
  ProgramBuilder& ruleFunc(uint32_t funcId) {
    ruleFuncs_.push_back(funcId);
    return *this;
  }

  /**
   * Records `childFuncId` as a child rule of `parentFuncId` in the program's
   * rule-ancestor table. A rule with no entry is a root rule.
   */
  ProgramBuilder& ruleAncestor(uint32_t childFuncId, uint32_t parentFuncId) {
    ruleAncestors_.push_back({childFuncId, parentFuncId});
    return *this;
  }

  /** Opens a new page identified by string-table entry `pageIdStringIdx`. */
  ProgramBuilder& beginPage(uint32_t pageIdStringIdx) {
    pages_.push_back(PageSpec{pageIdStringIdx, {}, {}});
    return *this;
  }

  /** Appends a root-rule funcId to the open page. */
  ProgramBuilder& pageRoot(uint32_t funcId) {
    pages_.back().roots.push_back(funcId);
    return *this;
  }

  /** Appends a host-bound action call site to the open page. */
  ProgramBuilder& pageHostCallSite(uint32_t callSiteId, uint32_t actionId) {
    pages_.back().callSites.push_back(CallSiteSpec{callSiteId, actionId});
    return *this;
  }

  /**
   * Encodes every section and decodes the program into `storage`. The
   * builder keeps the encoded wire alive: the image borrows its string
   * bytes, so the builder must outlive the image.
   */
  wendoo::ProgramImage build(std::vector<uint8_t>& storage) {
    // Presence bits: ACTS is bit 0, RULF is bit 1, RANC is bit 2.
    const uint8_t presence =
        static_cast<uint8_t>((actions_.empty() ? 0 : 1) | (ruleFuncs_.empty() ? 0 : 2) |
                             (ruleAncestors_.empty() ? 0 : 4));
    WireBuilder& w = wire_;
    w = programHeader(presence);
    w.varUint(static_cast<uint32_t>(strings_.size()))
        .varUint(static_cast<uint32_t>(strings_.size()));
    for (const char* s : strings_) {
      w.str(s);
    }
    w.varUint(typeCount_).raw(types_.span());
    w.varUint(numberCount_).raw(numbers_.span());
    w.varUint(valueCount_).raw(values_.span());
    w.varUint(static_cast<uint32_t>(functions_.size()));
    for (const FunctionSpec& fn : functions_) {
      w.varUint(fn.numParams).varUint(fn.extraLocals).varUint(0).varUint(fn.instrCount);
      w.raw(fn.body.span());
    }
    w.varUint(static_cast<uint32_t>(variables_.size()));
    for (const VariableSlot& slot : variables_) {
      varsSlot(w, slot.nameStringIdx,
               slot.initValueIdx == wendoo::kNoVariableInit ? 0 : slot.initValueIdx + 1);
    }
    if (!actions_.empty()) {
      w.varUint(static_cast<uint32_t>(actions_.size()));
      for (const uint32_t entryFuncId : actions_) {
        w.u8(0).varUint(entryFuncId); // no lifecycle hooks
      }
    }
    if (!ruleFuncs_.empty()) {
      w.varUint(static_cast<uint32_t>(ruleFuncs_.size()));
      for (const uint32_t funcId : ruleFuncs_) {
        w.varUint(funcId);
      }
    }
    if (!ruleAncestors_.empty()) {
      w.varUint(static_cast<uint32_t>(ruleAncestors_.size()));
      for (const RuleAncestorSpec& edge : ruleAncestors_) {
        w.varUint(edge.childFuncId).varUint(edge.parentFuncId);
      }
    }
    w.varUint(static_cast<uint32_t>(pages_.size()));
    for (size_t i = 0; i < pages_.size(); i++) {
      const PageSpec& page = pages_[i];
      w.varUint(static_cast<uint32_t>(i)).varUint(page.pageIdStringIdx);
      w.varUint(static_cast<uint32_t>(page.roots.size()));
      for (const uint32_t funcId : page.roots) {
        w.varUint(funcId);
      }
      w.varUint(static_cast<uint32_t>(page.callSites.size()));
      for (const CallSiteSpec& site : page.callSites) {
        w.u8(0).varUint(site.callSiteId).varUint(site.actionId); // host binding tag
      }
    }
    return decodeOk(w, storage);
  }

private:
  struct FunctionSpec {
    uint32_t numParams;
    uint32_t extraLocals;
    uint32_t instrCount;
    WireBuilder body;
  };

  struct CallSiteSpec {
    uint32_t callSiteId;
    uint32_t actionId;
  };

  /** One rule-ancestor edge: a child rule's funcId and its parent rule's funcId. */
  struct RuleAncestorSpec {
    uint32_t childFuncId;
    uint32_t parentFuncId;
  };

  struct PageSpec {
    uint32_t pageIdStringIdx;
    std::vector<uint32_t> roots;
    std::vector<CallSiteSpec> callSites;
  };

  std::vector<const char*> strings_;
  WireBuilder wire_;
  uint32_t typeCount_ = 0;
  WireBuilder types_;
  uint32_t numberCount_ = 0;
  WireBuilder numbers_;
  uint32_t valueCount_ = 0;
  WireBuilder values_;
  /** One brain variable slot: its name's string index and its starting value's pool index. */
  struct VariableSlot {
    uint32_t nameStringIdx;
    uint32_t initValueIdx;
  };

  std::vector<FunctionSpec> functions_;
  std::vector<VariableSlot> variables_;
  std::vector<uint32_t> actions_;
  std::vector<uint32_t> ruleFuncs_;
  std::vector<RuleAncestorSpec> ruleAncestors_;
  std::vector<PageSpec> pages_;
};

/**
 * An execution state bound to its own fixed regions, pre-allocated to the full
 * caps with a null grow allocator: the cap arguments act as the overflow guards
 * and the regions never grow.
 */
struct Machine {
  explicit Machine(uint32_t stackLimit = wendoo::test::kDeviceProfileCaps.maxStackSize,
                   uint32_t localsLimit = wendoo::test::kDeviceProfileCaps.maxLocalsSize,
                   uint32_t frameLimit = wendoo::test::kDeviceProfileCaps.maxFrameDepth,
                   uint32_t handlerLimit = wendoo::test::kDeviceProfileCaps.maxHandlers) {
    state.stack = stackStorage.data();
    state.stackLimit = stackLimit;
    state.stackCapacity = wendoo::test::kDeviceProfileCaps.maxStackSize;
    state.stackDepth = 0;
    state.locals = localsStorage.data();
    state.localsLimit = localsLimit;
    state.localsCapacity = wendoo::test::kDeviceProfileCaps.maxLocalsSize;
    state.localsDepth = 0;
    state.frames = frameStorage.data();
    state.frameLimit = frameLimit;
    state.frameCapacity = wendoo::test::kDeviceProfileCaps.maxFrameDepth;
    state.frameDepth = 0;
    state.handlers = handlerStorage.data();
    state.handlerLimit = handlerLimit;
    state.handlerCapacity = wendoo::test::kDeviceProfileCaps.maxHandlers;
    state.handlerDepth = 0;
    state.allocator = nullptr;
    state.budget = 0;
  }

  std::array<wendoo::Value, wendoo::test::kDeviceProfileCaps.maxStackSize> stackStorage;
  std::array<wendoo::Value, wendoo::test::kDeviceProfileCaps.maxLocalsSize> localsStorage;
  std::array<wendoo::Frame, wendoo::test::kDeviceProfileCaps.maxFrameDepth> frameStorage;
  std::array<wendoo::Handler, wendoo::test::kDeviceProfileCaps.maxHandlers> handlerStorage;
  wendoo::ExecutionState state;
};

/** Starts function 0 with `args` and runs it under `budget` against `surface`. */
inline wendoo::RunResult runProgram(Machine& machine, const wendoo::ProgramImage& image,
                                    wendoo::Span<const wendoo::Value> args = {},
                                    int32_t budget = 1000,
                                    const wendoo::RuntimeSurface& surface = {}) {
  REQUIRE(wendoo::startExecution(machine.state, image, 0, args).isOk());
  machine.state.budget = budget;
  return wendoo::runExecution(machine.state, image, surface);
}
