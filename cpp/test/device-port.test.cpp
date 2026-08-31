#include "doctest/doctest.h"

#include "codal/accelerometer-gesture.h"
#include "codal/device-port.h"

#include <map>
#include <string>
#include <vector>

namespace {

struct RecordingDisplay : wendoo::PixelDisplayPort {
  struct Call {
    int16_t x;
    int16_t y;
    uint8_t brightness;
  };
  std::vector<Call> calls;

  void setPixel(int16_t x, int16_t y, uint8_t brightness) override {
    calls.push_back({x, y, brightness});
  }

  void scrollText(const uint8_t*, uint32_t, uint32_t, wendoo::mc_number_t,
                  wendoo::AsyncHandle) override {}

  void drawFrames(wendoo::DrawFrameSource&, uint32_t, wendoo::mc_number_t,
                  wendoo::AsyncHandle) override {}

  void preempt() override {}

  void clear() override {}

  int getLightLevel() override { return lightLevel; }

  int lightLevel = 128;
};

struct FixedButtons : wendoo::ButtonInputPort {
  bool pressed[2] = {false, false};

  bool isPressed(uint8_t buttonIndex) override { return pressed[buttonIndex]; }
};

// Whole degrees for a radian orientation, matching CODAL's
// int getPitch() { return (int)((360.0f*radians)/(2.0f*(float)PI)); }.
int32_t degreesFromRadians(wendoo::mc_number_t radians) {
  const float twoPi = 2.0f * static_cast<float>(3.141592653589793);
  return static_cast<int32_t>((360.0f * radians) / twoPi);
}

struct SettableAccelerometer : wendoo::AccelerometerInputPort {
  uint16_t gesture = 0;
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;
  wendoo::mc_number_t pitchRadians = 0;
  wendoo::mc_number_t rollRadians = 0;

  uint16_t getGesture() override { return gesture; }
  int32_t getX() override { return x; }
  int32_t getY() override { return y; }
  int32_t getZ() override { return z; }
  int32_t getPitch() override { return degreesFromRadians(pitchRadians); }
  int32_t getRoll() override { return degreesFromRadians(rollRadians); }
  wendoo::mc_number_t getPitchRadians() override { return pitchRadians; }
  wendoo::mc_number_t getRollRadians() override { return rollRadians; }
};

struct SettableThermometer : wendoo::ThermometerInputPort {
  int32_t temperature = 21;

  int32_t getTemperature() override { return temperature; }
};

struct RecordingFaultDisplay : wendoo::FaultDisplayPort {
  int faceShown = 0;
  std::vector<std::string> scrolled;

  void showFaultFace() override { faceShown++; }
  void scrollFaultCode(const char* code) override { scrolled.push_back(code); }
};

struct RecordingI2C : wendoo::I2CPort {
  struct Write {
    uint16_t address;
    std::vector<uint8_t> bytes;
  };
  std::vector<Write> writes;
  std::vector<uint8_t> readResponse;

  int write(uint16_t address, const uint8_t* data, int len) override {
    writes.push_back({address, std::vector<uint8_t>(data, data + (len > 0 ? len : 0))});
    return 0;
  }

  int read(uint16_t, uint8_t* data, int len) override {
    for (int i = 0; i < len; i++) {
      data[i] = static_cast<size_t>(i) < readResponse.size() ? readResponse[i] : 0;
    }
    return 0;
  }
};

struct SteppingClock : wendoo::MonotonicClockPort {
  uint32_t now = 0;

  uint32_t uptimeMillis() override { return now; }
};

struct RecordingGpio : wendoo::GPIOPort {
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
  std::vector<DigitalWrite> writes;
  std::vector<Pull> pulls;
  std::vector<Servo> servos;
  std::map<int, int> levels;
  std::map<int, int> analogLevels;

  int digitalRead(int pin) override {
    const auto it = levels.find(pin);
    return it == levels.end() ? 0 : it->second;
  }

  int analogRead(int pin) override {
    const auto it = analogLevels.find(pin);
    return it == analogLevels.end() ? 0 : it->second;
  }

  int digitalWrite(int pin, int value) override {
    writes.push_back({pin, value});
    return 0;
  }

  int setPull(int pin, int mode) override {
    pulls.push_back({pin, mode});
    return 0;
  }

