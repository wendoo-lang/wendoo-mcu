```brain noframe do
{ "tile": "${tileId}" }
```

# Immediately

Takes over the display or speaker at once, cutting off whatever was showing or
playing.

---

Attach to `tile:tile.actuator->microbit-v2.display-scroll`,
`tile:tile.actuator->microbit-v2.draw-image`, or
`tile:tile.actuator->microbit-v2.play-sound`: whatever is showing or playing
is stopped and this one starts now. Without it, a request made while the
display or speaker is busy is dropped.
