#pragma once

#include <cstdint>

#include "core/platform/span.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/host-actions/core-host-action-env.h"
#include "core/runtime/program.h"
#include "core/runtime/rule-structure.h"
#include "core/runtime/value.h"

namespace wendoo {

/**
 * Sensor body: true when the rule immediately above this one at its own nesting
 * level evaluated its WHEN and did not fire. A subject that fired, one still
 * evaluating, a rule with no preceding sibling, and a dispatch outside any rule
 * all read false. `hostData` is the bound {@link CoreHostActionEnv}. Mirrors the
 * TS otherwise sensor.
 */
inline Value execOtherwise(void* hostData, ExecutionContext& ctx, Span<const Value> args) {
  static_cast<void>(args);
  const CoreHostActionEnv& env = *static_cast<CoreHostActionEnv*>(hostData);
  if (env.program == nullptr) {
    return Value::boolean(false);
  }
  const uint32_t subject = precedingSiblingRuleFuncId(*env.program, ctx.currentRuleFuncId);
  if (subject == kNoFuncId) {
    return Value::boolean(false);
  }
  return Value::boolean(ctx.ruleFiringState(subject) == RuleFiringState::DidNotFire);
}

} // namespace wendoo
