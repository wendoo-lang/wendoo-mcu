# Spec: Dynamic tiles

A library may supply tiles whose existence and shape are not known when the library is written. A
**tile provider** declared in library source derives a set of **dynamic tiles** from live data -
devices attached to a discovery service, endpoints reported by an API, entities listed by a
running subsystem - and the host registers that set in its **tile roster**. Each published tile
carries its own label, icon, documentation, and its own argument grammar, all derived from what the
subsystem reported, while every tile in a family dispatches into one statically compiled body.

**Publication is an edit-time event.** Only the authoring host invokes a provider; provider code
never runs inside a compiled program, and a flashed program carries no publication machinery (see
Semantics). What a published tile compiles to - a handler call with bound constants - is the whole
runtime story.

This is a generalized capability, not a device feature. Device discovery is its motivating consumer,
but nothing in the mechanism is specific to devices: a provider derives tiles from whatever data
the authoring host can reach for it - an accessory bus's attached modules, an OpenAPI document's
operations, a SCADA gateway's tag tree.

The mechanism rests on a separation the platform already makes: a compiled action bundle carries
tile metadata and compiled artifacts as separate collections, and many tiles may bind one action.
Dynamic tiles extend the metadata half to be publishable at edit time; the code half stays
statically compiled, so nothing here requires code generation, on-device compilation, or a second
execution model.

ABI ids: TBD.

## Ownership

- **The host owns the tile roster, realized as tile catalogs.** The roster is not a new registry:
  it is one or more host-owned `ITileCatalog`s -- the brain editor's existing multi-catalog
  composition (`BrainEditorContext.tileCatalogs`) -- that the host rebuilds from provider
  derivations and injects into editing and compile sessions. Libraries do not hold it, and it is
  not project content: catalogs are derived state, rebuilt at will. (A provider's DOMAIN may
  itself be project content -- a curated identity table -- but the catalogs derive from it.)
- **The authoring host is the only publisher.** The host invokes a provider and applies the
  returned set; nothing publishes from a running program. One publisher per roster means two
  providers ticked from different contexts can never race or clobber each other's sets.
- **The subsystem the provider queries is the source of truth.** A provider does not maintain a
  durable list of what exists; it derives what the subsystem currently says exists, each time the
  host invokes it.
- **Publication is scoped to a provider.** Each invocation replaces that provider's entire
  previously registered set. The host removes a tile by re-invoking the provider and applying a
  set that omits it.

Because the subsystem is authoritative, a tile whose backing instance is gone is **cleaned up**: the
host removes it from the roster. The properties that make this safe - deterministic identity and
preserved references - are normative and specified below.

## Authoring surface

**The provider** is a declaration, a sibling of `Sensor`, `Actuator`, and `System`:

- `TileProvider({ name, id?, provide })` - declares a provider identity and its derivation. `id`
  is the stable opaque identifier assigned on first compile, treated as opaque exactly as for
  other declarations; the provider identity is what scopes a publication's replacement.
  `provide()` is a pure derivation - domain state in, `TileDeclaration[]` out - invoked only by
  the authoring host (see Semantics). It reads its domain through the same subsystem surface the
  library's runtime code uses, host-mediated; it performs no side effects and holds no state.

**The handler** is an ordinary `Sensor` or `Actuator` declaration marked as a handler. A handler is
the compiled body every tile in its family dispatches into. It is not itself offered in the picker.
A handler declares the full argument vocabulary its family can receive; published tiles select from
that vocabulary.

Each entry of the returned declarations carries:

- `instanceKey` - the identity of the thing this tile represents, supplied by the subsystem. With the
  library coordinate and the provider, it determines the tile id (see Identity).
- `kind` - `sensor` or `actuator`.
- `handler` - a reference to the handler declaration this tile dispatches into. A reference, not a
  name string, per the standing preference for type and binding references over strings.
- `bind` - constant values baked into named handler arguments for this tile. This is how a tile
  carries which instance it addresses without the user seeing or typing it.
- `args` - this tile's own argument grammar, built from the ordinary combinators. Each argument names
  the handler argument it `feeds`.
