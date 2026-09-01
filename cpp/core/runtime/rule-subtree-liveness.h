#pragma once

#include <cstdint>

namespace wendoo {

/** Answers whether a rule's cluster still holds a live fiber. */
struct RuleSubtreeLiveness {
  /**
   * True while any live (runnable or waiting) fiber belongs to `ruleFuncId`'s
   * cluster: its own fiber, or a fiber whose rule reaches `ruleFuncId` by
   * walking the program's rule-ancestor chain. Mirrors `hasLiveRuleSubtree` in
   * external/wendoo-lang/packages/core/src/runtime/vm.ts.
   */
  virtual bool hasLiveRuleSubtree(uint32_t ruleFuncId) = 0;

protected:
  ~RuleSubtreeLiveness() = default;
};

} // namespace wendoo
