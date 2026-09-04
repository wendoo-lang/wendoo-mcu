# Spec: jacdac (draft)

Jacdac is the **plug-and-play accessory bus** for micro:bit v2 and other boards: modules
(buttons, sliders, servos, lights, sensors) share a single wire, announce themselves, and expose
typed **services** the brain can read, write, and receive events from. The external protocol is
specified at aka.ms/jacdac; this spec covers how Wendoo exposes it.

The defining user experience: **plugging in a module makes its tiles appear in the language;
unplugging removes them; swapping a module for an equivalent one requires no tile fixups.** A
module's tiles are per-ROLE ("red button pressed") -- a role is a stable named slot the module
fills (see Roles) -- published through the dynamic-tiles mechanism
(`docs/specs/dynamic-tiles.md`). Rules that reference a role with no module bound are preserved
and resume working the moment a matching module attaches.

Jacdac spans three layers with distinct owners:

- **The bus-core primitive** (`ctx.microbit.jacdac`, platform-owned): the wire machinery -- device
  discovery, a register cache, event delivery, command transmit. Small, eternal, per-target.
- **The jacdac extension library** (user-imported, `docs/specs/extensions.md`): a small
  **handler** set (a generic register read/write pair covering tag-shaped registers on every
  service class, plus class-specific handlers for events and commands), a **host-invoked
  `TileProvider`** that derives per-role tiles from the project's role table (fed by the bus's
  attached devices) at edit time, and a `System` (`docs/specs/system.md`) for runtime
  housekeeping in running brains. All service-class
  knowledge (registers, scaling, events, labels, grammars) lives here. A brain that does not use
  jacdac tiles carries no jacdac code (System inclusion is per-brain by reachability; the
  provider is edit-time-only and never compiled into any program).
- **The dynamic-tiles mechanism** (shared packages): roster, publication, deterministic tile ids,
  unresolved-reference preservation. Specified in `docs/specs/dynamic-tiles.md`, not here.

The brain is a Jacdac **client** ("brain" in Jacdac terms): it consumes module services. It does
not expose its own display/buttons/sensors as Jacdac services (see Open questions).

ABI ids: TBD (appended per the append-only rule when assigned).

## Stances

Jacdac follows the standard device stance (`docs/specs/microbit-context.md`), mapped per surface:

- **Reading a module value is a poll sensor.** The bus core maintains a **register cache** fed by
  the wire (streamed sensor reports, get/report exchanges). A handler read is a synchronous cache
  read -- never a wire round-trip inside a think. An absent module or a never-reported register
  reads as nil, and published sensor tiles are **presence-gated**
  (`docs/specs/value-sensor-presence-gate.md`), so a bare WHEN gates on presence, not truthiness.
- **Acting on a module is a sync actuator.** A register write or command is enqueue-only into the
  TX outbox and returns immediately (Jacdac writes are fire-and-forget on the wire). Acting on an
  absent module is a **silent no-op** (the arcade-shield convention: no error, no trace port line).
- **Module events are delivered like radio packets.** Events land enqueue-only in a bounded ring,
  each with a monotonic sequence number; readers drain by cursor (the radio receive model,
  `docs/specs/radio.md`). Published event tiles get host-managed per-callsite cursors; the Device
  API uses a user-managed cursor (`events(since)` + `currentSeq()`, seq starts at 1, 0 is the
  before-any sentinel).

All external input (announces, reports, events) enters enqueue-only between thinks; the VM
observes it on the next think. The single-entry rule holds at every layer.

## Identity crosses as hex strings

Wendoo numbers are f32 (24-bit mantissa). Two Jacdac identifiers do not fit:

- a **device id** is a 64-bit serial;
- a **service class** is a full u32 (e.g. `0x1473a263`), routinely above 2^24.

Both therefore cross the Device API as **lowercase hex strings** (`"e1f2a3b4c5d6e7f8"`,
`"1473a263"`), never as numbers. Register codes, event codes, and service indices are u16 or
smaller and cross as numbers. Serials identify concrete modules at the wire and Device-API level;
TILE identity is deliberately not serial-based -- tiles bind roles (next section), so that
swapping a module never changes a tile id.

## Roles: stable names over replaceable modules

