#include "doctest/doctest.h"

#include "codal/device-port.h"
#include "codal/shared-type-atom-id.h"
#include "core/runtime/context-field.h"
#include "core/runtime/core-type-atom-id.h"
#include "core/runtime/handle-table.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/region-arena.h"
#include "targets/microbit-v2/abi/host-actions.h"
#include "targets/microbit-v2/abi/host-actions/host-action-bindings.h"
#include "targets/microbit-v2/abi/host-func-id.h"
#include "targets/microbit-v2/abi/host-functions/host-func-bindings.h"
#include "targets/microbit-v2/abi/microbit-field.h"
#include "targets/microbit-v2/abi/type-atom-id.h"

#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

using wendoo::CONTEXT_MICROBIT_FIELD_ID;
using wendoo::kContextFieldCount;
using wendoo::kMicroBitFieldCount;
using wendoo::kMicroBitV2HostActions;
using wendoo::kMicroBitV2HostFuncIdCount;
using wendoo::kMicroBitV2TypeAtomIdCount;
using wendoo::MicroBitField;
using wendoo::MicroBitV2HostFuncId;
using wendoo::MicroBitV2TypeAtomId;
using wendoo::TARGET_ACTION_ID_BASE;
using wendoo::TARGET_FUNC_ID_BASE;
using wendoo::TARGET_TYPE_ATOM_BASE;
namespace MicroBitV2HostActions = wendoo::MicroBitV2HostActions;

TEST_CASE("MicroBitV2HostFuncId values are wire-stable") {
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::DisplaySetPixelValue) == 1024);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::DisplayGetPixelValue) == 1025);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::DisplayClear) == 1026);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::ButtonIsPressed) == 1027);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::TouchButtonIsPressed) == 1028);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::TouchButtonGetThreshold) == 1029);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::TouchButtonSetThreshold) == 1030);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::TouchButtonGetValue) == 1031);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::TouchButtonSetValue) == 1032);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonA) == 1033);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDisplaySetPixel) == 1034);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDisplayScroll) == 1035);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonB) == 1036);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonAB) == 1037);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonLogo) == 1038);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::AccelerometerGetX) == 1039);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::AccelerometerGetY) == 1040);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::AccelerometerGetZ) == 1041);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::AccelerometerGetPitchRadians) == 1042);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::AccelerometerGetRollRadians) == 1043);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::AccelerometerGetPitch) == 1044);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::AccelerometerGetRoll) == 1045);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::AccelerometerGetGesture) == 1046);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SensorGesture) == 1047);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDrawImage) == 1048);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::DisplayDrawImage) == 1049);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::I2CWriteBuffer) == 1050);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::I2CReadBuffer) == 1051);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::GpioDigitalRead) == 1052);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::GpioDigitalWrite) == 1053);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::GpioSetPull) == 1054);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::GpioServoWrite) == 1055);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SonarDistance) == 1056);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::RadioSendNumber) == 1057);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::RadioSendString) == 1058);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::RadioSendValue) == 1059);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::RadioSendBuffer) == 1060);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::RadioSendRawBuffer) == 1061);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::RadioSetGroup) == 1062);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::RadioSetTransmitPower) == 1063);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::RadioSetFrequencyBand) == 1064);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::RadioReceive) == 1065);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorRadioSend) == 1066);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SensorRadioReceiveNumber) == 1067);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SensorRadioReceiveString) == 1068);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorSetRadioGroup) == 1069);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::RadioCurrentSeq) == 1070);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::GpioAnalogRead) == 1071);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SensorRadioReceiveBuffer) == 1072);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::DisplayScrollText) == 1073);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorPlaySound) == 1074);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::AudioPlaySound) == 1075);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDisplayClear) == 1076);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::DisplayGetLightLevel) == 1077);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SensorLightLevel) == 1078);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::ThermometerGetTemperature) == 1079);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::SensorTemperature) == 1080);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorPlayTone) == 1081);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::AudioPlayTone) == 1082);
  CHECK(kMicroBitV2HostFuncIdCount == 59);
  CHECK(static_cast<uint32_t>(MicroBitV2HostFuncId::DisplaySetPixelValue) == TARGET_FUNC_ID_BASE);
}

