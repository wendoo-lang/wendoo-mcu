```brain noframe do
{ "tile": "${tileId}" }
```

# Play sound

Plays a built-in sound on the speaker.

---

Plays the given `tile:tile.parameter->microbit-v2.sound-emoji`, a built-in
sound such as `tile:tile.literal->struct:<SoundEmoji>->twinkle`. With no sound
it plays `tile:tile.literal->struct:<SoundEmoji>->hello`. The rule waits until
the sound finishes: until then a rule under it does not get its turn, and this
rule cannot fire again. A play made while the speaker is busy is dropped; add
`tile:tile.modifier->microbit-v2.immediately` to take over the speaker at
once, or `tile:tile.modifier->microbit-v2.in-background` to let the rule
continue without waiting.

```assistant
The rule holds until the sound has finished: until then a rule under it does not get its turn, and this rule cannot fire again. A sound asked for while another is playing is dropped; add "in background" to let the rule carry on, or "immediately" to cut off the sound that is playing.
```