A **role** is a named, typed slot: a role id, a display name, and a service class ("red button" /
button). Roles are what tiles bind and what users see; which physical module currently fills a
role is mutable state underneath. This is the inversion that makes replacement "just work": the
role is the durable identity, the serial binding is not. (The semantics mirror Jacdac's own role
manager; no role-manager WIRE service is involved -- see Protocol capability coverage.)

- **Minting.** When a module attaches whose services match no existing role, the host mints a
  role per service. The display name is seeded from the module's self-description where
  available: the service's instance-name register first (a module naming its own services, e.g.
  a dual button's "L" / "R"), else the module's product or device description combined with the
  class name, else the plain service class name ("button"). Duplicates take an ordinal suffix
  ("button 2"). The self-description sources are string registers read over the wire, so minting
  need not wait for them: the host mints immediately with the best seed at hand and upgrades the
  label when a better source's report lands -- safe because names carry no identity. A label the
  user has renamed is never upgraded over. Self-description seeds the NAME only; it never
  participates in identity -- the role id is opaque and stable regardless of what seeded the
  label. The **role table** -- roles, names, preferred bindings -- is durable
  project-adjacent state, and is the provider's domain: tiles derive from roles, not from the raw
  bus.
- **Binding.** A role resolves to a concrete `(device, serviceIndex)` at runtime: the
  **preferred binding** (the module that most recently filled the role) when that module is
  present, else **auto-bind** to the first unassigned attached instance of the class. Binding
  reacts to attach/detach; a resolved binding is stable while its module stays attached.
  Auto-bind updates the preference, so the replacement becomes the new preferred module.
- **Storage.** The role TABLE (ids, names, classes) is PROJECT-scoped, stored in the project
  file: one set of roles, one set of tile ids, shared by every micro:bit instance, so a brain
  reassigned to another instance keeps resolving. Preferred BINDINGS are PER-INSTANCE, also in
  the project file: each instance fills the shared roles from its own bus. Minting binds an
  attaching module to an existing role of its class unbound ON THAT INSTANCE before minting a
  new role, so a fleet of micro:bits each carrying a button all fill the one "button" role.
  Deploy bakes the assigned instance's preferred bindings into the flashed program; flashing
  without an instance bakes none (pure auto-bind).
- **Replacement.** Swap a dead button for a new one: the old serial never reappears, auto-bind
  claims the new module, every tile keeps working. No fixups, no user action.
- **Renaming.** The user may rename a role ("red button"); labels re-derive, tile ids do not
  change (identity is the role id, never the display name).
- **Disambiguation.** With two attached instances of one class, the app resolves "which one is
  which" through the Jacdac identify command (blink the module's LED) or by asking the user to
  actuate the one they mean ("press the button you mean"). Assignment updates preferred bindings.
- **On hardware.** The flashed program carries each role's record (id, class, preferred binding)
  as baked constants; the bus core runs the same preferred-else-auto-bind resolution on device.
  A single module per class -- the common case -- needs no preference at all. Multiple same-class
  modules with no valid preference bind deterministically but arbitrarily (stable order by
  serial); getting a specific assignment requires setting preferences at authoring time.
- **Three tile states.** BOUND (module attached, role resolved): the tile renders normally,
  compiles, runs. UNBOUND (module unplugged; the role persists): the tile definition stays in
  the catalog, so the placed tile renders IDENTICALLY -- same label, icon, editable arguments,
  docs -- never an error or placeholder style; a not-connected presentation affordance (dimmed
  accent, indicator) conveys the state. It still compiles, because binding is runtime state,
  not compile input, so unplug and replug cause zero recompilation; at runtime it
  presence-gates (sensor never fires, actuator no-ops). MISSING (role deleted from the table,
  or the library uninstalled): the definition is genuinely absent; the reference is preserved
  and rendered from its display hint in a neutral waiting style, never an error style, the
  containing rule gates out of compilation, and the rest of the brain runs
  (`docs/specs/dynamic-tiles.md`). Unplugging never produces the MISSING state.

## The bus core as a service (device state)

`ctx.microbit.jacdac` holds, per micro:bit instance:

- **Presence**: whether a Jacdac bus is connected at all (injectable input, like accessory
  presence in `docs/specs/arcade-shield.md`).
- **The attached-device table**: one entry per announced device -- device id (hex string), the
  announced service list (class + index pairs), a liveness stamp. A device unseen past the
  protocol's announce-timeout is removed (attach and detach are both observable state changes,
  driven only by wire input).