- `label`, `icon`, `docs`, `tags`, `language`, `outputs` - display and catalog metadata, exactly as
  on a statically declared tile.

## Usage sketch

```ts
// One compiled body backs every published reading tile. It declares the full
// argument vocabulary the family can receive; a published tile uses a subset.
const ReadCapability = Sensor({
  name: "read capability",
  handler: true,
  returnType: "number",
  args: [
    param("device", { type: "string", anonymous: true }),
    param("capability", { type: "string", anonymous: true }),
    optional(param("threshold", { type: "number" })),
  ],
  onExecute(ctx, args) {
    return discovery.read(args.device as string, args.capability as string);
  },
});

// The provider is a pure derivation the AUTHORING HOST invokes whenever it
// observes the discovery domain changed (a device attached or detached, the
// session opened, the library installed). Each invocation's result replaces
// the provider's entire registered set. No brain code references `provide`,
// so it is tree-shaken out of every compiled program.
const AttachedDevices = TileProvider({
  name: "attached devices",
  provide(): TileDeclaration[] {
    const declarations: TileDeclaration[] = [];
    for (const device of discovery.list()) {
      for (const capability of device.capabilities) {
        declarations.push({
          // Identity comes from the device, never from enumeration order.
          instanceKey: `${device.serial}.${capability.id}`,
          kind: "sensor",
          handler: ReadCapability,
          bind: { device: device.serial, capability: capability.id },
          label: `${device.name} ${capability.label}`,
          icon: capability.icon,
          args: capability.comparable ? [optional(param("above", { type: "number", feeds: "threshold" }))] : [],
        });
      }
    }
    return declarations;
  },
});
```

In the editor, each published tile appears in the picker as an ordinary tile of its kind, reading
with its own words and offering its own arguments. Nothing distinguishes it from a statically
declared tile at the point of use.

## Semantics

- **A tile id is a pure function of coordinate, provider, and instance key.** Publishing the same
  instance key from the same provider in the same library always produces the same tile id, on every
  machine, in every session, in any order. Nothing about the publication order, the session, the
  connection handle, or the number of instances may enter the id.

  This is the property the whole design rests on, and the failure it prevents is severe: an
  identity derived from enumeration order would silently rebind a saved rule to a different instance
  the moment one instance disappeared, changing what a program does without changing the program.
  An `instanceKey` must therefore be derived from the subsystem's own durable identity for the
  thing - a device serial, a stable resource id - never from an index, a handle, or a mint at
  publication time.

- **An unresolved reference is preserved, never destroyed.** A brain may reference a dynamic tile
  that the roster does not currently hold, because the instance is absent. The reference survives
  loading, editing, and saving of the containing brain byte-identically. No load-time repair, no
  silent drop, no rewrite. A rule holding an unresolved reference renders as an unresolved tile and
  does not compile, and the rest of the brain compiles and runs normally.

- **Preserved is a waiting state, not an error.** An unresolved tile renders from its display
  hint in a neutral preserved style -- the thing the author placed, dimmed or ghosted -- never
  in an error style, with no error-marker label prefix and no parse-error surfacing. The
  affected rule gates out quietly; nothing asks the user to repair anything, because restoration
  is automatic when the instance returns. Error styling is reserved for states the user must
  act on; an absent instance is not one.

- **Restoration is automatic and requires no repair step.** When the instance returns and its
  provider publishes it again, the deterministic id resolves the preserved reference and the rule is
  whole. Removing a tile from the roster is therefore a recoverable act rather than a destructive
  one, which is what makes cleanup on disappearance safe.

- **A reference carries a display hint.** Alongside the tile id, a brain persists the tile's label as
  captured when the reference was authored. The hint is non-authoritative: it never participates in
  resolution, compilation, or identity, and a resolved tile always renders from the roster. Its only
  purpose is that an unresolved reference can render as the thing the author placed rather than as an
  opaque key.

