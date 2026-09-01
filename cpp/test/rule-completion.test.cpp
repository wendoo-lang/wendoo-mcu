/**
 * Rule-cluster completion: the subtree-liveness query, the watcher slot each
 * rule carries, the settle walk that resolves watchers at fiber terminal
 * transitions, and the rule-trigger host action that parks on them. Mirrors
 * `rule-completion.spec.ts` in external/wendoo-lang/packages/core/src/runtime.
 *
 * Every fixture runs hand-built rule bytecode on a real VM and fiber scheduler
 * over the same three-rule, one-page program: the subject and the waiter are the
 * page's root rules in that order, and the child is a child rule of the subject.
 */

#include "doctest/doctest.h"

#include "core/runtime/core-host-actions.h"
#include "core/runtime/device-profile-caps.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/fiber-scheduler.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/host-action.h"
#include "core/runtime/host-actions/core-host-action-bindings.h"
#include "core/runtime/host-actions/core-host-action-env.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/program.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/result.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"
#include "vm-harness.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

using wendoo::AsyncHandle;
using wendoo::CoreHostActionEnv;
using wendoo::ErrorCode;
using wendoo::ExecutionContext;
using wendoo::FiberRecord;
using wendoo::FiberScheduler;
using wendoo::FiberState;
using wendoo::Handle;
using wendoo::HandleState;
using wendoo::HostActionBinding;
using wendoo::kNoHandleId;
using wendoo::makeCoreHostActionBindings;
using wendoo::ManagedHeap;
using wendoo::Op;
using wendoo::ProgramImage;
using wendoo::RegionArena;
using wendoo::Result;
using wendoo::RuleFiringState;
using wendoo::RuntimeSurface;
using wendoo::Span;
using wendoo::Status;
using wendoo::Value;
using wendoo::test::kDeviceProfileCaps;

namespace CoreHostActions = wendoo::CoreHostActions;

namespace {

/** Root rule every fixture watches: the subject of the rule below it. */
constexpr uint32_t kSubject = 0;

/** Child rule of {@link kSubject}. */
constexpr uint32_t kChild = 1;

/** Root rule directly below {@link kSubject}: the rule that waits on it. */
constexpr uint32_t kWaiter = 2;

/** Value-pool indices shared by every fixture body. */
constexpr int32_t kTrueConst = 0;
constexpr int32_t kFalseConst = 1;
constexpr int32_t kNilConst = 2;

/** Records for the three fixture rules, plus headroom. */
constexpr uint32_t kRuleRecordCount = 8;

/** Host-action id of the test-only asynchronous action the fixtures park on. */
constexpr uint32_t kParkActionId = 501;

/** Call-site ids the fixture bodies dispatch their host actions through. */
constexpr int32_t kParkCallSiteId = 0;
constexpr int32_t kTriggerCallSiteId = 1;

/**
 * Asynchronous action body that records its handle and returns, so each dispatch
 * parks the calling fiber until the fixture settles it from outside the runtime.
 */
struct ParkingAction {
  std::vector<AsyncHandle> pending;

  /** Resolves every handle the action has left pending, oldest first. */
  void resolvePending(const Value& value) {
    for (const AsyncHandle& handle : pending) {
      handle.resolve(value);
    }
    pending.clear();
  }

