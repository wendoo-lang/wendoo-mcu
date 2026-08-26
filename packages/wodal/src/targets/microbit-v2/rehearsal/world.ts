import type { OperationEnding, ScenarioInput, ScenarioInputKind, SubjectStateChannel } from "@wendoo/assistant-bridge";
import { DispatchOutcome } from "@wendoo/assistant-bridge";
import type { RehearsalWorld, WorldStaging } from "@wendoo/assistant-bridge/kit";
import { AccelerometerGesture } from "../../../core/accelerometer";
import { GestureInjector } from "../../../core/gesture-injector";
import { OperationEnd } from "../../../core/operation-end";
import { decodeRadioFrame, encodeRadioFrame, RadioPacketType, radioNumberIsInteger } from "../../../core/radio";
import { buildWodalProgramImage } from "../../../wendoo/build-kernel";
import { getWodalDeviceProfile, WodalDeviceProfileId } from "../../../wendoo/device-profile";
import { MicroBit } from "../microbit";
import type { DeviceOperationEnding } from "../wendoo/context";
import { getMicroBitContextDevice } from "../wendoo/context";
import { WodalMicroBitRuntime } from "../wendoo/runtime";

/** Simulated milliseconds one think advances the device. */
const THINK_STEP_MS = 16;

/** Signal strength, in -dBm, staged radio messages carry until a scenario sets another. */
const DEFAULT_RSSI = -42;

/** Serial number staged radio messages carry, the value a sender that transmits none leaves. */
const STAGED_SENDER_SERIAL = 0;

/** The gesture each scripted name plays, keyed by the name a scenario spells. */
const GESTURE_NAMES: Readonly<Record<string, AccelerometerGesture>> = {
  none: AccelerometerGesture.None,
  shake: AccelerometerGesture.Shake,
  "tilt up": AccelerometerGesture.TiltUp,
  "tilt down": AccelerometerGesture.TiltDown,
  "tilt left": AccelerometerGesture.TiltLeft,
  "tilt right": AccelerometerGesture.TiltRight,
  "face up": AccelerometerGesture.FaceUp,
  "face down": AccelerometerGesture.FaceDown,
  freefall: AccelerometerGesture.Freefall,
};

/** The device a scenario's percepts are applied to, with the state staging them needs. */
interface StagedDevice {
  readonly device: MicroBit;
  /** Plays scripted motion through the real gesture detector. */
  readonly gestures: GestureInjector;
  /** Signal strength, in -dBm, stamped on every radio message staged from now on. */
  rssi: number;
}

/** One percept channel of the device: what an entry of it delivers, and how it reaches the device. */
interface Percept {
  /** One plain sentence stating what a scripted entry delivers and the range it is read over. */
  readonly description: string;
  /** Applies one scripted entry to a device through its own host-input seam. */
  apply(stage: StagedDevice, value: number | boolean | string): void;
}

/**
 * Delivers one radio message into the device's receive ring, narrowed, capped,
 * and typed as an on-air message is, on the group the device is listening to at
 * the moment of delivery, stamped with the stage's signal strength.
 */
function deliverRadioMessage(stage: StagedDevice, type: RadioPacketType, value: number, text: string): void {
  const radio = stage.device.radio;
  const group = radio.group;
  const frame = encodeRadioFrame(
    { type, group, value, name: "", text, bytes: new Uint8Array() },
    stage.device.systemTime(),
    STAGED_SENDER_SERIAL
  );
  radio.deliver(decodeRadioFrame(frame, group, stage.rssi));
}

/**
 * The percept channels a scenario may script, keyed by the input kind that
 * names each. A level applied by one entry holds until a later entry of the
 * same kind changes it; an arrival is delivered on the think its entry names
 * and on no other.
 */
