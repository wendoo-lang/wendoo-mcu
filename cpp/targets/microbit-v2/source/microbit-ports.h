#pragma once

#include "MicroBit.h"
#include "Synthesizer.h"

#include "codal/device-port.h"
#include "codal/radio-wire.h"
#include "targets/microbit-v2/abi/button-index.h"
#include "targets/microbit-v2/abi/display-scroll.h"
#include "targets/microbit-v2/abi/sound-emoji.h"
#include "targets/microbit-v2/abi/speaker-tone.h"

namespace wendoo
{

/**
 * micro:bit v2 implementations of the device ports. Each binds one abstract
 * port (cpp/codal/device-port.h) to a CODAL peripheral on the shared `MicroBit`
 * instance, which must outlive every port.
 */

/**
 * Drives the 5x5 LED matrix through `MicroBitDisplay`: direct pixel writes, the
 * asynchronous text show, and the asynchronous image draw. The text show and the
 * timed draw share one display lease: a multi-character text starts CODAL's
 * `scrollAsync`, a one-character text prints its glyph statically (blanked again
 * when its hold elapses), and a timed draw pastes the frame; each resolves its
 * async handle once its duration has elapsed, polled each host-loop tick by
 * {@link pollDisplay}. A show or draw requested while the lease is held is
 * silently dropped (its handle settles at once); a zero-duration draw pastes and
 * settles at once without taking the lease.
 */
class MicroBitPixelDisplayPort : public PixelDisplayPort
{
public:
    explicit MicroBitPixelDisplayPort(MicroBit &uBit) : uBit_(uBit) {}

    void setPixel(int16_t x, int16_t y, uint8_t brightness) override
    {
        uBit_.display.image.setPixelValue(x, y, brightness);
    }

    void scrollText(const uint8_t *bytes, uint32_t length, uint32_t delayMs, mc_number_t,
                    AsyncHandle handle) override
    {
        if (busy_)
        {
            handle.resolve(kVoidValue);
            return;
        }
        active_ = handle;
        busy_ = true;
        completionTime_ =
            static_cast<uint32_t>(system_timer_current_time()) + scrollDurationMs(length, delayMs);
        // The text shows on a blank display; clear any prior content (an
        // earlier draw) first so it does not linger under the animation.
        uBit_.display.image.clear();
        if (length == 1)
        {
            // A one-character text shows statically; pollDisplay blanks it when
            // its hold elapses.
            staticShow_ = true;
            uBit_.display.printCharAsync(static_cast<char>(bytes[0]), 0);
            return;
        }
        ManagedString text(reinterpret_cast<const char *>(bytes), static_cast<int16_t>(length));
        uBit_.display.scrollAsync(text, static_cast<int>(delayMs));
    }

    void drawFrames(DrawFrameSource &frames, uint32_t perFrameDurationMs, mc_number_t,
                    AsyncHandle handle) override
    {
        if (busy_)
        {
            handle.resolve(kVoidValue);
            return;
        }
        const uint32_t frameCount = frames.frameCount();
        uint8_t buf[kPixelCount];
        if (perFrameDurationMs == 0)
        {
            // Fire-and-forget: paint only the final frame and take no lease.
            uint32_t width = 0;
            uint32_t height = 0;
            frames.writeFrame(frameCount - 1, buf, width, height);
            pasteFrame(buf, width, height);
            handle.resolve(kVoidValue);
            return;
        }
        releaseFrames();
        frameCount_ = frameCount;
        frameBytes_ = new uint8_t[frameCount * kPixelCount];
        frameWidths_ = new uint32_t[frameCount];
        frameHeights_ = new uint32_t[frameCount];
        for (uint32_t i = 0; i < frameCount; i++)
        {
            uint32_t width = 0;
            uint32_t height = 0;
            frames.writeFrame(i, &frameBytes_[i * kPixelCount], width, height);
            frameWidths_[i] = width;
            frameHeights_[i] = height;
        }
        pasteFrame(&frameBytes_[0], frameWidths_[0], frameHeights_[0]);
        busy_ = true;
        seqStart_ = static_cast<uint32_t>(system_timer_current_time());
        perFrameMs_ = perFrameDurationMs;
        paintedCount_ = 1;
        completionTime_ = seqStart_ + frameCount * perFrameDurationMs;
        active_ = handle;
    }

