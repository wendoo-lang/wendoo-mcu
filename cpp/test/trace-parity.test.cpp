#include "doctest/doctest.h"

#include "codal/accelerometer-gesture.h"
#include "codal/device-port.h"
#include "codal/host-loop.h"
#include "codal/shared-type-atom-id.h"
#include "core/codec/program-reader.h"
#include "core/runtime/brain-runtime.h"
#include "core/runtime/core-host-functions.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/fiber-scheduler.h"
#include "core/runtime/host-actions/core-host-action-bindings.h"
#include "core/runtime/load-error.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/type-registry.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"
#include "device-profile-caps.h"
#include "fixture-paths.h"
#include "hostkit/observable-trace.h"
#include "string-sink.h"
#include "targets/microbit-v2/abi/display-scroll.h"
#include "targets/microbit-v2/abi/host-actions/host-action-bindings.h"
#include "targets/microbit-v2/abi/host-functions/host-func-bindings.h"
#include "targets/microbit-v2/abi/native-struct-bindings.h"
#include "targets/microbit-v2/abi/registered-structs.h"
#include "targets/microbit-v2/abi/sound-emoji.h"
#include "targets/microbit-v2/abi/type-atom-id.h"

#include <array>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using wendoo::BrainRuntime;
using wendoo::ByteSpan;
using wendoo::ErrorCode;
using wendoo::ExecutionContext;
using wendoo::FiberScheduler;
using wendoo::HostLoop;
using wendoo::kMicroBitV2TypeAtomIdCount;
using wendoo::kSharedTypeAtomIdCount;
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

namespace {

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

// Whole degrees for a radian orientation, matching CODAL's
// int getPitch() { return (int)((360.0f*radians)/(2.0f*(float)PI)); }.
int32_t degreesFromRadians(wendoo::mc_number_t radians) {
  const float twoPi = 2.0f * static_cast<float>(3.141592653589793);
  return static_cast<int32_t>((360.0f * radians) / twoPi);
}

/**
 * Host MicroBit stub over the device ports: a 5x5 pixel grid, settable
 * button levels, and a display tap that records each pixel write into the
 * observable trace before it lands.
 */
struct HostMicroBit {
  struct TracingDisplay : wendoo::PixelDisplayPort {
    ObservableTraceWriter* writer = nullptr;
    uint8_t pixels[5][5] = {};
    // The display lease, shared by the scroll and the timed draw: busy until
    // completionTime, holding active. A scroll or draw requested while busy is
    // dropped; a zero-duration draw pastes and settles without taking it.
    bool busy = false;
    float completionTime = 0;
    wendoo::AsyncHandle active{};
    // A held image-sequence draw: its frames (each row-major brightness bytes),
    // their sizes, and the playback cursor. Empty while a scroll holds the lease.
    std::vector<std::vector<uint8_t>> seqFrames;
    std::vector<uint32_t> seqWidths;
    std::vector<uint32_t> seqHeights;
    float seqStart = 0;
    uint32_t perFrameMs = 0;
    uint32_t paintedCount = 0;

    // Emit the port draw line and paste a frame top-left (no per-pixel port lines).
    void pasteFrame(const uint8_t* frame, uint32_t width, uint32_t height) {
      if (writer != nullptr) {
        writer->displayDraw(width, height, frame);
      }
      for (uint32_t row = 0; row < height && row < 5; row++) {
        for (uint32_t col = 0; col < width && col < 5; col++) {
          pixels[row][col] = frame[row * width + col];
        }
      }
    }

    void setPixel(int16_t x, int16_t y, uint8_t brightness) override {
      if (writer != nullptr) {
        writer->displaySetPixel(static_cast<float>(x), static_cast<float>(y),
                                static_cast<float>(brightness));
      }
      // The device drops a write outside the matrix (CODAL Image::setPixelValue).
      if (x >= 0 && x < 5 && y >= 0 && y < 5) {
        pixels[y][x] = brightness;
      }
    }

    // Host stub: completion is driven by the pinned formula against logical time
    // (no glyph rendering), so the resume round matches the wodal oracle. A
    // scroll requested while the lease is held is dropped (settled at once, no
    // port line).
    void scrollText(const uint8_t* bytes, uint32_t length, uint32_t delayMs, float requestTimeMs,
                    wendoo::AsyncHandle handle) override {
      if (busy) {
        handle.resolve(wendoo::kVoidValue);
        return;
      }
      if (writer != nullptr) {
        writer->displayScroll(bytes, length);
      }
      busy = true;
      completionTime =
          requestTimeMs + static_cast<float>(wendoo::scrollDurationMs(length, delayMs));
      active = handle;
    }

    // Host stub: paste a clipped frame sequence top-left (no per-pixel port
    // lines). A draw requested while the lease is held is dropped (settled at
    // once, no port line, nothing pasted); a positive per-frame duration holds
    // the lease for the whole sequence; a zero-duration draw paints only the last
    // frame and settles at dispatch without holding it.
    void drawFrames(wendoo::DrawFrameSource& frames, uint32_t perFrameDurationMs,
                    float requestTimeMs, wendoo::AsyncHandle handle) override {
      if (busy) {
        handle.resolve(wendoo::kVoidValue);
        return;
      }
      const uint32_t frameCount = frames.frameCount();
      uint8_t buf[5 * 5] = {};
      if (perFrameDurationMs == 0) {
        uint32_t width = 0;
        uint32_t height = 0;
        frames.writeFrame(frameCount - 1, buf, width, height);
        pasteFrame(buf, width, height);
        handle.resolve(wendoo::kVoidValue);
        return;
      }
      seqFrames.clear();
      seqWidths.clear();
      seqHeights.clear();
      for (uint32_t i = 0; i < frameCount; i++) {
        uint32_t width = 0;
        uint32_t height = 0;
        frames.writeFrame(i, buf, width, height);
        seqFrames.emplace_back(buf, buf + width * height);
        seqWidths.push_back(width);
        seqHeights.push_back(height);
      }
      pasteFrame(seqFrames[0].data(), seqWidths[0], seqHeights[0]);
      busy = true;
      seqStart = requestTimeMs;
      perFrameMs = perFrameDurationMs;
      paintedCount = 1;
      completionTime = requestTimeMs + static_cast<float>(frameCount * perFrameDurationMs);
      active = handle;
    }

    // Advance a held image sequence to the frame due at `now`, then resolve the
    // held scroll or sequence once its lease has elapsed. Mirrors pollDisplay.
    void advanceScroll(float now) {
      if (!busy) {
        return;
      }
      if (!seqFrames.empty()) {
        const uint32_t frameCount = static_cast<uint32_t>(seqFrames.size());
        const int64_t raw = static_cast<int64_t>((now - seqStart) / static_cast<float>(perFrameMs));
        uint32_t target = raw < 0 ? 0 : static_cast<uint32_t>(raw);
        if (target > frameCount - 1) {
          target = frameCount - 1;
        }
        while (paintedCount <= target) {
          pasteFrame(seqFrames[paintedCount].data(), seqWidths[paintedCount],
                     seqHeights[paintedCount]);
          paintedCount++;
        }
      }
      if (now < completionTime) {
        return;
      }
      busy = false;
      seqFrames.clear();
      seqWidths.clear();
      seqHeights.clear();
      active.resolve(wendoo::kVoidValue);
    }

    // Release the current lease at once, resolving the held op's handle.
    void preempt() override {
      if (!busy) {
        return;
      }
      const wendoo::AsyncHandle held = active;
      busy = false;
      seqFrames.clear();
      seqWidths.clear();
      seqHeights.clear();
      held.resolve(wendoo::kVoidValue);
    }

    // Cancel any held lease and blank the matrix (no per-pixel port lines).
    void clear() override {
      if (writer != nullptr) {
        writer->displayClear();
      }
      preempt();
      for (auto& row : pixels) {
        for (auto& pixel : row) {
          pixel = 0;
        }
      }
    }

    // Injectable ambient light level (0-255), resting at the sim model's default.
    int lightLevel = 128;

    int getLightLevel() override { return lightLevel; }
  };

  struct SettableButtons : wendoo::ButtonInputPort {
    // Index 0 is button A, 1 is button B, 2 is the touch logo.
    bool pressed[3] = {false, false, false};

    bool isPressed(uint8_t buttonIndex) override {
      return buttonIndex < 3 ? pressed[buttonIndex] : false;
    }
  };

  // Injectable accelerometer: radians are the primary orientation reading and
  // degrees derive from them, mirroring CODAL and the wodal Accelerometer model;
  // the other reads return their held field. Resting defaults are zero.
  struct SettableAccelerometer : wendoo::AccelerometerInputPort {
    int32_t gesture = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
    wendoo::mc_number_t pitchRadians = 0;
    wendoo::mc_number_t rollRadians = 0;

    uint16_t getGesture() override { return static_cast<uint16_t>(gesture); }
    int32_t getX() override { return x; }
    int32_t getY() override { return y; }
    int32_t getZ() override { return z; }
    int32_t getPitch() override { return degreesFromRadians(pitchRadians); }
    int32_t getRoll() override { return degreesFromRadians(rollRadians); }
    wendoo::mc_number_t getPitchRadians() override { return pitchRadians; }
    wendoo::mc_number_t getRollRadians() override { return rollRadians; }
  };

  // Injectable thermometer: serves the held die temperature (signed whole
  // degrees Celsius), resting at the sim model's default. Mirrors the wodal
  // Thermometer model.
  struct SettableThermometer : wendoo::ThermometerInputPort {
    int32_t temperature = 21;

    int32_t getTemperature() override { return temperature; }
  };

  // Injectable I2C bus: records each write and serves reads from a per-address
  // response a test injects, emitting the port trace line at each transaction.
  // Holds no real hardware; mirrors the wodal I2CBus sim model.
  struct TracingI2C : wendoo::I2CPort {
    struct Write {
      uint16_t address;
      std::vector<uint8_t> bytes;
    };
    ObservableTraceWriter* writer = nullptr;
    std::vector<Write> writes;
    std::map<uint16_t, std::vector<uint8_t>> readResponses;

    int write(uint16_t address, const uint8_t* data, int len) override {
      const uint32_t count = len > 0 ? static_cast<uint32_t>(len) : 0;
      if (writer != nullptr) {
        writer->i2cWrite(address, data, count);
      }
      writes.push_back({address, std::vector<uint8_t>(data, data + count)});
      return 0;
    }

    int read(uint16_t address, uint8_t* data, int len) override {
      const uint32_t count = len > 0 ? static_cast<uint32_t>(len) : 0;
      const auto it = readResponses.find(address);
      if (it == readResponses.end()) {
        // No-device read: no bytes returned.
        if (writer != nullptr) {
          writer->i2cRead(address, count, data, 0);
        }
        return 1;
      }
      const std::vector<uint8_t>& response = it->second;
      for (uint32_t i = 0; i < count; i++) {
        data[i] = i < response.size() ? response[i] : 0;
      }
      if (writer != nullptr) {
        writer->i2cRead(address, count, data, count);
      }
      return 0;
    }
  };

  // Injectable GPIO pins: records each write, pull, and servo and serves digital
  // reads from a per-pin level a test injects, emitting the port trace line at
  // each call. A pin outside 0-20 is a no-op (a read returns 0) but still traces,
  // mirroring the wodal Gpio sim model.
  struct TracingGpio : wendoo::GPIOPort {
    struct DigitalWrite {
      int pin;
      int value;
    };
    struct Pull {
      int pin;
      int mode;
    };
    struct Servo {
      int pin;
      int angle;
    };
    static constexpr int kMaxPin = 20;
    ObservableTraceWriter* writer = nullptr;
    std::vector<DigitalWrite> writes;
    std::vector<Pull> pulls;
    std::vector<Servo> servos;
    std::map<int, int> levels;
    std::map<int, int> analogLevels;

    static bool validPin(int pin) { return pin >= 0 && pin <= kMaxPin; }

    int digitalRead(int pin) override {
      int value = 0;
      if (validPin(pin)) {
        const auto it = levels.find(pin);
        value = it == levels.end() ? 0 : it->second;
      }
      if (writer != nullptr) {
        writer->gpioDigitalRead(static_cast<uint32_t>(pin), static_cast<uint32_t>(value));
      }
      return value;
    }

    int analogRead(int pin) override {
      int value = 0;
      if (validPin(pin)) {
        const auto it = analogLevels.find(pin);
        value = it == analogLevels.end() ? 0 : it->second;
      }
      if (writer != nullptr) {
        writer->gpioAnalogRead(static_cast<uint32_t>(pin), static_cast<uint32_t>(value));
      }
      return value;
    }

    int digitalWrite(int pin, int value) override {
      if (writer != nullptr) {
        writer->gpioDigitalWrite(static_cast<uint32_t>(pin), static_cast<uint32_t>(value));
      }
      if (validPin(pin)) {
        writes.push_back({pin, value});
      }
      return 0;
    }

    int setPull(int pin, int mode) override {
      if (writer != nullptr) {
        writer->gpioSetPull(static_cast<uint32_t>(pin), static_cast<uint32_t>(mode));
      }
      if (validPin(pin)) {
        pulls.push_back({pin, mode});
      }
      return 0;
    }

    int setServo(int pin, int angle) override {
      if (writer != nullptr) {
        writer->gpioServoWrite(static_cast<uint32_t>(pin), static_cast<uint32_t>(angle));
      }
      if (validPin(pin)) {
        servos.push_back({pin, angle});
      }
      return 0;
    }
  };

  // Injectable sonar driver (logical time): registers a sonar on the first
  // reference to its (trig, echo) pins, serves the cached distance, and refreshes
  // each registered sonar's cache from an injected echo width once per cycle()
  // with a fixed one-cycle lag. Mirrors the wodal SensorDriver model.
  struct TracingSonar : wendoo::SonarPort {
    static constexpr int kMaxDistanceCm = 200;
    static constexpr int kNoEcho = -1;
    struct Sonar {
      int trig;
      int echo;
      int echoMicros;
      int cache;
    };
    ObservableTraceWriter* writer = nullptr;
    std::vector<Sonar> sonars;

