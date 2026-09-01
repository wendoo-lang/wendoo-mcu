#pragma once

#include <cstdint>
#include <type_traits>

#include "core/runtime/error-code.h"
#include "core/runtime/mc-number.h"
#include "core/runtime/program.h"
#include "core/runtime/value.h"

namespace wendoo {

class StackRegionAllocator;

/**
 * State recorded when a fiber parks on a pending async handle (`AWAIT`), used
 * to restore its frame and operand stacks and resume at the instruction past
 * the await. Mirrors `AwaitSite` in
 * external/wendoo-lang/packages/core/src/runtime/vm-types.ts.
 */
struct AwaitSite {
  /** Absolute pc within the awaiting frame's function to resume at. */
  uint32_t resumePc;
  /** Operand-stack depth to truncate to before pushing the resolved value. */
  uint32_t stackHeight;
  /** Frame-stack depth to truncate to before resuming. */
  uint32_t frameDepth;
  /** The handle the fiber is waiting on. */
  uint32_t handleId;
};

/**
 * Per-frame binding describing the action call and call site whose state
 * slots back the frame. Mirrors `ActionFrameBinding` in
 * external/wendoo-lang/packages/core/src/runtime/vm-types.ts with the
 * action identified by stable id.
 */
struct ActionFrameBinding {
  /** Stable id of the bound action. */
  uint32_t actionId;
  /** Call-site id keying per-callsite state. */
  uint32_t callSiteId;
  /** True when the frame was entered through an async action call. */
  bool isAsync;
};

/**
 * Single call frame on an execution state's frame stack. Mirrors `Frame` in
 * external/wendoo-lang/packages/core/src/runtime/vm-types.ts. `captures`,
 * `ruleFuncId`, and the action binding are carried for the closure, rule
 * bookkeeping, and action-dispatch opcodes.
 */
struct Frame {
  /** FuncId of the executing function. */
  uint32_t funcId;
  /** Index of the next instruction within the function's body. */
  uint32_t pc;
  /** Operand-stack depth at frame entry; `RET` truncates the stack to it. */
  uint32_t base;
  /** First slot of this frame's locals in the execution state's locals region. */
  uint32_t localsOffset;
  /** Number of local slots, including params. */
  uint32_t localsCount;
  /** Captures handle, or {@link kNoCaptures} when the frame has none. */
  uint32_t captures;
  /** FuncId of the owning rule, or {@link kNoFuncId} when none. */
  uint32_t ruleFuncId;
  /** True when {@link actionBinding} is meaningful. */
  bool hasActionBinding;
  /** The action call backing this frame. Meaningful only when {@link hasActionBinding}. */
  ActionFrameBinding actionBinding;
};

/**
 * One try-handler on a fiber's handler stack. Mirrors `Handler` in
 * external/wendoo-lang/packages/core/src/runtime/vm-types.ts. A handler
 * carries only indices and a pc - no `Value`s - so the handler stack is never
 * a garbage-collection root.
 */
struct Handler {
  /** Frame depth at `TRY`; `THROW` truncates the frame stack to it. */
  uint32_t frameIndex;
  /** Operand-stack depth at `TRY`; `THROW` truncates the stack to it. */
  uint32_t stackHeight;
  /** Absolute pc within the catching frame's function `THROW` jumps to. */
  uint32_t catchTarget;
};

/**
 * One VM execution thread's complete runtime state: the operand stack, the
 * frame stack, the frame-locals region, the try-handler stack, and the
 * instruction budget. Each region is referenced by a base pointer plus a depth
 * cap (the `*Limit` guard) and a current allocated capacity (the `*Capacity`
 * that grows on demand toward the cap); the state never owns storage. A push
 * past the live capacity grows the region through {@link allocator} (when set);
 * a push past the cap faults `StackOverflow`. Suspension (budget exhaustion or
 * `YIELD`) preserves every live range, so a suspended state resumes by
 * re-arming {@link budget} and re-entering the dispatch loop.
 */
struct ExecutionState {
  /** Operand-stack region base. */
  Value* stack;
  /** Operand-stack depth cap; pushing past it faults `StackOverflow`. */
  uint32_t stackLimit;
  /** Allocated operand-stack capacity in slots; grows on demand toward the cap. */
  uint32_t stackCapacity;
  /** Live operand count; the top of stack is `stack[stackDepth - 1]`. */
  uint32_t stackDepth;

  /** Locals-region base; frames hold contiguous slot runs in push order. */
  Value* locals;
  /** Locals-region depth cap; exceeding it faults `StackOverflow`. */
  uint32_t localsLimit;
  /** Allocated locals capacity in slots; grows on demand toward the cap. */
  uint32_t localsCapacity;
  /** Live local-slot count across all frames. */
  uint32_t localsDepth;

