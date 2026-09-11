# cpp

Native C++ tree for the Wendoo on-hardware VM. The TypeScript reference VM
(`external/wendoo-lang/packages/core/src/runtime/`) is the executable spec;
everything here mirrors its observable semantics.

## Layout

- `core/` - the host-agnostic runtime library. No CODAL, board, engine, or
  desktop dependency; every other part of this tree depends on it and it
  depends on none of them.
  - `core/platform/` - byte-stream and common primitives.
  - `core/runtime/` - value model, error/status types, ABI id mirrors.
- `codal/` - the CODAL-common, board-agnostic layer (the `wendoo-codal`
  library): the device-port interfaces (`codal/device-port.h`), the host-loop
  driver (`codal/host-loop.h` - sources time through the clock port and drives
  one `BrainRuntime` think per tick, single-entry), and the device fault-mode
  policy (`codal/fault-mode.h` - stop ticking, then loop the fault face plus a
  scrolled diagnostic code). Depends on `core/` and CODAL only; board specifics
  are forbidden (enforced by `check-deps.sh`).
- `targets/microbit-v2/` - the micro:bit v2 device firmware: a pinned copy of
  the official `microbit-v2-samples` CODAL build scaffold (see its
  `VENDORING.md`) whose `source/` plus `cpp/core/` and `cpp/codal/` compile into
  `MICROBIT.hex`. Built by its own `build.py`/Docker flow, not by the host
  CMake tree (see "Device build" below). `source/main.cpp` reads the brain
  image from a reserved on-flash region (`mcprogram-region.ld` pins the region;
  `source/program-region.h` exposes its boot-readable symbol), validates the
  on-flash header (`cpp/codal/on-flash-region.h`), decodes it in place, and
  runs `cpp/codal/`'s host loop on the board; `source/microbit-ports.h` binds
  the `cpp/codal/` device ports to CODAL peripherals (display, buttons, system
  timer, the fault-mode LED rendering). The `abi/` holds the mirrored ABI id
  enums plus the host-action bodies bound to those ports. The build emits the
  region's offset/size (plus the on-flash magic and format version) as
  `MICROBIT.metadata.json` (`tools/emit-metadata.py`) for the patcher.
- `hostkit/` - host-only tooling (the `wendoo-hostkit` library) that renders
  a decoded program (`program-dump.{h,cpp}`) or a VM run (`observable-trace.{h,cpp}`)
  as canonical text for the parity tests, sharing the `text-render.h` writer. The
  device firmware never reads these emitters, so they live outside `core/` (whose
  tree the firmware build globs whole) and are compiled only by the host CMake and
  the test suite. Depends on `core/`.
- `test/` - the desktop host test harness (doctest, vendored under
  `test/vendor/`). Depends on `core/` and `hostkit/`.
- `tools/` - host-side tooling (see `tools/README.md`).

## Prerequisites (macOS)

- CMake >= 3.21 and Ninja (`brew install cmake ninja`)
- A host C++ compiler (Apple clang from the Xcode command line tools)
- clang-format (bundled with Xcode; found via `xcrun --find clang-format`)

## Build, test, and check

The single check entry point (build + tests + sanitizers + format check +
dependency guardrails):

```
./check.sh
```

Individual presets (`debug` is the default developer build, `-g -O0`;
`sanitize` runs the tests under ASan + UBSan):