- **The register cache**: values for registers the brain has read or subscribed to, updated
  enqueue-only from wire reports, read synchronously by handlers. Cache entries carry a
  last-updated tick.
- **The role-binding table**: role records (id, service class, preferred binding) registered
  from running programs, each resolved to a current `(device, serviceIndex)` or unbound;
  re-resolved enqueue-only on attach/detach per the Roles rules.
- **The event ring**: bounded, monotonic `seq`, overflow overwrites oldest (depth pinned in the
  target section).
- **The TX outbox**: writes and commands queued by actuator calls, flushed to the wire by the
  host loop after think.

`reset()` restores power-on state: empty table, empty cache, empty ring, seq preserved
monotonic within a session (the radio convention).

## Decode: the caller supplies the format

The platform ships **no Jacdac service specification database**. Knowledge of which registers a
service has, their pack formats, units, and scaling belongs to the extension library (which bakes
it into handler bind constants). The bus core implements one generic mechanism: a **jdpack subset
decoder/encoder** -- given a register's bytes and a caller-supplied pack format string, produce
number(s) or a Buffer; given values and a format, produce bytes.

The supported format subset covers scalar and fixed-point primitives (`u8 u16 u32 i8 i16 i32`,
fixed-point forms such as `u0.16` and `i16.16`, `b` for raw bytes). A `u32`/`i32` field decodes
through f32 and loses exactness above 2^24 -- acceptable for sensor readings; a consumer needing
exact wide integers reads the raw Buffer. Unsupported format strings are refused with a stable
code, never silently misdecoded.

## Device API (`ctx.microbit.jacdac`)

The primitive surface tracks the bus core, not any service's semantics. All ids hex strings per
Identity; `serviceIndex` numeric.

| `ctx.microbit.jacdac.*` | Returns | Notes |
| ----------------------- | ------- | ----- |
| `isConnected()` | boolean | bus presence (injectable) |
| `devices()` | `JacdacDevice[]` | the attached-device table; each entry carries `id` (hex string) and `services` (`JacdacServiceRef[]` of `{ serviceClass, index }`, class a hex string) |
| `isAttached(deviceId)` | boolean | table membership |
| `readRegister(deviceId, serviceIndex, register, format)` | number \| nil | cached value decoded per `format` (first field); nil when absent/never reported |
| `readRegisterBuffer(deviceId, serviceIndex, register)` | Buffer | raw cached bytes; empty Buffer when absent |
| `subscribe(deviceId, serviceIndex, register)` | nil | ask the bus core to keep this register fresh (streaming for sensor readings); idempotent |
| `writeRegister(deviceId, serviceIndex, register, format, value)` | nil | encode per `format`, enqueue; silent no-op when absent |
| `writeRegisterBuffer(deviceId, serviceIndex, register, buffer)` | nil | raw write, enqueue |
| `sendCommand(deviceId, serviceIndex, command, buffer?)` | nil | enqueue a command packet |
| `events(since)` | `JacdacEvent[]` | all ring events with `seq > since`; stateless filter, caller owns the cursor (radio model) |
| `currentSeq()` | number | ring head seq (0 when empty) |
| `resolveRole(role, serviceClass, preferredDeviceId?, preferredServiceIndex?)` | `JacdacRoleBinding` \| nil | idempotently registers/refreshes the role record in the binding table and returns its current binding, or nil when unbound; the resolution rules are in Roles |

`JacdacEvent` is a value-struct: `seq`, `deviceId` (hex string), `serviceIndex`, `code`, `buffer`
(raw payload; the library decodes it with the format it knows). `JacdacRoleBinding` is a
value-struct: `deviceId` (hex string), `serviceIndex`.

- **ABI ids (append-only):** `MicroBitField.Jacdac` appended last per the field-order invariant;
  type atoms for `Jacdac`, `JacdacDevice`, `JacdacDeviceList`, `JacdacServiceRef`,
  `JacdacServiceRefList`, `JacdacEvent`, `JacdacEventList`, `JacdacRoleBinding`; host-function
  ids for the calls above.
  All TBD until assigned by the wodal build; cpp mirrors.
- **Not 1:1 with tiles:** there are no platform tiles at all (next section). The Device API is
  the floor the library builds on, and the escape hatch for user code driving a module the
  library does not know.

