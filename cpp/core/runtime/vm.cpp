#include "core/runtime/vm.h"

#include <cstring>

#include "core/runtime/async-action-spawner.h"
#include "core/runtime/child-rule-spawner.h"
#include "core/runtime/core-func-id.h"
#include "core/runtime/core-host-functions.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/rule-structure.h"
#include "core/runtime/rule-var-store.h"
#include "core/runtime/stack-region.h"
#include "core/runtime/type-registry.h"
#include "core/runtime/when-result.h"

namespace wendoo {

namespace {

/**
 * Grows the operand stack to hold at least `needed` slots, returning false
 * when it cannot (no allocator, or the arena is exhausted). The caller checks
 * the {@link ExecutionState::stackLimit} cap guard first, so `needed` never
 * exceeds the cap. A grow re-derives `state.stack`.
 */
bool ensureStackCapacity(ExecutionState& state, uint32_t needed) {
  if (needed <= state.stackCapacity) {
    return true;
  }
  return state.allocator != nullptr &&
         state.allocator->grow(&state.stack, &state.stackCapacity, state.stackDepth, needed,
                               state.stackLimit);
}

/** Grows the locals region to hold at least `needed` slots. See {@link ensureStackCapacity}. */
bool ensureLocalsCapacity(ExecutionState& state, uint32_t needed) {
  if (needed <= state.localsCapacity) {
    return true;
  }
  return state.allocator != nullptr &&
         state.allocator->grow(&state.locals, &state.localsCapacity, state.localsDepth, needed,
                               state.localsLimit);
}

/** Grows the frame stack to hold at least `needed` frames. See {@link ensureStackCapacity}. */
bool ensureFrameCapacity(ExecutionState& state, uint32_t needed) {
  if (needed <= state.frameCapacity) {
    return true;
  }
  return state.allocator != nullptr &&
         state.allocator->grow(&state.frames, &state.frameCapacity, state.frameDepth, needed,
                               state.frameLimit);
}

/** Grows the handler stack to hold at least `needed` handlers. See {@link ensureStackCapacity}. */
bool ensureHandlerCapacity(ExecutionState& state, uint32_t needed) {
  if (needed <= state.handlerCapacity) {
    return true;
  }
  return state.allocator != nullptr &&
         state.allocator->grow(&state.handlers, &state.handlerCapacity, state.handlerDepth, needed,
                               state.handlerLimit);
}

/**
 * Floors a brain number to an integer container index. Returns false for
 * non-finite values or magnitudes outside the int32 range; callers treat a
 * false result as an out-of-range index. Container sizes are bounded by the
 * region, far inside int32, so a value outside that range is always out of
 * bounds.
 */
bool numberToIndex(mc_number_t value, int32_t& out) {
  if (value != value) {
    return false; // NaN
  }
  // The bounds are exact powers of two in f32; `< 2^31` keeps the cast in range.
  if (!(value >= -2147483648.0f && value < 2147483648.0f)) {
    return false; // +/-inf or beyond int32
  }
  int32_t truncated = static_cast<int32_t>(value); // truncates toward zero
  if (static_cast<mc_number_t>(truncated) > value) {
    truncated -= 1; // floor toward negative infinity
  }
  out = truncated;
  return true;
}

/**
 * Converts a popped map-key value to a {@link MapKey}. Returns false when the
 * value is neither a number nor a string, mirroring the TS key-kind fault.
 */
bool valueToMapKey(const Value& value, MapKey& out) {
  if (value.isNumber()) {
    out = MapKey{true, false, value.asNumber(), 0};
    return true;
  }
  if (value.isString()) {
    out = MapKey{false, value.isManagedString(), 0.0f, value.stringRef() & kStringRefIndexMask};
    return true;
  }
  return false;
}

/**
 * Resolves the bytes of string `value`: a managed string reads from `heap`, a
 * borrowed string reads from `program`'s string table. Returns false when the
 * borrowed index is out of range or the heap cannot back a managed string.
 */
bool stringValueBytes(const Value& value, const ProgramImage& program, const ManagedHeap& heap,
                      const char*& bytes, uint32_t& length) {
  if (value.isManagedString()) {
    return heap.stringContent(value, bytes, length);
  }
  const uint32_t index = value.borrowedStringIndex();
  if (index >= program.strings.size()) {
    return false;
  }
  const StringRef& ref = program.strings[index];
  bytes = reinterpret_cast<const char*>(program.stringData.data()) + ref.offset;
  length = ref.length;
  return true;
}

/**
 * Reads struct field `fieldId` of `source`: dispatches to the type's native
 * getter when one is registered, else reads the managed slab slot. Mirrors
 * `readStructFieldById` in external/wendoo-lang/.../vm.ts. Requires a
 * non-null `surface.heap`.
 */
Value readStructFieldById(const RuntimeSurface& surface, const Value& source, uint32_t fieldId) {
  if (surface.types != nullptr) {
    const NativeStructFieldGetter getter = surface.types->nativeStructGetter(source.typeId());
    if (getter != nullptr) {
      return getter(source, fieldId);
    }
  }
  return surface.heap->structGet(surface.heap->structOf(source), fieldId);
}

/**
 * Writes struct field `fieldId` of `source` as a pure store: dispatches to the
 * type's native setter when one is registered (returning its accept/reject),
 * else writes the managed slab slot. Mirrors `writeStructFieldById` in
 * external/wendoo-lang/.../vm.ts. Requires a non-null `surface.heap`.
 */
bool writeStructFieldById(const RuntimeSurface& surface, const Value& source, uint32_t fieldId,
                          const Value& value) {
  if (surface.types != nullptr) {
    const NativeStructFieldSetter setter = surface.types->nativeStructSetter(source.typeId());
    if (setter != nullptr) {
      return setter(source, fieldId, value);
    }
  }
  surface.heap->structSet(surface.heap->structOf(source), fieldId, value);
  return true;
}

/**
 * Push `value`; false when the operand stack is at its cap or cannot grow.
 * Taken by value: a grow can relocate the stack, so a by-reference argument
 * aliasing a stack slot (e.g. from `DUP`) would dangle.
 */
bool pushValue(ExecutionState& state, Value value) {
  if (state.stackDepth >= state.stackLimit) {
    return false;
  }
  if (state.stackDepth >= state.stackCapacity &&
      !ensureStackCapacity(state, state.stackDepth + 1)) {
    return false;
  }
  state.stack[state.stackDepth++] = value;
  return true;
}

/** Pop the top of stack into `out`; false when the operand stack is empty. */
bool popValue(ExecutionState& state, Value& out) {
  if (state.stackDepth == 0) {
    return false;
  }
  out = state.stack[--state.stackDepth];
  return true;
}

/**
 * Throws fault classifier `code` into `state` through the innermost active
 * handler. Returns true and positions execution at the catch target when a
 * handler catches it; otherwise returns false and writes the escaping fault to
 * `out` (located at `siteFunc`/`sitePc`), which the caller returns. Mirrors
 * `throwValue` in external/wendoo-lang/packages/core/src/runtime/vm.ts.
 */
bool throwError(ExecutionState& state, ErrorCode code, uint32_t siteFunc, uint32_t sitePc,
                RunResult& out) {
  if (state.handlerDepth == 0) {
    out = RunResult::fault(code, siteFunc, sitePc);
    return false;
  }
  // Unwind to the innermost handler: truncate frames and the operand stack to
  // its recorded depths, then re-derive localsDepth from the frame that remains
  // on top. Handlers carry no Values, so no root is touched.
  const Handler handler = state.handlers[--state.handlerDepth];
  state.frameDepth = handler.frameIndex;
  state.localsDepth = state.frameDepth == 0 ? 0
                                            : state.frames[state.frameDepth - 1].localsOffset +
                                                  state.frames[state.frameDepth - 1].localsCount;
  if (state.stackDepth > handler.stackHeight) {
    state.stackDepth = handler.stackHeight;
  }
  if (!pushValue(state, Value::error(code))) {
    out = RunResult::fault(ErrorCode::StackOverflow, siteFunc, sitePc);
    return false;
  }
  if (state.frameDepth == 0) {
    out = RunResult::fault(ErrorCode::ScriptError, kNoFuncId, 0);
    return false;
  }
  state.frames[state.frameDepth - 1].pc = handler.catchTarget;
  return true;
}

/** Apply a signed relative offset (two's-complement bit pattern) to a pc. */
uint32_t addRel(uint32_t pc, uint32_t relBits) {
  return static_cast<uint32_t>(static_cast<int32_t>(pc) + static_cast<int32_t>(relBits));
}

/**
 * Commits a call frame for `calleeId`. The callee's args are the topmost `argc`
 * operands (arg0 deepest); they are moved into the callee's locals 0..argc-1 and
 * the operand stack is truncated, dropping the args plus `extraDrop` slots below
 * them (1 for the indirect-call function reference, 0 for a direct call).
 * `captures` becomes the callee frame's capture handle. When `injectCtx` is set
 * and the callee declares a context injection, a native context struct is
 * synthesized into local 0 and the bytecode args follow it, so `argc` is one
 * short of `numParams`. With `exactArity`, `argc` must equal the stack-supplied
 * parameter count (`numParams`, less the injected context); otherwise surplus
 * args are dropped and missing args are nil-padded. On success the caller
 * frame's pc is advanced past the call instruction. Returns false with `err`
 * set on an out-of-bounds funcId, arity mismatch, or frame/locals overflow; on
 * failure the caller pc is left at the call instruction. The caller must have
 * checked that `argc + extraDrop <= stackDepth`.
 */
bool pushCallFrame(ExecutionState& state, const ProgramImage& program, uint32_t calleeId,
                   uint32_t argc, uint32_t captures, bool exactArity, uint32_t extraDrop,
                   bool hasActionBinding, const ActionFrameBinding& actionBinding, bool injectCtx,
                   ErrorCode& err) {
  if (calleeId >= program.functions.size()) {
    err = ErrorCode::ScriptError;
    return false;
  }
  if (state.frameDepth >= state.frameLimit) {
    err = ErrorCode::StackOverflow;
    return false;
  }
  const FunctionBytecode& fn = program.functions[calleeId];
  // An action/rule entry takes its context as an implicit first parameter the
  // call site does not push; the bytecode arg count is then one short of the
  // declared parameter count.
  const uint32_t injected = (injectCtx && fn.injectCtxTypeIdx != kNoTypeIdx) ? 1u : 0u;
  const uint32_t stackParams = fn.numParams - injected;
  if (exactArity && argc != stackParams) {
    err = ErrorCode::ScriptError;
    return false;
  }
  if (fn.numLocals > state.localsLimit - state.localsDepth) {
    err = ErrorCode::StackOverflow;
    return false;
  }
  // Grow the locals and frame regions before any write; a grow re-derives the
  // region base, so every access below reads the post-grow base.
  if (!ensureLocalsCapacity(state, state.localsDepth + fn.numLocals) ||
      !ensureFrameCapacity(state, state.frameDepth + 1)) {
    err = ErrorCode::StackOverflow;
    return false;
  }

  const uint32_t argsBase = state.stackDepth - argc;
  const uint32_t take = argc < stackParams ? argc : stackParams;
  const uint32_t localsOffset = state.localsDepth;
  // The operand stack and locals region are distinct arrays, so the args are
  // read out before the stack is truncated below. The injected context, when
  // present, occupies local 0 and the bytecode args follow it.
  for (uint32_t i = 0; i < fn.numLocals; i++) {
    if (injected == 1 && i == 0) {
      state.locals[localsOffset] = Value::structValue(fn.injectCtxTypeIdx, 0);
      continue;
    }
    const uint32_t argIdx = i - injected;
    state.locals[localsOffset + i] = argIdx < take ? state.stack[argsBase + argIdx] : kNilValue;
  }
  state.localsDepth += fn.numLocals;
  state.stackDepth = argsBase - extraDrop;

  // Advance the caller before pushing the callee so its pc resumes after the
  // call on return.
  state.frames[state.frameDepth - 1].pc++;

  Frame& callee = state.frames[state.frameDepth++];
  callee.funcId = calleeId;
  callee.pc = 0;
  callee.base = state.stackDepth;
  callee.localsOffset = localsOffset;
  callee.localsCount = fn.numLocals;
  callee.captures = captures;
  callee.ruleFuncId = kNoFuncId;
  callee.hasActionBinding = hasActionBinding;
  callee.actionBinding = actionBinding;
  return true;
}

/**
 * The action binding of the nearest enclosing action frame on `state`'s frame
 * stack, or nullptr when no live frame carries one. Mirrors
 * `getCurrentActionBinding` in
 * external/wendoo-lang/packages/core/src/runtime/vm.ts.
 */
const ActionFrameBinding* currentActionBinding(const ExecutionState& state) {
  for (uint32_t i = state.frameDepth; i-- > 0;) {
    if (state.frames[i].hasActionBinding) {
      return &state.frames[i].actionBinding;
    }
  }
  return nullptr;
}

/**
 * The rule funcId in scope for `frame`: its own rule binding when set, else its
 * function id resolved against the program's rule set. Mirrors
 * `resolveFrameRuleFuncId` in external/wendoo-lang/.../vm.ts.
 */
uint32_t resolveFrameRuleFuncId(const ProgramImage& program, const Frame& frame) {
  if (frame.ruleFuncId != kNoFuncId) {
    return frame.ruleFuncId;
  }
  return resolveDirectRuleFuncId(program, frame.funcId);
}

/**
 * The rule funcId a called function runs under: the callee's own rule binding
 * when it is itself a rule entry, else the calling frame's rule in scope. So a
 * rule that calls a plain helper forwards its rule, and a `ctx.rule` access
 * inside the helper resolves to the calling rule's store. Mirrors
 * `resolveCalleeRuleFuncId` in external/wendoo-lang/.../vm.ts.
 */
uint32_t resolveCalleeRuleFuncId(const ProgramImage& program, const Frame& caller,
                                 uint32_t calleeId) {
  const uint32_t direct = resolveDirectRuleFuncId(program, calleeId);
  if (direct != kNoFuncId) {
    return direct;
  }
  return resolveFrameRuleFuncId(program, caller);
}

/**
 * Resolves the UTF-8 content of a string `value` tolerating a null heap: a
 * borrowed (constant-pool) string reads from `program`, a managed string needs
 * `heap` (false when it is null). Returns false for a non-string value.
 */
bool contextVarNameBytes(const Value& value, const ProgramImage& program, const ManagedHeap* heap,
                         const char*& bytes, uint32_t& length) {
  if (!value.isString()) {
    return false;
  }
  if (value.isManagedString()) {
    return heap != nullptr && heap->stringContent(value, bytes, length);
  }
  const uint32_t index = value.borrowedStringIndex();
  if (index >= program.strings.size()) {
    return false;
  }
  const StringRef& ref = program.strings[index];
  bytes = reinterpret_cast<const char*>(program.stringData.data()) + ref.offset;
  length = ref.length;
  return true;
}

/**
 * The brain-variable slot bound to `nameValue`, written to `slotOut` on a hit.
 * Scans the program's `variableNames` pool by string content (the slot-name
 * binding the TS brain builds from `program.variableNames`); a name the program
 * does not declare is unbound. `heap` is needed only for a managed-string name.
 */
bool resolveBrainVarSlot(const ProgramImage& program, const ManagedHeap* heap,
                         const Value& nameValue, uint32_t& slotOut) {
  const char* nameBytes = nullptr;
  uint32_t nameLength = 0;
  if (!contextVarNameBytes(nameValue, program, heap, nameBytes, nameLength)) {
    return false;
  }
  for (uint32_t slot = 0; slot < program.variableNames.size(); slot++) {
    const uint32_t strIdx = program.variableNames[slot];
    if (strIdx >= program.strings.size()) {
      continue;
    }
    const StringRef& ref = program.strings[strIdx];
    const char* varBytes = reinterpret_cast<const char*>(program.stringData.data()) + ref.offset;
    if (ref.length == nameLength && std::memcmp(varBytes, nameBytes, nameLength) == 0) {
      slotOut = slot;
      return true;
    }
  }
  return false;
}

/**
 * Writes a rule-scoped variable by name to `ruleFuncId`'s own inner map,
 * allocating the outer store and the inner map on demand; never writes through
 * to ancestors. Mirrors `setByName` in
 * external/wendoo-lang/packages/core/src/runtime/rule-services.ts. A write
 * with no rule in scope is a no-op (returns true). Returns false on a name that
 * is neither a string nor a number, or when the heap cannot back an allocation.
 */
bool ruleVarSet(ExecutionContext* context, ManagedHeap* heapPtr, GcRoots* roots,
                uint32_t ruleFuncId, const Value& nameValue, const Value& value) {
  if (ruleFuncId == kNoFuncId) {
    return true;
  }
  if (heapPtr == nullptr) {
    return false;
  }
  MapKey nameKey;
  if (!valueToMapKey(nameValue, nameKey)) {
    return false;
  }
  ExecutionContext& ctx = *context;
  ManagedHeap& heap = *heapPtr;
  // Pin the caller's values: a host action may pass fresh unrooted managed
  // strings, and any allocation below can run a collection.
  ManagedHeap::Pin pinName(heap, nameValue);
  ManagedHeap::Pin pinValue(heap, value);
  if (!ctx.ruleVarStores.isMap()) {
    Value outerValue;
    if (!heap.newMap(kNoTypeIdx, roots, outerValue)) {
      return false;
    }
    ctx.ruleVarStores = outerValue;
  }
  // Pin the outer store across the inner-map allocation and the inserts below:
  // a collection can run on any allocation, and the store is reachable as a
  // root only once it lands on the context.
  ManagedHeap::Pin pinOuter(heap, ctx.ruleVarStores);
  const MapKey ruleKey{true, false, static_cast<mc_number_t>(ruleFuncId), 0};
  MapObject* outer = heap.map(ctx.ruleVarStores);
  Value innerValue;
  if (heap.mapHas(outer, ruleKey)) {
    innerValue = heap.mapGet(outer, ruleKey);
  } else {
    if (!heap.newMap(kNoTypeIdx, roots, innerValue)) {
      return false;
    }
    ManagedHeap::Pin pinFreshInner(heap, innerValue);
    if (!heap.mapSet(outer, ruleKey, innerValue, roots)) {
      return false;
    }
  }
  ManagedHeap::Pin pinInner(heap, innerValue);
  return heap.mapSet(heap.map(innerValue), nameKey, value, roots);
}

/**
 * True for the by-name context-variable host functions: `ctx.brain` and
 * `ctx.rule` get/set (`CoreFuncId` 48-51).
 */
bool isContextVariableFunc(CoreFuncId id) {
  return id == CoreFuncId::BrainContextGetVariable || id == CoreFuncId::BrainContextSetVariable ||
         id == CoreFuncId::RuleContextGetVariable || id == CoreFuncId::RuleContextSetVariable;
}

/**
 * Services a by-name context-variable host function over `args`. The
 * struct-method calling convention puts the receiver at arg 0; the variable
 * name is arg 1 and (for the setters) the value is arg 2. Mirrors the bodies in
 * external/wendoo-lang/.../context-types.ts together with the brain and rule
 * variable services. Brain variables are slot-backed (an undeclared name reads
 * nil and writes are dropped); rule variables resolve the in-scope rule from
 * `frame`. Faults `HostError` without a context (or, for the rule setter,
 * without a heap) and `ScriptError` on a missing or non-string name argument.
 */
Status dispatchContextVariableFunc(CoreFuncId id, Span<const Value> args,
                                   const ProgramImage& program, const RuntimeSurface& surface,
                                   const Frame& frame, Value& out) {
  if (surface.context == nullptr) {
    return Status::fail(ErrorCode::HostError);
  }
  if (args.size() < 2 || !args[1].isString()) {
    return Status::fail(ErrorCode::ScriptError);
  }
  const Value& name = args[1];
  switch (id) {
  case CoreFuncId::BrainContextGetVariable: {
    uint32_t slot = 0;
    out = resolveBrainVarSlot(program, surface.heap, name, slot) ? surface.context->variables[slot]
                                                                 : kNilValue;
    return Status::ok();
  }
  case CoreFuncId::BrainContextSetVariable: {
    if (args.size() < 3) {
      return Status::fail(ErrorCode::ScriptError);
    }
    uint32_t slot = 0;
    if (resolveBrainVarSlot(program, surface.heap, name, slot)) {
      surface.context->variables[slot] = args[2];
    }
    out = kNilValue;
    return Status::ok();
  }
  case CoreFuncId::RuleContextGetVariable: {
    MapKey nameKey;
    if (surface.heap != nullptr && valueToMapKey(name, nameKey)) {
      out = ruleVarGet(*surface.context, *surface.heap, resolveFrameRuleFuncId(program, frame),
                       nameKey);
    } else {
      out = kNilValue;
    }
    return Status::ok();
  }
  case CoreFuncId::RuleContextSetVariable: {
    if (args.size() < 3) {
      return Status::fail(ErrorCode::ScriptError);
    }
    if (!ruleVarSet(surface.context, surface.heap, surface.roots,
                    resolveFrameRuleFuncId(program, frame), name, args[2])) {
      return Status::fail(ErrorCode::HostError);
    }
    out = kNilValue;
    return Status::ok();
  }
  default:
    return Status::fail(ErrorCode::ScriptError);
  }
}

/**
 * Services `Context.getWhenResult`: binds the in-scope rule from `frame` (a sync
 * HOST_CALL dispatch does not otherwise set `ctx.currentRuleFuncId`, unlike
 * HOST_ACTION_CALL) and reads that rule's captured WHEN result through the
 * dedicated reserved-key accessor. The only host argument is the struct-method
 * receiver at arg 0, which carries no data, so the caller's arg buffer is unused
 * here. Faults `HostError` without a context or heap. Mirrors the
 * `Context.getWhenResult` body in external/wendoo-lang/.../context-types.ts.
 */
Status dispatchGetWhenResult(const RuntimeSurface& surface, const ProgramImage& program,
                             const Frame& frame, Value& out) {
  if (surface.context == nullptr || surface.heap == nullptr) {
    return Status::fail(ErrorCode::HostError);
  }
  const uint32_t priorRuleFuncId = surface.context->currentRuleFuncId;
  surface.context->currentRuleFuncId = resolveFrameRuleFuncId(program, frame);
  out = getWhenResult(*surface.context, *surface.heap);
  surface.context->currentRuleFuncId = priorRuleFuncId;
  return Status::ok();
}

/**
 * The firing record a chain gate writes: `DidFire` when the rule fired, and on a
 * rule that did not fire the record of its subject -- the rule directly above it
 * at its own nesting level. A rule with no subject records its own outcome.
 * Mirrors `chainedFiringState` in external/wendoo-lang/.../vm.ts.
 */
RuleFiringState chainedFiringState(const ExecutionContext& ctx, const ProgramImage& program,
                                   uint32_t ruleFuncId, bool fired) {
  if (fired) {
    return RuleFiringState::DidFire;
  }
  const uint32_t subject = precedingSiblingRuleFuncId(program, ruleFuncId);
  if (subject == kNoFuncId) {
    return RuleFiringState::DidNotFire;
  }
  return ctx.ruleFiringState(subject);
}

/**
 * The shared body of the four WHEN boundary gates. Pops the WHEN result, copies
 * it into the rule's reserved `__whenResult` variable (every rule captures,
 * whatever the gate decides), computes the fired outcome, writes the firing
 * record, and advances the pc by one on a fire or by the signed `a` offset on a
 * skip, which lands past the DO section and any nested boundaries. Returns false
 * with an empty operand stack, which the caller faults `StackUnderflow`.
 * Mirrors `execWhenGate` in external/wendoo-lang/.../vm.ts.
 *
 * @param presenceGated - Gate on the WHEN result being present (non-nil), so a
 *   present falsy value fires. Otherwise the gate is on truthiness.
 * @param chained - Write the chain-aware firing record of a rule that chains
 *   onto its subject. Otherwise the record is the rule's own outcome.
 */
bool execWhenGate(ExecutionState& state, const ProgramImage& program, const RuntimeSurface& surface,
                  Frame& frame, const Instr& ins, bool presenceGated, bool chained) {
  if (state.stackDepth == 0) {
    return false;
  }
  // Peek (not pop) so the value stays rooted on the operand stack across the
  // rule-var allocation. Best-effort: a missing heap or allocation failure drops
  // the capture without disturbing the gate.
  const Value value = state.stack[state.stackDepth - 1];
  const uint32_t ruleFuncId = resolveFrameRuleFuncId(program, frame);
  ruleVarSet(surface.context, surface.heap, surface.roots, ruleFuncId,
             Value::number(kWhenResultRuleVarKey), value);
  state.stackDepth--;
  const bool fired = presenceGated ? !value.isNil() : isTruthy(value, program, surface.heap);
  if (surface.context != nullptr) {
    const RuleFiringState record =
        chained ? chainedFiringState(*surface.context, program, ruleFuncId, fired)
        : fired ? RuleFiringState::DidFire
                : RuleFiringState::DidNotFire;
    surface.context->setRuleFiringState(ruleFuncId, record);
  }
  frame.pc = fired ? frame.pc + 1 : addRel(frame.pc, ins.a);
  return true;
}

/**
 * Records that the async dispatch at `pc` found no free async handle. Returns
 * true once that same dispatch has backpressured
 * {@link kHandleBackpressureFaultRounds} consecutive rounds, at which point the
 * caller faults the fiber `StackOverflow`. Mirrors `backpressureOnHandles` in
 * external/wendoo-lang/packages/core/src/runtime/vm.ts.
 */
bool noteHandleBackpressure(ExecutionState& state, uint32_t pc) {
  state.handleBackpressureRounds =
      state.handleBackpressurePc == pc ? state.handleBackpressureRounds + 1 : 1;
  state.handleBackpressurePc = pc;
  return state.handleBackpressureRounds >= kHandleBackpressureFaultRounds;
}

/** Clears the backpressure run recorded by {@link noteHandleBackpressure}. */
void clearHandleBackpressure(ExecutionState& state) {
  state.handleBackpressurePc = 0;
  state.handleBackpressureRounds = 0;
}

} // namespace

bool constValueToRuntime(const ConstValue& constant, Value& out) {
  switch (constant.kind) {
  case ConstValueKind::Unknown:
    out = kUnknownValue;
    return true;
  case ConstValueKind::Void:
    out = kVoidValue;
    return true;
  case ConstValueKind::Nil:
    out = kNilValue;
    return true;
  case ConstValueKind::Boolean:
    out = Value::boolean(constant.boolean.value);
    return true;
  case ConstValueKind::Number:
    out = Value::number(constant.number.value);
    return true;
  case ConstValueKind::String:
    out = Value::borrowedString(constant.string.stringIdx);
    return true;
  case ConstValueKind::Buffer:
    out = Value::borrowedBuffer(constant.buffer.byteOffset, constant.buffer.byteCount);
    return true;
  case ConstValueKind::Enum:
    out = Value::enumSymbol(constant.enumVal.typeIdx, constant.enumVal.ordinal);
    return true;
  case ConstValueKind::Function:
    if (constant.function.hasCaptures) {
      return false;
    }
    out = Value::function(constant.function.funcId);
    return true;
  case ConstValueKind::List:
  case ConstValueKind::Map:
  case ConstValueKind::Struct:
    return false;
  }
  return false;
}

bool setRuleVariable(ExecutionContext& ctx, ManagedHeap& heap, GcRoots* roots, const Value& name,
                     const Value& value) {
  return ruleVarSet(&ctx, &heap, roots, ctx.currentRuleFuncId, name, value);
}

bool isTruthy(const Value& value, const ProgramImage& program, const ManagedHeap* heap) {
  switch (value.tag()) {
  case ValueTag::Unknown:
  case ValueTag::Void:
  case ValueTag::Nil:
    return false;
  case ValueTag::Boolean:
    return value.asBoolean();
  case ValueTag::Number:
    // NaN compares unequal to zero, so NaN numbers are truthy, matching the
    // TS reference rule (`v !== 0`).
    return value.asNumber() != 0.0f;
  case ValueTag::String: {
    const uint32_t index = value.borrowedStringIndex();
    return index < program.strings.size() && program.strings[index].length > 0;
  }
  case ValueTag::Enum:
  case ValueTag::Struct:
  case ValueTag::Function:
  case ValueTag::Handle:
    return true;
  case ValueTag::List:
    // A list is truthy when non-empty. Reachable only with a configured heap.
    return heap != nullptr && heap->list(value)->size > 0;
  case ValueTag::Map:
    // A map is truthy when non-empty. Reachable only with a configured heap.
    return heap != nullptr && heap->map(value)->size > 0;
  case ValueTag::Buffer:
    return value.bufferLength() > 0;
  case ValueTag::Err:
    return false;
  }
  return false;
}

Status startExecution(ExecutionState& state, const ProgramImage& program, uint32_t funcId,
                      Span<const Value> args) {
  if (funcId >= program.functions.size()) {
    return Status::fail(ErrorCode::HostError);
  }
  const FunctionBytecode& fn = program.functions[funcId];
  if (state.frameDepth >= state.frameLimit) {
    return Status::fail(ErrorCode::StackOverflow);
  }
  if (fn.numLocals > state.localsLimit - state.localsDepth) {
    return Status::fail(ErrorCode::StackOverflow);
  }
  // Grow the locals and frame regions before any write (see pushCallFrame).
  if (!ensureLocalsCapacity(state, state.localsDepth + fn.numLocals) ||
      !ensureFrameCapacity(state, state.frameDepth + 1)) {
    return Status::fail(ErrorCode::StackOverflow);
  }

  // A rule/action entry spawned as a root fiber takes its context as an
  // implicit first parameter; the injected context occupies local 0 and the
  // caller-supplied args follow it.
  const uint32_t injected = fn.injectCtxTypeIdx != kNoTypeIdx ? 1u : 0u;
  const uint32_t localsOffset = state.localsDepth;
  for (uint32_t i = 0; i < fn.numLocals; i++) {
    if (injected == 1 && i == 0) {
      state.locals[localsOffset] = Value::structValue(fn.injectCtxTypeIdx, 0);
      continue;
    }
    const uint32_t argIdx = i - injected;
    state.locals[localsOffset + i] = argIdx < args.size() ? args[argIdx] : kNilValue;
  }
  state.localsDepth += fn.numLocals;

  Frame& frame = state.frames[state.frameDepth++];
  frame.funcId = funcId;
  frame.pc = 0;
  frame.base = state.stackDepth;
  frame.localsOffset = localsOffset;
  frame.localsCount = fn.numLocals;
  frame.captures = kNoCaptures;
  frame.ruleFuncId = kNoFuncId;
  frame.hasActionBinding = false;
  frame.actionBinding = ActionFrameBinding{0, 0, false};
  return Status::ok();
}

RunResult runExecution(ExecutionState& state, const ProgramImage& program,
                       const RuntimeSurface& surface) {
  if (state.budget <= 0) {
    // Host-contract violation: a slice must be entered with a positive
    // budget. The state is untouched and stays resumable.
    return RunResult::fault(ErrorCode::HostError, kNoFuncId, 0);
  }

  while (state.budget > 0) {
    if (state.cancelled) {
      // Cancelled mid-slice by a host body (page change or restart): stop at
      // this instruction boundary and abandon the rest of the body. Mirrors the
      // mid-run cancel check in runFiber (TS vm.ts).
      return RunResult::done(kNilValue);
    }
    state.budget--;

    if (state.pendingInjectedThrow) {
      // A handle settled rejected/cancelled; throw it at the resume point before
      // running the next instruction. Mirrors the pendingInjectedThrow check at
      // the top of runFiber in vm.ts.
      state.pendingInjectedThrow = false;
      if (state.frameDepth == 0) {
        return RunResult::fault(state.injectedError, kNoFuncId, 0);
      }
      const Frame& top = state.frames[state.frameDepth - 1];
      RunResult escaped = RunResult::yielded();
      if (!throwError(state, state.injectedError, top.funcId, top.pc, escaped)) {
        return escaped;
      }
      continue;
    }

    if (state.frameDepth == 0) {
      return RunResult::fault(ErrorCode::ScriptError, kNoFuncId, 0);
    }
    Frame& frame = state.frames[state.frameDepth - 1];
    const auto fault = [&frame](ErrorCode code) {
      return RunResult::fault(code, frame.funcId, frame.pc);
    };

    if (frame.funcId >= program.functions.size()) {
      return fault(ErrorCode::ScriptError);
    }
    const FunctionBytecode& fn = program.functions[frame.funcId];
    if (frame.pc >= fn.codeCount) {
      return fault(ErrorCode::ScriptError);
    }
    const Instr& ins = program.instructions[fn.codeOffset + frame.pc];

    switch (ins.op) {
    case Op::PUSH_CONST_VAL: {
      if (ins.a >= program.constantPools.valueCount) {
        return fault(ErrorCode::ScriptError);
      }
      const ConstValue& constant = program.constValues[ins.a];
      Value value;
      if (constant.kind == ConstValueKind::Struct) {
        // Materialize a baked struct constant into a fresh managed struct whose
        // slots are its inline field values. The fields are scalar or borrowed
        // (number/buffer/string), so none allocates and the struct cannot be
        // collected between its allocation and the slot writes; a nested
        // container field is unsupported and faults via constValueToRuntime.
        if (surface.heap == nullptr) {
          return fault(ErrorCode::HostError);
        }
        const uint32_t fieldCount = constant.structVal.fieldsCount;
        if (!surface.heap->newStruct(constant.structVal.typeIdx, fieldCount, surface.roots,
                                     value)) {
          return fault(ErrorCode::StackOverflow);
        }
        StructObject* obj = surface.heap->structOf(value);
        for (uint32_t i = 0; i < fieldCount; i++) {
          Value field;
          if (!constValueToRuntime(program.constValues[constant.structVal.fieldsOffset + i],
                                   field)) {
            return fault(ErrorCode::ScriptError);
          }
          surface.heap->structSet(obj, i, field);
        }
      } else if (!constValueToRuntime(constant, value)) {
        return fault(ErrorCode::ScriptError);
      }
      if (!pushValue(state, value)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::PUSH_CONST_NUM: {
      if (ins.a >= program.constantPools.numberCount) {
        return fault(ErrorCode::ScriptError);
      }
      if (!pushValue(state, Value::number(program.constNumbers[ins.a]))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::PUSH_CONST_STR: {
      if (ins.a >= program.constantPools.stringCount) {
        return fault(ErrorCode::ScriptError);
      }
      if (!pushValue(state, Value::borrowedString(ins.a))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::POP: {
      Value discarded;
      if (!popValue(state, discarded)) {
        return fault(ErrorCode::StackUnderflow);
      }
      frame.pc++;
      break;
    }

    case Op::DUP: {
      if (state.stackDepth == 0) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!pushValue(state, state.stack[state.stackDepth - 1])) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::SWAP: {
      Value a;
      Value b;
      if (!popValue(state, a) || !popValue(state, b)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!pushValue(state, a) || !pushValue(state, b)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::STACK_SET_REL: {
      Value value;
      if (!popValue(state, value)) {
        return fault(ErrorCode::StackUnderflow);
      }
      // `a` addresses the post-pop stack relative to its top; an offset past
      // the bottom is an out-of-bounds write.
      if (ins.a >= state.stackDepth) {
        return fault(ErrorCode::ScriptError);
      }
      state.stack[state.stackDepth - 1 - ins.a] = value;
      frame.pc++;
      break;
    }

    case Op::JMP: {
      frame.pc = addRel(frame.pc, ins.a);
      break;
    }

    case Op::JMP_IF_FALSE: {
      Value value;
      if (!popValue(state, value)) {
        return fault(ErrorCode::StackUnderflow);
      }
      frame.pc = isTruthy(value, program, surface.heap) ? frame.pc + 1 : addRel(frame.pc, ins.a);
      break;
    }

    case Op::JMP_IF_TRUE: {
      Value value;
      if (!popValue(state, value)) {
        return fault(ErrorCode::StackUnderflow);
      }
      frame.pc = isTruthy(value, program, surface.heap) ? addRel(frame.pc, ins.a) : frame.pc + 1;
      break;
    }

    case Op::RET: {
      Value retv;
      if (!popValue(state, retv)) {
        return fault(ErrorCode::StackUnderflow);
      }
      const Frame returning = frame;
      state.frameDepth--;
      if (state.stackDepth > returning.base) {
        state.stackDepth = returning.base;
      }
      state.localsDepth = returning.localsOffset;
      // A dispatched sync action frame always has a caller below it; a hook
      // frame and an async action's child frame are their fiber's entry frame,
      // and neither is a dispatch hand-back. The popped frame's locals are
      // still readable: shrinking localsDepth leaves the slots in place.
      if (state.frameDepth > 0 && returning.hasActionBinding && !returning.actionBinding.isAsync &&
          surface.observer != nullptr) {
        const FunctionBytecode& returningFn = program.functions[returning.funcId];
        const uint32_t injected = returningFn.injectCtxTypeIdx != kNoTypeIdx ? 1u : 0u;
        const Span<const Value> args(state.locals + returning.localsOffset + injected,
                                     returningFn.numParams - injected);
        surface.observer->onBytecodeActionCall(returning.actionBinding.actionId,
                                               returning.actionBinding.callSiteId, args, retv);
      }
      if (!pushValue(state, retv)) {
        return RunResult::fault(ErrorCode::StackOverflow, returning.funcId, returning.pc);
      }
      if (state.frameDepth == 0) {
        return RunResult::done(retv);
      }
      // The caller's pc was advanced when the call was made; execution
      // continues at its next instruction.
      break;
    }

    case Op::LOAD_LOCAL: {
      if (ins.a >= frame.localsCount) {
        return fault(ErrorCode::ScriptError);
      }
      if (!pushValue(state, state.locals[frame.localsOffset + ins.a])) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::STORE_LOCAL: {
      if (ins.a >= frame.localsCount) {
        return fault(ErrorCode::ScriptError);
      }
      Value value;
      if (!popValue(state, value)) {
        return fault(ErrorCode::StackUnderflow);
      }
      state.locals[frame.localsOffset + ins.a] = value;
      frame.pc++;
      break;
    }

    case Op::LOAD_VAR_SLOT: {
      if (ins.a >= program.variableNames.size()) {
        return fault(ErrorCode::ScriptError);
      }
      if (surface.context == nullptr || ins.a >= surface.context->variables.size()) {
        return fault(ErrorCode::HostError);
      }
      if (!pushValue(state, surface.context->variables[ins.a])) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::STORE_VAR_SLOT: {
      if (ins.a >= program.variableNames.size()) {
        return fault(ErrorCode::ScriptError);
      }
      if (surface.context == nullptr || ins.a >= surface.context->variables.size()) {
        return fault(ErrorCode::HostError);
      }
      if (state.stackDepth == 0) {
        return fault(ErrorCode::StackUnderflow);
      }
      // Struct values are deep-copied on store (value semantics); primitives and
      // containers are written by reference. The source stays on the operand
      // stack (rooted) across the allocating copy.
      const Value& top = state.stack[state.stackDepth - 1];
      Value stored = top;
      if (top.isStruct()) {
        if (surface.heap == nullptr) {
          return fault(ErrorCode::HostError);
        }
        if (!surface.heap->deepCopy(top, surface.roots, stored)) {
          return fault(ErrorCode::StackOverflow);
        }
      }
      surface.context->variables[ins.a] = stored;
      state.stackDepth--;
      frame.pc++;
      break;
    }

    case Op::LOAD_SYSTEM_VAR: {
      // An unwritten or out-of-range System slot reads nil; no bound context is
      // a host-contract violation.
      if (surface.context == nullptr) {
        return fault(ErrorCode::HostError);
      }
      const Span<Value>& store = surface.context->systemStore;
      const Value value = ins.a < store.size() ? store[ins.a] : kNilValue;
      if (!pushValue(state, value)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::STORE_SYSTEM_VAR: {
      if (surface.context == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Span<Value>& store = surface.context->systemStore;
      // System store slots are linker-assigned in 0..systemCount-1; an index
      // past the store is a malformed program.
      if (ins.a >= store.size()) {
        return fault(ErrorCode::HostError);
      }
      if (state.stackDepth == 0) {
        return fault(ErrorCode::StackUnderflow);
      }
      // Written by reference (no deep copy): a System's state struct mutates in
      // place across thinks.
      store[ins.a] = state.stack[state.stackDepth - 1];
      state.stackDepth--;
      frame.pc++;
      break;
    }

    case Op::LOAD_CALLSITE_VAR: {
      // An unwritten or out-of-stride slot reads nil; no bound action is a
      // host-contract violation.
      if (surface.context == nullptr) {
        return fault(ErrorCode::HostError);
      }
      const ActionFrameBinding* binding = currentActionBinding(state);
      if (binding == nullptr) {
        return fault(ErrorCode::ScriptError);
      }
      ExecutionContext& ctx = *surface.context;
      if (binding->callSiteId >= ctx.callSiteAllocated.size()) {
        return fault(ErrorCode::HostError);
      }
      if (!pushValue(state, ctx.callSiteSlot(binding->callSiteId, ins.a))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::STORE_CALLSITE_VAR: {
      // No bound action is a host-contract violation; a slot index past the
      // program-sized pad is a script error.
      if (surface.context == nullptr) {
        return fault(ErrorCode::HostError);
      }
      const ActionFrameBinding* binding = currentActionBinding(state);
      if (binding == nullptr) {
        return fault(ErrorCode::ScriptError);
      }
      ExecutionContext& ctx = *surface.context;
      if (binding->callSiteId >= ctx.callSiteAllocated.size()) {
        return fault(ErrorCode::HostError);
      }
      if (ins.a >= ctx.callSiteSlotStride) {
        return fault(ErrorCode::ScriptError);
      }
      Value value;
      if (!popValue(state, value)) {
        return fault(ErrorCode::StackUnderflow);
      }
      ctx.setCallSiteSlot(binding->callSiteId, ins.a, value);
      frame.pc++;
      break;
    }

    case Op::HOST_CALL: {
      // Operand layout: a = funcId, b = argc, c = callSiteId. The core bodies
      // take no execution context, so the call-site operand is unused. Core ids
      // dispatch by id; target ids (>= TARGET_FUNC_ID_BASE) dispatch through the
      // registered target host-function table and fault as unregistered when no
      // binding holds the id.
      const uint32_t fnId = ins.a;
      const uint32_t argc = ins.b;
      if (argc > state.stackDepth) {
        return fault(ErrorCode::StackUnderflow);
      }
      const Span<const Value> args(state.stack + (state.stackDepth - argc), argc);
      Value result;
      Status status = Status::fail(ErrorCode::ScriptError);
      if (fnId < TARGET_FUNC_ID_BASE) {
        const CoreFuncId coreId = static_cast<CoreFuncId>(fnId);
        if (isContextVariableFunc(coreId)) {
          status = dispatchContextVariableFunc(coreId, args, program, surface, frame, result);
        } else if (coreId == CoreFuncId::ContextGetWhenResult) {
          status = dispatchGetWhenResult(surface, program, frame, result);
        } else {
          const HostCallEnv env{surface.heap, surface.roots, surface.rng, surface.types};
          status = callCoreHostFunction(coreId, args, env, result);
        }
      } else {
        const TargetHostFuncBinding* binding = findTargetHostFuncById(surface.hostFunctions, fnId);
        if (binding != nullptr) {
          status = binding->exec(binding->hostData, args, result);
        }
      }
      if (!status.isOk()) {
        return fault(status.error());
      }
      state.stackDepth -= argc;
      if (!pushValue(state, result)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::HOST_ACTION_CALL: {
      const uint32_t argc = ins.b;
      if (argc > state.stackDepth) {
        return fault(ErrorCode::StackUnderflow);
      }
      const HostActionBinding* action = findHostActionById(surface.actions, ins.a);
      if (action == nullptr || action->execSync == nullptr) {
        // Existence check: no registration holds the id (or it has no sync
        // body), mirroring the TS resolveHostAction fault.
        return fault(ErrorCode::ScriptError);
      }
      if (surface.context == nullptr || ins.c >= surface.context->callSiteStates.size()) {
        return fault(ErrorCode::HostError);
      }
      ExecutionContext& ctx = *surface.context;
      ctx.currentCallSiteId = ins.c;
      ctx.currentRuleFuncId = resolveFrameRuleFuncId(program, frame);
      const Span<const Value> args(state.stack + (state.stackDepth - argc), argc);
      const Value result = action->execSync(action->hostData, ctx, args);
      if (surface.observer != nullptr) {
        surface.observer->onHostActionCall(ins.a, ins.c, args, result);
      }
      state.stackDepth -= argc;
      ctx.currentCallSiteId = kNoCallSiteId;
      ctx.currentRuleFuncId = kNoFuncId;
      if (!pushValue(state, result)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::HOST_CALL_ASYNC: {
      // a = funcId, b = argc, c = callSiteId. Allocates a pending handle, hands
      // it to the async target host function, and pushes it. Mirrors
      // execHostCallAsync in vm.ts. Inside a sync action frame the dispatch
      // cannot suspend, so the await it sets up would be illegal: fault now.
      const ActionFrameBinding* suspendBinding = currentActionBinding(state);
      if (suspendBinding != nullptr && !suspendBinding->isAsync) {
        return fault(ErrorCode::ScriptError);
      }
      if (surface.handles == nullptr || surface.context == nullptr) {
        return fault(ErrorCode::HostError);
      }
      const uint32_t fnId = ins.a;
      const uint32_t argc = ins.b;
      if (argc > state.stackDepth) {
        return fault(ErrorCode::StackUnderflow);
      }
      // Only target host functions carry async bodies; core ids have none.
      const TargetHostFuncBinding* binding =
          fnId >= TARGET_FUNC_ID_BASE ? findTargetHostFuncById(surface.hostFunctions, fnId)
                                      : nullptr;
      if (binding == nullptr || binding->execAsync == nullptr) {
        return fault(ErrorCode::ScriptError);
      }
      // Backpressure on handle exhaustion: with no free handle, yield without
      // advancing the pc so the fiber re-enters the run queue and re-executes
      // this same dispatch next round. The check precedes every side effect (no
      // args popped, no handle allocated, no action started, pc unchanged), so
      // the retry is an exact re-execution once an in-flight async settles a
      // slot.
      if (!surface.handles->hasCapacity()) {
        if (noteHandleBackpressure(state, frame.pc)) {
          return fault(ErrorCode::StackOverflow);
        }
        return RunResult::yielded();
      }
      clearHandleBackpressure(state);
      const uint32_t handleId = surface.handles->createPending();
      if (handleId == kNoHandleId) {
        return fault(ErrorCode::StackOverflow);
      }
      // The arg view is valid only for the call; the body copies what it
      // retains. A failing body rolls back the handle and faults.
      const Span<const Value> args(state.stack + (state.stackDepth - argc), argc);
      const Status status = binding->execAsync(binding->hostData, *surface.context, args,
                                               AsyncHandle{surface.handles, handleId});
      if (!status.isOk()) {
        surface.handles->deleteHandle(handleId);
        return fault(status.error());
      }
      state.stackDepth -= argc;
      if (!pushValue(state, Value::handle(handleId))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::HOST_ACTION_CALL_ASYNC: {
      // a = actionId, b = argc, c = callSiteId. Allocates a pending handle, hands
      // it to the async host action with its call site bound, and pushes it.
      // Mirrors execHostActionCallAsync in vm.ts.
      const ActionFrameBinding* suspendBinding = currentActionBinding(state);
      if (suspendBinding != nullptr && !suspendBinding->isAsync) {
        return fault(ErrorCode::ScriptError);
      }
      if (surface.handles == nullptr) {
        return fault(ErrorCode::HostError);
      }
      const uint32_t argc = ins.b;
      if (argc > state.stackDepth) {
        return fault(ErrorCode::StackUnderflow);
      }
      const HostActionBinding* action = findHostActionById(surface.actions, ins.a);
      if (action == nullptr || action->execAsync == nullptr) {
        // No registration holds the id, or it has no async body: mirrors the
        // resolveHostAction fault for a sync/missing async action.
        return fault(ErrorCode::ScriptError);
      }
      if (surface.context == nullptr || ins.c >= surface.context->callSiteStates.size()) {
        return fault(ErrorCode::HostError);
      }
      // Backpressure on handle exhaustion: with no free handle, yield without
      // advancing the pc so the fiber re-enters the run queue and re-executes
      // this same dispatch next round. The check precedes every side effect (no
      // args popped, no call site bound, no handle allocated, no action started,
      // pc unchanged), so the retry is an exact re-execution once an in-flight
      // async settles a slot. An action declaring `uncappedHandles` skips the
      // check and allocates outside the cap.
      const bool capped = !action->uncappedHandles;
      if (capped && !surface.handles->hasCapacity()) {
        if (noteHandleBackpressure(state, frame.pc)) {
          return fault(ErrorCode::StackOverflow);
        }
        return RunResult::yielded();
      }
      clearHandleBackpressure(state);
      ExecutionContext& ctx = *surface.context;
      ctx.currentCallSiteId = ins.c;
      ctx.currentRuleFuncId = resolveFrameRuleFuncId(program, frame);
      const uint32_t handleId = surface.handles->createPending(capped);
      if (handleId == kNoHandleId) {
        return fault(ErrorCode::StackOverflow);
      }
      const Span<const Value> args(state.stack + (state.stackDepth - argc), argc);
      const Status status =
          action->execAsync(action->hostData, ctx, args, AsyncHandle{surface.handles, handleId});
      ctx.currentCallSiteId = kNoCallSiteId;
      ctx.currentRuleFuncId = kNoFuncId;
      if (!status.isOk()) {
        surface.handles->deleteHandle(handleId);
        return fault(status.error());
      }
      if (surface.observer != nullptr) {
        surface.observer->onHostActionCallAsync(ins.a, ins.c, args);
      }
      state.stackDepth -= argc;
      if (!pushValue(state, Value::handle(handleId))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::AWAIT: {
      // Pops a handle. Resolved -> push its value; rejected/cancelled -> throw
      // into the handler path; pending -> park the fiber WAITING and let the
      // scheduler register it as a waiter. An await inside a sync action frame
      // cannot suspend and faults. Mirrors execAwait in vm.ts.
      const ActionFrameBinding* suspendBinding = currentActionBinding(state);
      if (suspendBinding != nullptr && !suspendBinding->isAsync) {
        return fault(ErrorCode::ScriptError);
      }
      if (surface.handles == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value handleValue;
      if (!popValue(state, handleValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!handleValue.isHandle()) {
        return fault(ErrorCode::ScriptError);
      }
      Handle* h = surface.handles->get(handleValue.handleId());
      if (h == nullptr) {
        return fault(ErrorCode::ScriptError);
      }
      if (h->state == HandleState::Resolved) {
        if (!pushValue(state, h->result)) {
          return fault(ErrorCode::StackOverflow);
        }
        frame.pc++;
        break;
      }
      if (h->state == HandleState::Rejected || h->state == HandleState::Cancelled) {
        RunResult escaped = RunResult::yielded();
        if (!throwError(state, h->error, frame.funcId, frame.pc, escaped)) {
          return escaped;
        }
        break;
      }
      // Pending: record where to resume (stack height is post-pop) and return
      // Waiting; the scheduler parks the fiber and adds it to the handle's
      // waiters.
      state.awaiting = true;
      state.await =
          AwaitSite{frame.pc + 1, state.stackDepth, state.frameDepth, handleValue.handleId()};
      return RunResult::waiting();
    }

    case Op::WHEN_START: {
      // The WHEN section leaves exactly one value for the gate at its end. The
      // rule's firing record reads Evaluating from here until that gate writes
      // the outcome.
      if (surface.context != nullptr) {
        surface.context->setRuleFiringState(resolveFrameRuleFuncId(program, frame),
                                            RuleFiringState::Evaluating);
      }
      frame.pc++;
      break;
    }

    case Op::DO_START:
    case Op::DO_END: {
      // Pure section markers: advance the pc, no other effect.
      frame.pc++;
      break;
    }

    case Op::WHEN_END: {
      // The WHEN section leaves exactly one value: truthy falls through into
      // the DO section, falsy jumps past it by the signed `a` offset.
      if (!execWhenGate(state, program, surface, frame, ins, false, false)) {
        return fault(ErrorCode::StackUnderflow);
      }
      break;
    }

    case Op::WHEN_END_PRESENT: {
      // Presence-gated WHEN: a present value (including a falsy 0 / "" / false)
      // falls through into the DO section; only nil (absent) jumps past it by
      // the signed `a` offset.
      if (!execWhenGate(state, program, surface, frame, ins, true, false)) {
        return fault(ErrorCode::StackUnderflow);
      }
      break;
    }

    case Op::WHEN_END_CHAIN: {
      // The truthiness gate of a chaining rule: same condition, same skip, and
      // the chain-aware firing record.
      if (!execWhenGate(state, program, surface, frame, ins, false, true)) {
        return fault(ErrorCode::StackUnderflow);
      }
      break;
    }

    case Op::WHEN_END_PRESENT_CHAIN: {
      // The presence gate of a chaining rule: same condition, same skip, and the
      // chain-aware firing record.
      if (!execWhenGate(state, program, surface, frame, ins, true, true)) {
        return fault(ErrorCode::StackUnderflow);
      }
      break;
    }

    case Op::LIST_NEW: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value listValue;
      if (!surface.heap->newList(ins.b, surface.roots, listValue)) {
        return fault(ErrorCode::StackOverflow);
      }
      if (!pushValue(state, listValue)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::LIST_PUSH: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      // [list, item] -> [list]. Both operands stay on the stack (rooted)
      // across the possible backing growth and collection.
      if (state.stackDepth < 2) {
        return fault(ErrorCode::StackUnderflow);
      }
      const Value& listSlot = state.stack[state.stackDepth - 2];
      const Value& itemSlot = state.stack[state.stackDepth - 1];
      if (!listSlot.isList()) {
        return fault(ErrorCode::ScriptError);
      }
      if (!surface.heap->listPush(surface.heap->list(listSlot), itemSlot, surface.roots)) {
        return fault(ErrorCode::StackOverflow);
      }
      state.stackDepth--; // drop the item, leaving the list on top
      frame.pc++;
      break;
    }

    case Op::LIST_GET: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value index;
      Value listValue;
      if (!popValue(state, index) || !popValue(state, listValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!listValue.isList()) {
        return fault(ErrorCode::ScriptError);
      }
      if (!index.isNumber()) {
        return fault(ErrorCode::ScriptError);
      }
      int32_t idx = 0;
      Value result = kNilValue;
      if (numberToIndex(index.asNumber(), idx)) {
        result = surface.heap->listGet(surface.heap->list(listValue), idx);
      }
      if (!pushValue(state, result)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::LIST_SET: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      // [list, index, value] -> [list]. An in-place overwrite never allocates.
      if (state.stackDepth < 3) {
        return fault(ErrorCode::StackUnderflow);
      }
      const Value& listSlot = state.stack[state.stackDepth - 3];
      const Value& indexSlot = state.stack[state.stackDepth - 2];
      const Value& valueSlot = state.stack[state.stackDepth - 1];
      if (!listSlot.isList()) {
        return fault(ErrorCode::ScriptError);
      }
      if (!indexSlot.isNumber()) {
        return fault(ErrorCode::ScriptError);
      }
      int32_t idx = 0;
      if (numberToIndex(indexSlot.asNumber(), idx)) {
        surface.heap->listSet(surface.heap->list(listSlot), idx, valueSlot);
      }
      state.stackDepth -= 2; // drop index and value, leaving the list on top
      frame.pc++;
      break;
    }

    case Op::LIST_LEN: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value listValue;
      if (!popValue(state, listValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!listValue.isList()) {
        return fault(ErrorCode::ScriptError);
      }
      const Value len =
          Value::number(static_cast<mc_number_t>(surface.heap->list(listValue)->size));
      if (!pushValue(state, len)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::LIST_POP: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value listValue;
      if (!popValue(state, listValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!listValue.isList()) {
        return fault(ErrorCode::ScriptError);
      }
      if (!pushValue(state, surface.heap->listPop(surface.heap->list(listValue)))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::LIST_SHIFT: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value listValue;
      if (!popValue(state, listValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!listValue.isList()) {
        return fault(ErrorCode::ScriptError);
      }
      if (!pushValue(state, surface.heap->listShift(surface.heap->list(listValue)))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::LIST_REMOVE: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value index;
      Value listValue;
      if (!popValue(state, index) || !popValue(state, listValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!listValue.isList()) {
        return fault(ErrorCode::ScriptError);
      }
      if (!index.isNumber()) {
        return fault(ErrorCode::ScriptError);
      }
      int32_t idx = 0;
      Value result = kNilValue;
      if (numberToIndex(index.asNumber(), idx)) {
        result = surface.heap->listRemove(surface.heap->list(listValue), idx);
      }
      if (!pushValue(state, result)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::LIST_INSERT: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      // [list, index, value] -> []; operands stay rooted across a grow.
      if (state.stackDepth < 3) {
        return fault(ErrorCode::StackUnderflow);
      }
      const Value& listSlot = state.stack[state.stackDepth - 3];
      const Value& indexSlot = state.stack[state.stackDepth - 2];
      const Value& valueSlot = state.stack[state.stackDepth - 1];
      if (!listSlot.isList()) {
        return fault(ErrorCode::ScriptError);
      }
      if (!indexSlot.isNumber()) {
        return fault(ErrorCode::ScriptError);
      }
      int32_t idx = 0;
      if (numberToIndex(indexSlot.asNumber(), idx)) {
        if (!surface.heap->listInsert(surface.heap->list(listSlot), idx, valueSlot,
                                      surface.roots)) {
          return fault(ErrorCode::StackOverflow);
        }
      }
      state.stackDepth -= 3;
      frame.pc++;
      break;
    }

    case Op::LIST_SWAP: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      // [list, i, j] -> []. An in-place swap never allocates.
      if (state.stackDepth < 3) {
        return fault(ErrorCode::StackUnderflow);
      }
      const Value& listSlot = state.stack[state.stackDepth - 3];
      const Value& iSlot = state.stack[state.stackDepth - 2];
      const Value& jSlot = state.stack[state.stackDepth - 1];
      if (!listSlot.isList()) {
        return fault(ErrorCode::ScriptError);
      }
      if (!iSlot.isNumber() || !jSlot.isNumber()) {
        return fault(ErrorCode::ScriptError);
      }
      int32_t i = 0;
      int32_t j = 0;
      if (numberToIndex(iSlot.asNumber(), i) && numberToIndex(jSlot.asNumber(), j)) {
        surface.heap->listSwap(surface.heap->list(listSlot), i, j);
      }
      state.stackDepth -= 3;
      frame.pc++;
      break;
    }

    case Op::MAP_NEW: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value mapValue;
      if (!surface.heap->newMap(ins.b, surface.roots, mapValue)) {
        return fault(ErrorCode::StackOverflow);
      }
      if (!pushValue(state, mapValue)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::MAP_SET: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      // [map, key, value] -> [map]; operands stay rooted across a grow.
      if (state.stackDepth < 3) {
        return fault(ErrorCode::StackUnderflow);
      }
      const Value& mapSlot = state.stack[state.stackDepth - 3];
      const Value& keySlot = state.stack[state.stackDepth - 2];
      const Value& valueSlot = state.stack[state.stackDepth - 1];
      if (!mapSlot.isMap()) {
        return fault(ErrorCode::ScriptError);
      }
      MapKey key;
      if (!valueToMapKey(keySlot, key)) {
        return fault(ErrorCode::ScriptError);
      }
      if (!surface.heap->mapSet(surface.heap->map(mapSlot), key, valueSlot, surface.roots)) {
        return fault(ErrorCode::StackOverflow);
      }
      state.stackDepth -= 2; // drop key and value, leaving the map on top
      frame.pc++;
      break;
    }

    case Op::MAP_GET: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value key;
      Value mapValue;
      if (!popValue(state, key) || !popValue(state, mapValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!mapValue.isMap()) {
        return fault(ErrorCode::ScriptError);
      }
      MapKey mapKey;
      if (!valueToMapKey(key, mapKey)) {
        return fault(ErrorCode::ScriptError);
      }
      if (!pushValue(state, surface.heap->mapGet(surface.heap->map(mapValue), mapKey))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::MAP_HAS: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value key;
      Value mapValue;
      if (!popValue(state, key) || !popValue(state, mapValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!mapValue.isMap()) {
        return fault(ErrorCode::ScriptError);
      }
      MapKey mapKey;
      if (!valueToMapKey(key, mapKey)) {
        return fault(ErrorCode::ScriptError);
      }
      const Value has = Value::boolean(surface.heap->mapHas(surface.heap->map(mapValue), mapKey));
      if (!pushValue(state, has)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::MAP_DELETE: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value key;
      Value mapValue;
      if (!popValue(state, key) || !popValue(state, mapValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!mapValue.isMap()) {
        return fault(ErrorCode::ScriptError);
      }
      MapKey mapKey;
      if (!valueToMapKey(key, mapKey)) {
        return fault(ErrorCode::ScriptError);
      }
      surface.heap->mapDelete(surface.heap->map(mapValue), mapKey);
      if (!pushValue(state, mapValue)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::TYPE_CHECK: {
      Value value;
      if (!popValue(state, value)) {
        return fault(ErrorCode::StackUnderflow);
      }
      const bool match = static_cast<int32_t>(value.tag()) == static_cast<int32_t>(ins.a);
      if (!pushValue(state, Value::boolean(match))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::INSTANCE_OF: {
      Value value;
      if (!popValue(state, value)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (ins.a >= program.types.size()) {
        return fault(ErrorCode::ScriptError);
      }
      const bool result = value.isStruct() && value.typeId() == ins.a;
      if (!pushValue(state, Value::boolean(result))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::CALL: {
      const uint32_t argc = ins.b;
      if (state.stackDepth < argc) {
        return fault(ErrorCode::StackUnderflow);
      }
      // Resolve the callee's rule from the caller before pushCallFrame, which
      // can relocate the frame region and invalidate `frame`.
      const uint32_t calleeRuleFuncId = resolveCalleeRuleFuncId(program, frame, ins.a);
      ErrorCode err = ErrorCode::ScriptError;
      if (!pushCallFrame(state, program, ins.a, argc, kNoCaptures, true, 0, false,
                         ActionFrameBinding{0, 0, false}, false, err)) {
        return fault(err);
      }
      state.frames[state.frameDepth - 1].ruleFuncId = calleeRuleFuncId;
      // The caller pc was advanced inside pushCallFrame; the callee runs next.
      break;
    }

    case Op::SPAWN_RULE: {
      // Operand layout: a = child rule funcId. Spawns a fire-and-forget child
      // rule fiber and continues; nothing is pushed. The child runs in a later
      // round (the round-tick rule). Mirrors execSpawnRule in vm.ts.
      if (surface.childRuleSpawner == nullptr) {
        return fault(ErrorCode::HostError);
      }
      ErrorCode err = ErrorCode::HostError;
      if (!surface.childRuleSpawner->spawnChildRule(ins.a, err)) {
        return fault(err);
      }
      frame.pc++;
      break;
    }

    case Op::CALL_INDIRECT:
    case Op::CALL_INDIRECT_ARGS: {
      const uint32_t argc = ins.a;
      // Stack layout: [func, arg0, ..., arg(argc-1)]; the function reference
      // sits below the args.
      if (state.stackDepth <= argc) {
        return fault(ErrorCode::StackUnderflow);
      }
      const Value& funcRef = state.stack[state.stackDepth - argc - 1];
      if (!funcRef.isFunction()) {
        return fault(ErrorCode::ScriptError);
      }
      const uint32_t calleeId = funcRef.functionId();
      const uint32_t captures = funcRef.functionCaptures();
      const bool exactArity = ins.op == Op::CALL_INDIRECT;
      const uint32_t calleeRuleFuncId = resolveCalleeRuleFuncId(program, frame, calleeId);
      ErrorCode err = ErrorCode::ScriptError;
      if (!pushCallFrame(state, program, calleeId, argc, captures, exactArity, 1, false,
                         ActionFrameBinding{0, 0, false}, false, err)) {
        return fault(err);
      }
      state.frames[state.frameDepth - 1].ruleFuncId = calleeRuleFuncId;
      break;
    }

    case Op::ACTION_CALL: {
      // Operand layout: a = action slot, b = argc, c = call-site id. Enter the
      // action's entry function as a sync action frame; args lay out as locals
      // like a direct call, and the frame's action binding keys callsite state.
      const uint32_t actionSlot = ins.a;
      const uint32_t argc = ins.b;
      const uint32_t callSiteId = ins.c;
      if (!program.hasActions || actionSlot >= program.actions.size()) {
        return fault(ErrorCode::ScriptError);
      }
      if (argc > state.stackDepth) {
        return fault(ErrorCode::StackUnderflow);
      }
      const BytecodeAction& action = program.actions[actionSlot];
      // The action inherits the calling rule so a ctx.rule access inside its
      // body resolves to the enclosing rule's store. Resolve from the caller
      // before pushCallFrame, which can relocate the frame region.
      const uint32_t inheritedRuleFuncId = resolveFrameRuleFuncId(program, frame);
      ErrorCode err = ErrorCode::ScriptError;
      const ActionFrameBinding binding{actionSlot, callSiteId, false};
      if (!pushCallFrame(state, program, action.entryFuncId, argc, kNoCaptures, true, 0, true,
                         binding, true, err)) {
        return fault(err);
      }
      state.frames[state.frameDepth - 1].ruleFuncId = inheritedRuleFuncId;
      break;
    }

    case Op::ACTION_CALL_ASYNC: {
      // Operand layout: a = action slot, b = argc, c = call-site id. Spawns a
      // child fiber running the async bytecode action, allocates its result
      // handle, and pushes that handle for the following AWAIT. An
      // ACTION_CALL_ASYNC inside a sync action frame cannot suspend and faults.
      // Mirrors the bytecode-action branch of execActionCallAsync in vm.ts; host
      // async actions dispatch via HOST_ACTION_CALL_ASYNC (op 45), so op 43 only
      // takes the bytecode path (program.actions holds bytecode actions).
      const ActionFrameBinding* suspendBinding = currentActionBinding(state);
      if (suspendBinding != nullptr && !suspendBinding->isAsync) {
        return fault(ErrorCode::ScriptError);
      }
      if (surface.spawner == nullptr || surface.handles == nullptr) {
        return fault(ErrorCode::HostError);
      }
      const uint32_t actionSlot = ins.a;
      const uint32_t argc = ins.b;
      const uint32_t callSiteId = ins.c;
      if (!program.hasActions || actionSlot >= program.actions.size()) {
        return fault(ErrorCode::ScriptError);
      }
      if (argc > state.stackDepth) {
        return fault(ErrorCode::StackUnderflow);
      }
      // Backpressure on handle exhaustion: with no free handle, yield without
      // advancing the pc so the fiber re-enters the run queue and re-executes
      // this same dispatch next round. The check precedes every side effect (no
      // args popped, no child fiber spawned, no handle allocated, pc unchanged),
      // so the retry is an exact re-execution once an in-flight async settles a
      // slot.
      if (!surface.handles->hasCapacity()) {
        if (noteHandleBackpressure(state, frame.pc)) {
          return fault(ErrorCode::StackOverflow);
        }
        return RunResult::yielded();
      }
      clearHandleBackpressure(state);
      const BytecodeAction& action = program.actions[actionSlot];
      // Resolve the inherited rule from the caller before spawning; the spawned
      // child takes the args off the top of the operand stack as its locals.
      const uint32_t inheritedRuleFuncId = resolveFrameRuleFuncId(program, frame);
      const Span<const Value> args(state.stack + (state.stackDepth - argc), argc);
      ErrorCode err = ErrorCode::HostError;
      const uint32_t handleId = surface.spawner->spawnAsyncActionChild(
          action.entryFuncId, actionSlot, callSiteId, inheritedRuleFuncId, args, err);
      if (handleId == kNoHandleId) {
        return fault(err);
      }
      if (surface.observer != nullptr) {
        surface.observer->onBytecodeActionCallAsync(actionSlot, callSiteId, args);
      }
      state.stackDepth -= argc;
      if (!pushValue(state, Value::handle(handleId))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::MAKE_CLOSURE: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      const uint32_t captureCount = ins.b;
      if (state.stackDepth < captureCount) {
        return fault(ErrorCode::StackUnderflow);
      }
      uint32_t capturesHandle = kNoCaptures;
      if (captureCount > 0) {
        // The captures stay on the operand stack (rooted) across the allocation.
        if (!surface.heap->newCaptures(captureCount, surface.roots, capturesHandle)) {
          return fault(ErrorCode::StackOverflow);
        }
        CapturesObject* obj = surface.heap->captures(capturesHandle);
        const uint32_t base = state.stackDepth - captureCount;
        // Captures were pushed left-to-right; keep that order in the array.
        for (uint32_t i = 0; i < captureCount; i++) {
          obj->slots[i] = state.stack[base + i];
        }
        state.stackDepth = base;
      }
      if (!pushValue(state, Value::function(ins.a, capturesHandle))) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::LOAD_CAPTURE: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      if (frame.captures == kNoCaptures) {
        return fault(ErrorCode::ScriptError);
      }
      CapturesObject* obj = surface.heap->captures(frame.captures);
      if (ins.a >= obj->count) {
        return fault(ErrorCode::ScriptError);
      }
      if (!pushValue(state, obj->slots[ins.a])) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::STRUCT_NEW: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      // Operand `a` is reserved and must be 0.
      if (ins.a != 0) {
        return fault(ErrorCode::ScriptError);
      }
      uint32_t slotCount = 0;
      if (ins.b != kNoTypeIdx) {
        if (ins.b >= program.types.size()) {
          return fault(ErrorCode::ScriptError);
        }
        const TypeEntry& entry = program.types[ins.b];
        if (entry.tag == TypeTag::Struct) {
          slotCount = entry.structOf.slotCount;
        } else if (entry.tag == TypeTag::Atom) {
          // A host-registered nominal struct: resolve its field storage slot
          // count from the registry by atom id.
          if (surface.types == nullptr ||
              !surface.types->registeredStructSlotCount(entry.atom.atomId, slotCount)) {
            return fault(ErrorCode::ScriptError);
          }
        } else {
          return fault(ErrorCode::ScriptError);
        }
      }
      Value structValue;
      if (!surface.heap->newStruct(ins.b, slotCount, surface.roots, structValue)) {
        return fault(ErrorCode::StackOverflow);
      }
      if (!pushValue(state, structValue)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::STRUCT_COPY_EXCEPT: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      // Object-rest copy: build a struct of the operand type from the source,
      // dropping the excluded field names and mapping the rest by name. Mirrors
      // execStructCopyExcept in external/wendoo-lang/.../vm.ts.
      const uint32_t numExclude = ins.a;
      if (state.stackDepth < numExclude + 1u) {
        return fault(ErrorCode::StackUnderflow);
      }
      // Stack layout: [source, key0, ..., key(numExclude-1)]; keys and source
      // stay in place (rooted) across the result allocation below.
      const uint32_t keysBase = state.stackDepth - numExclude;
      const uint32_t sourceIdx = keysBase - 1;
      for (uint32_t i = 0; i < numExclude; i++) {
        if (!state.stack[keysBase + i].isString()) {
          return fault(ErrorCode::ScriptError);
        }
      }
      const Value source = state.stack[sourceIdx];
      if (!source.isStruct()) {
        return fault(ErrorCode::ScriptError);
      }

      uint32_t resultTypeIdx = ins.b;
      uint32_t resultSlots = 0;
      if (resultTypeIdx != kNoTypeIdx) {
        if (resultTypeIdx >= program.types.size() ||
            program.types[resultTypeIdx].tag != TypeTag::Struct) {
          return fault(ErrorCode::ScriptError);
        }
        resultSlots = program.types[resultTypeIdx].structOf.slotCount;
      }
      // An unsized result type becomes a copy of the source's own type.
      bool copyAll = false;
      if (resultSlots == 0) {
        resultTypeIdx = source.typeId();
        resultSlots = surface.heap->structOf(source)->slotCount;
        copyAll = true;
      }
      const bool sameAsSource = resultTypeIdx == source.typeId();

      Value result;
      if (!surface.heap->newStruct(resultTypeIdx, resultSlots, surface.roots, result)) {
        return fault(ErrorCode::StackOverflow);
      }
      StructObject* srcObj = surface.heap->structOf(source);
      StructObject* dstObj = surface.heap->structOf(result);
      if (copyAll) {
        for (uint32_t i = 0; i < srcObj->slotCount && i < dstObj->slotCount; i++) {
          dstObj->slots[i] = srcObj->slots[i];
        }
      }
      if (surface.types != nullptr && source.typeId() < program.types.size() &&
          program.types[source.typeId()].tag == TypeTag::Struct) {
        const TypeEntry::StructOf& srcStruct = program.types[source.typeId()].structOf;
        for (uint32_t i = 0; i < srcStruct.fieldsCount; i++) {
          const StructFieldRef& field = program.structFields[srcStruct.fieldsOffset + i];
          const StringRef& nameRef = program.strings[field.nameStringIdx];
          const char* nameBytes =
              reinterpret_cast<const char*>(program.stringData.data()) + nameRef.offset;
          const uint32_t nameLen = nameRef.length;
          bool excluded = false;
          for (uint32_t k = 0; k < numExclude && !excluded; k++) {
            const char* keyBytes = nullptr;
            uint32_t keyLen = 0;
            if (stringValueBytes(state.stack[keysBase + k], program, *surface.heap, keyBytes,
                                 keyLen) &&
                keyLen == nameLen &&
                (nameLen == 0 || std::memcmp(keyBytes, nameBytes, nameLen) == 0)) {
              excluded = true;
            }
          }
          if (excluded) {
            if (sameAsSource && field.fieldId < dstObj->slotCount) {
              dstObj->slots[field.fieldId] = kNilValue;
            }
          } else {
            uint32_t targetId = 0;
            if (surface.types->findStructField(resultTypeIdx, nameBytes, nameLen, targetId) &&
                field.fieldId < srcObj->slotCount && targetId < dstObj->slotCount) {
              dstObj->slots[targetId] = srcObj->slots[field.fieldId];
            }
          }
        }
      }
      state.stackDepth = sourceIdx;
      if (!pushValue(state, result)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::STRUCT_GET_FIELD: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      Value structValue;
      if (!popValue(state, structValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!structValue.isStruct()) {
        return fault(ErrorCode::ScriptError);
      }
      const Value field = readStructFieldById(surface, structValue, ins.a);
      if (!pushValue(state, field)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::STRUCT_SET_FIELD: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      // A pure store: the field slot is overwritten in place, never deep-copied.
      Value value;
      Value structValue;
      if (!popValue(state, value) || !popValue(state, structValue)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!structValue.isStruct()) {
        return fault(ErrorCode::ScriptError);
      }
      if (!writeStructFieldById(surface, structValue, ins.a, value)) {
        return fault(ErrorCode::ScriptError);
      }
      if (!pushValue(state, structValue)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::STRUCT_DEEP_COPY: {
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      if (state.stackDepth == 0) {
        return fault(ErrorCode::StackUnderflow);
      }
      // The source stays on the operand stack (rooted) across the allocating
      // copy; the result replaces it in place.
      Value copy;
      if (!surface.heap->deepCopy(state.stack[state.stackDepth - 1], surface.roots, copy)) {
        return fault(ErrorCode::StackOverflow);
      }
      state.stack[state.stackDepth - 1] = copy;
      frame.pc++;
      break;
    }

    case Op::GET_FIELD: {
      // Dynamic computed-key read: resolve the field name to its id and read
      // that field. A non-struct source or an unresolved name yields nil.
      Value fieldName;
      Value source;
      if (!popValue(state, fieldName) || !popValue(state, source)) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (!fieldName.isString()) {
        return fault(ErrorCode::ScriptError);
      }
      Value result = kNilValue;
      if (source.isStruct()) {
        if (surface.heap == nullptr) {
          return fault(ErrorCode::HostError);
        }
        const char* nameBytes = nullptr;
        uint32_t nameLen = 0;
        uint32_t fieldId = 0;
        if (surface.types != nullptr &&
            stringValueBytes(fieldName, program, *surface.heap, nameBytes, nameLen) &&
            surface.types->findStructField(source.typeId(), nameBytes, nameLen, fieldId)) {
          result = readStructFieldById(surface, source, fieldId);
        }
      }
      if (!pushValue(state, result)) {
        return fault(ErrorCode::StackOverflow);
      }
      frame.pc++;
      break;
    }

    case Op::SET_FIELD: {
      // Dynamic computed-key write: deep-copy the value (struct value-semantics),
      // resolve the field name to its id, and write that field. An unresolved
      // name leaves the struct unchanged.
      if (state.stackDepth < 3) {
        return fault(ErrorCode::StackUnderflow);
      }
      if (surface.heap == nullptr) {
        return fault(ErrorCode::HostError);
      }
      // The value is the stack top; deep-copy it while it stays rooted there.
      Value copied;
      if (!surface.heap->deepCopy(state.stack[state.stackDepth - 1], surface.roots, copied)) {
        return fault(ErrorCode::StackOverflow);
      }
      Value value;
      Value fieldName;
      popValue(state, value);
      popValue(state, fieldName);
      if (!fieldName.isString()) {
        return fault(ErrorCode::ScriptError);
      }
      const Value source = state.stack[state.stackDepth - 1];
      if (!source.isStruct()) {
        return fault(ErrorCode::ScriptError);
      }
      const char* nameBytes = nullptr;
      uint32_t nameLen = 0;
      uint32_t fieldId = 0;
      if (surface.types != nullptr &&
          stringValueBytes(fieldName, program, *surface.heap, nameBytes, nameLen) &&
          surface.types->findStructField(source.typeId(), nameBytes, nameLen, fieldId)) {
        if (!writeStructFieldById(surface, source, fieldId, copied)) {
          return fault(ErrorCode::ScriptError);
        }
      }
      // The source struct is already on the stack top as the result.
      frame.pc++;
      break;
    }

    case Op::YIELD: {
      // A YIELD inside a sync action frame cannot suspend and faults; outside
      // one (or inside an async action) it is a cooperative yield that suspends
      // and re-enqueues the fiber for the next round. Mirrors assertCanSuspend.
      const ActionFrameBinding* binding = currentActionBinding(state);
      if (binding != nullptr && !binding->isAsync) {
        return fault(ErrorCode::ScriptError);
      }
      frame.pc++;
      return RunResult::yielded();
    }

    case Op::TRY: {
      // Record a handler at the current frame/stack depths, with the signed
      // operand as the relative catch target. Mirrors vm.ts execTry.
      if (state.handlerDepth >= state.handlerLimit) {
        return fault(ErrorCode::StackOverflow);
      }
      if (state.handlerDepth >= state.handlerCapacity &&
          !ensureHandlerCapacity(state, state.handlerDepth + 1)) {
        return fault(ErrorCode::StackOverflow);
      }
      Handler& handler = state.handlers[state.handlerDepth++];
      handler.frameIndex = state.frameDepth;
      handler.stackHeight = state.stackDepth;
      handler.catchTarget = addRel(frame.pc, ins.a);
      frame.pc++;
      break;
    }

    case Op::END_TRY: {
      // Pop the innermost handler. An empty stack is a no-op, mirroring the
      // TS List.pop on an empty handler list.
      if (state.handlerDepth > 0) {
        state.handlerDepth--;
      }
      frame.pc++;
      break;
    }

    case Op::THROW: {
      // Pop the thrown value; an error value carries its classifier, anything
      // else throws a ScriptError (mirrors vm.ts execThrow).
      Value thrown;
      if (!popValue(state, thrown)) {
        return fault(ErrorCode::StackUnderflow);
      }
      const ErrorCode code = thrown.isErr() ? thrown.errorCode() : ErrorCode::ScriptError;
      RunResult escaped = RunResult::yielded();
      if (!throwError(state, code, frame.funcId, frame.pc, escaped)) {
        return escaped;
      }
      break;
    }

    case Op::RESERVED_111:
    case Op::RESERVED_112:
      // Producer-free reserved opcodes: decoded for numbering, never executed.
      return fault(ErrorCode::ScriptError);

    default:
      // Every opcode outside the implemented subset faults deterministically.
      return fault(ErrorCode::ScriptError);
    }
  }

  return RunResult::yielded();
}

} // namespace wendoo
