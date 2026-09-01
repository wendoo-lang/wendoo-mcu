#pragma once

#include <cstddef>
#include <cstdint>

#include "core/platform/span.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/mc-number.h"
#include "core/runtime/program.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/value.h"

namespace wendoo {

/** Sentinel call-site id marking execution outside any host-call dispatch. */
inline constexpr uint32_t kNoCallSiteId = 0xffffffffu;

/**
 * Outcome of a rule's most recent WHEN evaluation, recorded by the VM at the
 * WHEN boundary opcodes. Mirrors `RuleFiringState` in
 * external/wendoo-lang/packages/core/src/runtime/rule-services.ts.
 */
enum class RuleFiringState : uint8_t {
  /** The rule's most recent completed WHEN evaluation passed its gate. */
  DidFire = 0,
  /** The rule's most recent completed WHEN evaluation failed its gate. */
  DidNotFire = 1,
  /** The rule began its WHEN check and has not yet reached a gate. */
  Evaluating = 2,
};

/**
 * Brain-wide runtime state one execution observes: the think-loop time
 * stamps, the bound call site of an in-flight host dispatch, per-callsite
 * host state, and the brain variable slots. Mirrors the runtime-state surface
 * of `ExecutionContext` in
 * external/wendoo-lang/packages/core/src/runtime/context.ts for the
 * implemented opcode subset.
 *
 * The slot tables are sized to the loaded program and drawn from the shared
 * region by {@link bindSlots} (the brain runtime binds them at startup); they
 * are empty until then. A slot access past a table's size is a host-contract
 * violation the VM faults `ErrorCode::HostError`.
 */
struct ExecutionContext {
  /** Current think time in milliseconds. Stamped before each tick. */
  mc_number_t time = 0;

  /**
   * Milliseconds since the previous think; 0 until a previous think exists.
   * Stamped before each tick.
   */
  mc_number_t dt = 0;

  /** Current tick number. Incremented on each think. */
  uint32_t currentTick = 0;

  /**
   * Call-site id of the in-flight host dispatch, or {@link kNoCallSiteId}
   * outside one. Bound by the VM before invoking a host-action body.
   */
  uint32_t currentCallSiteId = kNoCallSiteId;

  /**
   * Rule funcId of the in-flight host-action dispatch, or {@link kNoFuncId}
   * outside one. Bound by the VM before invoking a host-action body.
   */
  uint32_t currentRuleFuncId = kNoFuncId;

  /** Brain variable slots, nil until stored. Sized by {@link bindSlots}. */
  Span<Value> variables{};

  /**
   * Brain-global System state slots backing `LOAD_SYSTEM_VAR` /
   * `STORE_SYSTEM_VAR`: one slot per registered System, indexed by the
   * linker-assigned store slot. Shared across all callsites, not reachable from
   * brain code, and written by reference (no deep copy) so a System's state
   * struct mutates in place. Nil until written; sized by {@link bindSlots}.
   */
  Span<Value> systemStore{};

  /** Per-callsite host-state slots, keyed by call-site id. */
  Span<Value> callSiteStates{};

  /** Present flags for {@link callSiteStates}; false reads as no state. */
  Span<bool> callSiteStatePresent{};

  /**
   * Bytecode-addressable per-callsite state slots backing `LOAD_CALLSITE_VAR` /
   * `STORE_CALLSITE_VAR`: a flat row-major pad of `callSiteCount` rows by
   * {@link callSiteSlotStride} columns, indexed by the active action's
   * call-site id and the operand slot index. Brain-instance-scoped: a slot
   * persists across action calls and page changes. Nil until written.
   */
  Span<Value> callSiteSlots{};

  /**
   * Columns per call site in {@link callSiteSlots}: the program's largest
   * callsite-var index plus one, 0 when the program uses no callsite vars.
   */
  uint32_t callSiteSlotStride = 0;

  /**
   * Whether each call site's persistent state has been allocated. Drives the
   * once-only action initializer hook: it fires the first time a call site is
   * activated and never again for the brain's lifetime. False until then.
   */
  Span<bool> callSiteAllocated{};

  /**
   * Rule-scoped variable storage: an outer managed map keyed by rule funcId,
   * each value an inner managed map of variable name to value. Nil until the
   * first rule-variable write allocates the outer map; brain-instance-scoped
   * (persists across action calls, pages, and fibers). The garbage collector
   * reaches the whole structure as one root by marking this value.
   */
  Value ruleVarStores = kNilValue;

  /**
   * Rule-ancestor edges (child ruleFuncId -> parent ruleFuncId) of the loaded
   * program, bound once at startup. Rule-variable resolution walks this chain:
   * a read starts at the current rule and falls through to ancestors, so a
   * nested rule that never wrote a variable reads an enclosing rule's value.
   * Empty (no walk past the current rule) until bound.
   */
  Span<const RuleAncestor> ruleAncestors{};

  /**
   * Per-rule firing records indexed by rule funcId: the outcome of each rule's
   * most recent WHEN evaluation. Sized and initialized to
   * {@link RuleFiringState::DidFire} by {@link bindSlots}; empty until then, in
   * which case every read returns `DidFire` and every write is dropped.
   * Runtime-internal: the records are not serialized and not traced.
   */
  Span<RuleFiringState> ruleFiring{};

