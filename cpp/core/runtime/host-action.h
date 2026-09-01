#pragma once

#include <cstdint>

#include "core/platform/span.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/result.h"
#include "core/runtime/value.h"

namespace wendoo {

/**
 * Synchronous host-action body. `args` is an ephemeral view of the
 * positional arg buffer (argc slots; a missing optional slot is nil) valid
 * only for the duration of the call; `hostData` is the pointer the action
 * was registered with. The bound call site is
 * `ctx.currentCallSiteId`. Returns the value the call pushes back.
 */
using HostActionExecSync = Value (*)(void* hostData, ExecutionContext& ctx, Span<const Value> args);

/**
 * Asynchronous host-action body, serviced by `HOST_ACTION_CALL_ASYNC`. `args`
 * is an owned snapshot of the positional arg buffer valid only for the duration
 * of the call; the bound call site is `ctx.currentCallSiteId`; `handle` is the
 * bound settle handle the call owns. The body must arrange for that handle to
 * eventually resolve, reject, or cancel. A non-ok status faults the dispatching
 * call (the opcode frees the handle first).
 */
using HostActionExecAsync = Status (*)(void* hostData, ExecutionContext& ctx,
                                       Span<const Value> args, AsyncHandle handle);

/**
 * Page-lifecycle hook of a host action. Invoked with the hook's call site
 * bound on `ctx.currentCallSiteId`.
 */
using HostActionPageHook = void (*)(void* hostData, ExecutionContext& ctx);

/**
 * One registered host action: its stable action id, the synchronous body,
 * the optional page-activation hook, the opaque pointer passed back to them,
 * and the optional asynchronous body. An action is sync ({@link execSync} set,
 * serviced by `HOST_ACTION_CALL`) or async ({@link execAsync} set, serviced by
 * `HOST_ACTION_CALL_ASYNC`); the opcode validates the matching body is present.
 */
struct HostActionBinding {
  /** Stable host-action id (the `HOST_ACTION_CALL` operand). */
  uint32_t actionId;

  /** Synchronous body, or null when the action is asynchronous. */
  HostActionExecSync execSync;

  /** Hook run on page activation for each of the action's call sites, or null. */
  HostActionPageHook onPageEntered;

  /** Opaque pointer handed to {@link execSync}, {@link execAsync}, and {@link onPageEntered}. */
  void* hostData;

  /** Asynchronous body, or null when the action is synchronous. */
  HostActionExecAsync execAsync = nullptr;

  /**
   * When true, `HOST_ACTION_CALL_ASYNC` allocates this action's handles outside
   * the runtime's `maxHandles` accounting: the dispatch skips the capacity
   * check and the handle is not counted while live. Set it only on an action
   * whose live pending handles the runtime bounds by construction. False means
   * the ordinary capped allocation. Mirrors `HostActionBinding.uncappedHandles`
   * in external/wendoo-lang/packages/core/src/runtime/context.ts.
   */
  bool uncappedHandles = false;
};

/**
 * Returns the registration holding `actionId`, or nullptr when no entry of
 * `bindings` does.
 */
inline const HostActionBinding* findHostActionById(Span<const HostActionBinding> bindings,
                                                   uint32_t actionId) {
  for (size_t i = 0; i < bindings.size(); i++) {
    if (bindings[i].actionId == actionId) {
      return &bindings[i];
    }
  }
  return nullptr;
}

} // namespace wendoo
