```brain noframe do
{ "tile": "${tileId}" }
```

# In background

Runs the show or sound just as usual, but the rule does not wait for it: the
rule carries on at once and can fire again, while the display or speaker stays
busy until this one finishes.

---

Attach to `tile:tile.actuator->microbit-v2.display-scroll`,
`tile:tile.actuator->microbit-v2.draw-image`, or
`tile:tile.actuator->microbit-v2.play-sound`: the animation or sound runs as
usual, but the rule continues at once instead of waiting for it to finish. The
display or speaker stays busy until it completes, so a new request during that
time is still dropped.
