```brain noframe do
{ "tile": "${tileId}" }
```

# Beep

Plays a plain tone on the speaker.

---

Holds one steady pitch for a set time -- the classic feedback beep. The bare
number is the pitch in Hz, 880 when left off; a pitch of 0 is a rest, silent
for the whole time while still holding the speaker, so beeps and gaps can be
built from the one tile. `tile:tile.parameter->microbit-v2.duration` sets how
long the tone lasts in seconds (0.5 when left off, fractions allowed) and
`tile:tile.parameter->microbit-v2.volume` sets how loud it is, from 0 to 1 (1
when left off). Add one wave shape -- `tile:tile.modifier->microbit-v2.square`,
`tile:tile.modifier->microbit-v2.sawtooth`,
`tile:tile.modifier->microbit-v2.sine`, or
`tile:tile.modifier->microbit-v2.triangle` -- to change the character of the
tone; with none of them it is a triangle wave. The rule waits until the tone
ends: until then a rule under it does not get its turn, and this rule cannot
fire again. A beep made while the speaker is busy is dropped; add
`tile:tile.modifier->microbit-v2.immediately` to take over the speaker at once,
or `tile:tile.modifier->microbit-v2.in-background` to let the rule continue
without waiting.

## Example

A short, quiet beep as feedback for a button press. It uses
`tile:tile.modifier->microbit-v2.in-background` so the rule can fire again on
the next press without waiting for the tone.

```brain
{
  "ruleJsons": [
    {
      "version": 1,
      "when": [
        "tile.sensor->microbit-v2.button-a",
        "tile.modifier->microbit-v2.pressed"
      ],
      "do": [
        "${tileId}",
        "tile.literal->number:<number>->440",
        "tile.parameter->microbit-v2.duration",
        "tile.literal->number:<number>->0.1",
        "tile.parameter->microbit-v2.volume",
        "tile.literal->number:<number>->0.3",
        "tile.modifier->microbit-v2.in-background"
      ],
      "children": [],
      "comment": "A quiet 440 Hz blip, without holding up the rule."
    }
  ],
  "catalog": [
    {
      "version": 2,
      "kind": "literal",
      "tileId": "tile.literal->number:<number>->440",
      "valueType": "number:<number>",
      "value": 440,
      "valueLabel": "440",
      "displayFormat": "default"
    },
    {
      "version": 2,
      "kind": "literal",
      "tileId": "tile.literal->number:<number>->0.1",
      "valueType": "number:<number>",
      "value": 0.1,
      "valueLabel": "0.1",
      "displayFormat": "default"
    },
    {
      "version": 2,
      "kind": "literal",
      "tileId": "tile.literal->number:<number>->0.3",
      "valueType": "number:<number>",
      "value": 0.3,
      "valueLabel": "0.3",
      "displayFormat": "default"
    }
  ]
}
```

## Example: a beep, a rest, a higher beep

Each rule waits for its own tone, so nesting them plays them one after another.
The middle tone is a rest: a pitch of 0 sounds nothing but still takes its
0.15 seconds, which is the gap between the two beeps.

```brain
{
  "ruleJsons": [
    {
      "version": 1,
      "when": [
        "tile.sensor->microbit-v2.button-b",
        "tile.modifier->microbit-v2.pressed"
      ],
      "do": [
        "${tileId}",
        "tile.literal->number:<number>->660",
        "tile.parameter->microbit-v2.duration",
        "tile.literal->number:<number>->0.15"
      ],
      "children": [
        {
          "version": 1,
          "when": [],
          "do": [
            "${tileId}",
            "tile.literal->number:<number>->0",
            "tile.parameter->microbit-v2.duration",
            "tile.literal->number:<number>->0.15"
          ],
          "children": [
            {
              "version": 1,
              "when": [],
              "do": [
                "${tileId}",
                "tile.literal->number:<number>->990",
                "tile.parameter->microbit-v2.duration",
                "tile.literal->number:<number>->0.15"
              ],
              "children": [],
              "comment": "Then a higher beep."
            }
          ],
          "comment": "A rest: silent, but it still takes 0.15 seconds."
        }
      ],
      "comment": "First beep."
    }
  ],
  "catalog": [
    {
      "version": 2,
      "kind": "literal",
      "tileId": "tile.literal->number:<number>->660",
      "valueType": "number:<number>",
      "value": 660,
      "valueLabel": "660",
      "displayFormat": "default"
    },
    {
      "version": 2,
      "kind": "literal",
      "tileId": "tile.literal->number:<number>->990",
      "valueType": "number:<number>",
      "value": 990,
      "valueLabel": "990",
      "displayFormat": "default"
    },
    {
      "version": 2,
      "kind": "literal",
      "tileId": "tile.literal->number:<number>->0",
      "valueType": "number:<number>",
      "value": 0,
      "valueLabel": "0",
      "displayFormat": "default"
    },
    {
      "version": 2,
      "kind": "literal",
      "tileId": "tile.literal->number:<number>->0.15",
      "valueType": "number:<number>",
      "value": 0.15,
      "valueLabel": "0.15",
      "displayFormat": "default"
    }
  ]
}
```

## See Also

`tile:tile.actuator->microbit-v2.play-sound`
`tile:tile.parameter->microbit-v2.duration`
`tile:tile.parameter->microbit-v2.volume`

```assistant
A pitch of 0 plays silence for the duration, a rest. One wave-shape word (square / sawtooth / sine / triangle) picks the sound, triangle when none is given. The rule holds until the tone ends: until then a rule under it does not get its turn, and this rule cannot fire again. A tone asked for while a sound is playing is dropped; add "in background" to let the rule carry on, or "immediately" to cut off the sound that is playing.
```