const PERCEPTS: Readonly<Record<string, Percept>> = {
  "button-a": {
    description: "Whether the A button is held down: true presses it, false releases it.",
    apply: (stage, value) => stage.device.setButtonPressed("A", Boolean(value)),
  },
  "button-b": {
    description: "Whether the B button is held down: true presses it, false releases it.",
    apply: (stage, value) => stage.device.setButtonPressed("B", Boolean(value)),
  },
  "logo-touch": {
    description: "Whether the touch logo is being touched: true holds the touch, false lets go.",
    apply: (stage, value) => stage.device.setLogoTouched(Boolean(value)),
  },
  "light-level": {
    description: "How bright the room is, from 0 (dark) to 255 (bright); a level outside that range is clamped.",
    apply: (stage, value) => stage.device.setLightLevel(Number(value)),
  },
  temperature: {
    description: "How warm the device is, in whole degrees Celsius; it holds until another entry changes it.",
    apply: (stage, value) => stage.device.setTemperature(Number(value)),
  },
  gesture: {
    description:
      'How the device is being moved, named exactly: "shake", "freefall", "tilt up", "tilt down", "tilt left", "tilt right", "face up", "face down", or "none" to set it back down. A shake or a freefall plays once and takes a few thinks to be felt; a tilt or a face holds until another entry changes it.',
    apply: (stage, value) => stage.gestures.select(GESTURE_NAMES[String(value)] ?? AccelerometerGesture.None),
  },
  "radio-message-number": {
    description: "A number arriving over the radio, delivered once, on this think alone.",
    apply: (stage, value) => {
      const number = Number(value);
      const type = radioNumberIsInteger(number) ? RadioPacketType.Number : RadioPacketType.Double;
      deliverRadioMessage(stage, type, number, "");
    },
  },
  "radio-message-string": {
    description: "A word arriving over the radio, delivered once, on this think alone; long text is cut short.",
    apply: (stage, value) => deliverRadioMessage(stage, RadioPacketType.String, 0, String(value)),
  },
  "radio-signal-strength": {
    description: `How strong the radio messages after it come in, as -dBm: near 40 is close by and near 90 is far away. It holds until another entry changes it, and starts at ${-DEFAULT_RSSI}.`,
    apply: (stage, value) => {
      stage.rssi = -Math.abs(Number(value));
    },
  },
};

/** Every percept kind a scenario may script for this target, sorted by name. */
export const PERCEPT_KINDS: readonly ScenarioInputKind[] = Object.entries(PERCEPTS)
  .map(([name, percept]) => ({ name, description: percept.description }))
  .sort((a, b) => (a.name < b.name ? -1 : 1));

/** Channel name the screen's contents are reported under. */
const DISPLAY_CHANNEL = "display";

/** Channel name the speaker is reported under. */
const SPEAKER_CHANNEL = "speaker";

/** Brightness steps a reported pixel is quantised to: one digit, 0 through 9. */
const BRIGHTNESS_STEPS = 10;

/** Largest brightness a pixel holds. */
const MAX_BRIGHTNESS = 255;

/** The value the speaker channel reports while nothing is playing. */
const SPEAKER_IDLE = "idle";

/** What separates the sound the speaker reports from the count of starts behind it. */
const SPEAKER_PLAY_MARK = "#";

/**
 * The sound the speaker stands at, taken from the value the channel reports:
 * the name alone, without the count of starts behind it, so playing the same
 * sound again leaves the speaker where it was.
 */
function speakerSound(reported: string): string {
  const mark = reported.indexOf(SPEAKER_PLAY_MARK);
  return mark === -1 ? reported : reported.slice(0, mark);
}

/**
 * Every state channel this target reports of the device, in the order a delta
 * lists them.
 */
export const STATE_CHANNELS: readonly SubjectStateChannel[] = [
  {
    name: DISPLAY_CHANNEL,
    description:
      "What the 5x5 screen shows, as 25 brightness digits read left to right and top to bottom, one row after the next: 0 is off and 9 is full brightness, so `0000000900000000000000000` is one lit pixel in the middle.",
  },
  {
    name: SPEAKER_CHANNEL,
    description: `The sound playing right now, as its name followed by \`#\` and a number that counts up each time a sound starts, or \`${SPEAKER_IDLE}\` while nothing is playing.`,
    identityValue: speakerSound,
  },
];

/**
 * The screen as 25 brightness digits, row-major, each pixel's 0..255 brightness
 * quantised to one digit.
 */