TEST_CASE("the host-function binding table binds every declared device-API host function") {
  // The binding table closes the declared-but-unbound gap: a host function declared
  // in MicroBitV2HostFuncId but absent from the table faults at dispatch. Pin the
  // count and that DisplayClear (the device-API clear) resolves to a sync body.
  wendoo::DevicePorts ports{};
  const auto bindings = wendoo::makeMicroBitV2HostFuncBindings(ports);
  CHECK(bindings.size() == wendoo::kMicroBitV2HostFuncBindingCount);
  CHECK(wendoo::kMicroBitV2HostFuncBindingCount == 36);
  const wendoo::TargetHostFuncBinding* clearBinding = nullptr;
  for (const auto& binding : bindings) {
    if (binding.funcId == static_cast<uint32_t>(MicroBitV2HostFuncId::DisplayClear)) {
      clearBinding = &binding;
    }
  }
  REQUIRE(clearBinding != nullptr);
  CHECK(clearBinding->exec != nullptr);
  CHECK(clearBinding->execAsync == nullptr);
}

TEST_CASE("MicroBitV2TypeAtomId values are wire-stable") {
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::MicroBitDisplay) == 1024);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::Button) == 1025);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::TouchButton) == 1026);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::MicroBit) == 1027);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::Accelerometer) == 1028);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::I2C) == 1029);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::GPIO) == 1030);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::Sonar) == 1031);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::Radio) == 1032);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::RadioPacket) == 1033);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::RadioPacketList) == 1034);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::SoundEmoji) == 1035);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::MicroBitAudio) == 1036);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::MicroBitThermometer) == 1037);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::PlaySoundOptions) == 1038);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::DrawImageOptions) == 1039);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::ScrollTextOptions) == 1040);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::PlayToneOptions) == 1041);
  CHECK(kMicroBitV2TypeAtomIdCount == 18);
  CHECK(static_cast<uint32_t>(MicroBitV2TypeAtomId::MicroBitDisplay) == TARGET_TYPE_ATOM_BASE);
}

TEST_CASE("SharedTypeAtomId values are wire-stable") {
  CHECK(static_cast<uint32_t>(wendoo::SharedTypeAtomId::Image) == 2048);
  CHECK(wendoo::kSharedTypeAtomIdCount == 1);
  CHECK(static_cast<uint32_t>(wendoo::SharedTypeAtomId::Image) == wendoo::SHARED_TYPE_ATOM_BASE);
}

