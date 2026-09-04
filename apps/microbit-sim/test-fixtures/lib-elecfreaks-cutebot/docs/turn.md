```brain noframe do
{ "tile": "${tileId}" }
```

# Cutebot turn

Turns the Cutebot in a forward arc.

---

Drives the outer wheel while the inner wheel rests, so the robot arcs toward
the chosen side (default right) and a turn alone creeps forward. Add up to
three `slowly` or `quickly` words to change the rate, and `left` or `right` to
pick the side.

```assistant
A turn alone creeps forward in an arc: the outer wheel drives while the inner wheel rests. For spinning on the spot use "cutebot pivot" instead. Turning blends with driving -- a rule firing "drive" and another firing "turn" in the same think steer the robot in a moving arc. Movement is per-think: the rule must keep firing to keep turning, and a few silent thinks stop the robot. The bare tile turns right at the middle rate.
```
