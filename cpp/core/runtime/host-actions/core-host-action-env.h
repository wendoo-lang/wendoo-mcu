#pragma once

namespace wendoo {

class BrainRuntime;
class GcRoots;
class ManagedHeap;
struct ProgramImage;
struct RuleSubtreeLiveness;
struct VmRng;

/**
 * Ambient capabilities the core sensor/actuator bodies reach for. Every core
 * host action is registered with a pointer to one of these as its `hostData`.
 * Each pointer is non-owning and must outlive every dispatch through the
 * bindings; the page-control bodies need {@link brain}, the random sensor needs
 * {@link rng}, the timeout sensor needs {@link heap}/{@link roots} to back its
 * per-callsite state, the sibling-reading sensors need {@link program} to read
 * the loaded brain's rule structure, and the rule trigger additionally needs
 * {@link ruleLiveness} to ask whether its subject's cluster is still running.
 */
struct CoreHostActionEnv {
  /** Brain runtime the page-control sensors and actuators drive. */
  BrainRuntime* brain = nullptr;
  /** VM-global pseudo-random stream backing the random sensor. */
  VmRng* rng = nullptr;
  /** Managed heap backing the timeout sensor's per-callsite state list. */
  ManagedHeap* heap = nullptr;
  /** Collection root source for the timeout sensor's state allocation. */
  GcRoots* roots = nullptr;
  /** Loaded program image the sibling-reading sensors derive rule order from. */
  const ProgramImage* program = nullptr;
  /** Cluster-liveness query (the fiber scheduler) the rule trigger reads. */
  RuleSubtreeLiveness* ruleLiveness = nullptr;
};

} // namespace wendoo
