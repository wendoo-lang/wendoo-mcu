#include "doctest/doctest.h"

#include "conformance-profile.h"
#include "core/codec/program-reader.h"
#include "core/runtime/brain-runtime.h"
#include "core/runtime/device-profile-caps.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/fiber-scheduler.h"
#include "core/runtime/host-action.h"
#include "core/runtime/host-actions/core-host-action-bindings.h"
#include "core/runtime/host-actions/core-host-action-env.h"
#include "core/runtime/load-error.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/type-registry.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"
#include "fixture-paths.h"
#include "hostkit/observable-trace.h"
#include "json.h"
#include "string-sink.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

using wendoo::BrainRuntime;
using wendoo::ByteSpan;
using wendoo::DeviceProfileCaps;
using wendoo::ErrorCode;
using wendoo::ExecutionContext;
using wendoo::FiberScheduler;
using wendoo::HostActionBinding;
using wendoo::LoadError;
using wendoo::ObservableTraceWriter;
using wendoo::ProgramImage;
using wendoo::ProgramReaderOptions;
using wendoo::RegionArena;
using wendoo::Result;
using wendoo::RuntimeSurface;
using wendoo::Span;
using wendoo::Value;
using wendoo::VmObserver;
using wendoo::test::ConformanceWorld;
using wendoo::test::JsonValue;

namespace {

/**
 * Precision variant this VM replays. `mc_number_t` is the build's device
 * numeric type; a corpus case is replayed only when the manifest declares this
 * variant for it.
 */
constexpr const char* kProfilePrecision = "f32";

/**
 * Scheduling and per-fiber caps the shared corpus is minted under. Mirrors
 * `CONFORMANCE_SCHEDULER_CONFIG` in
 * external/wendoo-lang/packages/conformance/src/profile.ts; every VM replaying
 * the corpus schedules with these values.
 */
constexpr DeviceProfileCaps kConformanceCaps{1000, 10000, 100, 256, 256, 64, 16, 8};

/**
 * Type-atom ranges the conformance profile registers. It declares no target and
 * no shared type atoms, so a corpus binary referencing either fails to decode.
 */
constexpr ProgramReaderOptions kConformanceReaderOptions{0, 0};

/** One corpus case as the manifest declares it. */
struct CorpusCase {
  /** Case id; the basename every artifact of the case is filed under. */
  std::string id;
  /** Precisions the case is minted at. */
  std::vector<std::string> precisions;
  /** Tick advances in milliseconds, in order; one think per entry. */
  std::vector<float> schedule;
};

/** The corpus manifest: the index of cases every VM must pass. */
struct CorpusManifest {
  /** Numeric device-profile id every committed binary envelope and trace header carries. */
  uint32_t profileId;
  /** The cases, in manifest order. */
  std::vector<CorpusCase> cases;
};

std::string corpusPath(const std::string& name) {
  return std::string(wendoo::test::kConformanceCorpusDir) + "/" + name;
}

std::vector<uint8_t> readBinaryFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE_MESSAGE(stream.good(), "cannot open ", path);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream),
                              std::istreambuf_iterator<char>());
}

std::string readTextFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE_MESSAGE(stream.good(), "cannot open ", path);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

/** Reads and parses the corpus manifest into its case index. */
CorpusManifest readManifest() {
  const JsonValue root = wendoo::test::parseJson(readTextFile(corpusPath("manifest.json")));
  CorpusManifest manifest;
  manifest.profileId = static_cast<uint32_t>(root.member("profileId").number());
  for (const JsonValue& entry : root.member("cases").elements()) {
    CorpusCase entryCase;
    entryCase.id = entry.member("id").string();
    for (const JsonValue& precision : entry.member("precisions").elements()) {
      entryCase.precisions.push_back(precision.string());
    }
    for (const JsonValue& advance : entry.member("schedule").elements()) {
      entryCase.schedule.push_back(static_cast<float>(advance.number()));
    }
    manifest.cases.push_back(entryCase);
  }
  return manifest;
}

/** True when `entry` is minted at the precision this VM computes in. */
bool hasProfilePrecision(const CorpusCase& entry) {
  for (const std::string& precision : entry.precisions) {
    if (precision == kProfilePrecision) {
      return true;
    }
  }
  return false;
}

/** Forwards the VM's host-binding events into the observable trace. */
struct TraceTap : VmObserver {
  explicit TraceTap(ObservableTraceWriter& writer) : writer(writer) {}

