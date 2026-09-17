#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/platform/span.h"
#include "core/runtime/core-host-actions.h"
#include "core/runtime/core-type-atom-id.h"
#include "core/runtime/error-code.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/host-action.h"
#include "core/runtime/host-function.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/result.h"
#include "core/runtime/type-registry.h"
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
/** Asynchronous sensor whose handle resolves to its value argument after a stated tick count. */
inline constexpr HostActionIds DeferRead{TARGET_ACTION_ID_BASE + 8, TARGET_FUNC_ID_BASE + 8};
/** Synchronous actuator over a String-typed argument slot, returning its argument. */
inline constexpr HostActionIds EmitText{TARGET_ACTION_ID_BASE + 9, TARGET_FUNC_ID_BASE + 10};
/** Synchronous actuator over a Boolean-typed argument slot, returning its argument. */
inline constexpr HostActionIds EmitFlag{TARGET_ACTION_ID_BASE + 10, TARGET_FUNC_ID_BASE + 11};
/** Asynchronous inline sensor whose handle resolves to a `Point` struct reading. */
inline constexpr HostActionIds DeferPoint{TARGET_ACTION_ID_BASE + 11, TARGET_FUNC_ID_BASE + 12};
/** Asynchronous inline sensor whose handle resolves to an `Anchor` native struct value. */
inline constexpr HostActionIds DeferAnchor{TARGET_ACTION_ID_BASE + 12, TARGET_FUNC_ID_BASE + 13};
/** Asynchronous inline sensor whose handle resolves to a resolver-backed `Target` value. */
inline constexpr HostActionIds DeferTarget{TARGET_ACTION_ID_BASE + 13, TARGET_FUNC_ID_BASE + 14};
/** Synchronous actuator returning void, receiving its repeated slot's arguments as one list. */
inline constexpr HostActionIds EmitAll{TARGET_ACTION_ID_BASE + 14, TARGET_FUNC_ID_BASE + 15};
} // namespace ConformanceHostActions

/**
 * Stable funcIds of the conformance profile's operator overloads, continuing
 * the target partition offsets {@link ConformanceHostActions} allocates from.
 * Mirrors `ConformanceOperators` in
 * external/wendoo-lang/packages/conformance/src/profile.ts; the ids are
 * wire-stable, so never renumber or reuse one.
 */
namespace ConformanceOperators {
/**
 * Asynchronous infix `defer plus`, resolving to the sum of its two operands one
 * tick after its dispatch.
 */
inline constexpr uint32_t DeferAdd = TARGET_FUNC_ID_BASE + 9;
} // namespace ConformanceOperators

/**
 * Stable type-atom id of the conformance `Point` struct type. Mirrors
 * `ConformanceTypeAtomIds.Point` in
 * external/wendoo-lang/packages/conformance/src/profile.ts; wire-stable, so
 * never renumber or reuse it.
 */
inline constexpr uint32_t kConformancePointAtomId = TARGET_TYPE_ATOM_BASE;

/** Field storage slot of the `Point` struct's `x` field. Mirrors `ConformancePointField.X`. */
inline constexpr uint32_t kConformancePointFieldX = 0;

/** Field storage slot of the `Point` struct's `y` field. Mirrors `ConformancePointField.Y`. */
inline constexpr uint32_t kConformancePointFieldY = 1;

/** Field storage slot count of the `Point` struct (its highest field id + 1). */
inline constexpr uint32_t kConformancePointSlotCount = 2;

/** `x` field value of every settled `defer point` reading. Mirrors `CONFORMANCE_POINT_READING.x`.
 */
inline constexpr mc_number_t kConformancePointX = 1.5;

/** `y` field value of every settled `defer point` reading. Mirrors `CONFORMANCE_POINT_READING.y`.
 */
inline constexpr mc_number_t kConformancePointY = 2.25;

/**
 * Stable type-atom id of the conformance `Anchor` native struct type. Mirrors
 * `ConformanceTypeAtomIds.Anchor` in
 * external/wendoo-lang/packages/conformance/src/profile.ts; wire-stable, so
 * never renumber or reuse it. Native struct values of the type carry it as
 * their `Value::typeId`, following the device-struct convention: atom ids sit
 * above every program type-table index, so the value classifies as native.
 */
inline constexpr uint32_t kConformanceAnchorAtomId = TARGET_TYPE_ATOM_BASE + 1;

