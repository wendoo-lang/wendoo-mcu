```brain noframe do
{ "tile": "${tileId}" }
```

# Draw image

Shows one or more images on the display.

---

Draws each given `tile:tile.parameter->microbit-v2.image` in order, holding
each for `tile:tile.parameter->microbit-v2.duration` seconds (default 1
second); the last image stays on the display. With no image it draws
`tile:tile.literal->struct:<Image>->happy`. The rule waits for the hold to
finish: until then a rule under it does not get its turn, and this rule cannot
fire again. A draw made while the display is busy is dropped; add
`tile:tile.modifier->microbit-v2.immediately` to take over the display at once,
or `tile:tile.modifier->microbit-v2.in-background` to let the rule continue
without waiting. A duration of 0 paints the image and continues at once.

```assistant
The rule holds until the image has finished showing: until then a rule under it does not get its turn, and this rule cannot fire again. A draw asked for while the display is busy is dropped; add "in background" to let the rule carry on, or "immediately" to cut off what the display is showing.
```
