#include "MicroBit.h"

#include "codal/device-port.h"
#include "codal/fault-mode.h"
#include "codal/host-loop.h"
#include "codal/on-flash-region.h"
#include "codal/shared-type-atom-id.h"
#include "core/codec/program-reader.h"
#include "core/platform/span.h"
#include "core/runtime/brain-runtime.h"
#include "core/runtime/core-host-functions.h"
#include "core/runtime/execution-context.h"
#include "core/runtime/fiber-scheduler.h"
#include "core/runtime/host-actions/core-host-action-bindings.h"
#include "core/runtime/load-error.h"
#include "core/runtime/managed-heap.h"
#include "core/runtime/region-arena.h"
#include "core/runtime/result.h"
#include "core/runtime/type-registry.h"
#include "core/runtime/value.h"
#include "core/runtime/vm.h"
#include "targets/microbit-v2/abi/device-profile.h"
#include "targets/microbit-v2/abi/host-actions/host-action-bindings.h"
#include "targets/microbit-v2/abi/host-functions/host-func-bindings.h"
#include "targets/microbit-v2/abi/native-struct-bindings.h"
#include "targets/microbit-v2/abi/registered-structs.h"
#include "targets/microbit-v2/abi/type-atom-id.h"

#include "microbit-ports.h"
#include "program-region.h"

#include <array>
#include <cstddef>

MicroBit uBit;

namespace
{

using namespace wendoo;

/** Milliseconds the firmware sleeps between host-loop ticks. */
constexpr int kTickIntervalMs = 16;

/** Bytes of RAM the VM region reserves. */
constexpr uint32_t kVmRegionBytes = 32u * 1024u;

/**
 * Backing storage for the VM's RegionArena: the decoded program image and the
 * scheduler's fiber pools are drawn from it on demand. A brain that outgrows it
 * faults `ErrorCode::StackOverflow` at spawn.
 */
uint8_t g_vmRegion[kVmRegionBytes];

/** Renders the device fault mode forever; never returns. */
[[noreturn]] void faultLoop(FaultDisplayPort &faultDisplay, FaultDomain domain, uint16_t code)
{
    char text[kFaultCodeSize];
    formatFaultCode(text, domain, code);
    while (true)
    {
        showFaultPass(faultDisplay, text);
    }
}

} // namespace

/**
 * Boots CODAL, reads the brain image from the reserved on-flash region, decodes
 * it in place, and runs the host loop: button A toggles a display pixel. An
 * erased/invalid region, a decode/load failure, or a startup fault enters the
 * device fault mode (sad face, then the scrolled diagnostic code) with a stable
 * domain-prefixed code (R region, L load, E runtime).
 */