    static int distanceCm(int echoMicros) {
      if (echoMicros < 0) {
        return kMaxDistanceCm;
      }
      const int cm = echoMicros * 34 / 2 / 1000;
      return cm > kMaxDistanceCm ? kMaxDistanceCm : cm;
    }

    Sonar& sonarFor(int trig, int echo) {
      for (auto& s : sonars) {
        if (s.trig == trig && s.echo == echo) {
          return s;
        }
      }
      sonars.push_back({trig, echo, kNoEcho, kMaxDistanceCm});
      return sonars.back();
    }

    int distance(int trig, int echo) override {
      const int cm = sonarFor(trig, echo).cache;
      if (writer != nullptr) {
        writer->sonarDistance(static_cast<uint32_t>(trig), static_cast<uint32_t>(echo),
                              static_cast<uint32_t>(cm));
      }
      return cm;
    }

    // Inject the echo width the next cycle() measures for the (trig, echo) sonar.
    void setEchoMicros(int trig, int echo, int echoMicros) {
      sonarFor(trig, echo).echoMicros = echoMicros;
    }

    // One driver cycle: refresh each registered sonar's cache from its injected
    // echo width. Called after each think so the next think reads this cycle's
    // measurement (the one-cycle lag).
    void cycle() {
      for (auto& s : sonars) {
        s.cache = distanceCm(s.echoMicros);
      }
    }
  };

  struct TracingRadio : wendoo::RadioPort {
    struct Packet {
      int seq;
      int type;
      uint32_t group;
      wendoo::mc_number_t value;
      std::string name;
      std::string text;
      std::vector<uint8_t> bytes;
      int rssi;
      int serial;
      int time;
    };
    ObservableTraceWriter* writer = nullptr;
    std::deque<Packet> ring;
    uint8_t groupValue = 0;
    int powerValue = 6;
    int bandValue = 7;
    int lastSeq = 0;
    wendoo::RadioPacketView scratch{};

    void send(const wendoo::RadioSendView& packet) override {
      if (writer != nullptr) {
        writer->radioSend(packet.type, packet.group, packet.value, packet.name, packet.nameLen,
                          packet.text, packet.textLen, packet.bytes, packet.bytesLen);
      }
    }
    uint8_t group() override { return groupValue; }
    void setGroup(int g) override { groupValue = static_cast<uint8_t>(g); }
    void setTransmitPower(int p) override { powerValue = p; }
    void setFrequencyBand(int b) override { bandValue = b; }
    uint32_t ringSize() override { return static_cast<uint32_t>(ring.size()); }
    const wendoo::RadioPacketView& ringAt(uint32_t index) override {
      const Packet& pk = ring[index];
      scratch.seq = pk.seq;
      scratch.type = pk.type;
      scratch.group = pk.group;
      scratch.value = pk.value;
      scratch.name = reinterpret_cast<const uint8_t*>(pk.name.data());
      scratch.nameLen = static_cast<uint32_t>(pk.name.size());
      scratch.text = reinterpret_cast<const uint8_t*>(pk.text.data());
      scratch.textLen = static_cast<uint32_t>(pk.text.size());
      scratch.bytes = pk.bytes.data();
      scratch.bytesLen = static_cast<uint32_t>(pk.bytes.size());
      scratch.rssi = pk.rssi;
      scratch.serial = pk.serial;
      scratch.time = pk.time;
      return scratch;
    }
    int headSequence() override { return lastSeq; }

    // Inject a received packet (the golden-injection path): drop a wrong-group
    // packet, assign the next sequence, overflow-evict the oldest.
    void deliver(int type, wendoo::mc_number_t value, const std::string& text,
                 std::vector<uint8_t> bytes = {}) {
      if (groupValue != 0) {
        return;
      }
      lastSeq++;
      ring.push_back(Packet{lastSeq, type, 0, value, "", text, std::move(bytes), -42, 0, 0});
      if (ring.size() > RADIO_RX_RING_DEPTH) {
        ring.pop_front();
      }
    }
  };

  // Host stub speaker: the play lease is driven by the pinned nominal-duration
  // table (a tone by its own duration) against logical time. An accepted play
  // emits the port line and holds the lease to its completion; a play requested
  // while busy, an unknown name, and a negative-duration tone are settled at
  // once with no port line. Mirrors pollSpeaker.
  struct TracingSpeaker : wendoo::SpeakerPort {
    ObservableTraceWriter* writer = nullptr;
    bool busy = false;
    float completionTime = 0;
    wendoo::AsyncHandle active{};

    void playSoundEmoji(const uint8_t* name, uint32_t length, float requestTimeMs,
                        wendoo::AsyncHandle handle) override {
      if (busy) {
        handle.resolve(wendoo::kVoidValue);
        return;
      }
      uint32_t durationMs = 0;
      if (!wendoo::soundEmojiDurationMs(name, length, durationMs)) {
        handle.resolve(wendoo::kVoidValue);
        return;
      }
      if (writer != nullptr) {
        writer->speakerPlay(name, length);
      }
      busy = true;
      completionTime = requestTimeMs + static_cast<float>(durationMs);
      active = handle;
    }

    void playTone(const wendoo::SpeakerToneCommand& tone, float requestTimeMs,
                  wendoo::AsyncHandle handle) override {
      if (busy || tone.durationMs < 0) {
        handle.resolve(wendoo::kVoidValue);
        return;
      }
      if (writer != nullptr) {
        writer->speakerTone(static_cast<uint32_t>(tone.waveform), tone.frequencyHz,
                            static_cast<uint32_t>(tone.durationMs), tone.volume);
      }
      busy = true;
      completionTime = requestTimeMs + static_cast<float>(tone.durationMs);
      active = handle;
    }

    // Resolve the held play once its nominal duration has elapsed by `now`.
    void advancePlay(float now) {
      if (!busy || now < completionTime) {
        return;
      }
      busy = false;
      active.resolve(wendoo::kVoidValue);
    }

    // Release the current lease at once, resolving the held play's handle.
    void preempt() override {
      if (!busy) {
        return;
      }
      const wendoo::AsyncHandle held = active;
      busy = false;
      held.resolve(wendoo::kVoidValue);
    }
  };

  struct NullFaultDisplay : wendoo::FaultDisplayPort {
    void showFaultFace() override {}
    void scrollFaultCode(const char*) override {}
  };

  struct FixedClock : wendoo::MonotonicClockPort {
    uint32_t now = 0;

    uint32_t uptimeMillis() override { return now; }
  };

  TracingDisplay display;
  SettableButtons buttons;
  SettableAccelerometer accelerometer;
  SettableThermometer thermometer;
  TracingI2C i2c;
  TracingGpio gpio;
  TracingSonar sonar;
  TracingRadio radio;
  TracingSpeaker speaker;
  NullFaultDisplay faultDisplay;
  FixedClock clock;

  wendoo::DevicePorts ports{&display, &buttons, &faultDisplay, &clock,   &accelerometer, &i2c,
                            &gpio,    &sonar,   &radio,        &speaker, &thermometer};
};

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

/** One scheduled think: an optional button-A level applied before the time advance. */
struct ScheduleStep {
  /** Simulated milliseconds to advance before the think. */
  float advanceMs;

  /** Button A level to apply before the advance: 0 released, 1 pressed, -1 none. */
  int buttonA;
};

// Mirrors PRESS_CYCLES_SCHEDULE in wodal
// packages/wodal/src/targets/microbit-v2/wendoo/observable-trace.spec.ts,
// the generator of the committed golden trace. The two copies are kept in
// sync by hand; a divergence fails the byte comparison below.
constexpr ScheduleStep kPressCyclesSchedule[10] = {
    {16, -1}, // first eval seeds callsite state, no edge
    {16, -1}, // steady released
    {32, 1},  // press edge fires the rule
    {16, -1}, // held: edge detection does not re-trigger
    {16, -1}, // still held
    {48, 0},  // release: not reported by the default modifier
    {16, -1}, // steady released
    {32, 1},  // second press edge fires the rule again
    {16, 0},  // release again
    {16, -1}, // steady released
};

/**
 * One scheduled think for a button-sensor fixture: button/logo levels applied
 * before the time advance (1 pressed, 0 released, -1 unchanged). Mirrors the
 * ScheduleStep of wodal
 * packages/wodal/src/targets/microbit-v2/wendoo/button-sensor-trace.spec.ts.
 */
struct ButtonScheduleStep {
  float advanceMs;
  int a;
  int b;
  int logo;
};

/**
 * Loads a button-sensor fixture binary, replays its scripted button schedule
 * through the host loop, and byte-compares the rendered trace against the
 * committed golden.
 */
void runButtonSensorParity(const std::string& name, const ButtonScheduleStep* schedule, int steps) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The button sensor backs its per-callsite derivation state on a managed
  // heap; the env hands the body the button port, heap, and roots.
  wendoo::ManagedHeap heap(arena);
  wendoo::MicroBitV2ButtonSensorEnv buttonEnv{&microbit.buttons, &heap, nullptr};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, nullptr, &buttonEnv);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  buttonEnv.roots = &scheduler;
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < steps; i++) {
    const ButtonScheduleStep& step = schedule[i];
    if (step.a >= 0) {
      microbit.buttons.pressed[0] = step.a == 1;
    }
    if (step.b >= 0) {
      microbit.buttons.pressed[1] = step.b == 1;
    }
    if (step.logo >= 0) {
      microbit.buttons.pressed[2] = step.logo == 1;
    }
    const float timeMs = lastThinkTimeMs + step.advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Every button-sensor fixture's schedule fires the rule, lighting pixel (0,0).
  CHECK(microbit.display.pixels[0][0] == 255);
}

/** One scheduled think for a gesture fixture: the gesture code injected before the
 * time advance. Mirrors the ScheduleStep of wodal
 * packages/wodal/src/targets/microbit-v2/wendoo/gesture-sensor-trace.spec.ts. */
struct GestureScheduleStep {
  float advanceMs;
  wendoo::AccelerometerGesture gesture;
};

/**
 * Loads a gesture fixture binary, replays its scripted gesture schedule through
 * the host loop, and byte-compares the rendered trace against the committed
 * golden. The gesture sensor is stateless and reads the accelerometer port off
 * the device ports.
 */
void runGestureSensorParity(const std::string& name, const GestureScheduleStep* schedule,
                            int steps) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::ManagedHeap heap(arena);
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < steps; i++) {
    const GestureScheduleStep& step = schedule[i];
    microbit.accelerometer.gesture = static_cast<int32_t>(step.gesture);
    const float timeMs = lastThinkTimeMs + step.advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each gesture fixture's matching gesture fires once, lighting pixel (0,0).
  CHECK(microbit.display.pixels[0][0] == 255);
}

} // namespace

TEST_CASE("the button-display fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/button-display";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".press-cycles.trace");

  // A host scratch region comfortably larger than the test brain's demand,
  // holding the program image and the scheduler's fiber pools.
  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The button sensor backs its per-callsite derivation state on a managed
  // heap; the env hands the body the button port, heap, and roots.
  wendoo::ManagedHeap heap(arena);
  wendoo::MicroBitV2ButtonSensorEnv buttonEnv{&microbit.buttons, &heap, nullptr};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, nullptr, &buttonEnv);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  buttonEnv.roots = &scheduler;
  BrainRuntime brain(image, scheduler, surface);

  // Drive the cpp/codal host loop: it sources time through the clock port and
  // calls think() once per tick.
  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < 10; i++) {
    const ScheduleStep& step = kPressCyclesSchedule[i];
    if (step.buttonA >= 0) {
      microbit.buttons.pressed[0] = step.buttonA == 1;
    }
    const float timeMs = lastThinkTimeMs + step.advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    // Mirrors the TS harness's dt stamping: dt stays 0 until a prior think exists.
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The end state mirrors the TS spec's device assertion: pixel (0,0) lit.
  CHECK(microbit.display.pixels[0][0] == 255);
}

TEST_CASE("the button-pressed fixture byte-matches the golden observable trace") {
  const ButtonScheduleStep schedule[4] = {
      {16, -1, -1, -1}, {16, 1, -1, -1}, {16, -1, -1, -1}, {16, 0, -1, -1}};
  runButtonSensorParity("button-pressed", schedule, 4);
}

TEST_CASE("the button-released fixture byte-matches the golden observable trace") {
  const ButtonScheduleStep schedule[3] = {{16, -1, -1, -1}, {16, 1, -1, -1}, {16, 0, -1, -1}};
  runButtonSensorParity("button-released", schedule, 3);
}

TEST_CASE("the button-long-click fixture byte-matches the golden observable trace") {
  const ButtonScheduleStep schedule[4] = {
      {16, -1, -1, -1}, {16, 1, -1, -1}, {1016, -1, -1, -1}, {16, 0, -1, -1}};
  runButtonSensorParity("button-long-click", schedule, 4);
}

TEST_CASE("the button-double-click fixture byte-matches the golden observable trace") {
  const ButtonScheduleStep schedule[4] = {
      {16, -1, -1, -1}, {16, 1, -1, -1}, {16, 0, -1, -1}, {16, 1, -1, -1}};
  runButtonSensorParity("button-double-click", schedule, 4);
}

TEST_CASE("the button-held fixture byte-matches the golden observable trace") {
  const ButtonScheduleStep schedule[4] = {
      {16, -1, -1, -1}, {16, 1, -1, -1}, {16, -1, -1, -1}, {16, 0, -1, -1}};
  runButtonSensorParity("button-held", schedule, 4);
}

TEST_CASE("the button-b fixture byte-matches the golden observable trace") {
  const ButtonScheduleStep schedule[3] = {{16, -1, -1, -1}, {16, -1, 1, -1}, {16, -1, 0, -1}};
  runButtonSensorParity("button-b", schedule, 3);
}

