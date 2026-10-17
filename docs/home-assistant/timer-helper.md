# Timer helper and automation

How to set up a timer in Home Assistant that switches a device on while it runs and off when it
ends, as the `timer.bathroom_fan_30min` of the sample dashboard
([rooms.yaml](../dashboard-sample/rooms.yaml)). The dashboard's timer tile and panel
([lvgl-panels.md](../ui/lvgl/lvgl-panels.md) section 7) start, pause and cancel the timer; the
automation does the switching, so the fan follows the timer however it is started (the dashboard,
Home Assistant's UI, a script).

## 1. The timer helper

Settings → Devices & services → Helpers → Create helper → Timer:

| Field | Value |
| --- | --- |
| Name | `Bathroom fan 30min` (the entity becomes `timer.bathroom_fan_30min`) |
| Icon | optional; the dashboard draws its own (`timer-outline`, `timer-pause-outline`, `timer-off-outline`) |
| Duration | `0:30:00`, the default for a start without a duration of its own |
| Restore | on: a running timer goes on after Home Assistant restarts |

Or in `configuration.yaml`:

```yaml
timer:
  bathroom_fan_30min:
    name: Bathroom fan 30min
    duration: "0:30:00"
    restore: true
```

The duration is the default of every start without one. A start with a duration (the panel's
rollers, `timer.start` with `duration: "0:45:00"`) counts for that run only: Home Assistant shows
it in the `duration` attribute while the timer runs and puts the default back once it is idle.
Home Assistant has no action that changes the default; that is the helper's Duration field.

## 2. The automation

Settings → Automations & scenes → Create automation → ⋮ → Edit in YAML, paste, change the two
entities, save:

```yaml
alias: Bathroom fan timer
description: The fan runs while the timer runs; turning the fan off cancels the timer
triggers:
  - trigger: state
    entity_id: timer.bathroom_fan_30min
    to: active
    id: started
  - trigger: state
    entity_id: timer.bathroom_fan_30min
    to: idle
    id: finished
  - trigger: state
    entity_id: switch.bathroom_fan
    to: 'off'
    id: fan_off
conditions: []
actions:
  - choose:
      - conditions:
          - condition: trigger
            id: started
        sequence:
          - action: switch.turn_on
            target:
              entity_id: switch.bathroom_fan
      - conditions:
          - condition: trigger
            id: finished
        sequence:
          - action: switch.turn_off
            target:
              entity_id: switch.bathroom_fan
      - conditions:
          - condition: trigger
            id: fan_off
          - condition: state
            entity_id: timer.bathroom_fan_30min
            state: active
        sequence:
          - action: timer.cancel
            target:
              entity_id: timer.bathroom_fan_30min
mode: queued
```

For a light, use `light.*` with `light.turn_on` and `light.turn_off`.

## 3. How it works

| Event | Trigger | What the automation does |
| --- | --- | --- |
| The timer starts (double tap on the tile, Start in the panel) | `started`: the timer goes to `active` | turns the fan on |
| The time is up, or Finish, or Cancel | `finished`: the timer goes to `idle` | turns the fan off |
| The fan is turned off by hand while the timer runs | `fan_off` | cancels the timer |
| The timer is paused | none (`paused` is neither `active` nor `idle`) | nothing; the fan stays on |
| Resume after a pause | `started` | turns the fan on (it is on already) |

The state triggers (`active`, `idle`) cover a cancel as well as the end; the `timer.finished`
trigger alone would leave the fan on after a cancel.

No loop: when the time is up, the automation turns the fan off, which fires `fan_off`; the timer
is `idle` by then, so the `active` condition skips the cancel. When the fan is turned off by
hand, the cancel takes the timer to `idle`, which fires `finished`; turning off a fan that is off
does nothing.

A restart of a running timer (Restart in the panel) fires no trigger: the timer stays `active`,
and the fan is on already.

`mode: queued`: with three triggers in one automation, two of them can fire close together (the
fan off, then the timer idle); `single` would drop the second.

The quotes in `to: 'off'` are needed: YAML reads a bare `off` as the boolean `false`.

To cancel a paused timer too when the fan is turned off, change the condition to
`state: [active, paused]`.