/**
 * Stable type-atom id of the conformance `Target` native struct type. Mirrors
 * `ConformanceTypeAtomIds.Target`; wire-stable, so never renumber or reuse
 * it. Carried as the `Value::typeId` of the type's values, like
 * {@link kConformanceAnchorAtomId}.
 */
inline constexpr uint32_t kConformanceTargetAtomId = TARGET_TYPE_ATOM_BASE + 2;

/** Field id of the `Anchor` type's `x` field. Mirrors `ConformanceAnchorField.X`. */
inline constexpr uint32_t kConformanceAnchorFieldX = 0;

/** Field id of the `Anchor` type's `y` field. Mirrors `ConformanceAnchorField.Y`. */
inline constexpr uint32_t kConformanceAnchorFieldY = 1;

/** Field id of the `Target` type's `value` field. Mirrors `ConformanceTargetField.Value`. */
inline constexpr uint32_t kConformanceTargetFieldValue = 0;

/** Starting `x` of the world's anchor host object. Mirrors `CONFORMANCE_ANCHOR_READING.x`. */
inline constexpr mc_number_t kConformanceAnchorX = 1.5;

/** Starting `y` of the world's anchor host object. Mirrors `CONFORMANCE_ANCHOR_READING.y`. */
inline constexpr mc_number_t kConformanceAnchorY = 2.25;

/** `value` of the first target resolution's object. Mirrors `CONFORMANCE_TARGET_READING.first`. */
inline constexpr mc_number_t kConformanceTargetFirstValue = 1.5;

/** `value` of every later target resolution's object. Mirrors `CONFORMANCE_TARGET_READING.second`.
 */
inline constexpr mc_number_t kConformanceTargetLaterValue = 8.5;

/** Host token of the world's one anchor object, carried as an `Anchor` value's handle. */
inline constexpr uint32_t kConformanceAnchorToken = 0;

/** Host token of an unresolved `Target` value: the lazy-resolver analog its snapshot materializes.
 */
inline constexpr uint32_t kConformanceTargetResolverToken = 0;

/** Host token of the target object the first resolution returns. */
inline constexpr uint32_t kConformanceTargetFirstToken = 1;

/** Host token of the target object every later resolution returns. */
inline constexpr uint32_t kConformanceTargetLaterToken = 2;

/**
 * Mutable host state behind the profile's two native struct types: the anchor
 * object's fields and the target resolution counter. The corpus harness
 * installs a fresh instance per replay through
 * {@link gConformanceNativeEnv}, so two runs of one program observe the same
 * state.
 */
struct ConformanceNativeEnv {
  /** `x` field of the world's one anchor host object. */
  mc_number_t anchorX = kConformanceAnchorX;
  /** `y` field of the world's one anchor host object. */
  mc_number_t anchorY = kConformanceAnchorY;
  /** How many target resolutions have run; the first returns the first object. */
  uint32_t targetResolutions = 0;
};

/**
 * The active replay's native-struct host state, read by the profile's native
 * field hooks. The corpus test installs a fresh instance before a replay and
 * clears it after. Null outside a replay; every hook reads nil, rejects the
 * write, or passes the handle through while it is null.
 */
inline ConformanceNativeEnv* gConformanceNativeEnv = nullptr;

/** Number of conformance host-action bindings the profile registers. */
inline constexpr uint32_t kConformanceHostActionBindingCount = 15;

/** Number of conformance host-function bindings the profile registers. */
inline constexpr uint32_t kConformanceHostFuncBindingCount = 1;

/**
 * Arg-buffer slot of the value argument of `echo`, `emit`, `defer echo`,
 * `defer read`, `emit text`, and `emit flag`.
 */
inline constexpr uint32_t kConformanceValueSlot = 0;

/** Arg-buffer slot of the whole-tick count of `defer echo`. */
inline constexpr uint32_t kDeferEchoTicksSlot = 1;

/** Arg-buffer slot of the whole-tick count of `defer fail`. */
inline constexpr uint32_t kDeferFailTicksSlot = 0;

/** Arg-buffer slot of the whole-tick period of `signal`. */
inline constexpr uint32_t kSignalPeriodSlot = 0;

/** Arg-buffer slot of the whole-tick count of `defer cancel`. */
inline constexpr uint32_t kDeferCancelTicksSlot = 0;

/** Arg-buffer slot of the whole-tick count of `defer read`. */
inline constexpr uint32_t kDeferReadTicksSlot = 1;

