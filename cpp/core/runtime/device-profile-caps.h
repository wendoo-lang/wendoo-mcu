#pragma once

#include <cstdint>

namespace wendoo {

/**
 * Scheduling and per-fiber resource caps a {@link FiberScheduler} runs under.
 * The target/host builds the struct from the device profile and passes it to
 * the scheduler's constructor. Mirrors the union of the TypeScript
 * `SchedulerConfig` and the cap fields of `VmConfig` in
 * external/wendoo-lang/packages/core/src/runtime/vm-types.ts, which the
 * reference carries as flat fields on the device profile.
 *
 * Every `max*` field is a runtime upper-bound guard, never a preallocation
 * size: a fiber's regions start small and grow on demand toward these caps, and
 * a push past a cap faults `ErrorCode::StackOverflow`.
 */
struct DeviceProfileCaps {
  /**
   * Instruction budget granted to each fiber's slice of a scheduler round. One
   * unit is consumed per dispatched instruction; a fiber that reaches zero
   * suspends at the next instruction boundary and resumes in a later round.
   */
  int32_t defaultBudget;

  /**
   * Instruction budget granted to a synchronous page-lifecycle hook fiber for
   * its single slice; a hook that does not complete within it cannot suspend
   * and faults.
   */
  int32_t hookBudget;

  /**
   * Upper bound on concurrently live fibers, checked at spawn; a spawn past it
   * faults `ErrorCode::StackOverflow`.
   */
  uint32_t maxFibers;

  /** Per-fiber operand-stack depth cap in values. */
  uint32_t maxStackSize;

  /** Per-fiber locals-region depth cap in values (summed across live frames). */
  uint32_t maxLocalsSize;

  /** Per-fiber call-frame depth cap. */
  uint32_t maxFrameDepth;

  /** Per-fiber try-handler depth cap. */
  uint32_t maxHandlers;

  /**
   * Concurrent async-handle cap. Bounds the live handles that count against it;
   * a binding declaring `uncappedHandles` allocates outside this accounting.
   */
  uint32_t maxHandles;
};

} // namespace wendoo
