#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/platform/span.h"
#include "core/runtime/core-host-actions.h"
#include "core/runtime/error-code.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/host-action.h"
#include "core/runtime/result.h"
#include "core/runtime/value.h"

namespace wendoo::test {

/**
 * Numeric device-profile id the shared conformance corpus is minted under; the
 * value every corpus binary's envelope and every committed trace header
 * carries. Mirrors `CONFORMANCE_PROFILE_ID` in
 * external/wendoo-lang/packages/conformance/src/profile.ts.
 */
inline constexpr uint32_t kConformanceProfileId = 1000;

/**
 * Stable host-action ids of the conformance host profile, allocated from the
 * target partition core reserves. Mirrors `ConformanceHostActions` in
 * external/wendoo-lang/packages/conformance/src/profile.ts; the ids are
 * wire-stable, so never renumber or reuse one.
 */
namespace ConformanceHostActions {
/** Synchronous sensor returning its argument unchanged. */
inline constexpr HostActionIds Echo{TARGET_ACTION_ID_BASE, TARGET_FUNC_ID_BASE};
/** Synchronous actuator returning void; the corpus's observable output channel. */
inline constexpr HostActionIds Emit{TARGET_ACTION_ID_BASE + 1, TARGET_FUNC_ID_BASE + 1};
/** Asynchronous actuator whose handle resolves to its value argument after a stated tick count. */
inline constexpr HostActionIds DeferEcho{TARGET_ACTION_ID_BASE + 2, TARGET_FUNC_ID_BASE + 2};
/** Asynchronous actuator whose handle rejects with `HostError` after a stated tick count. */
inline constexpr HostActionIds DeferFail{TARGET_ACTION_ID_BASE + 3, TARGET_FUNC_ID_BASE + 3};
/** Synchronous actuator that faults its calling fiber with `ScriptError`. */
inline constexpr HostActionIds Fault{TARGET_ACTION_ID_BASE + 4, TARGET_FUNC_ID_BASE + 4};
/** Presence-gated synchronous sensor delivering a value on the thinks its period divides. */
inline constexpr HostActionIds Signal{TARGET_ACTION_ID_BASE + 5, TARGET_FUNC_ID_BASE + 5};
/** Synchronous sensor returning its call site's read count since its page was activated. */
inline constexpr HostActionIds Counter{TARGET_ACTION_ID_BASE + 6, TARGET_FUNC_ID_BASE + 6};
/** Asynchronous actuator whose handle is cancelled after a stated tick count. */
inline constexpr HostActionIds DeferCancel{TARGET_ACTION_ID_BASE + 7, TARGET_FUNC_ID_BASE + 7};
} // namespace ConformanceHostActions

/** Number of conformance host-action bindings the profile registers. */
inline constexpr uint32_t kConformanceHostActionBindingCount = 8;

/** Arg-buffer slot of the value argument of `echo`, `emit`, and `defer echo`. */
inline constexpr uint32_t kConformanceValueSlot = 0;

/** Arg-buffer slot of the whole-tick count of `defer echo`. */
inline constexpr uint32_t kDeferEchoTicksSlot = 1;

/** Arg-buffer slot of the whole-tick count of `defer fail`. */
inline constexpr uint32_t kDeferFailTicksSlot = 0;

/** Arg-buffer slot of the whole-tick period of `signal`. */
inline constexpr uint32_t kSignalPeriodSlot = 0;

/** Arg-buffer slot of the whole-tick count of `defer cancel`. */
inline constexpr uint32_t kDeferCancelTicksSlot = 0;

/** Count `counter` holds at a just-reset call site; its first read returns one more. */
inline constexpr mc_number_t kCounterStart = 0;

/** Whole-tick count a deferred call waits, or a signal's period, when the argument carries none. */
inline constexpr uint32_t kDefaultWholeTicks = 1;

/** Value `signal` delivers on a tick it is present: falsy, so only a presence gate fires on it. */
inline constexpr mc_number_t kSignalValue = 0;

/** Error code `defer fail` rejects its handle with. */
inline constexpr ErrorCode kDeferFailCode = ErrorCode::HostError;

/** How a deferred call settles the handle it was dispatched on. */
enum class DeferredOutcome {
  /** Resolve the handle to the value the call captured. */
  Resolve,
  /** Reject the handle with {@link kDeferFailCode}. */
  Reject,
  /** Cancel the handle. */
  Cancel,
};

/**
 * Deterministic world the conformance host actions run against: the pending
 * deferred settlements and nothing else. It owns no clock and no random stream,
 * so two runs of one program over one schedule observe the same world.
 *
 * Register it as the `hostData` of the profile's bindings and call
 * {@link settleDue} with the ordinal of the think that is about to run, before
 * running it. Mirrors `ConformanceWorld` in
 * external/wendoo-lang/packages/conformance/src/profile.ts.
 */
class ConformanceWorld {
public:
  /**
   * Settles every deferred call due at or before `tick`, oldest dispatch first,
   * each by the outcome it was recorded with.
   *
   * @param tick - 1-based ordinal of the think about to run.
   */
  void settleDue(uint32_t tick) {
    std::vector<Pending> due;
    std::vector<Pending> rest;
    for (const Pending& entry : pending_) {
      if (entry.dueTick <= tick) {
        due.push_back(entry);
      } else {
        rest.push_back(entry);
      }
    }
    pending_ = rest;
    for (const Pending& entry : due) {
      switch (entry.outcome) {
      case DeferredOutcome::Reject:
        entry.handle.reject(kDeferFailCode);
        break;
      case DeferredOutcome::Cancel:
        entry.handle.cancel();
        break;
      case DeferredOutcome::Resolve:
        entry.handle.resolve(entry.value);
        break;
      }
    }
  }