    /**
     * Advances a held image sequence to the frame due now, then settles the held
     * show or sequence handle once its lease has elapsed (enqueue-only; the
     * think loop resumes the waiter), blanking a static one-character show as it
     * settles. Call once per host-loop tick before the brain thinks.
     */
    void pollDisplay()
    {
        if (!busy_)
        {
            return;
        }
        const uint32_t now = static_cast<uint32_t>(system_timer_current_time());
        if (frameCount_ > 0)
        {
            const uint32_t elapsed = now > seqStart_ ? now - seqStart_ : 0;
            uint32_t target = perFrameMs_ == 0 ? 0 : elapsed / perFrameMs_;
            if (target > frameCount_ - 1)
            {
                target = frameCount_ - 1;
            }
            while (paintedCount_ <= target)
            {
                pasteFrame(&frameBytes_[paintedCount_ * kPixelCount], frameWidths_[paintedCount_],
                           frameHeights_[paintedCount_]);
                paintedCount_++;
            }
        }
        if (now < completionTime_)
        {
            return;
        }
        releaseFrames();
        if (staticShow_)
        {
            staticShow_ = false;
            uBit_.display.image.clear();
        }
        const AsyncHandle done = active_;
        busy_ = false;
        done.resolve(kVoidValue);
    }

    void preempt() override
    {
        if (!busy_)
        {
            return;
        }
        const AsyncHandle held = active_;
        busy_ = false;
        staticShow_ = false;
        releaseFrames();
        // Stop any in-flight CODAL scroll animation; the next operation repaints.
        uBit_.display.stopAnimation();
        held.resolve(kVoidValue);
    }

    void clear() override
    {
        preempt();
        uBit_.display.image.clear();
    }

    int getLightLevel() override { return uBit_.display.readLightLevel(); }

private:
    /** Pixels in the 5x5 matrix; the per-frame storage stride. */
    static constexpr uint32_t kPixelCount = 25;

    MicroBit &uBit_;
    bool busy_ = false;
    // True while the held lease is a static one-character show, blanked at completion.
    bool staticShow_ = false;
    uint32_t completionTime_ = 0;
    AsyncHandle active_{};
    // A held image sequence: each frame's bytes (stride kPixelCount), its size,
    // and the playback cursor. Null/zero while no sequence holds the lease.
    uint8_t *frameBytes_ = nullptr;
    uint32_t *frameWidths_ = nullptr;
    uint32_t *frameHeights_ = nullptr;
    uint32_t frameCount_ = 0;
    uint32_t paintedCount_ = 0;
    uint32_t seqStart_ = 0;
    uint32_t perFrameMs_ = 0;

    /** Paste one frame top-left into the display image, row-major. */
    void pasteFrame(const uint8_t *frame, uint32_t width, uint32_t height)
    {
        for (uint32_t row = 0; row < height; row++)
        {
            for (uint32_t col = 0; col < width; col++)
            {
                uBit_.display.image.setPixelValue(
                    static_cast<int16_t>(col), static_cast<int16_t>(row), frame[row * width + col]);
            }
        }
    }

    /** Free the held sequence storage and reset the playback cursor. */
    void releaseFrames()
    {
        delete[] frameBytes_;
        delete[] frameWidths_;
        delete[] frameHeights_;
        frameBytes_ = nullptr;
        frameWidths_ = nullptr;
        frameHeights_ = nullptr;
        frameCount_ = 0;
        paintedCount_ = 0;
    }
};

/**
 * Drives the on-board speaker through CODAL's `SoundExpressions`. An accepted
 * built-in play stops any still-sounding expression and starts the named one
 * asynchronously; CODAL resolves the name to its encoded data itself and audio
 * activation is automatic. An accepted tone is encoded as a single
 * `SoundEffect` -- the wave shape's tone function at a constant frequency and a
 * flat volume -- and handed to the same synthesizer. Built-in plays and tones
 * share one lease, settled against their pinned duration (never CODAL's
 * completion events), polled each host-loop tick by {@link pollSpeaker} against
 * `system_timer_current_time()`; a device synth may randomize the actual
 * playback length around a built-in's nominal, and the stop-before-play keeps a
 * still-ringing tail from blocking the next sound. A play or tone requested
 * while the lease is held is silently dropped (its handle settles at once); a
 * name outside the built-in set, and a tone with a negative duration, never
 * reach CODAL (settled at once, no lease).
 */
class MicroBitSpeakerPort : public SpeakerPort
{
public:
    explicit MicroBitSpeakerPort(MicroBit &uBit) : uBit_(uBit) {}