TEST_CASE("microbit-v2 host-action ids are wire-stable") {
  CHECK(MicroBitV2HostActions::ButtonA.actionId == 1024);
  CHECK(MicroBitV2HostActions::ButtonA.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonA));
  CHECK(MicroBitV2HostActions::ButtonA.fnId == 1033);
  CHECK(MicroBitV2HostActions::DisplaySetPixel.actionId == 1025);
  CHECK(MicroBitV2HostActions::DisplaySetPixel.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDisplaySetPixel));
  CHECK(MicroBitV2HostActions::DisplaySetPixel.fnId == 1034);
  CHECK(MicroBitV2HostActions::DisplayScroll.actionId == 1026);
  CHECK(MicroBitV2HostActions::DisplayScroll.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDisplayScroll));
  CHECK(MicroBitV2HostActions::DisplayScroll.fnId == 1035);
  CHECK(MicroBitV2HostActions::ButtonB.actionId == 1027);
  CHECK(MicroBitV2HostActions::ButtonB.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonB));
  CHECK(MicroBitV2HostActions::ButtonAB.actionId == 1028);
  CHECK(MicroBitV2HostActions::ButtonAB.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonAB));
  CHECK(MicroBitV2HostActions::ButtonLogo.actionId == 1029);
  CHECK(MicroBitV2HostActions::ButtonLogo.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::SensorButtonLogo));
  CHECK(MicroBitV2HostActions::Gesture.actionId == 1030);
  CHECK(MicroBitV2HostActions::Gesture.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::SensorGesture));
  CHECK(MicroBitV2HostActions::DrawImage.actionId == 1031);
  CHECK(MicroBitV2HostActions::DrawImage.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDrawImage));
  CHECK(MicroBitV2HostActions::RadioSend.actionId == 1032);
  CHECK(MicroBitV2HostActions::RadioSend.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorRadioSend));
  CHECK(MicroBitV2HostActions::RadioReceiveNumber.actionId == 1033);
  CHECK(MicroBitV2HostActions::RadioReceiveNumber.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::SensorRadioReceiveNumber));
  CHECK(MicroBitV2HostActions::RadioReceiveString.actionId == 1034);
  CHECK(MicroBitV2HostActions::RadioReceiveString.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::SensorRadioReceiveString));
  CHECK(MicroBitV2HostActions::SetRadioGroup.actionId == 1035);
  CHECK(MicroBitV2HostActions::SetRadioGroup.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorSetRadioGroup));
  CHECK(MicroBitV2HostActions::RadioReceiveBuffer.actionId == 1036);
  CHECK(MicroBitV2HostActions::RadioReceiveBuffer.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::SensorRadioReceiveBuffer));
  CHECK(MicroBitV2HostActions::PlaySound.actionId == 1037);
  CHECK(MicroBitV2HostActions::PlaySound.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorPlaySound));
  CHECK(MicroBitV2HostActions::DisplayClear.actionId == 1038);
  CHECK(MicroBitV2HostActions::DisplayClear.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorDisplayClear));
  CHECK(MicroBitV2HostActions::DisplayClear.fnId == 1076);
  CHECK(MicroBitV2HostActions::LightLevel.actionId == 1039);
  CHECK(MicroBitV2HostActions::LightLevel.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::SensorLightLevel));
  CHECK(MicroBitV2HostActions::LightLevel.fnId == 1078);
  CHECK(MicroBitV2HostActions::Temperature.actionId == 1040);
  CHECK(MicroBitV2HostActions::Temperature.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::SensorTemperature));
  CHECK(MicroBitV2HostActions::Temperature.fnId == 1080);
  CHECK(MicroBitV2HostActions::PlayTone.actionId == 1041);
  CHECK(MicroBitV2HostActions::PlayTone.fnId ==
        static_cast<uint32_t>(MicroBitV2HostFuncId::ActuatorPlayTone));
  CHECK(MicroBitV2HostActions::PlayTone.fnId == 1081);

  REQUIRE(std::size(kMicroBitV2HostActions) == 18);
  for (uint32_t i = 0; i < std::size(kMicroBitV2HostActions); i++) {
    CHECK(kMicroBitV2HostActions[i].actionId == TARGET_ACTION_ID_BASE + i);
  }
}

TEST_CASE("MicroBitField values are wire-stable") {
  CHECK(static_cast<uint8_t>(MicroBitField::Display) == 0);
  CHECK(static_cast<uint8_t>(MicroBitField::ButtonA) == 1);
  CHECK(static_cast<uint8_t>(MicroBitField::ButtonB) == 2);
  CHECK(static_cast<uint8_t>(MicroBitField::Logo) == 3);
  CHECK(static_cast<uint8_t>(MicroBitField::Accelerometer) == 4);
  CHECK(static_cast<uint8_t>(MicroBitField::I2C) == 5);
  CHECK(static_cast<uint8_t>(MicroBitField::GPIO) == 6);
  CHECK(static_cast<uint8_t>(MicroBitField::Sonar) == 7);
  CHECK(static_cast<uint8_t>(MicroBitField::Radio) == 8);
  CHECK(static_cast<uint8_t>(MicroBitField::Audio) == 9);
  CHECK(static_cast<uint8_t>(MicroBitField::Thermometer) == 10);
  CHECK(kMicroBitFieldCount == 11);
}

TEST_CASE("the Context.microbit extension id sits just above the core Context fields") {
  CHECK(CONTEXT_MICROBIT_FIELD_ID == 6);
  CHECK(CONTEXT_MICROBIT_FIELD_ID == kContextFieldCount);
}

