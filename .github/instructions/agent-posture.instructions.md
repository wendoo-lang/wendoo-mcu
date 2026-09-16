---
applyTo: "**"
---

# Agent Posture for Wendoo MCU

This file captures the working temperament and decision style expected of agents
in this repository, especially for ambiguous work touching WODAL,
`apps/microbit-sim`, the C++ on-hardware VM (`cpp/`), or Wendoo bytecode
compatibility.

It complements the enforceable rules; it does not restate them. The active
instruction sources remain `AGENTS.md`, `global.instructions.md`, and
`vm.instructions.md`. This document captures the judgment to apply where those
leave a decision open.

## General Posture

Work as a careful engineering collaborator, not a code generator trying to
maximize diff size. Move the system forward in small, reviewable steps while
preserving the architectural boundaries that make the project viable.

Default to these habits:

- Read before proposing.
- Propose before editing when the next step is architectural or ambiguous.
- Keep each implementation slice narrow.
- Prefer one coherent seam over several speculative scaffolds.
- Name uncertainties explicitly.
- Stop at a design note when the right code boundary is not clear.
- Verify with focused checks after changes.

Avoid these failure modes:

- Large opportunistic rewrites.
- Scaffolding future features not needed for the current slice.
- Turning exploratory architecture discussion into committed implementation.
- Hiding missing contracts behind placeholder code.
- Treating WODAL as an app UI dependency.
- Treating `apps/microbit-sim` as the owner of simulated device semantics.
- Letting the C++ VM (`cpp/`) diverge from the TypeScript reference VM's
  observable semantics.

## Small-Step Workflow

For non-trivial work, use this sequence:

1. Read the relevant instructions and files.
2. Summarize the current shape in concrete terms.
3. Identify the smallest coherent next slice.
4. State the ownership boundaries and invariants involved.
5. Wait for confirmation when the slice is not already explicitly requested.
6. Implement only that slice.
7. Run the narrowest meaningful checks.
8. Report what changed, what was verified, and what remains.

Small does not mean superficial. A small step should still be complete at its
level: a parser slice includes stable diagnostics and tests; a runtime slice
preserves execution boundaries and state ownership.

## Viability Before Done: Correct at Real Multiplicity

A change is done when it works for the real callers the system already has -- not
when its checks pass. Green proves the code does what its tests exercise; it does
not prove the feature is usable. Before declaring any mechanism complete:

- Count the real multiplicity. State how many independent instances the mechanism
  must serve in the codebase as it exists TODAY -- callers, registration passes,
  pages, devices, concurrent fibers. This is a present fact, not a future
  scenario. A mechanism that is correct only at one (one caller, one pass, one
  instance) when the system already has more is not limited; it is broken.
- Trace the second instance. Explicitly follow the mechanism through a second
  real caller, not just the first. "Works in the demo, dies in practice" failures
  are invisible at N=1 and structural at N=2. Name the second caller and walk it.
- Test at least two. If the behavior is about interaction between instances
  (uniqueness, dedup, ordering, sharing, isolation), a single-instance test that
  passes hides the collision. Exercise two.
- Global identity comes from a global source. A value that must be unique or
  comparable across the whole program -- an id, a key, a dedup handle -- must be
  derived from an actually-global source, never minted by a local, stateful, or
  order-dependent allocator (a per-pass counter, a per-file offset). If a
  globally-unique value already exists, use it; do not build a weaker parallel one.

## A Viability-Breaking "Limitation" Is a Defect

Do not let the word "limitation" launder a defect. This binds the implementer who
writes it and the reviewer who accepts it equally.

- An acceptable limitation is an edge case the primary user can avoid, with a
  stated workaround.
- A viability break is a flaw ordinary expected use will hit -- the next real
  consumer, the normal composition, the first integration.

If a "known limitation" would be triggered by the next real caller or forces the
user to avoid a normal pattern, it is dead on arrival. Do not ship it, and do not
accept it in review as "moot for now" -- "moot for the first consumer" is exactly
how a structural defect reaches production. Escalate it as a blocking design flaw
and fix the mechanism.

## Right-Layer Placement

Change-avoidance is not a virtue. When data or behavior belongs to a component -- by the
spec's assignment of responsibility, or by the plain ownership question "who executes this,
who is the source of truth for this" -- the correct change is to that component, even when it
is core, platform, or otherwise expensive to touch. A cheaper change at the wrong layer is a
parallel mechanism, and parallel mechanisms drift.

- "No core change needed" is a claim to examine, not a merit to report. Ask what the
  avoided change would have owned, and who owns it instead now.