## Tiles: all jacdac tiles are dynamic, declared by the library, published by the host

The platform declares **zero static Jacdac tiles**. The extension library declares:

- **Handlers** -- `Sensor`/`Actuator` declarations marked `handler: true`, in two tiers:
  - **The generic register pair.** Jacdac's `value`, `intensity`, and other rw registers are
    read/write value slots -- the same shape as a SCADA tag -- so one `read register` sensor
    handler and one `write register` actuator handler, parameterized entirely by `bind` (the
    role record plus `register`, `format`, scaling constants), cover that subset of EVERY
    service class with no per-class code. The handler body resolves its role
    (`resolveRole`, nil when unbound) and then reads or writes through the register primitives. Which registers a service class publishes as
    tiles, with what labels, formats, and scaling, is provider metadata, not handler code. (A
    class with a requested-vs-actual split, like servo `angle` vs `actual_angle`, publishes the
    rw register as the write tile and either register as the read tile -- a provider-metadata
    choice.)
  - **Class-specific handlers** only where behavior is not a value slot: events (a cursor over
    the event ring, e.g. button Down/Hold), commands (e.g. play-tone -- no readable state), and
    derived semantics. These grow class-by-class as classes earn hand-crafted tiles.

  A handler's argument vocabulary includes the bound instance identity and any author-facing
  arguments (a threshold, an angle); its body calls the Device API. Handlers are ordinary
  library tiles: their identities live in the library coordinate's user-tile id space, not the
  platform ABI.
- **A `TileProvider`** -- a pure derivation the AUTHORING HOST invokes whenever the role table
  changes (a module attached or detached, a role renamed, a project opened): role table in, tile
  declarations out. `instanceKey = <role id>` (aspect-suffixed when one role yields several
  tiles), labels from the role's display name, class-appropriate argument grammar, `bind`
  carrying the role record and class constants. Tile ids are therefore stable across module
  replacement by construction. Because the host maintains the role table without any brain
  running, tiles appear the moment a module does -- including on a project with no brains yet --
  and because the host is the only publisher, any number of running brains coexist without
  publication conflicts. No brain code references the provider, so it is tree-shaken out of
  every compiled program: zero flash, memory, or cycle cost on hardware for the publication
  machinery (`docs/specs/dynamic-tiles.md`, "Publication does not exist at runtime").
- **The jacdac `System`** -- the library's RUNTIME service core, with no publication role. Its
  `think()` re-issues `subscribe` calls for the registers its brain's placed sensor tiles read
  and keeps streaming bookkeeping; its state holds that subscription set. Inclusion is per-brain
  by reachability: no jacdac tile, no System, no cost.

A published sensor tile occupies the poll bucket (its handler reads the cache); a published
actuator tile is a sync actuator (its handler enqueues); a published event tile is an event
sensor over the ring with a host-managed per-callsite cursor, presence-gated. A compiled program
never depends on the roster: each published tile compiles to its handler with the bound role
record baked in, so the flashed program resolves its roles on device (preferred binding, else
auto-bind) and behaves per the absence rules above while a role is unbound.

## Simulator (apps/microbit-sim)

- **Accessory catalog + attach/detach.** The simulator offers a catalog of simulated Jacdac
  modules; the user instantiates one and attaches it to a micro:bit instance's bus. Attach and
  detach do two things: they drive the same injectable wire-input path the parity harness scripts
  (announce appears, announce-timeout removes -- the runtime side), and they update the role
  table, which triggers the authoring host to re-invoke the library's `TileProvider` (the
  edit-time side), so tiles appear and disappear in the picker immediately, whether or not any
  brain is running.
- **Role management, kept invisible.** Attaching a module of a new class mints its role
  automatically; the user sees only names. Renaming a role is inline on the accessory (labels
  update, tile ids do not). With two same-class modules, assignment uses identify: the app
  blinks a module or asks the user to actuate the one they mean, and preferred bindings update.
  There is no roles screen and no serial number anywhere in the UI.