  int setServo(int pin, int angle) override {
    servos.push_back({pin, angle});
    return 0;
  }
};

struct RecordingSonar : wendoo::SonarPort {
  int reported = 0;
  int lastTrig = -1;
  int lastEcho = -1;

  int distance(int trig, int echo) override {
    lastTrig = trig;
    lastEcho = echo;
    return reported;
  }
};

struct RecordingSpeaker : wendoo::SpeakerPort {
  struct Play {
    std::string name;
    wendoo::mc_number_t requestTimeMs;
  };
  struct Tone {
    wendoo::SpeakerToneCommand command;
    wendoo::mc_number_t requestTimeMs;
  };
  std::vector<Play> plays;
  std::vector<Tone> tones;
  int preempts = 0;

  void playSoundEmoji(const uint8_t* name, uint32_t length, wendoo::mc_number_t requestTimeMs,
                      wendoo::AsyncHandle) override {
    plays.push_back({std::string(reinterpret_cast<const char*>(name), length), requestTimeMs});
  }

  void playTone(const wendoo::SpeakerToneCommand& tone, wendoo::mc_number_t requestTimeMs,
                wendoo::AsyncHandle) override {
    tones.push_back({tone, requestTimeMs});
  }

  void preempt() override { preempts++; }
};

struct RecordingRadio : wendoo::RadioPort {
  int sentType = -2;
  uint8_t groupValue = 0;
  wendoo::RadioPacketView empty{};

  void send(const wendoo::RadioSendView& packet) override { sentType = packet.type; }
  uint8_t group() override { return groupValue; }
  void setGroup(int g) override { groupValue = static_cast<uint8_t>(g); }
  void setTransmitPower(int) override {}
  void setFrequencyBand(int) override {}
  uint32_t ringSize() override { return 0; }
  const wendoo::RadioPacketView& ringAt(uint32_t) override { return empty; }
  int headSequence() override { return 0; }
};

} // namespace

