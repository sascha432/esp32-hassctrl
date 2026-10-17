# Motion sensor helper

How to set up Home Assistant for the motion sensor of a panel: a PIR or radar on a GPIO of the
board (the JC4880P443C: GPIO 34 of the expansion header; another board through `motion_gpio` in its
variant, [adding-a-board.md](../adding-a-board.md) "Variants"). The panel does not create an entity
of its own: it turns a toggle helper that you create, `input_boolean.<device>_motion`, on while it
sees motion and off when it is clear (docs/architecture.md section 9.7b). Nothing is configured on
the panel.

The toggle alone is enough (on / off). A template binary sensor on it (section 6) adds the motion
device class: Home Assistant then shows Detected / Clear in its own language and treats it as a
motion sensor.

## 1. The device name

`<device>` is the panel's name with `_` for `-`: `hassctrl-7e7d` becomes `hassctrl_7e7d`. The name
is `hassctrl-` and the last two bytes of the MAC address. It is shown:

* on the panel: Settings → Hotspot, "Name";
* in the web UI's address: `http://hassctrl-7e7d.local/`;
* in the log when the helper is missing (section 4).

## 2. The toggle helper

Settings → Devices & services → Helpers → Create helper → Toggle:

| Field | Value |
| --- | --- |
| Name | `hassctrl_7e7d_motion` (the entity becomes `input_boolean.hassctrl_7e7d_motion`); rename it afterwards if you like, the entity ID stays |
| Icon | optional, e.g. `mdi:motion-sensor` |

A toggle can be switched by anyone, so keep it out of the way once the binary sensor of section 6
exists: in its entity settings turn "Visible" off (no auto-generated dashboards), and don't expose
it to Assist (Settings → Voice assistants → Expose). Don't turn "Enabled" off: the panel could not
switch a disabled helper.

Or in `configuration.yaml` (section 6 says how to edit it; the key is the entity ID, the name is
free):

```yaml
input_boolean:
  hassctrl_7e7d_motion:
    name: Kitchen panel motion
    icon: mdi:motion-sensor
```

A panel with a normal long-lived token may switch it; no other permission is needed.

## 3. Check it

On the panel's console (`python tools/build.py <board> -p COMx monitor`, or any serial terminal):

```
motion detected
motion clear
motion status
```

`detected` and `clear` send that state to the helper by hand; the sensor's next change overrides
it. `status` shows the sensor, the held state and the hold time. Home Assistant shows the toggle
switch in its entity dialog.

## 4. When the helper is missing

The panel logs once per connection:

```
W (...) hass: motion not published: ... (create the toggle helper input_boolean.hassctrl_7e7d_motion in Home Assistant)
```

That line also names the exact entity ID to create. After the helper exists, the next change of
the sensor switches it; a reconnect (or a restart of the panel) switches it at once.

## 5. How it behaves

| Event | Helper |
| --- | --- |
| The sensor sees motion | on at once (the pin is read every 100 ms, two equal reads count) |
| The sensor stops seeing motion | stays on for the hold time, then off; every new motion restarts the hold |
| The panel connects to Home Assistant (start, reconnect) | the current state is sent again |
| Someone switches the toggle by hand | stays so until the sensor's next change or the next reconnect |
| The panel is off or offline | keeps its last state: Home Assistant does not know the panel is gone |
| Home Assistant restarts | the helper restores its last state; the panel sends the current one when it reconnects |

The hold time is a setting of the panel: Settings → Display → "Motion hold", 1 to 60 min,
default 5 min. It keeps a PIR that resets after 1-2 s from switching the helper on and off all the
time, and it is the "off delay" of anything that uses the helper.

The screen's own wake on motion (Settings → Display) is local and does not need the helper or
Home Assistant.

## 6. A binary sensor with the motion device class (recommended)

A toggle has no device class: Home Assistant shows On / Off and does not treat it as motion (no
motion icon by state, not in the motion lists of the area). A template binary sensor does.

Settings → Devices & services → Helpers → Create helper → Template → Template a binary sensor:

| Field | Value |
| --- | --- |
| Name | `Kitchen panel motion` (the entity becomes `binary_sensor.kitchen_panel_motion`) |
| State template | `{{ is_state('input_boolean.hassctrl_7e7d_motion', 'on') }}` |
| Device class | Motion |

The UI has no availability field, and none is needed: the toggle is always on or off.

Or in `configuration.yaml` (Home Assistant OS: the File editor add-on edits it in the browser;
check the configuration and restart afterwards):

```yaml
template:
  - binary_sensor:
      - name: Kitchen panel motion
        unique_id: hassctrl_7e7d_motion_binary
        device_class: motion
        state: "{{ is_state('input_boolean.hassctrl_7e7d_motion', 'on') }}"
```

On the panel's dashboard, put the tile on the binary sensor (`entity:
binary_sensor.kitchen_panel_motion`): it shows Detected / Clear and a timeline in its panel. A tile
on the toggle would be a switch tile, and a tap would switch the helper.

## 7. An automation: light while there is motion

Settings → Automations & scenes → Create automation → ⋮ → Edit in YAML (the automation's own
editor, not `configuration.yaml`), paste, change the entities, save:

```yaml
alias: Kitchen light on motion
description: On while the panel sees motion, off when the hold time is over
triggers:
  - trigger: state
    entity_id: binary_sensor.kitchen_panel_motion
    to: 'on'
    id: detected
  - trigger: state
    entity_id: binary_sensor.kitchen_panel_motion
    to: 'off'
    id: clear
conditions: []
actions:
  - choose:
      - conditions:
          - condition: trigger
            id: detected
        sequence:
          - action: light.turn_on
            target:
              entity_id: light.kitchen
      - conditions:
          - condition: trigger
            id: clear
        sequence:
          - action: light.turn_off
            target:
              entity_id: light.kitchen
mode: queued
```

The quotes in `to: 'on'` and `to: 'off'` are needed: YAML reads a bare `on` and `off` as booleans.
Without the binary sensor, use `input_boolean.hassctrl_7e7d_motion` with the same `'on'` / `'off'`.

No `for:` delay is needed on the `off` trigger: the panel's hold time already is one.

Because the helper keeps its state while the panel is offline, a light can stay on when the panel
loses power while it is on. If that matters, add a third trigger, the binary sensor staying `'on'`
for longer than anyone stays in the room (`for: "01:00:00"`), with the `clear` id.