function renderDisplay(pixels: readonly number[]): string {
  let text = "";
  for (const pixel of pixels) {
    const step = Math.floor((pixel * BRIGHTNESS_STEPS) / (MAX_BRIGHTNESS + 1));
    text += String(step);
  }
  return text;
}

/**
 * How a device operation ending reads as an observation of the call that
 * started it, or `undefined` for an ending the run can already see: an
 * operation its caller waited for reports its end by settling that call's
 * handle.
 */
function publishedEnding(ending: DeviceOperationEnding): OperationEnding | undefined {
  if (ending.end === OperationEnd.Dropped) return DispatchOutcome.Dropped;
  if (ending.end === OperationEnd.Preempted) return DispatchOutcome.Preempted;
  return ending.inBackground ? DispatchOutcome.BackgroundEnd : undefined;
}

/**
 * One device running the brain under study, stepped at a fixed simulated
 * timestep, with the scenario's percepts applied before the think they name.
 */
class DeviceWorld implements RehearsalWorld {
  /** Zero-based index of the think the next {@link step} runs. */
  private think = 0;

  constructor(
    private readonly stage: StagedDevice,
    private readonly runtime: WodalMicroBitRuntime,
    private readonly inputs: readonly ScenarioInput[]
  ) {}

  step(): void {
    for (const input of this.inputs) {
      if (input.at === this.think) {
        PERCEPTS[input.kind].apply(this.stage, input.value);
      }
    }
    this.stage.gestures.advance(THINK_STEP_MS);
    this.runtime.tick(THINK_STEP_MS);
    this.think++;
  }

  /** True: the device is in the world from the first think and never leaves it. */
  subjectPresent(): boolean {
    return true;
  }

  participants(): number {
    return 1;
  }

  brainsExecuted(): number {
    return 1;
  }

  /** The screen and the speaker as they stand at the end of the think just stepped. */
  readState(): ReadonlyMap<string, string> {
    const playing = this.stage.device.speaker.snapshot().playing;
    return new Map([
      [DISPLAY_CHANNEL, renderDisplay(this.stage.device.display.snapshot().pixels)],
      [SPEAKER_CHANNEL, playing === undefined ? SPEAKER_IDLE : `${playing.name}#${playing.playId}`],
    ]);
  }

  shutdown(): void {
    this.runtime.unload();
  }
}

/**
 * Stage one device world: build the brain under study into a program image for
 * the device profile, load it onto a fresh device, report the loaded brain as
 * the participant under study, and publish the endings of the display and
 * speaker operations its calls start. Throws when the brain does not build, the
 * device refuses the image, or the load publishes no event stream.
 */
export function createDeviceWorld(staging: WorldStaging): RehearsalWorld {
  const built = buildWodalProgramImage({
    brainDef: staging.subjectBrain,
    environment: staging.environment,
    deviceProfile: getWodalDeviceProfile(WodalDeviceProfileId.MICROBIT_V2),
  });
  if (!built.ok) {
    throw new Error(`the brain under study did not build: ${built.errors.map((error) => error.code).join(", ")}`);
  }

  const device = new MicroBit();
  const runtime = new WodalMicroBitRuntime({
    environment: staging.environment,
    microbit: device,
    operations: (ending) => {
      const published = publishedEnding(ending);
      if (published !== undefined) {
        staging.publishOperationEnding({ handleId: ending.handleId, ending: published });
      }
    },
  });
  const loaded = runtime.loadWodalProgramImage(built.image);
  if (!loaded.ok) {
    throw new Error(`the device refused the program image: ${loaded.errors.map((error) => error.code).join(", ")}`);
  }

  const events = runtime.brainEvents();
  if (events === undefined) {
    throw new Error("the device published no brain event stream after loading the program image");
  }
  staging.observeSubject({
    brain: { events: () => events },
    runs: (ctx) => getMicroBitContextDevice(ctx) === device,
  });

  return new DeviceWorld(
    { device, gestures: new GestureInjector(device.accelerometer), rssi: DEFAULT_RSSI },
    runtime,
    staging.inputs
  );
}
