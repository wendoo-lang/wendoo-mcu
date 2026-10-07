#pragma once

#include <cstdint>

#include "core/platform/span.h"
#include "core/runtime/error-code.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/execution-state.h"
#include "core/runtime/host-action.h"
#include "core/runtime/host-function.h"
#include "core/runtime/program.h"
#include "core/runtime/result.h"
#include "core/runtime/value.h"
#include "core/runtime/vm-observer.h"

namespace wendoo {

class ManagedHeap;
class GcRoots;
class TypeRegistry;
class HandleTable;
struct RandomStream;
struct AsyncActionSpawner;
struct ChildRuleSpawner;

/**
 * Outcome of one dispatch-loop slice. Mirrors `VmStatus` in
 * external/wendoo-lang/packages/core/src/runtime/vm-types.ts. `Waiting`
 * is produced once async handles land; no current opcode emits it.
 */
enum class RunStatus : uint8_t {
  /** The root frame returned; the state's function completed. */
  Done,
  /** The instruction budget ran out; the state is suspended and resumable. */
  Yielded,
  /** The state parked on a pending async handle (`AWAIT`); resumed on settle. */
  Waiting,
  /** Execution faulted; the state must not be re-entered. */
  Fault,
};

/** Location of a faulting instruction. */
struct FaultSite {
  /** FuncId of the faulting function, or {@link kNoFuncId} for faults raised
   * outside any frame. */
  uint32_t funcId;
  /** Instruction index of the fault within the function body. */
  uint32_t pc;
};

/**
 * Result of {@link runExecution}: the slice status plus the payload of the
 * terminal statuses. Construct through the static factories.
 */
struct RunResult {
  RunStatus status;
  /** The root frame's return value. Meaningful only for `Done`. */
  Value result;
  /** The fault classifier. Meaningful only for `Fault`. */
  ErrorCode error;
  /** The fault location. Meaningful only for `Fault`. */
  FaultSite site;

  /** A `Done` result carrying the completed function's return value. */
  static constexpr RunResult done(Value result) {
    return RunResult{RunStatus::Done, result, ErrorCode::HostError, {kNoFuncId, 0}};
  }

  /** A `Yielded` result: budget exhausted, state suspended and resumable. */
  static constexpr RunResult yielded() {
    return RunResult{RunStatus::Yielded, kNilValue, ErrorCode::HostError, {kNoFuncId, 0}};
  }

  /** A `Waiting` result: the fiber parked on a pending handle, recorded in its
   * execution state's await site. */
  static constexpr RunResult waiting() {
    return RunResult{RunStatus::Waiting, kNilValue, ErrorCode::HostError, {kNoFuncId, 0}};
  }

  /** A `Fault` result carrying the classifier and faulting site. */
  static constexpr RunResult fault(ErrorCode error, uint32_t funcId, uint32_t pc) {
    return RunResult{RunStatus::Fault, kNilValue, error, {funcId, pc}};
  }
};

/**
 * Brain-wide host surface the dispatch loop runs against: the execution
 * context, the host-action binding table, and the optional passive observer.
 * Every pointer is non-owning. With a null `context`, the opcodes that need
 * runtime state (`LOAD_VAR_SLOT`, `STORE_VAR_SLOT`, `HOST_ACTION_CALL`)
 * fault `ErrorCode::HostError`.
 */
struct RuntimeSurface {
  /** Runtime state the surface-dependent opcodes read and write. */
  ExecutionContext* context = nullptr;

  /** Registered host actions, resolved by stable action id. */
  Span<const HostActionBinding> actions{};

  /** Passive tap on the host-binding surface, or null. */
  VmObserver* observer = nullptr;

  /**
   * Managed heap backing the container value types. Null when the host
   * registers no heap; a container opcode then faults `ErrorCode::HostError`.
   */
  ManagedHeap* heap = nullptr;

  /**
   * Garbage-collection root source the heap collects over (the fiber
   * scheduler). Null when no heap is configured.
   */
  GcRoots* roots = nullptr;

  /**
   * Random stream `MathRandom` draws from. Null when the program
   * makes no random host call; a `MathRandom` `HOST_CALL` then faults
   * `ErrorCode::HostError`.
   */
  RandomStream* rng = nullptr;

