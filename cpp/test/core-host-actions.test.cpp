#include "doctest/doctest.h"

#include "core/runtime/core-host-actions.h"
#include "core/runtime/core-host-functions.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/host-action.h"
#include "core/runtime/host-actions/core-host-action-bindings.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/mc-number.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/value.h"

#include <cstdint>
#include <iterator>
#include <vector>

using wendoo::CoreFuncId;
using wendoo::CoreHostActionEnv;
using wendoo::ExecutionContext;
using wendoo::findHostActionById;
using wendoo::GcMarker;
using wendoo::GcRoots;
using wendoo::HostActionBinding;
using wendoo::kCoreHostActions;
using wendoo::makeCoreHostActionBindings;
using wendoo::ManagedHeap;
using wendoo::mc_number_t;
using wendoo::RegionArena;
using wendoo::Span;
using wendoo::TARGET_ACTION_ID_BASE;
using wendoo::Value;
using wendoo::VmRng;
namespace CoreHostActions = wendoo::CoreHostActions;

TEST_CASE("action partition constant matches the TS declaration") {
  CHECK(TARGET_ACTION_ID_BASE == 1024);
}

TEST_CASE("core host-action ids are wire-stable") {
  CHECK(CoreHostActions::SwitchPage.actionId == 0);
  CHECK(CoreHostActions::RestartPage.actionId == 1);
  CHECK(CoreHostActions::Yield.actionId == 2);
  CHECK(CoreHostActions::Random.actionId == 3);
  CHECK(CoreHostActions::OnPageEntered.actionId == 4);
  CHECK(CoreHostActions::Timeout.actionId == 5);
  CHECK(CoreHostActions::CurrentPage.actionId == 6);
  CHECK(CoreHostActions::PreviousPage.actionId == 7);
  CHECK(CoreHostActions::Otherwise.actionId == 8);
  CHECK(CoreHostActions::RuleTrigger.actionId == 9);
}

TEST_CASE("core host-action fnIds reference the declared core funcIds") {
  CHECK(CoreHostActions::SwitchPage.fnId == static_cast<uint32_t>(CoreFuncId::ActuatorSwitchPage));
  CHECK(CoreHostActions::SwitchPage.fnId == 57);
  CHECK(CoreHostActions::RestartPage.fnId == 58);
  CHECK(CoreHostActions::Yield.fnId == 59);
  CHECK(CoreHostActions::Random.fnId == 52);
  CHECK(CoreHostActions::OnPageEntered.fnId == 53);
  CHECK(CoreHostActions::Timeout.fnId == 54);
  CHECK(CoreHostActions::CurrentPage.fnId == 55);
  CHECK(CoreHostActions::PreviousPage.fnId == 56);
  CHECK(CoreHostActions::Otherwise.fnId == 106);
  CHECK(CoreHostActions::RuleTrigger.fnId == static_cast<uint32_t>(CoreFuncId::SensorRuleTrigger));
  CHECK(CoreHostActions::RuleTrigger.fnId == 107);
}

TEST_CASE("record table covers every action densely in action-id order") {
  REQUIRE(std::size(kCoreHostActions) == 10);
  for (uint32_t i = 0; i < std::size(kCoreHostActions); i++) {
    CHECK(kCoreHostActions[i].actionId == i);
    CHECK(kCoreHostActions[i].actionId < TARGET_ACTION_ID_BASE);
  }
}

namespace {

/** Roots covering one execution context's per-callsite host-state slots. */
struct CtxRoots : GcRoots {
  ExecutionContext* ctx = nullptr;

  void enumerateRoots(GcMarker& marker) override {
    for (size_t i = 0; i < ctx->callSiteStates.size(); i++) {
      if (ctx->callSiteStatePresent[i]) {
        marker.mark(ctx->callSiteStates[i]);
      }
    }
  }
};

/** Drives the timeout sensor over `tickCount` thinks at `tickMs` each from base
 *  uptime `baseMs`, collecting the heap each tick (as the device host loop does),
 *  and returns the 1-based ticks on which it fired. */
std::vector<int> runTimeoutTicks(mc_number_t baseMs, mc_number_t tickMs, int tickCount) {
  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  ManagedHeap heap(arena);

  ExecutionContext ctx;
  std::vector<uint8_t> ctxStorage(4 * 1024);
  RegionArena ctxArena(Span<uint8_t>(ctxStorage.data(), ctxStorage.size()));
  REQUIRE(ctx.bindSlots(ctxArena, 0, 1, 0));
  ctx.currentCallSiteId = 0;

  CtxRoots roots;
  roots.ctx = &ctx;
  CoreHostActionEnv env;
  env.heap = &heap;
  env.roots = &roots;

  auto bindings = makeCoreHostActionBindings(env);
  const HostActionBinding* timeout =
      findHostActionById({bindings.data(), bindings.size()}, CoreHostActions::Timeout.actionId);
  REQUIRE(timeout != nullptr);
  REQUIRE(timeout->onPageEntered != nullptr);
  REQUIRE(timeout->execSync != nullptr);

  timeout->onPageEntered(timeout->hostData, ctx);

  std::vector<int> fired;
  for (int i = 0; i < tickCount; i++) {
    ctx.time = baseMs + static_cast<mc_number_t>(i + 1) * tickMs;
    ctx.currentTick++;
    const Value result = timeout->execSync(timeout->hostData, ctx, Span<const Value>{});
    if (result.isBoolean() && result.asBoolean()) {
      fired.push_back(i + 1);
    }
    heap.collect(roots);
  }
  return fired;
}

} // namespace

TEST_CASE("the timeout sensor fires at the device tick cadence (16 ms thinks)") {
  // Mirrors the firmware host loop: 16 ms thinks from boot. A default 1 s timeout
  // should fire once each second (first cross of the 1000 ms mark), never on entry.
  const std::vector<int> fired = runTimeoutTicks(0, 16, 130);
  REQUIRE(fired.size() == 2);
  CHECK(fired[0] == 64);
  CHECK(fired[1] == 127);
}

TEST_CASE("the random sensor yields the surface rng stream") {
  VmRng rng;
  VmRng reference;
  CoreHostActionEnv env;
  env.rng = &rng;
  auto bindings = makeCoreHostActionBindings(env);
  const HostActionBinding* random =
      findHostActionById({bindings.data(), bindings.size()}, CoreHostActions::Random.actionId);
  REQUIRE(random != nullptr);

  ExecutionContext ctx;
  for (int i = 0; i < 4; i++) {
    const Value result = random->execSync(random->hostData, ctx, Span<const Value>{});
    REQUIRE(result.isNumber());
    CHECK(result.asNumber() == reference.next());
  }
}
