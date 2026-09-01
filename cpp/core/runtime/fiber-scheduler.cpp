#include "core/runtime/fiber-scheduler.h"

#include "core/runtime/execution-context.h"
#include "core/runtime/rule-structure.h"

namespace wendoo {

FiberScheduler::FiberScheduler(const ProgramImage& program, const RuntimeSurface& surface,
                               RegionArena& arena, const DeviceProfileCaps& caps)
    : program_(program), surface_(surface), arena_(arena), caps_(caps), records_(arena),
      regions_(arena), handles_(arena, caps.maxHandles) {
  // The scheduler is the heap's root source; point the surface the dispatch
  // loop runs against back at it so an allocation can collect over all fibers.
  surface_.roots = this;
  // A collection over those fibers traces every stack and locals slot; the
  // heap needs the surface's type registry to recognize native struct values
  // (injected contexts, device receivers) that carry no heap handle.
  if (surface_.heap != nullptr) {
    surface_.heap->setTypes(surface_.types);
  }
  // The async opcodes reach the handle table through the same surface.
  surface_.handles = &handles_;
  // ACTION_CALL_ASYNC spawns its child fiber through the same surface.
  surface_.spawner = this;
  // SPAWN_RULE spawns its child-rule fiber through the same surface.
  surface_.childRuleSpawner = this;
}

void FiberScheduler::enumerateRoots(GcMarker& marker) {
  records_.forEachLive([&marker](FiberRecord& record) {
    const ExecutionState& exec = record.exec;
    for (uint32_t i = 0; i < exec.stackDepth; i++) {
      marker.mark(exec.stack[i]);
    }
    for (uint32_t i = 0; i < exec.localsDepth; i++) {
      marker.mark(exec.locals[i]);
    }
  });
  if (surface_.context != nullptr) {
    const ExecutionContext& ctx = *surface_.context;
    for (size_t i = 0; i < ctx.variables.size(); i++) {
      marker.mark(ctx.variables[i]);
    }
    for (size_t i = 0; i < ctx.systemStore.size(); i++) {
      marker.mark(ctx.systemStore[i]);
    }
    for (size_t i = 0; i < ctx.callSiteStates.size(); i++) {
      if (ctx.callSiteStatePresent[i]) {
        marker.mark(ctx.callSiteStates[i]);
      }
    }
    for (size_t i = 0; i < ctx.callSiteSlots.size(); i++) {
      marker.mark(ctx.callSiteSlots[i]);
    }
    // The rule-variable store is one root: marking the outer map traces every
    // per-rule inner map and the values they hold.
    marker.mark(ctx.ruleVarStores);
  }
  // A resolved handle holds its result value until a waiter consumes it, so the
  // handle table is also a root source.
  handles_.enumerateResults(marker);
}

FiberRecord* FiberScheduler::allocFiber(uint32_t funcId, bool inlineId, Span<const Value> args,
                                        ErrorCode& err) {
  if (records_.liveCount() >= caps_.maxFibers) {
    err = ErrorCode::StackOverflow;
    return nullptr;
  }

  // Reserve the four execution regions at their initial sizes; they grow on
  // demand toward the caps. A failed reserve releases whatever already landed.
  ExecutionState exec{};
  exec.allocator = &regions_;
  exec.stackLimit = caps_.maxStackSize;
  exec.localsLimit = caps_.maxLocalsSize;
  exec.frameLimit = caps_.maxFrameDepth;
  exec.handlerLimit = caps_.maxHandlers;
  exec.stack = regions_.reserve<Value>(kInitialStackSlots);
  exec.locals = regions_.reserve<Value>(kInitialLocalsSlots);
  exec.frames = regions_.reserve<Frame>(kInitialFrameSlots);
  exec.handlers = regions_.reserve<Handler>(kInitialHandlerSlots);
  if (exec.stack == nullptr || exec.locals == nullptr || exec.frames == nullptr ||
      exec.handlers == nullptr) {
    exec.stackCapacity = exec.stack != nullptr ? kInitialStackSlots : 0;
    exec.localsCapacity = exec.locals != nullptr ? kInitialLocalsSlots : 0;
    exec.frameCapacity = exec.frames != nullptr ? kInitialFrameSlots : 0;
    exec.handlerCapacity = exec.handlers != nullptr ? kInitialHandlerSlots : 0;
    releaseRegions(exec);
    err = ErrorCode::StackOverflow;
    return nullptr;
  }
  exec.stackCapacity = kInitialStackSlots;
  exec.localsCapacity = kInitialLocalsSlots;
  exec.frameCapacity = kInitialFrameSlots;
  exec.handlerCapacity = kInitialHandlerSlots;

  FiberRecord* record = records_.alloc();
  if (record == nullptr) {
    releaseRegions(exec);
    err = ErrorCode::StackOverflow;
    return nullptr;
  }

  const Status started = startExecution(exec, program_, funcId, args);
  if (!started.isOk()) {
    records_.free(record);
    releaseRegions(exec);
    err = started.error();
    return nullptr;
  }

  record->id = inlineId ? nextInlineFiberId_-- : nextFiberId_++;
  record->state = FiberState::Runnable;
  record->exec = exec;
  // Only SPAWN_RULE child fibers carry a subtree root; all others leave it unset.
  record->rootRuleFuncId = kNoFuncId;
  record->ruleFuncId = resolveDirectRuleFuncId(program_, funcId);
  if (record->ruleFuncId != kNoFuncId && surface_.context != nullptr) {
    // Entering a rule at its own entry point starts that rule's next firing, so
    // any abandonment mark left by the previous one stops applying here.
    surface_.context->clearRuleAbandoned(record->ruleFuncId);
  }
  return record;
}

Result<uint32_t> FiberScheduler::spawn(uint32_t funcId) {
  ErrorCode err = ErrorCode::StackOverflow;
  FiberRecord* record = allocFiber(funcId, false, {}, err);
  if (record == nullptr) {
    return Result<uint32_t>::fail(err);
  }
  enqueue(record);
  return Result<uint32_t>::ok(record->id);
}

uint32_t FiberScheduler::spawnAsyncActionChild(uint32_t entryFuncId, uint32_t actionId,
                                               uint32_t callSiteId, uint32_t ruleFuncId,
                                               Span<const Value> args, ErrorCode& err) {
  // Create the result handle, then spawn the child and link the handle to it;
  // the child's completion settles the handle. On a spawn failure the handle is
  // deleted. Mirrors the handle wiring in execActionCallAsync (vm.ts).
  const uint32_t handleId = handles_.createPending();
  if (handleId == kNoHandleId) {
    err = ErrorCode::StackOverflow;
    return kNoHandleId;
  }
  // Child async-action fibers share the descending inline id space with hook
  // fibers, mirroring TS `nextInternalFiberId`.
  FiberRecord* record = allocFiber(entryFuncId, true, args, err);
  if (record == nullptr) {
    handles_.deleteHandle(handleId);
    return kNoHandleId;
  }
  Frame& entry = record->exec.frames[0];
  entry.ruleFuncId = ruleFuncId;
  // An async-action child belongs to the cluster of the rule that dispatched it.
  record->ruleFuncId = ruleFuncId;
  entry.hasActionBinding = true;
  entry.actionBinding = ActionFrameBinding{actionId, callSiteId, true};
  record->exec.asyncResultHandleId = handleId;
  // Capture the spawn-time tick time on the child; the scheduler stamps it on
  // the shared context for each of the child's slices.
  record->exec.dispatchTime = surface_.context != nullptr ? surface_.context->time : 0;
  enqueue(record);
  return handleId;
}

bool FiberScheduler::spawnChildRule(uint32_t funcId, ErrorCode& err) {
  // Tag the child with this subtree's root rule: the spawner's root when the
  // spawner is itself a child, or the spawner's own rule funcId when it is the
  // root.
  uint32_t subtreeRoot = funcId;
  if (running_ != nullptr) {
    subtreeRoot = running_->rootRuleFuncId != kNoFuncId ? running_->rootRuleFuncId
                                                        : running_->exec.frames[0].funcId;
  }
  FiberRecord* record = allocFiber(funcId, false, {}, err);
  if (record == nullptr) {
    return false;
  }
  record->rootRuleFuncId = subtreeRoot;
  // Hold the child out of the run queue; it drains within this tick after its
  // parent's slice (a synchronous same-think cascade).
  enqueueSpawn(record);
  return true;
}

bool FiberScheduler::hasLiveDescendantOfRoot(uint32_t rootRuleFuncId) {
  bool found = false;
  records_.forEachLive([&](FiberRecord& record) {
    if (record.rootRuleFuncId == rootRuleFuncId &&
        (record.state == FiberState::Runnable || record.state == FiberState::Waiting)) {
      found = true;
    }
  });
  return found;
}

bool FiberScheduler::hasLiveRuleSubtree(uint32_t ruleFuncId) {
  if (ruleFuncId == kNoFuncId) {
    return false;
  }
  bool found = false;
  records_.forEachLive([&](FiberRecord& record) {
    if (found || (record.state != FiberState::Runnable && record.state != FiberState::Waiting)) {
      return;
    }
    for (uint32_t cur = record.ruleFuncId; cur != kNoFuncId;
         cur = parentRuleFuncId(program_, cur)) {
      if (cur == ruleFuncId) {
        found = true;
        return;
      }
    }
  });
  return found;
}

void FiberScheduler::transitionTerminal(FiberRecord& record, FiberState terminal) {
  record.state = terminal;
  settleRuleWatchers(record, terminal);
}

void FiberScheduler::settleRuleWatchers(const FiberRecord& record, FiberState cause) {
  if (surface_.context == nullptr) {
    return;
  }
  ExecutionContext& ctx = *surface_.context;
  const bool abandoning = cause != FiberState::Done;
  // Cluster liveness is monotone from a rule to its ancestors: a live cluster's
  // parent cluster is live too.
  bool liveAbove = false;
  for (uint32_t ruleFuncId = record.ruleFuncId; ruleFuncId != kNoFuncId;
       ruleFuncId = parentRuleFuncId(program_, ruleFuncId)) {
    if (abandoning) {
      ctx.markRuleAbandoned(ruleFuncId);
    }
    const uint32_t handleId = ctx.ruleWatcher(ruleFuncId);
    if (handleId == kNoHandleId || liveAbove) {
      continue;
    }
    if (hasLiveRuleSubtree(ruleFuncId)) {
      liveAbove = true;
      continue;
    }
    const bool fired = !ctx.isRuleAbandoned(ruleFuncId) &&
                       ctx.ruleFiringState(ruleFuncId) == RuleFiringState::DidFire;
    ctx.clearRuleWatcher(ruleFuncId);
    handles_.resolve(handleId, fired ? kTrueValue : kFalseValue);
  }
}

void FiberScheduler::cancelChildRuleFibers() {
  records_.forEachLive([&](FiberRecord& record) {
    if (record.rootRuleFuncId != kNoFuncId &&
        (record.state == FiberState::Runnable || record.state == FiberState::Waiting)) {
      cancelRecord(record);
    }
  });
}

Status FiberScheduler::runActionHook(uint32_t funcId, uint32_t actionId, uint32_t callSiteId) {
  ErrorCode err = ErrorCode::StackOverflow;
  FiberRecord* record = allocFiber(funcId, true, {}, err);
  if (record == nullptr) {
    return Status::fail(err);
  }
  // Mark the entry frame as a sync action frame bound to this action and call
  // site.
  record->exec.frames[0].hasActionBinding = true;
  record->exec.frames[0].actionBinding = ActionFrameBinding{actionId, callSiteId, false};

  record->exec.budget = caps_.hookBudget;
  const RunResult result = runExecution(record->exec, program_, surface_);

  Status status = Status::ok();
  switch (result.status) {
  case RunStatus::Done:
    break;
  case RunStatus::Fault:
    status = Status::fail(result.error);
    break;
  case RunStatus::Yielded:
  case RunStatus::Waiting:
    // A hook that did not complete in its single slice cannot suspend.
    status = Status::fail(ErrorCode::ScriptError);
    break;
  }

  releaseRegions(record->exec);
  records_.free(record);
  return status;
}

Status FiberScheduler::runSystemFunction(uint32_t funcId) {
  ErrorCode err = ErrorCode::StackOverflow;
  FiberRecord* record = allocFiber(funcId, true, {}, err);
  if (record == nullptr) {
    return Status::fail(err);
  }
  // A System function runs as a brain-global frame with no action binding. It
  // receives the injected `ctx` only when its bytecode declares the param (the
  // entry frame's injectCtx slot).
  record->exec.budget = caps_.hookBudget;
  const RunResult result = runExecution(record->exec, program_, surface_);

  Status status = Status::ok();
  switch (result.status) {
  case RunStatus::Done:
    break;
  case RunStatus::Fault:
    status = Status::fail(result.error);
    break;
  case RunStatus::Yielded:
  case RunStatus::Waiting:
    // A System init / think runs to completion in its single slice and cannot
    // suspend.
    status = Status::fail(ErrorCode::ScriptError);
    break;
  }

  releaseRegions(record->exec);
  records_.free(record);
  return status;
}

void FiberScheduler::releaseRegions(ExecutionState& exec) {
  regions_.release<Value>(exec.stack, exec.stackCapacity);
  regions_.release<Value>(exec.locals, exec.localsCapacity);
  regions_.release<Frame>(exec.frames, exec.frameCapacity);
  regions_.release<Handler>(exec.handlers, exec.handlerCapacity);
  exec.stack = nullptr;
  exec.locals = nullptr;
  exec.frames = nullptr;
  exec.handlers = nullptr;
}

void FiberScheduler::cancel(uint32_t fiberId) {
  FiberRecord* record = findFiber(fiberId);
  if (record == nullptr ||
      (record->state != FiberState::Runnable && record->state != FiberState::Waiting)) {
    return;
  }
  cancelRecord(*record);
}

void FiberScheduler::cancelRecord(FiberRecord& record) {
  // A waiting fiber must leave its handle's waiter list so a later settle does
  // not try to resume a cancelled fiber.
  if (record.state == FiberState::Waiting && record.exec.awaiting) {
    handles_.removeWaiter(record.exec.await.handleId, record.id);
    record.exec.awaiting = false;
  }
  transitionTerminal(record, FiberState::Cancelled);
  // A cancelled async-action child cancels its pending result handle; the
  // awaiting parent resumes on the next drain with a Cancelled throw. Mirrors
  // cancelAsyncActionHandle in vm.ts.
  if (record.exec.asyncResultHandleId != kNoHandleId) {
    handles_.cancel(record.exec.asyncResultHandleId);
    record.exec.asyncResultHandleId = kNoHandleId;
  }
  // Signal an in-flight dispatch slice (a self-cancel from a host body) to stop
  // at its next instruction boundary.
  record.exec.cancelled = true;
  // A cancelled record must leave the run queue before a sweep can free it.
  removeFromQueue(&record);
}

void FiberScheduler::resumeFiberFromHandle(FiberRecord& record, const Handle& h) {
  if (record.state != FiberState::Waiting || !record.exec.awaiting ||
      record.exec.await.handleId != h.id) {
    return;
  }
  ExecutionState& exec = record.exec;
  const AwaitSite site = exec.await;
  // Restore the operand and frame stacks to the await depths and resume at the
  // instruction past the await. The handle was popped at AWAIT, so its slot is
  // free for the resolved value.
  exec.frameDepth = site.frameDepth;
  exec.stackDepth = site.stackHeight;
  exec.localsDepth = exec.frameDepth == 0 ? 0
                                          : exec.frames[exec.frameDepth - 1].localsOffset +
                                                exec.frames[exec.frameDepth - 1].localsCount;
  if (exec.frameDepth > 0) {
    exec.frames[exec.frameDepth - 1].pc = site.resumePc;
  }
  exec.awaiting = false;
  record.state = FiberState::Runnable;

  if (h.state == HandleState::Resolved) {
    exec.stack[exec.stackDepth++] = h.result;
  } else {
    // Rejected or cancelled: throw the handle's error at the resume point.
    exec.pendingInjectedThrow = true;
    exec.injectedError = h.error;
  }
  enqueue(&record);
}

void FiberScheduler::drainCompletedHandles() {
  for (Handle* h = handles_.popCompleted(); h != nullptr; h = handles_.popCompleted()) {
    const uint32_t handleId = h->id;
    for (HandleWaiter* waiter = h->waitersHead; waiter != nullptr; waiter = waiter->next) {
      FiberRecord* record = findFiber(waiter->fiberId);
      if (record != nullptr) {
        resumeFiberFromHandle(*record, *h);
      }
    }
    handles_.deleteHandle(handleId);
  }
}

void FiberScheduler::runFiberSlice(FiberRecord* record) {
  record->exec.budget = caps_.defaultBudget;
  // An async-action child (one holding a result handle) observes its frozen
  // dispatch time; stamp it on the shared context for the slice and restore the
  // live tick time afterward.
  const bool stampDispatchTime =
      record->exec.asyncResultHandleId != kNoHandleId && surface_.context != nullptr;
  const mc_number_t liveTime = stampDispatchTime ? surface_.context->time : 0;
  if (stampDispatchTime) {
    surface_.context->time = record->exec.dispatchTime;
  }
  // Expose the running fiber so a SPAWN_RULE in its slice can link the child
  // back to it. Cleared after the slice returns.
  running_ = record;
  const RunResult result = runExecution(record->exec, program_, surface_);
  running_ = nullptr;
  if (stampDispatchTime) {
    surface_.context->time = liveTime;
  }
  if (record->state == FiberState::Cancelled) {
    // A host body cancelled this fiber mid-slice (a page change or restart); the
    // slice stopped early. Leave the Cancelled state in place.
    return;
  }
  switch (result.status) {
  case RunStatus::Yielded:
    enqueue(record);
    break;
  case RunStatus::Done:
    transitionTerminal(*record, FiberState::Done);
    // An async-action child resolves its result handle on completion so the
    // awaiting parent resumes. Mirrors resolveAsyncActionHandle in vm.ts.
    if (record->exec.asyncResultHandleId != kNoHandleId) {
      handles_.resolve(record->exec.asyncResultHandleId, result.result);
      record->exec.asyncResultHandleId = kNoHandleId;
    }
    break;
  case RunStatus::Fault:
    transitionTerminal(*record, FiberState::Fault);
    // A faulted async-action child rejects its result handle so the awaiting
    // parent throws at its AWAIT. Mirrors rejectAsyncActionHandle in vm.ts.
    if (record->exec.asyncResultHandleId != kNoHandleId) {
      handles_.reject(record->exec.asyncResultHandleId, result.error);
      record->exec.asyncResultHandleId = kNoHandleId;
    }
    if (surface_.observer != nullptr) {
      surface_.observer->onFiberFault(record->id, result.error);
    }
    break;
  case RunStatus::Waiting:
    // The fiber parked on a pending handle (AWAIT). Hold it Waiting and add it to
    // the handle's waiters (the await site, including the handle id, lives on the
    // fiber's execution state); a later drain resumes it on settle.
    record->state = FiberState::Waiting;
    handles_.addWaiter(record->exec.await.handleId, record->id);
    break;
  }
}

uint32_t FiberScheduler::drainSpawnedSubtrees() {
  if (spawnHead_ == nullptr) {
    return 0;
  }
  uint32_t executed = 0;
  // Detach the fibers the just-finished slice spawned; grandchildren spawned
  // while draining form a fresh list this recursion consumes depth-first.
  FiberRecord* child = spawnHead_;
  spawnHead_ = nullptr;
  spawnTail_ = nullptr;
  while (child != nullptr) {
    FiberRecord* next = child->nextRunnable;
    child->nextRunnable = nullptr;
    // A page switch or parent stop earlier in the drain may have cancelled this
    // still-pending child through the page-scoped cascade; skip it.
    if (child->state == FiberState::Runnable) {
      runFiberSlice(child);
      executed++;
      executed += drainSpawnedSubtrees();
    }
    child = next;
  }
  return executed;
}

uint32_t FiberScheduler::tick() {
  uint32_t executed = 0;
  // Round snapshot: only the fibers queued at entry run; mid-round enqueues
  // stay queued for the next round.
  const uint32_t roundSize = queueCount_;
  for (uint32_t i = 0; i < roundSize; i++) {
    // A running fiber can cancel still-queued siblings (a page switch / restart
    // cancels the page's other rule fibers), draining the queue below roundSize.
    // Once empty it stays empty for the rest of the round, since only a running
    // fiber re-enqueues.
    if (runHead_ == nullptr) {
      break;
    }
    FiberRecord* record = dequeue();
    if (record->state != FiberState::Runnable) {
      continue;
    }

    runFiberSlice(record);
    executed++;

    // Drain the synchronous subtree this fiber just spawned (its child rules and
    // their descendants) within this same tick, before the next fiber in the
    // round.
    executed += drainSpawnedSubtrees();
  }
  return executed;
}

void FiberScheduler::sweep() {
  records_.forEachLive([&](FiberRecord& record) {
    if (record.state == FiberState::Done || record.state == FiberState::Fault ||
        record.state == FiberState::Cancelled) {
      releaseRegions(record.exec);
      records_.free(&record);
    }
  });
}

const FiberRecord* FiberScheduler::fiber(uint32_t fiberId) const {
  return const_cast<FiberScheduler*>(this)->findFiber(fiberId);
}

uint32_t FiberScheduler::liveCount() const { return records_.liveCount(); }

FiberRecord* FiberScheduler::findFiber(uint32_t fiberId) {
  FiberRecord* found = nullptr;
  records_.forEachLive([&](FiberRecord& record) {
    if (record.id == fiberId) {
      found = &record;
    }
  });
  return found;
}

void FiberScheduler::enqueue(FiberRecord* record) {
  record->nextRunnable = nullptr;
  if (runTail_ != nullptr) {
    runTail_->nextRunnable = record;
  } else {
    runHead_ = record;
  }
  runTail_ = record;
  queueCount_++;
}

FiberRecord* FiberScheduler::dequeue() {
  FiberRecord* record = runHead_;
  runHead_ = record->nextRunnable;
  if (runHead_ == nullptr) {
    runTail_ = nullptr;
  }
  record->nextRunnable = nullptr;
  queueCount_--;
  return record;
}

void FiberScheduler::removeFromQueue(FiberRecord* record) {
  FiberRecord* prev = nullptr;
  for (FiberRecord* cur = runHead_; cur != nullptr; cur = cur->nextRunnable) {
    if (cur == record) {
      if (prev != nullptr) {
        prev->nextRunnable = cur->nextRunnable;
      } else {
        runHead_ = cur->nextRunnable;
      }
      if (runTail_ == cur) {
        runTail_ = prev;
      }
      cur->nextRunnable = nullptr;
      queueCount_--;
      return;
    }
    prev = cur;
  }
}

void FiberScheduler::enqueueSpawn(FiberRecord* record) {
  record->nextRunnable = nullptr;
  if (spawnTail_ != nullptr) {
    spawnTail_->nextRunnable = record;
  } else {
    spawnHead_ = record;
  }
  spawnTail_ = record;
}

} // namespace wendoo
