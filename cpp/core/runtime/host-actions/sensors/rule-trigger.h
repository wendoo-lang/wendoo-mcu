#pragma once

#include <cstdint>

#include "core/platform/span.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/host-actions/core-host-action-env.h"
#include "core/runtime/program.h"
#include "core/runtime/result.h"
#include "core/runtime/rule-structure.h"
#include "core/runtime/rule-subtree-liveness.h"
#include "core/runtime/value.h"

namespace wendoo {

/**
 * Sensor body: answers whether the calling rule's subject -- the rule
 * immediately above it at its own nesting level -- has fired and completed. The
 * caller consumes the handle with an ordinary `AWAIT`, so an immediate answer
 * falls through without suspending and a pending one parks the rule.
 *
 * - The subject's cluster is in flight: the handle is left pending in the
 *   subject's watcher slot and the calling rule's own record is written
 *   `DidNotFire`. The settle walk resolves the handle when the subject's
 *   cluster empties.
 * - The subject fired and its cluster has emptied with no fault or
 *   cancellation in it: resolves true.
 * - The subject is settled without a firing, its firing was abandoned by a
 *   fault or a cancellation in its cluster, the calling rule has no subject, or
 *   the host bound no program or liveness query: resolves false.
 *
 * `hostData` is the bound {@link CoreHostActionEnv}. Takes no arguments.
 * Mirrors the TS rule-trigger sensor.
 */
inline Status execRuleTrigger(void* hostData, ExecutionContext& ctx, Span<const Value> args,
                              AsyncHandle handle) {
  static_cast<void>(args);
  const CoreHostActionEnv& env = *static_cast<CoreHostActionEnv*>(hostData);
  if (env.program == nullptr || env.ruleLiveness == nullptr) {
    handle.resolve(kFalseValue);
    return Status::ok();
  }
  const uint32_t ruleFuncId = ctx.currentRuleFuncId;
  const uint32_t subject = precedingSiblingRuleFuncId(*env.program, ruleFuncId);
  if (subject == kNoFuncId) {
    handle.resolve(kFalseValue);
    return Status::ok();
  }

  if (env.ruleLiveness->hasLiveRuleSubtree(subject)) {
    ctx.setRuleFiringState(ruleFuncId, RuleFiringState::DidNotFire);
    ctx.setRuleWatcher(subject, handle.id);
    return Status::ok();
  }

  const bool completed =
      ctx.ruleFiringState(subject) == RuleFiringState::DidFire && !ctx.isRuleAbandoned(subject);
  handle.resolve(completed ? kTrueValue : kFalseValue);
  return Status::ok();
}

} // namespace wendoo
