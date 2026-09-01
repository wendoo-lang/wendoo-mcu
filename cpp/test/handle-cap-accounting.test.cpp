/**
 * Async-handle cap accounting and starvation faulting.
 *
 * Two mechanisms share the `maxHandles` guard. A host action declaring
 * `uncappedHandles` allocates its handles outside the accounting, so its live
 * count never blocks capped device work; every other async dispatch counts, and
 * one that finds the cap full parks and re-executes its identical dispatch next
 * round. A dispatch that stays starved for `kHandleBackpressureFaultRounds`
 * consecutive rounds faults `StackOverflow` through the ordinary fault path.
 *
 * Mirrors the TypeScript pins in
 * external/wendoo-lang/packages/core/src/runtime/vm-handle-backpressure.spec.ts.
 */

#include "doctest/doctest.h"

#include "core/runtime/brain-runtime.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/execution-state.h"
#include "core/runtime/fiber-scheduler.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/host-action.h"
#include "core/runtime/value.h"
#include "device-profile-caps.h"
#include "vm-harness.h"

#include <array>
#include <cstdint>
#include <vector>

using wendoo::AsyncHandle;
using wendoo::BrainRuntime;
using wendoo::ErrorCode;
using wendoo::ExecutionContext;
using wendoo::FiberScheduler;
using wendoo::HandleTable;
using wendoo::HostActionBinding;
using wendoo::kHandleBackpressureFaultRounds;
using wendoo::Op;
using wendoo::ProgramImage;
using wendoo::RegionArena;
using wendoo::RuntimeSurface;
using wendoo::Span;
using wendoo::Status;
using wendoo::Value;
using wendoo::VmObserver;

namespace {

/** Records every fiber fault the runtime reports, with the last error code. */
struct FaultObserver : VmObserver {
  uint32_t faults = 0;
  ErrorCode lastError = ErrorCode::ScriptError;
  void onHostActionCall(uint32_t, uint32_t, Span<const Value>, const Value&) override {}
  void onFiberFault(uint32_t, ErrorCode code) override {
    faults++;
    lastError = code;
  }
};

/** One shared region with room for the fixtures' fibers and execution regions. */
struct SchedulerStorage {
  static constexpr size_t kArenaBytes = 8 * (2048 + sizeof(wendoo::FiberRecord) + 64) + 1024;
  std::array<uint8_t, kArenaBytes> bytes;
  RegionArena arena{Span<uint8_t>(bytes.data(), bytes.size())};
};

constexpr uint32_t kParkActionId = 140;
constexpr uint32_t kParkCallSiteId = 0;

/** Counts dispatches and keeps every handle it is handed, never settling one. */
struct ParkEnv {
  uint32_t dispatches = 0;
};

Status execPark(void* hostData, ExecutionContext&, Span<const Value>, AsyncHandle) {
  static_cast<ParkEnv*>(hostData)->dispatches++;
  return Status::ok();
}

/**
 * A one-page program with `rootCount` root rules, each dispatching the parking
 * async host action and awaiting its handle.
 */
ProgramImage buildParkingRootsProgram(ProgramBuilder& b, std::vector<uint8_t>& storage,
                                      uint32_t rootCount) {
  b.poolString("page-id");
  b.valueNil();
  for (uint32_t i = 0; i < rootCount; i++) {
    b.beginFunction()
        .instr(Op::HOST_ACTION_CALL_ASYNC, static_cast<int32_t>(kParkActionId), 0, kParkCallSiteId)
        .instr(Op::AWAIT)
        .instr(Op::PUSH_CONST_VAL, 0)
        .instr(Op::RET);
  }
  b.beginPage(0);
  for (uint32_t i = 0; i < rootCount; i++) {
    b.pageRoot(static_cast<int32_t>(i));
  }
  b.pageHostCallSite(kParkCallSiteId, kParkActionId);
  return b.build(storage);
}

} // namespace