    void playSoundEmoji(const uint8_t *name, uint32_t length, mc_number_t,
                        AsyncHandle handle) override
    {
        if (busy_)
        {
            handle.resolve(kVoidValue);
            return;
        }
        uint32_t durationMs = 0;
        if (!soundEmojiDurationMs(name, length, durationMs))
        {
            handle.resolve(kVoidValue);
            return;
        }
        active_ = handle;
        busy_ = true;
        completionTime_ = static_cast<uint32_t>(system_timer_current_time()) + durationMs;
        // Stop any still-sounding expression (a randomized tail can outlast the
        // nominal lease) before starting the new one.
        uBit_.audio.soundExpressions.stop();
        ManagedString sound(reinterpret_cast<const char *>(name), static_cast<int16_t>(length));
        uBit_.audio.soundExpressions.playAsync(sound);
    }

    void playTone(const SpeakerToneCommand &tone, mc_number_t, AsyncHandle handle) override
    {
        if (busy_ || tone.durationMs < 0)
        {
            handle.resolve(kVoidValue);
            return;
        }
        active_ = handle;
        busy_ = true;
        completionTime_ = static_cast<uint32_t>(system_timer_current_time()) +
                          static_cast<uint32_t>(tone.durationMs);
        uBit_.audio.soundExpressions.stop();
        ManagedBuffer sound(sizeof(SoundEffect));
        SoundEffect *fx = reinterpret_cast<SoundEffect *>(&sound[0]);
        fx->frequency = tone.frequencyHz;
        // SoundEffect volume is a 0-1 fraction, scaled by the synthesizer across
        // its own 0-1023 sample range.
        fx->volume = tone.volume;
        fx->duration = static_cast<float>(tone.durationMs);
        fx->tone.tonePrint = tonePrintFor(tone.waveform);
        // The zeroed effect slots leave the tone at a constant pitch and a flat
        // volume: a null effect function is skipped by the synthesizer.
        uBit_.audio.soundExpressions.playAsync(sound);
    }

    /**
     * Settles the held play's handle once its nominal duration has elapsed
     * (enqueue-only; the think loop resumes the waiter). Call once per
     * host-loop tick before the brain thinks.
     */
    void pollSpeaker()
    {
        if (!busy_)
        {
            return;
        }
        const uint32_t now = static_cast<uint32_t>(system_timer_current_time());
        if (now < completionTime_)
        {
            return;
        }
        const AsyncHandle done = active_;
        busy_ = false;
        done.resolve(kVoidValue);
    }

    void preempt() override
    {
        if (!busy_)
        {
            return;
        }
        const AsyncHandle held = active_;
        busy_ = false;
        uBit_.audio.soundExpressions.stop();
        held.resolve(kVoidValue);
    }

private:
    MicroBit &uBit_;
    bool busy_ = false;
    uint32_t completionTime_ = 0;
    AsyncHandle active_{};

    /** The synthesizer tone function generating `waveform`. */
    static TonePrintFunction tonePrintFor(SpeakerToneWaveform waveform)
    {
        switch (waveform)
        {
        case SpeakerToneWaveform::Square:
            return Synthesizer::SquareWaveTone;
        case SpeakerToneWaveform::Sawtooth:
            return Synthesizer::SawtoothTone;
        case SpeakerToneWaveform::Sine:
            return Synthesizer::SineTone;
        default:
            return Synthesizer::TriangleTone;
        }
    }
};

/** Reads button levels: index 0 is button A, 1 is button B, 2 is the touch logo. */
class MicroBitButtonInputPort : public ButtonInputPort
{
public:
    explicit MicroBitButtonInputPort(MicroBit &uBit) : uBit_(uBit) {}