- **A published tile's grammar is free-form over the handler's vocabulary.** Two tiles from one
  provider may differ in argument count, kinds, labels, defaults, and structure. The handler's
  declared argument vocabulary bounds the family: a published tile may feed only arguments the
  handler declares, and handler arguments neither bound nor fed arrive absent under the ordinary
  missing-optional-slot convention. A published argument whose type does not match the handler
  argument it feeds, or that names an argument the handler does not declare, is refused at
  publication with a stable code; it is never silently dropped or coerced.

- **Invocation is the host's choice, driven by its view of the domain.** The host invokes a
  provider when it observes the provider's domain changed - a device attached or detached, a
  session opened, the library installed. How the host observes change is host-and-consumer
  specific; the provider itself stays a pure derivation. Because the host can see the domain
  without any brain running, tiles appear the moment the domain does - publication never waits on
  a program to run.

- **A roster change is applied at a quiescent point.** When brains are running, the host applies a
  publication between thinks, never mid-fiber, in keeping with the single-entry rule. It then
  rides the ordinary catalog-change path: applying a publication recompiles what the change
  affects, bumps the catalog revision, and reports invalidated brains, exactly as any other change
  to the registered tile set does. Dynamic tiles introduce no parallel refresh mechanism.
  Invalidation follows catalog composition: a change to one catalog affects only the brains whose
  sessions compose that catalog.

- **Publication does not exist at runtime.** Providers are invoked only by the authoring host, and
  no brain code references a provider's `provide`, so reachability tree-shaking excludes the
  provider - its derivation code, labels, and grammar construction - from every compiled program.
  A flashed program carries no publication machinery and pays no flash, memory, or cycle cost for
  it; the on-device behavior of a published tile is entirely its handler's.

- **A compiled program never depends on the roster.** A published tile compiles to a call into its
  handler with its bound constants baked in. A flashed program therefore carries no reference to the
  roster, the provider, or the publication that produced it, and runs identically whether or not the
  authoring host ever sees the instance again. A handler that addresses an instance no longer present
  at runtime reports absence through the ordinary value-absence path rather than a new error class.

- **Two providers never collide.** Provider identity participates in every tile id, so two providers
  in one library, or the same instance key published by two different libraries, produce distinct
  tiles. A single provider returning one instance key twice in one derivation is refused with a
  stable code.

- **Uninstalling a library removes its providers' tiles.** The roster drops them, and references to
  them are preserved under the same rule as any other absent instance, so reinstalling the library
  (whereupon the host re-invokes its providers) restores them.

## Identity and ABI anchors

- Tile id: `tile.<kind>-><coordinate>:dynamic.<kind>.<provider>.<instanceKey>`, paralleling the
  statically declared user-tile form `tile.<kind>-><coordinate>:user.<kind>.<id>`. The coordinate
  namespaces the library, the provider scopes the publication, and the instance key identifies the
  thing.
- Argument tile ids for a published tile's own arguments derive from that tile's id plus the argument
  name, so they inherit the same determinism.
- Provider stable id: assigned on first compile from the declaration, treated as opaque, never edited
  or reused.
- There is no publication host function: publication is host-side only and needs no runtime ABI id.
- Refusal codes for a derivation result (unknown handler argument, type mismatch, duplicate instance
  key, unknown handler): TBD, allocated in one family; raised by the host when applying a
  provider's returned set.

## Assistant surface

Published tiles must be as legible to the LLM assistant as statically declared tiles are; the
mechanism treats the assistant as a first-class catalog consumer.

- **Catalog reach.** Assistant authoring workspaces compose the same catalogs an editing or
  compile session composes, including the host-injected catalogs that realize the roster. A
  published tile is visible to the assistant's catalog tools and counted in the session's
  catalog digest exactly when it is offered in the picker.
- **Description channel.** A declaration's `docs` prose feeds the assistant's per-tile
  description the same way a static tile's documentation does. The machine-consumed portion is
  the paragraph between the title heading and the first subheading, so published docs lead with
  a one-paragraph behavioral summary; content below the first subheading addresses human
  readers only.
- **Grammar notes.** A published tile may carry a one-sentence note for a usage rule its
  argument grammar cannot state; the note reaches the assistant alongside the grammar.