int main()
{
    uBit.init();
    uBit.serial.printf("mc boot\r\n");

    MicroBitPixelDisplayPort display(uBit);
    MicroBitButtonInputPort buttons(uBit);
    MicroBitAccelerometerInputPort accelerometer(uBit);
    MicroBitThermometerInputPort thermometer(uBit);
    MicroBitI2CPort i2c(uBit);
    MicroBitGPIOPort gpio(uBit);
    MicroBitSonarPort sonar(uBit);
    MicroBitRadioPort radio(uBit);
    MicroBitSpeakerPort speaker(uBit);
    MicroBitMonotonicClockPort clock;
    MicroBitFaultDisplayPort faultDisplay(uBit);
    DevicePorts ports{&display, &buttons, &faultDisplay, &clock,   &accelerometer, &i2c,
                      &gpio,    &sonar,   &radio,        &speaker, &thermometer};

    // Read the brain image from the reserved on-flash region.
    const Result<ByteSpan, RegionError> region = readRegionProgram(programFlashRegion());
    if (!region.isOk())
    {
        uBit.serial.printf("mc R%d\r\n", static_cast<int>(region.error()));
        faultLoop(faultDisplay, FaultDomain::Region, static_cast<uint16_t>(region.error()));
    }

    RegionArena arena(Span<uint8_t>(g_vmRegion, sizeof(g_vmRegion)));
    constexpr ProgramReaderOptions options{kMicroBitV2TypeAtomIdCount, kSharedTypeAtomIdCount};
    const Result<ProgramImage, LoadError> decoded =
        readProgramImage(region.value(), arena, options);
    if (!decoded.isOk())
    {
        uBit.serial.printf("mc L%d\r\n", static_cast<int>(decoded.error()));
        faultLoop(faultDisplay, FaultDomain::Load, static_cast<uint16_t>(decoded.error()));
    }
    const ProgramImage &image = decoded.value();

    // Reject a program built for any other device profile before running it.
    if (image.profileId != kMicroBitV2NumericProfileId)
    {
        uBit.serial.printf("mc L%d\r\n", static_cast<int>(LoadError::UnsupportedDeviceProfile));
        faultLoop(faultDisplay, FaultDomain::Load,
                  static_cast<uint16_t>(LoadError::UnsupportedDeviceProfile));
    }

    // The firmware action table is the core sensor/actuator surface (ids 0-9)
    // followed by the microbit-v2 host actions (ids 1024+). The core bodies
    // reach the brain, RNG, heap, and scheduler through coreEnv, filled once the
    // scheduler and brain exist.
    CoreHostActionEnv coreEnv;
    VmRng rng;
    // The async scroll bodies (the display-text tile action and the scrollText
    // host function) reach the display and the heap (to read their text string)
    // through this env; its heap is filled once the heap exists.
    MicroBitV2DisplayScrollEnv scrollEnv{&display, nullptr};
    // The async draw-image body reaches the display, the heap (to read the Image
    // struct and a managed pixel buffer), and the program (to resolve a borrowed
    // pixel buffer); its heap is filled once the heap exists.
    MicroBitV2DrawImageEnv drawEnv{&display, nullptr, &image};
    // The async play-sound bodies (the play-sound tile action and the
    // audio.playSound host function) reach the speaker and the heap (to read
    // the sound name string); the heap is filled once the heap exists.
    MicroBitV2PlaySoundEnv playSoundEnv{&speaker, nullptr};
    // The audio.playTone host function reaches the speaker and the heap (to read
    // its options struct and waveform name); the heap is filled once it exists.
    MicroBitV2PlayToneEnv playToneEnv{&speaker, nullptr};
    // The I2C write body reaches the bus port, the heap (to resolve a managed
    // Buffer argument), and the program (to resolve a borrowed one); its heap is
    // filled once the heap exists.
    MicroBitV2I2CWriteEnv i2cWriteEnv{&i2c, nullptr, &image};
    // The I2C read body reaches the bus port and the heap plus roots to allocate
    // the managed Buffer it returns; its heap and roots are filled once the
    // scheduler exists.
    MicroBitV2I2CReadEnv i2cReadEnv{&i2c, nullptr, nullptr};
    // The button sensors poll the button port and back their per-callsite state
    // on the heap; the heap and roots are filled once the scheduler exists.
    MicroBitV2ButtonSensorEnv buttonEnv{&buttons, nullptr, nullptr};
    // The radio send tile reads a string value off the heap; its heap is filled
    // once the heap exists.
    MicroBitV2RadioSendEnv radioSendEnv{&radio, nullptr};
    // The typed radio receive sensors read the ring and allocate their managed
    // string results and output rule variables; their heap and roots are filled
    // once the scheduler exists.
    MicroBitV2RadioSensorEnv radioSensorEnv{&radio, nullptr, nullptr};
    // The radio send / config host functions resolve managed string / buffer
    // arguments off the heap; its heap is filled once the heap exists.
    MicroBitV2RadioEnv radioEnv{&radio, nullptr};
    // The radio receive(since) host function allocates the managed RadioPacket[]
    // it returns and resolves its struct/list type-atoms; its heap, roots, and
    // type registry are filled once they exist.
    MicroBitV2RadioReceiveEnv radioReceiveEnv{&radio, nullptr, nullptr, nullptr};
    auto coreBindings = makeCoreHostActionBindings(coreEnv);
    auto mbBindings = makeMicroBitV2HostActionBindings(
        ports, &scrollEnv, &buttonEnv, &drawEnv, &radioSendEnv, &radioSensorEnv, &playSoundEnv);
    std::array<HostActionBinding, kCoreHostActionBindingCount + kMicroBitV2HostActionBindingCount>
        actions{};
    for (size_t i = 0; i < coreBindings.size(); i++)
    {
        actions[i] = coreBindings[i];
    }
    for (size_t i = 0; i < mbBindings.size(); i++)
    {
        actions[coreBindings.size() + i] = mbBindings[i];
    }
    auto hostFuncs =
        makeMicroBitV2HostFuncBindings(ports, &drawEnv, &i2cWriteEnv, &i2cReadEnv, &radioEnv,
                                       &radioReceiveEnv, &scrollEnv, &playSoundEnv, &playToneEnv);
    ManagedHeap heap(arena, &image);
    TypeRegistry types(image);
    auto nativeStructs = makeMicroBitV2NativeStructBindings(types);
    types.setNativeStructBindings({nativeStructs.data(), nativeStructs.size()});
    auto registeredStructs = makeMicroBitV2RegisteredStructSlotCounts();
    types.setRegisteredStructSlotCounts({registeredStructs.data(), registeredStructs.size()});
    ExecutionContext ctx;
    RuntimeSurface surface{&ctx, {actions.data(), actions.size()}, nullptr, &heap};
    surface.types = &types;
    surface.hostFunctions = {hostFuncs.data(), hostFuncs.size()};
    surface.rng = &rng;
    FiberScheduler scheduler(image, surface, arena, kMicroBitV2DeviceProfileCaps);
    BrainRuntime brain(image, scheduler, surface);
    coreEnv.brain = &brain;
    coreEnv.rng = &rng;
    coreEnv.heap = &heap;
    coreEnv.roots = &scheduler;
    coreEnv.program = &image;
    coreEnv.ruleLiveness = &scheduler;
    scrollEnv.heap = &heap;
    drawEnv.heap = &heap;
    playSoundEnv.heap = &heap;
    playToneEnv.heap = &heap;
    i2cWriteEnv.heap = &heap;
    i2cReadEnv.heap = &heap;
    i2cReadEnv.roots = &scheduler;
    buttonEnv.heap = &heap;
    buttonEnv.roots = &scheduler;
    radioSendEnv.heap = &heap;
    radioSensorEnv.heap = &heap;
    radioSensorEnv.roots = &scheduler;
    radioEnv.heap = &heap;
    radioReceiveEnv.heap = &heap;
    radioReceiveEnv.roots = &scheduler;
    radioReceiveEnv.types = &types;

    HostLoop hostLoop(brain, ports);
    const wendoo::Status startupStatus = hostLoop.startup();
    if (!startupStatus.isOk())
    {
        uBit.serial.printf("mc E%d\r\n", static_cast<int>(startupStatus.error()));
    }
    else
    {
        uBit.serial.printf("mc run\r\n");
    }

    while (true)
    {
        // Settle any completed scroll, timed draw, or speaker play, and drain any
        // radio packets that arrived, before the brain thinks, so an awaiting rule
        // resumes and a freshly received packet is in the ring on this round's
        // drain.
        display.pollDisplay();
        speaker.pollSpeaker();
        radio.pollRx();
        hostLoop.tick();
        uBit.sleep(kTickIntervalMs);
    }
}