TEST_CASE("the button-ab fixture byte-matches the golden observable trace") {
  const ButtonScheduleStep schedule[3] = {{16, -1, -1, -1}, {16, 1, -1, -1}, {16, -1, 1, -1}};
  runButtonSensorParity("button-ab", schedule, 3);
}

TEST_CASE("the button-logo fixture byte-matches the golden observable trace") {
  const ButtonScheduleStep schedule[3] = {{16, -1, -1, -1}, {16, -1, -1, 1}, {16, -1, -1, 0}};
  runButtonSensorParity("button-logo", schedule, 3);
}

TEST_CASE("the gesture-shake fixture byte-matches the golden observable trace") {
  using wendoo::AccelerometerGesture;
  const GestureScheduleStep schedule[3] = {{16, AccelerometerGesture::None},
                                           {16, AccelerometerGesture::Shake},
                                           {16, AccelerometerGesture::TiltUp}};
  runGestureSensorParity("gesture-shake", schedule, 3);
}

TEST_CASE("the gesture-tilt-up fixture byte-matches the golden observable trace") {
  using wendoo::AccelerometerGesture;
  const GestureScheduleStep schedule[3] = {{16, AccelerometerGesture::None},
                                           {16, AccelerometerGesture::TiltUp},
                                           {16, AccelerometerGesture::Shake}};
  runGestureSensorParity("gesture-tilt-up", schedule, 3);
}

TEST_CASE("the gesture-tilt-down fixture byte-matches the golden observable trace") {
  using wendoo::AccelerometerGesture;
  const GestureScheduleStep schedule[3] = {{16, AccelerometerGesture::None},
                                           {16, AccelerometerGesture::TiltDown},
                                           {16, AccelerometerGesture::TiltUp}};
  runGestureSensorParity("gesture-tilt-down", schedule, 3);
}

TEST_CASE("the gesture-tilt-left fixture byte-matches the golden observable trace") {
  using wendoo::AccelerometerGesture;
  const GestureScheduleStep schedule[3] = {{16, AccelerometerGesture::None},
                                           {16, AccelerometerGesture::TiltLeft},
                                           {16, AccelerometerGesture::TiltRight}};
  runGestureSensorParity("gesture-tilt-left", schedule, 3);
}

TEST_CASE("the gesture-tilt-right fixture byte-matches the golden observable trace") {
  using wendoo::AccelerometerGesture;
  const GestureScheduleStep schedule[3] = {{16, AccelerometerGesture::None},
                                           {16, AccelerometerGesture::TiltRight},
                                           {16, AccelerometerGesture::TiltLeft}};
  runGestureSensorParity("gesture-tilt-right", schedule, 3);
}

TEST_CASE("the gesture-face-up fixture byte-matches the golden observable trace") {
  using wendoo::AccelerometerGesture;
  const GestureScheduleStep schedule[3] = {{16, AccelerometerGesture::None},
                                           {16, AccelerometerGesture::FaceUp},
                                           {16, AccelerometerGesture::FaceDown}};
  runGestureSensorParity("gesture-face-up", schedule, 3);
}

TEST_CASE("the gesture-face-down fixture byte-matches the golden observable trace") {
  using wendoo::AccelerometerGesture;
  const GestureScheduleStep schedule[3] = {{16, AccelerometerGesture::None},
                                           {16, AccelerometerGesture::FaceDown},
                                           {16, AccelerometerGesture::FaceUp}};
  runGestureSensorParity("gesture-face-down", schedule, 3);
}

TEST_CASE("the gesture-freefall fixture byte-matches the golden observable trace") {
  using wendoo::AccelerometerGesture;
  const GestureScheduleStep schedule[3] = {{16, AccelerometerGesture::None},
                                           {16, AccelerometerGesture::Freefall},
                                           {16, AccelerometerGesture::Shake}};
  runGestureSensorParity("gesture-freefall", schedule, 3);
}

TEST_CASE("the gesture-default fixture byte-matches the golden observable trace") {
  using wendoo::AccelerometerGesture;
  const GestureScheduleStep schedule[3] = {{16, AccelerometerGesture::None},
                                           {16, AccelerometerGesture::Shake},
                                           {16, AccelerometerGesture::TiltUp}};
  runGestureSensorParity("gesture-default", schedule, 3);
}

/** One injected packet: a MakeCode packet type and its numeric, string, or byte payload. */
struct RadioInject {
  int type;
  wendoo::mc_number_t value;
  std::string text;
  std::vector<uint8_t> bytes;
};

/** One scheduled think for a radio receive fixture: packets injected before the time advance. */
struct RadioReceiveStep {
  float advanceMs;
  std::vector<RadioInject> inject;
};

/** A number packet (NUMBER) carrying `value`. */
RadioInject radioNumber(wendoo::mc_number_t value) { return RadioInject{0, value, "", {}}; }

/** A string packet (STRING) carrying `text`. */
RadioInject radioString(const std::string& text) { return RadioInject{2, 0, text, {}}; }

/** A buffer packet (BUFFER) carrying `bytes`. */
RadioInject radioBuffer(std::vector<uint8_t> bytes) {
  return RadioInject{3, 0, "", std::move(bytes)};
}

/**
 * Loads a radio-receive fixture binary, replays its injected-packet schedule
 * through the host loop, and byte-compares the rendered trace against the
 * committed golden. The typed receive sensors keep per-callsite cursors; their
 * managed string results render through the heap bound on the writer. Also
 * serves the receive-output fixtures, whose do() sends an output tile's value
 * back out through `radio send`. Mirrors the ScheduleStep of wodal
 * packages/wodal/src/targets/microbit-v2/wendoo/radio-receive-trace.spec.ts
 * and the schedules of radio-receive-output-trace.spec.ts and
 * radio-receive-output-multi-provider-trace.spec.ts.
 */
void runRadioReceiveParity(const std::string& name, const std::vector<RadioReceiveStep>& schedule) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  microbit.radio.writer = &writer;
  TraceTap tap(writer);

  // The string sensor allocates its managed string result; the writer resolves
  // it through the bound heap when rendering the action's result token. The
  // heap carries the program image so the sensors' managed output-key strings
  // compare equal to the fixtures' borrowed read keys.
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2RadioSensorEnv radioSensorEnv{&microbit.radio, &heap, nullptr};
  wendoo::MicroBitV2RadioSendEnv radioSendEnv{&microbit.radio, &heap};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, nullptr, nullptr,
                                                           nullptr, &radioSendEnv, &radioSensorEnv);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  radioSensorEnv.roots = &scheduler;
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (size_t i = 0; i < schedule.size(); i++) {
    const RadioReceiveStep& step = schedule[i];
    for (const RadioInject& packet : step.inject) {
      microbit.radio.deliver(packet.type, packet.value, packet.text, packet.bytes);
    }
    const float timeMs = lastThinkTimeMs + step.advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the radio-receive-number-single fixture byte-matches the golden observable trace") {
  runRadioReceiveParity("radio-receive-number-single",
                        {{16, {}}, {16, {radioNumber(7)}}, {16, {}}});
}

TEST_CASE("the radio-receive-number-zero fixture byte-matches the golden observable trace") {
  runRadioReceiveParity("radio-receive-number-zero", {{16, {}}, {16, {radioNumber(0)}}, {16, {}}});
}

TEST_CASE("the radio-receive-string-empty fixture byte-matches the golden observable trace") {
  runRadioReceiveParity("radio-receive-string-empty",
                        {{16, {}}, {16, {radioString("")}}, {16, {}}});
}

TEST_CASE("the radio-receive-number-burst fixture byte-matches the golden observable trace") {
  runRadioReceiveParity(
      "radio-receive-number-burst",
      {{16, {}}, {16, {radioNumber(10), radioNumber(20), radioNumber(30)}}, {16, {}}, {16, {}}});
}

TEST_CASE("the radio-receive-mixed fixture byte-matches the golden observable trace") {
  runRadioReceiveParity(
      "radio-receive-mixed",
      {{16, {}},
       {16, {radioNumber(5), radioString("hi"), radioNumber(6), radioString("yo")}},
       {16, {}},
       {16, {}}});
}

TEST_CASE("the radio-receive-both-fire fixture byte-matches the golden observable trace") {
  runRadioReceiveParity("radio-receive-both-fire", {{16, {}}, {16, {radioNumber(9)}}, {16, {}}});
}

TEST_CASE("the radio-receive-overflow fixture byte-matches the golden observable trace") {
  runRadioReceiveParity(
      "radio-receive-overflow",
      {{16, {}},
       {16, {radioNumber(1), radioNumber(2), radioNumber(3), radioNumber(4), radioNumber(5)}},
       {16, {}},
       {16, {}},
       {16, {}},
       {16, {}}});
}

TEST_CASE("the radio-receive-freshness fixture byte-matches the golden observable trace") {
  runRadioReceiveParity("radio-receive-freshness",
                        {{16, {radioNumber(99)}}, {16, {}}, {16, {radioNumber(7)}}, {16, {}}});
}

TEST_CASE("the radio-receive-buffer-single fixture byte-matches the golden observable trace") {
  runRadioReceiveParity("radio-receive-buffer-single",
                        {{16, {}}, {16, {radioBuffer({0x01, 0x00, 0xab})}}, {16, {}}});
}

TEST_CASE("the radio-receive-buffer-empty fixture byte-matches the golden observable trace") {
  runRadioReceiveParity("radio-receive-buffer-empty",
                        {{16, {}}, {16, {radioBuffer({})}}, {16, {}}});
}

TEST_CASE("the radio-receive-buffer-mixed fixture byte-matches the golden observable trace") {
  runRadioReceiveParity(
      "radio-receive-buffer-mixed",
      {{16, {}},
       {16, {radioBuffer({0x01, 0xff, 0x7f}), radioNumber(5), radioBuffer({0x0a, 0x0b})}},
       {16, {}},
       {16, {radioNumber(6)}},
       {16, {}}});
}

TEST_CASE("the radio-receive-output-string-value fixture byte-matches the golden trace") {
  runRadioReceiveParity("radio-receive-output-string-value",
                        {{16, {}}, {16, {radioString("hi")}}, {16, {}}});
}

TEST_CASE("the radio-receive-output-number-value fixture byte-matches the golden trace") {
  runRadioReceiveParity("radio-receive-output-number-value",
                        {{16, {}}, {16, {radioNumber(7)}}, {16, {}}});
}

TEST_CASE("the radio-receive-output-rssi fixture byte-matches the golden trace") {
  runRadioReceiveParity("radio-receive-output-rssi", {{16, {}}, {16, {radioNumber(7)}}, {16, {}}});
}

TEST_CASE("the radio-receive-output-multi-provider fixture byte-matches the golden trace") {
  runRadioReceiveParity("radio-receive-output-multi-provider",
                        {{16, {}}, {16, {radioNumber(7), radioString("hi")}}, {16, {}}});
}

TEST_CASE("the radio-receive-output-buffer-value fixture byte-matches the golden trace") {
  runRadioReceiveParity("radio-receive-output-buffer-value",
                        {{16, {}}, {16, {radioBuffer({0x01, 0x00, 0xab})}}, {16, {}}});
}

TEST_CASE("the radio-receive-output-buffer-rssi fixture byte-matches the golden trace") {
  runRadioReceiveParity("radio-receive-output-buffer-rssi",
                        {{16, {}}, {16, {radioBuffer({0x0a, 0x0b})}}, {16, {}}});
}

/**
 * Loads a `radio send` tile fixture binary (button when() -> radio send do()),
 * replays the scripted button schedule, and byte-compares the rendered trace.
 * Mirrors the schedule of wodal
 * packages/wodal/src/targets/microbit-v2/wendoo/radio-send-tile-trace.spec.ts.
 */
void runRadioSendTileParity(const std::string& name) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.radio.writer = &writer;
  TraceTap tap(writer);

  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2ButtonSensorEnv buttonEnv{&microbit.buttons, &heap, nullptr};
  wendoo::MicroBitV2RadioSendEnv radioSendEnv{&microbit.radio, &heap};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, nullptr, &buttonEnv,
                                                           nullptr, &radioSendEnv, nullptr);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  buttonEnv.roots = &scheduler;
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Button A pressed at tick 2, released at tick 3.
  const ButtonScheduleStep schedule[3] = {{16, -1, -1, -1}, {16, 1, -1, -1}, {16, 0, -1, -1}};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    if (schedule[i].a >= 0) {
      microbit.buttons.pressed[0] = schedule[i].a == 1;
    }
    const float timeMs = lastThinkTimeMs + schedule[i].advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the radio-send-when-result fixture byte-matches the golden observable trace") {
  runRadioSendTileParity("radio-send-when-result");
}

TEST_CASE("the radio-send-explicit fixture byte-matches the golden observable trace") {
  runRadioSendTileParity("radio-send-explicit");
}

TEST_CASE("the radio-send-position fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/radio-send-position";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.radio.writer = &writer;
  TraceTap tap(writer);

  // The inline user sensor builds a position struct directly in the send
  // slot; the compiled position -> Buffer conversion encodes it to the state
  // packet before the host radio-send action transmits.
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2ButtonSensorEnv buttonEnv{&microbit.buttons, &heap, nullptr};
  wendoo::MicroBitV2RadioSendEnv radioSendEnv{&microbit.radio, &heap};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, nullptr, &buttonEnv,
                                                           nullptr, &radioSendEnv, nullptr);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  buttonEnv.roots = &scheduler;
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Button A pressed at tick 2, released at tick 3, mirroring the schedule in
  // wodal packages/wodal/src/targets/microbit-v2/wendoo/radio-send-position.spec.ts.
  const ButtonScheduleStep schedule[3] = {{16, -1, -1, -1}, {16, 1, -1, -1}, {16, 0, -1, -1}};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    if (schedule[i].a >= 0) {
      microbit.buttons.pressed[0] = schedule[i].a == 1;
    }
    const float timeMs = lastThinkTimeMs + schedule[i].advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the user-tile radio-send fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-radio-send";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.radio.writer = &writer;
  TraceTap tap(writer);

  // The user-code actuator sends each packet form through ctx.microbit.radio;
  // the heap (with image) resolves the string and Buffer.fromHex arguments.
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2RadioEnv radioEnv{&microbit.radio, &heap};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, nullptr, nullptr,
                                                          &radioEnv, nullptr);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  microbit.clock.now = 16;
  writer.tick(1, 16, 0);
  hostLoop.tick();
  REQUIRE_FALSE(hostLoop.faulted());

  CHECK(sink.text() == golden);
}