  /** Rejects every handle the action has left pending, oldest first. */
  void rejectPending(ErrorCode error) {
    for (const AsyncHandle& handle : pending) {
      handle.reject(error);
    }
    pending.clear();
  }
};

Status execPark(void* hostData, ExecutionContext&, Span<const Value>, AsyncHandle handle) {
  static_cast<ParkingAction*>(hostData)->pending.push_back(handle);
  return Status::ok();
}

/** The shape of a fixture rule's body. */
enum class RuleBody : uint8_t {
  /** Fires on a truthy WHEN and returns, with an empty DO. */
  FiresEmptyDo,
  /** Evaluates a falsy WHEN and returns without firing. */
  NeverFires,
  /** Fires, then faults inside its DO on a pop from an empty operand stack. */
  FiresThenFaults,
  /** Parks mid-WHEN on the parking action, gating on whatever it resolves to. */
  ParksMidWhen,
  /** Fires, spawns {@link kChild}, then parks its own fiber mid-DO. */
  SpawnsChildThenParks,
  /** Fires, then parks its own fiber mid-DO, so its outcome is recorded first. */
  FiresThenParks,
  /** Awaits the rule trigger in its WHEN section and fires on a true answer. */
  TriggeredBySubject,
};

/** Appends `body` as the next function of `b`. */
void appendRuleBody(ProgramBuilder& b, RuleBody body) {
  switch (body) {
  case RuleBody::FiresEmptyDo:
  case RuleBody::NeverFires:
    b.beginFunction()
        .instr(Op::WHEN_START)
        .instr(Op::PUSH_CONST_VAL, body == RuleBody::NeverFires ? kFalseConst : kTrueConst)
        .instr(Op::WHEN_END, 3)
        .instr(Op::DO_START)
        .instr(Op::DO_END)
        .instr(Op::PUSH_CONST_VAL, kNilConst)
        .instr(Op::RET);
    return;
  case RuleBody::FiresThenFaults:
    b.beginFunction()
        .instr(Op::WHEN_START)
        .instr(Op::PUSH_CONST_VAL, kTrueConst)
        .instr(Op::WHEN_END, 4)
        .instr(Op::DO_START)
        .instr(Op::POP)
        .instr(Op::DO_END)
        .instr(Op::PUSH_CONST_VAL, kNilConst)
        .instr(Op::RET);
    return;
  case RuleBody::ParksMidWhen:
    b.beginFunction()
        .instr(Op::WHEN_START)
        .instr(Op::HOST_ACTION_CALL_ASYNC, static_cast<int32_t>(kParkActionId), 0, kParkCallSiteId)
        .instr(Op::AWAIT)
        .instr(Op::WHEN_END, 3)
        .instr(Op::DO_START)
        .instr(Op::DO_END)
        .instr(Op::PUSH_CONST_VAL, kNilConst)
        .instr(Op::RET);
    return;
  case RuleBody::SpawnsChildThenParks:
    b.beginFunction()
        .instr(Op::WHEN_START)
        .instr(Op::PUSH_CONST_VAL, kTrueConst)
        .instr(Op::WHEN_END, 7)
        .instr(Op::DO_START)
        .instr(Op::SPAWN_RULE, static_cast<int32_t>(kChild))
        .instr(Op::HOST_ACTION_CALL_ASYNC, static_cast<int32_t>(kParkActionId), 0, kParkCallSiteId)
        .instr(Op::AWAIT)
        .instr(Op::POP)
        .instr(Op::DO_END)
        .instr(Op::PUSH_CONST_VAL, kNilConst)
        .instr(Op::RET);
    return;
  case RuleBody::FiresThenParks:
    b.beginFunction()
        .instr(Op::WHEN_START)
        .instr(Op::PUSH_CONST_VAL, kTrueConst)
        .instr(Op::WHEN_END, 6)
        .instr(Op::DO_START)
        .instr(Op::HOST_ACTION_CALL_ASYNC, static_cast<int32_t>(kParkActionId), 0, kParkCallSiteId)
        .instr(Op::AWAIT)
        .instr(Op::POP)
        .instr(Op::DO_END)
        .instr(Op::PUSH_CONST_VAL, kNilConst)
        .instr(Op::RET);
    return;
  case RuleBody::TriggeredBySubject:
    b.beginFunction()
        .instr(Op::WHEN_START)
        .instr(Op::HOST_ACTION_CALL_ASYNC,
               static_cast<int32_t>(CoreHostActions::RuleTrigger.actionId), 0, kTriggerCallSiteId)
        .instr(Op::AWAIT)
        .instr(Op::WHEN_END, 3)
        .instr(Op::DO_START)
        .instr(Op::DO_END)
        .instr(Op::PUSH_CONST_VAL, kNilConst)
        .instr(Op::RET);
    return;
  }
}

/** One shared region with room for the fixtures' fibers and execution regions. */
struct SchedulerStorage {
  static constexpr size_t kArenaBytes = 8 * (2048 + sizeof(FiberRecord) + 64) + 1024;
  std::array<uint8_t, kArenaBytes> bytes;
  RegionArena arena{Span<uint8_t>(bytes.data(), bytes.size())};
};

/**
 * A running VM, its scheduler, and the brain-instance state the mechanisms read.
 * The program's three rules take the bodies handed to the constructor.
 */
struct Harness {
  Harness(RuleBody subject, RuleBody child, RuleBody waiter) {
    builder.poolString("page-id");
    builder.valueBool(true).valueBool(false).valueNil();
    appendRuleBody(builder, subject);
    appendRuleBody(builder, child);
    appendRuleBody(builder, waiter);
    builder.ruleFunc(kSubject).ruleFunc(kChild).ruleFunc(kWaiter);
    builder.ruleAncestor(kChild, kSubject);
    builder.beginPage(0)
        .pageRoot(kSubject)
        .pageRoot(kWaiter)
        .pageHostCallSite(static_cast<uint32_t>(kParkCallSiteId), kParkActionId)
        .pageHostCallSite(static_cast<uint32_t>(kTriggerCallSiteId),
                          CoreHostActions::RuleTrigger.actionId);
    image = builder.build(storage);

    REQUIRE(ctx.bindSlots(ctxArena, 0, 2, 0, 0, kRuleRecordCount));

    coreBindings = makeCoreHostActionBindings(env);
    bindings.push_back({kParkActionId, nullptr, nullptr, &park, &execPark});
    for (const HostActionBinding& binding : coreBindings) {
      bindings.push_back(binding);
    }

    surface.context = &ctx;
    surface.actions = {bindings.data(), bindings.size()};
    surface.heap = &heap;
    scheduler.emplace(image, surface, pools.arena, kDeviceProfileCaps);
    env.program = &image;
    env.ruleLiveness = &*scheduler;
    env.heap = &heap;
    env.roots = &*scheduler;
  }

