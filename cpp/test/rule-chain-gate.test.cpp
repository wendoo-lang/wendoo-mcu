/**
 * The chain-gate WHEN boundaries, `WHEN_END_CHAIN` and `WHEN_END_PRESENT_CHAIN`:
 * the WHEN value each fires on, the `__whenResult` it captures, the DO-section
 * skip it takes, and the firing record it writes. Mirrors
 * `rule-chain-gate.spec.ts` in external/wendoo-lang/packages/core/src/runtime.
 */

#include "doctest/doctest.h"

#include "core/runtime/bytecode.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/program.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"
#include "core/runtime/when-result.h"
#include "vm-harness.h"

#include <array>
#include <cstdint>
#include <vector>

using wendoo::ExecutionContext;
using wendoo::GcMarker;
using wendoo::GcRoots;
using wendoo::ManagedHeap;
using wendoo::Op;
using wendoo::ProgramImage;
using wendoo::RegionArena;
using wendoo::RuleFiringState;
using wendoo::runExecution;
using wendoo::RunResult;
using wendoo::RunStatus;
using wendoo::RuntimeSurface;
using wendoo::Span;
using wendoo::startExecution;
using wendoo::Value;

namespace {

/** The rule directly above the gated rule at its own level: its subject. */
constexpr uint32_t kSubjectFuncId = 0;

/** The rule whose gate every fixture below exercises. */
constexpr uint32_t kGatedFuncId = 1;

/** Records for both fixture rules, plus headroom. */
constexpr uint32_t kRuleRecordCount = 4;

/** Value-pool index of the value the gated rule's WHEN section pushes. */
constexpr int32_t kWhenConst = 0;

/** Value-pool indices of the markers the fired and skipped paths return. */
constexpr int32_t kFiredConst = 1;
constexpr int32_t kSkippedConst = 2;

/** String-table index of the empty string every fixture pools. */
constexpr uint32_t kEmptyStringIdx = 1;

/** The WHEN values both gate pairs are compared over. */
enum class WhenValue : uint8_t { True, False, Nil, Zero, Seven, EmptyString };

/** Appends `value` to `b`'s value pool at the next index. */
void appendWhenValue(ProgramBuilder& b, WhenValue value) {
  switch (value) {
  case WhenValue::True:
    b.valueBool(true);
    return;
  case WhenValue::False:
    b.valueBool(false);
    return;
  case WhenValue::Nil:
    b.valueNil();
    return;
  case WhenValue::Zero:
    b.valueNumber(0.0f);
    return;
  case WhenValue::Seven:
    b.valueNumber(7.0f);
    return;
  case WhenValue::EmptyString:
    b.valueString(kEmptyStringIdx);
    return;
  }
}

/**
 * A two-rule, one-page program: the subject at {@link kSubjectFuncId} (never
 * run; it exists so the gated rule has a preceding sibling) and the gated rule
 * at {@link kGatedFuncId}, whose WHEN section pushes the value under test and
 * whose two exits name the path taken -- the fired marker after the DO section,
 * the skipped marker at the gate's jump target.
 *
 * @param withSubject - When false, the gated rule is the page's only root rule
 *   and therefore has no subject.
 */
ProgramImage buildGateProgram(ProgramBuilder& b, Op gate, WhenValue whenValue, bool withSubject,
                              std::vector<uint8_t>& storage) {
  b.poolString("page-id").poolString("");
  appendWhenValue(b, whenValue);
  b.valueBool(true).valueBool(false);

  b.beginFunction().instr(Op::PUSH_CONST_VAL, kSkippedConst).instr(Op::RET);
  b.beginFunction()
      .instr(Op::WHEN_START)                    // 0
      .instr(Op::PUSH_CONST_VAL, kWhenConst)    // 1
      .instr(gate, 4)                           // 2: not fired -> pc 6
      .instr(Op::DO_START)                      // 3
      .instr(Op::DO_END)                        // 4
      .instr(Op::JMP, 3)                        // 5: fired -> pc 8
      .instr(Op::PUSH_CONST_VAL, kSkippedConst) // 6
      .instr(Op::RET)                           // 7
      .instr(Op::PUSH_CONST_VAL, kFiredConst)   // 8
      .instr(Op::RET);                          // 9

  b.ruleFunc(kSubjectFuncId).ruleFunc(kGatedFuncId);
  b.beginPage(0);
  if (withSubject) {
    b.pageRoot(kSubjectFuncId);
  }
  b.pageRoot(kGatedFuncId);
  return b.build(storage);
}

/** What one gate run recorded, returned, and captured. */
struct GateRun {
  RunStatus status;
  /** The gated rule's firing record after the run. */
  RuleFiringState record;
  /** True when the DO section ran, false when the gate skipped it. */
  bool fired;
  /** The `__whenResult` the gate captured for the gated rule. */
  Value captured;
};

/** Roots covering the rule-variable store the `__whenResult` capture allocates. */
struct CtxRoots : GcRoots {
  ExecutionContext* ctx = nullptr;