TEST_CASE("the user-tile radio-receive fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-radio-receive";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  TraceTap tap(writer);

  // The user-code actuator drains every packet new since the cursor and echoes
  // each packet's value to I2C; the receive env builds the managed RadioPacket[]
  // and the i2c-write env surfaces the bytes.
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  wendoo::MicroBitV2RadioReceiveEnv radioReceiveEnv{&microbit.radio, &heap, nullptr, &types};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv, nullptr,
                                                          nullptr, &radioReceiveEnv);
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  radioReceiveEnv.roots = &scheduler;
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Inject 3 packets before think 2; the drain returns the whole batch then.
  const RadioReceiveStep schedule[3] = {
      {16, {}}, {16, {radioNumber(10), radioNumber(20), radioNumber(30)}}, {16, {}}};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    for (const RadioInject& packet : schedule[i].inject) {
      microbit.radio.deliver(packet.type, packet.value, packet.text);
    }
    const float timeMs = lastThinkTimeMs + schedule[i].advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the user-tile radio-current-seq fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-radio-current-seq";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  TraceTap tap(writer);

  // The user-code actuator arms its cursor to currentSeq() on the first think,
  // then drains with receive(since); the pre-arm packet is never delivered.
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  wendoo::MicroBitV2RadioEnv radioEnv{&microbit.radio, &heap};
  wendoo::MicroBitV2RadioReceiveEnv radioReceiveEnv{&microbit.radio, &heap, nullptr, &types};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv, nullptr,
                                                          &radioEnv, &radioReceiveEnv);
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  radioReceiveEnv.roots = &scheduler;
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // 99 arrives before arming (think 1, never delivered); 7 arrives after (think 2).
  const RadioReceiveStep schedule[3] = {{16, {radioNumber(99)}}, {16, {radioNumber(7)}}, {16, {}}};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    for (const RadioInject& packet : schedule[i].inject) {
      microbit.radio.deliver(packet.type, packet.value, packet.text);
    }
    const float timeMs = lastThinkTimeMs + schedule[i].advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the exceptions-yield fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/exceptions-yield";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  ExecutionContext ctx;
  // TRY/THROW/YIELD and the cross-frame CALL touch no managed heap, so the
  // scheduler runs without one - exercising the no-heap grow-on-demand path.
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // The rule yields across a round boundary, so the per-tick output differs;
  // three 16ms ticks mirror the TS oracle schedule.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the container-ops fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/container-ops";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  TraceTap tap(writer);

  // The actuator exercises the list/map opcodes, packs the computed scalars into
  // a managed buffer, and surfaces it through the writeBuffer host function; the
  // env resolves the buffer's bytes through the heap.
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/container-ops-trace.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think writes the packed list/map results to address 0x20.
  REQUIRE(microbit.i2c.writes.size() == 2);
  for (const auto& write : microbit.i2c.writes) {
    CHECK(write.address == 0x20);
    CHECK(write.bytes == std::vector<uint8_t>{3, 20, 30, 4, 1, 0});
  }
}

TEST_CASE("the buffer-ops fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/buffer-ops";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  TraceTap tap(writer);

  // The actuator constructs managed buffers through the buffer builtins and
  // surfaces each by writing it to an I2C address through the writeBuffer host
  // function; the env resolves each buffer's bytes through the heap.
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/buffer-ops-trace.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think writes the four buffers the actuator built, in address order.
  REQUIRE(microbit.i2c.writes.size() == 8);
  const std::vector<std::pair<uint16_t, std::vector<uint8_t>>> expected = {
      {0x10, {10, 20, 30}},
      {0x11, {0, 255, 127}},
      {0x12, {72, 105}},
      {0x13, {3, 20, 0, 72}},
  };
  for (size_t i = 0; i < microbit.i2c.writes.size(); i++) {
    const auto& want = expected[i % expected.size()];
    CHECK(microbit.i2c.writes[i].address == want.first);
    CHECK(microbit.i2c.writes[i].bytes == want.second);
  }
}

TEST_CASE("the dynamic-field-access fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/dynamic-field-access";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  ExecutionContext ctx;
  // The dynamic computed-key opcodes resolve field names through the type
  // registry over the decoded image; the struct slots draw from a managed heap.
  wendoo::ManagedHeap heap(arena);
  const wendoo::TypeRegistry types(image);
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the sync-action-yield fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/sync-action-yield";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  ExecutionContext ctx;
  // ACTION_CALL and the YIELD-in-sync-action fault touch no managed heap.
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // The rule yields legally across a round boundary, then a sync action yields
  // and faults; three 16ms ticks mirror the TS oracle schedule. A per-fiber
  // fault does not latch host-loop fault mode.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the action-page-lifecycle fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/action-page-lifecycle";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  ExecutionContext ctx;
  // The bytecode actions and lifecycle hooks touch no managed heap.
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  // Startup runs page 0's initializer and activation hooks before tick 1.
  REQUIRE(hostLoop.startup().isOk());

  // Page index requested before each tick; -1 leaves the current page in place.
  // Switching to page 1 then back to page 0 mirrors the TS oracle schedule.
  const int requestedPage[4] = {-1, -1, 1, 0};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 4; i++) {
    if (requestedPage[i] >= 0) {
      brain.requestPageChange(static_cast<uint32_t>(requestedPage[i]));
    }
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the context-variables fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/context-variables";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  ExecutionContext ctx;
  // Rule variables are per-rule managed maps; the heap is configured with the
  // image so the borrowed-string variable-name keys compare by content.
  wendoo::ManagedHeap heap(arena, &image);
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // The brain ignores input and runs identically each tick; advance 16ms thrice
  // to mirror the TS oracle schedule.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the rule-helper-variables fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/rule-helper-variables";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  TraceTap tap(writer);

  // The root rule's actuator calls a plain helper that reads and writes rule
  // variables through the calling rule's store, then packs both reads into a
  // managed buffer surfaced through the writeBuffer host function.
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/rule-helper-variables-trace.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think surfaces the inherited read (77) and the helper's write read back (88).
  REQUIRE(microbit.i2c.writes.size() == 2);
  for (const auto& write : microbit.i2c.writes) {
    CHECK(write.address == 0x10);
    CHECK(write.bytes == std::vector<uint8_t>{77, 88});
  }
}

TEST_CASE("the struct-closure fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/struct-closure";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  TraceTap tap(writer);

  // The actuator builds a struct and a capturing closure, packs the reads and
  // call results into a managed buffer, and surfaces it through the writeBuffer
  // host function; the env resolves the buffer's bytes through the heap.
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/struct-closure-trace.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think surfaces p.x (9), p.y (4), identity(42), and the closure capture (7).
  REQUIRE(microbit.i2c.writes.size() == 2);
  for (const auto& write : microbit.i2c.writes) {
    CHECK(write.address == 0x10);
    CHECK(write.bytes == std::vector<uint8_t>{9, 4, 42, 7});
  }
}

TEST_CASE("the helper-below-class fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/helper-below-class";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  TraceTap tap(writer);

  // The actuator constructs a class declared above a helper it calls, invokes
  // the method and the helper directly, packs the results into a managed
  // buffer, and surfaces it through the writeBuffer host function; the env
  // resolves the buffer's bytes through the heap.
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/helper-below-class-trace.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think surfaces the field read (40), the method's helper-backed sum (42),
  // and the direct helper call (2).
  REQUIRE(microbit.i2c.writes.size() == 2);
  for (const auto& write : microbit.i2c.writes) {
    CHECK(write.address == 0x10);
    CHECK(write.bytes == std::vector<uint8_t>{40, 42, 2});
  }
}

TEST_CASE("the user-tile enum-return fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-enum-return";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  TraceTap tap(writer);

  // The WHEN sensor returns a module-qualified user enum value (always
  // truthy), so the rule fires every think and the actuator surfaces a probe
  // buffer through the writeBuffer host function. Each probe byte exercises
  // one enum surface: `===`, `!==`, the enum->number conversion (stored into
  // a number variable, plus one), and the enum->string conversion (a template
  // literal compared against its expected text).
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/enum-return-trace.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think fires the enum-triggered rule once and surfaces the probe bytes.
  REQUIRE(microbit.i2c.writes.size() == 2);
  for (const auto& write : microbit.i2c.writes) {
    CHECK(write.address == 0x10);
    CHECK(write.bytes == std::vector<uint8_t>{2, 1, 3, 4});
  }
}

TEST_CASE("the user-tile button-display fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-button-display";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  // The injected context reaches the device through native struct field getters
  // (ctx.microbit, microbit.display/buttonA) and the button read / pixel write
  // dispatch through the target host-function table; both reach the same device
  // ports the host-action path uses, so the pixel write emits the same port line.
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  // STRUCT_GET_FIELD requires a heap even though native struct values carry no
  // managed slab; the brain allocates nothing on it.
  wendoo::ManagedHeap heap(arena);
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Mirrors HELD_PIXEL_SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-observable-trace.spec.ts.
  // The level-triggered rule lights the pixel on the two held ticks. -1 leaves
  // the button level unchanged before the think.
  constexpr int kButtonSchedule[4] = {-1, 1, -1, 0};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 4; i++) {
    if (kButtonSchedule[i] >= 0) {
      microbit.buttons.pressed[0] = kButtonSchedule[i] == 1;
    }
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The device ends with pixel (0,0) lit, mirroring the TS oracle assertion.
  CHECK(microbit.display.pixels[0][0] == 255);
}

TEST_CASE("the user-tile button-states fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-button-states";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The actuator reads buttonA/buttonB/logo through native struct field getters
  // and writes each level to a pixel; the three reads dispatch through the target
  // host-function table (Button.isPressed and TouchButton.isPressed share one
  // body, keyed by the receiver discriminator).
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::ManagedHeap heap(arena);
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Mirrors SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-button-states.spec.ts:
  // released, A only, A+B, then logo only. Index 0 is A, 1 is B, 2 is the logo;
  // -1 leaves a level unchanged before the think.
  const ButtonScheduleStep schedule[4] = {
      {16, -1, -1, -1}, {16, 1, -1, -1}, {16, -1, 1, -1}, {16, 0, 0, 1}};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 4; i++) {
    const ButtonScheduleStep& step = schedule[i];
    if (step.a >= 0) {
      microbit.buttons.pressed[0] = step.a == 1;
    }
    if (step.b >= 0) {
      microbit.buttons.pressed[1] = step.b == 1;
    }
    if (step.logo >= 0) {
      microbit.buttons.pressed[2] = step.logo == 1;
    }
    const float timeMs = lastThinkTimeMs + step.advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The final think touches only the logo, so pixel (2,0) ends lit and (0,0) dark.
  CHECK(microbit.display.pixels[0][2] == 1);
  CHECK(microbit.display.pixels[0][0] == 0);
}

namespace {

/** One scheduled think for the accelerometer user-tile fixture: the inputs set
 * before the time advance, each guarded by a present flag so an unset input
 * holds its last value. Mirrors the ScheduleStep of wodal
 * packages/wodal/src/targets/microbit-v2/wendoo/user-tile-accelerometer-reads.spec.ts. */
struct AccelerometerScheduleStep {
  float advanceMs;
  bool setSample;
  int x;
  int y;
  int z;
  bool setGesture;
  int gesture;
  bool setPitchRadians;
  float pitchRadians;
  bool setRollRadians;
  float rollRadians;
};

} // namespace

TEST_CASE("the user-tile accelerometer-reads fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-accelerometer-reads";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The actuator reads ctx.microbit.accelerometer.* through the native struct
  // field getter and the eight accelerometer host-function bodies, then writes
  // each value to a pixel; the reads reach the same accelerometer port the A1
  // read-back vectors exercise.
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::ManagedHeap heap(arena);
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Mirrors SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-accelerometer-reads.spec.ts:
  // tick 3 sets nothing (all reads hold), ticks 2 and 4 set only some inputs.
  const AccelerometerScheduleStep schedule[4] = {
      {16, true, 10, 20, 30, true, 11, true, 1.0f, true, 2.0f},
      {16, true, 40, 20, 30, true, 3, true, 3.0f, false, 0.0f},
      {16, false, 0, 0, 0, false, 0, false, 0.0f, false, 0.0f},
      {16, true, 40, 20, 50, true, 0, false, 0.0f, true, 0.0f},
  };
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 4; i++) {
    const AccelerometerScheduleStep& step = schedule[i];
    if (step.setSample) {
      microbit.accelerometer.x = step.x;
      microbit.accelerometer.y = step.y;
      microbit.accelerometer.z = step.z;
    }
    if (step.setGesture) {
      microbit.accelerometer.gesture = step.gesture;
    }
    if (step.setPitchRadians) {
      microbit.accelerometer.pitchRadians = step.pitchRadians;
    }
    if (step.setRollRadians) {
      microbit.accelerometer.rollRadians = step.rollRadians;
    }
    const float timeMs = lastThinkTimeMs + step.advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The final think holds x=40 at pixel (0,0) and derives pitch 171 from 3
  // radians at pixel (3,0); the gesture cleared to 0 at pixel (2,1).
  CHECK(microbit.display.pixels[0][0] == 40);
  CHECK(microbit.display.pixels[0][3] == 171);
  CHECK(microbit.display.pixels[1][2] == 0);
}

