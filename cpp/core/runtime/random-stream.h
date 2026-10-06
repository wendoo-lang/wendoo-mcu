#pragma once

#include "core/runtime/mc-number.h"

namespace wendoo {

/**
 * The random-number stream a brain's random reads draw from: the random
 * sensor (through {@link CoreHostActionEnv::rng}) and the `MathRandom` host
 * function (through {@link RuntimeSurface::rng}). Wire one instance onto both
 * so the two consumers share one stream. Mirrors `IRngServices` in
 * external/wendoo-lang/packages/core/src/runtime/services.ts.
 */
struct RandomStream {
  /** Advances the stream and returns its next value in `[0, 1)` at the profile precision. */
  virtual mc_number_t next() = 0;

protected:
  ~RandomStream() = default;
};

} // namespace wendoo
