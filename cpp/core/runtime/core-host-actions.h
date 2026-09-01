#pragma once

#include <cstdint>

#include "core/runtime/core-func-id.h"

namespace wendoo {

/**
 * First host-action id owned by the active target; core action ids are below
 * this. Mirrors TARGET_ACTION_ID_BASE in
 * external/wendoo-lang/packages/core/src/runtime/abi-ids.ts.
 */
inline constexpr uint32_t TARGET_ACTION_ID_BASE = 1024;

/**
 * Identity record of one host action: its stable host-action id (the
 * `HOST_ACTION_CALL` operand) and the stable funcId of the action's host
 * function. Mirrors the numeric ids of the TS `HostActionIds` records; the
 * records' string keys are build-time identities and are not mirrored.
 */
struct HostActionIds {
  /** Stable host-action id. */
  uint32_t actionId;
  /** Stable funcId of the action's host function. */
  uint32_t fnId;
};

/**
 * Identity records of the core sensors and actuators, one per host action.
 * Mirrors the CoreHostActions table in
 * external/wendoo-lang/packages/core/src/runtime/abi-ids.ts. Action ids
 * are wire-stable: never renumber or reuse one; append new records at the
 * next free action id.
 */
namespace CoreHostActions {
inline constexpr HostActionIds SwitchPage{0, static_cast<uint32_t>(CoreFuncId::ActuatorSwitchPage)};
inline constexpr HostActionIds RestartPage{1,
                                           static_cast<uint32_t>(CoreFuncId::ActuatorRestartPage)};
inline constexpr HostActionIds Yield{2, static_cast<uint32_t>(CoreFuncId::ActuatorYield)};
inline constexpr HostActionIds Random{3, static_cast<uint32_t>(CoreFuncId::SensorRandom)};
inline constexpr HostActionIds OnPageEntered{
    4, static_cast<uint32_t>(CoreFuncId::SensorOnPageEntered)};
inline constexpr HostActionIds Timeout{5, static_cast<uint32_t>(CoreFuncId::SensorTimeout)};
inline constexpr HostActionIds CurrentPage{6, static_cast<uint32_t>(CoreFuncId::SensorCurrentPage)};
inline constexpr HostActionIds PreviousPage{7,
                                            static_cast<uint32_t>(CoreFuncId::SensorPreviousPage)};
inline constexpr HostActionIds Otherwise{8, static_cast<uint32_t>(CoreFuncId::SensorOtherwise)};
inline constexpr HostActionIds RuleTrigger{9, static_cast<uint32_t>(CoreFuncId::SensorRuleTrigger)};
} // namespace CoreHostActions

/** All core host-action records, in action-id order; ids are dense from 0. */
inline constexpr HostActionIds kCoreHostActions[] = {
    CoreHostActions::SwitchPage,  CoreHostActions::RestartPage,   CoreHostActions::Yield,
    CoreHostActions::Random,      CoreHostActions::OnPageEntered, CoreHostActions::Timeout,
    CoreHostActions::CurrentPage, CoreHostActions::PreviousPage,  CoreHostActions::Otherwise,
    CoreHostActions::RuleTrigger,
};

} // namespace wendoo