TEST_CASE("the light-level-sensor fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/light-level-sensor";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The light-level sensor is stateless and reads the display port off the
  // device ports; the set-pixel actuator lights pixel (0,0) each firing tick.
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::ManagedHeap heap(arena);
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Mirrors SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/light-level-sensor-trace.spec.ts:
  // a dark tick (0, no fire) then two distinct positive levels (each fires).
  const int schedule[3] = {0, 200, 40};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    microbit.display.lightLevel = schedule[i];
    const float timeMs = lastThinkTimeMs + 16.0f;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The two positive-level ticks fire, lighting pixel (0,0); the dark tick does not.
  CHECK(microbit.display.pixels[0][0] == 255);
}

TEST_CASE("the user-tile light-level fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-light-level";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The actuator reads ctx.microbit.display.getLightLevel() through the native
  // struct field getter and the Device-API host-function body, then writes the
  // value to a pixel; the read reaches the same display port the tile sensor does.
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::ManagedHeap heap(arena);
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Mirrors SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-light-level.spec.ts:
  // two distinct levels then a hold tick (sets nothing, the reading holds 40).
  struct Step {
    bool set;
    int lightLevel;
  };
  const Step schedule[3] = {{true, 200}, {true, 40}, {false, 0}};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    if (schedule[i].set) {
      microbit.display.lightLevel = schedule[i].lightLevel;
    }
    const float timeMs = lastThinkTimeMs + 16.0f;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The final think holds 40 at pixel (0,0).
  CHECK(microbit.display.pixels[0][0] == 40);
}

TEST_CASE("the temperature-sensor fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/temperature-sensor";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The temperature sensor is stateless and reads the thermometer port off the
  // device ports; the set-pixel actuator lights pixel (0,0) each firing tick.
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::ManagedHeap heap(arena);
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Mirrors SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/temperature-sensor-trace.spec.ts:
  // a zero tick (no fire), a positive reading (fires), and a negative reading (fires).
  const int32_t schedule[3] = {0, 22, -5};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    microbit.thermometer.temperature = schedule[i];
    const float timeMs = lastThinkTimeMs + 16.0f;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The positive and negative ticks fire, lighting pixel (0,0); the zero tick does not.
  CHECK(microbit.display.pixels[0][0] == 255);
}

TEST_CASE("the user-tile temperature fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-temperature";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The actuator reads ctx.microbit.thermometer.getTemperature() through the
  // native struct field getter and the Device-API host-function body, then writes
  // the value to a pixel; the read reaches the same thermometer port the tile
  // sensor does.
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::ManagedHeap heap(arena);
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Mirrors SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-temperature.spec.ts:
  // a positive reading, a negative reading, then a hold tick (sets nothing, holds -5).
  struct Step {
    bool set;
    int32_t temperature;
  };
  const Step schedule[3] = {{true, 22}, {true, -5}, {false, 0}};
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    if (schedule[i].set) {
      microbit.thermometer.temperature = schedule[i].temperature;
    }
    const float timeMs = lastThinkTimeMs + 16.0f;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The final think holds -5, which the display port narrows to uint8 251 at pixel (0,0).
  CHECK(microbit.display.pixels[0][0] == 251);
}

TEST_CASE("the user-tile i2c-write fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-i2c-write";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  TraceTap tap(writer);

  // The actuator reads ctx.microbit.i2c through the native struct field getter
  // and writes a Buffer.fromHex constant (a managed buffer) through the
  // writeBuffer host function; the env resolves that buffer through the heap.
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-i2c-write.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The injectable bus records the exact address and bytes the brain wrote.
  REQUIRE(microbit.i2c.writes.size() == 2);
  for (const auto& write : microbit.i2c.writes) {
    CHECK(write.address == 0x10);
    CHECK(write.bytes == std::vector<uint8_t>{1, 2, 3, 4, 5});
  }
}

TEST_CASE("the user-tile system fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-system";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  TraceTap tap(writer);

  // The System's startup-init latches count = 10; each think the rule bumps it
  // through the bump method (LOAD/STORE_SYSTEM_VAR), then the System's think
  // writes the running count to the I2C device through writeBuffer.
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Three 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-system.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The shared System state persists and increments across thinks: the bytes
  // written are the running count 11, 12, 13.
  REQUIRE(microbit.i2c.writes.size() == 3);
  for (size_t i = 0; i < microbit.i2c.writes.size(); i++) {
    CHECK(microbit.i2c.writes[i].address == 0x10);
    CHECK(microbit.i2c.writes[i].bytes == std::vector<uint8_t>{static_cast<uint8_t>(11 + i)});
  }
}

TEST_CASE("the user-tile system struct state fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-system-struct-state";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.gpio.writer = &writer;
  TraceTap tap(writer);

  // The rover System's state field holds a vec2 struct built by the StructType
  // factory; each think the rule mirrors pos.x to pin 8 and pos.y to pin 9,
  // then the System's think replaces the struct with an advanced copy. The
  // struct value lives in the GC-rooted System store across thinks.
  wendoo::ManagedHeap heap(arena, &image);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Three 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-system-struct-state.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // x climbs 1, 2, 3 across thinks while y stays 5: the struct-typed state
  // field persists in the System store and mutates think over think.
  REQUIRE(microbit.gpio.writes.size() == 6);
  for (size_t i = 0; i < microbit.gpio.writes.size(); i += 2) {
    CHECK(microbit.gpio.writes[i].pin == 8);
    CHECK(microbit.gpio.writes[i].value == static_cast<int>(1 + i / 2));
    CHECK(microbit.gpio.writes[i + 1].pin == 9);
    CHECK(microbit.gpio.writes[i + 1].value == 5);
  }
}

TEST_CASE("the user-tile i2c-read fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-i2c-read";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.i2c.writer = &writer;
  // The responder address returns three bytes; the absent address is a no-device
  // read. Mirrors the injected responses in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-i2c-read.spec.ts.
  microbit.i2c.readResponses[0x42] = {0xaa, 0xbb, 0xcc};
  TraceTap tap(writer);

  // The actuator reads ctx.microbit.i2c through the native struct field getter
  // and echoes each read (a managed Buffer the readBuffer host function
  // allocates) straight back through writeBuffer; the read body allocates its
  // buffer through the heap with the scheduler as its collection roots.
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2I2CWriteEnv i2cWriteEnv{&microbit.i2c, &heap, &image};
  wendoo::MicroBitV2I2CReadEnv i2cReadEnv{&microbit.i2c, &heap, nullptr};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs =
      wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cWriteEnv, &i2cReadEnv);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  i2cReadEnv.roots = &scheduler;
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-i2c-read.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each echoed write carries the bytes the read returned: the responder bytes
  // for the responder address, an empty buffer for the no-device read.
  REQUIRE(microbit.i2c.writes.size() == 4);
  for (size_t i = 0; i < microbit.i2c.writes.size(); i += 2) {
    CHECK(microbit.i2c.writes[i].address == 0x10);
    CHECK(microbit.i2c.writes[i].bytes == std::vector<uint8_t>{0xaa, 0xbb, 0xcc});
    CHECK(microbit.i2c.writes[i + 1].address == 0x11);
    CHECK(microbit.i2c.writes[i + 1].bytes.empty());
  }
}

TEST_CASE("the user-tile gpio fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-gpio";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.gpio.writer = &writer;
  // Inject the line-sensor level the actuator reads. Mirrors the injected level
  // in wodal packages/wodal/src/targets/microbit-v2/wendoo/user-tile-gpio.spec.ts.
  microbit.gpio.levels[13] = 1;
  TraceTap tap(writer);

  // The actuator reads ctx.microbit.gpio through the native struct field getter
  // and drives the four pin ops; the gpio bodies bind over the port in ports.
  wendoo::ManagedHeap heap(arena, &image);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-gpio.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The injectable model records the in-range writes/pull/servo and served the
  // injected read; the out-of-range pin recorded nothing.
  REQUIRE(microbit.gpio.writes.size() == 2);
  for (const auto& write : microbit.gpio.writes) {
    CHECK(write.pin == 2);
    CHECK(write.value == 1);
  }
  REQUIRE(microbit.gpio.pulls.size() == 2);
  for (const auto& pull : microbit.gpio.pulls) {
    CHECK(pull.pin == 13);
    CHECK(pull.mode == 0);
  }
  REQUIRE(microbit.gpio.servos.size() == 2);
  for (const auto& servo : microbit.gpio.servos) {
    CHECK(servo.pin == 1);
    CHECK(servo.angle == 90);
  }
}

TEST_CASE("the user-tile buffer-narrowing fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-buffer-narrowing";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.gpio.writer = &writer;
  TraceTap tap(writer);

  // The actuator stores a managed buffer and a number in rule variables, reads
  // each back as a WendooValue union, and discriminates them with
  // Buffer.isBuffer through the tag-general TYPE_CHECK op: the buffer branch
  // drives buffer.get(0) (42) onto pin 2; the number branch typeof-narrows the
  // stored 7 onto pin 3. The managed buffer is built through the buffer builtins
  // and round-trips through the rule-variable store on the managed heap.
  wendoo::ManagedHeap heap(arena, &image);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-buffer-narrowing.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think drove the narrowed buffer's first byte (42) onto pin 2 and the
  // round-tripped number (7) onto pin 3, proving Buffer.isBuffer discriminated
  // the two union values identically to the reference VM.
  REQUIRE(microbit.gpio.writes.size() == 4);
  int bufferWrites = 0;
  int numberWrites = 0;
  for (const auto& write : microbit.gpio.writes) {
    if (write.pin == 2) {
      CHECK(write.value == 42);
      bufferWrites++;
    } else {
      CHECK(write.pin == 3);
      CHECK(write.value == 7);
      numberWrites++;
    }
  }
  CHECK(bufferWrites == 2);
  CHECK(numberWrites == 2);
}

// Replays a user-tile-when-result fixture and checks that both call sites drove
// `expected` onto their pins. The rule's WHEN produces a value; an inline
// decoder sensor (pin 2) and the driver actuator's own body (pin 3) each read it
// back through ctx.getWhenResult() and drive the narrowed byte. The C++ sync
// HOST_CALL binds ctx.currentRuleFuncId from the frame before the accessor runs,
// so both sites resolve the same rule store the reference VM does.
static void checkWhenResultFixture(const std::string& name, uint32_t expected) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.gpio.writer = &writer;
  TraceTap tap(writer);

  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-when-result.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think: the inline decoder drove the narrowed WHEN result onto pin 2 and
  // the driver drove the same value onto pin 3, proving ctx.getWhenResult()
  // resolves the enclosing rule's captured result from both a sensor and an
  // actuator call site identically to the reference VM.
  REQUIRE(microbit.gpio.writes.size() == 4);
  int sensorWrites = 0;
  int actuatorWrites = 0;
  for (const auto& write : microbit.gpio.writes) {
    CHECK(write.value == expected);
    if (write.pin == 2) {
      sensorWrites++;
    } else {
      CHECK(write.pin == 3);
      actuatorWrites++;
    }
  }
  CHECK(sensorWrites == 2);
  CHECK(actuatorWrites == 2);
}

TEST_CASE("the user-tile when-result number fixture byte-matches the golden observable trace") {
  checkWhenResultFixture("user-tile-when-result-number", 42);
}

TEST_CASE("the user-tile when-result buffer fixture byte-matches the golden observable trace") {
  checkWhenResultFixture("user-tile-when-result-buffer", 55);
}

// Replays a nested user-tile-when-result fixture: a parent rule (WHEN 42) with an
// inner rule whose DO reads ctx.getWhenResult() from both an inline decoder
// sensor (pin 2) and the driver actuator body (pin 3). The inner rule fires once,
// roughly one think after the parent spawns it, so it drives one write per pin.
// Both carry `expected`: for an inner rule with its own WHEN that is its own
// captured result; for an inner rule with no WHEN condition (no captured result)
// it is the parent's result, reached by walking the ancestor chain -- the same
// rule-variable resolution the reference VM performs.
static void checkNestedWhenResultFixture(const std::string& name, uint32_t expected) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.gpio.writer = &writer;
  TraceTap tap(writer);

  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-when-result.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  REQUIRE_FALSE(microbit.gpio.writes.empty());
  bool sawSensorPin = false;
  bool sawActuatorPin = false;
  for (const auto& write : microbit.gpio.writes) {
    CHECK(write.value == expected);
    if (write.pin == 2) {
      sawSensorPin = true;
    } else {
      CHECK(write.pin == 3);
      sawActuatorPin = true;
    }
  }
  CHECK(sawSensorPin);
  CHECK(sawActuatorPin);
}

TEST_CASE("the user-tile when-result nested fixture byte-matches the golden observable trace") {
  // Inner rule produces its own number WHEN result (7); it must never read the parent's 42.
  checkNestedWhenResultFixture("user-tile-when-result-nested", 7);
}

TEST_CASE(
    "the user-tile when-result nested-empty fixture byte-matches the golden observable trace") {
  // Inner rule has no WHEN condition, so it captures no result; both inner call
  // sites walk the ancestor chain to the parent's captured WHEN result (42).
  checkNestedWhenResultFixture("user-tile-when-result-nested-empty", 42);
}

TEST_CASE("the user-tile gpio analog fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-gpio-analog";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.gpio.writer = &writer;
  // Inject the boundary axis values the actuator reads on the first think.
  // Mirrors the injection schedule in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-gpio-analog.spec.ts.
  microbit.gpio.analogLevels[1] = 0;
  microbit.gpio.analogLevels[2] = 1023;
  TraceTap tap(writer);

  // The actuator reads ctx.microbit.gpio through the native struct field getter,
  // reads both analog pins, and mirrors their sum to a digital write.
  wendoo::ManagedHeap heap(arena, &image);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Three 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-gpio-analog.spec.ts:
  // before think 2 the vertical axis moves to a mid value while the horizontal
  // axis holds its injected boundary value.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    if (i == 1) {
      microbit.gpio.analogLevels[1] = 512;
    }
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The mirrored digital writes carry each think's sum, proving the read values
  // flowed through the compiled brain.
  REQUIRE(microbit.gpio.writes.size() == 3);
  CHECK(microbit.gpio.writes[0].pin == 8);
  CHECK(microbit.gpio.writes[0].value == 1023);
  CHECK(microbit.gpio.writes[1].pin == 8);
  CHECK(microbit.gpio.writes[1].value == 1535);
  CHECK(microbit.gpio.writes[2].pin == 8);
  CHECK(microbit.gpio.writes[2].value == 1535);
}

