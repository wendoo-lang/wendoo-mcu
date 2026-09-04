```brain noframe do
{ "tile": "${tileId}" }
```

# Cutebot pivot

Spins the Cutebot in place.

---

Counter-rotates the wheels so the robot spins on the spot toward the chosen
side (default right). Add up to three `slowly` or `quickly` words to change
the rate, and `left` or `right` to pick the side.

```assistant
Pivot spins in place with no forward motion (the wheels counter-rotate); for a moving arc use "cutebot turn". Movement is per-think: the rule must keep firing to keep spinning, and a few silent thinks stop the robot. Pivoting blends additively with other movement tiles firing the same think. The bare tile pivots right at the middle rate.
```
