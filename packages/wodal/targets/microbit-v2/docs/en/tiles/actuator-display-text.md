```brain noframe do
{ "tile": "${tileId}" }
```

# Display text

Scrolls text across the display.

---

Scrolls the text from right to left and ends with a blank display; a single
character is shown still for a moment instead of scrolling. With no
`tile:tile.parameter->microbit-v2.text` argument it shows the value the WHEN
side produced, or "hello". The rule waits until the animation finishes: until
then a rule under it does not get its turn, and this rule cannot fire again. A
request made while the display is busy is dropped; add
`tile:tile.modifier->microbit-v2.immediately` to take over the display at once,
or `tile:tile.modifier->microbit-v2.in-background` to let the rule continue
without waiting.

```assistant
The rule holds until the text has finished showing: until then a rule under it does not get its turn, and this rule cannot fire again. A show asked for while the display is busy is dropped; add "in background" to let the rule carry on, or "immediately" to cut off what the display is showing.
```