    bool isPressed(uint8_t buttonIndex) override
    {
        switch (static_cast<MicroBitButtonIndex>(buttonIndex))
        {
        case MicroBitButtonIndex::A:
            return uBit_.buttonA.isPressed() != 0;
        case MicroBitButtonIndex::B:
            return uBit_.buttonB.isPressed() != 0;
        case MicroBitButtonIndex::Logo:
            return uBit_.logo.isPressed() != 0;
        default:
            return false;
        }
    }

private:
    MicroBit &uBit_;
};

/**
 * Reads the accelerometer through `MicroBitAccelerometer`: the last recognized
 * gesture code plus the live acceleration (milli-g) and rotation-compensated
 * orientation (degrees). Each read returns CODAL's current value.
 */
class MicroBitAccelerometerInputPort : public AccelerometerInputPort
{
public:
    explicit MicroBitAccelerometerInputPort(MicroBit &uBit) : uBit_(uBit) {}

    uint16_t getGesture() override
    {
        // The value getters call requestUpdate() internally, but CODAL's getGesture()
        // does not; without a requestUpdate() the accelerometer never enables its idle
        // sampling, so gesture detection never runs and getGesture() stays NONE.
        uBit_.accelerometer.requestUpdate();
        return uBit_.accelerometer.getGesture();
    }

    int32_t getX() override { return uBit_.accelerometer.getX(); }

    int32_t getY() override { return uBit_.accelerometer.getY(); }

    int32_t getZ() override { return uBit_.accelerometer.getZ(); }

    int32_t getPitch() override { return uBit_.accelerometer.getPitch(); }

    int32_t getRoll() override { return uBit_.accelerometer.getRoll(); }

    mc_number_t getPitchRadians() override { return uBit_.accelerometer.getPitchRadians(); }

    mc_number_t getRollRadians() override { return uBit_.accelerometer.getRollRadians(); }

private:
    MicroBit &uBit_;
};

/**
 * Reads the die temperature through `MicroBitThermometer`: the current
 * temperature in whole degrees Celsius (signed), as CODAL's `getTemperature()`
 * reports it.
 */
class MicroBitThermometerInputPort : public ThermometerInputPort
{
public:
    explicit MicroBitThermometerInputPort(MicroBit &uBit) : uBit_(uBit) {}

    int32_t getTemperature() override { return uBit_.thermometer.getTemperature(); }

private:
    MicroBit &uBit_;
};

/** Drives the external I2C bus through CODAL's `MicroBitI2C`. */
class MicroBitI2CPort : public I2CPort
{
public:
    explicit MicroBitI2CPort(MicroBit &uBit) : uBit_(uBit) {}

    int write(uint16_t address, const uint8_t *data, int len) override
    {
        // CODAL's I2C::write takes the 8-bit address: the 7-bit address shifted up
        // one, with the R/W bit clear.
        return uBit_.i2c.write(static_cast<uint16_t>(address << 1), const_cast<uint8_t *>(data),
                               len, false);
    }

    int read(uint16_t address, uint8_t *data, int len) override
    {
        // CODAL's I2C::read takes the 8-bit address, matching write.
        return uBit_.i2c.read(static_cast<uint16_t>(address << 1), data, len, false);
    }

private:
    MicroBit &uBit_;
};

/**
 * Drives the edge-connector GPIO pins through CODAL's `MicroBitIO`: simple
 * synchronous digital read/write, pull configuration, and servo output. Pins are
 * addressed by number 0-20 and resolved to their `NRF52Pin` through a lookup
 * table; a pin outside 0-20 is a no-op (a read returns 0).
 */
class MicroBitGPIOPort : public GPIOPort
{
public:
    explicit MicroBitGPIOPort(MicroBit &uBit)
        : pins_{&uBit.io.P0,  &uBit.io.P1,  &uBit.io.P2,  &uBit.io.P3,  &uBit.io.P4,  &uBit.io.P5,
                &uBit.io.P6,  &uBit.io.P7,  &uBit.io.P8,  &uBit.io.P9,  &uBit.io.P10, &uBit.io.P11,
                &uBit.io.P12, &uBit.io.P13, &uBit.io.P14, &uBit.io.P15, &uBit.io.P16, nullptr,
                nullptr,      &uBit.io.P19, &uBit.io.P20}
    {
    }

    int digitalRead(int pin) override
    {
        NRF52Pin *p = pinFor(pin);
        return p != nullptr ? p->getDigitalValue() : 0;
    }

    int digitalWrite(int pin, int value) override
    {
        NRF52Pin *p = pinFor(pin);
        if (p != nullptr)
        {
            p->setDigitalValue(value != 0 ? 1 : 0);
        }
        return 0;
    }