  /**
   * Records a settlement due `ticks` thinks after the think of ordinal
   * `dispatchTick`.
   *
   * @param dispatchTick - Ordinal of the think the deferred call was dispatched on.
   * @param ticks - Whole ticks between the dispatch and the settlement.
   * @param handle - Handle the deferred call was dispatched on.
   * @param value - Value a resolving settlement carries.
   * @param outcome - How the settlement settles the handle.
   */
  void defer(uint32_t dispatchTick, uint32_t ticks, AsyncHandle handle, const Value& value,
             DeferredOutcome outcome) {
    pending_.push_back(Pending{dispatchTick + ticks, handle, value, outcome});
  }

private:
  struct Pending {
    uint32_t dueTick;
    AsyncHandle handle;
    Value value;
    DeferredOutcome outcome;
  };

  std::vector<Pending> pending_;
};

namespace conformance_detail {

/** Whole-tick count carried by the argument at `slot`, or the default when it carries none. */
inline uint32_t wholeTicksArg(Span<const Value> args, uint32_t slot) {
  if (slot >= args.size() || !args[slot].isNumber()) {
    return kDefaultWholeTicks;
  }
  return static_cast<uint32_t>(args[slot].asNumber());
}

/** The value argument at `slot`, or nil when the buffer has no such slot. */
inline Value valueArg(Span<const Value> args, uint32_t slot) {
  return slot >= args.size() ? kNilValue : args[slot];
}

inline Value execEcho(void*, ExecutionContext&, Span<const Value> args) {
  return valueArg(args, kConformanceValueSlot);
}

inline Value execEmit(void*, ExecutionContext&, Span<const Value>) { return kVoidValue; }

inline Status execDeferEcho(void* hostData, ExecutionContext& ctx, Span<const Value> args,
                            AsyncHandle handle) {
  ConformanceWorld* world = static_cast<ConformanceWorld*>(hostData);
  const Value value = valueArg(args, kConformanceValueSlot);
  if (world == nullptr) {
    handle.resolve(value);
    return Status::ok();
  }
  world->defer(ctx.currentTick, wholeTicksArg(args, kDeferEchoTicksSlot), handle, value,
               DeferredOutcome::Resolve);
  return Status::ok();
}

inline Status execDeferFail(void* hostData, ExecutionContext& ctx, Span<const Value> args,
                            AsyncHandle handle) {
  ConformanceWorld* world = static_cast<ConformanceWorld*>(hostData);
  if (world == nullptr) {
    handle.reject(kDeferFailCode);
    return Status::ok();
  }
  world->defer(ctx.currentTick, wholeTicksArg(args, kDeferFailTicksSlot), handle, kNilValue,
               DeferredOutcome::Reject);
  return Status::ok();
}

inline Status execDeferCancel(void* hostData, ExecutionContext& ctx, Span<const Value> args,
                              AsyncHandle handle) {
  ConformanceWorld* world = static_cast<ConformanceWorld*>(hostData);
  if (world == nullptr) {
    handle.cancel();
    return Status::ok();
  }
  world->defer(ctx.currentTick, wholeTicksArg(args, kDeferCancelTicksSlot), handle, kNilValue,
               DeferredOutcome::Cancel);
  return Status::ok();
}

inline Value execFault(void*, ExecutionContext&, Span<const Value>) {
  return Value::error(ErrorCode::ScriptError);
}

inline Value execSignal(void*, ExecutionContext& ctx, Span<const Value> args) {
  const uint32_t period = wholeTicksArg(args, kSignalPeriodSlot);
  return ctx.currentTick % period == 0 ? Value::number(kSignalValue) : kNilValue;
}

inline void counterPageEntered(void*, ExecutionContext& ctx) {
  ctx.setCallSiteState(Value::number(kCounterStart));
}

inline Value execCounter(void*, ExecutionContext& ctx, Span<const Value>) {
  const mc_number_t stored = ctx.hasCallSiteState() && ctx.callSiteState().isNumber()
                                 ? ctx.callSiteState().asNumber()
                                 : kCounterStart;
  const Value next = Value::number(stored + 1);
  ctx.setCallSiteState(next);
  return next;
}

} // namespace conformance_detail

/**
 * Builds the conformance host-action binding table over `world`, one entry per
 * profile action in registry order: echo, emit, defer echo, defer fail, fault,
 * signal, counter, defer cancel.
 * `world` must outlive every dispatch through the table.
 *
 * @param world - Deterministic world the deferred actions park their handles in.
 */
inline std::array<HostActionBinding, kConformanceHostActionBindingCount>
makeConformanceHostActionBindings(ConformanceWorld& world) {
  return {{
      {ConformanceHostActions::Echo.actionId, &conformance_detail::execEcho, nullptr, &world},
      {ConformanceHostActions::Emit.actionId, &conformance_detail::execEmit, nullptr, &world},
      {ConformanceHostActions::DeferEcho.actionId, nullptr, nullptr, &world,
       &conformance_detail::execDeferEcho},
      {ConformanceHostActions::DeferFail.actionId, nullptr, nullptr, &world,
       &conformance_detail::execDeferFail},
      {ConformanceHostActions::Fault.actionId, &conformance_detail::execFault, nullptr, &world},
      {ConformanceHostActions::Signal.actionId, &conformance_detail::execSignal, nullptr, &world},
      {ConformanceHostActions::Counter.actionId, &conformance_detail::execCounter,
       &conformance_detail::counterPageEntered, &world},
      {ConformanceHostActions::DeferCancel.actionId, nullptr, nullptr, &world,
       &conformance_detail::execDeferCancel},
  }};
}

} // namespace wendoo::test