  /**
   * Per-rule watcher slots indexed by rule funcId: the pending rule-trigger
   * handle of the rule watching this one for completion, or
   * {@link kNoHandleId} when none does. A rule holds at most one. Sized and
   * emptied by {@link bindSlots}; empty until then, in which case every read
   * returns {@link kNoHandleId} and every write is dropped. Runtime-internal:
   * the slots are not serialized and not traced.
   */
  Span<uint32_t> ruleWatchers{};

  /**
   * Per-rule abandonment marks indexed by rule funcId: true once the rule's
   * current firing has lost a fiber to a fault or a cancellation anywhere in
   * its cluster. Cleared when the rule spawns its next firing. Sized and
   * cleared by {@link bindSlots}; empty until then, in which case every read
   * returns false and every write is dropped. Runtime-internal: the marks are
   * not serialized and not traced.
   */
  Span<bool> ruleAbandoned{};

  /**
   * Allocates the slot tables from `arena`: `variableCount` brain-variable
   * slots, `callSiteCount` per-callsite host-state slots (initially absent),
   * the `callSiteCount` by `callSiteSlotStride` bytecode callsite-var pad
   * (initialized to nil) plus its allocation flags, `systemCount` brain-global
   * System store slots (initialized to nil), and `ruleRecordCount` entries in
   * each of the three per-rule tables: firing records (initialized to
   * {@link RuleFiringState::DidFire}), watcher slots (empty), and abandonment
   * marks (clear). Returns false when the arena cannot back them, leaving the
   * tables empty.
   *
   * Each variable slot is seeded from the matching `variableInitValues` entry,
   * an index into `constValues` or {@link kNoVariableInit}. A slot with no
   * entry, or whose entry is the sentinel, is initialized to nil.
   */
  bool bindSlots(RegionArena& arena, uint32_t variableCount, uint32_t callSiteCount,
                 uint32_t slotStride = 0, uint32_t systemCount = 0, uint32_t ruleRecordCount = 0,
                 Span<const uint32_t> variableInitValues = {},
                 Span<const ConstValue> constValues = {}) {
    Value* vars = arena.allocate<Value>(variableCount);
    Value* states = arena.allocate<Value>(callSiteCount);
    bool* present = arena.allocate<bool>(callSiteCount);
    const uint32_t slotTotal = callSiteCount * slotStride;
    Value* slots = arena.allocate<Value>(slotTotal);
    bool* allocated = arena.allocate<bool>(callSiteCount);
    Value* sysSlots = arena.allocate<Value>(systemCount);
    RuleFiringState* firing = arena.allocate<RuleFiringState>(ruleRecordCount);
    uint32_t* watchers = arena.allocate<uint32_t>(ruleRecordCount);
    bool* abandoned = arena.allocate<bool>(ruleRecordCount);
    if ((variableCount > 0 && vars == nullptr) ||
        (callSiteCount > 0 && (states == nullptr || present == nullptr || allocated == nullptr)) ||
        (slotTotal > 0 && slots == nullptr) || (systemCount > 0 && sysSlots == nullptr) ||
        (ruleRecordCount > 0 &&
         (firing == nullptr || watchers == nullptr || abandoned == nullptr))) {
      return false;
    }
    for (uint32_t i = 0; i < variableCount; i++) {
      vars[i] = kNilValue;
      if (i >= variableInitValues.size()) {
        continue;
      }
      const uint32_t initIdx = variableInitValues[i];
      if (initIdx == kNoVariableInit || initIdx >= constValues.size()) {
        continue;
      }
      Value seeded = kNilValue;
      if (constValueToRuntime(constValues[initIdx], seeded)) {
        vars[i] = seeded;
      }
    }
    for (uint32_t i = 0; i < slotTotal; i++) {
      slots[i] = kNilValue;
    }
    for (uint32_t i = 0; i < systemCount; i++) {
      sysSlots[i] = kNilValue;
    }
    for (uint32_t i = 0; i < ruleRecordCount; i++) {
      firing[i] = RuleFiringState::DidFire;
      watchers[i] = kNoHandleId;
      abandoned[i] = false;
    }
    variables = {vars, variableCount};
    callSiteStates = {states, callSiteCount};
    callSiteStatePresent = {present, callSiteCount};
    callSiteSlots = {slots, slotTotal};
    callSiteSlotStride = slotStride;
    callSiteAllocated = {allocated, callSiteCount};
    systemStore = {sysSlots, systemCount};
    ruleFiring = {firing, ruleRecordCount};
    ruleWatchers = {watchers, ruleRecordCount};
    ruleAbandoned = {abandoned, ruleRecordCount};
    return true;
  }

  /**
   * The outcome of `ruleFuncId`'s most recent completed WHEN evaluation.
   * Returns {@link RuleFiringState::DidFire} when no rule is in scope, the
   * records are unbound, or `ruleFuncId` is past the record table -- a rule that
   * never wrote a record reads as fired.
   */
  RuleFiringState ruleFiringState(uint32_t ruleFuncId) const {
    if (ruleFuncId == kNoFuncId || ruleFuncId >= ruleFiring.size()) {
      return RuleFiringState::DidFire;
    }
    return ruleFiring[ruleFuncId];
  }