/** Arg-buffer slot of the left operand of a binary operator overload. */
inline constexpr uint32_t kOperatorLhsSlot = 0;

/** Arg-buffer slot of the right operand of a binary operator overload. */
inline constexpr uint32_t kOperatorRhsSlot = 1;

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
  /** Construct the `Point` reading at settle time and resolve the handle to it. */
  ResolvePoint,
};

class ConformanceWorld;

/**
 * The world, heap, type registry, and GC roots the `defer point` binding
 * reaches: the world to park the settlement in, and the heap, registry, and
 * roots to construct the managed `Point` reading at settle time. The caller
 * fills every field before the first settlement is due.
 */
struct ConformancePointEnv {
  ConformanceWorld* world = nullptr;
  ManagedHeap* heap = nullptr;
  const TypeRegistry* types = nullptr;
  GcRoots* roots = nullptr;
};

/**
 * Builds one `Point` struct reading: resolves the atom's program type index,
 * allocates the managed struct, and writes the two number field slots.
 * Returns nil when the env is incomplete, the program's type table carries no
 * `Point` atom entry, or the heap cannot back the allocation.
 */
inline Value buildConformancePoint(const ConformancePointEnv& env) {
  if (env.heap == nullptr || env.types == nullptr) {
    return kNilValue;
  }
  const uint32_t typeIdx = env.types->findAtomType(kConformancePointAtomId);
  if (typeIdx == kNoTypeIdx) {
    return kNilValue;
  }
  Value out;
  if (!env.heap->newStruct(typeIdx, kConformancePointSlotCount, env.roots, out)) {
    return kNilValue;
  }
  StructObject* obj = env.heap->structOf(out);
  env.heap->structSet(obj, kConformancePointFieldX, Value::number(kConformancePointX));
  env.heap->structSet(obj, kConformancePointFieldY, Value::number(kConformancePointY));
  return out;
}

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
      case DeferredOutcome::ResolvePoint:
        entry.handle.resolve(buildConformancePoint(*entry.pointEnv));
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

  /**
   * Records a `Point`-resolving settlement due `ticks` thinks after the think
   * of ordinal `dispatchTick`. The reading is built by
   * {@link buildConformancePoint} at settle time. `env` must outlive the
   * settlement.
   *
   * @param dispatchTick - Ordinal of the think the deferred call was dispatched on.
   * @param ticks - Whole ticks between the dispatch and the settlement.
   * @param handle - Handle the deferred call was dispatched on.
   * @param env - Heap, type registry, and roots the settle-time construction uses.
   */
  void deferPoint(uint32_t dispatchTick, uint32_t ticks, AsyncHandle handle,
                  const ConformancePointEnv& env) {
    pending_.push_back(
        Pending{dispatchTick + ticks, handle, kNilValue, DeferredOutcome::ResolvePoint, &env});
  }

