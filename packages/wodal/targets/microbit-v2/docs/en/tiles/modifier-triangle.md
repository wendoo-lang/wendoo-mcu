```brain noframe do
{ "tile": "${tileId}" }
```

# Triangle

Gives a beep a soft, hollow triangle wave.

---

Attach to `tile:tile.actuator->microbit-v2.play-tone`: the tone is sounded as a
triangle wave, which rises and falls in straight lines and sits between
`tile:tile.modifier->microbit-v2.sine` and
`tile:tile.modifier->microbit-v2.square` -- soft, with a hollow, flute-like
edge. This is what a beep uses when no wave shape is attached, so add it only
to say so plainly. Use at most one wave shape per beep.
