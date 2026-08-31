# Spec: micro:bit Context surface (TS user-code device API) - registry index

The cross-cutting registry / index of `ctx.microbit.*` - the lower-level
device API exposed to TypeScript user code (**Device API** of the model). Each peripheral's **full
design across every surface it is exposed through** lives in its own **feature spec**
(`docs/specs/<feature>.md`); this file holds only the cross-cutting conventions and the index of
which feature spec owns each `ctx.microbit.*` sub-interface.

## What this is (and isn't)

- A host-function API bound to CODAL `uBit` (in `cpp/targets/microbit-v2/source/main.cpp`),
  shaped like the device `*Port` layer (`cpp/codal/device-port.h`). It is **not 1:1 with the
  tiles** - it tracks the device/port shape, not tile semantics. Example: `buttonA.isPressed()`
  is the raw pressed level; the click/hold/double-click derivation lives only in the button
  *sensor tile*, not here.
- The TS-author-facing type surface is the ambient
  `packages/wodal/ambient/wendoo.microbit-v2.d.ts` (the `Context.microbit: MicroBit`
  interface). **The feature specs are the design intent; that `.d.ts` is its maintained mirror** -
  keep them in lockstep.
- **Reads share the underlying poll with the tiles:** one poll per input, consumed by both
  the tile derivation and the host-function. Do not duplicate the read.

## Conventions (per peripheral)

- `ctx.microbit.<peripheral>` is a native-struct getter (the `Struct(typeId,
  discriminator)` rep + `native-struct-bindings.h`); its methods are host-functions over a
  `DevicePort`, bound to `uBit` on device and to the wodal sim model.
- Stance (same as the tiles): instantaneous reads/writes = **sync** host-functions; temporal
  effects = **awaited**. A Device-API awaited host-function dispatches as op 41 `HOST_CALL_ASYNC`
  (e.g. `display.drawImage`); the tile / brain-action form of the same effect dispatches
  as op 45 `HOST_ACTION_CALL_ASYNC`. Both return an awaited handle and share one display lease.
- ABI ids are append-only. Each member is implemented + tested on **both VMs**
  and declared in the ambient `.d.ts`.
- **Invariant (native-struct field order):** the compiler keys `STRUCT_GET_FIELD` by a field's
  *position* in the registered fields list, so the wodal `MicroBit` field order and the C++
  `MicroBitField` enum values must stay equal (position == id). Append a new `ctx.microbit` field
  **last**, at the next free id.

## Surface registry (index)

Each `ctx.microbit.*` sub-interface and the feature spec that owns its full design (all surfaces):

| `ctx.microbit.*` | Summary | Feature spec |
| ---------------- | ------- | ------------ |
| `display` | per-pixel `setPixelValue`/`getPixelValue`/`clear` + temporal `drawImage`/`scrollText` (each takes an optional per-method options bag -- `DrawImageOptions {duration?, immediately?, inBackground?}` / `ScrollTextOptions {immediately?, inBackground?}`); the draw family (display text, draw image, `Image` type, image editors) | `docs/specs/display.md` |
| `buttonA` / `buttonB` / `logo` | `isPressed()` + the logo touch config | `docs/specs/button.md` |
| `accelerometer` | `getX/Y/Z`, `getPitch/Roll(+Radians)`, `getGesture()` reads | `docs/specs/accelerometer.md` |
| `i2c` | `writeBuffer` / `readBuffer` (edge-connector, no tile) | `docs/specs/i2c.md` |
| `gpio` | digital/pull/servo/analog-read (+ designed: analog write/PWM, touch) (edge-connector, no tile; fns 1052-1055 + 1071) | `docs/specs/gpio.md` |
| `sonar` | `distance(trig, echo)` ultrasonic, pin-keyed (edge-connector, no tile; via the background sensor driver) | `docs/specs/sonar.md` |
| `radio` | builtin 2.4 GHz packet radio: send (number/string/value/buffer/raw) + buffered receive (depth-4 ring, typed number/string/buffer tiles) + group/power/band config (has tiles; field 8, atoms 1032-1034, fns 1057-1070 + 1072, actions 1032-1036) | `docs/specs/radio.md` |
| `audio` | `playSound(name, options?)` (`PlaySoundOptions {immediately?, inBackground?}`): play a built-in sound over the speaker lease; awaited + reject-by-default (busy = silent drop, unknown name = no-op), `immediately` preempts the lease, `inBackground` resolves at dispatch; `playTone(frequency?, options?)` (`PlayToneOptions {duration?, volume?, waveform?, immediately?, inBackground?}`): play a plain constant-pitch tone (the `beep` tile) over the same lease (has tiles; field 9, atoms 1035 `SoundEmoji` + 1036, fns 1074-1075, action 1037; `playTone` action 1041, actuator fn 1081, `MicroBitAudio.playTone` fn 1082, atom 1041 `PlayToneOptions`) | `docs/specs/audio.md` |
| `arcadeShield` | attachable Arcade display shield: presence + 7 buttons + palette-indexed framebuffer draw commands (clear/pixel/line/rect/circle/text/image), all sync; presence-gated sensors, silent no-op actuators while absent; a second display coexisting with the matrix, no lease (has tiles; ids TBD) | `docs/specs/arcade-shield.md` |

