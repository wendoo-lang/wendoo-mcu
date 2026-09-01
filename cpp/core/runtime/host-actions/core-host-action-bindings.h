#pragma once

#include <array>
#include <cstdint>

#include "core/runtime/core-host-actions.h"
#include "core/runtime/host-action.h"
#include "core/runtime/host-actions/actuators/restart-page.h"
#include "core/runtime/host-actions/actuators/switch-page.h"
#include "core/runtime/host-actions/actuators/yield.h"
#include "core/runtime/host-actions/core-host-action-env.h"
#include "core/runtime/host-actions/sensors/current-page.h"
#include "core/runtime/host-actions/sensors/on-page-entered.h"
#include "core/runtime/host-actions/sensors/otherwise.h"
#include "core/runtime/host-actions/sensors/previous-page.h"
#include "core/runtime/host-actions/sensors/random.h"
#include "core/runtime/host-actions/sensors/rule-trigger.h"
#include "core/runtime/host-actions/sensors/timeout.h"

namespace wendoo {

/** Number of core host-action bindings the table holds. */
inline constexpr uint32_t kCoreHostActionBindingCount = 10;

/**
 * Builds the core host-action binding table over `env`, one entry per core
 * sensor/actuator. Each body lives in its own header under
 * `core/runtime/host-actions/{actuators,sensors}/`. `env` must outlive every
 * dispatch through the table.
 */
inline std::array<HostActionBinding, kCoreHostActionBindingCount>
makeCoreHostActionBindings(CoreHostActionEnv& env) {
  return {{
      {CoreHostActions::SwitchPage.actionId, &execSwitchPage, nullptr, &env},
      {CoreHostActions::RestartPage.actionId, &execRestartPage, nullptr, &env},
      {CoreHostActions::Yield.actionId, &execYield, nullptr, &env},
      {CoreHostActions::Random.actionId, &execRandom, nullptr, &env},
      {CoreHostActions::OnPageEntered.actionId, &execOnPageEntered, &onPageEnteredPageEntered,
       &env},
      {CoreHostActions::Timeout.actionId, &execTimeout, &timeoutPageEntered, &env},
      {CoreHostActions::CurrentPage.actionId, &execCurrentPage, nullptr, &env},
      {CoreHostActions::PreviousPage.actionId, &execPreviousPage, nullptr, &env},
      {CoreHostActions::Otherwise.actionId, &execOtherwise, nullptr, &env},
      // A rule holds at most one watcher, and a trigger handle lives only in a
      // watcher slot, so the live count is bounded by the program's rule count.
      {CoreHostActions::RuleTrigger.actionId, nullptr, nullptr, &env, &execRuleTrigger, true},
  }};
}

} // namespace wendoo
