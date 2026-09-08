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
You can create an image: mint this factory through propose_edit, giving value and name together. The value is 25 hex digits, one per pixel of the 5x5 grid, rows left to right from the top row down -- 0 is dark, f is brightest, and a digit between is a dimmer pixel. The name is required; it is the word the image is listed under, and a mint without one is refused. The minted image joins the document group of read_catalog and works anywhere a built-in image does. Before minting, check read_catalog and read_project for an image that already fits -- built-in, drawn by the user, or minted earlier -- and reuse it by tile id. You cannot change an image's pixels after minting; to get a different picture, mint a new image under a new name. The person can edit any image in the grid editor; such an edit keeps the tile id and updates every placement at once.
```