    int setPull(int pin, int mode) override
    {
        NRF52Pin *p = pinFor(pin);
        if (p == nullptr)
        {
            return 0;
        }
        switch (mode)
        {
        case 0:
            p->setPull(codal::PullMode::None);
            break;
        case 1:
            p->setPull(codal::PullMode::Up);
            break;
        case 2:
            p->setPull(codal::PullMode::Down);
            break;
        default:
            // An unrecognized mode is a no-op.
            break;
        }
        return 0;
    }

    int setServo(int pin, int angle) override
    {
        NRF52Pin *p = pinFor(pin);
        if (p != nullptr)
        {
            p->setServoValue(angle);
        }
        return 0;
    }

    int analogRead(int pin) override
    {
        NRF52Pin *p = pinFor(pin);
        return p != nullptr ? p->getAnalogValue() : 0;
    }

private:
    /** Highest addressable pin number; the lookup-table bound. */
    static constexpr int kMaxPin = 20;

    NRF52Pin *pins_[kMaxPin + 1];

    /**
     * The `NRF52Pin` for `pin` (0-20), or nullptr when `pin` is out of range or is
     * P17/P18 (the board's 3V power pins, which are not GPIO).
     */
    NRF52Pin *pinFor(int pin) { return pin >= 0 && pin <= kMaxPin ? pins_[pin] : nullptr; }
};

/**
 * Drives SR04-style ultrasonic sonars through one shared background CODAL fiber.
 * Sonars are addressed by their (trigger, echo) pins: {@link distance} registers
 * a sonar on the first reference to a pin pair (appending a node and starting the
 * shared fiber on the first registration) and returns its cached distance; later
 * references to the same pins read the same cache. The fiber services each
 * registered sonar in turn - it pulses the trigger (a ~10 us synchronous blip),
 * then measures the echo's HIGH-pulse width with `NRF52Pin::getPulseUs`, which
 * blocks (yields) the fiber until the pulse completes or the timeout caps it, so
 * the VM keeps running during the wait. A lone echo dropout (common while a target
 * moves) holds the last good distance; the cache reports the maximum only after
 * several consecutive timeouts. Valid readings feed an exponential moving average
 * that smooths per-ping jitter. It writes the cached distance and never re-enters
 * the VM.
 */
class MicroBitSonarPort : public SonarPort
{
public:
    explicit MicroBitSonarPort(MicroBit &uBit)
        : pins_{&uBit.io.P0,  &uBit.io.P1,  &uBit.io.P2,  &uBit.io.P3,  &uBit.io.P4,  &uBit.io.P5,
                &uBit.io.P6,  &uBit.io.P7,  &uBit.io.P8,  &uBit.io.P9,  &uBit.io.P10, &uBit.io.P11,
                &uBit.io.P12, &uBit.io.P13, &uBit.io.P14, &uBit.io.P15, &uBit.io.P16, nullptr,
                nullptr,      &uBit.io.P19, &uBit.io.P20}
    {
    }

    int distance(int trig, int echo) override
    {
        Sonar *sonar = registerSonar(trig, echo);
        return sonar != nullptr ? sonar->cacheCm : kMaxDistanceCm;
    }

private:
    /** Highest addressable pin number; the lookup-table bound. */
    static constexpr int kMaxPin = 20;

    /** Distance reported before a measurement completes, on timeout, and as the clamp ceiling. */
    static constexpr int kMaxDistanceCm = 200;

    /** Echo timeout in microseconds: the cap that bounds the usable range to ~200 cm. */
    static constexpr int kEchoTimeoutUs = 12000;

    /** Delay between measurement sweeps in milliseconds; lets echoes settle between pings. */
    static constexpr int kSettleMs = 30;

    /** Consecutive echo timeouts before a sonar reports max distance; rejects lone dropouts. */
    static constexpr int kMaxMisses = 3;

    /** Exponential-moving-average weight on each new reading (0-1); smaller is smoother but
     * laggier. */
    static constexpr float kEmaAlpha = 0.7f;

    /**
     * A registered sonar: its pins, the cached distance the VM reads, the smoothed
     * distance the average tracks, and the consecutive-miss count.
     */
    struct Sonar
    {
        int trig;
        int echo;
        NRF52Pin *trigPin;
        NRF52Pin *echoPin;
        volatile int cacheCm;
        float smoothCm;
        int missCount;
        Sonar *next;
    };

