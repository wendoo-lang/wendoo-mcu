# Wendoo MCU

Bring [Wendoo](https://github.com/wendoo-lang/wendoo-lang) brains to the
[BBC micro:bit](https://microbit.org/).

Wendoo programs are built by arranging **tiles** -- typed, composable tokens
-- into **rules**. Each rule has a WHEN side (condition) and a DO side (action).
A collection of rules forms a **brain** that drives an autonomous actor. This
repository takes those brains off the screen and onto hardware: it hosts a
native C++ implementation of the Wendoo bytecode VM that runs on the
micro:bit v2, a web-based device runtime and simulator for authoring and testing
brains in the browser, and the micro:bit platform surface (display, buttons,
accelerometer, audio, radio, GPIO, I2C, light and temperature sensors, and robot
chassis drivers) exposed to brains as sensors and actuators.

The Wendoo language, compiler, and reference VM live in the upstream
[wendoo-lang](https://github.com/wendoo-lang/wendoo-lang) repository, included
here as a git submodule under `external/`. The TypeScript reference VM is the
executable specification; the native VM and the web runtime both mirror its
observable bytecode semantics byte-for-byte.

## Demo

- [Code a BBC micro:bit](https://microbit.playwendoo.com) -- build and test
  brains in the browser, then flash the same brain to a physical micro:bit over WebUSB.

## Two VMs, one contract

Every conforming Wendoo VM decodes the same bytecode to the same semantics.
This repository adds two implementations to the reference one:

| Runtime | Where | Role |
|---------|-------|------|
| TypeScript reference VM | [external/wendoo-lang](external/wendoo-lang) (submodule) | Executable spec; source of golden traces. |
| Native C++ VM | [cpp/](cpp/) | On-hardware runtime; compiles to `MICROBIT.hex` for the micro:bit v2. |
| WODAL web runtime | [packages/wodal](packages/wodal/) | CODAL-inspired device model that runs brains in the browser. |

The compiler is target-unaware: it emits identical bytecode regardless of the
eventual runtime. Host and platform differences are expressed through registered
host functions, actuators, and device adapters -- never through opcode subsets.

## Packages

| Package | Description |
|---------|-------------|
| [@wendoo/wodal](packages/wodal/) | WODAL -- a CODAL-inspired web device runtime for the Wendoo bytecode VM. Models micro:bit peripherals, event/fiber mechanics, and radio through host-loop drains. Ships a `wodal` CLI. |

## Apps

| App | Description |
|-----|-------------|
| [micro:bit Editor](apps/microbit-sim/) | Author, run, and debug Wendoo brains against a simulated micro:bit in the browser. Flash brains to a physical micro:bit over WebUSB, drive synthetic device inputs (gestures, light level, temperature), and inspect per-brain diagnostics. |

## Native tree

| Directory | Description |
|-----------|-------------|
| [cpp/](cpp/) | Native C++ Wendoo VM. `core/` is the host-agnostic runtime, `codal/` the board-agnostic CODAL layer, and `targets/microbit-v2/` the micro:bit v2 firmware that boots a brain from a reserved on-flash region. See the [cpp README](cpp/README.md) for the layout and device build. |

## Getting Started

This repository is not an npm workspaces monorepo. Each package and app installs
into its own `node_modules`; cross-package links use `file:` dependencies. The
Wendoo language lives in a submodule.

```bash
git clone https://github.com/wendoo-lang/wendoo-mcu.git
cd wendoo-mcu

# Install every package and app, and build the packages the apps resolve as
# dist, in dependency order. The root `preinstall` hook initializes the
# submodule first, before anything resolves a dependency inside it, so a bare
# `npm install` does the install half on its own.
npm run bootstrap
```

Run the micro:bit simulator:

```bash
npm --prefix apps/microbit-sim run dev
```

Building the micro:bit v2 firmware uses its own CODAL/Docker flow rather than the
host CMake tree; see [cpp/README.md](cpp/README.md) for the device build, and
its "Build, test, and check" section for building and running the native VM
tests on your workstation.

## Documentation

Feature specifications live under [docs/specs/](docs/specs/) -- one normative
document per capability (display, audio, radio, accelerometer, GPIO, I2C,
movement, and more). These specs are the contract; the reference VM, native VM,
and WODAL are all held to them.

## Contributing

To report a bug or request a feature, please
[open an issue](https://github.com/wendoo-lang/wendoo-mcu/issues).

## License

[MIT](LICENSE)
