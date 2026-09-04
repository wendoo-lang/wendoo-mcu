```brain noframe do
{ "tile": "${tileId}" }
```

# Cutebot drive

Drives the Cutebot straight.

---

Drives both wheels at the normal rate; add up to three `slowly` or `quickly`
words to change the speed, and `backward` to reverse. A rule keeps the robot
moving by firing every think: when no movement rule fires for a few thinks,
the robot stops. Movement tiles firing together blend into one motion, and
`cutebot stop` overrides them all.

```assistant
Movement is per-think: a rule keeps the robot driving by firing every think, and when no movement rule fires for a few thinks the robot stops on its own. Movement tiles firing in the same think blend by adding their wheel influences into one motion; "cutebot stop" discards them all. The rate words step a fixed ladder (three "slowly" words is a crawl; three "quickly" is full speed); a bare tile drives at the middle rate, and "backward" reverses it. Do not sequence drive with "then" rules to make a path -- hold each leg with a firing rule and switch legs on a sensor or a variable.
```