    NRF52Pin *pins_[kMaxPin + 1];
    Sonar *head_ = nullptr;
    bool fiberStarted_ = false;

    /**
     * The `NRF52Pin` for `pin` (0-20), or nullptr when `pin` is out of range or is
     * P17/P18 (the board's 3V power pins, which are not GPIO).
     */
    NRF52Pin *pinFor(int pin) { return pin >= 0 && pin <= kMaxPin ? pins_[pin] : nullptr; }

    /** Distance in centimeters for an echo width (us), clamped to the maximum. */
    static int distanceCm(int echoMicros)
    {
        const int cm = echoMicros * 34 / 2 / 1000;
        return cm > kMaxDistanceCm ? kMaxDistanceCm : cm;
    }

    /** Find-or-register the sonar wired to `trig`/`echo`, starting the fiber on the first one. */
    Sonar *registerSonar(int trig, int echo)
    {
        for (Sonar *s = head_; s != nullptr; s = s->next)
        {
            if (s->trig == trig && s->echo == echo)
            {
                return s;
            }
        }
        NRF52Pin *trigPin = pinFor(trig);
        NRF52Pin *echoPin = pinFor(echo);
        if (trigPin == nullptr || echoPin == nullptr)
        {
            return nullptr;
        }
        // Measure the echo's HIGH-pulse width. getPulseUs reads the polarity once on
        // its first call, so set it before the fiber takes its first measurement.
        echoPin->setPolarity(1);
        Sonar *sonar = new Sonar{trig,    echo,           trigPin,
                                 echoPin, kMaxDistanceCm, static_cast<float>(kMaxDistanceCm),
                                 0,       head_};
        head_ = sonar;
        if (!fiberStarted_)
        {
            fiberStarted_ = true;
            create_fiber(&MicroBitSonarPort::fiberEntry, this);
        }
        return sonar;
    }

    /** Trampoline into {@link run} for `create_fiber`. */
    static void fiberEntry(void *param) { static_cast<MicroBitSonarPort *>(param)->run(); }

    /** The shared background loop: measure each registered sonar in turn, forever. */
    [[noreturn]] void run()
    {
        while (true)
        {
            for (Sonar *s = head_; s != nullptr; s = s->next)
            {
                measure(s);
            }
            fiber_sleep(kSettleMs);
        }
    }

    /** One measurement for `sonar`: pulse the trigger, time the echo, cache the distance. */
    void measure(Sonar *sonar)
    {
        sonar->trigPin->setPull(codal::PullMode::None);
        sonar->trigPin->setDigitalValue(0);
        waitMicros(2);
        sonar->trigPin->setDigitalValue(1);
        waitMicros(10);
        sonar->trigPin->setDigitalValue(0);
        // getPulseUs yields the fiber until the echo's HIGH pulse completes or the
        // timeout caps it (returning DEVICE_CANCELLED, a negative value), so the VM
        // keeps running during the wait. A moving target scatters the ping, so single
        // echoes drop out; hold the last good distance and only report the maximum
        // after kMaxMisses consecutive timeouts, rejecting those lone dropouts. A
        // valid reading feeds the exponential moving average that smooths the
        // remaining per-ping jitter.
        const int echoMicros = sonar->echoPin->getPulseUs(kEchoTimeoutUs);
        if (echoMicros >= 0)
        {
            sonar->smoothCm = kEmaAlpha * static_cast<float>(distanceCm(echoMicros)) +
                              (1.0f - kEmaAlpha) * sonar->smoothCm;
            sonar->cacheCm = static_cast<int>(sonar->smoothCm + 0.5f);
            sonar->missCount = 0;
        }
        else if (++sonar->missCount >= kMaxMisses)
        {
            sonar->smoothCm = static_cast<float>(kMaxDistanceCm);
            sonar->cacheCm = kMaxDistanceCm;
        }
    }

