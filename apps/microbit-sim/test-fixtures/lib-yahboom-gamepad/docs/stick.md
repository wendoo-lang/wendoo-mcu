```brain noframe when
{ "tile": "${tileId}" }
```

# Stick

Reads the thumb stick as a direction.

---

True while the stick is pushed in one of the named directions. Add any of the
`up`, `down`, `left`, `right` words to pick directions; with none, the tile is
true for any direction. A stick that is pressed straight in reports no
direction, so every direction reads false while it is pressed.

```assistant
The direction words are any-of: place several and the tile is true while the stick matches any one of them. The stick reports at most one direction at a time (vertical wins over horizontal), and a pressed stick reports none -- every direction reads false while the stick is pressed, so a press rule and a direction rule never fire together. This is the coarse thresholded reading; for proportional control read "stick position" instead.
```
