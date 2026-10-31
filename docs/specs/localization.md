# Localization

Localization makes every user-visible string and document in Wendoo renderable in the
user's language. It covers the brain editor, the web apps, tile labels and type names,
compiler diagnostics, and the documentation system. It does not touch the VM, bytecode,
brain documents, or any persisted identity: localization is a display-time concern, and
nothing localized is ever stored or executed.

## Design principles

1. **English source strings are the keys.** A translatable string is keyed by its English
   text. There is no invented key vocabulary; the source code reads naturally; the
   fallback for a missing translation is the key itself, so partial catalogs degrade
   gracefully by construction. A string may carry an optional context tag used only when
   the same English text needs different translations in different roles (for example
   "Stop" as a button versus "stop" as a tile label). Editing an English source string
   intentionally orphans its translations; tooling surfaces the orphaned entries rather
   than silently keeping stale ones.

2. **Nothing localized is persisted.** Tile ids, type identity keys, doc content keys,
   brain documents, bytecode, trace output, and golden fixtures all remain
   English-stable. A saved brain opened under any locale is byte-identical; switching
   locale is a pure re-render. The one exception is user-created content seeded from a
   localized default (a new page named "Unnamed Page" in the user's language): the seeded
   value becomes ordinary user content at creation time and is never re-localized.

3. **Structured data first, prose at the edge.** Anything that must be displayed in the
   user's language is carried as a code or key plus named parameters until the moment of
   display. Prose is rendered at the display layer by the localizer. This is the same
   posture the codebase already takes for machine-readable diagnostics, extended to its
   conclusion.

4. **The localizer is an injected service.** Core stays platform-agnostic: the localizer
   uses no browser, Node, or Intl APIs and holds no global state. Hosts construct it with
   their catalogs and current locale and inject it through the existing service
   aggregates and editor config.

## The message model

### Keys and catalogs

A message key is the English source string, optionally qualified by a context tag:

- `"Search tiles"` -- a plain key.
- `("Stop", context: "tile-label")` -- a contextualized key, used only where a collision
  in meaning exists.

A catalog is a per-locale mapping from key (with optional context) to a translated
template. Catalogs are authored as plain JSON files (one per locale) and compiled by a
build step into generated TypeScript modules, mirroring how the docs content build
already works. The English catalog is implicit: the identity mapping. A missing entry in
any catalog falls back to the English source string, per entry.

### Templates and interpolation

Templates use named placeholders, never positional ones, so translations reorder freely:

- English: `Tile "{tile}" requires the WHEN to produce a {type} result`
- A translation may say the equivalent of `A {type} result from the WHEN is required by
  tile "{tile}"` -- same parameters, different order.

The template language is a minimal, explicitly bounded subset of ICU MessageFormat:

- **Named arguments:** `{name}` substitutes a parameter.
- **Plurals:** `{count, plural, one {# error} other {# errors}}` selects a branch by the
  locale's plural rules; `#` substitutes the number. Plural-rule data ships with the
  catalog build, not with core.
- **Select:** `{side, select, when {...} do {...} other {...}}` for enumerated variants.

Nothing else from ICU (no dates, ordinals, nested formats, skeletons). A literal brace,
`#`, or backslash is written with a backslash escape (`\{`, `\}`, `\#`, `\\`); brace
doubling is not an escape, because a doubled close brace is ambiguous where a branch body
ends directly before a placeholder close. Template parsing is total -- malformed spans
render verbatim and are reported as issues -- and the catalog build is where malformed
templates fail loud. Dates and number formatting beyond plain digits are out of scope
until a real surface needs them; the one current `toLocaleString()` call site is treated
as host-side formatting, not part of this system.

### List glue

Joining lists with a conjunction ("expected Number or String") is language-specific. The
localizer provides `list(items, kind)` where kind is `"or"` or `"and"`; templates receive
the joined result as a single parameter. No call site may `join(" or ")` by hand.

### The localizer service

```
interface Localizer {
  locale(): string;
  tr(source: string, params?: Record<string, LocalizedValue>, context?: string): string;
  list(items: readonly string[], kind: "and" | "or"): string;
  compare(a: string, b: string): number;
  foldForSearch(s: string): string;
}
```

`compare` orders DISPLAY strings (sorted library names, sorted doc entries) by the
locale's collation; it is never applied to machine keys, ids, or anything whose order
reaches a compiled or persisted artifact -- those keep ordinal comparison so build
outputs stay deterministic across locales.

`LocalizedValue` is a string or number; strings passed as parameters are inserted as-is
(vocabulary localization of parameters happens before the call -- see Diagnostics).
The service slots into the `AppServices` aggregate in core, and reaches the editor
through `BrainEditorConfig`; React surfaces consume it through a provider hook so a
locale change re-renders. The vestigial Roblox system (`src/i18n/Core/init.csv`,
`ITranslatorService` on the game-services aggregate) is deleted and replaced by this
service.

## Vocabulary: tile labels and type names

Tile labels and type display names are the shared vocabulary that appears both as
standalone UI (the picker, variable factories) and inside diagnostic prose. They are
localized through the same catalogs, under dedicated contexts:

- **Tile labels** (`context: "tile-label"`): a tile's display label is authored in
  English (metadata label or derived default) and passed through the localizer at every
  display site. The existing resolution chain (metadata label, then derived defaults,
  then catalog fallback) gains exactly one step: the resolved English label is looked up
  before rendering. Labels remain English in tile definitions, documents, and ids.
- **Type names** (`context: "type-name"`): the core type display names and registered
  user-type display names route through the same lookup at display time. Canonical
  registry names (identity) remain English and lowercase as today.
- **Field/accessor names, output names, modifier labels**: same treatment, same context
  family.

User-authored vocabulary -- variable names, page names, brain names, and the `name:` of
user-code tiles -- is user content and is not localized. (Extension-shipped tiles may
carry their own catalogs one day; the extension design owns that hook, and this spec only
requires that the lookup consult catalogs by key, so additional catalogs compose.)

**Picker search** is a character-bag match (every typed character present in the
candidate, any order, case-insensitive) against the tile's label. Under localization it
matches the DISPLAYED label -- the localized one, or the English source exactly where no
translation exists, which the per-entry fallback provides for free. It deliberately does
not match both languages at once: bag matching loses its selectivity when the candidate
character pool doubles, and matching what the user sees is the expectation anyway. The
localizer supplies a `foldForSearch` normalization (case plus diacritic folding, applied
to query and candidate alike) so `uber` finds `über`; the bag matcher adopts it.

Folding belongs to SEARCH, never to IDENTITY. A comparison that decides whether two
names denote the SAME thing -- a typed variable name against the variables that already
exist, a key, an id -- is a key comparison, and folding it would merge distinct names:
under `foldForSearch`, a newly typed `cafe` would silently become the existing `café`.
This is the same exemption `compare` carries for machine keys, stated for folding
because the two functions are reached from adjacent code. A surface that both searches
and identifies applies the fold to the first and not the second.

Locale-natural synonyms have a home in DOCS search without a dedicated mechanism: the
docs manifest's per-tile tag lists localize per language, and a locale's tag set may
deliberately diverge from the English one to add the terms native speakers actually
reach for. Docs search matches the localized and English tag sets alike; coverage
tooling diffs tag sets like any other content. The picker has no tag index today, and
adding one is a search feature outside this design's scope.

## Diagnostics

This is the one structural change. A diagnostic today carries a code and a pre-baked
English message string; interpolation happens at the emit site. Under localization:

- **Every diagnostic code has exactly one English template**, held in a per-family
  template registry (brain-compiler and ts-compiler each own theirs). Emit sites stop
  writing prose inline: they call a constructor with the code and named parameters, and
  the English `message` field is rendered from the template at emit time (so logs, test
  output, and non-localized consumers are unchanged and cannot drift from the template).
- **Diagnostics carry their parameters.** The diagnostic shape gains
  `params: Record<string, string | number>` alongside `code`, `severity`, and `message`.
  Parameters that are vocabulary (a tile label, a type name, a field name) are carried as
  their English display term.
- **Display layers render localized prose** in two levels: each vocabulary parameter is
  first localized through its context (`tile-label`, `type-name`), then the code's
  template -- localized through the message catalog -- is interpolated with the localized
  parameters. Badges, tooltips, and build dialogs render this way; a locale switch
  re-renders every visible diagnostic without recompiling, because the stored diagnostic
  is code + params.
- **Tests assert on codes and params**, not prose. The existing prose-substring
  assertions migrate to `code` plus `params` equality; template wording changes then
  never break tests, and translated runs behave identically.

The diagnostic-code disciplines are unchanged: codes are append-only, machine-readable
first, and the template registry gives the extraction tooling a complete inventory of
every diagnostic message in one place per family.

The code+params shape applies to EVERY diagnostic-carrying family, not only the two
compilers: wodal's build diagnostics, the service-api document-validation diagnostics,
the app-host library-operation results, and the simulator's flash/fault entries all
carry code + params, and every downstream carrier (the bridge, app diagnostic lists,
toasts) preserves the structure rather than pre-composing prose. A carrier that joins
several messages into one string, or splices a code into a sentence by hand, defeats
display-time rendering and is out of contract.

**Command descriptions** follow the same posture: an undoable command exposes its
user-facing description as a source string plus named parameters (including plural
counts), and the undo/redo UI renders it through the localizer. Command objects never
carry pre-baked display prose.

## Documentation

The docs subsystem already separates a locale-independent manifest (tile id, tags,
category, content key) from per-language content trees, with a build step generating a
TypeScript module per locale. Localization completes that shape:

- **Content trees per locale:** `content/{locale}/tiles/*.md` and
  `content/{locale}/concepts/*.md`, mirroring the English tree's relative paths and
  content keys exactly. The build generates `_generated/{locale}.ts` per locale present.
- **Per-document fallback:** the loader resolves a content key against the current
  locale's generated module and falls back to English per key, so a partially translated
  language shows translated pages where they exist and English elsewhere -- never
  all-or-nothing.
- **Manifest strings:** categories are display strings and localize through the message
  catalogs (`context: "docs-category"`). Tags are search vocabulary: search matches both
  the localized and English tag sets. Content keys and tile ids stay English.
- **Cross-references** (`tile:tile.op->sub`) are ids and survive translation untouched;
  the renderer resolves them to localized labels at display time.

Doc markdown is authored per language as whole files -- no in-file string splicing. A
translated page is a real document a native speaker can write naturally.

**Library-authored tile docs are a separate channel.** A library ships one markdown
document per tile (`docs: "./docs/x.md"` in its tile config), compiled into the tile's
metadata and keyed by tile id -- no content key and no locale dimension. Localizing that
channel belongs to the library system's authoring surface (alongside library-shipped
catalogs, names, and descriptions); until it exists, the docs viewer shows the one
authored document for every locale. The docs registry composes app, core, and library
sources, so a per-locale library channel slots in without reshaping the registry.

