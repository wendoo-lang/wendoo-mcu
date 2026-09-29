# Agent Instructions

These instructions apply to this repository.

Before working in any area of the codebase, list `.github/instructions/` and
read `global.instructions.md` plus any instruction files whose name matches the
area you are working in. `global.instructions.md` is the canonical home of the
comment guidelines, the plan-only-names ban, the ASCII-only rule, the zero-noise
check policy, and the broad-view rule; those rules are not repeated here.

For the working temperament and decision style to apply on ambiguous work
(small-step workflow, ownership boundaries, handoff posture), read
`.github/instructions/agent-posture.instructions.md`.

## Git Is Read-Only

Git is a read-only tool for every agent, subagents included. Inspection and
history queries are free, and `git rm` / `git mv` are allowed as ordinary file
operations while editing. Everything that changes repository state -- the
working tree, the index, the stash, any commit, branch, tag, or ref -- is the
user's to run, and only when they ask for that specific write in that turn.
`git checkout -- <file>` and `git restore <file>` read as harmless but discard
uncommitted work: treat them as writes. To undo or fix a file, edit or
regenerate it directly, and leave changes you did not make as they stand. The
submodule at `external/wendoo-lang` is a second repository under the same
rule: a pointer bump is a write to this repo, a commit inside it a write to
that one.

## Never Kill Processes You Do Not Own

Never kill, signal, or otherwise terminate a process you did not start in the
current session -- no `kill`, `pkill`, `killall`, or equivalent. This is an
absolute rule, and a port being in use is not evidence its listener is stale
or yours: same app, same cwd, same port may well be the user's own server. If
a port you want is occupied, pick another or ask the user; never free it
yourself. This covers dev servers, watchers, language servers, databases, and
every other long-running process.

## Command Approvals

- When requesting escalated command approval, include a narrow `prefix_rule`
  when safe and allowed so repeated commands can be approved persistently.
- Do not include a `prefix_rule` for destructive commands, heredocs, or overly
  broad command prefixes.

## Package Manager

- This repo uses npm exclusively; the only root lockfile is `package-lock.json`.
  Keep pnpm and yarn out entirely -- no alternate lockfiles, no `packageManager`
  field, no `pnpm`/`yarn` invocations.
- This is intentionally NOT an npm workspaces monorepo, and must not become
  one: the platform submodule is consumed by several independent repository
  roots, and a workspace root cannot span them. No root `workspaces` field.
  Each package and app runs its own `npm install`; cross-package links use
  `file:` dependencies and symlinks.
- Because there is no hoisting, every package must declare the dev tooling its
  own scripts invoke (for example `tsx`) in its own `devDependencies`, so the
  script resolves the binary from that package's `node_modules/.bin`. Do not
  rely on a root-level binary being on PATH, and do not work around a missing
  binary by editing PATH -- add the dependency to the package instead.
- Run scripts with npm from the relevant package directory, for example
  `npm test`, `npm run build`, `npm run typecheck`, `npm run generate:ambient`,
  `npm run check`.

## Code Quality

- Never emit placeholder code. Do not use `TODO`, `FIXME`, `...`,
  `/* implementation */`, `throw new Error("Not implemented")`, or any other
  stub pattern unless the user has explicitly written a stub and is asking to
  fill it in.
- Never produce non-production statements such as `console.log("test")`,
  `console.log("here")`, hardcoded magic strings used only for debugging, or
  temporary workarounds presented as real code.
- Complete functions fully. If a complete implementation cannot be inferred
  from context, suggest the minimal correct skeleton rather than a placeholder
  body.
- Write type-only imports as a top-level `import type` statement in `.ts` and
  `.tsx` files; do not use inline `import()` type expressions.

## Minimalism and the Arc (No Over-Build, No Erosion)

Two forces govern scope, and both bind. Minimalism prunes what no
planned work needs; the arc protects what planned work will need.
Applying either without the other fails: unchecked building buries
the system in speculative machinery, and minimizing at every turn
erases architectural structure. The ledger and the slice plan are
the arbiter between them.

### Minimalism (No Over-Build)

Build the minimum that satisfies the slice's acceptance criteria;
speculative robustness is out of scope by default. Over-building --
adding machinery for problems that cannot occur in the system as it
exists today or in any planned slice -- is a recurring failure to
actively guard against. The unifying tell: it defends against a
state that cannot occur, or duplicates a guarantee an existing
layer already provides.

- Burden of proof is on inclusion, not omission -- and it applies
  to machinery being BUILT, never to functionality being CARRIED.
  Existing working capability that a refactor touches is preserved
  by default; dropping it is not minimalism, it is a decision, and
  the decision is the user's. A characterization pass that maps
  what a refactor keeps and what it removes, with the authority for
  each removal, is part of the refactor.
