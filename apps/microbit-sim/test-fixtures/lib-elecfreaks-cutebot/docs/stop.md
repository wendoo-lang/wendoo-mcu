```brain noframe do
{ "tile": "${tileId}" }
```

# Cutebot stop

Stops the Cutebot at once.

---

Stops both wheels immediately. A stop wins over every other movement tile that
fires in the same think, so use it for safety rules that must always take
effect. Movement resumes normally on the next think.

```assistant
Stop is exclusive for its think: every movement influence commanded that think is discarded, whether it fired before or after the stop, and both wheels write zero at once. Movement resumes normally on the next think, so a stop rule must keep firing to hold the robot still. Use it as the safety rule that must win over every concurrently firing movement rule.
```