  void enumerateRoots(GcMarker& marker) override { marker.mark(ctx->ruleVarStores); }
};

/**
 * Runs the gated rule of a freshly built program once, with the subject's record
 * seeded to `subjectState`.
 */
GateRun runGate(Op gate, WhenValue whenValue, RuleFiringState subjectState,
                bool withSubject = true) {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = buildGateProgram(b, gate, whenValue, withSubject, storage);

  std::vector<uint8_t> heapStorage(64 * 1024);
  RegionArena heapArena(Span<uint8_t>(heapStorage.data(), heapStorage.size()));
  ManagedHeap heap(heapArena);

  ExecutionContext ctx;
  std::vector<uint8_t> ctxStorage(4 * 1024);
  RegionArena ctxArena(Span<uint8_t>(ctxStorage.data(), ctxStorage.size()));
  REQUIRE(ctx.bindSlots(ctxArena, 0, 0, 0, 0, kRuleRecordCount));
  ctx.setRuleFiringState(kSubjectFuncId, subjectState);

  CtxRoots roots;
  roots.ctx = &ctx;
  RuntimeSurface surface{&ctx, {}, nullptr, &heap};
  surface.roots = &roots;

  Machine machine;
  REQUIRE(startExecution(machine.state, image, kGatedFuncId, {}).isOk());
  machine.state.budget = 1000;
  const RunResult result = runExecution(machine.state, image, surface);

  GateRun run{result.status, ctx.ruleFiringState(kGatedFuncId), false, wendoo::kNilValue};
  if (result.status == RunStatus::Done) {
    REQUIRE(result.result.isBoolean());
    run.fired = result.result.asBoolean();
  }
  ctx.currentRuleFuncId = kGatedFuncId;
  run.captured = wendoo::getWhenResult(ctx, heap);
  return run;
}

/** Every WHEN value the base-gate / chain-gate parity cases sweep. */
constexpr std::array<WhenValue, 6> kWhenValues{{WhenValue::True, WhenValue::False, WhenValue::Nil,
                                                WhenValue::Zero, WhenValue::Seven,
                                                WhenValue::EmptyString}};

} // namespace

TEST_CASE("a chain gate that fires records DidFire whatever its subject recorded") {
  const std::array<RuleFiringState, 3> subjectStates{
      {RuleFiringState::DidFire, RuleFiringState::DidNotFire, RuleFiringState::Evaluating}};
  for (const RuleFiringState subjectState : subjectStates) {
    CAPTURE(static_cast<int>(subjectState));

    const GateRun truthiness = runGate(Op::WHEN_END_CHAIN, WhenValue::True, subjectState);
    CHECK(truthiness.record == RuleFiringState::DidFire);
    CHECK(truthiness.fired);

    const GateRun presence = runGate(Op::WHEN_END_PRESENT_CHAIN, WhenValue::Seven, subjectState);
    CHECK(presence.record == RuleFiringState::DidFire);
    CHECK(presence.fired);
  }
}

TEST_CASE("a chain gate that does not fire below an unfired subject records DidNotFire") {
  CHECK(runGate(Op::WHEN_END_CHAIN, WhenValue::False, RuleFiringState::DidNotFire).record ==
        RuleFiringState::DidNotFire);
  CHECK(runGate(Op::WHEN_END_PRESENT_CHAIN, WhenValue::Nil, RuleFiringState::DidNotFire).record ==
        RuleFiringState::DidNotFire);
}