TEST_CASE("a host stub can implement every device port") {
  RecordingDisplay display;
  FixedButtons buttons;
  RecordingFaultDisplay faultDisplay;
  SteppingClock clock;
  SettableAccelerometer accelerometer;
  SettableThermometer thermometer;
  RecordingI2C i2c;
  RecordingGpio gpio;
  RecordingSonar sonar;
  RecordingRadio radio;
  RecordingSpeaker speaker;

  wendoo::DevicePorts ports{&display, &buttons, &faultDisplay, &clock,   &accelerometer, &i2c,
                            &gpio,    &sonar,   &radio,        &speaker, &thermometer};

  ports.display->setPixel(2, 3, 255);
  REQUIRE(display.calls.size() == 1);
  CHECK(display.calls[0].x == 2);
  CHECK(display.calls[0].y == 3);
  CHECK(display.calls[0].brightness == 255);

  buttons.pressed[0] = true;
  CHECK(ports.buttons->isPressed(0));
  CHECK_FALSE(ports.buttons->isPressed(1));

  ports.faultDisplay->showFaultFace();
  ports.faultDisplay->scrollFaultCode("E042");
  CHECK(faultDisplay.faceShown == 1);
  REQUIRE(faultDisplay.scrolled.size() == 1);
  CHECK(faultDisplay.scrolled[0] == "E042");

  clock.now = 1000;
  CHECK(ports.clock->uptimeMillis() == 1000);

  accelerometer.gesture = static_cast<uint16_t>(wendoo::AccelerometerGesture::Shake);
  accelerometer.x = 1024;
  accelerometer.y = -512;
  accelerometer.z = -1000;
  accelerometer.pitchRadians = 0.7853982f; // pi/4
  accelerometer.rollRadians = -0.5235988f; // -pi/6
  CHECK(ports.accelerometer->getGesture() == 11);
  CHECK(ports.accelerometer->getX() == 1024);
  CHECK(ports.accelerometer->getY() == -512);
  CHECK(ports.accelerometer->getZ() == -1000);
  CHECK(ports.accelerometer->getPitchRadians() == doctest::Approx(0.7853982f));
  CHECK(ports.accelerometer->getRollRadians() == doctest::Approx(-0.5235988f));
  // Degrees derive from the radian reading, matching CODAL's conversion.
  CHECK(ports.accelerometer->getPitch() == 45);
  CHECK(ports.accelerometer->getRoll() == -30);

  // The die temperature is a signed whole-degree reading.
  CHECK(ports.thermometer->getTemperature() == 21);
  thermometer.temperature = -5;
  CHECK(ports.thermometer->getTemperature() == -5);

  const uint8_t payload[3] = {0xde, 0xad, 0xbe};
  CHECK(ports.i2c->write(0x10, payload, 3) == 0);
  REQUIRE(i2c.writes.size() == 1);
  CHECK(i2c.writes[0].address == 0x10);
  CHECK(i2c.writes[0].bytes == std::vector<uint8_t>{0xde, 0xad, 0xbe});

  i2c.readResponse = {0xaa, 0xbb, 0xcc};
  uint8_t readBuf[3] = {0, 0, 0};
  CHECK(ports.i2c->read(0x42, readBuf, 3) == 0);
  CHECK(std::vector<uint8_t>(readBuf, readBuf + 3) == std::vector<uint8_t>{0xaa, 0xbb, 0xcc});

  gpio.levels[13] = 1;
  CHECK(ports.gpio->digitalRead(13) == 1);
  CHECK(ports.gpio->digitalRead(14) == 0);
  gpio.analogLevels[1] = 1023;
  CHECK(ports.gpio->analogRead(1) == 1023);
  CHECK(ports.gpio->analogRead(2) == 0);
  CHECK(ports.gpio->digitalWrite(2, 1) == 0);
  CHECK(ports.gpio->setPull(13, 0) == 0);
  CHECK(ports.gpio->setServo(1, 90) == 0);
  REQUIRE(gpio.writes.size() == 1);
  CHECK(gpio.writes[0].pin == 2);
  CHECK(gpio.writes[0].value == 1);
  REQUIRE(gpio.pulls.size() == 1);
  CHECK(gpio.pulls[0].pin == 13);
  CHECK(gpio.pulls[0].mode == 0);
  REQUIRE(gpio.servos.size() == 1);
  CHECK(gpio.servos[0].pin == 1);
  CHECK(gpio.servos[0].angle == 90);

  sonar.reported = 42;
  CHECK(ports.sonar->distance(8, 12) == 42);
  CHECK(sonar.lastTrig == 8);
  CHECK(sonar.lastEcho == 12);

  ports.radio->setGroup(7);
  CHECK(ports.radio->group() == 7);
  wendoo::RadioSendView packet{};
  packet.type = 0;
  ports.radio->send(packet);
  CHECK(radio.sentType == 0);

  const uint8_t soundName[5] = {'h', 'e', 'l', 'l', 'o'};
  ports.speaker->playSoundEmoji(soundName, 5, 250, wendoo::AsyncHandle{});
  REQUIRE(speaker.plays.size() == 1);
  CHECK(speaker.plays[0].name == "hello");
  CHECK(speaker.plays[0].requestTimeMs == 250);
  const wendoo::SpeakerToneCommand tone{wendoo::SpeakerToneWaveform::Sine, 440.0f, 250, 0.5f};
  ports.speaker->playTone(tone, 300, wendoo::AsyncHandle{});
  REQUIRE(speaker.tones.size() == 1);
  CHECK(speaker.tones[0].command.waveform == wendoo::SpeakerToneWaveform::Sine);
  CHECK(speaker.tones[0].command.frequencyHz == 440.0f);
  CHECK(speaker.tones[0].command.durationMs == 250);
  CHECK(speaker.tones[0].command.volume == 0.5f);
  CHECK(speaker.tones[0].requestTimeMs == 300);

  ports.speaker->preempt();
  CHECK(speaker.preempts == 1);
}

TEST_CASE("AccelerometerGesture codes match the CODAL accelerometer gesture values") {
  using wendoo::AccelerometerGesture;
  CHECK(static_cast<uint16_t>(AccelerometerGesture::None) == 0);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::TiltUp) == 1);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::TiltDown) == 2);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::TiltLeft) == 3);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::TiltRight) == 4);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::FaceUp) == 5);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::FaceDown) == 6);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::Freefall) == 7);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::Impact3G) == 8);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::Impact6G) == 9);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::Impact8G) == 10);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::Shake) == 11);
  CHECK(static_cast<uint16_t>(AccelerometerGesture::Impact2G) == 12);
}
