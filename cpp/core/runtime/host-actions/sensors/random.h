#pragma once

#include "core/platform/span.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/host-actions/core-host-action-env.h"
#include "core/runtime/random-stream.h"
#include "core/runtime/value.h"

namespace wendoo {

/**
 * Sensor body: the next number of the bound random stream, in `[0, 1)`. `hostData` is the
 * bound {@link CoreHostActionEnv}. Mirrors the TS random sensor over `app.rng`.
 */
inline Value execRandom(void* hostData, ExecutionContext& ctx, Span<const Value> args) {
  static_cast<void>(ctx);
  static_cast<void>(args);
  CoreHostActionEnv& env = *static_cast<CoreHostActionEnv*>(hostData);
  return Value::number(env.rng->next());
}

} // namespace wendoo