## Surfaces and their integration

- **packages/ui (editor chrome):** all literals route through the provider hook (`tr`) --
  dialog titles, buttons, placeholders, menu items, tooltips, the candidate strip's
  headings and live-region status lines, and the library-browser UI (whose user-facing
  vocabulary is "library/libraries"; code identity stays "extension"). The ui package
  also ships the locale-selection component and the provider owns locale switching, so
  every app reuses one implementation.
- **packages/docs (docs viewer):** the docs sidebar/page/print components and their
  strings (counts, headings, aria labels) localize like editor chrome. The docs provider
  is its own injection seam and receives the localizer alongside the registry.
- **apps/microbit-sim and apps/ecosim:** same pattern via each app's provider; each app
  owns its catalog entries, including its flash/fault/library toasts and dialogs. Locale
  persists in the app's own settings store through a small settings adapter (get/set of
  the locale code) the app injects via the provider config; the ui-shipped selector
  reads and writes through it.
- **vscode-extension / bridge:** split where VS Code splits. Static contributions in
  `package.json` (command titles, settings descriptions, menus) can only be localized
  through VS Code's native `package.nls.<locale>.json` bundles, so a build step
  generates those bundles from the shared catalogs. Everything dynamic (tree items,
  status bar, prompts, CodeLens titles, and the diagnostics pass-through, which carries
  code + params like every other display surface) renders at runtime through the shared
  localizer with the catalogs bundled into the extension. The extension's locale
  follows VS Code's display language. VS Code icon tokens (`$(warning)` and kin) embed
  in templates verbatim and must survive translation and pseudo-localization unaltered.