private:
  struct Pending {
    uint32_t dueTick;
    AsyncHandle handle;
    Value value;
    DeferredOutcome outcome;
    /** Construction env of a `ResolvePoint` settlement; null for every other outcome. */
    const ConformancePointEnv* pointEnv = nullptr;
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

inline Status execDeferRead(void* hostData, ExecutionContext& ctx, Span<const Value> args,
                            AsyncHandle handle) {
  ConformanceWorld* world = static_cast<ConformanceWorld*>(hostData);
  const Value value = valueArg(args, kConformanceValueSlot);
  if (world == nullptr) {
    handle.resolve(value);
    return Status::ok();
  }
  world->defer(ctx.currentTick, wholeTicksArg(args, kDeferReadTicksSlot), handle, value,
               DeferredOutcome::Resolve);
  return Status::ok();
}

/** Whether `value` carries a number that is not a NaN. */
inline bool isValidNumber(const Value& value) {
  return value.isNumber() && value.asNumber() == value.asNumber();
}

/** The sum of the two operand slots; nil when either operand or the sum is not a number. */
inline Value operandSum(Span<const Value> args) {
  const Value lhs = valueArg(args, kOperatorLhsSlot);
  const Value rhs = valueArg(args, kOperatorRhsSlot);
  if (!isValidNumber(lhs) || !isValidNumber(rhs)) {
    return kNilValue;
  }
  const mc_number_t sum = lhs.asNumber() + rhs.asNumber();
  return sum == sum ? Value::number(sum) : kNilValue;
}

inline Status execDeferAdd(void* hostData, ExecutionContext& ctx, Span<const Value> args,
                           AsyncHandle handle) {
  ConformanceWorld* world = static_cast<ConformanceWorld*>(hostData);
  const Value sum = operandSum(args);
  if (world == nullptr) {
    handle.resolve(sum);
    return Status::ok();
  }
  world->defer(ctx.currentTick, kDefaultWholeTicks, handle, sum, DeferredOutcome::Resolve);
  return Status::ok();
}

inline Status execDeferPoint(void* hostData, ExecutionContext& ctx, Span<const Value>,
                             AsyncHandle handle) {
  ConformancePointEnv* env = static_cast<ConformancePointEnv*>(hostData);
  if (env == nullptr || env->world == nullptr) {
    handle.resolve(env == nullptr ? kNilValue : buildConformancePoint(*env));
    return Status::ok();
  }
  env->world->deferPoint(ctx.currentTick, kDefaultWholeTicks, handle, *env);
  return Status::ok();
}

/** Reads an `Anchor` field off the env's anchor host object; nil without an env or a declared
 * field. */
inline Value anchorFieldGetter(const Value& /*source*/, uint32_t fieldId) {
  const ConformanceNativeEnv* env = gConformanceNativeEnv;
  if (env == nullptr) {
    return kNilValue;
  }
  if (fieldId == kConformanceAnchorFieldX) {
    return Value::number(env->anchorX);
  }
  if (fieldId == kConformanceAnchorFieldY) {
    return Value::number(env->anchorY);
  }
  return kNilValue;
}

/** Writes an `Anchor` field of the env's anchor host object; rejects without an env, a number, or a
 * declared field. */
inline bool anchorFieldSetter(const Value& /*source*/, uint32_t fieldId, const Value& value) {
  ConformanceNativeEnv* env = gConformanceNativeEnv;
  if (env == nullptr || !value.isNumber()) {
    return false;
  }
  if (fieldId == kConformanceAnchorFieldX) {
    env->anchorX = value.asNumber();
    return true;
  }
  if (fieldId == kConformanceAnchorFieldY) {
    env->anchorY = value.asNumber();
    return true;
  }
  return false;
}

/** Resolves the current target object's token, counting the call: first the first object, then the
 * later one. */
inline uint32_t resolveTargetToken(ConformanceNativeEnv& env) {
  env.targetResolutions += 1;
  return env.targetResolutions == 1 ? kConformanceTargetFirstToken : kConformanceTargetLaterToken;
}

/**
 * Reads a `Target` field through the object the value designates, resolving
 * the lazy resolver token on every read of an unassigned value; nil without
 * an env or a declared field.
 */
inline Value targetFieldGetter(const Value& source, uint32_t fieldId) {
  ConformanceNativeEnv* env = gConformanceNativeEnv;
  if (env == nullptr || fieldId != kConformanceTargetFieldValue) {
    return kNilValue;
  }
  uint32_t token = source.structHandle();
  if (token == kConformanceTargetResolverToken) {
    token = resolveTargetToken(*env);
  }
  if (token == kConformanceTargetFirstToken) {
    return Value::number(kConformanceTargetFirstValue);
  }
  if (token == kConformanceTargetLaterToken) {
    return Value::number(kConformanceTargetLaterValue);
  }
  return kNilValue;
}

/**
 * Snapshot hook of the `Target` type: a deep copy materializes the lazy
 * resolver token to the concrete object token it resolves to; a value already
 * carrying an object token passes through unchanged.
 */
inline Value targetSnapshot(const Value& source) {
  ConformanceNativeEnv* env = gConformanceNativeEnv;
  if (env == nullptr || source.structHandle() != kConformanceTargetResolverToken) {
    return source;
  }
  return Value::structValue(source.typeId(), resolveTargetToken(*env));
}

inline Status execDeferAnchor(void* hostData, ExecutionContext& ctx, Span<const Value>,
                              AsyncHandle handle) {
  ConformanceWorld* world = static_cast<ConformanceWorld*>(hostData);
  const Value reading = Value::structValue(kConformanceAnchorAtomId, kConformanceAnchorToken);
  if (world == nullptr) {
    handle.resolve(reading);
    return Status::ok();
  }
  world->defer(ctx.currentTick, kDefaultWholeTicks, handle, reading, DeferredOutcome::Resolve);
  return Status::ok();
}

inline Status execDeferTarget(void* hostData, ExecutionContext& ctx, Span<const Value>,
                              AsyncHandle handle) {
  ConformanceWorld* world = static_cast<ConformanceWorld*>(hostData);
  const Value reading =
      Value::structValue(kConformanceTargetAtomId, kConformanceTargetResolverToken);
  if (world == nullptr) {
    handle.resolve(reading);
    return Status::ok();
  }
  world->defer(ctx.currentTick, kDefaultWholeTicks, handle, reading, DeferredOutcome::Resolve);
  return Status::ok();
}

inline Value execEmitText(void*, ExecutionContext&, Span<const Value> args) {
  return valueArg(args, kConformanceValueSlot);
}

inline Value execEmitFlag(void*, ExecutionContext&, Span<const Value> args) {
  return valueArg(args, kConformanceValueSlot);
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
 * signal, counter, defer cancel, defer read, emit text, emit flag, defer
 * point, defer anchor, defer target, emit all. `world` and `pointEnv` must outlive every
 * dispatch through the table, and the caller fills `pointEnv`'s fields before
 * the first `defer point` settlement is due.
 *
 * @param world - Deterministic world the deferred actions park their handles in.
 * @param pointEnv - Construction env the `defer point` binding settles through.
 */
inline std::array<HostActionBinding, kConformanceHostActionBindingCount>
makeConformanceHostActionBindings(ConformanceWorld& world, ConformancePointEnv& pointEnv) {
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
      {ConformanceHostActions::DeferRead.actionId, nullptr, nullptr, &world,
       &conformance_detail::execDeferRead},
      {ConformanceHostActions::EmitText.actionId, &conformance_detail::execEmitText, nullptr,
       &world},
      {ConformanceHostActions::EmitFlag.actionId, &conformance_detail::execEmitFlag, nullptr,
       &world},
      {ConformanceHostActions::DeferPoint.actionId, nullptr, nullptr, &pointEnv,
       &conformance_detail::execDeferPoint},
      {ConformanceHostActions::DeferAnchor.actionId, nullptr, nullptr, &world,
       &conformance_detail::execDeferAnchor},
      {ConformanceHostActions::DeferTarget.actionId, nullptr, nullptr, &world,
       &conformance_detail::execDeferTarget},
      {ConformanceHostActions::EmitAll.actionId, &conformance_detail::execEmit, nullptr, &world},
  }};
}