```
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Build output lands in `cpp/build/<preset>/` (gitignored). Each configure
exports `compile_commands.json` into its build directory for clangd and
IntelliSense.

## Golden fixtures and the regen loop

The C++ reader tests decode the committed binary `.mcprogram.bin` fixtures
and byte-compare their canonical program dumps against the committed
`.mcprogram.dump` goldens. The behavioral parity test
(`test/trace-parity.test.cpp`) additionally runs the `button-display` binary
under the mirrored input schedule - driven through the real `codal/` host loop
(`HostLoop`) over the device-port stub, sourcing time through the clock port -
and byte-compares its observable trace against the committed
`button-display.press-cycles.trace` golden (format record: wodal
`targets/microbit-v2/wendoo/observable-trace.ts`; generator: the sibling
spec). The two fixture roots are wired into the test tree by
CMake (see `test/fixture-paths.h`):

- core reference vectors: `external/wendoo-lang/packages/core/src/runtime/
  __fixtures__/` (`struct-field-access`, `control-flow`,
  `values-and-collections`)
- wodal microbit-v2 goldens: `packages/wodal/src/targets/microbit-v2/
  wendoo/__fixtures__/` (`button-display`, `user-tile-button-display`)

A third root holds the shared cross-VM conformance corpus:

- shared corpus: `external/wendoo-lang/packages/conformance/corpus/`
  (per-case `.program.json`, `.program.bin`, and `.trace`, plus
  `manifest.json` and `behaviors.json`)

The corpus is minted upstream by the TypeScript reference VM, which is its
only writer, and is never regenerated from this repo. A C++ case that fails
its byte-compare is a C++ defect: fix the VM, do not re-mint or hand-edit the
golden. `test/conformance-corpus.test.cpp` replays every case whose precision
matches the build's profile.

The goldens (JSON, binary, and dump) are generated by the TypeScript fixture
specs on a write-if-missing basis, and each spec asserts its committed
goldens are byte-stable against a fresh deterministic build, so a fixture
and its dump regenerate together or the owning spec fails. The dump format
is pinned in
`external/wendoo-lang/packages/core/src/runtime/brain-program-dump.ts`.

To regenerate the goldens (delete the stale files first; the specs rewrite
any missing golden) and re-verify the C++ suite against them, run from the
repo root:

```
(cd external/wendoo-lang/packages/core && npm test)
(cd packages/wodal && npm test)
cpp/check.sh
```

## Device build (micro:bit v2)

The device firmware builds from the vendored CODAL scaffold in
`targets/microbit-v2/` (pins recorded in its `VENDORING.md`). Both paths
produce `MICROBIT.hex` with `cpp/core/` and `cpp/codal/` compiled in, plus
`MICROBIT.metadata.json` (the build->patcher contract: the on-flash region
offset/size, magic, and format version). Flash by copying the hex
onto the `MICROBIT` USB drive. The firmware reads the brain from the reserved
on-flash region; once the patcher has written a program there, press button A
and the addressed display pixel toggles. An erased/invalid region, a
decode/load failure, or a startup fault drops into the device fault mode (sad
face, then the diagnostic code scrolled - `R` region, `L` load, `E` runtime),
with the full code printed once over serial.

The reserved region is empty in the prebuilt firmware (the firmware does not
embed a program); a brain is patched into it later by the browser/CLI patcher
using the emitted metadata.

Reproducible Docker path (pinned gcc-arm-none-eabi 10.3-2021.10; run from
`cpp/`). Each target exports into its own directory under `out/`:

```
docker build -f targets/microbit-v2/Dockerfile --output out/microbit-v2 .
```

`MICROBIT.hex` and `MICROBIT.metadata.json` land in `cpp/out/microbit-v2/`.

Local path: put a GNU Arm Embedded toolchain on PATH (for example ARM's
darwin-arm64 `.tar.xz` release extracted anywhere, or
`brew install --cask gcc-arm-embedded`, which needs admin rights), then:

```
cd targets/microbit-v2
python3 build.py
python3 tools/emit-metadata.py
```

The first configure clones the pinned CODAL target and libraries into
`targets/microbit-v2/libraries/` (gitignored). The hex lands in
`targets/microbit-v2/MICROBIT.hex` and the metadata (extracted from the linked
ELF's region symbols) in `targets/microbit-v2/MICROBIT.metadata.json`.

To watch the firmware's serial output on macOS (115200 baud; the micro:bit
shows up as a `usbmodem` device):

```
screen /dev/cu.usbmodem* 115200
```

Exit screen with ctrl-a then k, then y.

## Debugging in VS Code

The committed `.vscode/` at the repo root provides tasks and launch
configurations:

1. Install the recommended extensions (CMake Tools, CodeLLDB).
2. Run the task `cpp: build (debug)` (Terminal -> Run Task), or let the launch
   configuration trigger it.
3. Set a breakpoint in any test under `cpp/test/` and start the
   `cpp tests (CodeLLDB)` launch configuration. A cpptools-based
   `cpp tests (cpptools)` configuration is the fallback.

## Foundations

- C++ standard: C++17, no compiler extensions, one standard for the whole
  tree.
- `core/` builds with `-fno-exceptions -fno-rtti`. Errors propagate as
  `Status` / `Result<T>` returns carrying numeric `ErrorCode`s
  (`core/runtime/result.h`, `core/runtime/error-code.h`).
- The test tree builds with exceptions enabled (doctest requires them) and
  links the exception-free core.
- Warnings: `-Wall -Wextra -Werror` everywhere in the host tree.
- Std-library policy for `core/`: freestanding-leaning. `<cstdint>`,
  `<cstddef>`, `<cstring>`, `<type_traits>`, `std::array`, and span-style
  views are allowed; `std::string`, `std::vector`, `std::map`, iostreams, and
  any heap-allocating std facility are not. `test/` may use the full standard
  library.
- No process-global mutable state: the VM, scheduler, and runtime are
  constructed instances.
- Formatting: `.clang-format` in this directory; enforced by `check.sh`.

## Memory model

All VM working memory comes from one allocator over one region
(`core/runtime/region-arena.h`) - there is no second pool system and nothing is
pre-sized. The region is a forward-only bump allocator with two clients:

- **Program-lifetime data** is bump-allocated once and never individually freed:
  the decoded program image (the Phase 2 reader) and the program-sized context
  slot tables (brain variables and per-callsite state, bound at brain startup).
- **Individually-managed objects** go through `core/runtime/pool.h` - one
  `Pool<T>` template, shared by every typed pool. A `Pool<T>` carves a `T` slot
  from the region on first use, recycles it through a free list on release, and
  chains its slots so the live set is iterable (the basis for scheduler scans
  now and mark-sweep collection later). The scheduler holds `Pool<FiberRecord>`
  and `Pool<FiberWorkspace>`; the heap's value-container pools (Phase 6a) are
  further instances of the same template.

A dormant fiber slot or unused variable costs zero region bytes, fibers free in
any order (no stack-discipline retention), and the region's size is the one
number a target picks - against the free SRAM left after CODAL, not a
`fibers x caps` product.

The micro:bit v2 firmware claims its region from CODAL's heap at boot (the
largest free block less a CODAL headroom), so the region scales to the board's
actual free SRAM rather than a compile-time constant. A brain needing more
concurrently live fibers than the region holds faults `ErrorCode::StackOverflow`
loudly at spawn. The host parity build allocates a single fixed scratch region
(image plus pooled fibers), so CI exercises the device model; the board build
links and runs against real SRAM as the final validation.

## Mirror headers

Hand-maintained C++ mirrors of the TS-declared device ABI id spaces live in
`core/runtime/` (core id spaces) and `targets/microbit-v2/abi/` (the target
id spaces), one header per id space, with the enum and its companion table
side by side in the same header. Each header names the TS declaration file it
mirrors. The values are wire-stable contracts: never renumber, never reuse a
retired value, append only. `core/runtime/error-code.h` (mirroring
`ErrorCode` in core `runtime/value.ts`) is the specimen for the convention.
Every mirror header has a value-pinning test under `test/`; update the test
together with the mirror.

## Test fixtures

Parity tests consume the committed `.mcprogram` / `.mcprogram.bin` fixtures
from the TS packages. The fixture roots are wired by `test/CMakeLists.txt` as
compile definitions exposed through `test/fixture-paths.h`:

- core: `external/wendoo-lang/packages/core/src/runtime/__fixtures__/`
- wodal: `packages/wodal/src/targets/microbit-v2/wendoo/__fixtures__/`
- shared corpus: `external/wendoo-lang/packages/conformance/corpus/`

Goldens are regenerated by the TS build path (write-if-missing, byte-stable),
never hand-edited; after regenerating fixtures, re-run `./check.sh`. The
shared corpus is minted upstream only and is never regenerated from here.