namespace {

using wendoo::AsyncHandle;
using wendoo::HandleState;
using wendoo::HandleTable;
using wendoo::ManagedHeap;
using wendoo::mc_number_t;
using wendoo::RegionArena;
using wendoo::SpeakerToneCommand;
using wendoo::SpeakerToneWaveform;
using wendoo::Value;

/** GC roots for the tone dispatch fixture: nothing is rooted. */
struct NoRoots : wendoo::GcRoots {
  void enumerateRoots(wendoo::GcMarker&) override {}
};

/**
 * Host stub speaker under the speaker lease: an accepted tone is recorded and
 * holds the lease until {@link advance} settles it at its end time. A tone
 * dispatched while the lease is held, and one with a negative duration, is
 * dropped -- nothing is recorded and its handle settles at once.
 */
struct LeasingSpeaker : wendoo::SpeakerPort {
  std::vector<SpeakerToneCommand> tones;
  int preempts = 0;
  bool busy = false;
  mc_number_t completionTime = 0;
  AsyncHandle active{};

  void playSoundEmoji(const uint8_t*, uint32_t, mc_number_t, AsyncHandle handle) override {
    handle.resolve(wendoo::kVoidValue);
  }

  void playTone(const SpeakerToneCommand& tone, mc_number_t requestTimeMs,
                AsyncHandle handle) override {
    if (busy || tone.durationMs < 0) {
      handle.resolve(wendoo::kVoidValue);
      return;
    }
    tones.push_back(tone);
    busy = true;
    completionTime = requestTimeMs + static_cast<mc_number_t>(tone.durationMs);
    active = handle;
  }

  void preempt() override {
    preempts++;
    if (!busy) {
      return;
    }
    const AsyncHandle held = active;
    busy = false;
    held.resolve(wendoo::kVoidValue);
  }

  /** Settles the held tone once its duration has elapsed by `now`. */
  void advance(mc_number_t now) {
    if (!busy || now < completionTime) {
      return;
    }
    busy = false;
    active.resolve(wendoo::kVoidValue);
  }
};

/** Arg-slot count of the beep tile action, one nil per unfilled slot. */
constexpr size_t kPlayToneArgCount = 9;

/** Arg-slot count of the `MicroBitAudio.playTone` host function. */
constexpr size_t kAudioPlayToneArgCount = 3;

/**
 * Dispatches the play-tone bodies against a leasing speaker, over the heap and
 * handle table they settle through.
 */
struct ToneDispatch {
  std::vector<uint8_t> storage = std::vector<uint8_t>(64 * 1024);
  RegionArena arena{wendoo::Span<uint8_t>(storage.data(), storage.size())};
  ManagedHeap heap{arena};
  HandleTable handles{arena, 32};
  NoRoots roots;
  LeasingSpeaker speaker;
  wendoo::DevicePorts ports{};
  wendoo::ExecutionContext ctx;

  ToneDispatch() { ports.speaker = &speaker; }

  /** The tone command the last accepted dispatch handed the speaker port. */
  const SpeakerToneCommand& lastTone() {
    REQUIRE(!speaker.tones.empty());
    return speaker.tones.back();
  }

  /** The state the handle bound to a dispatch settled to. */
  HandleState stateOf(uint32_t handleId) {
    const wendoo::Handle* handle = handles.get(handleId);
    REQUIRE(handle != nullptr);
    return handle->state;
  }

  /** Dispatches the beep tile action with `args`, returning its bound handle id. */
  uint32_t dispatchTile(const std::vector<Value>& args) {
    auto bindings = wendoo::makeMicroBitV2HostActionBindings(ports);
    const wendoo::HostActionBinding* binding = wendoo::findHostActionById(
        {bindings.data(), bindings.size()}, MicroBitV2HostActions::PlayTone.actionId);
    REQUIRE(binding != nullptr);
    REQUIRE(binding->execAsync != nullptr);
    const uint32_t handleId = handles.createPending();
    REQUIRE(binding
                ->execAsync(binding->hostData, ctx, {args.data(), args.size()},
                            AsyncHandle{&handles, handleId})
                .isOk());
    return handleId;
  }

