#include "doctest/doctest.h"

#include "core/runtime/brain-runtime.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/fiber-scheduler.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/host-action.h"
#include "core/runtime/host-function.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"
#include "vm-harness.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using wendoo::AsyncHandle;
using wendoo::BrainRuntime;
using wendoo::ErrorCode;
using wendoo::ExecutionContext;
using wendoo::FiberScheduler;
using wendoo::Frame;
using wendoo::HostActionBinding;
using wendoo::kNoCallSiteId;
using wendoo::Op;
using wendoo::ProgramImage;
using wendoo::RegionArena;
using wendoo::RuntimeSurface;
using wendoo::Span;
using wendoo::Status;
using wendoo::TargetHostFuncBinding;
using wendoo::Value;
using wendoo::VmObserver;

namespace {

/** One shared region with room for several fibers' regions and records; tests spawn a few. */
struct SchedulerStorage {
  static constexpr size_t kArenaBytes = 8 * (2048 + sizeof(wendoo::FiberRecord) + 64) + 256;
  std::array<uint8_t, kArenaBytes> bytes;
  RegionArena arena{Span<uint8_t>(bytes.data(), bytes.size())};
};

/** Observer counting dispatches and recording faulted fiber ids. */
struct CountingObserver : VmObserver {
  uint32_t actionCalls = 0;
  std::vector<uint32_t> faultedFibers;
  std::vector<ErrorCode> faultCodes;

  void onHostActionCall(uint32_t, uint32_t, Span<const Value>, const Value&) override {
    actionCalls++;
  }

  void onFiberFault(uint32_t fiberId, ErrorCode code) override {
    faultedFibers.push_back(fiberId);
    faultCodes.push_back(code);
  }
};

Value execNoop(void*, ExecutionContext&, Span<const Value>) { return wendoo::kVoidValue; }

/** Host-action env carrying the brain so an action body can restart its page. */
struct RestartEnv {
  BrainRuntime* brain = nullptr;
};

/** A host action that restarts the active page mid-round (cancels its fibers). */
Value execRestartPage(void* hostData, ExecutionContext&, Span<const Value>) {
  static_cast<RestartEnv*>(hostData)->brain->requestPageRestart();
  return wendoo::kVoidValue;
}

void markStateOnPageEntered(void*, ExecutionContext& ctx) {
  ctx.setCallSiteState(Value::boolean(true));
}

/** A one-page program whose single rule dispatches action 1 at call site 0. */
ProgramImage rulePageProgram(ProgramBuilder& b, std::vector<uint8_t>& storage) {
  b.poolString("page-id");
  b.beginFunction().instr(Op::HOST_ACTION_CALL, 1, 0, 0).instr(Op::RET);
  b.ruleFunc(0);
  b.beginPage(0).pageRoot(0).pageHostCallSite(0, 1);
  return b.build(storage);
}

} // namespace

TEST_CASE("think stamps time, the dt rule, and the tick counter") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = rulePageProgram(b, storage);

  const HostActionBinding bindings[1] = {{1, &execNoop, nullptr, nullptr}};
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings, 1}, nullptr};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  // dt stays 0 until a previous think exists.
  REQUIRE(brain.think(16.0f).isOk());
  CHECK(ctx.time == 16.0f);
  CHECK(ctx.dt == 0.0f);
  CHECK(ctx.currentTick == 1);

  REQUIRE(brain.think(48.0f).isOk());
  CHECK(ctx.time == 48.0f);
  CHECK(ctx.dt == 32.0f);
  CHECK(ctx.currentTick == 2);
}

TEST_CASE("a completed rule fiber respawns and re-evaluates every think") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = rulePageProgram(b, storage);

  const HostActionBinding bindings[1] = {{1, &execNoop, nullptr, nullptr}};
  ExecutionContext ctx;
  CountingObserver observer;
  RuntimeSurface surface{&ctx, {bindings, 1}, &observer};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  for (int i = 1; i <= 3; i++) {
    REQUIRE(brain.think(16.0f * static_cast<float>(i)).isOk());
    CHECK(observer.actionCalls == static_cast<uint32_t>(i));
  }
}