TEST_CASE("a chain gate that does not fire below a fired subject records DidFire") {
  CHECK(runGate(Op::WHEN_END_CHAIN, WhenValue::False, RuleFiringState::DidFire).record ==
        RuleFiringState::DidFire);
  CHECK(runGate(Op::WHEN_END_PRESENT_CHAIN, WhenValue::Nil, RuleFiringState::DidFire).record ==
        RuleFiringState::DidFire);
}

TEST_CASE("a chain gate that does not fire below a subject still evaluating records Evaluating") {
  CHECK(runGate(Op::WHEN_END_CHAIN, WhenValue::False, RuleFiringState::Evaluating).record ==
        RuleFiringState::Evaluating);
  CHECK(runGate(Op::WHEN_END_PRESENT_CHAIN, WhenValue::Nil, RuleFiringState::Evaluating).record ==
        RuleFiringState::Evaluating);
}

TEST_CASE("a chain gate with no subject records its own outcome") {
  // A first rule at its level has no record to adopt.
  CHECK(runGate(Op::WHEN_END_CHAIN, WhenValue::False, RuleFiringState::DidFire, false).record ==
        RuleFiringState::DidNotFire);
  CHECK(runGate(Op::WHEN_END_CHAIN, WhenValue::True, RuleFiringState::DidFire, false).record ==
        RuleFiringState::DidFire);
}

TEST_CASE("a base gate below the same subject records only its own outcome") {
  // The chain write belongs to the chain gates alone.
  CHECK(runGate(Op::WHEN_END, WhenValue::False, RuleFiringState::DidFire).record ==
        RuleFiringState::DidNotFire);
  CHECK(runGate(Op::WHEN_END_PRESENT, WhenValue::Nil, RuleFiringState::DidFire).record ==
        RuleFiringState::DidNotFire);
}

TEST_CASE("the truthiness chain gate takes the same path as WHEN_END for every WHEN value") {
  for (const WhenValue value : kWhenValues) {
    CAPTURE(static_cast<int>(value));
    const GateRun base = runGate(Op::WHEN_END, value, RuleFiringState::DidNotFire);
    const GateRun chained = runGate(Op::WHEN_END_CHAIN, value, RuleFiringState::DidNotFire);
    REQUIRE(base.status == RunStatus::Done);
    REQUIRE(chained.status == RunStatus::Done);
    CHECK(chained.fired == base.fired);
    CHECK(chained.captured.tag() == base.captured.tag());
  }
}

TEST_CASE("the presence chain gate takes the same path as WHEN_END_PRESENT for every WHEN value") {
  for (const WhenValue value : kWhenValues) {
    CAPTURE(static_cast<int>(value));
    const GateRun base = runGate(Op::WHEN_END_PRESENT, value, RuleFiringState::DidNotFire);
    const GateRun chained = runGate(Op::WHEN_END_PRESENT_CHAIN, value, RuleFiringState::DidNotFire);
    REQUIRE(base.status == RunStatus::Done);
    REQUIRE(chained.status == RunStatus::Done);
    CHECK(chained.fired == base.fired);
    CHECK(chained.captured.tag() == base.captured.tag());
  }
}

TEST_CASE("a present but falsy WHEN value fires the presence chain gate and skips the other") {
  const std::array<WhenValue, 3> presentFalsy{
      {WhenValue::Zero, WhenValue::EmptyString, WhenValue::False}};
  for (const WhenValue value : presentFalsy) {
    CAPTURE(static_cast<int>(value));
    CHECK(runGate(Op::WHEN_END_PRESENT_CHAIN, value, RuleFiringState::DidNotFire).fired);
    CHECK_FALSE(runGate(Op::WHEN_END_CHAIN, value, RuleFiringState::DidNotFire).fired);
  }
}

TEST_CASE("both chain gates capture the WHEN result they were handed") {
  const GateRun truthiness =
      runGate(Op::WHEN_END_CHAIN, WhenValue::Seven, RuleFiringState::DidNotFire);
  REQUIRE(truthiness.captured.isNumber());
  CHECK(truthiness.captured.asNumber() == 7.0f);

  const GateRun presence =
      runGate(Op::WHEN_END_PRESENT_CHAIN, WhenValue::Seven, RuleFiringState::DidNotFire);
  REQUIRE(presence.captured.isNumber());
  CHECK(presence.captured.asNumber() == 7.0f);
}