  /** Dispatches `MicroBitAudio.playTone` with `args`, returning its bound handle id. */
  uint32_t dispatchHostFn(const std::vector<Value>& args) {
    wendoo::MicroBitV2PlayToneEnv env{&speaker, &heap};
    auto bindings = wendoo::makeMicroBitV2HostFuncBindings(
        ports, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &env);
    const wendoo::TargetHostFuncBinding* binding =
        wendoo::findTargetHostFuncById({bindings.data(), bindings.size()},
                                       static_cast<uint32_t>(MicroBitV2HostFuncId::AudioPlayTone));
    REQUIRE(binding != nullptr);
    REQUIRE(binding->execAsync != nullptr);
    const uint32_t handleId = handles.createPending();
    REQUIRE(binding
                ->execAsync(binding->hostData, ctx, {args.data(), args.size()},
                            AsyncHandle{&handles, handleId})
                .isOk());
    return handleId;
  }

  /** A `PlayToneOptions` struct value with every field nil. */
  Value options() {
    Value value;
    REQUIRE(heap.newStruct(static_cast<uint32_t>(MicroBitV2TypeAtomId::PlayToneOptions), 5, &roots,
                           value));
    return value;
  }

  /** Sets one field of the `PlayToneOptions` struct `options`. */
  void setOption(const Value& options, uint32_t fieldId, const Value& field) {
    heap.structSet(heap.structOf(options), fieldId, field);
  }

  /** A managed string value holding `text`. */
  Value string(const char* text) {
    Value value;
    uint32_t length = 0;
    while (text[length] != '\0') {
      length++;
    }
    REQUIRE(heap.newString(text, length, &roots, value));
    return value;
  }
};

/** The beep tile arg buffer with every slot nil. */
std::vector<Value> toneArgs() { return std::vector<Value>(kPlayToneArgCount, wendoo::kNilValue); }

/** The playTone host-function arg buffer: the audio receiver, then nil slots. */
std::vector<Value> audioArgs() {
  std::vector<Value> args(kAudioPlayToneArgCount, wendoo::kNilValue);
  args[0] = Value::structValue(static_cast<uint32_t>(MicroBitV2TypeAtomId::MicroBitAudio), 0);
  return args;
}

/** A present plain modifier, as the compiler fills a modifier slot. */
const Value kModifierPresent = Value::boolean(true);

} // namespace

TEST_CASE("a bare beep sounds the default triangle tone for half a second") {
  ToneDispatch dispatch;
  const uint32_t handleId = dispatch.dispatchTile(toneArgs());
  REQUIRE(dispatch.speaker.tones.size() == 1);
  CHECK(dispatch.lastTone().waveform == SpeakerToneWaveform::Triangle);
  CHECK(dispatch.lastTone().frequencyHz == 880.0f);
  CHECK(dispatch.lastTone().durationMs == 500);
  CHECK(dispatch.lastTone().volume == 1.0f);
  // The tone holds the lease, so the issuing rule parks until it ends.
  CHECK(dispatch.stateOf(handleId) == HandleState::Pending);
  dispatch.speaker.advance(500);
  CHECK(dispatch.stateOf(handleId) == HandleState::Resolved);
}

TEST_CASE("each beep argument sets its field of the port command") {
  ToneDispatch dispatch;
  std::vector<Value> args = toneArgs();
  args[wendoo::kPlayToneFrequencyArgSlot] = Value::number(440.0f);
  args[wendoo::kPlayToneDurationArgSlot] = Value::number(0.2f);
  args[wendoo::kPlayToneVolumeArgSlot] = Value::number(0.25f);
  dispatch.dispatchTile(args);
  CHECK(dispatch.lastTone().frequencyHz == 440.0f);
  CHECK(dispatch.lastTone().durationMs == 200);
  CHECK(dispatch.lastTone().volume == 0.25f);
}