TEST_CASE("a fault kills the fiber, not the rule: it respawns next think") {
  ProgramBuilder b;
  b.poolString("page-id");
  b.beginFunction().instr(Op::POP).instr(Op::RET);
  b.ruleFunc(0);
  b.beginPage(0).pageRoot(0);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  ExecutionContext ctx;
  CountingObserver observer;
  RuntimeSurface surface{&ctx, {}, &observer};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  REQUIRE(brain.think(16.0f).isOk());
  REQUIRE(brain.think(32.0f).isOk());
  REQUIRE(observer.faultedFibers.size() == 2);
  // Each think faulted a fresh fiber: respawn allocated a new fiber id.
  CHECK(observer.faultedFibers[0] == 1);
  CHECK(observer.faultedFibers[1] == 2);
  CHECK(observer.faultCodes[0] == ErrorCode::StackUnderflow);
  CHECK(observer.faultCodes[1] == ErrorCode::StackUnderflow);
}

TEST_CASE("an unregistered action id faults the fiber and the rule respawns") {
  ProgramBuilder b;
  b.poolString("page-id");
  b.beginFunction().instr(Op::HOST_ACTION_CALL, 0x777, 0, 0).instr(Op::RET);
  b.ruleFunc(0);
  b.beginPage(0).pageRoot(0).pageHostCallSite(0, 0x777);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  ExecutionContext ctx;
  CountingObserver observer;
  RuntimeSurface surface{&ctx, {}, &observer};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  // Activation skips the unregistered call site; the existence check faults
  // at dispatch instead.
  REQUIRE(brain.startup().isOk());

  REQUIRE(brain.think(16.0f).isOk());
  REQUIRE(brain.think(32.0f).isOk());
  REQUIRE(observer.faultedFibers.size() == 2);
  CHECK(observer.faultedFibers[0] == 1);
  CHECK(observer.faultedFibers[1] == 2);
  CHECK(observer.faultCodes[0] == ErrorCode::ScriptError);
  CHECK(observer.faultCodes[1] == ErrorCode::ScriptError);
}

TEST_CASE("page activation runs each call site's page-entered hook bound to it") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = rulePageProgram(b, storage);

  // The hook records state against whatever call site activation binds; finding
  // it set on call site 0, with the binding restored afterward, proves the hook
  // ran bound to that site.
  const HostActionBinding bindings[1] = {{1, &execNoop, &markStateOnPageEntered, nullptr}};
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings, 1}, nullptr};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  REQUIRE(ctx.callSiteStatePresent.size() >= 1);
  CHECK(ctx.callSiteStatePresent[0]);
  CHECK(ctx.callSiteStates[0].asBoolean());
  CHECK(ctx.currentCallSiteId == kNoCallSiteId);
}

namespace {

/**
 * Appends one host-action lifecycle observation to `log` as a kind letter
 * plus the bound call-site id digit: 'i' initialized, 'e' page entered,
 * 'x' page exited, 'c' body dispatched.
 */
void logEvent(void* hostData, char kind, uint32_t callSiteId) {
  std::string& log = *static_cast<std::string*>(hostData);
  log += kind;
  log += static_cast<char>('0' + callSiteId);
}

void logInitialized(void* hostData, ExecutionContext& ctx) {
  logEvent(hostData, 'i', ctx.currentCallSiteId);
}

void logPageEntered(void* hostData, ExecutionContext& ctx) {
  logEvent(hostData, 'e', ctx.currentCallSiteId);
}

void logPageExited(void* hostData, ExecutionContext& ctx) {
  logEvent(hostData, 'x', ctx.currentCallSiteId);
}

Value execLogCall(void* hostData, ExecutionContext& ctx, Span<const Value>) {
  logEvent(hostData, 'c', ctx.currentCallSiteId);
  return wendoo::kVoidValue;
}

/** The fully-hooked logging registration for action 1 over `log`. */
HostActionBinding hookedLogBinding(std::string& log) {
  HostActionBinding binding{1, &execLogCall, &logPageEntered, &log};
  binding.onInitialized = &logInitialized;
  binding.onPageExited = &logPageExited;
  return binding;
}

} // namespace

