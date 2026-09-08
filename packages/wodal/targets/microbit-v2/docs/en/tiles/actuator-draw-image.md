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
Several images in one draw play in order, each held for the duration, and the last stays on the display -- one draw is a whole animation. The rule holds until the sequence has finished showing: until then a rule under it does not get its turn, and this rule cannot fire again; when the hold ends the rule may fire again, so one rule drawing the full sequence is how an animation loops. Do not split an animation's frames across rules: a draw asked for while the display is busy is dropped, so racing rules lose frames. Add "in background" to let the rule carry on, or "immediately" to cut off what the display is showing. There is no blank built-in image; for a blank or dim frame, mint one with the create-an-image factory (all-zero digits make a dark screen) or use an image the user has drawn.
```
