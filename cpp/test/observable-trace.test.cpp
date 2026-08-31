#include "doctest/doctest.h"

#include "core/runtime/brain-runtime.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/fiber-scheduler.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"
#include "hostkit/observable-trace.h"
#include "string-sink.h"
#include "vm-harness.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using wendoo::BrainRuntime;
using wendoo::ErrorCode;
using wendoo::ExecutionContext;
using wendoo::FiberScheduler;
using wendoo::Frame;
using wendoo::ObservableTraceWriter;
using wendoo::Op;
using wendoo::ProgramImage;
using wendoo::RegionArena;
using wendoo::RuntimeSurface;
using wendoo::Span;
using wendoo::Value;
using wendoo::VmObserver;

namespace {

/** Forwards scheduler faults to the trace writer. */
struct FaultTraceTap : VmObserver {
  explicit FaultTraceTap(ObservableTraceWriter& writer) : writer(writer) {}

  ObservableTraceWriter& writer;

  void onHostActionCall(uint32_t, uint32_t, Span<const Value>, const Value&) override {}

  void onFiberFault(uint32_t fiberId, ErrorCode code) override { writer.fiberFault(fiberId, code); }
};

/** Forwards bytecode-action dispatches to the trace writer. */
struct BytecodeActionTraceTap : VmObserver {
  explicit BytecodeActionTraceTap(ObservableTraceWriter& writer) : writer(writer) {}

  ObservableTraceWriter& writer;

  void onHostActionCall(uint32_t, uint32_t, Span<const Value>, const Value&) override {}

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

/** Call-site id the bytecode-action fixtures bind their dispatch to. */
constexpr uint32_t kActionCallSiteId = 5;

} // namespace

TEST_CASE("the trace header renders the format version, profile, and precision") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  CHECK(sink.text() == "mctrace 1\nprofile 0\nprecision f32\n");
}

TEST_CASE("tick lines render 1-based ordinals and f32 bit-pattern stamps") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  writer.tick(1, 16.0f, 0.0f);
  writer.tick(10, 224.0f, 16.0f);
  const std::string expected = "mctrace 1\nprofile 0\nprecision f32\n"
                               "tick 1 time 41800000 dt 00000000\n"
                               "tick a time 43600000 dt 41800000\n";
  CHECK(sink.text() == expected);
}

TEST_CASE("action lines render every defined value token") {
  ProgramBuilder b;
  b.poolString("a\"b\\c\x01");
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  const Value args[5] = {wendoo::kVoidValue, wendoo::kNilValue, Value::boolean(true),
                         Value::number(-7.25f), Value::borrowedString(0)};
  writer.hostActionCall(0x400, 3, Span<const Value>(args, 5), Value::boolean(false));
  const std::string expected =
      "mctrace 1\nprofile 0\nprecision f32\n"
      "action 400 site 3 args 5 void nil bool 1 number c0e80000 string \"a\\\"b\\\\c\\x01\""
      " result bool 0\n";
  CHECK(sink.text() == expected);
}

TEST_CASE("a value kind outside the trace vocabulary renders opaque") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  // A function value has no rendering, and an enum whose type is not in the
  // program's type table cannot resolve its symbol.
  const Value args[1] = {Value::enumSymbol(0, 2)};
  writer.hostActionCall(1, 0, Span<const Value>(args, 1), Value::function(0, 0));
  const std::string expected = "mctrace 1\nprofile 0\nprecision f32\n"
                               "action 1 site 0 args 1 opaque result opaque\n";
  CHECK(sink.text() == expected);
}

TEST_CASE("an enum value renders its symbol name") {
  ProgramBuilder b;
  b.poolString("Go");
  b.poolString("Stop");
  b.enumType(0, {{0, 0}, {1, 1}});
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  const Value args[1] = {Value::enumSymbol(0, 1)};
  writer.hostActionCall(2, 0, Span<const Value>(args, 1), Value::enumSymbol(0, 0));
  const std::string expected = "mctrace 1\nprofile 0\nprecision f32\n"
                               "action 2 site 0 args 1 enum \"Stop\" result enum \"Go\"\n";
  CHECK(sink.text() == expected);
}

TEST_CASE("port and fault lines render their fixed shapes") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  writer.displaySetPixel(0.0f, 2.0f, 255.0f);
  const uint8_t i2cBytes[5] = {1, 2, 3, 4, 5};
  writer.i2cWrite(0x10, i2cBytes, 5);
  const uint8_t i2cRead[3] = {0xaa, 0xbb, 0xcc};
  writer.i2cRead(0x42, 3, i2cRead, 3);
  writer.i2cRead(0x55, 2, i2cRead, 0);
  writer.gpioDigitalRead(0xd, 1);
  writer.gpioDigitalWrite(0x2, 1);
  writer.gpioSetPull(0xd, 0);
  writer.gpioServoWrite(0x1, 0x5a);
  writer.gpioAnalogRead(0x1, 0x3ff);
  writer.fiberFault(3, ErrorCode::StackUnderflow);
  const std::string expected = "mctrace 1\nprofile 0\nprecision f32\n"
                               "port display set-pixel 00000000 40000000 437f0000\n"
                               "port i2c write 10 0102030405\n"
                               "port i2c read 42 3 aabbcc\n"
                               "port i2c read 55 2 \n"
                               "port gpio digital-read d 1\n"
                               "port gpio digital-write 2 1\n"
                               "port gpio set-pull d 0\n"
                               "port gpio servo-write 1 5a\n"
                               "port gpio analog-read 1 3ff\n"
                               "fault 3 6\n";
  CHECK(sink.text() == expected);
}