  /** Spawns a root-rule fiber for `funcId` and returns its fiber id. */
  uint32_t spawnRoot(uint32_t funcId) {
    const Result<uint32_t> spawned = scheduler->spawn(funcId);
    REQUIRE(spawned.isOk());
    return spawned.value();
  }

  /** The lifecycle state of `fiberId`, or `Done` once its record is gone. */
  FiberState fiberState(uint32_t fiberId) const {
    const FiberRecord* record = scheduler->fiber(fiberId);
    return record == nullptr ? FiberState::Done : record->state;
  }

  /** Parks a fresh pending handle in `ruleFuncId`'s watcher slot. */
  uint32_t parkWatcher(uint32_t ruleFuncId) {
    const uint32_t handleId = scheduler->handles().createPending();
    REQUIRE(handleId != kNoHandleId);
    ctx.setRuleWatcher(ruleFuncId, handleId);
    return handleId;
  }

  ProgramBuilder builder;
  std::vector<uint8_t> storage = std::vector<uint8_t>(16 * 1024);
  ProgramImage image;
  std::vector<uint8_t> ctxStorage = std::vector<uint8_t>(4 * 1024);
  RegionArena ctxArena{Span<uint8_t>(ctxStorage.data(), ctxStorage.size())};
  ExecutionContext ctx;
  std::vector<uint8_t> heapStorage = std::vector<uint8_t>(64 * 1024);
  RegionArena heapArena{Span<uint8_t>(heapStorage.data(), heapStorage.size())};
  ManagedHeap heap{heapArena};
  ParkingAction park;
  CoreHostActionEnv env;
  std::array<HostActionBinding, wendoo::kCoreHostActionBindingCount> coreBindings{};
  std::vector<HostActionBinding> bindings;
  RuntimeSurface surface;
  SchedulerStorage pools;
  std::optional<FiberScheduler> scheduler;
};

/** The state of `handleId` in `harness`, treating a freed handle as resolved-away. */
HandleState handleStateOf(Harness& harness, uint32_t handleId) {
  const Handle* handle = harness.scheduler->handles().get(handleId);
  REQUIRE(handle != nullptr);
  return handle->state;
}

/** The value `handleId` resolved with. */
Value handleResultOf(Harness& harness, uint32_t handleId) {
  const Handle* handle = harness.scheduler->handles().get(handleId);
  REQUIRE(handle != nullptr);
  return handle->result;
}

/** Dispatches the rule trigger for `callerFuncId` and returns the handle it was given. */
uint32_t dispatchTrigger(Harness& harness, uint32_t callerFuncId) {
  const HostActionBinding* action =
      wendoo::findHostActionById(harness.surface.actions, CoreHostActions::RuleTrigger.actionId);
  REQUIRE(action != nullptr);
  REQUIRE(action->execAsync != nullptr);
  const uint32_t handleId = harness.scheduler->handles().createPending();
  REQUIRE(handleId != kNoHandleId);
  harness.ctx.currentRuleFuncId = callerFuncId;
  const Status status = action->execAsync(action->hostData, harness.ctx, Span<const Value>{},
                                          AsyncHandle{&harness.scheduler->handles(), handleId});
  harness.ctx.currentRuleFuncId = wendoo::kNoFuncId;
  REQUIRE(status.isOk());
  return handleId;
}

} // namespace

