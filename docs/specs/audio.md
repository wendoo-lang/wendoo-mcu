# Spec: audio functions

A device's speaker is a single shared sound output. This spec is the home for the **family of
functions that produce sound** - their shared arbitration model, the async-play pattern they
follow, and the `Sound` type they consume. It is the audio counterpart of the display draw
family (`display.md`): the same three-surface delivery, registered-Struct-literal type, leased
single output, and formula-based completion apply. The high-value capability is **authoring fun
sound effects** (a ramping saw wave with tremolo, sci-fi sweeps, and the like) in an editor and
playing them on the device; the device's built-in named sounds play through the same tile. The
family model and types are **target-agnostic**; the audio capability (speaker, waveforms,
effects, pitch range, built-in sounds) is **defined by the target**. The first - and currently
only - target is micro:bit-v2. The members are **play sound** (built-in and authored sounds) and
**playTone** (the `beep` tile - a plain constant-pitch tone).

This is an evolving family spec; the settled design is below, with unresolved items collected under
**Open questions** at the end. Musical note sequences, melodies, and chords are a deliberately
deferred direction (see Open questions) - not the near-term focus.

## Target parameterization

The cross-target parts of this spec - the family model, the lease arbitration, the async-play
pattern, and the `Sound` structure - do not assume a synthesis engine or a fixed sound set. The
target defines:

- **Audio capability.** Whether a speaker exists, the available waveforms and effects, the usable
  pitch range, and any built-in sound library.
- **Pitch realization.** How an authored frequency maps to a played tone is the target's.

The cross-target layer owns the sound-effect / sound-expression **concepts**; the target fills in
the synthesis. Only micro:bit-v2 is specified and built here; another target (for example an
Arcade device) slots in as a new target without changing the family model or the `Sound` type.

## The speaker as a shared output