TEST_CASE("speaker lines render the played sound and the tone's port command") {
  ProgramBuilder b;
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  const uint8_t name[5] = {'h', 'e', 'l', 'l', 'o'};
  writer.speakerPlay(name, 5);
  writer.speakerTone(0, 880.0f, 500, 1.0f);
  writer.speakerTone(1, 262.0f, 300, 0.5f);
  writer.speakerTone(2, 9999.0f, 0, 0.25f);
  writer.speakerTone(3, 0.0f, 1000, 0.0f);
  const std::string expected = "mctrace 1\nprofile 0\nprecision f32\n"
                               "port speaker play \"hello\"\n"
                               "port speaker tone square 445c0000 1f4 3f800000\n"
                               "port speaker tone sawtooth 43830000 12c 3f000000\n"
                               "port speaker tone sine 461c3c00 0 3e800000\n"
                               "port speaker tone triangle 00000000 3e8 00000000\n";
  CHECK(sink.text() == expected);
}

TEST_CASE("a faulting rule traces the fault line shape and respawns next think") {
  // A synthetic program whose only rule faults immediately: each think emits
  // one fault line carrying a fresh fiber id and the numeric ErrorCode.
  ProgramBuilder b;
  b.poolString("page-id");
  b.beginFunction().instr(Op::POP).instr(Op::RET);
  b.ruleFunc(0);
  b.beginPage(0).pageRoot(0);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  FaultTraceTap tap(writer);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {}, &tap};
  std::array<uint8_t, 4 * (2048 + sizeof(wendoo::FiberRecord) + 64) + 256> arenaBytes;
  RegionArena arena(Span<uint8_t>(arenaBytes.data(), arenaBytes.size()));
  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  writer.tick(1, 16.0f, 0.0f);
  REQUIRE(brain.think(16.0f).isOk());
  writer.tick(2, 32.0f, 16.0f);
  REQUIRE(brain.think(32.0f).isOk());

  const std::string expected = "mctrace 1\nprofile 0\nprecision f32\n"
                               "tick 1 time 41800000 dt 00000000\n"
                               "fault 1 6\n"
                               "tick 2 time 42000000 dt 41800000\n"
                               "fault 2 6\n";
  CHECK(sink.text() == expected);
}

TEST_CASE("a synchronous bytecode-action call traces at the body's hand-back") {
  // A rule dispatching the program's one bytecode action with a single number
  // argument; the body returns a number of its own. The action line renders the
  // action's slot as its id and the value the body handed back.
  ProgramBuilder b;
  b.poolString("page-id");
  b.number(7);  // const 0: the dispatched argument
  b.number(42); // const 1: the body's return value
  b.valueNil(); // value const 0: what the rule returns
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::ACTION_CALL, 0, 1, kActionCallSiteId)
      .instr(Op::POP)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::RET);
  b.beginFunction(1).instr(Op::PUSH_CONST_NUM, 1).instr(Op::RET);
  b.bytecodeAction(1);
  b.ruleFunc(0);
  b.beginPage(0).pageRoot(0);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  BytecodeActionTraceTap tap(writer);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {}, &tap};
  std::array<uint8_t, 4 * (2048 + sizeof(wendoo::FiberRecord) + 64) + 256> arenaBytes;
  RegionArena arena(Span<uint8_t>(arenaBytes.data(), arenaBytes.size()));
  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  writer.tick(1, 16.0f, 0.0f);
  REQUIRE(brain.think(16.0f).isOk());

  const std::string expected = "mctrace 1\nprofile 0\nprecision f32\n"
                               "tick 1 time 41800000 dt 00000000\n"
                               "tile 0 site 5 args 1 number 40e00000 result number 42280000\n";
  CHECK(sink.text() == expected);
}

TEST_CASE("an asynchronous bytecode-action call traces at the dispatch") {
  // The same action dispatched through ACTION_CALL_ASYNC: the line renders when
  // the child fiber running the body is spawned, and ends with `async` for the
  // pending handle the dispatch yields.
  ProgramBuilder b;
  b.poolString("page-id");
  b.number(7);
  b.number(42);
  b.valueNil();
  b.beginFunction()
      .instr(Op::PUSH_CONST_NUM, 0)
      .instr(Op::ACTION_CALL_ASYNC, 0, 1, kActionCallSiteId)
      .instr(Op::AWAIT)
      .instr(Op::POP)
      .instr(Op::PUSH_CONST_VAL, 0)
      .instr(Op::RET);
  b.beginFunction(1).instr(Op::PUSH_CONST_NUM, 1).instr(Op::RET);
  b.bytecodeAction(1);
  b.ruleFunc(0);
  b.beginPage(0).pageRoot(0);
  std::vector<uint8_t> storage(16 * 1024);
  const ProgramImage image = b.build(storage);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  BytecodeActionTraceTap tap(writer);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {}, &tap};
  std::array<uint8_t, 4 * (2048 + sizeof(wendoo::FiberRecord) + 64) + 256> arenaBytes;
  RegionArena arena(Span<uint8_t>(arenaBytes.data(), arenaBytes.size()));
  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  REQUIRE(brain.startup().isOk());

  writer.tick(1, 16.0f, 0.0f);
  REQUIRE(brain.think(16.0f).isOk());

  const std::string expected = "mctrace 1\nprofile 0\nprecision f32\n"
                               "tick 1 time 41800000 dt 00000000\n"
                               "tile 0 site 5 args 1 number 40e00000 async\n";
  CHECK(sink.text() == expected);
}