TEST_CASE("activation runs each host call site's initializer once, before its entered hook") {
  ProgramBuilder b;
  b.poolString("page-id");
  // Two call sites of the same action on one page: the initializer and the
  // entered hook run per call site, in call-site order, interleaved.
  b.beginPage(0).pageHostCallSite(0, 1).pageHostCallSite(1, 1);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  std::string log;
  const HostActionBinding bindings[1] = {hookedLogBinding(log)};
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings, 1}, nullptr};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  CHECK(log == "i0e0i1e1");
  CHECK(ctx.currentCallSiteId == kNoCallSiteId);
}

TEST_CASE("a page transition runs exited hooks, then the new page's hooks, then its rules") {
  ProgramBuilder b;
  b.poolString("page-0");
  b.poolString("page-1");
  // funcId 0: page 1's root rule, dispatching action 1 at call site 1.
  b.beginFunction().instr(Op::HOST_ACTION_CALL, 1, 0, 1).instr(Op::RET);
  b.ruleFunc(0);
  // Each page also carries a call site of hookless action 2, which must pass
  // through activation and deactivation without effect.
  b.beginPage(0).pageHostCallSite(0, 1).pageHostCallSite(2, 2);
  b.beginPage(1).pageRoot(0).pageHostCallSite(1, 1).pageHostCallSite(3, 2);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  std::string log;
  const HostActionBinding bindings[2] = {hookedLogBinding(log), {2, &execNoop, nullptr, nullptr}};
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings, 2}, nullptr};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());
  CHECK(log == "i0e0");

  // Switching pages runs the old page's exited hook, then the new page's
  // initializer and entered hooks, and only then the new page's rules.
  log.clear();
  brain.requestPageChange(1);
  REQUIRE(brain.think(16.0f).isOk());
  CHECK(log == "x0i1e1c1");

  // Switching back re-enters call site 0 without re-running its initializer:
  // the initializer fires once per call site for the brain's lifetime.
  log.clear();
  brain.requestPageChange(0);
  REQUIRE(brain.think(32.0f).isOk());
  CHECK(log == "x1e0");
  CHECK(ctx.currentCallSiteId == kNoCallSiteId);
}

TEST_CASE("a reset call site re-runs its initializer on the next activation") {
  ProgramBuilder b;
  b.poolString("page-0");
  b.poolString("page-1");
  // Two hooked call sites on page 0; page 1 is bare, so a round trip through it
  // deactivates and re-activates both of page 0's call sites.
  b.beginPage(0).pageHostCallSite(0, 1).pageHostCallSite(1, 1);
  b.beginPage(1);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  std::string log;
  const HostActionBinding bindings[1] = {hookedLogBinding(log)};
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings, 1}, nullptr};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());
  REQUIRE(log == "i0e0i1e1");

  ctx.resetCallSite(0);

  log.clear();
  brain.requestPageChange(1);
  REQUIRE(brain.think(16.0f).isOk());
  REQUIRE(log == "x0x1");

  // Re-activating page 0 re-runs only the reset call site's initializer; call
  // site 1 keeps its spent gate and gets its entered hook alone.
  log.clear();
  brain.requestPageChange(0);
  REQUIRE(brain.think(32.0f).isOk());
  CHECK(log == "i0e0e1");
  CHECK(ctx.currentCallSiteId == kNoCallSiteId);
}

namespace {

/** Host data of {@link execReenterThink}: the runtime to re-enter and the result. */
struct ReentryProbe {
  BrainRuntime* brain;
  Status reentry = Status::ok();
};

Value execReenterThink(void* hostData, ExecutionContext&, Span<const Value>) {
  ReentryProbe* probe = static_cast<ReentryProbe*>(hostData);
  probe->reentry = probe->brain->think(999.0f);
  return wendoo::kVoidValue;
}

} // namespace

