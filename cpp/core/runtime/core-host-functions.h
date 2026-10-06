#pragma once

#include <cstdint>

#include "core/platform/span.h"
#include "core/runtime/core-func-id.h"
#include "core/runtime/mc-number.h"
#include "core/runtime/random-stream.h"
#include "core/runtime/result.h"
#include "core/runtime/value.h"

namespace wendoo {

class ManagedHeap;
class GcRoots;
class TypeRegistry;

/**
 * The VM's default random stream: the 32-bit linear congruential
 * generator mirrored from `MathOps.random` in
 * external/wendoo-lang/packages/core/src/platform/math.node.ts (Numerical
 * Recipes parameters a = 1664525, c = 1013904223, modulus 2^32). The state is a
 * single `uint32_t` advanced in unsigned 32-bit arithmetic and mapped to
 * `[0, 1)` by dividing by 2^32 at the profile precision. The pinned default
 * seed is `1`.
 */
struct VmRng final : RandomStream {
  /** The pinned default seed, matching the TS module's initial `randomSeed`. */
  static constexpr uint32_t kDefaultSeed = 1;

  uint32_t state = kDefaultSeed;

  /** Advances the generator and returns the next value in `[0, 1)`. */
  mc_number_t next() override {
    state = state * 1664525u + 1013904223u; // unsigned: wraps mod 2^32
    // Dividing by an exact power of two only rescales the exponent, so the f32
    // quotient matches rounding the f64 quotient to f32.
    return static_cast<float>(state) / 4294967296.0f;
  }
};

/**
 * Ambient capabilities a core host-function body may need. Every pointer is
 * non-owning and may be null when no dispatched body needs it; a body invoked
 * without a capability it requires faults `HostError`.
 */
struct HostCallEnv {
  /**
   * Managed heap backing string/list/map results and resolving string-operand
   * content (borrowed content only when its program is configured), or null.
   */
  ManagedHeap* heap = nullptr;
  /** Collection root source used when a result allocation triggers a collect. */
  GcRoots* roots = nullptr;
  /** Random stream `MathRandom` draws from, or null when no body needs it. */
  RandomStream* rng = nullptr;
  /**
   * Type registry resolving instantiated container typeIds, or null. With a
   * null registry, container results carry {@link kNoTypeIdx}.
   */
  const TypeRegistry* types = nullptr;
};

/**
 * Dispatches core host function `id` over `args` (the topmost `argc` operands,
 * arg0 first), writing the result to `out` on success.
 *
 * Numeric results are at the build profile's precision (f32 on microbit-v2):
 * the TS NaN/div0/mod0 -> nil conventions hold, bitwise operands coerce through
 * ECMAScript ToInt32, and string operations are byte-oriented (exact for the
 * ASCII subset).
 *
 * Faults (leaving `out` untouched):
 * - `ScriptError` for an `id` this dispatch cannot service: an id with no body
 *   (context, sensor, and actuator ids and any unknown id). This is the
 *   `HOST_CALL` host-failure fault the VM contract specifies.
 * - `HostError` when a body needs a capability {@link env} does not supply (the
 *   heap for a string/list/map body, or the rng for `MathRandom`).
 * - `StackOverflow` when the heap cannot back a string/list/map result.
 */
Status callCoreHostFunction(CoreFuncId id, Span<const Value> args, const HostCallEnv& env,
                            Value& out);

} // namespace wendoo