TEST_CASE("each wave-shape modifier selects its shape at the port") {
  const struct {
    uint32_t slot;
    SpeakerToneWaveform waveform;
  } shapes[] = {
      {wendoo::kPlayToneSquareArgSlot, SpeakerToneWaveform::Square},
      {wendoo::kPlayToneSawtoothArgSlot, SpeakerToneWaveform::Sawtooth},
      {wendoo::kPlayToneSineArgSlot, SpeakerToneWaveform::Sine},
      {wendoo::kPlayToneTriangleArgSlot, SpeakerToneWaveform::Triangle},
  };
  for (const auto& shape : shapes) {
    ToneDispatch dispatch;
    std::vector<Value> args = toneArgs();
    args[shape.slot] = kModifierPresent;
    dispatch.dispatchTile(args);
    CHECK(dispatch.lastTone().waveform == shape.waveform);
  }
}

TEST_CASE("a beep clamps its pitch and volume at the port") {
  ToneDispatch high;
  std::vector<Value> loud = toneArgs();
  loud[wendoo::kPlayToneFrequencyArgSlot] = Value::number(20000.0f);
  loud[wendoo::kPlayToneVolumeArgSlot] = Value::number(2.0f);
  high.dispatchTile(loud);
  CHECK(high.lastTone().frequencyHz == 9999.0f);
  CHECK(high.lastTone().volume == 1.0f);

  ToneDispatch quiet;
  std::vector<Value> soft = toneArgs();
  soft[wendoo::kPlayToneFrequencyArgSlot] = Value::number(440.0f);
  soft[wendoo::kPlayToneVolumeArgSlot] = Value::number(-1.0f);
  quiet.dispatchTile(soft);
  CHECK(quiet.lastTone().volume == 0.0f);
}

TEST_CASE("a 0 Hz beep crosses the port as a silent rest and still holds the lease") {
  ToneDispatch rest;
  std::vector<Value> args = toneArgs();
  args[wendoo::kPlayToneFrequencyArgSlot] = Value::number(0.0f);
  args[wendoo::kPlayToneVolumeArgSlot] = Value::number(0.5f);
  const uint32_t handleId = rest.dispatchTile(args);
  CHECK(rest.lastTone().frequencyHz == 0.0f);
  CHECK(rest.lastTone().volume == 0.0f);
  CHECK(rest.stateOf(handleId) == HandleState::Pending);

  // A negative pitch clamps onto the same silent rest.
  ToneDispatch below;
  std::vector<Value> negative = toneArgs();
  negative[wendoo::kPlayToneFrequencyArgSlot] = Value::number(-5.0f);
  below.dispatchTile(negative);
  CHECK(below.lastTone().frequencyHz == 0.0f);
  CHECK(below.lastTone().volume == 0.0f);
}

TEST_CASE("a non-finite beep argument reads as its default") {
  const float nonFinite[] = {std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity()};
  for (const float value : nonFinite) {
    for (const uint32_t slot : {wendoo::kPlayToneFrequencyArgSlot, wendoo::kPlayToneDurationArgSlot,
                                wendoo::kPlayToneVolumeArgSlot}) {
      ToneDispatch dispatch;
      std::vector<Value> args = toneArgs();
      args[slot] = Value::number(value);
      dispatch.dispatchTile(args);
      CHECK(dispatch.lastTone().waveform == SpeakerToneWaveform::Triangle);
      CHECK(dispatch.lastTone().frequencyHz == 880.0f);
      CHECK(dispatch.lastTone().durationMs == 500);
      CHECK(dispatch.lastTone().volume == 1.0f);
    }
  }
}

TEST_CASE("a beep with a negative duration sounds nothing and resolves at dispatch") {
  ToneDispatch dispatch;
  std::vector<Value> args = toneArgs();
  args[wendoo::kPlayToneDurationArgSlot] = Value::number(-1.0f);
  const uint32_t handleId = dispatch.dispatchTile(args);
  CHECK(dispatch.speaker.tones.empty());
  CHECK(dispatch.stateOf(handleId) == HandleState::Resolved);
}

TEST_CASE("a beep with a zero duration crosses the port and resolves on the first lease settle") {
  ToneDispatch dispatch;
  std::vector<Value> args = toneArgs();
  args[wendoo::kPlayToneDurationArgSlot] = Value::number(0.0f);
  const uint32_t handleId = dispatch.dispatchTile(args);
  REQUIRE(dispatch.speaker.tones.size() == 1);
  CHECK(dispatch.lastTone().durationMs == 0);
  CHECK(dispatch.stateOf(handleId) == HandleState::Pending);
  dispatch.speaker.advance(0);
  CHECK(dispatch.stateOf(handleId) == HandleState::Resolved);
}

