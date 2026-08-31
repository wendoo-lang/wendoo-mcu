```brain noframe do
{ "tile": "${tileId}" }
```

# Volume

How loud a tone is, from 0 to 1.

---

Gives `tile:tile.actuator->microbit-v2.play-tone` its loudness as a fraction of
full: 1 is as loud as the tone gets, 0.5 is half, and 0 is silent. When left
off, the tone plays at 1. A silent tone still holds the speaker for its whole
`tile:tile.parameter->microbit-v2.duration`. The device's own volume setting
still applies on top of this.