  /** Frame-stack region base. */
  Frame* frames;
  /** Frame-stack depth cap; pushing past it faults `StackOverflow`. */
  uint32_t frameLimit;
  /** Allocated frame-stack capacity in slots; grows on demand toward the cap. */
  uint32_t frameCapacity;
  /** Live frame count; the current frame is `frames[frameDepth - 1]`. */
  uint32_t frameDepth;

  /** Try-handler stack base; grown on demand like the value regions. */
  Handler* handlers;
  /** Handler-stack depth cap; pushing past it faults `StackOverflow`. */
  uint32_t handlerLimit;
  /** Allocated handler capacity in slots; grows on demand toward the cap. */
  uint32_t handlerCapacity;
  /** Live handler count; the innermost handler is `handlers[handlerDepth - 1]`. */
  uint32_t handlerDepth;

  /**
   * Grow-on-demand backing allocator for the four regions, or null when the
   * regions are fixed (pre-allocated to their capacity, as in standalone test
   * states). A grow re-derives the region base, so never cache a base across a
   * push that can grow. The allocator must outlive every state that points at
   * it.
   */
  StackRegionAllocator* allocator;

  /**
   * Remaining instruction budget. Each dispatched instruction consumes one
   * unit; the dispatch loop requires a positive budget at entry and suspends
   * cleanly at the first instruction boundary where it reaches zero.
   */
  int32_t budget;

  /**
   * Set when the running fiber is cancelled mid-slice (a host body requested a
   * page change or restart). The dispatch loop checks it at each instruction
   * boundary and stops the slice, abandoning the rest of the current body.
   */
  bool cancelled = false;

  /**
   * Set while the fiber is parked on a pending async handle; {@link await}
   * records where to resume. Cleared when the scheduler resumes the fiber.
   */
  bool awaiting = false;

  /** The park site; meaningful only while {@link awaiting}. */
  AwaitSite await{};

  /**
   * Set when a handle settled to rejected/cancelled and the fiber must throw
   * {@link injectedError} at its next instruction before resuming normal
   * dispatch. Mirrors `Fiber.pendingInjectedThrow` in vm.ts.
   */
  bool pendingInjectedThrow = false;

  /** The error injected on resume; meaningful only while {@link pendingInjectedThrow}. */
  ErrorCode injectedError = ErrorCode::ScriptError;

  /**
   * Handle id of the pending async-action result handle when this state backs a
   * child fiber spawned by `ACTION_CALL_ASYNC`; `0` (the {@link kNoHandleId}
   * sentinel) otherwise. The scheduler settles it when the child completes
   * (resolve on `Done`, reject on `Fault`, cancel on cancellation). Mirrors
   * `Fiber.asyncResultHandleId` in
   * external/wendoo-lang/packages/core/src/runtime/vm-types.ts.
   */
  uint32_t asyncResultHandleId = 0;

  /**
   * Logical tick time captured when an `ACTION_CALL_ASYNC` child fiber was spawned
   * (meaningful while {@link asyncResultHandleId} is set). The scheduler stamps it
   * on the shared context's `time` for each of the child's slices.
   */
  mc_number_t dispatchTime = 0;

  /**
   * Program counter of the async dispatch this fiber last parked on for want of
   * a free async handle, paired with {@link handleBackpressureRounds}. Mirrors
   * `Fiber.handleBackpressurePc` in
   * external/wendoo-lang/packages/core/src/runtime/vm-types.ts.
   */
  uint32_t handleBackpressurePc = 0;

  /**
   * Consecutive rounds this fiber has re-executed the dispatch at
   * {@link handleBackpressurePc} without a free handle. Reset to zero by a
   * dispatch that allocates, and by a backpressure at a different pc. Reaching
   * {@link kHandleBackpressureFaultRounds} faults the fiber `StackOverflow`.
   */
  uint32_t handleBackpressureRounds = 0;
};

/**
 * Consecutive rounds one fiber may re-execute the same async dispatch without a
 * free async handle before the runtime faults it `ErrorCode::StackOverflow`.
 * A dispatch that allocates clears the count. Mirrors
 * `HANDLE_BACKPRESSURE_FAULT_ROUNDS` in
 * external/wendoo-lang/packages/core/src/runtime/vm-types.ts.
 */
inline constexpr uint32_t kHandleBackpressureFaultRounds = 8192;

static_assert(std::is_trivially_copyable_v<Frame>, "execution state stays trivially copyable");
static_assert(std::is_trivially_copyable_v<Handler>, "execution state stays trivially copyable");
static_assert(std::is_trivially_copyable_v<ExecutionState>,
              "execution state stays trivially copyable");

} // namespace wendoo