TEST_CASE("the user-tile conversion fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-conversion";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.gpio.writer = &writer;
  TraceTap tap(writer);

  // The rule places a number literal into the actuator's buffer slot; the
  // compiler emitted the user conversion (byte recipe [7, n, n+1]) as a
  // bytecode action call, and the actuator mirrors the folded packet bytes
  // to a digital write.
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-conversion.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think's write carries the folded converted bytes [7, 42, 43] -> 74243,
  // proving the compiled convert function produced the exact packet on this VM.
  REQUIRE(microbit.gpio.writes.size() == 2);
  for (const auto& write : microbit.gpio.writes) {
    CHECK(write.pin == 8);
    CHECK(write.value == 74243);
  }
}

TEST_CASE("the user-tile struct fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-struct";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.gpio.writer = &writer;
  TraceTap tap(writer);

  // A user-declared struct flows through the brain: a sensor returns the
  // position, a struct variable stores a deep copy, one actuator receives the
  // struct through an anonymous param and packs its fields (pin 8), and a
  // brain-side accessor read feeds a number actuator (pin 9).
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-struct.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think writes the packed fields x*100+y = 304 on pin 8 and the
  // accessor-read x = 3 on pin 9, proving the struct value and its field
  // reads behave identically on this VM.
  REQUIRE(microbit.gpio.writes.size() == 4);
  for (size_t i = 0; i < microbit.gpio.writes.size(); i += 2) {
    CHECK(microbit.gpio.writes[i].pin == 8);
    CHECK(microbit.gpio.writes[i].value == 304);
    CHECK(microbit.gpio.writes[i + 1].pin == 9);
    CHECK(microbit.gpio.writes[i + 1].value == 3);
  }
}

TEST_CASE("the variable-zero-init fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/variable-zero-init";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  // The slot's starting value travels in the program image.
  REQUIRE(image.variableNames.size() == 1);
  REQUIRE(image.variableInitValues.size() == 1);
  CHECK(image.variableInitValues[0] != wendoo::kNoVariableInit);

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  wendoo::ManagedHeap heap(arena, &image);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Startup binds the slot tables. The unwritten count holds its starting value
  // of 0 before the first think, so the WHEN comparison holds from tick 1.
  REQUIRE(ctx.variables.size() == 1);
  CHECK(ctx.variables[0].tag() == wendoo::ValueTag::Number);
  CHECK(ctx.variables[0].asNumber() == 0.0f);

  // Six 16ms thinks mirror the schedule in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/variable-zero-init-trace.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 6; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The rule ran three times and stopped: count counted 0, 1, 2 and reached 3.
  CHECK(ctx.variables[0].asNumber() == 3.0f);
}

TEST_CASE("the assignment-conversion fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/assignment-conversion";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.gpio.writer = &writer;
  TraceTap tap(writer);

  // Implicit conversions surface past the port boundary: a Boolean literal
  // assigned to a Number variable (pin 9), a String literal assigned to a
  // struct's Number field (pin 10), and [not] over a Number literal whose
  // Boolean result feeds a Number arg slot (pin 11).
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/assignment-conversion-trace.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think surfaces the three converted values, proving the emitted
  // conversion calls behave identically on this VM: true -> 1 (pin 9),
  // "37" -> 37 (pin 10), and not(0) -> true -> 1 (pin 11).
  REQUIRE(microbit.gpio.writes.size() == 6);
  for (size_t i = 0; i < microbit.gpio.writes.size(); i += 3) {
    CHECK(microbit.gpio.writes[i].pin == 9);
    CHECK(microbit.gpio.writes[i].value == 1);
    CHECK(microbit.gpio.writes[i + 1].pin == 10);
    CHECK(microbit.gpio.writes[i + 1].value == 37);
    CHECK(microbit.gpio.writes[i + 2].pin == 11);
    CHECK(microbit.gpio.writes[i + 2].value == 1);
  }
}

TEST_CASE("the user-tile nested struct fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-struct-nested";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.gpio.writer = &writer;
  TraceTap tap(writer);

  // A struct-typed field flows through the brain: a sensor constructs
  // sprite {pos: position {x, y}, hp} through both factories, a struct
  // variable stores a deep copy at both levels, a chained accessor read
  // [sprite][pos][x] feeds pin 8, and a flat accessor read [sprite][hp]
  // feeds pin 9.
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-struct-nested.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Each think writes the chained read pos.x = 3 on pin 8 and the flat read
  // hp = 7 on pin 9, proving the nested struct value and its field reads
  // behave identically on this VM.
  REQUIRE(microbit.gpio.writes.size() == 4);
  for (size_t i = 0; i < microbit.gpio.writes.size(); i += 2) {
    CHECK(microbit.gpio.writes[i].pin == 8);
    CHECK(microbit.gpio.writes[i].value == 3);
    CHECK(microbit.gpio.writes[i + 1].pin == 9);
    CHECK(microbit.gpio.writes[i + 1].value == 7);
  }
}

TEST_CASE("the user-tile presence-guard fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-presence-guard";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.radio.writer = &writer;
  TraceTap tap(writer);

  // The user sensor presence-guards its optional buffer arg. Rule 0 leaves the
  // slot empty: onExecute receives nil, the guard returns undefined, and the
  // WHEN holds false. Rule 1 fills it with the [42, 7, 9] constant: the guard
  // passes and the decoded 7*256+9 flows as the WHEN-result into a bare radio
  // send. The golden pins one send of 1801 per think and no sentinel send.
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2RadioSendEnv radioSendEnv{&microbit.radio, &heap};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, nullptr, nullptr,
                                                           nullptr, &radioSendEnv, nullptr);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-presence-guard.spec.ts.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the user-tile sonar fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-sonar";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.sonar.writer = &writer;
  // Inject the echo width the background driver measures. Mirrors the injected
  // width in wodal packages/wodal/src/targets/microbit-v2/wendoo/user-tile-sonar.spec.ts.
  microbit.sonar.setEchoMicros(8, 12, 5000);
  TraceTap tap(writer);

  // The actuator reads ctx.microbit.sonar through the native struct field getter
  // and reads the same sonar twice; the sonar body binds over the port in ports.
  wendoo::ManagedHeap heap(arena, &image);
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Two 16ms thinks mirror SCHEDULE in wodal
  // packages/wodal/src/targets/microbit-v2/wendoo/user-tile-sonar.spec.ts. The
  // driver cycle runs after each think (mirroring the wodal runtime), so the next
  // think reads the previous cycle's measurement (the fixed one-cycle lag).
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 2; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    microbit.sonar.cycle();
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // The two reads per think reference the same pin pair, so the driver registers
  // exactly one sonar.
  REQUIRE(microbit.sonar.sonars.size() == 1);
  CHECK(microbit.sonar.sonars[0].trig == 8);
  CHECK(microbit.sonar.sonars[0].echo == 12);
}

TEST_CASE("the user-tile pixel-conversion fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-pixel-conversion";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The actuator writes fractional / negative / over-range / out-of-matrix values
  // through the display setPixel host-function, which narrows each to the port's
  // int16 coordinate / uint8 brightness before the write crosses the port.
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports);
  ExecutionContext ctx;
  wendoo::ManagedHeap heap(arena);
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // The brain ignores input and writes the same constants each tick; one 16ms
  // think mirrors the TS oracle schedule.
  microbit.clock.now = 16;
  writer.tick(1, 16, 0);
  hostLoop.tick();
  REQUIRE_FALSE(hostLoop.faulted());

  CHECK(sink.text() == golden);
  // The stored pixels reflect the narrowing: 7.9 -> 7, 300 -> 44, -30 -> 226, and
  // the fractional coordinate 1.9 -> column 1.
  CHECK(microbit.display.pixels[0][1] == 7);
  CHECK(microbit.display.pixels[0][2] == 44);
  CHECK(microbit.display.pixels[0][3] == 226);
  CHECK(microbit.display.pixels[1][1] == 5);
}

namespace {

/**
 * The firmware action table: the core sensor/actuator surface (ids 0-7)
 * followed by the microbit-v2 host actions. Mirrors the table assembled in
 * targets/microbit-v2/source/main.cpp.
 */
std::array<wendoo::HostActionBinding,
           wendoo::kCoreHostActionBindingCount + wendoo::kMicroBitV2HostActionBindingCount>
combineActionTable(
    const std::array<wendoo::HostActionBinding, wendoo::kCoreHostActionBindingCount>& core,
    const std::array<wendoo::HostActionBinding, wendoo::kMicroBitV2HostActionBindingCount>&
        microbit) {
  std::array<wendoo::HostActionBinding,
             wendoo::kCoreHostActionBindingCount + wendoo::kMicroBitV2HostActionBindingCount>
      table{};
  for (size_t i = 0; i < core.size(); i++) {
    table[i] = core[i];
  }
  for (size_t i = 0; i < microbit.size(); i++) {
    table[core.size() + i] = microbit[i];
  }
  return table;
}

// Loads the draw-image fixture `name`, runs `tickCount` thinks at `tickMs` each
// (settling the display lease before each think, as the device's pollDisplay
// does), and byte-compares the rendered trace against the committed golden. The
// draw env reaches the display, heap, and program; the writer's heap renders the
// Image struct argument's slots in the async dispatch line.
void checkDrawFixture(const std::string& name, int tickCount, float tickMs) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2DrawImageEnv drawEnv{&microbit.display, &heap, &image};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings =
      wendoo::makeMicroBitV2HostActionBindings(microbit.ports, nullptr, nullptr, &drawEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < tickCount; i++) {
    const float timeMs = lastThinkTimeMs + tickMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.display.advanceScroll(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

// Loads a speaker fixture `name` whose rules dispatch the async play-sound or
// play-tone host actions, replays it over `tickCount` thinks at `tickMs` each,
// and byte-compares the rendered trace against the committed golden. The
// play-sound body reads the SoundEmoji argument's name string through the
// image-backed heap; the play-tone body reads the speaker port straight off the
// device ports. The speaker lease settles before each think, as pollSpeaker does
// on device: a built-in sound on the pinned nominal-duration table, a tone on
// its own duration.
void checkSpeakerActionFixture(const std::string& name, int tickCount, float tickMs) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  microbit.speaker.writer = &writer;
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2PlaySoundEnv playSoundEnv{&microbit.speaker, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(
      microbit.ports, nullptr, nullptr, nullptr, nullptr, nullptr, &playSoundEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < tickCount; i++) {
    const float timeMs = lastThinkTimeMs + tickMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.speaker.advancePlay(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

// Loads a user-tile display fixture `name` whose async actuator awaits a
// display host function (the op-41 async ctx.microbit.display.drawImage or
// ctx.microbit.display.scrollText). Wires the native-struct receiver resolution
// and the host-function table (with the draw and scroll envs) alongside the
// core host actions (the on-page-entered sensor), runs `tickCount` thinks at
// `tickMs` each (settling the display lease before each think, as the device's
// pollDisplay does), and byte-compares the rendered trace against the committed
// golden.
void checkUserTileDrawFixture(const std::string& name, int tickCount, float tickMs) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2DrawImageEnv drawEnv{&microbit.display, &heap, &image};
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, &drawEnv, nullptr,
                                                          nullptr, nullptr, nullptr, &scrollEnv);
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  auto registeredStructs = wendoo::makeMicroBitV2RegisteredStructSlotCounts();
  types.setRegisteredStructSlotCounts({registeredStructs.data(), registeredStructs.size()});
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < tickCount; i++) {
    const float timeMs = lastThinkTimeMs + tickMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.display.advanceScroll(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

// Loads a user-tile speaker fixture `name` whose async actuators await the
// op-41 async ctx.microbit.audio.playSound or ctx.microbit.audio.playTone host
// function. Wires the native-struct receiver resolution and the host-function
// table (with the play-sound and play-tone envs) alongside the core host actions
// (the on-page-entered sensor), runs `tickCount` thinks at `tickMs` each,
// settling the speaker lease before each think as the device's pollSpeaker does,
// and byte-compares the rendered trace against the committed golden. Every play
// the fixture dispatches must last longer than `tickMs`.
void checkUserTileSpeakerFixture(const std::string& name, int tickCount, float tickMs) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  microbit.speaker.writer = &writer;
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::MicroBitV2PlaySoundEnv playSoundEnv{&microbit.speaker, &heap};
  wendoo::MicroBitV2PlayToneEnv playToneEnv{&microbit.speaker, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  auto hostFuncs =
      wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, nullptr, nullptr, nullptr,
                                             nullptr, nullptr, &playSoundEnv, &playToneEnv);
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  auto registeredStructs = wendoo::makeMicroBitV2RegisteredStructSlotCounts();
  types.setRegisteredStructSlotCounts({registeredStructs.data(), registeredStructs.size()});
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < tickCount; i++) {
    const float timeMs = lastThinkTimeMs + tickMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.speaker.advancePlay(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

} // namespace

TEST_CASE("the timer-brain fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/timer-brain";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The core sensor/actuator surface reaches the brain, RNG, and heap; the
  // timeout sensor's per-callsite state list draws from the managed heap.
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Four 600ms thinks mirror the TS oracle schedule: the timeout fires on tick
  // 3 and switches to page 2, which lights pixel (0,0) on tick 4.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 4; i++) {
    const float timeMs = lastThinkTimeMs + 600;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  CHECK(microbit.display.pixels[0][0] == 255);
}

TEST_CASE("the timeout-bounce fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/timeout-bounce";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Ten 600ms thinks mirror the TS oracle schedule: page 0's timeout fires and
  // switches to page 1, whose own timeout must re-arm and fire to switch back --
  // the pages bounce, so each page's timeout fires across the run.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 10; i++) {
    const float timeMs = lastThinkTimeMs + 600;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("a multi-rule page switching mid-round runs fault-free") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/multi-rule-page-switch";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Each think, rule 0 switches pages while its sibling rule 1 is still queued,
  // exercising two regressions: the active-page root-rule buffer is allocated
  // once (a per-activation allocation would leak the bump region and fault), and
  // the switch's cancellation of the queued sibling drains the round's queue
  // (the scheduler must stop the round when it empties).
  bool faulted = false;
  for (int i = 0; i < 20000; i++) {
    microbit.clock.now = static_cast<uint32_t>(i + 1);
    hostLoop.tick();
    if (hostLoop.faulted()) {
      faulted = true;
      break;
    }
  }
  CHECK_FALSE(faulted);
}

TEST_CASE("the restart-interrupt fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/restart-interrupt";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Three 600ms thinks mirror the TS oracle schedule: tick 1 switches to the
  // current page (a restart), which cancels the rule before its pixel write, so
  // the write is abandoned and the pixel never lights.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    const float timeMs = lastThinkTimeMs + 600;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  CHECK(microbit.display.pixels[0][0] == 0);
}

TEST_CASE("the core-host-actions fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/core-host-actions";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The switch-page-by-id actuator resolves the borrowed page-id string through
  // the heap configured with the image.
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Three 600ms thinks mirror the TS oracle schedule: page 0 reads page state,
  // yields, and switches by page id on tick 1; page 1 reads page state and
  // restarts itself on ticks 2 and 3.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    const float timeMs = lastThinkTimeMs + 600;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the display-scroll fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/display-scroll";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The rule's on-page-entered trigger is a core action; the async scroll and
  // set-pixel are microbit actions. The scroll body reads its borrowed text
  // string through the image-backed heap; the scroll env hands the body that
  // heap and the display port.
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, &scrollEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Four 1100ms thinks mirror the wodal display-scroll oracle schedule: the
  // scroll dispatches async on tick 1, its handle resolves once the pinned
  // completion time passes, and the rule resumes and lights pixel (0,0) on tick
  // 4. The scroll completion settles the handle out of band before each think,
  // as the CODAL animation-complete event does on device; the think then drains
  // it and resumes the waiter on the next round.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 4; i++) {
    const float timeMs = lastThinkTimeMs + 1100;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.display.advanceScroll(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  CHECK(microbit.display.pixels[0][0] == 255);
}

// Replays a scroll-when-result fixture over one 1100ms think and byte-compares
// its rendered trace. The single rule has a value-producing WHEN side and a DO
// that scrolls with no explicit text argument, so the scroll falls back to the
// captured __whenResult: a numeric WHEN scrolls its number, a boolean WHEN keeps
// the default text.
static void checkScrollWhenResultFixture(const std::string& name) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, &scrollEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // One 1100ms think mirrors the wodal schedule: the scroll dispatches async on
  // tick 1 and the rule parks awaiting the animation.
  const float timeMs = 1100;
  microbit.clock.now = static_cast<uint32_t>(timeMs);
  writer.tick(1, timeMs, 0);
  microbit.display.advanceScroll(timeMs);
  hostLoop.tick();
  REQUIRE_FALSE(hostLoop.faulted());

  CHECK(sink.text() == golden);
}

TEST_CASE("the scroll-when-result fixture byte-matches the golden observable trace") {
  checkScrollWhenResultFixture("scroll-when-result");
}

TEST_CASE("the scroll-when-result-bool fixture byte-matches the golden observable trace") {
  checkScrollWhenResultFixture("scroll-when-result-bool");
}

TEST_CASE("the draw-image-forget fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-forget", 3, 100.0f);
}

TEST_CASE("the draw-image-timed fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-timed", 5, 100.0f);
}

TEST_CASE("the draw-image-dropped fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-dropped", 5, 100.0f);
}

TEST_CASE("the draw-image-defaults fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-defaults", 4, 600.0f);
}

TEST_CASE("the draw-image-preempt fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-preempt", 3, 100.0f);
}

TEST_CASE("the draw-image-background fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-background", 8, 1100.0f);
}