TEST_CASE("a rule whose own fiber is live has a live subtree") {
  Harness h(RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo);
  h.spawnRoot(kSubject);

  CHECK(h.scheduler->hasLiveRuleSubtree(kSubject));
  // An unspawned rule has no live fiber.
  CHECK_FALSE(h.scheduler->hasLiveRuleSubtree(kWaiter));
}

TEST_CASE("a live descendant keeps its ancestor's subtree live") {
  Harness h(RuleBody::SpawnsChildThenParks, RuleBody::ParksMidWhen, RuleBody::FiresEmptyDo);
  h.spawnRoot(kSubject);
  h.scheduler->tick();

  CHECK(h.scheduler->hasLiveRuleSubtree(kChild));
  // The child's fiber reaches the subject through the rule-ancestor chain.
  CHECK(h.scheduler->hasLiveRuleSubtree(kSubject));
  // Ancestry does not cross to a sibling.
  CHECK_FALSE(h.scheduler->hasLiveRuleSubtree(kWaiter));
}

TEST_CASE("a subtree whose fibers have all finished is not live") {
  Harness h(RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo);
  const uint32_t fiberId = h.spawnRoot(kSubject);
  h.scheduler->tick();

  CHECK(h.fiberState(fiberId) == FiberState::Done);
  CHECK_FALSE(h.scheduler->hasLiveRuleSubtree(kSubject));
}

TEST_CASE("a fiber waiting on a handle counts as live") {
  Harness h(RuleBody::ParksMidWhen, RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo);
  const uint32_t fiberId = h.spawnRoot(kSubject);
  h.scheduler->tick();

  CHECK(h.fiberState(fiberId) == FiberState::Waiting);
  CHECK(h.scheduler->hasLiveRuleSubtree(kSubject));
}

TEST_CASE("a watcher resolves true when the watched rule fired and its cluster emptied") {
  Harness h(RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo);
  const uint32_t handleId = h.parkWatcher(kSubject);
  h.spawnRoot(kSubject);
  h.scheduler->tick();

  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK(handleResultOf(h, handleId).asBoolean());
  CHECK(h.ctx.ruleFiringState(kSubject) == RuleFiringState::DidFire);
}

TEST_CASE("a watcher resolves false when the watched rule ended its think without firing") {
  Harness h(RuleBody::NeverFires, RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo);
  const uint32_t handleId = h.parkWatcher(kSubject);
  h.spawnRoot(kSubject);
  h.scheduler->tick();

  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK_FALSE(handleResultOf(h, handleId).asBoolean());
  CHECK(h.ctx.ruleFiringState(kSubject) == RuleFiringState::DidNotFire);
}

