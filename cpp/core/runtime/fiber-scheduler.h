#pragma once

#include <cstdint>

#include "core/platform/span.h"
#include "core/runtime/async-action-spawner.h"
#include "core/runtime/child-rule-spawner.h"
#include "core/runtime/device-profile-caps.h"
#include "core/runtime/execution-state.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/pool.h"
#include "core/runtime/program.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/result.h"
#include "core/runtime/rule-subtree-liveness.h"
#include "core/runtime/stack-region.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"

namespace wendoo {

/**
 * Slot counts a freshly spawned fiber's four regions (operand stack, locals,
 * frames, handlers) start at before growing on demand toward their `kMax*`
 * caps. Each must be at least one slot.
 */
inline constexpr uint32_t kInitialStackSlots = 8;
inline constexpr uint32_t kInitialLocalsSlots = 4;
inline constexpr uint32_t kInitialFrameSlots = 2;
inline constexpr uint32_t kInitialHandlerSlots = 1;

/** Sentinel fiber id; real ids start at 1. */
inline constexpr uint32_t kNoFiberId = 0;

/**
 * Lifecycle state of a fiber. Mirrors `FiberState` in
 * external/wendoo-lang/packages/core/src/runtime/vm-types.ts.
 */
enum class FiberState : uint8_t {
  /** Eligible for a slice in a scheduler round. */
  Runnable,
  /** Blocked on a pending async handle. Unproduced until handles land. */
  Waiting,
  /** The fiber's function completed. */
  Done,
  /** The fiber faulted and must not be re-entered. */
  Fault,
  /** The fiber was cancelled by the host. */
  Cancelled,
};

/**
 * One live fiber: its id, lifecycle state, and execution state. The four
 * execution regions are slab-backed blocks the scheduler reserves at spawn and
 * releases at reclaim; `exec` holds their bases and capacities directly (no
 * separate workspace), and grows them on demand through the shared
 * {@link StackRegionAllocator}.
 */
struct FiberRecord {
  /** Scheduler-assigned fiber id, unique for the scheduler's lifetime. */
  uint32_t id;
  FiberState state;
  ExecutionState exec;
  /** Next fiber in the run queue (intrusive FIFO), or nullptr at the tail. */
  FiberRecord* nextRunnable;
  /**
   * Func id of the root rule whose subtree this child-rule fiber belongs to, set
   * when spawned by `SPAWN_RULE`. {@link kNoFuncId} for root-rule, async-action
   * child, and hook fibers, so its presence marks a child-rule fiber. Mirrors
   * `Fiber.rootRuleFuncId` in vm.ts.
   */
  uint32_t rootRuleFuncId;
  /**
   * Func id of the rule this fiber runs: its entry function when that function
   * is a rule entry, the dispatching rule for an async-action child fiber, and
   * {@link kNoFuncId} for a fiber that belongs to no rule. Rule-cluster
   * membership walks up from here through the program's rule-ancestor chain.
   * Mirrors `Fiber.ruleFuncId` in vm.ts.
   */
  uint32_t ruleFuncId;
};

/**
 * Cooperative fiber scheduler with round-based ticks. A `tick()` is one
 * round: every fiber in the runnable queue at entry receives exactly one
 * {@link DeviceProfileCaps::defaultBudget} slice in FIFO order, and anything
 * enqueued while the round runs joins the next round. A fiber's record is drawn
 * from a pool and its execution regions are slab-backed blocks - both over one
 * shared {@link RegionArena} - and the run queue is an intrusive FIFO threaded
 * through the records; spawn faults `ErrorCode::StackOverflow` at
 * {@link DeviceProfileCaps::maxFibers} or when the region is exhausted. Mirrors
 * `FiberScheduler` in
 * external/wendoo-lang/packages/core/src/runtime/vm.ts under the
 * round-tick semantics.
 *
 * Single-entry: only the host think loop may call {@link tick}; host
 * callbacks and action bodies must never re-enter it.
 *
 * The scheduler is also the managed heap's {@link GcRoots} source: it holds
 * every live fiber, so a collection enumerates all reachable values from the
 * fibers' operand stacks and locals plus the bound execution context's brain
 * variables and per-callsite state.
 */
class FiberScheduler : public GcRoots,
                       public AsyncActionSpawner,
                       public ChildRuleSpawner,
                       public RuleSubtreeLiveness {
public:
  /**
   * A scheduler executing `program` against `surface` under `caps`. The caps
   * supply the per-round instruction budget and every per-fiber resource limit;
   * the target/host builds them from the device profile. Each fiber's four
   * execution regions start small (the `kInitial*Slots` counts) and grow on
   * demand toward the caps; their blocks, and the fiber records, are drawn from
   * `arena`. `arena` is the shared VM working-memory block - typically the same
   * one the program image was decoded into - and must outlive the scheduler.
   */
  FiberScheduler(const ProgramImage& program, const RuntimeSurface& surface, RegionArena& arena,
                 const DeviceProfileCaps& caps);

  /**
   * Spawns a runnable fiber executing `funcId` with no arguments and
   * enqueues it. The new fiber joins the round a subsequent `tick()` opens.
   * Fails with `ErrorCode::StackOverflow` when the live-fiber count is already
   * {@link DeviceProfileCaps::maxFibers} or the region cannot back the fiber's
   * record or initial execution regions, and with the {@link startExecution}
   * code when the entry frame cannot be pushed.
   */
  Result<uint32_t> spawn(uint32_t funcId);

  /**
   * Runs a page-lifecycle hook function to completion synchronously, outside
   * the round queue, as a sync bytecode action frame: the entry frame carries
   * an action binding ({@link actionId}, {@link callSiteId}, not async) so the
   * hook's `LOAD_CALLSITE_VAR` / `STORE_CALLSITE_VAR` resolve and a `YIELD`
   * inside it faults. The hook gets one {@link DeviceProfileCaps::hookBudget} slice
   * and must finish in it. Returns ok when the hook completes; fails with the fault
   * code when it faults, and with `ErrorCode::ScriptError` when it cannot
   * finish in one slice (it suspended). Mirrors `runBytecodeHook` in
   * external/wendoo-lang/packages/core/src/runtime/brain-runtime.ts.
   */
  Status runActionHook(uint32_t funcId, uint32_t actionId, uint32_t callSiteId);

  /**
   * Runs a System lifecycle function (`init` / `think`) to completion
   * synchronously, outside the round queue, as a plain bytecode frame with no
   * action binding. The function gets one {@link DeviceProfileCaps::hookBudget}
   * slice and must finish in it, receiving the injected `ctx` only when its
   * bytecode declares the param. Returns ok when it completes; fails with the
   * fault code when it faults, and with `ErrorCode::ScriptError` when it cannot
   * finish in one slice (it suspended). Mirrors `runSystemFunction` in
   * external/wendoo-lang/packages/core/src/runtime/brain-runtime.ts.
   */
  Status runSystemFunction(uint32_t funcId);

  /**
   * Spawns a child fiber for an async bytecode action and returns its pending
   * result handle id. Implements {@link AsyncActionSpawner}; the dispatch loop
   * calls it through {@link RuntimeSurface::spawner} for `ACTION_CALL_ASYNC`.
   */
  uint32_t spawnAsyncActionChild(uint32_t entryFuncId, uint32_t actionId, uint32_t callSiteId,
                                 uint32_t ruleFuncId, Span<const Value> args,
                                 ErrorCode& err) override;

  /**
   * Spawns a fire-and-forget child-rule fiber for `funcId`, linked to the fiber
   * currently executing this round, and holds it in the same-think spawn drain.
   * The child runs later in the current {@link tick}, after its parent's slice,
   * as a synchronous cascade. Implements {@link ChildRuleSpawner}; the dispatch
   * loop calls it through {@link RuntimeSurface::childRuleSpawner} for
   * `SPAWN_RULE`.
   */
  bool spawnChildRule(uint32_t funcId, ErrorCode& err) override;

  /**
   * Cancels every live child-rule fiber (those carrying a subtree root). The
   * page-scoped cancellation cascade: exactly one page is active, so every live
   * child-rule fiber descends from one of its root rules. Each cancel removes
   * the fiber from the run queue, so the cascade is safe mid-round. Root-rule,
   * async-action child, and hook fibers are left untouched.
   */
  void cancelChildRuleFibers();

  /**
   * Returns true when a live (runnable or waiting) child-rule fiber belongs to
   * the subtree of root rule `rootRuleFuncId`. Lets a root rule hold its re-fire
   * while any of its descendant child rules is still in flight.
   */
  bool hasLiveDescendantOfRoot(uint32_t rootRuleFuncId);

  /**
   * Returns true when any live (runnable or waiting) fiber belongs to
   * `ruleFuncId`'s cluster -- its own fiber, or one whose rule reaches
   * `ruleFuncId` through the program's rule-ancestor chain. Child-rule fibers
   * held in the current tick's spawn drain are runnable and count. Implements
   * {@link RuleSubtreeLiveness}.
   */
  bool hasLiveRuleSubtree(uint32_t ruleFuncId) override;

  /** Cancels the fiber holding `fiberId`; a no-op when it is not live. */
  void cancel(uint32_t fiberId);

  /**
   * The async-handle table backing this scheduler's fibers. The host loop and
   * async host bodies settle handles through it; the dispatch loop reaches it
   * via the runtime surface.
   */
  HandleTable& handles() { return handles_; }

  /**
   * Drains every settled handle: for each, resumes the fibers waiting on it
   * (restoring their await sites, queuing a resolved value or a pending throw)
   * and then frees the handle. Resumed fibers enqueue as runnable, so they join
   * the next round (the round-tick rule). Safe to call from the host loop after
   * external callbacks have settled handles out of band. Mirrors the
   * `onHandleCompleted` drain in
   * external/wendoo-lang/packages/core/src/runtime/vm.ts.
   */
  void drainCompletedHandles();

  /**
   * Runs one round: every fiber runnable at entry gets one budget slice in FIFO
   * order. After each slice, the child rules that slice spawned drain within
   * this same round as a synchronous cascade ({@link drainSpawnedSubtrees}).
   * Fibers enqueued during the round for another reason -- a budget-exhaustion
   * re-enqueue, a `YIELD`, or a settled handle's resumed waiter -- run in the
   * next round. Returns the number of fibers that received a slice, including
   * drained child-rule fibers.
   */
  uint32_t tick();

  /**
   * Reclaims finished fibers (`Done`/`Fault`/`Cancelled`): releases each one's
   * four execution regions back to the region allocator and its record back to
   * the pool. A suspended (`Runnable` via `YIELD` or budget) fiber keeps its
   * regions across rounds; only a terminal fiber releases them. Order is
   * unconstrained - slots and blocks recycle independently.
   */
  void sweep();

  /** The record holding `fiberId`, or nullptr when none does. */
  const FiberRecord* fiber(uint32_t fiberId) const;

  /** Number of record slots currently in use. */
  uint32_t liveCount() const;

  /** The shared region backing this scheduler's pools. */
  RegionArena& arena() { return arena_; }

  /**
   * Marks every live garbage-collection root into `marker`: each live fiber's
   * operand stack and locals, plus the bound execution context's brain
   * variables and present per-callsite state. Implements {@link GcRoots}.
   */
  void enumerateRoots(GcMarker& marker) override;

private:
  // Reserves a fiber's record and four execution regions and pushes its entry
  // frame for `funcId`, without enqueuing it. Returns the record, or nullptr
  // with `err` set on a cap, region-exhaustion, or entry-frame failure. Shared
  // by spawn (which then enqueues) and the synchronous hook runner. When
  // `inlineId` is true the record takes its id from the descending inline space
  // (hook and async-action child fibers). `args` seed the entry frame's locals.
  FiberRecord* allocFiber(uint32_t funcId, bool inlineId, Span<const Value> args, ErrorCode& err);

  FiberRecord* findFiber(uint32_t fiberId);
  // Moves `record` to a terminal state (Done, Fault, or Cancelled) and runs the
  // settle walk for it. Every terminal transition goes through here. Mirrors
  // transitionState in vm.ts.
  void transitionTerminal(FiberRecord& record, FiberState terminal);
  // The settle walk: takes the finishing fiber's rule and walks it and its
  // ancestors. A fiber that faulted or was cancelled marks every rule on that
  // walk -- the whole set of clusters it belonged to -- as an abandoned firing.
  // Then, for each rule whose cluster has emptied, resolves the pending trigger
  // handle in that rule's watcher slot and clears the slot: true on a DidFire
  // record with no abandonment mark, false otherwise. Call only once
  // `record.state` is terminal. Mirrors settleRuleWatchers in vm.ts.
  void settleRuleWatchers(const FiberRecord& record, FiberState cause);
  // Cancels a live (runnable or waiting) record: detaches it from any awaited
  // handle, cancels a pending async-action result handle, marks it Cancelled,
  // and removes it from the run queue. Shared by cancel() and the cascade.
  void cancelRecord(FiberRecord& record);
  void enqueue(FiberRecord* record);
  FiberRecord* dequeue();
  void removeFromQueue(FiberRecord* record);
  // Appends a freshly spawned child-rule fiber to the same-think spawn drain.
  void enqueueSpawn(FiberRecord* record);

  // Runs one budget slice of `record` and routes its outcome exactly as the main
  // round does: stamps a frozen dispatch time for an async-action child, exposes
  // it as running_ so a SPAWN_RULE links its child, and on return re-enqueues a
  // yielded fiber, parks a waiting one on its handle, or settles a terminal
  // async-action child's result handle. A slice that self-cancels mid-run keeps
  // its Cancelled state. Shared by tick() and drainSpawnedSubtrees().
  void runFiberSlice(FiberRecord* record);

  // Drains the child-rule fibers spawned during the just-finished slice as a
  // same-think synchronous cascade, depth-first: each child runs a slice and any
  // grandchildren it spawns drain before that child's next sibling, so one
  // subtree completes before the next begins. A child that parks (AWAIT), yields,
  // or exhausts its budget takes its normal next-round or handle-wait path.
  // Returns the number of child-rule slices run.
  uint32_t drainSpawnedSubtrees();

  // Resumes a single waiting fiber whose handle `h` has settled: restores its
  // await site, queues the resolved value or a pending injected throw, marks it
  // runnable, and enqueues it. A no-op when the fiber is no longer waiting on
  // `h`. Mirrors resumeFiberFromHandle in vm.ts.
  void resumeFiberFromHandle(FiberRecord& record, const Handle& h);

  // Releases all four execution regions of `exec` back to the region allocator.
  void releaseRegions(ExecutionState& exec);

  const ProgramImage& program_;
  RuntimeSurface surface_;
  RegionArena& arena_;
  DeviceProfileCaps caps_;
  Pool<FiberRecord> records_;
  // Grow-on-demand backing for every fiber's stack/locals/frame/handler regions.
  StackRegionAllocator regions_;
  // Pending async handles backing AWAIT; pool-backed, capped by caps_.maxHandles.
  HandleTable handles_;
  // Intrusive FIFO run queue over the records. Each live fiber is enqueued at
  // most once (at spawn or on a budget re-enqueue after being dequeued).
  FiberRecord* runHead_ = nullptr;
  FiberRecord* runTail_ = nullptr;
  uint32_t queueCount_ = 0;
  // Intrusive FIFO of child-rule fibers spawned during the current slice, held
  // out of the run queue. drainSpawnedSubtrees() runs them within the same tick
  // and empties this list before the tick returns. Empty whenever no SPAWN_RULE
  // ran this tick, which keeps tick() byte-identical for a spawn-free brain.
  FiberRecord* spawnHead_ = nullptr;
  FiberRecord* spawnTail_ = nullptr;
  // The fiber whose slice is currently running in tick(), or nullptr outside a
  // slice. SPAWN_RULE reads it to link a child fiber to its parent.
  FiberRecord* running_ = nullptr;
  // Ascending id space for root/rule fibers, starting at 1.
  uint32_t nextFiberId_ = 1;
  // Descending id space for synchronous hook fibers, starting at (uint32_t)-1
  // and counting down. Mirrors TS `nextInlineFiberId`.
  uint32_t nextInlineFiberId_ = 0xffffffffu;
};

} // namespace wendoo
