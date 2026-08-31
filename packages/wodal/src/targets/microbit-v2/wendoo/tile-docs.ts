/** One tile's documentation entry: the tile it documents, and the file that documents it. */
export interface MicroBitV2TileDocEntry {
  /** Catalog tile id the entry documents. */
  readonly tileId: string;
  /**
   * Base name, without the `.md` extension, of the markdown file under this
   * package's `targets/microbit-v2/docs/<locale>/tiles/` directory.
   */
  readonly contentKey: string;
}

/**
 * Documentation entry for every microbit-v2 catalog tile that ships a markdown
 * page. A tile absent from this list ships none.
 */
export const MICROBIT_V2_TILE_DOCS: readonly MicroBitV2TileDocEntry[] = [
  { tileId: "tile.sensor->microbit-v2.button-a", contentKey: "sensor-button-a" },
  { tileId: "tile.sensor->microbit-v2.button-b", contentKey: "sensor-button-b" },
  { tileId: "tile.sensor->microbit-v2.button-ab", contentKey: "sensor-button-ab" },
  { tileId: "tile.sensor->microbit-v2.button-logo", contentKey: "sensor-logo" },
  { tileId: "tile.sensor->microbit-v2.gesture", contentKey: "sensor-gesture" },
  { tileId: "tile.sensor->microbit-v2.light-level", contentKey: "sensor-light-level" },
  { tileId: "tile.sensor->microbit-v2.temperature", contentKey: "sensor-temperature" },
  { tileId: "tile.sensor->microbit-v2.radio-receive-number", contentKey: "sensor-radio-receive-number" },
  { tileId: "tile.sensor->microbit-v2.radio-receive-string", contentKey: "sensor-radio-receive-string" },
  { tileId: "tile.sensor->microbit-v2.radio-receive-buffer", contentKey: "sensor-radio-receive-buffer" },
  { tileId: "tile.out->number:<number>.value", contentKey: "output-number-value" },
  { tileId: "tile.out->string:<string>.value", contentKey: "output-string-value" },
  { tileId: "tile.out->buffer:<buffer>.value", contentKey: "output-buffer-value" },
  { tileId: "tile.out->number:<number>.rssi", contentKey: "output-number-rssi" },
  { tileId: "tile.actuator->microbit-v2.radio-send", contentKey: "actuator-radio-send" },
  { tileId: "tile.actuator->microbit-v2.set-radio-group", contentKey: "actuator-set-radio-group" },
  { tileId: "tile.actuator->microbit-v2.display-set-pixel", contentKey: "actuator-set-pixel" },
  { tileId: "tile.actuator->microbit-v2.display-scroll", contentKey: "actuator-display-text" },
  { tileId: "tile.actuator->microbit-v2.display-clear", contentKey: "actuator-display-clear" },
  { tileId: "tile.actuator->microbit-v2.draw-image", contentKey: "actuator-draw-image" },
  { tileId: "tile.actuator->microbit-v2.play-sound", contentKey: "actuator-play-sound" },
  { tileId: "tile.actuator->microbit-v2.play-tone", contentKey: "actuator-play-tone" },
  { tileId: "tile.modifier->microbit-v2.pressed", contentKey: "modifier-pressed" },
  { tileId: "tile.modifier->microbit-v2.released", contentKey: "modifier-released" },
  { tileId: "tile.modifier->microbit-v2.click", contentKey: "modifier-click" },
  { tileId: "tile.modifier->microbit-v2.double-click", contentKey: "modifier-double-click" },
  { tileId: "tile.modifier->microbit-v2.long-click", contentKey: "modifier-long-click" },
  { tileId: "tile.modifier->microbit-v2.held", contentKey: "modifier-held" },
  { tileId: "tile.modifier->microbit-v2.shake", contentKey: "modifier-shake" },
  { tileId: "tile.modifier->microbit-v2.tilt-up", contentKey: "modifier-tilt-up" },
  { tileId: "tile.modifier->microbit-v2.tilt-down", contentKey: "modifier-tilt-down" },
  { tileId: "tile.modifier->microbit-v2.tilt-left", contentKey: "modifier-tilt-left" },
  { tileId: "tile.modifier->microbit-v2.tilt-right", contentKey: "modifier-tilt-right" },
  { tileId: "tile.modifier->microbit-v2.face-up", contentKey: "modifier-face-up" },
  { tileId: "tile.modifier->microbit-v2.face-down", contentKey: "modifier-face-down" },
  { tileId: "tile.modifier->microbit-v2.freefall", contentKey: "modifier-freefall" },
  { tileId: "tile.modifier->microbit-v2.immediately", contentKey: "modifier-immediately" },
  { tileId: "tile.modifier->microbit-v2.in-background", contentKey: "modifier-in-background" },
  { tileId: "tile.modifier->microbit-v2.square", contentKey: "modifier-square" },
  { tileId: "tile.modifier->microbit-v2.sawtooth", contentKey: "modifier-sawtooth" },
  { tileId: "tile.modifier->microbit-v2.sine", contentKey: "modifier-sine" },
  { tileId: "tile.modifier->microbit-v2.triangle", contentKey: "modifier-triangle" },
  { tileId: "tile.parameter->microbit-v2.x", contentKey: "parameter-x" },
  { tileId: "tile.parameter->microbit-v2.y", contentKey: "parameter-y" },
  { tileId: "tile.parameter->microbit-v2.brightness", contentKey: "parameter-brightness" },
  { tileId: "tile.parameter->microbit-v2.text", contentKey: "parameter-text" },
  { tileId: "tile.parameter->microbit-v2.image", contentKey: "parameter-image" },
  { tileId: "tile.parameter->microbit-v2.duration", contentKey: "parameter-duration" },
  { tileId: "tile.parameter->microbit-v2.sound-emoji", contentKey: "parameter-sound" },
  { tileId: "tile.parameter->microbit-v2.volume", contentKey: "parameter-volume" },
  { tileId: "tile.literal->struct:<Image>->heart", contentKey: "literal-image-heart" },
  { tileId: "tile.literal->struct:<Image>->happy", contentKey: "literal-image-happy" },
  { tileId: "tile.literal->struct:<Image>->sad", contentKey: "literal-image-sad" },
  { tileId: "tile.literal->struct:<Image>->arrow-north", contentKey: "literal-image-arrow-north" },
  { tileId: "tile.literal->struct:<Image>->arrow-south", contentKey: "literal-image-arrow-south" },
  { tileId: "tile.literal->struct:<Image>->arrow-east", contentKey: "literal-image-arrow-east" },
  { tileId: "tile.literal->struct:<Image>->arrow-west", contentKey: "literal-image-arrow-west" },
  { tileId: "tile.literal->struct:<SoundEmoji>->giggle", contentKey: "literal-sound-giggle" },
  { tileId: "tile.literal->struct:<SoundEmoji>->happy", contentKey: "literal-sound-happy" },
  { tileId: "tile.literal->struct:<SoundEmoji>->hello", contentKey: "literal-sound-hello" },
  { tileId: "tile.literal->struct:<SoundEmoji>->mysterious", contentKey: "literal-sound-mysterious" },
  { tileId: "tile.literal->struct:<SoundEmoji>->sad", contentKey: "literal-sound-sad" },
  { tileId: "tile.literal->struct:<SoundEmoji>->slide", contentKey: "literal-sound-slide" },
  { tileId: "tile.literal->struct:<SoundEmoji>->soaring", contentKey: "literal-sound-soaring" },
  { tileId: "tile.literal->struct:<SoundEmoji>->spring", contentKey: "literal-sound-spring" },
  { tileId: "tile.literal->struct:<SoundEmoji>->twinkle", contentKey: "literal-sound-twinkle" },
  { tileId: "tile.literal->struct:<SoundEmoji>->yawn", contentKey: "literal-sound-yawn" },
];