  ObservableTraceWriter& writer;

  void onHostActionCall(uint32_t actionId, uint32_t callSiteId, Span<const Value> args,
                        const Value& result) override {
    writer.hostActionCall(actionId, callSiteId, args, result);
  }

  void onHostActionCallAsync(uint32_t actionId, uint32_t callSiteId,
                             Span<const Value> args) override {
    writer.hostActionCallAsync(actionId, callSiteId, args);
  }

  void onBytecodeActionCall(uint32_t actionSlot, uint32_t callSiteId, Span<const Value> args,
                            const Value& result) override {
    writer.bytecodeActionCall(actionSlot, callSiteId, args, result);
  }

  void onBytecodeActionCallAsync(uint32_t actionSlot, uint32_t callSiteId,
                                 Span<const Value> args) override {
    writer.bytecodeActionCallAsync(actionSlot, callSiteId, args);
  }

  void onFiberFault(uint32_t fiberId, ErrorCode code) override { writer.fiberFault(fiberId, code); }
};

/** The action table a corpus case runs against: the core surface then the conformance profile. */
using ConformanceActionTable =
    std::array<HostActionBinding, wendoo::kCoreHostActionBindingCount +
                                      wendoo::test::kConformanceHostActionBindingCount>;

ConformanceActionTable combineActionTable(
    const std::array<HostActionBinding, wendoo::kCoreHostActionBindingCount>& core,
    const std::array<HostActionBinding, wendoo::test::kConformanceHostActionBindingCount>&
        conformance) {
  ConformanceActionTable table{};
  for (size_t i = 0; i < core.size(); i++) {
    table[i] = core[i];
  }
  for (size_t i = 0; i < conformance.size(); i++) {
    table[core.size() + i] = conformance[i];
  }
  return table;
}

/**
 * Replays `wire` over `schedule` and returns the rendered observable trace. The
 * run reads nothing outside the decoded program, the schedule, and a fresh
 * {@link ConformanceWorld}. Mirrors `runTrace` in
 * external/wendoo-lang/packages/conformance/src/mint.ts, including the one
 * ordering rule the profile adds: the settlements due at ordinal N run
 * immediately before the think of ordinal N.
 */
std::string runTrace(const std::vector<uint8_t>& wire, const std::vector<float>& schedule,
                     uint32_t profileId) {
  std::vector<uint8_t> arenaStorage(256 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, kConformanceReaderOptions);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();
  REQUIRE(image.profileId == profileId);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::TypeRegistry types(image);
  writer.setHeap(&heap);
  ConformanceWorld world;
  const auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  const auto conformanceBindings = wendoo::test::makeConformanceHostActionBindings(world);
  ConformanceActionTable actions = combineActionTable(coreBindings, conformanceBindings);
  const auto hostFuncs = wendoo::test::makeConformanceHostFuncBindings(world);

  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, kConformanceCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;
  coreEnv.ruleLiveness = &scheduler;

  REQUIRE(brain.startup().isOk());

  float lastThinkTimeMs = 0;
  for (size_t i = 0; i < schedule.size(); i++) {
    const uint32_t ordinal = static_cast<uint32_t>(i + 1);
    const float timeMs = lastThinkTimeMs + schedule[i];
    writer.tick(ordinal, timeMs, lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    world.settleDue(ordinal);
    REQUIRE(brain.think(timeMs).isOk());
    lastThinkTimeMs = timeMs;
  }
  return sink.text();
}

} // namespace

TEST_CASE("every shared corpus case replays to its committed observable trace") {
  const CorpusManifest manifest = readManifest();
  REQUIRE(manifest.profileId == wendoo::test::kConformanceProfileId);
  REQUIRE_FALSE(manifest.cases.empty());

  uint32_t replayed = 0;
  for (const CorpusCase& entry : manifest.cases) {
    CAPTURE(entry.id);
    if (!hasProfilePrecision(entry)) {
      continue;
    }
    const std::string base = entry.id + "." + kProfilePrecision;
    const std::vector<uint8_t> wire = readBinaryFile(corpusPath(base + ".program.bin"));
    const std::string golden = readTextFile(corpusPath(base + ".trace"));
    CHECK(runTrace(wire, entry.schedule, manifest.profileId) == golden);
    replayed++;
  }
  // Every manifest case declares this VM's precision.
  CHECK(replayed == manifest.cases.size());
}