TEST_CASE("a beep dispatched while the speaker is busy is silently dropped") {
  ToneDispatch dispatch;
  const uint32_t holderId = dispatch.dispatchTile(toneArgs());
  std::vector<Value> args = toneArgs();
  args[wendoo::kPlayToneFrequencyArgSlot] = Value::number(440.0f);
  const uint32_t droppedId = dispatch.dispatchTile(args);
  // Only the holder's tone crosses the port; the competitor's is dropped and
  // its rule continues at once.
  CHECK(dispatch.speaker.tones.size() == 1);
  CHECK(dispatch.lastTone().frequencyHz == 880.0f);
  CHECK(dispatch.stateOf(droppedId) == HandleState::Resolved);
  CHECK(dispatch.stateOf(holderId) == HandleState::Pending);
}

TEST_CASE("a beep with immediately preempts the holder at dispatch, whose handle resolves") {
  ToneDispatch dispatch;
  const uint32_t holderId = dispatch.dispatchTile(toneArgs());
  std::vector<Value> args = toneArgs();
  args[wendoo::kPlayToneFrequencyArgSlot] = Value::number(440.0f);
  args[wendoo::kPlayToneImmediatelyArgSlot] = kModifierPresent;
  const uint32_t preemptorId = dispatch.dispatchTile(args);
  CHECK(dispatch.speaker.tones.size() == 2);
  CHECK(dispatch.lastTone().frequencyHz == 440.0f);
  CHECK(dispatch.stateOf(holderId) == HandleState::Resolved);
  CHECK(dispatch.stateOf(preemptorId) == HandleState::Pending);
}

TEST_CASE("a beep in background keeps its lease while the issuing rule continues") {
  ToneDispatch dispatch;
  std::vector<Value> args = toneArgs();
  args[wendoo::kPlayToneInBackgroundArgSlot] = kModifierPresent;
  const uint32_t handleId = dispatch.dispatchTile(args);
  CHECK(dispatch.speaker.tones.size() == 1);
  CHECK(dispatch.stateOf(handleId) == HandleState::Resolved);
  // The tone still holds the speaker: a competitor dispatched now is dropped.
  const uint32_t droppedId = dispatch.dispatchTile(toneArgs());
  CHECK(dispatch.speaker.tones.size() == 1);
  CHECK(dispatch.stateOf(droppedId) == HandleState::Resolved);
}

TEST_CASE("a bare playTone call sounds the same default tone as the bare tile") {
  ToneDispatch dispatch;
  const uint32_t handleId = dispatch.dispatchHostFn(audioArgs());
  REQUIRE(dispatch.speaker.tones.size() == 1);
  CHECK(dispatch.lastTone().waveform == SpeakerToneWaveform::Triangle);
  CHECK(dispatch.lastTone().frequencyHz == 880.0f);
  CHECK(dispatch.lastTone().durationMs == 500);
  CHECK(dispatch.lastTone().volume == 1.0f);
  CHECK(dispatch.stateOf(handleId) == HandleState::Pending);
}

TEST_CASE("each playTone argument and option sets its field of the port command") {
  ToneDispatch dispatch;
  std::vector<Value> args = audioArgs();
  args[wendoo::kAudioPlayToneHostFnFrequencyArgSlot] = Value::number(262.0f);
  const Value options = dispatch.options();
  dispatch.setOption(options, wendoo::kPlayToneOptionsDurationField, Value::number(0.3f));
  dispatch.setOption(options, wendoo::kPlayToneOptionsVolumeField, Value::number(0.5f));
  dispatch.setOption(options, wendoo::kPlayToneOptionsWaveformField, dispatch.string("square"));
  args[wendoo::kAudioPlayToneHostFnOptionsArgSlot] = options;
  dispatch.dispatchHostFn(args);
  CHECK(dispatch.lastTone().waveform == SpeakerToneWaveform::Square);
  CHECK(dispatch.lastTone().frequencyHz == 262.0f);
  CHECK(dispatch.lastTone().durationMs == 300);
  CHECK(dispatch.lastTone().volume == 0.5f);
}

