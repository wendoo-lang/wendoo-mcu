declare module "wendoo" {
  interface WendooTypeMap {
    MicroBitDisplay: MicroBitDisplay;
    Thermometer: Thermometer;
    SoundEmoji: SoundEmoji;
    PlaySoundOptions: PlaySoundOptions;
    PlayToneOptions: PlayToneOptions;
    DrawImageOptions: DrawImageOptions;
    ScrollTextOptions: ScrollTextOptions;
    MicroBitAudio: MicroBitAudio;
    MicroBit: MicroBit;
  }

  export interface MicroBitDisplay {
    readonly __brand: unique symbol;
    setPixelValue(x: number, y: number, brightness: number): void;
    getPixelValue(x: number, y: number): number;
    clear(): void;
    drawImage(image: Image, options?: DrawImageOptions): Promise<void>;
    scrollText(text: string, options?: ScrollTextOptions): Promise<void>;
    getLightLevel(): number;
  }
  export interface Thermometer {
    readonly __brand: unique symbol;
    getTemperature(): number;
  }
  export interface SoundEmoji {
    name: string;
  }
  export interface PlaySoundOptions {
    immediately?: boolean;
    inBackground?: boolean;
  }
  export interface PlayToneOptions {
    duration?: number;
    volume?: number;
    waveform?: string;
    immediately?: boolean;
    inBackground?: boolean;
  }
  export interface DrawImageOptions {
    duration?: number;
    immediately?: boolean;
    inBackground?: boolean;
  }
  export interface ScrollTextOptions {
    immediately?: boolean;
    inBackground?: boolean;
  }
  export interface MicroBitAudio {
    readonly __brand: unique symbol;
    playSound(sound: string, options?: PlaySoundOptions): Promise<void>;
    playTone(frequency?: number, options?: PlayToneOptions): Promise<void>;
  }
  export interface MicroBit {
    readonly __brand: unique symbol;
    readonly display: MicroBitDisplay;
    readonly buttonA: Button;
    readonly buttonB: Button;
    readonly logo: TouchButton;
    readonly accelerometer: Accelerometer;
    readonly i2c: I2C;
    readonly gpio: GPIO;
    readonly sonar: Sonar;
    readonly radio: Radio;
    readonly audio: MicroBitAudio;
    readonly thermometer: Thermometer;
  }
  export interface Context {
    readonly microbit: MicroBit;
  }
}