- **Compilers:** template registries + structured params as above. The ts-compiler's
  user-facing config diagnostics are included; its internal/tooling output is not.
- **app-host and service-api:** their diagnostic/result messages reach users through app
  toasts and dialogs; they join the code+params contract with their own template
  registries.
- **wodal:** its build diagnostics are user-visible and join the code+params contract.
  Its runtime/device layer, trace output, and fault codes are developer/machine surfaces
  and stay English.
- **cli:** developer-facing output; deliberately out of scope.
- **VMs / cpp:** no user-visible strings; out of scope.

## Tooling

- **Extraction:** a build tool walks the sources for `tr(...)` call sites, the diagnostic
  template registries, the vocabulary registries (tile labels, type names), and the docs
  manifest, producing the canonical key inventory. The English catalog is that inventory.
- **Coverage:** per locale, the tool reports missing keys (untranslated) and orphaned
  keys (translations whose source string no longer exists). Orphans are removed by a
  human, not automatically, since near-miss source edits may want the old translation as
  a starting point.
- **Pseudo-locale:** a generated `qps` locale (accented, bracketed, length-expanded
  English) exercises every localized path without real translations: layout stress,
  missing-lookup detection (unlocalized strings visibly lack the markers), and
  interpolation correctness. It is generated from the inventory, never hand-edited.
- **Docs coverage:** the same tool diffs each locale's content tree against English by
  content key.

## Non-goals

- Right-to-left layout and bidirectional text (revisit when an RTL locale is scheduled).
- Full ICU MessageFormat (dates, ordinals, nesting, skeletons).
- Localizing user content (variable names, user tile names, brain/page names, display
  scroll text).
- Machine translation and translation-management integrations.
- Locale-aware number/date formatting beyond plural rules.
- Library-shipped translations -- a library's tile docs, its catalog name/description,
  and any library-authored labels. The library system's authoring surface owns that
  hook; this design only requires that catalog lookup composes (additional catalogs can
  join the lookup) and that the docs registry tolerates a per-locale library channel.

Ordinal formatting (`selectordinal`) is excluded with the rest of the ICU surface; no
current string renders ordinal prose, and diagnostics that reference positions name the
slot instead, which localizes better than ordinals. It joins additively if a real
surface ever needs it.