  /**
   * Type registry resolving program-local struct field names, struct slot
   * counts, and instantiated container typeIds. Null when the host registers
   * none; the dynamic computed-key struct opcodes then resolve no field, and
   * container results carry {@link kNoTypeIdx}.
   */
  const TypeRegistry* types = nullptr;

  /**
   * Registered target host functions, resolved by stable funcId for a
   * `HOST_CALL` whose funcId is at or above {@link TARGET_FUNC_ID_BASE}. Empty
   * when the host registers none; such a call then faults
   * `ErrorCode::ScriptError`.
   */
  Span<const TargetHostFuncBinding> hostFunctions{};

  /**
   * Async-handle table backing `HOST_CALL_ASYNC`, `HOST_ACTION_CALL_ASYNC`, and
   * `AWAIT`. Null when the scheduler configures no async capacity
   * (`maxHandles == 0`); the async opcodes then fault `ErrorCode::HostError`.
   */
  HandleTable* handles = nullptr;

  /**
   * Spawns child fibers for `ACTION_CALL_ASYNC` (async bytecode actions). Null
   * when the dispatch loop runs outside a scheduler; `ACTION_CALL_ASYNC` then
   * faults `ErrorCode::HostError`.
   */
  AsyncActionSpawner* spawner = nullptr;

  /**
   * Spawns fire-and-forget child-rule fibers for `SPAWN_RULE`. Null when the
   * dispatch loop runs outside a scheduler; `SPAWN_RULE` then faults
   * `ErrorCode::HostError`.
   */
  ChildRuleSpawner* childRuleSpawner = nullptr;
};

/**
 * Truthiness of `value` per the VM contract: unknown, void, nil, `false`,
 * numeric zero, the empty string, empty containers, error values, and a struct
 * whose native type's existence hook reports it gone are falsy; everything else
 * (including NaN numbers) is truthy. `program` resolves borrowed string
 * references; `heap` resolves container lengths and must be non-null whenever a
 * `List` or `Map` value can reach this call; `types` resolves a struct's
 * existence hook, and a null `types` consults none.
 */
bool isTruthy(const Value& value, const ProgramImage& program, const ManagedHeap* heap = nullptr,
              const TypeRegistry* types = nullptr);

/**
 * Writes rule variable `name` on the rule bound to the in-flight host dispatch
 * (`ctx.currentRuleFuncId`), allocating the backing store on demand. `name` and
 * `value` may be fresh unrooted managed values; the write pins them across its
 * own allocations. A write outside any rule is a no-op (returns true). Returns
 * false on a name that is neither a string nor a number, or when the heap
 * cannot back an allocation. Mirrors `setRuleVariable` in
 * external/wendoo-lang/packages/core/src/runtime/context.ts.
 */
bool setRuleVariable(ExecutionContext& ctx, ManagedHeap& heap, GcRoots* roots, const Value& name,
                     const Value& value);

/**
 * Push the entry frame for `funcId` onto `state` and seed its locals from
 * `args` (excess args are dropped; remaining local slots are nil). The state
 * must have its stack regions bound. Fails with `ErrorCode::HostError` when
 * `funcId` is out of bounds and `ErrorCode::StackOverflow` when the frame or
 * locals region is exhausted.
 */
Status startExecution(ExecutionState& state, const ProgramImage& program, uint32_t funcId,
                      Span<const Value> args);

/**
 * Run `state` until its function completes, the budget runs out, or a fault
 * occurs. Requires a positive {@link ExecutionState::budget} at entry;
 * entering with a non-positive budget is a host-contract violation reported
 * as a `Fault` with `ErrorCode::HostError`, leaving the state untouched.
 * Budget exhaustion suspends at an instruction boundary with every live
 * range preserved; re-arm the budget and call again to resume. No platform
 * exception escapes the loop; every fault is a deterministic {@link
 * RunResult} carrying an {@link ErrorCode}. `surface` provides the host
 * bindings and runtime state the surface-dependent opcodes dispatch
 * against.
 */
RunResult runExecution(ExecutionState& state, const ProgramImage& program,
                       const RuntimeSurface& surface = {});

} // namespace wendoo
