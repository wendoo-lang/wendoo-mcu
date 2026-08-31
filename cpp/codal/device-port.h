#pragma once

#include <cstdint>

#include "core/runtime/handle-table.h"
#include "core/runtime/mc-number.h"

namespace wendoo {

/**
 * Supplies the frames of a timed image draw to a {@link PixelDisplayPort}. The
 * port reads every frame up front (into its own storage) when the draw is
 * accepted, so the source need only outlive the `drawFrames` call.
 */
class DrawFrameSource {
public:
  virtual ~DrawFrameSource() = default;

  /** Number of frames in the sequence; always at least one. */
  virtual uint32_t frameCount() = 0;

  /**
   * Write frame `index` (0-based, less than {@link frameCount}) as packed
   * brightness bytes, row-major, into `out`, and report its clipped
   * `width`/`height`. The frame is clipped to the board matrix, so
   * `width * height` bytes are written and `out` must have room for the board's
   * pixel count.
   */
  virtual void writeFrame(uint32_t index, uint8_t* out, uint32_t& width, uint32_t& height) = 0;
};

/**
 * Pixel-matrix display output. Coordinates are zero-based from the top-left;
 * dimensions are board-defined (5x5 on a micro:bit v2).
 */
class PixelDisplayPort {
public:
  virtual ~PixelDisplayPort() = default;

  /**
   * Set one pixel's brightness: 0 is off, 255 is full brightness. Coordinates
   * are signed (int16, the CODAL `Image::setPixelValue` parameter type); a write
   * whose coordinate falls outside the board's matrix is dropped by the device.
   */
  virtual void setPixel(int16_t x, int16_t y, uint8_t brightness) = 0;

  /**
   * Show `length` ASCII bytes on the display, requested at logical tick time
   * `requestTimeMs`. A text of any length other than one scrolls across the
   * display at `delayMs` per animation step and ends blank. A one-byte text is
   * a static show: its glyph is painted at once, holds the display for the
   * duration the scroll formula gives one character, and blanks at completion.
   * The call returns immediately; the show runs asynchronously and settles
   * `handle` (resolve) when it completes. A show requested while the display
   * is busy (a scroll, a static show, or a held draw) is silently dropped:
   * nothing is shown and `handle` settles at once.
   */
  virtual void scrollText(const uint8_t* bytes, uint32_t length, uint32_t delayMs,
                          mc_number_t requestTimeMs, AsyncHandle handle) = 0;

  /**
   * Paste a sequence of packed brightness frames (row-major, one byte per pixel,
   * each already clipped to the board's matrix) to the display top-left, one
   * frame per `perFrameDurationMs` slice, requested at logical tick time
   * `requestTimeMs`. The port reads every frame from `frames` up front. The call
   * returns immediately and settles `handle` (resolve): a positive
   * `perFrameDurationMs` holds the display for `frameCount * perFrameDurationMs`,
   * advancing to the next frame each slice, before settling; a non-positive
   * `perFrameDurationMs` is fire-and-forget -- only the final frame is pasted, no
   * hold is taken, and `handle` settles at once. A draw requested while the
   * display is busy (a scroll or a held draw) is silently dropped: nothing is
   * pasted and `handle` settles at once. The last pasted frame is never cleared;
   * it persists until the next draw.
   */
  virtual void drawFrames(DrawFrameSource& frames, uint32_t perFrameDurationMs,
                          mc_number_t requestTimeMs, AsyncHandle handle) = 0;

  /**
   * Release the current display lease at once: a held scroll or timed draw is
   * dropped and its handle resolved, so its awaiting rule resumes as if the
   * operation finished. A no-op when no lease is held. The display content is
   * left as-is; the next operation overwrites it.
   */
  virtual void preempt() = 0;

  /**
   * Cancel any held display lease and blank the matrix: a held scroll or timed
   * draw is preempted (its handle resolved, so its awaiting rule resumes as if
   * it had finished) before every pixel is zeroed. A no-op lease-wise when none
   * is held.
   */
  virtual void clear() = 0;

