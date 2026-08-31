#pragma once

#include <array>
#include <cstdint>

#include "core/runtime/host-action.h"
#include "targets/microbit-v2/abi/host-actions.h"
#include "targets/microbit-v2/abi/host-actions/actuators/display-clear.h"
#include "targets/microbit-v2/abi/host-actions/actuators/display-draw.h"
#include "targets/microbit-v2/abi/host-actions/actuators/display-scroll.h"
#include "targets/microbit-v2/abi/host-actions/actuators/display-set-pixel.h"
#include "targets/microbit-v2/abi/host-actions/actuators/play-sound.h"
#include "targets/microbit-v2/abi/host-actions/actuators/play-tone.h"
#include "targets/microbit-v2/abi/host-actions/actuators/radio-send.h"
#include "targets/microbit-v2/abi/host-actions/actuators/set-radio-group.h"
#include "targets/microbit-v2/abi/host-actions/sensors/button-sensor.h"
#include "targets/microbit-v2/abi/host-actions/sensors/gesture-sensor.h"
#include "targets/microbit-v2/abi/host-actions/sensors/light-level-sensor.h"
#include "targets/microbit-v2/abi/host-actions/sensors/radio-receive.h"
#include "targets/microbit-v2/abi/host-actions/sensors/temperature-sensor.h"

namespace wendoo
{

/** Number of microbit-v2 host-action bindings the slice registers. */
inline constexpr uint32_t kMicroBitV2HostActionBindingCount = 18;

/**
 * Builds the microbit-v2 host-action binding table over `ports`, one entry per
 * action body. The async scroll body uses `scrollEnv` (its display port and
 * heap); the async draw-image body uses `drawEnv` (its display port, heap, and
 * program); the async play-sound body uses `playSoundEnv` (its speaker port and
 * heap); the four button sensors use `buttonEnv` (the button port plus the
 * heap and roots backing their per-callsite state); the gesture sensor reads the
 * accelerometer port, the light-level sensor reads the display port, and the
 * async play-tone body reads the speaker port, each straight off `ports`. Pass null for
 * an env when the table will never dispatch its actions. `ports` and any supplied
 * env must outlive every dispatch through the table.
 */
inline std::array<HostActionBinding, kMicroBitV2HostActionBindingCount>
makeMicroBitV2HostActionBindings(DevicePorts &ports,
                                 MicroBitV2DisplayScrollEnv *scrollEnv = nullptr,
                                 MicroBitV2ButtonSensorEnv *buttonEnv = nullptr,
                                 MicroBitV2DrawImageEnv *drawEnv = nullptr,
                                 MicroBitV2RadioSendEnv *radioSendEnv = nullptr,
                                 MicroBitV2RadioSensorEnv *radioSensorEnv = nullptr,
                                 MicroBitV2PlaySoundEnv *playSoundEnv = nullptr)
{
    return {{
        {MicroBitV2HostActions::ButtonA.actionId, &execButtonA, &buttonSensorPageEntered,
         buttonEnv},
        {MicroBitV2HostActions::DisplaySetPixel.actionId, &execDisplaySetPixel, nullptr, &ports},
        {MicroBitV2HostActions::DisplayScroll.actionId, nullptr, nullptr, scrollEnv,
         &execScrollText},
        {MicroBitV2HostActions::ButtonB.actionId, &execButtonB, &buttonSensorPageEntered,
         buttonEnv},
        {MicroBitV2HostActions::ButtonAB.actionId, &execButtonAB, &buttonSensorPageEntered,
         buttonEnv},
        {MicroBitV2HostActions::ButtonLogo.actionId, &execButtonLogo, &buttonSensorPageEntered,
         buttonEnv},
        {MicroBitV2HostActions::Gesture.actionId, &execGestureSensor, nullptr, &ports},
        {MicroBitV2HostActions::DrawImage.actionId, nullptr, nullptr, drawEnv, &execDrawImage},
        {MicroBitV2HostActions::RadioSend.actionId, &execRadioSend, nullptr, radioSendEnv},
        {MicroBitV2HostActions::RadioReceiveNumber.actionId, &execRadioReceiveNumber,
         &radioReceivePageEntered, radioSensorEnv},
        {MicroBitV2HostActions::RadioReceiveString.actionId, &execRadioReceiveString,
         &radioReceivePageEntered, radioSensorEnv},
        {MicroBitV2HostActions::SetRadioGroup.actionId, &execSetRadioGroup, nullptr, &ports},
        {MicroBitV2HostActions::RadioReceiveBuffer.actionId, &execRadioReceiveBuffer,
         &radioReceivePageEntered, radioSensorEnv},
        {MicroBitV2HostActions::PlaySound.actionId, nullptr, nullptr, playSoundEnv, &execPlaySound},
        {MicroBitV2HostActions::DisplayClear.actionId, &execDisplayClearAction, nullptr, &ports},
        {MicroBitV2HostActions::LightLevel.actionId, &execLightLevelSensor, nullptr, &ports},
        {MicroBitV2HostActions::Temperature.actionId, &execTemperatureSensor, nullptr, &ports},
        {MicroBitV2HostActions::PlayTone.actionId, nullptr, nullptr, &ports, &execPlayTone},
    }};
}

} // namespace wendoo