- One speaker per device.
- Sound is transient: a play produces sound for its duration and then stops; there is no held
  state to overwrite (unlike the display's persistent image).
- Play functions are **temporal**: a sound takes observable time. Each is an async host-action
  (op 45 `HOST_ACTION_CALL_ASYNC` + `AWAIT`); the rule parks until the sound completes. There is
  no instantaneous audio stance.

## Surfaces

The audio family is delivered on the three standard surfaces:

- **Tiles.** The `play sound` actuator: an **anonymous sound argument** (a built-in sound
  literal tile, or an authored `Sound`) plus the shared **`immediately`** and
  **`in background`** modifiers - the display family's pair, interpreted the same way against
  the speaker lease (see Arbitration). A bare `play sound` plays the target's default built-in
  sound. A **`create a sound`** factory tile opens the sound-effect editor and produces a
  `Sound` value. The **`beep`** actuator (internally `playTone`) plays a plain constant-pitch
  tone: an optional anonymous frequency, optional named **`duration`** and **`volume`**
  parameters, and an optional wave-shape modifier (`square` / `sawtooth` / `sine` /
  `triangle`), through the same lease and modifier pair (see Member: playTone).
- **Device API.** `ctx.microbit.audio` (registry: `microbit-context.md`) has an awaited
  **`playSound(sound, options?)`** method taking a built-in sound name (a name outside the
  target's built-in set is a silent no-op that resolves at once) plus an optional
  **`PlaySoundOptions`** bag -- `{ immediately?, inBackground? }` -- selecting the same lease
  behaviors as the tile modifiers: `immediately` preempts the speaker lease at dispatch (the
  current sound stops and its awaiting rule resolves), and `inBackground` resolves the call at
  dispatch so the caller continues without awaiting playback. Both flags default false and the
  bag is optional, so a bare `playSound(sound)` is awaited and reject-by-default -- a play
  requested while the speaker is busy is dropped. An awaited **`playTone(frequency?, options?)`**
  method mirrors the `beep` tile: `frequency` in Hz (default 880, clamped into the target's
  usable pitch range; 0 plays a silent rest for the duration), and an optional
  **`PlayToneOptions`** bag --
  `{ duration?, volume?, waveform?, immediately?, inBackground? }` -- with `duration` in
  seconds (default 0.5), `volume` a 0-1 fraction of full tone volume (default 1), `waveform`
  one of `"square" | "sawtooth" | "sine" | "triangle"` (default `"triangle"`; a name outside
  that set is a silent no-op that resolves at once, as an unknown built-in sound name is),
  and the two lease flags behaving as in `PlaySoundOptions`. The `Sound` type is
  visible to TS user code.
- **Simulator - the sound-effect editor.** A **custom literal factory** bound to the `Sound`
  type - a simplified, friendlier sfxr: the controls are the `Sound` fields (waveform, base
  frequency, duration, volume, a frequency sweep, a vibrato, and a volume envelope / fade in-out),
  with live preview. It builds the `Sound` literal the `create a sound` tile manufactures.
  Editor-only: the editor UI is not on the parity path (the compiled `Sound` value is - see
  Conformance).

## Arbitration: the speaker lease

The speaker is a single output, leased by the playing sound for its duration - the same model as
the display.

- The lease is a **target-layer concept resolved against VM tick time** - deterministic and
  parity-matchable - **not** a device-driver busy/done state. Completion is a pure function of the
  sound's encoded durations, settled in the host-loop drain when VM tick time reaches it. The
  device-driver completion event (CODAL's `EVT_DONE` / mixer-silence) is never the completion
  signal - it is non-deterministic and does not reliably fire on hardware.
- The dispatching rule `AWAIT`s the lease and resumes on the first `think()` past completion;
  while parked it does not re-fire.
- **By default a `play sound` dispatched while a sound is playing is rejected**: the new sound is
  **dropped - not played, the actuator completes immediately, no error** (a missed feedback sound
  must not fault a reactive rule). There is no queue or mixing.
- **The `immediately` modifier overrides that**: the play **preempts the speaker lease** - the
  current sound stops, its awaiting rule's handle **resolves** (it continues, not an error), and
  the new sound takes the speaker for its duration. This is the display family's `immediately`,
  applied to the speaker lease. The preempt happens **at dispatch, before the new play is
  examined** (the display family's order): even when the new play then turns out to be a no-op
  (an unknown name), the current sound has still been preempted.
- **The `in background` modifier**: the sound keeps its speaker lease (settled on tick time as
  usual) but the actuator's handle resolves at dispatch, so the issuing rule continues this
  round without parking on the playback - the display family's `in background`.
- Determinism: rule execution order within a round is deterministic, so which sound holds the
  speaker - and whether a competitor is dropped or preempts - is a deterministic function of
  round order + the modifiers.

No mixing bus or voice pool is built: a single sound effect is monophonic, and concurrent
playback (the only polyphony case) is handled by the lease, not by summing voices.

## Async-play pattern

- Dispatched as op 45 `HOST_ACTION_CALL_ASYNC`, returning a handle the rule `AWAIT`s - the same
  machinery as `pause` and the display family.
- The sound is rendered by the target's synthesis engine (the device sequences the effect's
  segments itself; the sim renders via Web Audio). The VM does not synthesize - it hands the whole
  sound to the target and awaits the total.
- **Completion = `start + sum(segment durations)` vs VM tick time**, computed from the sound's
  encoded segments. The device-driver completion event is not used.

## Durations and looping

- Every sound segment carries an **explicit duration**, so a sound's total length is known before
  it starts and the completion tick is computable.
- **Looping is not supported.** A device driver treats a **negative duration** as "loop forever";
  we reject that. A sound segment with a negative duration is **silently dropped - not played, and
  not a fault** (no `ScriptError`). The drop happens in the **target audio port, in both VMs,
  before anything reaches the driver**, so the driver never receives a negative duration and never
  loops.
- The drop is **per-segment**: a sound keeps its finite segments and plays them; if filtering
  empties a sound entirely, the actuator resolves immediately (a no-op). **Zero duration is
  allowed** - a zero-length segment that contributes 0 to the total.

## Member: play sound

- An async actuator placed in `do`. Plays a sound - a built-in sound literal, an authored sound
  effect (from a `create a sound` factory tile), or a sound-typed variable - given as the
  **anonymous sound argument**. A bare `play sound` (no argument) plays the target's default
  built-in sound; the rule's captured WHEN result is not consulted. The **`immediately`** and
  **`in background`** modifiers select the lease behavior (see Arbitration).
- Behavior: when the speaker is free, the sound's encoded segments are handed to the target's
  synthesis engine, the rule awaits the total duration, and the speaker is leased for that time.
  When busy: default drops the new sound and completes immediately; `immediately` preempts the
  current sound (its rule resolves) and plays.
- Completion: `start + sum(segment durations)` vs VM tick time. Negative-duration segments are
  dropped per Durations and looping.

## Member: playTone (the beep tile)

An async actuator placed in `do` that plays a short plain tone at a constant pitch - the
classic feedback beep. The internal action name is **`playTone`**; the tile label is
**`beep`**. A tone is the **degenerate one-segment sound**: one waveform at a constant
frequency for a fixed duration, with no sweep, vibrato, or envelope. It rides the family
machinery end to end (the lease, the async-play pattern, the duration-formula completion);
no new synthesis or arbitration is added. TS user code reaches the same effect through
`ctx.microbit.audio.playTone` (see Surfaces).

### Identity

| Field         | Value |
| ------------- | ----- |
| Kind          | actuator |
| Stance        | async / awaited actuator (temporal) - the family stance; there is no instantaneous audio |
| Composability | rule action (placed in `do`) |
| Module        | microbit-v2 (`wendoo.microbit-v2`) |
| Internal name | `playTone` - the action key; the tile key derives from it |
| Label         | `beep` |
| Action / fn ids | Action `1041`, actuator fn `1081`, `MicroBitAudio.playTone` fn `1082` (with the `PlayToneOptions` type-atom id `1041`); append-only |

### Authoring

```
do: beep                        // default pitch, triangle, 0.5 s
do: beep 440                    // 440 Hz, triangle, 0.5 s
do: beep duration 2             // default pitch, triangle, 2 s
do: beep 1200 sine duration 0.1 // 1200 Hz sine, 0.1 s
do: beep 440 volume 0.2         // 440 Hz triangle, quiet, 0.5 s
```

### Arguments / modifiers

A `bag` of three optional params, an optional wave-shape modifier choice, and the family's
shared lease-modifier pair:

| Slot | Name        | mod/param | Type   | Required | Anonymous | Default |
| ---- | ----------- | --------- | ------ | -------- | --------- | ------- |
| 0    | (frequency) | param     | Number | no       | yes       | `880` Hz |
| 1    | duration    | param     | Number | no       | no        | `0.5` s |
| 2    | volume      | param     | Number | no       | no        | `1` |
| 3    | square      | mod       | -      | no       | -         | - |
| 4    | sawtooth    | mod       | -      | no       | -         | - |
| 5    | sine        | mod       | -      | no       | -         | - |
| 6    | triangle    | mod       | -      | no       | -         | the default shape |

- **frequency** - the tone pitch in Hz. **0 is a rest**: it plays silence for the duration,
  still awaiting and leasing the speaker, so a rule can sequence beeps and pauses with one
  tile. The target clamps the value into its usable pitch range (pitch realization is the
  target's, per Target parameterization; on micro:bit-v2 the range is 0-9999 Hz - see the
  target section).
- **duration** - the tone length in **seconds**, fractional allowed (the `pause` convention).
  Reuses the existing shared `microbit-v2.duration` parameter tile (the draw family's,
  seconds-typed), so its label, icon, and doc page are already shipped.
- **volume** - the tone's volume as a **0-1 fraction** of full (default 1), clamped into
  0-1; 0 is silent, and the silent tone still leases the speaker for its duration. It scales
  the tone only - the device master volume still applies on top. A new named parameter tile
  (the catalog ships no volume parameter to reuse). It must stay named, never anonymous: a
  second anonymous Number would be indistinguishable from the frequency.
- **Wave shape** - an `optional(choice(square, sawtooth, sine, triangle))` of plain modifiers:
  at most one appears; none means triangle (the cleaner-sounding default; an explicit
  `square` gives the classic harsh beep). Each selects the `Sound` waveform / target tone
  function for the segment. Square plays at the default duty (the duty control belongs to the
  `Sound` editor). Noise is not in the set - a pitched beep and noise are an odd fit; authored
  `Sound`s cover it.
- Defaults live in the body, not the grammar: an absent/nil slot reads as the default above,
  and a non-finite frequency or duration also reads as its default.
- **Lease modifiers:** the shared **`immediately`** and **`in background`** pair, interpreted
  against the speaker lease exactly as for `play sound` (see Arbitration). `in background` is
  the expected shape for feedback beeps in reactive rules - the rule continues instead of
  parking for the beep's duration.

### Behavior

- Dispatched under the speaker lease (see Arbitration): default drops the beep when the speaker
  is busy (completes immediately, no error), `immediately` preempts the current sound,
  `in background` resolves the handle at dispatch while the tone holds the lease.
- When accepted, the tone is handed to the target's synthesis engine as a single
  constant-pitch segment of the selected waveform; the rule awaits the duration and the
  speaker is leased for that time.
- Completion: `start + round(duration * 1000)` ms vs VM tick time - the family formula with one
  segment.
- Edge cases follow Durations and looping: a negative duration is the dropped-segment case (the
  beep is silently dropped and the actuator resolves immediately, no fault); a zero duration is
  allowed (a zero-length tone that resolves immediately).

### Device and trace

- **Device port:** the same speaker port as `play sound`; the play command carries the
  waveform, the clamped frequency, the duration, and the clamped volume. The tile and
  `ctx.microbit.audio.playTone` issue the same command. The waveform crosses the port as an
  enumerated value, never as text: the tile's modifier slot and the Device API's waveform
  string each resolve to it once at dispatch, so waveform names appear on device only as
  the small fixed lookup table the Device API parse reads.
- **Observable trace:** `port speaker tone <waveform> <frequencyHz> <durationMs> <volume>`
  plus the async action dispatch line. The field names here are shorthand; the exact wire
  encodings (f32 bit patterns for frequency and volume, minimal hex for durationMs) are
  pinned in the observable-trace contract, which is authoritative for the C++ mirror. A dropped tone (busy speaker, or negative duration) emits no
  port line - the dispatch line records the attempt, mirroring `play sound`.

### Catalog metadata (assistant, docs, art)

- **`grammarNote`** - the assistant-facing rule sentence the LLM catalog digest reads. It must
  state what the argument grammar does not: the units and defaults of both params, that the
  wave shapes default to triangle, and the lease behavior (in `play sound`'s wording). Required
  text:

  > Plays a plain tone: the bare number is the pitch in Hz (default 880; 0 plays silence
  > for the duration, a rest), "duration" is in seconds (default 0.5), "volume" is 0 to 1
  > (default 1), and one wave-shape word (square / sawtooth / sine / triangle) picks the
  > sound, triangle when none is given. The rule holds until the tone ends: until
  > then a rule under it does not get its turn, and this rule cannot fire again. A tone asked
  > for while a sound is playing is dropped; add "in background" to let the rule carry on, or
  > "immediately" to cut off the sound that is playing.

- **Doc pages** - the tile needs a proper author-facing doc page, delivered through the
  target tile-docs pipeline: a markdown file under the wodal package's
  `targets/microbit-v2/docs/en/tiles/` directory (the `play sound` page's format: the
  brain-frame header block, a title + summary line, then a body with `tile:` cross-references)
  plus its `MICROBIT_V2_TILE_DOCS` manifest entry - the rehearsal pairing check fails when
  either half is missing. Content: what a beep is, the params with units and defaults, the
  wave-shape modifiers, the lease modifiers, and a couple of worked examples including a
  beep-and-rest sequence. Each net-new tile - the four wave-shape modifiers and the `volume`
  parameter - gets its own page the same way. Required at
  implementation; the doc pages ship with the tile, not after it.
- **Tile art** - icons are the app's, not wodal metadata: `apps/microbit-sim` maps tile id ->
  icon URL in its tile-visuals table (under `assets/brain/icons/`). Required at
  implementation: an icon for the `beep` action tile, one waveform-glyph icon per
  wave-shape modifier (square / sawtooth / sine / triangle wave outlines), and an icon for
  the new `volume` parameter. The `duration` parameter and the shared lease modifier pair
  reuse already-shipped tiles, icons, and doc pages.

## Built-in sounds as literals

A target's built-in named sounds surface as **literal tiles**, one per sound, mirroring the
built-in image set:

- Each tile carries a baked value of a registered **`SoundEmoji`** struct holding the sound's
  **name** - the durable discriminator (literal tile ids derive from it; the set is
  append-only). The value is a brain-program constant, like a built-in `Image` literal.
- The nominal `SoundEmoji` type keeps the picker and typecheck exact: the `play sound` sound
  slot admits sound values, not arbitrary strings.
- The **target resolves the name** to its encoded sound data at the audio port. A name outside
  the target's built-in set is a **silent no-op**: nothing plays and the actuator resolves at
  once (with `immediately`, the dispatch preempt still occurs first - see Arbitration).
- Completion uses the same duration formula: the target pins a **nominal total duration** per
  built-in - the sum of its segments' encoded durations with any randomized contribution read
  as zero. The speaker lease runs to `start + nominal total`. A device synth may randomize the
  actual playback length around the nominal; that deviation is driver-side and not on the
  parity path (the port stops the synth when it accepts a new play, so a still-ringing tail
  never blocks the next sound).
- The simulator's renderer reads the same encoded data with randomized contributions at zero,
  so the rendered sound is deterministic and matches the nominal duration.

## The `Sound` type

- A `Sound` is a **registered Struct type** - not a new VM primitive. It reuses the existing
  struct and literal machinery (both VMs already implement structs), so the runtime needs nothing
  new. This mirrors the `Image` / `Vector2` pattern: a registered struct plus a custom literal
  editor (a `CustomLiteralType` whose `renderInputFields` is the editor), registered against the
  core factory API (`BrainTileFactoryDef` / `api.defineType` / `registerLiteralFactoryTileDef`).
- It holds an authored **sound effect**, a struct that mirrors CODAL's `SoundEffect` parameter
  set but exposed as friendly, simplified-sfxr controls (not raw function pointers, and not
  CODAL's opaque 72-char sound-expression string):
  - **base** frequency (Hz), volume, and duration (ms);
  - a **waveform** enum - sine / sawtooth / triangle / square (with a duty parameter) / noise;
  - up to **three effect slots** as named controls - a **frequency ramp** (an end frequency the
    pitch ramps to over the duration, the base frequency being the start; up if higher, down if
    lower, steady if equal; optional ramp shape), a **vibrato** (depth + rate), and a **volume
    envelope** (a fade-in / fade-out, i.e. attack + decay). Three named controls map exactly onto
    CODAL's three `SoundEffect` effect slots.
  - A `Sound` is one such segment, or a `List` of them for a multi-stage sound (e.g. a zap then a
    tail). Segment encoding (a `List` of segment structs vs a packed form) is pinned at
    implementation; see Open questions.
- The struct mirrors the parameters, not CODAL's representation: the **target port encodes a
  `Sound` to a CODAL `SoundEffect[]`** on device (waveform enum -> tone function, each control ->
  the matching effect function), and renders it via Web Audio in the sim. The cross-target layer
  writes only that encoder + the sim renderer - no custom DSP (the target's synth generates the
  sound; see micro:bit-v2).
- **Instances are literals.** A `Sound` value is created by the `create a sound` factory tile
  (the Tiles), whose editor (the Simulator) builds the struct; the literal is baked into the brain
  program as a constant and passed to `play sound`.
- The effect parameters' realization (frequency -> tone, the waveform and effect set) is
  **target-interpreted**. The struct *shape* is target-agnostic, but the *type registration* is
  **target-side**, because the synthesis capability is target-owned.

## Sound-effect editor (the Simulator)

The custom literal factory bound to `Sound` - a **simplified, friendlier jsfxr**. In microbit-sim
it is deliberately **basic** (labeled sliders + a few buttons, just to express the functionality;
a more polished editor can come later). Each control maps directly to a `Sound` field, which maps
to a CODAL `SoundEffect` slot:

- **Waveform** - sine / sawtooth / triangle / square / noise (a row of buttons); a **duty** slider
  appears when square is selected.
- **Frequency** - the start pitch (Hz).
- **End frequency (ramp)** - the pitch the frequency ramps to over the duration: above the start
  ramps up, below ramps down, equal stays steady; with an optional ramp shape (linear default;
  logarithmic / curve / exponential). This is the frequency-interpolation slot.
- **Duration** - total length (ms).
- **Volume** - overall level.
- **Vibrato** - depth + speed.
- **Envelope** - fade-in (attack) + fade-out (decay) (the volume slot).
- **Preview** - a play button (Web Audio) to hear the sound while editing, rendered by the same
  faithful synthesis port that plays built-in sounds (see the target section) so the preview
  tracks the device.
- **Presets / randomize** (recommended - the "fun" of sfxr; can be phased): a few one-click
  generators (laser, power-up, explosion, pickup, jump, blip) plus a randomize / mutate button,
  each just filling the controls above. Optional; the manual controls are the baseline.

Excluded (absent from CODAL `SoundEffect`, and out of scope for "simpler"): low/high-pass
filters, phaser / flanger, bit-crush.

## micro:bit-v2 target

The concrete fill-in of the target-parameterized pieces for micro:bit-v2:

- **Speaker:** the on-board MEMS speaker via `uBit.audio` (`MicroBitAudio`); master volume 0-255.
- **Synthesis (use CODAL's, build no custom generator):** `SoundEmojiSynthesizer` already
  generates the oscillator, applies the effects, and sequences segments in real time. The target
  port only **encodes a `Sound` to a CODAL `SoundEffect[]`** and plays it; there is no custom DSP.
  The mapping:
  - waveform -> `Synthesizer` tone function (`SineTone` / `SawtoothTone` / `TriangleTone` /
    `SquareWaveTone` or `SquareWaveToneExt` with duty / `NoiseTone`);
  - frequency ramp -> a frequency-interpolation effect (`linearInterpolation` default;
    `logarithmicInterpolation` / `curveInterpolation` / `exponentialRising`/`FallingInterpolation`):
    the base `SoundEffect.frequency` is the start, the effect's `parameter[0]` is the end
    frequency, interpolated over the steps (up or down);
  - vibrato -> `frequencyVibratoEffect` / `vibratoInterpolation` (and `warbleInterpolation` /
    `volumeVibratoEffect` tremolo are available);
  - volume envelope / fades -> `volumeRampEffect` (or `adsrVolumeEffect`).
  A `SoundEffect` has **3 effect slots** (`EMOJI_SYNTHESIZER_TONE_EFFECTS = 3`), 2 params each -
  enough for the sweep + vibrato + envelope set above; richer sounds chain `SoundEffect`
  segments. A single voice is used; `Mixer2` exists but no polyphony is built. (Filters / phaser /
  bit-crush are not in `SoundEffect` - deliberately out of the simplified editor.)
- **Built-in sounds:** the 10 CODAL sound emoji - `giggle`, `happy`, `hello`, `mysterious`, `sad`,
  `slide`, `soaring`, `spring`, `twinkle`, `yawn` (each a name mapped to an encoded
  sound-expression data string in codal-microbit-v2's `SoundExpressions`; micro:bit-specific).
  The default built-in (a bare `play sound`) is **`hello`**. On device the port plays an
  accepted name through `SoundExpressions` (`playAsync`), stopping the synthesizer first; a
  name outside the set never reaches CODAL (the port no-ops it). Each built-in's **nominal
  total duration** is pinned in both VMs from its encoded segment durations (the encoding
  carries per-segment randomization fields; they read as zero for the nominal). The micro:bit
  standard library (`lib-microbit-v2`) exports the built-in names as a **`sounds` const
  object** (`sounds.giggle` ... `sounds.yawn`, string-literal-typed), the discoverable,
  typo-safe way for TS user code to name a built-in in `playSound`.
- **Completion:** resolved on the duration formula; CODAL's message-bus completion events are
  ignored.
- **`Sound` type:** the registered struct authored by the `create a sound` factory tile + the
  sound-effect editor (the Simulator); type-atom id appended at implementation (append-only).
- **`SoundEmoji` type:** the registered struct a built-in sound literal carries (`{ name }`);
  target-side, since the built-in set is target-owned. Type-atom id appended at implementation.
- **playTone (beep):** encodes to a single CODAL `SoundEffect` - the selected waveform's tone
  function (`TriangleTone` default / `SquareWaveTone` / `SawtoothTone` / `SineTone`), constant
  frequency (no interpolation effect), flat volume - the 0-1 `volume` fraction passes straight
  through as the `SoundEffect` volume, itself a 0-1 float the synthesizer scales across its
  0-1023 sample range - duration from the call - played through the same synthesizer path as
  an authored `Sound`; the sim renders it via the faithful synthesis port. Tile action id `1041` (actuator fn `1081`); the `MicroBitAudio.playTone` host-function
  id is `1082` and the `PlayToneOptions` type-atom id is `1041`. The pitch clamp is
  **0-9999 Hz**: the range of CODAL's
  sound-expression frequency encoding (4 decimal digits; the expression parser rejects a
  negative frequency, so negatives clamp to 0 before anything reaches the driver),
  comfortably below the synthesizer's 22050 Hz Nyquist bound at its 44100 Hz sample rate.
  A 0 Hz tone is a rest: the port encodes it at volume 0 rather than letting the
  synthesizer emit its 0 Hz output (a constant full-level sample - a DC segment that pops
  and loads the speaker). Both VMs clamp and encode identically at the audio port, so the
  traced value is the clamped one. The micro:bit standard library (`lib-microbit-v2`)
  exports the wave-shape names as a **`waveforms` const object** (`waveforms.square`,
  `waveforms.sawtooth`, `waveforms.sine`, `waveforms.triangle`, string-literal-typed), the
  discoverable, typo-safe way for TS user code to name a wave shape in `playTone`.
- **play sound:** action / function ids assigned at implementation (append-only). On device the
  sound expression is sequenced by the synthesizer; **in the sim it is rendered by a faithful
  port of the same synthesis** - the device's tone functions (the phase-indexed waveform tables)
  and its stepped effect functions (frequency interpolation, volume ramp, vibrato/tremolo/warble)
  run in software to produce the sound's PCM, played through the Web Audio API as a pre-rendered
  buffer. The simulator does not approximate the synthesis with generic Web Audio oscillator and
  automation nodes; it reproduces the device algorithm so the simulated sound tracks the device
  closely.

## Conformance

- The wodal microbit module is the oracle; the C++ port mirrors it. Audio output is **not**
  byte-trace-matchable (it is an analog waveform), so parity is over the **play command** issued
  to the audio port - the sound's encoded segments (or built-in name) + durations - not the sound.
  Trace: a speaker-port line for the play (`port speaker play "<name>"` for a built-in,
  `port speaker tone <waveform> <frequencyHz> <durationMs> <volume>` for a tone; the
  authored-sound form is pinned with the `Sound` type) plus the async `action ... async`
  dispatch line. A play the busy speaker drops - or an unknown name - emits no port line (the
  dispatch line records the attempt), mirroring the display draw. Completion resolves on VM
  tick time (the duration formula), so the trace is deterministic and reproducible across both
  VMs.
- The compiled `Sound` value is a brain-program constant and is on the parity path (both VMs derive
  the same play command from it). The Simulator editor UI that authors it, and the rendered sound,
  are sim-only and not byte-matched.
- The lease outcome (which `play sound` holds the speaker, which a busy speaker rejects) is part of
  the trace and is deterministic via round order + the per-sound formula.
- The device-driver completion event is not on the parity path.

## Open questions

1. **`Sound` struct details** (shape resolved: mirrors CODAL's `SoundEffect` - base freq/vol/
   duration + waveform enum + sweep / vibrato / envelope controls, segments as a `List`). Left to
   pin: parameter ranges + units, the segment encoding (`List` of structs vs packed), and which
   CODAL variants the named controls expose (e.g. sweep shape options; whether tremolo / warble
   are surfaced as separate controls or folded into vibrato).
2. **`create a sound` from a template**: whether the factory can start from a built-in sound as
   a template (the built-in argument shape itself is settled: built-ins are `SoundEmoji`
   literal tiles - see Built-in sounds as literals).
3. **Sim fidelity (resolved: faithful port).** The simulator reproduces the device synthesis by
   porting its tone + effect functions and rendering PCM, played as a pre-rendered Web Audio
   buffer - not a generic-oscillator approximation. The rendered sound is still sim-only and not
   byte-matched (the traced command remains the parity anchor); "faithful" means the same
   algorithm, not sample-identical output (sample-rate resampling and float precision differ).
   Open only: whether to add a device-PCM cross-check test (a C++ dump of the real synthesizer
   compared to the port within tolerance) as a later hardening step.
4. **Musical notes / melodies / chords (deferred direction).** Pitched note sequences and chords
   were considered and deprioritized below sound effects. Reviving them would add a `Melody`-style
   type and, for chords, a mixing arbitration (a voice cap + over-cap policy) that replaces the
   single-output lease. Out of scope until prioritized.
5. **How far to formalize the target seam now**: the audio capability seam is settled, but the
   minimal interface a target exposes (waveform/effect set, built-in sounds, the encode-to-driver
   hook) is specified only as far as micro:bit needs until a second audio target is scoped.