TEST_CASE("a watcher resolves false when the cluster faults after firing") {
  Harness h(RuleBody::FiresThenFaults, RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo);
  const uint32_t handleId = h.parkWatcher(kSubject);
  const uint32_t fiberId = h.spawnRoot(kSubject);
  h.scheduler->tick();

  CHECK(h.fiberState(fiberId) == FiberState::Fault);
  // The rule fired before it faulted, but a fault abandons the sequence.
  CHECK(h.ctx.ruleFiringState(kSubject) == RuleFiringState::DidFire);
  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK_FALSE(handleResultOf(h, handleId).asBoolean());
}

TEST_CASE("a watcher resolves false when a page exit cancels the cluster") {
  Harness h(RuleBody::SpawnsChildThenParks, RuleBody::ParksMidWhen, RuleBody::FiresEmptyDo);
  const uint32_t handleId = h.parkWatcher(kSubject);
  const uint32_t subjectFiberId = h.spawnRoot(kSubject);
  h.scheduler->tick();
  // The parked child holds the cluster open.
  CHECK(handleStateOf(h, handleId) == HandleState::Pending);

  // The page-exit cascade: the root rule's own fiber plus every child-rule fiber
  // beneath it.
  h.scheduler->cancelChildRuleFibers();
  h.scheduler->cancel(subjectFiberId);

  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK_FALSE(handleResultOf(h, handleId).asBoolean());
}

TEST_CASE("the watcher slot is empty once the walk has resolved it") {
  Harness h(RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo);
  h.parkWatcher(kSubject);
  h.spawnRoot(kSubject);
  h.scheduler->tick();

  CHECK(h.ctx.ruleWatcher(kSubject) == kNoHandleId);
}

TEST_CASE("a cluster held open by a parked descendant resolves the think that descendant ends") {
  Harness h(RuleBody::SpawnsChildThenParks, RuleBody::ParksMidWhen, RuleBody::FiresEmptyDo);
  const uint32_t handleId = h.parkWatcher(kSubject);
  h.spawnRoot(kSubject);
  h.scheduler->tick();
  // The subject's own fiber is parked mid-DO and the child is parked mid-WHEN.
  CHECK(handleStateOf(h, handleId) == HandleState::Pending);

  h.park.resolvePending(wendoo::kTrueValue);
  h.scheduler->drainCompletedHandles();
  h.scheduler->tick();

  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK(handleResultOf(h, handleId).asBoolean());
}

TEST_CASE("a watcher resolves false when a descendant faulted before the cluster emptied") {
  Harness h(RuleBody::SpawnsChildThenParks, RuleBody::FiresThenFaults, RuleBody::FiresEmptyDo);
  const uint32_t handleId = h.parkWatcher(kSubject);
  h.spawnRoot(kSubject);
  h.scheduler->tick();

  // The child fired before it faulted; the subject's own parked fiber holds the
  // cluster open past the fault.
  CHECK(h.ctx.ruleFiringState(kChild) == RuleFiringState::DidFire);
  CHECK_FALSE(h.scheduler->hasLiveRuleSubtree(kChild));
  CHECK(handleStateOf(h, handleId) == HandleState::Pending);

  h.park.resolvePending(wendoo::kTrueValue);
  h.scheduler->drainCompletedHandles();
  h.scheduler->tick();

  // A fault anywhere in the cluster abandons the whole firing, even though the
  // fiber that empties the cluster finishes normally.
  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK_FALSE(handleResultOf(h, handleId).asBoolean());
}

TEST_CASE("a watcher resolves false when a descendant was cancelled before the cluster emptied") {
  Harness h(RuleBody::SpawnsChildThenParks, RuleBody::ParksMidWhen, RuleBody::FiresEmptyDo);
  const uint32_t handleId = h.parkWatcher(kSubject);
  h.spawnRoot(kSubject);
  h.scheduler->tick();

  h.scheduler->cancelChildRuleFibers();
  // The subject's own parked fiber holds the cluster open past the cancellation.
  CHECK(handleStateOf(h, handleId) == HandleState::Pending);

  h.park.resolvePending(wendoo::kTrueValue);
  h.scheduler->drainCompletedHandles();
  h.scheduler->tick();

  // A cancellation abandons the cluster exactly as a fault does.
  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK_FALSE(handleResultOf(h, handleId).asBoolean());
}

