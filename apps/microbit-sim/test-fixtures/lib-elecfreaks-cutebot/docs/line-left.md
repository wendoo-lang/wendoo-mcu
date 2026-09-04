```brain noframe when
{ "tile": "${tileId}" }
```

# Cutebot line (left)

Reads the left line-tracking sensor.

---

True while the left downward-facing sensor is over a dark line (the default,
also selectable as `on`). The optional word changes what it reports: `found`
is true only on the think the sensor crosses onto the line, and `lost` only on
the think it leaves it.

```assistant
Bare (or with "on") this is a level: true every think the sensor sits over the line. "found" and "lost" are one-think edges -- true only on the single think the sensor crossed onto or off the line -- so they fire a rule once per crossing; use them to count or react to crossings, and the level to steer while on the line. The sensor samples after rules run, so an edge is observed on the think after the crossing. The same words work on the right-side sensor.
```