  /**
   * Read the ambient light level off the LED matrix, 0 (dark) to 255 (bright).
   * A polled level unrelated to the displayed pixels.
   */
  virtual int getLightLevel() = 0;
};

/** Oscillator wave shape of a plain constant-pitch tone. */
enum class SpeakerToneWaveform : uint8_t {
  Square = 0,
  Sawtooth = 1,
  Sine = 2,
  Triangle = 3,
};

/**
 * A plain constant-pitch tone as accepted by the speaker port: one wave shape
 * at one frequency for one duration.
 */
struct SpeakerToneCommand {
  /** Wave shape the tone is sounded with. */
  SpeakerToneWaveform waveform;

  /**
   * Pitch in Hz, clamped into the board's usable range. Zero is a rest and
   * carries a zero {@link volume}.
   */
  mc_number_t frequencyHz;

  /**
   * Play length in whole milliseconds. A negative length marks a dropped tone:
   * {@link SpeakerPort::playTone} sounds nothing and settles at once.
   */
  int32_t durationMs;

  /** Volume as a fraction of full tone volume, 0 to 1. */
  mc_number_t volume;
};

/**
 * Speaker sound output: a single output leased by the playing sound or tone.
 * The lease is resolved against logical tick time from the play's nominal total
 * duration, never from a device-driver completion event.
 */
class SpeakerPort {
public:
  virtual ~SpeakerPort() = default;

  /**
   * Play the built-in sound named by `length` ASCII bytes, requested at
   * logical tick time `requestTimeMs`. The call returns immediately. An
   * accepted play takes the speaker lease for the sound's nominal total
   * duration and settles `handle` (resolve) once it elapses. A play requested
   * while the speaker is busy is silently dropped: nothing plays and `handle`
   * settles at once. A name outside the board's built-in set is a silent
   * no-op: nothing plays, no lease is taken, and `handle` settles at once.
   */
  virtual void playSoundEmoji(const uint8_t* name, uint32_t length, mc_number_t requestTimeMs,
                              AsyncHandle handle) = 0;

  /**
   * Sound the constant-pitch `tone`, requested at logical tick time
   * `requestTimeMs`. The call returns immediately. An accepted tone takes the
   * speaker lease for `tone.durationMs` and settles `handle` (resolve) once it
   * elapses. A tone requested while the speaker is busy is silently dropped:
   * nothing sounds and `handle` settles at once. A negative
   * `tone.durationMs` is dropped the same way; a zero one is accepted and
   * settles on the first lease settle.
   */
  virtual void playTone(const SpeakerToneCommand& tone, mc_number_t requestTimeMs,
                        AsyncHandle handle) = 0;

  /**
   * Release the current speaker lease at once: the held play stops and its
   * handle resolves, so its awaiting rule resumes as if the sound finished. A
   * no-op when no lease is held.
   */
  virtual void preempt() = 0;
};

/**
 * Momentary button input. Buttons are addressed by a zero-based index whose
 * mapping to physical inputs is board-defined (0 is button A, 1 is button B,
 * and 2 is the capacitive touch logo on a micro:bit v2).
 */
class ButtonInputPort {
public:
  virtual ~ButtonInputPort() = default;

  /** Current level of the button: true while physically pressed. */
  virtual bool isPressed(uint8_t buttonIndex) = 0;
};

/**
 * Three-axis accelerometer input. Exposes the last recognized gesture as a
 * board-defined gesture code plus the current acceleration (milli-g) and
 * rotation-compensated orientation (degrees or radians). All reads are polled
 * levels: the gesture holds its last value until a new one is recognized.
 */
class AccelerometerInputPort {
public:
  virtual ~AccelerometerInputPort() = default;

  /**
   * The last recognized gesture as a board-defined gesture code (0 when none
   * has been recognized). On a micro:bit v2 the codes are the CODAL
   * accelerometer gesture values.
   */
  virtual uint16_t getGesture() = 0;

  /** Acceleration along the X axis in milli-g; signed. */
  virtual int32_t getX() = 0;

  /** Acceleration along the Y axis in milli-g; signed. */
  virtual int32_t getY() = 0;

  /** Acceleration along the Z axis in milli-g; signed. */
  virtual int32_t getZ() = 0;

