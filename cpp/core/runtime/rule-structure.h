#pragma once

#include <cstdint>

#include "core/runtime/program.h"

namespace wendoo {

/**
 * The rule funcId for `funcId`, or {@link kNoFuncId} when `funcId` is not a rule
 * entry. Mirrors `getRuleFuncIdForFunc` in
 * external/wendoo-lang/packages/core/src/runtime/rule-services.ts.
 */
inline uint32_t resolveDirectRuleFuncId(const ProgramImage& program, uint32_t funcId) {
  if (program.hasRuleFuncIds) {
    for (uint32_t i = 0; i < program.ruleFuncIds.size(); i++) {
      if (program.ruleFuncIds[i] == funcId) {
        return funcId;
      }
    }
  }
  return kNoFuncId;
}

/**
 * The rule enclosing `ruleFuncId`, or {@link kNoFuncId} for a root rule and for
 * a funcId the program declares no ancestor edge for.
 */
inline uint32_t parentRuleFuncId(const ProgramImage& program, uint32_t ruleFuncId) {
  if (ruleFuncId == kNoFuncId) {
    return kNoFuncId;
  }
  for (const RuleAncestor& edge : program.ruleAncestors) {
    if (edge.ruleFuncId == ruleFuncId) {
      return edge.parentRuleFuncId;
    }
  }
  return kNoFuncId;
}

/**
 * The rule immediately above `ruleFuncId` at its own nesting level, or
 * {@link kNoFuncId} when `ruleFuncId` is the first rule at its level.
 *
 * A rule with an entry in `program.ruleAncestors` is a child: its subject is the
 * entry sharing its parent with the largest funcId below its own. A rule with no
 * entry is a root: its subject is its predecessor in its page's root-rule run.
 * Both derivations rest on rule funcIds being assigned in a pre-order
 * document-order walk, so a rule's whole subtree sits above it.
 */
inline uint32_t precedingSiblingRuleFuncId(const ProgramImage& program, uint32_t ruleFuncId) {
  if (ruleFuncId == kNoFuncId) {
    return kNoFuncId;
  }
  const uint32_t parent = parentRuleFuncId(program, ruleFuncId);
  if (parent != kNoFuncId) {
    uint32_t best = kNoFuncId;
    for (const RuleAncestor& edge : program.ruleAncestors) {
      if (edge.parentRuleFuncId != parent || edge.ruleFuncId >= ruleFuncId) {
        continue;
      }
      if (best == kNoFuncId || edge.ruleFuncId > best) {
        best = edge.ruleFuncId;
      }
    }
    return best;
  }
  for (const PageMetadata& page : program.pages) {
    for (uint32_t i = 0; i < page.rootRuleFuncIdsCount; i++) {
      if (program.rootRuleFuncIds[page.rootRuleFuncIdsOffset + i] != ruleFuncId) {
        continue;
      }
      return i == 0 ? kNoFuncId : program.rootRuleFuncIds[page.rootRuleFuncIdsOffset + i - 1];
    }
  }
  return kNoFuncId;
}

} // namespace wendoo