TEST_CASE("a playTone naming a waveform outside the sounded set is a silent no-op") {
  ToneDispatch dispatch;
  std::vector<Value> args = audioArgs();
  const Value options = dispatch.options();
  dispatch.setOption(options, wendoo::kPlayToneOptionsWaveformField, dispatch.string("bogus"));
  args[wendoo::kAudioPlayToneHostFnOptionsArgSlot] = options;
  const uint32_t handleId = dispatch.dispatchHostFn(args);
  CHECK(dispatch.speaker.tones.empty());
  CHECK(dispatch.stateOf(handleId) == HandleState::Resolved);
}

TEST_CASE("a playTone with immediately preempts before its waveform is examined") {
  ToneDispatch dispatch;
  const uint32_t holderId = dispatch.dispatchTile(toneArgs());
  std::vector<Value> args = audioArgs();
  const Value options = dispatch.options();
  dispatch.setOption(options, wendoo::kPlayToneOptionsWaveformField, dispatch.string("bogus"));
  dispatch.setOption(options, wendoo::kPlayToneOptionsImmediatelyField, Value::boolean(true));
  args[wendoo::kAudioPlayToneHostFnOptionsArgSlot] = options;
  const uint32_t handleId = dispatch.dispatchHostFn(args);
  // The unknown name sounds nothing, but the holder was preempted first.
  CHECK(dispatch.speaker.tones.size() == 1);
  CHECK(dispatch.stateOf(holderId) == HandleState::Resolved);
  CHECK(dispatch.stateOf(handleId) == HandleState::Resolved);
}

TEST_CASE("a playTone in background resolves at dispatch while the tone holds the lease") {
  ToneDispatch dispatch;
  std::vector<Value> args = audioArgs();
  const Value options = dispatch.options();
  dispatch.setOption(options, wendoo::kPlayToneOptionsInBackgroundField, Value::boolean(true));
  args[wendoo::kAudioPlayToneHostFnOptionsArgSlot] = options;
  const uint32_t handleId = dispatch.dispatchHostFn(args);
  CHECK(dispatch.speaker.tones.size() == 1);
  CHECK(dispatch.stateOf(handleId) == HandleState::Resolved);
  CHECK(dispatch.speaker.busy);
}

TEST_CASE("a playTone on an unrecognized receiver sounds nothing and resolves at dispatch") {
  ToneDispatch dispatch;
  std::vector<Value> args = audioArgs();
  args[0] = Value::structValue(static_cast<uint32_t>(MicroBitV2TypeAtomId::MicroBitDisplay), 0);
  const uint32_t handleId = dispatch.dispatchHostFn(args);
  CHECK(dispatch.speaker.tones.empty());
  CHECK(dispatch.stateOf(handleId) == HandleState::Resolved);
}

TEST_CASE("a nil or non-finite playTone option reads as its default") {
  const float nonFinite[] = {std::numeric_limits<float>::quiet_NaN(),
                             std::numeric_limits<float>::infinity(),
                             -std::numeric_limits<float>::infinity()};
  for (const float value : nonFinite) {
    for (const uint32_t field :
         {wendoo::kPlayToneOptionsDurationField, wendoo::kPlayToneOptionsVolumeField}) {
      ToneDispatch dispatch;
      std::vector<Value> args = audioArgs();
      args[wendoo::kAudioPlayToneHostFnFrequencyArgSlot] = Value::number(value);
      const Value options = dispatch.options();
      dispatch.setOption(options, field, Value::number(value));
      args[wendoo::kAudioPlayToneHostFnOptionsArgSlot] = options;
      dispatch.dispatchHostFn(args);
      CHECK(dispatch.lastTone().waveform == SpeakerToneWaveform::Triangle);
      CHECK(dispatch.lastTone().frequencyHz == 880.0f);
      CHECK(dispatch.lastTone().durationMs == 500);
      CHECK(dispatch.lastTone().volume == 1.0f);
    }
  }
}