- **Digest scale.** Every offered tile is one line of the assistant's catalog digest, so a
  provider's cardinality weighs directly on the model's context. The argument-space extension
  (see Open questions) is the pressure valve for large-cardinality domains.

## First consumer

A device discovery library is the motivating consumer and exercises the mechanism end to end: a
subsystem reports attached devices and their capabilities, one handler per capability class backs
every published tile, per-device parameters and modifiers come from what each device reported, and
unplugging a device removes its tiles while leaving every rule that used them recoverable.

Note on instance identity from that consumer: its `instanceKey` is not the raw hardware serial
but a durable ROLE - a named slot in a host-maintained table that the current hardware fills -
so that swapping a device for an equivalent one preserves every tile id. The "subsystem" a
provider derives from may itself be such a curated identity layer over the raw domain; the
determinism rule requires only that the key be durable and never enumeration-ordered.

## Open questions

- ~~**Grammar encoding across the runtime boundary.**~~ RESOLVED (moot): providers run in the
  authoring host, so declarations - grammar trees included - are host-native values that never
  cross the VM/host runtime boundary. No encoding is needed.
- ~~**Provider invocation by the host.**~~ RESOLVED: publication is exclusively host-invoked.
  Library-driven runtime publication was rejected on three grounds its first consumer exposed:
  it cannot bootstrap (per-brain reachability means no provider runs until a published tile is
  already placed), concurrent programs replaying one provider's replacement-set clobber each
  other, and the derivation work (labels, grammar construction) would ride into flashed programs
  as flash, memory, and cycle cost for a purely edit-time function.
- ~~**Roster scope.**~~ RESOLVED by the catalog realization: there is no single global roster to
  scope. The host owns one catalog per provider-domain instance it chooses -- per device bus, per
  project, per connection -- and injects into each session the catalogs relevant to that brain
  (e.g. the catalog of the device instance the brain is assigned to). Two open projects hold
  their own catalogs; a library installed by both derives into each project's catalogs
  independently. Tile identity stays deterministic and catalog-independent, so the same tile id
  appearing in several catalogs is one tile, and a brain moved between compositions keeps
  resolving.
- ~~**Display hint storage.**~~ RESOLVED: the storage already exists as the missing-def record
  (`BrainTileMissingDef` / `MissingTileJson`: tile id, original kind, label) -- serialized with
  the brain, minted into the catalog at load when the real definition is absent, superseded by
  the real definition when present, which is what makes restoration automatic. The mechanism is
  upgraded from its paste-placeholder origins (error-marker label prefix, parser error) to the
  first-class preserved rendering above; extending the record with an icon hint is an open
  detail of that upgrade.
- **Localization.** Published labels come from a subsystem and are not localizable through the
  catalog. Whether providers may supply localized forms, or published labels are permanently
  pass-through.
- **Outputs on published tiles.** Whether a published tile may declare outputs whose identities the
  handler does not statically declare, given that output identity is `(type, name)` and shared by
  construction.
- **Argument-space publication.** A provider currently publishes whole tiles, which is wrong at
  large cardinality: a domain with thousands of instances (a SCADA tag tree) wants ONE static tile
  with a dynamically published argument vocabulary (a browsable tag-path parameter), not one tile
  per instance per aspect. Whether a provider may publish parameter/modifier vocabularies for a
  static tile's grammar, and how deterministic identity and reference preservation extend to
  published argument values. The identity parameter must also admit a runtime-computed expression
  (indirect addressing), which the ordinary-typed-argument design already permits -- a published
  vocabulary is an affordance for picking, never a constraint on what the argument accepts.
- **Edit-time domain access.** How the host mediates a provider's access to its domain: a local
  bus the host already models is one posture; fetching an OpenAPI document or holding a gateway
  session on the provider's behalf is a capability grant (network, files). What the host offers,
  how a library declares what its providers need, and whether the user consents per library.
- **Trust.** A library that publishes tiles can name and shape them freely. Whether any constraint
  on published metadata is warranted, and what it would prevent. The question sharpens for
  providers whose domain access reaches the network at edit time (see Edit-time domain access)
  rather than a local bus.
</content>
</invoke>
