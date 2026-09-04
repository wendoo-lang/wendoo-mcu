```brain noframe when
{ "tile": "${tileId}" }
```

# Stick position

Reads the thumb stick as a live position value.

---

Reads both stick axes as a position: `x` right positive, `y` up positive, both
-100..100, with a centered dead zone that reads as 0. Read its `x` and `y`
fields through the accessor tiles, or feed the whole value to a tile that
takes a position, like `cutebot steer`; sending it with `radio send` encodes
it as a gamepad packet automatically.

```assistant
Each evaluation samples the stick fresh, so the value is live in any expression with no radio needed. The centered dead zone reads exactly (0, 0) -- treat zero as "hands off". Read the axes through the x and y accessor tiles. Placing this value where a buffer is expected (like "radio send") encodes it as a gamepad packet automatically; there is no need to build bytes by hand.
```