  /** Rotation-compensated pitch in whole degrees; signed. */
  virtual int32_t getPitch() = 0;

  /** Rotation-compensated roll in whole degrees; signed. */
  virtual int32_t getRoll() = 0;

  /** Rotation-compensated pitch in radians; signed. */
  virtual mc_number_t getPitchRadians() = 0;

  /** Rotation-compensated roll in radians; signed. */
  virtual mc_number_t getRollRadians() = 0;
};

/**
 * Die-temperature thermometer input. A polled synchronous read of the board's
 * own temperature (on a micro:bit v2 the on-chip die sensor behind
 * CODAL's `getTemperature()`).
 */
class ThermometerInputPort {
public:
  virtual ~ThermometerInputPort() = default;

  /** The current temperature in whole degrees Celsius; signed. */
  virtual int32_t getTemperature() = 0;
};

/**
 * External I2C bus on the board's edge connector (pins P19/P20 on a micro:bit
 * v2). The controller side of the bus a peripheral library drives.
 */
class I2CPort {
public:
  virtual ~I2CPort() = default;

  /**
   * Write `len` bytes from `data` to the 7-bit `address` in one complete
   * START/STOP transaction. A zero `len` is an address-only transaction (`data`
   * may be null only then). Returns 0 on success, nonzero on a bus error.
   */
  virtual int write(uint16_t address, const uint8_t* data, int len) = 0;

  /**
   * Read `len` bytes from the 7-bit `address` into `data` in one complete
   * START/STOP transaction. On success exactly `len` bytes are written to `data`
   * and 0 is returned; on a bus error or absent device nonzero is returned and
   * `data` is left undefined. A zero `len` reads no bytes (`data` may be null
   * only then).
   */
  virtual int read(uint16_t address, uint8_t* data, int len) = 0;
};

/**
 * GPIO pins on the board's edge connector (P0-P20 on a micro:bit v2). Pins are
 * addressed by number 0-20; a pin outside that range is a no-op (a read returns
 * 0). The simple synchronous pin I/O a peripheral library drives for line
 * sensors, outputs, pull configuration, and servos.
 */
class GPIOPort {
public:
  virtual ~GPIOPort() = default;

  /**
   * Read the digital level of `pin` (0-20): 0 for low, 1 for high. A pin outside
   * 0-20 returns 0.
   */
  virtual int digitalRead(int pin) = 0;

  /**
   * Write `value` (0 low, nonzero high) to `pin` (0-20). A pin outside 0-20 is a
   * no-op. Returns 0 on success.
   */
  virtual int digitalWrite(int pin, int value) = 0;

  /**
   * Set the pull mode of `pin` (0-20): `mode` is 0 none, 1 up, 2 down. A pin
   * outside 0-20 or an unrecognized mode is a no-op. Returns 0 on success.
   */
  virtual int setPull(int pin, int mode) = 0;

  /**
   * Write a servo `angle` in degrees (0-180) to `pin` (0-20). A pin outside 0-20
   * is a no-op. Returns 0 on success.
   */
  virtual int setServo(int pin, int angle) = 0;

  /**
   * Read the analog (ADC) value of `pin` (0-20): 0-1023. A pin outside 0-20
   * returns 0.
   */
  virtual int analogRead(int pin) = 0;
};

/**
 * Background sonar reads for an SR04-style ultrasonic, keyed by the sensor's
 * (trigger, echo) pin pair. A background driver runs the measurement off the VM
 * and a read returns the latest cached distance. The first reference to a pin
 * pair registers that sonar; later references to the same pins read its shared
 * cache. A read returns the previous driver cycle's measurement (a fixed
 * one-cycle lag), the defined maximum distance before any measurement has
 * completed, and the maximum distance on a timeout / no echo.
 */
class SonarPort {
public:
  virtual ~SonarPort() = default;