TEST_CASE(
    "the draw-image-background-immediately fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-background-immediately", 5, 1100.0f);
}

TEST_CASE("the draw-image-builtins fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-builtins", 2, 100.0f);
}

TEST_CASE("the draw-image-sequence fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-sequence", 8, 100.0f);
}

TEST_CASE("the draw-image-sequence-dropped fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-sequence-dropped", 5, 100.0f);
}

TEST_CASE("the draw-image-sequence-preempt fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-sequence-preempt", 3, 100.0f);
}

TEST_CASE("the draw-image-sequence-compiled fixture byte-matches the golden observable trace") {
  checkDrawFixture("draw-image-sequence-compiled", 5, 100.0f);
}

TEST_CASE("the play-sound-awaited fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-sound-awaited", 3, 1100.0f);
}

TEST_CASE("the play-sound-dropped fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-sound-dropped", 4, 1100.0f);
}

TEST_CASE("the play-sound-preempt fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-sound-preempt", 4, 1100.0f);
}

TEST_CASE("the play-sound-background fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-sound-background", 8, 1100.0f);
}

TEST_CASE("the play-sound-dropped-background fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-sound-dropped-background", 9, 1100.0f);
}

TEST_CASE("the play-tone-defaults fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-tone-defaults", 7, 100.0f);
}

TEST_CASE("the play-tone-waveforms fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-tone-waveforms", 9, 100.0f);
}

TEST_CASE("the play-tone-arguments fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-tone-arguments", 4, 100.0f);
}

TEST_CASE("the play-tone-rest-sequence fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-tone-rest-sequence", 10, 100.0f);
}

TEST_CASE("the play-tone-dropped fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-tone-dropped", 7, 100.0f);
}

TEST_CASE("the play-tone-preempt fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-tone-preempt", 7, 100.0f);
}

TEST_CASE("the play-tone-background fixture byte-matches the golden observable trace") {
  checkSpeakerActionFixture("play-tone-background", 4, 100.0f);
}

TEST_CASE("the user-tile-draw-timed fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("user-tile-draw-timed", 5, 100.0f);
}

TEST_CASE("the user-tile-draw-forget fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("user-tile-draw-forget", 2, 100.0f);
}

TEST_CASE("the user-tile-draw-icon fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("user-tile-draw-icon", 2, 100.0f);
}

TEST_CASE("the user-tile-scroll-awaited fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("user-tile-scroll-awaited", 4, 1100.0f);
}

TEST_CASE("the user-tile-scroll-glyph fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("user-tile-scroll-glyph", 4, 1100.0f);
}

TEST_CASE(
    "the user-tile-display-draw-preempts-scroll fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("user-tile-display-draw-preempts-scroll", 6, 500.0f);
}

TEST_CASE(
    "the user-tile-display-scroll-preempts-draw fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("user-tile-display-scroll-preempts-draw", 6, 500.0f);
}

TEST_CASE(
    "the user-tile-display-draw-background fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("user-tile-display-draw-background", 4, 500.0f);
}

TEST_CASE(
    "the user-tile-display-scroll-background fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("user-tile-display-scroll-background", 4, 500.0f);
}

TEST_CASE("the user-tile-play-sound fixture byte-matches the golden observable trace") {
  checkUserTileSpeakerFixture("user-tile-play-sound", 5, 500.0f);
}

TEST_CASE("the user-tile-play-sound-unknown fixture byte-matches the golden observable trace") {
  checkUserTileSpeakerFixture("user-tile-play-sound-unknown", 2, 500.0f);
}

TEST_CASE("the user-tile-play-sound-all fixture byte-matches the golden observable trace") {
  checkUserTileSpeakerFixture("user-tile-play-sound-all", 86, 500.0f);
}

TEST_CASE("the user-tile-play-sound-preempt fixture byte-matches the golden observable trace") {
  checkUserTileSpeakerFixture("user-tile-play-sound-preempt", 5, 500.0f);
}

TEST_CASE("the user-tile-play-sound-background fixture byte-matches the golden observable trace") {
  checkUserTileSpeakerFixture("user-tile-play-sound-background", 4, 500.0f);
}

TEST_CASE("the user-tile-play-tone fixture byte-matches the golden observable trace") {
  checkUserTileSpeakerFixture("user-tile-play-tone", 12, 100.0f);
}

TEST_CASE("the display-clear-tile fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("display-clear-tile", 2, 100.0f);
}

TEST_CASE("the user-tile-display-clear fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("user-tile-display-clear", 2, 100.0f);
}

TEST_CASE("the display-clear-preempts fixture byte-matches the golden observable trace") {
  checkUserTileDrawFixture("display-clear-preempts", 3, 100.0f);
}