TEST_CASE("a subject that fired and settled answers the trigger true at once") {
  Harness h(RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo, RuleBody::TriggeredBySubject);
  h.ctx.setRuleFiringState(kSubject, RuleFiringState::DidFire);

  const uint32_t handleId = dispatchTrigger(h, kWaiter);

  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK(handleResultOf(h, handleId).asBoolean());
  // An immediate answer occupies no watcher slot.
  CHECK(h.ctx.ruleWatcher(kSubject) == kNoHandleId);
}

TEST_CASE("a subject that faulted in its own DO after firing answers the trigger false at once") {
  Harness h(RuleBody::FiresThenFaults, RuleBody::FiresEmptyDo, RuleBody::TriggeredBySubject);
  const uint32_t fiberId = h.spawnRoot(kSubject);
  h.scheduler->tick();
  CHECK(h.fiberState(fiberId) == FiberState::Fault);
  CHECK(h.ctx.ruleFiringState(kSubject) == RuleFiringState::DidFire);

  const uint32_t handleId = dispatchTrigger(h, kWaiter);

  // The subtree a fault emptied is not a completion.
  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK_FALSE(handleResultOf(h, handleId).asBoolean());
}

TEST_CASE("a fresh firing clears the abandonment its predecessor left") {
  Harness h(RuleBody::FiresThenParks, RuleBody::FiresEmptyDo, RuleBody::TriggeredBySubject);
  h.spawnRoot(kSubject);
  h.scheduler->tick();
  h.park.rejectPending(ErrorCode::ScriptError);
  h.scheduler->drainCompletedHandles();
  h.scheduler->tick();

  // The rejection faulted the rule inside its DO, after it had fired, so the
  // abandoned firing skips the rule below it.
  CHECK(h.ctx.ruleFiringState(kSubject) == RuleFiringState::DidFire);
  CHECK(h.ctx.isRuleAbandoned(kSubject));
  CHECK_FALSE(handleResultOf(h, dispatchTrigger(h, kWaiter)).asBoolean());

  h.scheduler->sweep();
  const uint32_t watcherId = h.parkWatcher(kSubject);
  h.spawnRoot(kSubject);
  CHECK_FALSE(h.ctx.isRuleAbandoned(kSubject));
  h.scheduler->tick();
  h.park.resolvePending(wendoo::kTrueValue);
  h.scheduler->drainCompletedHandles();
  h.scheduler->tick();

  // The next firing completed with no fault in its cluster.
  CHECK(handleStateOf(h, watcherId) == HandleState::Resolved);
  CHECK(handleResultOf(h, watcherId).asBoolean());
  CHECK(handleResultOf(h, dispatchTrigger(h, kWaiter)).asBoolean());
}

TEST_CASE("a subject settled without firing answers the trigger false at once") {
  Harness h(RuleBody::NeverFires, RuleBody::FiresEmptyDo, RuleBody::TriggeredBySubject);
  h.ctx.setRuleFiringState(kSubject, RuleFiringState::DidNotFire);

  const uint32_t handleId = dispatchTrigger(h, kWaiter);

  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK_FALSE(handleResultOf(h, handleId).asBoolean());
}

TEST_CASE("a rule with no subject answers the trigger false at once") {
  Harness h(RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo, RuleBody::TriggeredBySubject);

  const uint32_t handleId = dispatchTrigger(h, kSubject);

  CHECK(handleStateOf(h, handleId) == HandleState::Resolved);
  CHECK_FALSE(handleResultOf(h, handleId).asBoolean());
}