    /** Busy-wait `micros` microseconds: the negligible synchronous trigger blip. */
    static void waitMicros(uint32_t micros)
    {
        const CODAL_TIMESTAMP start = system_timer_current_time_us();
        while (system_timer_current_time_us() - start < micros)
        {
        }
    }
};

/**
 * Drives the 2.4 GHz packet radio through CODAL's `MicroBitRadio`. Send encodes a
 * typed packet to its MakeCode-compatible on-air frame (or sends raw bytes for a
 * raw datagram) and calls `datagram.send`, which blocks until transmission
 * completes. Received packets are drained from CODAL's own RX queue into a
 * bounded depth-{@link RadioPort::RADIO_RX_RING_DEPTH} ring by {@link pollRx},
 * which the host loop calls once per tick before the brain thinks (enqueue-only;
 * the VM reads the ring on its next think). Each ring entry keeps its raw frame
 * and is decoded on demand by {@link ringAt}. The radio is demand-activated: it
 * is enabled lazily on the first send, config call, or poll. The nRF radio and
 * the BLE stack are mutually exclusive; this firmware runs no BLE stack, so
 * `enable()` succeeds.
 */
class MicroBitRadioPort : public RadioPort
{
public:
    explicit MicroBitRadioPort(MicroBit &uBit) : uBit_(uBit) {}

    void send(const RadioSendView &packet) override
    {
        ensureEnabled();
        uint8_t frame[kRadioMaxPacketSize];
        if (packet.type == kRadioRawPacketType)
        {
            uint32_t len =
                packet.bytesLen < kRadioMaxPacketSize ? packet.bytesLen : kRadioMaxPacketSize;
            for (uint32_t i = 0; i < len; i++)
            {
                frame[i] = packet.bytes[i];
            }
            uBit_.radio.datagram.send(frame, static_cast<int>(len));
            return;
        }
        RadioFrameInput in{};
        in.type = static_cast<RadioPacketType>(packet.type);
        in.value = packet.value;
        in.name = packet.name;
        in.nameLen = packet.nameLen;
        in.text = packet.text;
        in.textLen = packet.textLen;
        in.bytes = packet.bytes;
        in.bytesLen = packet.bytesLen;
        // System time is the sender's running time; serial is 0 (transmit-serial
        // is not enabled), matching the MakeCode wire-format metadata defaults.
        encodeRadioFrame(in, static_cast<int32_t>(system_timer_current_time()), 0, frame);
        uBit_.radio.datagram.send(frame, static_cast<int>(kRadioMaxPacketSize));
    }

    uint8_t group() override { return groupValue_; }

    void setGroup(int group) override
    {
        ensureEnabled();
        groupValue_ = static_cast<uint8_t>(group);
        uBit_.radio.setGroup(static_cast<uint8_t>(group));
    }

    void setTransmitPower(int power) override
    {
        ensureEnabled();
        uBit_.radio.setTransmitPower(power);
    }

    void setFrequencyBand(int band) override
    {
        ensureEnabled();
        uBit_.radio.setFrequencyBand(band);
    }

    uint32_t ringSize() override { return count_; }

    const RadioPacketView &ringAt(uint32_t index) override
    {
        const Slot &slot = ring_[(head_ + index) % RADIO_RX_RING_DEPTH];
        const RadioDecodedFrame decoded = decodeRadioFrame(slot.frame);
        scratch_.seq = slot.seq;
        scratch_.type = decoded.type;
        scratch_.group = slot.group;
        scratch_.value = decoded.value;
        scratch_.name = decoded.name;
        scratch_.nameLen = decoded.nameLen;
        scratch_.text = decoded.text;
        scratch_.textLen = decoded.textLen;
        scratch_.bytes = decoded.bytes;
        scratch_.bytesLen = decoded.bytesLen;
        scratch_.rssi = slot.rssi;
        scratch_.serial = decoded.serial;
        scratch_.time = decoded.time;
        return scratch_;
    }

    int headSequence() override { return lastSeq_; }

    /**
     * Drains every packet CODAL has queued into the receive ring, assigning each
     * the next arrival sequence and stamping its RSSI, evicting the oldest on
     * overflow. Enqueue-only: it never re-enters the VM. Call once per host-loop
     * tick before the brain thinks.
     */
    void pollRx()
    {
        ensureEnabled();
        // Drain the datagram queue directly. CODAL's `dataReady()` reports the
        // radio-level queue, which its idle callback empties into the separate
        // datagram queue between ticks; recv() returns EmptyPacket when drained.
        while (true)
        {
            PacketBuffer packet = uBit_.radio.datagram.recv();
            if (packet == PacketBuffer::EmptyPacket)
            {
                break;
            }
            pushPacket(packet.getBytes(), packet.length(), packet.getRSSI());
        }
    }

private:
    /**
     * One received packet's raw on-air frame plus the fields not carried in it:
     * the arrival sequence, the received signal strength, and the group it landed
     * on. The frame is decoded on demand by {@link ringAt}.
     */
    struct Slot
    {
        uint8_t frame[kRadioMaxPacketSize];
        int seq;
        int rssi;
        uint8_t group;
    };