TEST_CASE("the display-scroll-drop fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/display-scroll-drop";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, &scrollEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Four 1100ms thinks: the holder scrolls "hi" and takes the lease on tick 1,
  // the competitor's "yo" is dropped (no port line) and resumes in the same
  // think, and the holder resumes and lights pixel (0,0) on tick 4.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 4; i++) {
    const float timeMs = lastThinkTimeMs + 1100;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.display.advanceScroll(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  CHECK(microbit.display.pixels[0][0] == 255);
}

TEST_CASE("the display-scroll-background fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/display-scroll-background";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, &scrollEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Eight 1100ms thinks: the parent rule scrolls with the in-background modifier
  // and takes the lease on tick 1; its handle resolves at dispatch, so the parent
  // does not park and its child rule (SPAWN_RULE at the tail) lights a pixel on
  // the next think, while the scroll animation still holds the lease.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 8; i++) {
    const float timeMs = lastThinkTimeMs + 1100;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.display.advanceScroll(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the async-action fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/async-action";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The rule awaits a bytecode action (ACTION_CALL_ASYNC) and then reads the
  // current page id, a device-free core action; no microbit ports are written.
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Four 600ms thinks: tick 1 dispatches the async action and parks, tick 2 runs
  // the child action body, tick 3 resumes the rule (which reads the page id).
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 4; i++) {
    const float timeMs = lastThinkTimeMs + 600;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  // Device-free: no pixel is ever written.
  CHECK(microbit.display.pixels[0][0] == 0);
}

TEST_CASE("the sibling-rule-fibers fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/sibling-rule-fibers";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The root rule spawns two child rules (SPAWN_RULE); each awaits a bytecode
  // action (ACTION_CALL_ASYNC) and surfaces a set-pixel before and after.
  wendoo::ManagedHeap heap(arena, &image);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {mbBindings.data(), mbBindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Six 16ms thinks mirror the wodal schedule: both children dispatch their
  // async action in the same think and resume together a later think.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 6; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the parent-quiesce fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/parent-quiesce";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The parent rule lights a marker pixel and spawns one child rule that awaits
  // a bytecode action; the parent must not re-fire while the child is in flight.
  wendoo::ManagedHeap heap(arena, &image);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {mbBindings.data(), mbBindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < 8; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the async-parent-sequencing fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/async-parent-sequencing";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The parent rule fires once on page entry, scrolls (async), and parks; its
  // child rule lights a pixel after the scroll resolves (SPAWN_RULE at the tail).
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, &scrollEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < 12; i++) {
    const float timeMs = lastThinkTimeMs + 1100;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.display.advanceScroll(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the async-handle-backpressure fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/async-handle-backpressure";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // A parent rule fires once on page entry, lights a pixel, and spawns eight
  // child rules that each scroll (async) in the same think. Under a four-handle
  // cap the breadth backpressures and spills across thinks in bounded waves.
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, &scrollEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena,
                           wendoo::test::withMaxHandles(wendoo::test::kDeviceProfileCaps, 4));
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < 9; i++) {
    const float timeMs = lastThinkTimeMs + 1100;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.display.advanceScroll(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the non-firing-parent fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/non-firing-parent";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // A control rule lights a pixel every think; a parent rule gated on button A
  // (never pressed) has a child rule that scrolls. The parent never fires, so
  // its child never runs and no scroll crosses the trace.
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  wendoo::MicroBitV2ButtonSensorEnv buttonEnv{&microbit.buttons, &heap, nullptr};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, &scrollEnv, &buttonEnv);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, &tap, &heap};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  buttonEnv.roots = &scheduler;
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  // Three 16ms thinks with the button never pressed.
  float lastThinkTimeMs = 0;
  for (int i = 0; i < 3; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the sync-cascade-depth-first fixture byte-matches the golden observable trace") {
  const std::string base =
      std::string(wendoo::test::kWodalFixturesDir) + "/sync-cascade-depth-first";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // A synchronous parent -> child -> grandchild plus a second child, all lighting
  // pixels in one think in depth-first order (x = 0, 1, 2, 3).
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < 4; i++) {
    const float timeMs = lastThinkTimeMs + 1000;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the mixed-sync-async-child fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/mixed-sync-async-child";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // A synchronous parent lights a pixel; its synchronous child drains same-think,
  // while its asynchronous child parks on a scroll and its grandchild lights a
  // pixel only after the scroll completes (cross-think).
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, &scrollEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < 12; i++) {
    const float timeMs = lastThinkTimeMs + 1100;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.display.advanceScroll(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

TEST_CASE("the pixel-conversion fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/pixel-conversion";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The rule writes pixels with valid, fractional, and out-of-range coordinates
  // and over-bright/fractional brightness; the pinned f32->u8 conversion at the
  // port must discard the same writes and clamp the same values as the wodal
  // oracle.
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  microbit.clock.now = 600;
  writer.tick(1, 600, 0);
  hostLoop.tick();
  REQUIRE_FALSE(hostLoop.faulted());

  CHECK(sink.text() == golden);
  // The fractional x truncated to 1 so pixel (1, 2) is lit; the out-of-matrix
  // coordinate stored nothing; the wrapped and truncated brightnesses landed.
  CHECK(microbit.display.pixels[2][1] == 255);
  CHECK(microbit.display.pixels[0][0] == 44);
  CHECK(microbit.display.pixels[1][1] == 100);
}

TEST_CASE("the opcode-coverage fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/opcode-coverage";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The rule exercises the list-mutation and stack opcodes no other golden
  // reaches, then writes a pixel reading back the final list state. The managed
  // list lives on the heap.
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  microbit.clock.now = 600;
  writer.tick(1, 600, 0);
  hostLoop.tick();
  REQUIRE_FALSE(hostLoop.faulted());

  CHECK(sink.text() == golden);
  // The list mutations leave list = [1], lighting pixel (1, 1).
  CHECK(microbit.display.pixels[1][1] == 255);
}

TEST_CASE("the managed-string-scroll fixture byte-matches the golden observable trace") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/managed-string-scroll";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  TraceTap tap(writer);

  // The scrolled text is a managed string built by the string-concat host
  // function; the scroll body reads its bytes from the heap, the same path the
  // borrowed-string scroll exercises.
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::ManagedHeap heap(arena, &image);
  // The async scroll arg is a managed string; the trace writer resolves its
  // bytes through the heap.
  writer.setHeap(&heap);
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, &scrollEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < 4; i++) {
    const float timeMs = lastThinkTimeMs + 1100;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.display.advanceScroll(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
  CHECK(microbit.display.pixels[0][0] == 255);
}

/**
 * Regression: a heap collection during a System think must skip native struct
 * values. The user-tile system fixture's think holds the injected context and
 * the I2C receiver on its operand stack at exactly the allocation that
 * collects once the arena tightens; tracing them as heap objects reads and
 * writes unrelated arena memory. A small arena forces collections within the
 * run.
 */
TEST_CASE("a heap collection during a System think skips native receiver values") {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/user-tile-system";
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");

  std::vector<uint8_t> arenaStorage(16 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  HostMicroBit microbit;
  wendoo::ManagedHeap heap(arena, &image);
  wendoo::MicroBitV2I2CWriteEnv i2cEnv{&microbit.i2c, &heap, &image};
  auto bindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports);
  auto hostFuncs = wendoo::makeMicroBitV2HostFuncBindings(microbit.ports, nullptr, &i2cEnv);
  ExecutionContext ctx;
  wendoo::TypeRegistry types(image);
  auto nativeStructs = wendoo::makeMicroBitV2NativeStructBindings(types);
  types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
  RuntimeSurface surface{&ctx, {bindings.data(), bindings.size()}, nullptr, &heap};
  surface.types = &types;
  surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  BrainRuntime brain(image, scheduler, surface);

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (int i = 0; i < 500; i++) {
    const float timeMs = lastThinkTimeMs + 16;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  // The run long outlives what uncollected garbage would fit, so collections
  // happened; the System's think kept writing one buffer per think throughout,
  // and the heap's arena footprint stayed at its working set, leaving room for
  // later fiber growth.
  CHECK(arena.bytesRemaining() > 4096);
  CHECK(microbit.i2c.writes.size() == 500);
}

namespace {

/** One scheduled think of a trigger-mode fixture: button levels, injected packets, then the
 * advance. */
struct TriggerModeStep {
  /** Simulated milliseconds to advance before the think. */
  float advanceMs;
  /** Button A level: 1 down, 0 up, -1 unchanged. */
  int a;
  /** Button B level: 1 down, 0 up, -1 unchanged. */
  int b;
  /** Packets delivered into the receive ring before the think. */
  std::vector<RadioInject> inject;
};

/**
 * Loads a rule trigger-mode fixture binary, replays its schedule through the
 * host loop, and byte-compares the rendered trace against the committed golden.
 * The brains reach the core sensor table (the `otherwise` sensor, the
 * rule-trigger action, and the page-switch actuator) plus the microbit buttons,
 * radio, display scroll, and set-pixel. Mirrors the schedules of wodal
 * packages/wodal/src/targets/microbit-v2/wendoo/rule-trigger-modes-trace.spec.ts.
 */
void runTriggerModeParity(const std::string& name, const std::vector<TriggerModeStep>& schedule) {
  const std::string base = std::string(wendoo::test::kWodalFixturesDir) + "/" + name;
  const std::vector<uint8_t> wire = readBinaryFile(base + ".mcprogram.bin");
  const std::string golden = readTextFile(base + ".ticks.trace");

  std::vector<uint8_t> arenaStorage(64 * 1024);
  RegionArena arena(Span<uint8_t>(arenaStorage.data(), arenaStorage.size()));
  constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
  const Result<ProgramImage, LoadError> decoded =
      readProgramImage(ByteSpan(wire.data(), wire.size()), arena, options);
  REQUIRE(decoded.isOk());
  const ProgramImage& image = decoded.value();

  StringTextSink sink;
  ObservableTraceWriter writer(sink, image);
  HostMicroBit microbit;
  microbit.display.writer = &writer;
  microbit.radio.writer = &writer;
  TraceTap tap(writer);

  wendoo::ManagedHeap heap(arena, &image);
  writer.setHeap(&heap);
  wendoo::CoreHostActionEnv coreEnv;
  wendoo::VmRng rng;
  wendoo::MicroBitV2DisplayScrollEnv scrollEnv{&microbit.display, &heap};
  wendoo::MicroBitV2ButtonSensorEnv buttonEnv{&microbit.buttons, &heap, nullptr};
  wendoo::MicroBitV2RadioSensorEnv radioSensorEnv{&microbit.radio, &heap, nullptr};
  auto coreBindings = wendoo::makeCoreHostActionBindings(coreEnv);
  auto mbBindings = wendoo::makeMicroBitV2HostActionBindings(microbit.ports, &scrollEnv, &buttonEnv,
                                                             nullptr, nullptr, &radioSensorEnv);
  auto actions = combineActionTable(coreBindings, mbBindings);
  ExecutionContext ctx;
  RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, &tap, &heap};
  surface.rng = &rng;

  FiberScheduler scheduler(image, surface, arena, wendoo::test::kDeviceProfileCaps);
  buttonEnv.roots = &scheduler;
  radioSensorEnv.roots = &scheduler;
  BrainRuntime brain(image, scheduler, surface);
  coreEnv.brain = &brain;
  coreEnv.rng = &rng;
  coreEnv.heap = &heap;
  coreEnv.roots = &scheduler;
  coreEnv.program = &image;
  coreEnv.ruleLiveness = &scheduler;

  HostLoop hostLoop(brain, microbit.ports);
  REQUIRE(hostLoop.startup().isOk());

  float lastThinkTimeMs = 0;
  for (size_t i = 0; i < schedule.size(); i++) {
    const TriggerModeStep& step = schedule[i];
    if (step.a >= 0) {
      microbit.buttons.pressed[0] = step.a == 1;
    }
    if (step.b >= 0) {
      microbit.buttons.pressed[1] = step.b == 1;
    }
    for (const RadioInject& packet : step.inject) {
      microbit.radio.deliver(packet.type, packet.value, packet.text, packet.bytes);
    }
    const float timeMs = lastThinkTimeMs + step.advanceMs;
    microbit.clock.now = static_cast<uint32_t>(timeMs);
    writer.tick(static_cast<uint32_t>(i + 1), timeMs,
                lastThinkTimeMs == 0 ? 0 : timeMs - lastThinkTimeMs);
    microbit.display.advanceScroll(timeMs);
    hostLoop.tick();
    REQUIRE_FALSE(hostLoop.faulted());
    lastThinkTimeMs = timeMs;
  }

  CHECK(sink.text() == golden);
}

/** The twelve-think scroll schedule the parked-subject `then` fixtures share:
 *  button A goes down on think 2 and back up on think 5. */
std::vector<TriggerModeStep> parkedSubjectSchedule() {
  std::vector<TriggerModeStep> schedule;
  for (int i = 0; i < 12; i++) {
    const int a = i == 1 ? 1 : (i == 4 ? 0 : -1);
    schedule.push_back(TriggerModeStep{1100, a, -1, {}});
  }
  return schedule;
}

/** The six-think schedule the filtered `then` fixtures share: button A goes down
 *  on think 2 and back up on think 6, button B alternates over thinks 3 to 5. */
std::vector<TriggerModeStep> filteredThenSchedule() {
  return {{16, -1, -1, {}}, {16, 1, -1, {}}, {16, -1, 1, {}},
          {16, -1, 0, {}},  {16, -1, 1, {}}, {16, 0, -1, {}}};
}

/** The five-think schedule the fast-cadence pair fixtures share: button A goes
 *  down on think 2 and back up on think 4. */
std::vector<TriggerModeStep> fastPairSchedule() {
  return {{16, -1, -1, {}}, {16, 1, -1, {}}, {16, -1, -1, {}}, {16, 0, -1, {}}, {16, -1, -1, {}}};
}

} // namespace

TEST_CASE("the otherwise-chain-ladder fixture byte-matches the golden observable trace") {
  runTriggerModeParity(
      "otherwise-chain-ladder",
      {{16, -1, -1, {}}, {16, 1, -1, {}}, {16, 0, 1, {}}, {16, 1, -1, {}}, {16, 0, 0, {}}});
}

TEST_CASE("the otherwise-chain-presence-gated fixture byte-matches the golden observable trace") {
  runTriggerModeParity("otherwise-chain-presence-gated", {{16, -1, -1, {}},
                                                          {16, -1, -1, {radioNumber(0)}},
                                                          {16, -1, -1, {}},
                                                          {16, -1, -1, {radioNumber(9)}},
                                                          {16, 1, -1, {}}});
}

TEST_CASE("the otherwise-chain-parked-head fixture byte-matches the golden observable trace") {
  std::vector<TriggerModeStep> schedule;
  for (int i = 0; i < 10; i++) {
    const int a = i == 1 ? 1 : (i == 4 ? 0 : -1);
    schedule.push_back(TriggerModeStep{1100, a, i == 0 ? 1 : -1, {}});
  }
  runTriggerModeParity("otherwise-chain-parked-head", schedule);
}

TEST_CASE("the otherwise-chain-then-head fixture byte-matches the golden observable trace") {
  runTriggerModeParity("otherwise-chain-then-head", parkedSubjectSchedule());
}

TEST_CASE("the otherwise-chain-migrated-pair fixture byte-matches the golden observable trace") {
  runTriggerModeParity("otherwise-chain-migrated-pair", fastPairSchedule());
}

TEST_CASE("the then-after-childless-sibling fixture byte-matches the golden observable trace") {
  runTriggerModeParity("then-after-childless-sibling", fastPairSchedule());
}

TEST_CASE("the then-silent-sibling fixture byte-matches the golden observable trace") {
  runTriggerModeParity("then-silent-sibling",
                       {{16, -1, -1, {}}, {16, -1, -1, {}}, {16, -1, -1, {}}});
}

TEST_CASE("the then-after-empty-do-sibling fixture byte-matches the golden observable trace") {
  runTriggerModeParity("then-after-empty-do-sibling",
                       {{16, -1, -1, {}}, {16, 1, -1, {}}, {16, -1, -1, {}}, {16, 0, -1, {}}});
}

TEST_CASE("the then-after-parked-child fixture byte-matches the golden observable trace") {
  runTriggerModeParity("then-after-parked-child", parkedSubjectSchedule());
}

TEST_CASE("the then-filtered fixture byte-matches the golden observable trace") {
  runTriggerModeParity("then-filtered", filteredThenSchedule());
}

TEST_CASE("the then-chain-root fixture byte-matches the golden observable trace") {
  runTriggerModeParity("then-chain-root", parkedSubjectSchedule());
}

TEST_CASE("the then-chain-child fixture byte-matches the golden observable trace") {
  runTriggerModeParity("then-chain-child",
                       {{16, -1, -1, {}}, {16, 1, -1, {}}, {16, -1, -1, {}}, {16, 0, -1, {}}});
}

TEST_CASE("the then-chain-skipped-middle fixture byte-matches the golden observable trace") {
  runTriggerModeParity("then-chain-skipped-middle", filteredThenSchedule());
}

TEST_CASE("the then-chain-parked-links fixture byte-matches the golden observable trace") {
  // Thirteen rules, every one parking on its own awaited scroll: a chain longer
  // than the profile's concurrent-handle cap. Each waiting link holds an
  // uncapped pending trigger handle, so the chain marches to its last link.
  constexpr int kLinks = 12;
  constexpr int kThinksPerLink = 3;
  constexpr int kTickMs = 5000;
  std::vector<TriggerModeStep> schedule;
  const size_t thinks = 1 + static_cast<size_t>(kLinks + 1) * kThinksPerLink + 3;
  for (size_t i = 0; i < thinks; i++) {
    const int a = i == 1 ? 1 : (i == 2 ? 0 : -1);
    schedule.push_back(TriggerModeStep{kTickMs, a, -1, {}});
  }
  runTriggerModeParity("then-chain-parked-links", schedule);
}

TEST_CASE("the then-page-reentry fixture byte-matches the golden observable trace") {
  std::vector<TriggerModeStep> schedule{
      {1100, -1, -1, {}}, {1100, 1, -1, {}}, {1100, 0, 1, {}}, {1100, -1, 0, {}}};
  for (int i = 0; i < 5; i++) {
    schedule.push_back(TriggerModeStep{1100, -1, -1, {}});
  }
  schedule.push_back(TriggerModeStep{1100, 1, -1, {}});
  schedule.push_back(TriggerModeStep{1100, 0, -1, {}});
  for (int i = 0; i < 7; i++) {
    schedule.push_back(TriggerModeStep{1100, -1, -1, {}});
  }
  runTriggerModeParity("then-page-reentry", schedule);
}