TEST_CASE("the handle table counts only capped handles against its cap") {
  std::array<uint8_t, 4096> bytes{};
  RegionArena arena(Span<uint8_t>(bytes.data(), bytes.size()));
  HandleTable handles(arena, 2);

  const uint32_t u1 = handles.createPending(false);
  const uint32_t u2 = handles.createPending(false);
  const uint32_t u3 = handles.createPending(false);
  CHECK(u1 != wendoo::kNoHandleId);
  CHECK(u2 != wendoo::kNoHandleId);
  CHECK(u3 != wendoo::kNoHandleId);
  CHECK(handles.cappedSize() == 0u);
  CHECK(handles.size() == 3u);
  CHECK(handles.hasCapacity());

  // The whole cap is still available to capped work.
  const uint32_t c1 = handles.createPending();
  const uint32_t c2 = handles.createPending();
  CHECK(c1 != wendoo::kNoHandleId);
  CHECK(c2 != wendoo::kNoHandleId);
  CHECK(handles.cappedSize() == 2u);
  CHECK_FALSE(handles.hasCapacity());
  CHECK(handles.createPending() == wendoo::kNoHandleId);

  // An uncapped create still succeeds with the cap full.
  CHECK(handles.createPending(false) != wendoo::kNoHandleId);

  // Freeing a capped handle returns its slot; freeing an uncapped one does not
  // change the count.
  handles.deleteHandle(u1);
  CHECK(handles.cappedSize() == 2u);
  CHECK_FALSE(handles.hasCapacity());
  handles.deleteHandle(c1);
  CHECK(handles.cappedSize() == 1u);
  CHECK(handles.hasCapacity());
}

TEST_CASE("a capped async host action fills the cap and backpressures the rest") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = buildParkingRootsProgram(b, storage, 6);

  SchedulerStorage pools;
  ExecutionContext ctx;
  FaultObserver observer;
  ParkEnv env;
  const HostActionBinding actions[1] = {{kParkActionId, nullptr, nullptr, &env, &execPark}};
  RuntimeSurface surface{&ctx, {actions, 1}, &observer};

  FiberScheduler scheduler(image, surface, pools.arena,
                           wendoo::test::withMaxHandles(wendoo::test::kDeviceProfileCaps, 2));
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());
  REQUIRE(brain.think(16.0f).isOk());

  CHECK(env.dispatches == 2u);
  CHECK(scheduler.handles().cappedSize() == 2u);
  CHECK(observer.faults == 0);
}

TEST_CASE("an uncapped async host action dispatches past the cap") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = buildParkingRootsProgram(b, storage, 6);

  SchedulerStorage pools;
  ExecutionContext ctx;
  FaultObserver observer;
  ParkEnv env;
  HostActionBinding actions[1] = {{kParkActionId, nullptr, nullptr, &env, &execPark}};
  actions[0].uncappedHandles = true;
  RuntimeSurface surface{&ctx, {actions, 1}, &observer};

  FiberScheduler scheduler(image, surface, pools.arena,
                           wendoo::test::withMaxHandles(wendoo::test::kDeviceProfileCaps, 2));
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());
  REQUIRE(brain.think(16.0f).isOk());

  CHECK(env.dispatches == 6u);
  CHECK(scheduler.handles().size() == 6u);
  CHECK(scheduler.handles().cappedSize() == 0u);
  CHECK(observer.faults == 0);
}

TEST_CASE("a dispatch starved past the backpressure limit faults StackOverflow") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  // Two roots under a cap of one: the first takes the only slot and never
  // settles, so the second can never allocate.
  const ProgramImage image = buildParkingRootsProgram(b, storage, 2);

  SchedulerStorage pools;
  ExecutionContext ctx;
  FaultObserver observer;
  ParkEnv env;
  const HostActionBinding actions[1] = {{kParkActionId, nullptr, nullptr, &env, &execPark}};
  RuntimeSurface surface{&ctx, {actions, 1}, &observer};

  FiberScheduler scheduler(image, surface, pools.arena,
                           wendoo::test::withMaxHandles(wendoo::test::kDeviceProfileCaps, 1));
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  for (uint32_t t = 1; t < kHandleBackpressureFaultRounds; t++) {
    REQUIRE(brain.think(static_cast<float>(t) * 16.0f).isOk());
    REQUIRE(observer.faults == 0);
  }

  REQUIRE(brain.think(static_cast<float>(kHandleBackpressureFaultRounds) * 16.0f).isOk());

  CHECK(observer.faults == 1);
  CHECK(observer.lastError == ErrorCode::StackOverflow);
  // The fiber holding the only slot is untouched.
  CHECK(env.dispatches == 1u);
}