- **Per-instance tile catalogs (`instance.tileCatalog`).** Each micro:bit instance owns one
  tile catalog, named by its owner in code: `instance.tileCatalog` (an `ITileCatalog`; a
  store-level collection is `instanceTileCatalogs`, keyed by instance id). It rides the brain
  editor's existing multi-catalog composition -- the roster realization in
  `docs/specs/dynamic-tiles.md`. The catalog belongs to the instance, not to jacdac: the jacdac
  provider publishes into it, and any future provider whose domain is scoped to the instance
  publishes into the same catalog. Jacdac's published CONTENT derives uniformly from the
  project-scoped role table (every instance's catalog holds the project's role tiles -- this is
  what keeps brains portable and unplugging non-destructive); what differs per instance is
  BINDING presentation: bound roles offer normally, unbound roles carry the not-connected
  affordance and group accordingly in the picker (see Three tile states in Roles). Editing a
  brain composes in `instance.tileCatalog` for its assigned instance, so the picker presents
  what THIS micro:bit can do; a brain with no assigned instance composes no instance catalog
  until assigned. The unbound affordance rides the editor's app-supplied tile presentation
  resolver -- presentation only, no core catalog mutation. Catalog CONTENT changes (mint,
  delete, rename) invalidate per the catalog-change path; binding changes are presentation
  only and invalidate nothing.