TEST_CASE("think is single-entry: re-entering from a host body fails loudly") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = rulePageProgram(b, storage);

  ReentryProbe probe{nullptr};
  const HostActionBinding bindings[1] = {{1, &execReenterThink, nullptr, &probe}};
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings, 1}, nullptr};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  probe.brain = &brain;
  REQUIRE(brain.startup().isOk());

  REQUIRE(brain.think(16.0f).isOk());
  REQUIRE(!probe.reentry.isOk());
  CHECK(probe.reentry.error() == ErrorCode::HostError);
  // The outer think survived: time advanced normally on the next think.
  REQUIRE(brain.think(32.0f).isOk());
  CHECK(ctx.dt == 16.0f);
}

TEST_CASE("a program with no pages starts up and thinks as a no-op") {
  ProgramBuilder b;
  b.valueNil();
  b.beginFunction().instr(Op::PUSH_CONST_VAL, 0).instr(Op::RET);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {}, nullptr};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());
  REQUIRE(brain.think(16.0f).isOk());
  CHECK(ctx.currentTick == 0);
  CHECK(scheduler.liveCount() == 0);
}

TEST_CASE("requesting the active page restarts its rules from their entry") {
  ProgramBuilder b;
  b.poolString("page-id");
  // The rule dispatches the action, then yields and suspends mid-rule.
  b.beginFunction().instr(Op::HOST_ACTION_CALL, 1, 0, 0).instr(Op::YIELD).instr(Op::RET);
  b.ruleFunc(0);
  b.beginPage(0).pageRoot(0).pageHostCallSite(0, 1);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  const HostActionBinding bindings[1] = {{1, &execNoop, nullptr, nullptr}};
  ExecutionContext ctx;
  CountingObserver observer;
  RuntimeSurface surface{&ctx, {bindings, 1}, &observer};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  // Think 1 dispatches the action once, then the rule yields and suspends.
  REQUIRE(brain.think(16.0f).isOk());
  CHECK(observer.actionCalls == 1);

  // Requesting the active page restarts it: the suspended fiber is cancelled and
  // respawned from its entry, so think 2 re-dispatches the action. A plain
  // resume past the yield would leave the count at 1.
  brain.requestPageChange(0);
  REQUIRE(brain.think(32.0f).isOk());
  CHECK(observer.actionCalls == 2);
}

TEST_CASE(
    "a page restart mid-drain cancels a still-pending sibling child rule without orphaning it") {
  ProgramBuilder b;
  b.poolString("page-id");
  b.valueNil(); // value pool 0: the RET value
  // funcId 0: a parent rule that spawns two child rules at its tail (funcId 1
  // then funcId 2), both into the same-think spawn drain.
  b.beginFunction()
      .instr(Op::SPAWN_RULE, 1)
      .instr(Op::SPAWN_RULE, 2)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::RET);
  // funcId 1: the first child, drained first; it restarts the page mid-cascade,
  // which cancels itself and its still-pending sibling.
  b.beginFunction()
      .instr(Op::HOST_ACTION_CALL, 1, 0, 0)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::RET);
  // funcId 2: the sibling child, still pending in the spawn drain when the
  // restart cancels it, so it never runs.
  b.beginFunction().instr(Op::PUSH_CONST_VAL, 0).instr(Op::RET);
  b.ruleFunc(0);
  b.ruleFunc(1);
  b.ruleFunc(2);
  b.beginPage(0).pageRoot(0).pageHostCallSite(0, 1);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  RestartEnv env;
  const HostActionBinding bindings[1] = {{1, &execRestartPage, nullptr, &env}};
  ExecutionContext ctx;
  CountingObserver observer;
  RuntimeSurface surface{&ctx, {bindings, 1}, &observer};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  env.brain = &brain;
  REQUIRE(brain.startup().isOk());

  // Each think the parent spawns two children into the same-think drain; the
  // first child restarts the page mid-cascade, so the page-scoped cancellation
  // must reclaim both the running child and its still-pending sibling. A leak or
  // orphan would grow liveCount without bound across thinks; a mid-drain-cancel
  // crash would fault or abort.
  for (int i = 1; i <= 50; i++) {
    REQUIRE(brain.think(16.0f * static_cast<float>(i)).isOk());
    CHECK(scheduler.liveCount() <= 4);
  }
  CHECK(observer.faultedFibers.empty());
}

