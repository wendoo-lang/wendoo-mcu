```brain noframe do
{ "tile": "${tileId}" }
```

# Create an image

Draws a new 5x5 image of your own.

---

Opens a 5x5 grid to draw on: pick a brightness, then tap a pixel to light it at
that brightness, and tap it again to turn it off. Saving makes an image tile
that works anywhere a built-in image such as
`tile:tile.literal->struct:<Image>->heart` does -- give it to
`tile:tile.parameter->microbit-v2.image` on
`tile:tile.actuator->microbit-v2.draw-image`. Every drawing makes a tile of its
own, so two drawings that came out the same stay two tiles. Naming the image
gives it a word to read by, in the editor and in what the assistant sees.

Editing an image changes it everywhere it is placed. The tile stays the same
tile, so every rule holding it draws the new pixels, and undo puts the old ones
back everywhere at once. To change one placement on its own, duplicate the image
first: a duplicate is a separate image tile, drawn from the same pixels, that you
can edit without touching the one you copied.

```assistant
You cannot create an image: the grid editor is the only way one is drawn, and the person using the editor draws it. Use the images that already exist, by tile id -- read_catalog and read_project list them, the built-in ones and the ones the user drew. A drawn image may carry a name, which is the word it is listed under. Editing an image keeps its tile id and updates every placement of it at once; duplicating one makes a separate image tile with an id of its own.
```