- For every NEW field, check, error code, abstraction, parameter,
  or guard, name the concrete failure mode it prevents that no
  existing layer catches -- possible today, or scheduled to become
  possible in a planned slice. If the only justification is an
  unplanned future, a general good ("robustness", "flexibility"),
  symmetry, or something already guaranteed elsewhere, leave it
  out.
- Scope = the acceptance check or the real customer. A piece
  nothing exercises and no one uses does not get built. But two
  cautions: untested-but-real surfaces (dev tooling, operational
  consoles, docs affordances) have customers without tests --
  count them; and a test is only an oracle while it is faithful --
  a green suite over an unfaithful model licenses nothing.
- Banned by default, WHERE THE QUALIFYING PREMISE HOLDS -- check
  the premise before applying the ban, and check when it expires:
  redundant integrity duplicating an existing guarantee;
  compatibility/identity machinery in a genuinely single-build,
  single-version world; future-proofing for consumers no plan
  names; defensive validation of inputs that cannot be malformed;
  fixed- or cap-sized buffers and pools; error-code, state, or
  config inflation beyond what a path can reach.
- Deadlines beat deferral. Some machinery must exist BEFORE its
  failure mode becomes possible, because retrofitting it afterward
  is a migration: wire contracts, credential semantics, version
  carriers, public coordinates. For these, "add it when a failing
  test pulls it in" is wrong; the question is the deadline (the
  first pinned consumer, the first published artifact), and the
  deadline is tracked like any other slot.
- "Adding it later is cheap" holds only for reversible machinery.
  State the reversibility before leaning on it.
- A divergence from a reference app justified solely by minimalism
  is a flagged decision, never a silent default -- the
  follow-the-pattern rule does not lose to this section without a
  recorded argument.
- Subtraction before done. Before declaring a change complete, try
  to delete each piece you added; if no check breaks and no tracked
  slot claims it, remove it.

### The Arc (No Erosion)

This repository is built as a planned arc of slices toward one
architecture. Judge each piece against the whole planned arc, not
the current slice alone.

- A partially finished bridge is legitimate. A seam, surface, or
  mechanism may land incomplete and stay incomplete across several
  slices -- PROVIDED the incompleteness is tracked, with a named
  slot for its completion. Tracked incompleteness is construction;
  untracked incompleteness is debt; and completing it "minimally"
  by deleting the unfinished half is erosion, not cleanup.
- Structure that exists to carry the arc is not speculative
  machinery. "Speculative" means serving no planned slice, not
  serving a later one. Before pruning a piece as over-build, check
  the ledger and the plan: if a slotted slice claims it, it stands
  (sized honestly for what that slice needs, and marked).
- The empty-tree caution still applies in reverse: prefer one
  concrete implementation over a framework for implementations
  that do not exist -- but where the plan names the second
  implementation, the seam for it is arc, not framework.

For the C++ VM work specifically, the binding prohibitions are Locked Decisions 7
(no pre-sized pools), 8 (no ABI compatibility machinery), and 9 (this principle,
generalized) in the plan under `generated-docs/`.

## Repo-Specific Scope

- `external/wendoo-lang` is the imported reference checkout. Read it for
  contracts, compiler behavior, and TypeScript VM semantics, but do not make
  product changes there unless the task explicitly targets the reference.
- `docs/specs/contracts/vm-contract.md` is the local contract anchor for VM
  bytecode compatibility once this repo owns a docs tree.
- Planned first-cut ownership:
  - `packages/wodal` owns the CODAL-inspired web device runtime, initially
    implementing the microbit profile.
  - `apps/microbit-sim` owns visual UI, project management, compiler
    integration, and browser product shell.
- The shared Wendoo packages (`packages/core`, `packages/ts-compiler`,
  `packages/bridge-app`, `packages/app-host`, `packages/ui`) have two
  first-class, co-equal product consumers: `apps/microbit-sim` (this repo) and
  `external/wendoo-lang/apps/ecosim`, the game/sim app, which hosts live
  capability tiles (`[it]`/`see`/`bump`) on its own platform, unrelated to
  wodal/microbit. A change to a shared package or shared model is not done
  until both apps are swept and gated (typecheck + biome + test each); this is
  the one standing reason to run suites in the reference checkout. Design
  shared features for both consumers from the start: a default, assumption, or
  example tied to microbit-sim's platform (wodal/microbit-v2) must state how it
  also serves apps/ecosim's distinct platform.
- WODAL and the MCU VM must respect the Wendoo host calling convention and
  the single-entry VM rule: external callbacks enqueue only; the host loop
  drains, resolves handles, schedules, and executes.
