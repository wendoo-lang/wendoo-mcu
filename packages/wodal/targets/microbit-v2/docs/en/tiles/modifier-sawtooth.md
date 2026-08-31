```brain noframe do
{ "tile": "${tileId}" }
```

# Sawtooth

Gives a beep a bright, brassy sawtooth wave.

---

Attach to `tile:tile.actuator->microbit-v2.play-tone`: the tone is sounded as a
sawtooth wave, which climbs and then drops straight back and gives a bright,
buzzy edge -- softer than
`tile:tile.modifier->microbit-v2.square` but far from smooth. Use at most one
wave shape per beep; with none the beep is a triangle wave.
