```brain noframe when
{ "tile": "${tileId}" }
```

# Decoded stick position

Decodes a received gamepad packet into a position.

---

Use it under a `radio receive buffer` rule: it reads the received bytes
automatically and decodes them into a position (`x` and `y`, both -100..100),
read through the accessor tiles. An invalid or missing packet decodes to
(0, 0), the centered idle stick, so the value is always usable.

```assistant
Place it under a WHEN whose sensor produced a buffer ("radio receive buffer"): it reads that received value itself, and takes no argument of its own. It never fails: a missing, short, or foreign packet decodes to the centered (0, 0), the idle stick, so downstream math is always safe -- but that also means garbage radio traffic reads as "stick at rest", not as an error. Read the axes through the x and y accessor tiles.
```
