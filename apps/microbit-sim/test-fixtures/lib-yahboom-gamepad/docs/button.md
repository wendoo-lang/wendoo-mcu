```brain noframe when
{ "tile": "${tileId}" }
```

# Button

Reads the four colored gamepad buttons.

---

True while a colored button is held: red is B1, green is B2, blue is B3, and
yellow is B4. Add color words to pick buttons; with none, the tile is true
while any button is held. The buttons are wired active-low (the pin reads LOW
while pressed); the tile already accounts for that.

```assistant
The color words are any-of: place several and the tile is true while any of those buttons is held. It is a level, true every think a button stays down, not a press edge -- a rule on it fires every think of the hold; latch a variable to act once per press. With no color word it reads "any button held".
```