- Minimalism bounds WHAT gets built (no speculative machinery), never WHERE a needed thing
  lives. The two questions are independent; answer both.
- A change at the owning layer carries its conditions: the rationale stated where the work
  is reviewed, the owning package's full gates, and direct tests for the new surface.

## Boundary Discipline

Be critical about where code belongs.

`packages/wodal` owns:

- CODAL-inspired web device runtime behavior.
- Simulated microbit functionality.
- WODAL device profiles.
- WODAL target metadata parsing.
- WODAL program image artifacts.
- Runtime loading mechanics.

`apps/microbit-sim` owns:

- Visual rendering.
- Project UI.
- Browser product shell.
- User workflows.
- Integration wiring between project state, compiler output, WODAL runtime
  state, and visual controls.

`cpp/` owns:

- The native C++ on-hardware Wendoo VM and the micro:bit v2 device firmware.
- Bytecode-compatible mirroring of the TypeScript reference VM's observable
  semantics. The reference VM is the executable spec; `cpp/` must not diverge
  from it.
- Device firmware mechanics: the on-flash program region, the CODAL host-loop
  driver, and fault-mode policy.
- The hand-maintained C++ ABI id mirror headers, kept append-only and in sync
  with their TS-declared id spaces.
- The native memory model: one region arena plus typed pools, no pre-sized
  buffers, no process-global mutable state.

Shared Wendoo packages own:

- Product-level document contracts.
- Shared service APIs.
- Compiler/runtime contracts that apply across Wendoo targets.

Shared Wendoo packages have two first-class, co-equal consumers:
`apps/microbit-sim` and `external/wendoo-lang/apps/ecosim`. The binding rule
(sweep and gate both apps, design for both platforms) is stated once, in
"Repo-Specific Scope" in `AGENTS.md`.

When a module could plausibly fit in more than one place, prefer the owner with
the narrowest durable responsibility. If a shape is a product-level contract, do
not bury it in WODAL just because WODAL is the first consumer.

## WODAL Direction

WODAL is the CODAL-inspired web device runtime for Wendoo MCU. It should
remain adapter-shaped: it exposes runtime/device behavior that app layers
consume, but it must not know about app UI or project screens.

Mutable runtime state must be scoped to explicit runtime/device/environment
instances. Do not introduce process-global mutable state or singletons. A
process may host multiple Wendoo platforms and multiple WODAL runtimes.

## Project Contract Posture

Treat shared project documents and artifact formats as Wendoo product
contracts, not one-package implementation details. When project or artifact data
can be consumed by more than one package, put the common contract in a shared
package or design doc before binding it to one implementation. WODAL can own
WODAL-specific target metadata and artifact validation, but it should not
silently become the owner of the whole product file format.

## Error and Diagnostic Posture

Errors and diagnostics should be machine-readable first. Use stable constants
such as `WODAL_PROJECT_MISSING_WODAL_TARGET` or `INVALID_BYTECODE_VERSION`.
Human-readable messages are convenience text; tests and app logic should match
stable codes, not prose.

When adding a new error family, follow the existing repo convention:

- Export a `...Code` constant object.
- Export the corresponding union type.
- Include prose `message` fields only as secondary human context.
- Add direct tests for the new branch.

## API Surface Posture

Be conservative when adding public API. Permanent WODAL APIs, Wendoo
builtins, tiles, ambient declarations, and target document fields are external
contracts. Add them only when the confirmed implementation slice needs them or
when the design has been explicitly reviewed.

Test-only APIs and fixtures may prove runtime behavior, but scope them clearly
to tests and do not register them as permanent platform surface.

When modeling CODAL APIs, ground the shape in the actual CODAL repositories or
official docs. Do not invent parity from memory or from unofficial docs without
checking the source shape.

## Communication Posture

Be direct and concrete. Good responses identify the actual current state, say
whether a direction is complete, partial, or missing, name the next small step,
explain ownership boundaries when they matter, and call out risks without
dramatizing them. Do not argue a plan is good merely because it avoids an
obviously bad alternative; let the technical reasoning carry the recommendation.

When handing work to a fresh session, include:

- The larger product goal.
- Explicit boundaries.
- Known state to verify.
- An instruction to read and summarize before editing.

## Fresh-Session Starter

For a fresh session, the desired posture is:

```text
First verify the current repo state. Do not implement immediately.

Read the relevant instructions and files, summarize the current shape, identify
one narrow next slice, and wait for confirmation before editing. Keep the slice
small enough to review, but complete enough to be useful. Preserve WODAL,
microbit-sim, the C++ VM (cpp/), shared Wendoo package, and VM/runtime
boundaries.
```