TEST_CASE("a subject whose cluster is in flight parks the handle in its watcher slot") {
  Harness h(RuleBody::FiresEmptyDo, RuleBody::FiresEmptyDo, RuleBody::TriggeredBySubject);
  h.spawnRoot(kSubject);
  h.ctx.setRuleFiringState(kWaiter, RuleFiringState::Evaluating);

  const uint32_t handleId = dispatchTrigger(h, kWaiter);

  CHECK(handleStateOf(h, handleId) == HandleState::Pending);
  CHECK(h.ctx.ruleWatcher(kSubject) == handleId);
  // The waiting rule records that it has not fired, so a chain-gated sibling
  // below it evaluates during the wait.
  CHECK(h.ctx.ruleFiringState(kWaiter) == RuleFiringState::DidNotFire);
}

TEST_CASE("a parked trigger resumes its rule through AWAIT when the subject's cluster settles") {
  Harness h(RuleBody::ParksMidWhen, RuleBody::FiresEmptyDo, RuleBody::TriggeredBySubject);
  h.spawnRoot(kSubject);
  h.spawnRoot(kWaiter);

  h.scheduler->tick();
  // The subject is parked mid-WHEN; the trigger's pre-wait write survives its
  // own rule's WHEN_START.
  CHECK(h.ctx.ruleFiringState(kSubject) == RuleFiringState::Evaluating);
  CHECK(h.ctx.ruleFiringState(kWaiter) == RuleFiringState::DidNotFire);
  CHECK(h.ctx.ruleWatcher(kSubject) != kNoHandleId);

  h.park.resolvePending(wendoo::kTrueValue);
  h.scheduler->drainCompletedHandles();
  h.scheduler->tick();
  // The subject resumed, fired, and completed; a resumed waiter joins the next
  // round.
  CHECK(h.ctx.ruleFiringState(kSubject) == RuleFiringState::DidFire);
  CHECK(h.ctx.ruleFiringState(kWaiter) == RuleFiringState::DidNotFire);

  h.scheduler->drainCompletedHandles();
  h.scheduler->tick();
  // A true trigger answer fires the waiting rule.
  CHECK(h.ctx.ruleFiringState(kWaiter) == RuleFiringState::DidFire);
  CHECK(h.ctx.ruleWatcher(kSubject) == kNoHandleId);
}

TEST_CASE("a parked trigger answers false when the subject's cluster ends unfired") {
  Harness h(RuleBody::ParksMidWhen, RuleBody::FiresEmptyDo, RuleBody::TriggeredBySubject);
  h.spawnRoot(kSubject);
  h.spawnRoot(kWaiter);

  h.scheduler->tick();
  h.park.resolvePending(wendoo::kFalseValue);
  h.scheduler->drainCompletedHandles();
  h.scheduler->tick();
  h.scheduler->drainCompletedHandles();
  h.scheduler->tick();

  CHECK(h.ctx.ruleFiringState(kSubject) == RuleFiringState::DidNotFire);
  // The waiting rule takes the skip path.
  CHECK(h.ctx.ruleFiringState(kWaiter) == RuleFiringState::DidNotFire);
}

TEST_CASE("a page exit during a wait leaves no pending trigger handle") {
  for (const bool cancelSubjectFirst : {true, false}) {
    CAPTURE(cancelSubjectFirst);
    Harness h(RuleBody::ParksMidWhen, RuleBody::FiresEmptyDo, RuleBody::TriggeredBySubject);
    const uint32_t subjectFiberId = h.spawnRoot(kSubject);
    const uint32_t waiterFiberId = h.spawnRoot(kWaiter);
    h.scheduler->tick();

    const uint32_t triggerHandleId = h.ctx.ruleWatcher(kSubject);
    REQUIRE(triggerHandleId != kNoHandleId);

    if (cancelSubjectFirst) {
      h.scheduler->cancel(subjectFiberId);
      h.scheduler->cancel(waiterFiberId);
    } else {
      h.scheduler->cancel(waiterFiberId);
      h.scheduler->cancel(subjectFiberId);
    }

    CHECK(handleStateOf(h, triggerHandleId) != HandleState::Pending);
    CHECK(h.ctx.ruleWatcher(kSubject) == kNoHandleId);
    CHECK(h.fiberState(waiterFiberId) == FiberState::Cancelled);
  }
}