- **Module UI ships with the library, hosted by the app.** Each simulated module renders an
  interactive UI (press the simulated button, watch the simulated servo); sensor-side
  interaction injects module state, and actuator writes from the brain render as module state
  changes. This UI is NOT base-app code: the jacdac library ships it as one or more sandboxed
  surfaces (iframes) declared in a surface manifest, exchanging Jacdac wire frames with the
  instance's bus over named window message channels (the established
  `{channel: "jacdac", type: "messagepacket"}` convention, which also interoperates with
  MakeCode-built module simulators). The app provides the generic hosting: iframe slots, the
  sandbox boundary, the channel broker (routing opaque bytes between a surface and the
  instance's injectable wire path, never parsing them), and a window-management frame in which
  every library's contributed surfaces land and the user chooses which is most prominent. The
  app never contains per-module UI; the library's surfaces may use any UI stack, since the
  sandbox isolates their dependencies. Channel semantics: a channel is a BUS, not a pipe --
  within a channel, every subscribed surface receives every frame except its own
  (sender-suppressed), because the channel is the simulated wire; across channels and across
  instances, surfaces are fully isolated (a surface sees only the channels its manifest
  declares and the host grants, and one instance's bus channel never reaches another
  instance's surfaces).
- **The virtual bus.** A per-instance broker (the `SharedMedium` pattern from radio) carries
  frames between a micro:bit instance and its attached simulated modules, delivered at frame
  boundaries in stable order. The module implementations are protocol-real (announce, registers,
  events on the wire), so the bus core sees the same traffic shape hardware produces.
- **Modules are components with a UI-free functional core.** A simulated module's behavior --
  protocol answering, state, declared observable channels, offered scenario inputs -- is a
  functional core with no UI and no free-running time: the host owns the clock, and the core is
  headless-runnable (no DOM), so the same core serves the visual surface AND deterministic
  headless rehearsal. The iframe surface is one HOST of that core (free-running allowed, UI
  layered over it, subscribing to the core's emitted state); it is never the only holder of
  module state. The frame channel is the bus-I/O operation's transport; lifecycle, tick, and
  scenario-input delivery are their own operations of the same component contract.
- **The module core is a vetted engine under approved-library admission.** The jacdac module
  cores are an existing, vetted protocol engine the library ships, admitted because the library
  is APPROVED and the engine payload is VERSION-PINNED: what runs is exactly what was vetted,
  and an engine update is a re-approval event. The core executes realm-confined in every host:
  separate globals, host-injected clock and seeded rng, no ambient timers, network, or module
  access, with synchronous lockstep ticks -- headless in the browser (where rehearsal runs) and
  headless in Node (where conformance gating runs) alike. A hung or runaway core fails its run
  with a stable error under the host's tick-time policy; the sandboxed surface iframe remains
  the visual host's boundary and is distinct from these headless realms.
- Ownership: `apps/microbit-sim` owns the accessory catalog UI, the surface host and
  window-management frame, and the channel broker; the jacdac library owns the module UI and
  simulation behavior inside its surfaces; `packages/wodal` owns the bus core and the
  injectable wire-input mechanism the broker drives.

## Assistant

The LLM assistant must work skillfully with jacdac when the library is installed: reach for
role tiles when they fit the program being built, respect presence semantics, and choose the
right form (event tile vs register read) for the job.

- **Visibility.** Role tiles reach the assistant through the same instance catalog the editor
  and compiler compose (`instance.tileCatalog`, for the brain's assigned instance); the
  session's catalog digest includes them with their descriptions.
- **Generated docs.** The provider generates each published tile's documentation from the
  role's display name, the service specification's own prose (jacdac's per-service,
  per-register, and per-event descriptions, units, and ranges), and the tile's stance
  semantics (poll over cache, presence-gated, silent no-op while unbound). Docs lead with a
  one-paragraph behavioral summary -- the machine-consumed portion, per the dynamic-tiles
  assistant surface -- and carry an `assistant` section for rules the grammar cannot state
  ("fires only while a module fills the role").
- **Library assistant note.** The library ships a short concept note that joins the
  assistant's session context: what roles are, that replacement rebinds automatically, that
  unbound roles read as absent and never error, and when to prefer event tiles over register
  tiles.

## micro:bit-v2 target

- **Hardware backing.** Jacdac data is single-wire serial on edge-connector pin **P12** at
  1 Mbaud (the pin pxt-jacdac binds; CODAL provides the substrate as
  `codal::ZSingleWireSerial`, the nRF52 UARTE + DMA implementation). The protocol stack is the
  vendored **jacdac-c** library over that PHY. Its memory is STATIC bounded structures --
  frame queues, tables, and rings sized by protocol constants, the radio-ring pattern -- never
  the VM arena and never runtime heap allocation: the firmware's RAM is almost fully
  statically placed, so a heap-backed stack has nothing to draw from. IRQ and CODAL-event
  handlers enqueue only; the firmware main loop drains (`jacdac.pollRx()`-shape, the
  `MicroBitRadioPort` pattern) before `hostLoop.tick()`.
- **Memory budget is stated, never absorbed.** The on-flash user-program region starts at the
  page-aligned firmware end (only the region end is fixed), so jacdac's flash cost trades
  page-for-page against user program capacity -- there is no wall, only that trade. A build
  that adds jacdac states its region cost; RAM the stack needs beyond the firmware's slack is
  taken from the VM arena size explicitly, with the arena delta stated in the same change.
- **Port.** A board-agnostic `JacdacPort` in `cpp/codal/device-port.h` (a `DevicePorts` member),
  shaped by the bus-core surface above: device table, register cache, event ring, outbox. The
  concrete `MicroBitJacdacPort` binds jacdac-c. Ring depths and cache bounds are tied to
  jacdac-c protocol constants, not guesses.
- **ABI anchors.** TBD: `MicroBitField.Jacdac`, the type-atom block, the host-function block,
  mirrored in `cpp/targets/microbit-v2/abi/` with value-pinning tests. The registry index lives
  in `docs/specs/microbit-context.md`.
- **Trace.** Bus-core crossings emit `port jacdac ...` lines pinned in
  `docs/specs/contracts/observable-trace.md`: outbound writes/commands, and the injected-input
  schedule positions for announces, reports, and events.

## Protocol capability coverage

Per the full-surface-design principle, the Jacdac protocol capability set is accounted for; what
ships is a subset, each gap marked:

- **Shipped (bus core):** device discovery via announce + timeout removal; register read through
  the streamed/refreshed cache; register write; commands; event delivery; the jdpack subset
  codec; presence.
- **Bus-core internal (not surfaced):** frame CRC, ACK handling, retry/backoff, announce
  self-identification as a client, streaming re-arm mechanics. Correct wire behavior, no user
  surface.
- **Role semantics without the wire service:** Jacdac's role-manager SERVICE (an on-wire
  protocol with pipes and settings persistence) is not implemented; its SEMANTICS -- named
  roles, preferred bindings, auto-bind, identify-driven assignment -- are, split across the role
  table (host), the binding table (bus core), and the assignment UI (app). See Roles.
- **Composable in the library:** value scaling and units; event decoding. **Default coverage:** because the generic
  register pair needs no per-class code, the provider can publish read/write tiles for any
  service class whose registers it recognizes as value slots, even classes with no hand-crafted
  tiles -- the tile analog of the Jacdac dashboard's default widget for unmapped services.
  Hand-crafted per-class tiles (better labels, richer grammars, event tiles) refine popular
  classes over time.
- **Designed out (no consumer):** pipes (bulk transfer); the role-manager wire service; the
  settings, firmware-update, and infrastructure services; proxy/dashboard advertisement;
  brain-as-peripheral (exposing the micro:bit's own display/buttons/sensors as Jacdac services)
  -- see Open questions.

## Conformance

- The wodal microbit module is the oracle; the C++ target mirrors it at the bus-core surface
  (Device API semantics + trace lines), not at the protocol-stack level -- wodal and jacdac-c
  implement the wire independently; goldens pin the observable surface.
- Golden fixtures replay an injected wire-input schedule (announces, reports, events at fixed
  ticks -- the button-press injection pattern) against a brain using the Device API and
  library-published tiles; both VMs byte-match the trace, including attach, detach at
  announce-timeout, cache staleness, ring overflow, and cursor snap-forward.
- The simulator's virtual bus routes through the same injection entry point the goldens use, off
  the parity path.
- Parity scripts are OPEN-LOOP schedules: fixed frames at fixed ticks, never reactive. The
  moment a script needs stateful device behavior -- answering a read, reacting to a write --
  that behavior is a component and belongs behind the component contract, not in the schedule.
  This is the boundary between golden-parity scaffolding (platform primitive) and simulated
  modules (components).
- The ambient `.d.ts` typechecks the Device API; the library's handlers and provider compile
  through the extension test harness.

## Open questions

1. **Brain vs peripheral scope.** This draft is brain-only (the micro:bit consumes module
   services). Peripheral mode (advertising the micro:bit's own capabilities as Jacdac services,
   as MakeCode does) doubles the surface and has no driving user story; needs an explicit
   decision to stay out of scope or be designed later.
2. **Subscription model.** Explicit `subscribe` (as drafted, the library System re-arms) vs
   implicit subscribe-on-first-read (simpler for Device-API users, hides a wire side effect
   inside a read). Also the streaming refresh policy: interval, and whether the cache exposes
   staleness to user code beyond nil-vs-value.
3. **jdpack subset boundary.** The exact format-string subset the bus core codec supports
   (repeats `r:`? strings `s`/`z`? multi-field returns beyond the first field?), and the stable
   refusal codes for unsupported formats.
4. **Event tile cursor arming.** Published event tiles: page-enter arms to ring head (the radio
   tile rule) -- confirm the same rule applies when a tile is newly published mid-page.
5. ~~**Device metadata for labels.**~~ RESOLVED by roles: labels derive from the role's display
   name -- seeded from module self-description where available (service instance-name, else
   product/device description, else the service class name; see Minting in Roles),
   ordinal-suffixed for duplicates, user-renamable; self-description never participates in
   identity. Disambiguation of identical modules is the identify-driven assignment flow (see
   Roles and Simulator).
6. **Ring and cache bounds.** The event-ring depth and register-cache eviction policy, tied to
   jacdac-c constants on native; the wodal oracle must mirror the exact bounds for parity.
7. ~~**Dynamic-tiles prerequisites.**~~ RESOLVED: the `docs/specs/dynamic-tiles.md` questions
   this spec depended on are settled -- publication is exclusively host-invoked and edit-time,
   grammar encoding is moot (declarations never cross the runtime boundary), and the roster is
   realized as host-owned injected tile catalogs, which is what this spec's per-instance
   catalog design builds on.
8. ~~**Class-addressed tiles and module replacement.**~~ RESOLVED by roles: tiles bind roles,
   not serials, so replacement rebinds automatically (preferred-else-auto-bind) and no separate
   class-addressed tile kind is needed -- a role with no valid preference IS class-addressed
   first-match. See Roles.
9. **Role persistence details.** Storage location, scope, and deploy baking are settled (see
   Roles: project-scoped table + per-instance bindings in the project file; deploy bakes the
   assigned instance's bindings). Still open: the serialized form in the project file; whether
   on-device auto-bind updates (the replacement module becoming preferred) persist across power
   cycles on hardware, which would require flash-backed settings storage the target does not
   currently reserve -- or whether device-side preference updates are session-scoped, with
   durable preference changes happening only at authoring time; and role lifecycle --
   unplugging is presentation-only (defs persist; see Three tile states in Roles), but when a
   role leaves the TABLE entirely (explicit user delete, or a pruning policy for unbound roles
   no rule references) is undecided.