(Build status, dates, and as-built history live in the build plans, not the specs - specs are
eternal.)

## Roadmap (append as peripherals land)

Each peripheral adds a feature spec + a registry row here when it lands; the source of capability is
the device's CODAL surface:

- onboard: `accelerometer`, `thermometer` (getTemperature), `compass` (heading), display light level,
  microphone sound level. The speaker (`audio` - play built-in sounds + authored sound effects)
  has a dedicated spec (`docs/specs/audio.md`). `radio` (send/receive) has a dedicated spec (`docs/specs/radio.md`): a
  builtin bidirectional service with tiles, the receive side modeled on the buttons poll-derived
  injected-input pattern, and a simulator multi-instance virtual ether over the `SharedMedium` broker.
- **edge-connector primitives** (Device-API ONLY - no tile counterpart), each with a **dedicated
  spec**: **I2C (`docs/specs/i2c.md`)**, **GPIO (`docs/specs/gpio.md`)**, the native NEC IR-receive
  primitive (spec TBD), **NeoPixel/WS2812 (`docs/specs/neopixel.md`, designed; not built)**, and
  **serial/UART (`docs/specs/serial.md`, designed; built when a serial-MCU robot is a target)**. These
  are the chassis-agnostic library plumbing a per-chassis robot library consumes; they have no tile.
  Robot chassis fall into **two classes** (driver surveys), and the primitive set serves both:
  - **Peripheral-direct** (the micro:bit drives the chassis hardware itself): Cutebot, DFRobot Maqueen /
    Maqueen Plus. They distribute the **same** capabilities differently - Cutebot is GPIO-heavy (line
    sensors, gripper, ultrasonic on pins; I2C `0x10` only for motors/lamps), while Maqueen Plus routes
    nearly everything (motors, servos, RGB, line sensors, odometry, PID) through one I2C co-processor at
    `0x10`. So **`i2c` read + write is the workhorse**, with GPIO + the background sonar/IR for pin-level
    sensors, and NeoPixel for chassis using addressable LEDs (rather than firmware RGB over I2C).
  - **Serial-MCU** (the chassis has its own MCU; the micro:bit is just the UART host, the robot API is a
    packet protocol): the ELECFREAKS XGO-Rider class. Needs only the **serial** transport; the protocol
    is a pure-TS library. Note its **read is awaited** (a response is a round-trip wait) - the first
    awaited Device-API read.
- **background sensor driver** (`docs/specs/background-sensor-driver.md`): the device-runtime
  mechanism (a device-owned background CODAL fiber + cached, sync-read values) backing sensors that
  cannot be measured synchronously - the GPIO **ultrasonic** and the **NEC IR-receive** decoder. Not a
  `ctx.microbit.*` peripheral itself; it backs the ones that are.

## Conformance

- The wodal microbit module is the oracle; the C++ microbit-v2 target mirrors it; user-tile
  goldens exercise the host-functions (e.g. `user-tile-button-display`), byte-matched across
  both VMs. The ambient `.d.ts` typechecks against the declared surface.