/** Number of conformance native-struct field-accessor bindings. */
inline constexpr uint32_t kConformanceNativeStructBindingCount = 2;

/**
 * Builds the conformance native-struct binding table: the `Anchor` type's
 * field getter and setter, and the `Target` type's field getter and deep-copy
 * snapshot. The bindings key on the stable type-atom ids the profile's native
 * values carry as their typeId. The returned array must outlive any registry
 * it is installed into, and the hooks read the replay's
 * {@link gConformanceNativeEnv}.
 */
inline std::array<NativeStructTypeBinding, kConformanceNativeStructBindingCount>
makeConformanceNativeStructBindings() {
  return {{
      {kConformanceAnchorAtomId, &conformance_detail::anchorFieldGetter,
       &conformance_detail::anchorFieldSetter, nullptr},
      {kConformanceTargetAtomId, &conformance_detail::targetFieldGetter, nullptr,
       &conformance_detail::targetSnapshot},
  }};
}

/** Number of conformance registered (atom) struct slot-count entries. */
inline constexpr uint32_t kConformanceRegisteredStructCount = 1;

/**
 * Builds the conformance registered (atom) struct slot-count table: the
 * `Point` struct is the profile's one atom struct type. Installed on a
 * {@link TypeRegistry} so struct allocation can size a `Point` and GC tracing
 * classifies `Point` values as managed. The returned array must outlive any
 * registry it is installed into.
 */
inline std::array<RegisteredStructSlotCount, kConformanceRegisteredStructCount>
makeConformanceRegisteredStructSlotCounts() {
  return {{
      {kConformancePointAtomId, kConformancePointSlotCount},
  }};
}

/**
 * Builds the conformance host-function binding table over `world`, one entry
 * per profile operator overload: `defer plus`. `world` must outlive every
 * dispatch through the table.
 *
 * @param world - Deterministic world the deferred overloads park their handles in.
 */
inline std::array<TargetHostFuncBinding, kConformanceHostFuncBindingCount>
makeConformanceHostFuncBindings(ConformanceWorld& world) {
  return {{
      {ConformanceOperators::DeferAdd, nullptr, &world, &conformance_detail::execDeferAdd},
  }};
}

} // namespace wendoo::test