    MicroBit &uBit_;
    bool enabled_ = false;
    uint8_t groupValue_ = 0;
    Slot ring_[RADIO_RX_RING_DEPTH];
    uint32_t head_ = 0;
    uint32_t count_ = 0;
    int lastSeq_ = 0;
    RadioPacketView scratch_{};

    /** Enable the demand-activated CODAL radio on first use. */
    void ensureEnabled()
    {
        if (!enabled_)
        {
            uBit_.radio.enable();
            enabled_ = true;
        }
    }

    /** Append a received frame to the ring with the next sequence, evicting the oldest on overflow.
     */
    void pushPacket(const uint8_t *bytes, int length, int rssi)
    {
        uint32_t slotIndex;
        if (count_ < RADIO_RX_RING_DEPTH)
        {
            slotIndex = (head_ + count_) % RADIO_RX_RING_DEPTH;
            count_++;
        }
        else
        {
            slotIndex = head_;
            head_ = (head_ + 1) % RADIO_RX_RING_DEPTH;
        }
        Slot &slot = ring_[slotIndex];
        uint32_t len = length < 0 ? 0 : static_cast<uint32_t>(length);
        if (len > kRadioMaxPacketSize)
        {
            len = kRadioMaxPacketSize;
        }
        std::memset(slot.frame, 0, sizeof(slot.frame));
        for (uint32_t i = 0; i < len; i++)
        {
            slot.frame[i] = bytes[i];
        }
        slot.seq = ++lastSeq_;
        slot.rssi = rssi;
        slot.group = groupValue_;
    }
};

/** Monotonic millisecond clock backed by the CODAL system timer. */
class MicroBitMonotonicClockPort : public MonotonicClockPort
{
public:
    uint32_t uptimeMillis() override { return static_cast<uint32_t>(system_timer_current_time()); }
};

/**
 * Renders the device fault mode on the LED matrix: a sad face, then the
 * diagnostic code scrolled once. The code is also printed to serial once, the
 * first time it is shown.
 */
class MicroBitFaultDisplayPort : public FaultDisplayPort
{
public:
    explicit MicroBitFaultDisplayPort(MicroBit &uBit) : uBit_(uBit) {}

    void showFaultFace() override
    {
        uBit_.display.setBrightness(kFaultBrightness);
        uBit_.display.print(sadFace());
        uBit_.sleep(kFaultFaceHoldMs);
        uBit_.display.clear();
        uBit_.sleep(kFaultBlankMs);
    }

    void scrollFaultCode(const char *code) override
    {
        if (!serialReported_)
        {
            uBit_.serial.printf("wendoo-mcu fault: code=%s\r\n", code);
            serialReported_ = true;
        }
        uBit_.display.scroll(ManagedString(code), kScrollDelayMs);
        uBit_.display.clear();
        uBit_.sleep(kFaultBlankMs);
    }

private:
    /** Display brightness for fault rendering (0-255). */
    static constexpr int kFaultBrightness = 170;

    /** Milliseconds the sad face holds before the screen clears. */
    static constexpr int kFaultFaceHoldMs = 600;

    /** Milliseconds the screen stays blank between fault frames. */
    static constexpr int kFaultBlankMs = 200;

    /** Milliseconds per scroll step; larger scrolls slower. */
    static constexpr int kScrollDelayMs = 200;

    /** A 5x5 sad face: two eyes over a downturned mouth. */
    static MicroBitImage sadFace()
    {
        MicroBitImage image(5, 5);
        image.setPixelValue(1, 0, 255);
        image.setPixelValue(3, 0, 255);
        image.setPixelValue(1, 3, 255);
        image.setPixelValue(2, 3, 255);
        image.setPixelValue(3, 3, 255);
        image.setPixelValue(0, 4, 255);
        image.setPixelValue(4, 4, 255);
        return image;
    }

    MicroBit &uBit_;
    bool serialReported_ = false;
};

} // namespace wendoo
