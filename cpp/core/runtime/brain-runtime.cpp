#include "core/runtime/brain-runtime.h"

#include <cstring>

#include "core/runtime/execution-context.h"
#include "core/runtime/host-action.h"

namespace wendoo {

BrainRuntime::BrainRuntime(const ProgramImage& program, FiberScheduler& scheduler,
                           const RuntimeSurface& surface)
    : program_(program), scheduler_(scheduler), surface_(surface) {}

Status BrainRuntime::startup() {
  if (surface_.context == nullptr) {
    return Status::fail(ErrorCode::HostError);
  }
  // Bind the brain-lifetime slot tables from the shared region: one slot per
  // declared variable, one host-state slot per distinct call-site id, and a
  // per-callsite bytecode state pad whose column count is the program's
  // largest callsite-var index plus one.
  uint32_t callSiteCount = 0;
  for (const ActionCallSite& site : program_.callSites) {
    if (site.callSiteId + 1 > callSiteCount) {
      callSiteCount = site.callSiteId + 1;
    }
  }
  uint32_t callSiteSlotStride = 0;
  for (const Instr& ins : program_.instructions) {
    if (ins.op == Op::LOAD_CALLSITE_VAR || ins.op == Op::STORE_CALLSITE_VAR) {
      if (ins.a + 1 > callSiteSlotStride) {
        callSiteSlotStride = ins.a + 1;
      }
    }
  }
  const uint32_t variableCount = static_cast<uint32_t>(program_.variableNames.size());
  // The linker assigns each registered System a store slot in
  // 0..systemCount-1; the store needs one slot per System.
  uint32_t systemSlotCount = 0;
  for (const SystemRegistration& system : program_.systems) {
    if (system.storeSlot + 1 > systemSlotCount) {
      systemSlotCount = system.storeSlot + 1;
    }
  }
  // One entry per function in each per-rule table (firing record, watcher slot,
  // abandonment mark), indexed by funcId so every rule funcId the VM resolves is
  // in range. Allocated once for the brain's lifetime.
  const uint32_t ruleRecordCount = static_cast<uint32_t>(program_.functions.size());
  if (!surface_.context->bindSlots(scheduler_.arena(), variableCount, callSiteCount,
                                   callSiteSlotStride, systemSlotCount, ruleRecordCount,
                                   program_.variableInitValues, program_.constValues)) {
    return Status::fail(ErrorCode::HostError);
  }
  // Bind the program's rule-ancestor edges so rule-variable reads (including the
  // WHEN-result accessor) can walk from a nested rule up to its ancestors.
  if (program_.hasRuleAncestors) {
    surface_.context->ruleAncestors = program_.ruleAncestors;
  }
  lastThinkTime_ = 0;
  previousPageIndex_ = kNoPageIndex;
  // Every registered System's init runs once before the first page activates,
  // regardless of whether the program has any pages.
  const Status inits = runSystemInits();
  if (!inits.isOk()) {
    return inits;
  }
  if (program_.pages.empty()) {
    return Status::ok();
  }
  // Program-sized, allocated once for the brain's lifetime and reused on every
  // activation (never re-allocated per activation): one entry per root rule of
  // the brain's largest page, so any active page's root rules fit.
  uint32_t maxRootRuleCount = 0;
  for (const PageMetadata& page : program_.pages) {
    if (page.rootRuleFuncIdsCount > maxRootRuleCount) {
      maxRootRuleCount = page.rootRuleFuncIdsCount;
    }
  }
  if (maxRootRuleCount > 0) {
    ruleFibers_ = scheduler_.arena().allocate<RuleFiber>(maxRootRuleCount);
    if (ruleFibers_ == nullptr) {
      return Status::fail(ErrorCode::StackOverflow);
    }
  }
  currentPageIndex_ = 0;
  desiredPageIndex_ = 0;
  return activatePage(0);
}

Status BrainRuntime::activatePage(uint32_t pageIndex) {
  const PageMetadata& page = program_.pages[pageIndex];
  ExecutionContext& ctx = *surface_.context;

  // ruleFibers_ is the brain-lifetime buffer allocated once in startup; every
  // page's root-rule count fits within it.

  for (uint32_t i = 0; i < page.callSitesCount; i++) {
    const ActionCallSite& site = program_.callSites[page.callSitesOffset + i];
    if (site.binding == CallSiteBinding::Host) {
      // An unregistered action is skipped here; the existence check faults at
      // dispatch, mirroring the TS activation flow.
      const HostActionBinding* action = findHostActionById(surface_.actions, site.boundId);
      if (action == nullptr || action->onPageEntered == nullptr) {
        continue;
      }
      if (site.callSiteId >= ctx.callSiteStates.size()) {
        return Status::fail(ErrorCode::HostError);
      }
      ctx.currentCallSiteId = site.callSiteId;
      action->onPageEntered(action->hostData, ctx);
      ctx.currentCallSiteId = kNoCallSiteId;
      continue;
    }

    // Bytecode call site: run the action's one-time initializer (only the first
    // activation of this call site) and its per-activation hook, in that order.
    if (!program_.hasActions || site.boundId >= program_.actions.size()) {
      continue;
    }
    if (site.callSiteId >= ctx.callSiteAllocated.size()) {
      return Status::fail(ErrorCode::HostError);
    }
    const BytecodeAction& action = program_.actions[site.boundId];
    if (action.initializerFuncId != kNoFuncId && ctx.ensureCallSite(site.callSiteId)) {
      const Status hook =
          scheduler_.runActionHook(action.initializerFuncId, site.boundId, site.callSiteId);
      if (!hook.isOk()) {
        return hook;
      }
    }
    if (action.activationFuncId != kNoFuncId) {
      const Status hook =
          scheduler_.runActionHook(action.activationFuncId, site.boundId, site.callSiteId);
      if (!hook.isOk()) {
        return hook;
      }
    }
  }

  ruleFiberCount_ = 0;
  for (uint32_t i = 0; i < page.rootRuleFuncIdsCount; i++) {
    const uint32_t funcId = program_.rootRuleFuncIds[page.rootRuleFuncIdsOffset + i];
    const Result<uint32_t> spawned = scheduler_.spawn(funcId);
    if (!spawned.isOk()) {
      return Status::fail(spawned.error());
    }
    ruleFibers_[ruleFiberCount_++] = RuleFiber{funcId, spawned.value()};
  }

  pageActive_ = true;
  return Status::ok();
}

Status BrainRuntime::deactivateCurrentPage() {
  const PageMetadata& page = program_.pages[currentPageIndex_];
  ExecutionContext& ctx = *surface_.context;

  // Run each bytecode call site's deactivation hook in call-site order, then
  // cancel the page's rule fibers.
  for (uint32_t i = 0; i < page.callSitesCount; i++) {
    const ActionCallSite& site = program_.callSites[page.callSitesOffset + i];
    if (site.binding != CallSiteBinding::Bytecode) {
      continue;
    }
    if (!program_.hasActions || site.boundId >= program_.actions.size()) {
      continue;
    }
    const BytecodeAction& action = program_.actions[site.boundId];
    if (action.deactivationFuncId == kNoFuncId) {
      continue;
    }
    const Status hook =
        scheduler_.runActionHook(action.deactivationFuncId, site.boundId, site.callSiteId);
    if (!hook.isOk()) {
      return hook;
    }
  }

  cancelActiveFibers();
  ruleFiberCount_ = 0;
  ctx.currentCallSiteId = kNoCallSiteId;
  return Status::ok();
}

void BrainRuntime::cancelActiveFibers() {
  for (uint32_t i = 0; i < ruleFiberCount_; i++) {
    scheduler_.cancel(ruleFibers_[i].fiberId);
  }
  // Cascade: cancel every in-flight child-rule fiber spawned beneath the page's
  // roots, so a page switch or restart leaves no orphaned child fiber.
  scheduler_.cancelChildRuleFibers();
}

void BrainRuntime::requestPageChange(uint32_t pageIndex) {
  // Out of range is a no-op; the current page stays active.
  if (pageIndex >= program_.pages.size()) {
    return;
  }
  if (pageIndex == currentPageIndex_) {
    requestPageRestart();
    return;
  }
  desiredPageIndex_ = pageIndex;
  cancelActiveFibers();
}

void BrainRuntime::requestPageRestart() { cancelActiveFibers(); }

void BrainRuntime::requestPageChangeByPageId(const char* pageId, uint32_t length) {
  for (uint32_t i = 0; i < program_.pages.size(); i++) {
    const uint32_t idx = program_.pages[i].pageIdStringIdx;
    if (idx >= program_.strings.size()) {
      continue;
    }
    const StringRef& ref = program_.strings[idx];
    if (ref.length != length) {
      continue;
    }
    const char* pageBytes = reinterpret_cast<const char*>(program_.stringData.data()) + ref.offset;
    if (length == 0 || std::memcmp(pageBytes, pageId, length) == 0) {
      requestPageChange(i);
      return;
    }
  }
}

Value BrainRuntime::getCurrentPageId() const {
  return Value::borrowedString(program_.pages[currentPageIndex_].pageIdStringIdx);
}

Value BrainRuntime::getPreviousPageId() const {
  if (previousPageIndex_ >= program_.pages.size()) {
    return getCurrentPageId();
  }
  return Value::borrowedString(program_.pages[previousPageIndex_].pageIdStringIdx);
}

Status BrainRuntime::think(mc_number_t currentTimeMs) {
  if (inThink_) {
    return Status::fail(ErrorCode::HostError);
  }
  if (!pageActive_) {
    return Status::ok();
  }
  inThink_ = true;

  // A requested page switch deactivates the current page and activates the
  // requested one before the tick's time is stamped.
  if (currentPageIndex_ != desiredPageIndex_) {
    const Status deactivated = deactivateCurrentPage();
    if (!deactivated.isOk()) {
      inThink_ = false;
      return deactivated;
    }
    previousPageIndex_ = currentPageIndex_;
    currentPageIndex_ = desiredPageIndex_;
    const Status activated = activatePage(currentPageIndex_);
    if (!activated.isOk()) {
      inThink_ = false;
      return activated;
    }
  }

  ExecutionContext& ctx = *surface_.context;
  ctx.time = currentTimeMs;
  ctx.dt = lastThinkTime_ == 0 ? 0 : currentTimeMs - lastThinkTime_;
  ctx.currentTick++;

  // A completed, faulted, or cancelled rule fiber respawns; a fault kills
  // the fiber, never the rule.
  for (uint32_t i = 0; i < ruleFiberCount_; i++) {
    RuleFiber& entry = ruleFibers_[i];
    const FiberRecord* record = scheduler_.fiber(entry.fiberId);
    const bool dead = record == nullptr || record->state == FiberState::Done ||
                      record->state == FiberState::Fault || record->state == FiberState::Cancelled;
    // A root rule re-fires only once it is dead and no descendant child-rule
    // fiber is still in flight (the rule quiesces while a child it spawned is
    // parked).
    const bool needsRespawn = dead && !scheduler_.hasLiveDescendantOfRoot(entry.funcId);
    if (needsRespawn) {
      const Result<uint32_t> spawned = scheduler_.spawn(entry.funcId);
      if (!spawned.isOk()) {
        inThink_ = false;
        return Status::fail(spawned.error());
      }
      entry.fiberId = spawned.value();
    }
  }

  scheduler_.tick();
  // Drain any handles a host body settled during the round: resume their
  // waiters so they join the next round. An external callback may also have
  // settled a handle out of band before this think; the same drain handles it.
  scheduler_.drainCompletedHandles();
  // Every registered System's think runs after the scheduler round (and its
  // handle drain) and before the reclaim sweep, page-independent.
  const Status thinks = runSystemThinks();
  if (!thinks.isOk()) {
    inThink_ = false;
    return thinks;
  }
  scheduler_.sweep();
  // The end of a think is quiescent: every live value is reachable from the
  // scheduler's roots and this think's garbage is not. Reclaiming it here
  // bounds the heap's arena claim to its live set plus one think of churn.
  if (surface_.heap != nullptr) {
    surface_.heap->collect(scheduler_);
  }

  lastThinkTime_ = currentTimeMs;
  inThink_ = false;
  return Status::ok();
}

Status BrainRuntime::runSystemInits() {
  for (const SystemRegistration& system : program_.systems) {
    if (system.initFuncId == kNoFuncId) {
      continue;
    }
    const Status status = scheduler_.runSystemFunction(system.initFuncId);
    if (!status.isOk()) {
      return status;
    }
  }
  return Status::ok();
}

Status BrainRuntime::runSystemThinks() {
  for (const SystemRegistration& system : program_.systems) {
    if (system.thinkFuncId == kNoFuncId) {
      continue;
    }
    const Status status = scheduler_.runSystemFunction(system.thinkFuncId);
    if (!status.isOk()) {
      return status;
    }
  }
  return Status::ok();
}

} // namespace wendoo