  /**
   * Read the cached distance in centimeters of the sonar wired to `trig`/`echo`,
   * registering it on the first reference. Returns the previous driver cycle's
   * measurement (or the defined maximum distance before one has completed).
   */
  virtual int distance(int trig, int echo) = 0;
};

/**
 * A typed packet handed to {@link RadioPort::send}. Unused fields carry their
 * empty value. `type` is a MakeCode packet type (0-5) or -1 for a raw datagram;
 * `value` is the numeric payload; `name`/`text`/`bytes` are borrowed spans valid
 * only for the duration of the send call.
 */
struct RadioSendView {
  int type;
  uint32_t group;
  mc_number_t value;
  const uint8_t* name;
  uint32_t nameLen;
  const uint8_t* text;
  uint32_t textLen;
  const uint8_t* bytes;
  uint32_t bytesLen;
};

/**
 * A decoded packet read from the receive ring. `name`/`text`/`bytes` are
 * borrowed spans owned by the port, valid until the ring is next mutated. `seq`
 * is the monotonic arrival sequence (from 1); `type` is a MakeCode packet type
 * (0-5) or -1 for a raw datagram.
 */
struct RadioPacketView {
  int seq;
  int type;
  uint32_t group;
  mc_number_t value;
  const uint8_t* name;
  uint32_t nameLen;
  const uint8_t* text;
  uint32_t textLen;
  const uint8_t* bytes;
  uint32_t bytesLen;
  int rssi;
  int serial;
  int time;
};

/**
 * 2.4 GHz packet radio. Send is a synchronous port write (on device CODAL
 * `datagram.send` blocks until TX). Received packets land in a bounded ring
 * (depth {@link RADIO_RX_RING_DEPTH}); receive callsites scan the ring by
 * sequence number. The typed receive sensors keep their own per-callsite
 * cursors; the Device-API `receive(since)` is stateless (the caller passes its
 * last-seen sequence).
 */
class RadioPort {
public:
  /** Receive ring depth, the CODAL radio's maximum-RX-buffers protocol bound. */
  static constexpr uint32_t RADIO_RX_RING_DEPTH = 4;

  virtual ~RadioPort() = default;

  /** Transmit a typed packet on its group. */
  virtual void send(const RadioSendView& packet) = 0;

  /** The current group (0-255). */
  virtual uint8_t group() = 0;

  /** Set the radio group (0-255). */
  virtual void setGroup(int group) = 0;

  /** Set the transmit power level (0-7). */
  virtual void setTransmitPower(int power) = 0;

  /** Set the frequency band (0-83). */
  virtual void setFrequencyBand(int band) = 0;

  /** Number of packets currently present in the ring. */
  virtual uint32_t ringSize() = 0;

  /** The packet at ring `index` (0-based, in arrival order); `index < ringSize()`. */
  virtual const RadioPacketView& ringAt(uint32_t index) = 0;

  /** The highest arrival sequence assigned so far (0 when nothing has arrived). */
  virtual int headSequence() = 0;
};

/**
 * Board-side rendering primitives for the device fault mode. The fault-mode
 * policy (stop ticking the brain, then show-the-error in a loop) lives with
 * the host loop; implementations only render. Both calls may block for the
 * duration of their animation.
 */
class FaultDisplayPort {
public:
  virtual ~FaultDisplayPort() = default;

  /** Show the board's fault indicator (a sad face on a micro:bit v2). */
  virtual void showFaultFace() = 0;

  /** Scroll a short ASCII diagnostic code across the display, once. */
  virtual void scrollFaultCode(const char* code) = 0;
};

/** Monotonic time source used to stamp think-loop ticks. */
class MonotonicClockPort {
public:
  virtual ~MonotonicClockPort() = default;

  /** Milliseconds since an arbitrary fixed origin (boot). Never decreases. */
  virtual uint32_t uptimeMillis() = 0;
};

/**
 * The full set of board ports the host loop drives. Wired once by the target
 * at startup; every pointer is non-null and non-owning, and the pointed-to
 * ports must outlive the host loop.
 */
struct DevicePorts {
  PixelDisplayPort* display;
  ButtonInputPort* buttons;
  FaultDisplayPort* faultDisplay;
  MonotonicClockPort* clock;
  AccelerometerInputPort* accelerometer;
  I2CPort* i2c;
  GPIOPort* gpio;
  SonarPort* sonar;
  RadioPort* radio;
  SpeakerPort* speaker;
  ThermometerInputPort* thermometer;
};

} // namespace wendoo