  /**
   * Records `state` for `ruleFuncId`. A no-op when no rule is in scope, the
   * records are unbound, or `ruleFuncId` is past the record table.
   */
  void setRuleFiringState(uint32_t ruleFuncId, RuleFiringState state) {
    if (ruleFuncId == kNoFuncId || ruleFuncId >= ruleFiring.size()) {
      return;
    }
    ruleFiring[ruleFuncId] = state;
  }

  /**
   * The pending rule-trigger handle watching `ruleFuncId` for completion, or
   * {@link kNoHandleId} when the slot is empty, no rule is in scope, or the
   * slots are unbound.
   */
  uint32_t ruleWatcher(uint32_t ruleFuncId) const {
    if (ruleFuncId == kNoFuncId || ruleFuncId >= ruleWatchers.size()) {
      return kNoHandleId;
    }
    return ruleWatchers[ruleFuncId];
  }

  /**
   * Parks `handleId` in `ruleFuncId`'s watcher slot, replacing whatever it
   * held. A no-op when no rule is in scope or the slots are unbound.
   */
  void setRuleWatcher(uint32_t ruleFuncId, uint32_t handleId) {
    if (ruleFuncId == kNoFuncId || ruleFuncId >= ruleWatchers.size()) {
      return;
    }
    ruleWatchers[ruleFuncId] = handleId;
  }

  /** Empties `ruleFuncId`'s watcher slot. */
  void clearRuleWatcher(uint32_t ruleFuncId) { setRuleWatcher(ruleFuncId, kNoHandleId); }

  /**
   * True when `ruleFuncId`'s current firing lost a fiber to a fault or a
   * cancellation anywhere in its cluster. False when no rule is in scope or the
   * marks are unbound.
   */
  bool isRuleAbandoned(uint32_t ruleFuncId) const {
    if (ruleFuncId == kNoFuncId || ruleFuncId >= ruleAbandoned.size()) {
      return false;
    }
    return ruleAbandoned[ruleFuncId];
  }

  /**
   * Records that `ruleFuncId`'s current firing was abandoned. A no-op when no
   * rule is in scope or the marks are unbound.
   */
  void markRuleAbandoned(uint32_t ruleFuncId) {
    if (ruleFuncId == kNoFuncId || ruleFuncId >= ruleAbandoned.size()) {
      return;
    }
    ruleAbandoned[ruleFuncId] = true;
  }

  /**
   * Drops `ruleFuncId`'s abandonment mark, which its next firing begins
   * without.
   */
  void clearRuleAbandoned(uint32_t ruleFuncId) {
    if (ruleFuncId == kNoFuncId || ruleFuncId >= ruleAbandoned.size()) {
      return;
    }
    ruleAbandoned[ruleFuncId] = false;
  }

  /**
   * Reads bytecode callsite-var slot `idx` of `callSiteId`; nil when the index
   * is past the slot stride (mirrors an unwritten slot). `callSiteId` must be
   * within {@link callSiteAllocated}.
   */
  Value callSiteSlot(uint32_t callSiteId, uint32_t idx) const {
    if (idx >= callSiteSlotStride) {
      return kNilValue;
    }
    return callSiteSlots[callSiteId * callSiteSlotStride + idx];
  }

  /**
   * Writes bytecode callsite-var slot `idx` of `callSiteId`. Requires
   * `idx` < {@link callSiteSlotStride} and `callSiteId` within the pad.
   */
  void setCallSiteSlot(uint32_t callSiteId, uint32_t idx, const Value& value) {
    callSiteSlots[callSiteId * callSiteSlotStride + idx] = value;
  }

  /**
   * Marks `callSiteId`'s persistent state allocated, returning true the first
   * time (when the action's one-time initializer hook must run) and false
   * thereafter.
   */
  bool ensureCallSite(uint32_t callSiteId) {
    if (callSiteAllocated[callSiteId]) {
      return false;
    }
    callSiteAllocated[callSiteId] = true;
    return true;
  }

  /**
   * True when the current call site holds host state. Requires
   * {@link currentCallSiteId} to be bound and within {@link callSiteStates}.
   */
  bool hasCallSiteState() const { return callSiteStatePresent[currentCallSiteId]; }

  /**
   * The host state of the current call site. Meaningful only when
   * {@link hasCallSiteState} is true.
   */
  const Value& callSiteState() const { return callSiteStates[currentCallSiteId]; }

  /** Writes the host state of the current call site. */
  void setCallSiteState(const Value& value) {
    callSiteStates[currentCallSiteId] = value;
    callSiteStatePresent[currentCallSiteId] = true;
  }

  /** Drops the host state of the current call site. */
  void clearCallSiteState() {
    callSiteStates[currentCallSiteId] = kNilValue;
    callSiteStatePresent[currentCallSiteId] = false;
  }
};

} // namespace wendoo