namespace {

/** A one-page program carrying two hookable host call sites and no rules. */
ProgramImage twoCallSitePageProgram(ProgramBuilder& b, std::vector<uint8_t>& storage) {
  b.poolString("page-id");
  b.beginPage(0).pageHostCallSite(0, 1).pageHostCallSite(1, 1);
  return b.build(storage);
}

} // namespace

TEST_CASE("shutdown runs the current page's exited hooks bound to their call sites") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = twoCallSitePageProgram(b, storage);

  std::string log;
  const HostActionBinding bindings[1] = {hookedLogBinding(log)};
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings, 1}, nullptr};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());
  REQUIRE(log == "i0e0i1e1");

  log.clear();
  REQUIRE(brain.shutdown().isOk());
  CHECK(log == "x0x1");
  CHECK(ctx.currentCallSiteId == kNoCallSiteId);
}

TEST_CASE("a repeated shutdown runs the current page's exited hooks again") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = twoCallSitePageProgram(b, storage);

  std::string log;
  const HostActionBinding bindings[1] = {hookedLogBinding(log)};
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings, 1}, nullptr};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());
  REQUIRE(brain.shutdown().isOk());

  log.clear();
  REQUIRE(brain.shutdown().isOk());
  CHECK(log == "x0x1");
}

TEST_CASE("think after shutdown does nothing") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = rulePageProgram(b, storage);

  const HostActionBinding bindings[1] = {{1, &execNoop, nullptr, nullptr}};
  ExecutionContext ctx;
  CountingObserver observer;
  RuntimeSurface surface{&ctx, {bindings, 1}, &observer};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());
  REQUIRE(brain.think(16.0f).isOk());
  REQUIRE(observer.actionCalls == 1);

  REQUIRE(brain.shutdown().isOk());
  REQUIRE(brain.think(32.0f).isOk());
  // No rule re-fired and no time was stamped: the think returned before the
  // tick, leaving the counters where the last live think left them.
  CHECK(observer.actionCalls == 1);
  CHECK(ctx.currentTick == 1);
  CHECK(ctx.time == 16.0f);
}

namespace {

// A target-range async host function whose handle never settles, so a rule that
// awaits it is still parked when the brain shuts down.
constexpr uint32_t kNeverSettleFnId = 1500;

Status execNeverSettle(void*, ExecutionContext&, Span<const Value>, AsyncHandle) {
  return Status::ok();
}

} // namespace

TEST_CASE("shutdown drops the async handles the page left pending") {
  ProgramBuilder b;
  b.poolString("page-id");
  b.beginFunction()
      .instr(Op::HOST_CALL_ASYNC, kNeverSettleFnId, 0, 0)
      .instr(Op::AWAIT)
      .instr(Op::RET);
  b.ruleFunc(0);
  b.beginPage(0).pageRoot(0);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  const TargetHostFuncBinding hostFns[1] = {{kNeverSettleFnId, nullptr, nullptr, &execNeverSettle}};
  ExecutionContext ctx;
  CountingObserver observer;
  RuntimeSurface surface{&ctx, {}, &observer};
  surface.hostFunctions = {hostFns, 1};
  SchedulerStorage pools;
  FiberScheduler scheduler(image, surface, pools.arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  REQUIRE(brain.think(16.0f).isOk());
  REQUIRE(observer.faultedFibers.empty());
  REQUIRE(scheduler.handles().size() == 1);

  REQUIRE(brain.shutdown().isOk());
  CHECK(scheduler.handles().size() == 0);
  CHECK(scheduler.handles().cappedSize() == 0);
  CHECK(scheduler.handles().hasCapacity());
}
