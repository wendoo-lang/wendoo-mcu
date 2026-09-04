```brain noframe do
{ "tile": "${tileId}" }
```

# Cutebot steer

Drives the Cutebot from a position value.

---

Takes a position (both axes -100..100): its `y` axis (up positive) drives
forward or backward and its `x` axis (right positive) turns. Feed it the
gamepad `stick position` or `decoded stick position` to drive the robot from a
controller.

```assistant
Steer turns one position value into blended movement: the y axis feeds a straight drive and the x axis feeds a turn, both unscaled (-100..100). It obeys the same per-think movement model as the other tiles -- the rule must keep firing to keep moving -- and blends with them. The canonical remote-control brain is one rule: WHEN "radio receive buffer" DO "cutebot steer" with "decoded stick position" in its slot.
```
