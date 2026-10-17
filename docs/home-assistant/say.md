# Sounds from Home Assistant (hassctrl_say)

How Home Assistant makes a panel speak a text or play an MP3: on a board with a speaker (the
JC4880P443C; any board with `CONFIG_HC_AUDIO_MP3`). Home Assistant fires the event `hassctrl_say`
and the panel, which listens on its connection to Home Assistant, plays it (docs/architecture.md
section 9.6). Nothing is configured on the panel, and no helper or entity is needed: the event
uses the connection and the token the panel already has.

Section 6 adds a text helper that you type a sentence into, so the panel speaks it. That is
optional: an automation does the work, the panel does not read the helper.

## 1. The device name

`device` in the event is the panel's name as it is: `hassctrl-7e7d` (with `-`, upper or lower
case). The name is `hassctrl-` and the last two bytes of the MAC address. It is shown:

* on the panel: Settings → Hotspot, "Name";
* in the web UI's address: `http://hassctrl-7e7d.local/`.

`device` may be a list of names. Without `device`, every panel plays.

## 2. The event

| Key | Value |
| --- | --- |
| `device` | optional: a name or a list of names (section 1) |
| `message` | the text to speak; needs `engine` |
| `engine` | a text-to-speech entity of Home Assistant, e.g. `tts.google_en_com` (Settings → Devices & services → Entities, filter `tts.`) |
| `language` | optional, e.g. `en`; the engine's default without it |
| `options` | optional, a map of the engine's own options, e.g. Google Translate's `tld: co.uk` (the accent) |
| `url` | instead of `message`: an MP3, `/local/doorbell.mp3` (a file in Home Assistant's `www` folder) or a whole `http(s)://` address; at most 1 MB, a WAV does not play |

Home Assistant makes the speech (`/api/tts_get_url`), as MP3 in one channel; the panel downloads
it and plays it.

## 3. Try it

Developer tools → Events → "Fire an event": event type `hassctrl_say`, event data:

```yaml
device: hassctrl-7e7d
message: Hello
engine: tts.google_en_com
```

On the panel's console (`python tools/build.py <board> -p COMx monitor`, or any serial terminal)
the same without Home Assistant firing anything:

```
sound say tts.google_en_com Hello
sound url /local/doorbell.mp3
```

## 4. In an automation or a script

The action is `event` (in the automation editor: Add action → Other actions → Event, or ⋮ → Edit
in YAML):

```yaml
actions:
  - event: hassctrl_say
    event_data:
      device: hassctrl-7e7d
      message: The washing machine is done
      engine: tts.google_en_com
```

A template works in `message`, e.g. `"It is {{ states('sensor.outside_temperature') }} degrees
outside"`.

## 5. How it behaves

| Situation | What happens |
| --- | --- |
| A new sound while one is on its way or playing | the new one replaces it |
| The alarm rings | the alarm comes first |
| Touch sounds while it speaks | left out |
| Volume | the panel's (Settings → Sound); at 0 it is silent |
| The panel is offline | the event is lost; it is not played later |
| Another `device` name | the panel stays silent |

The panel logs every event it plays (`hass: hassctrl_say: Hello`) and what went wrong:
`hassctrl_say: a message needs its engine (a tts entity)`, `say: "Hello": speech: HTTP 500: ...`
(an unknown engine), `the token was rejected`, `too large`, `a WAV file, only MP3 plays`.

## 6. A talk helper (optional)

A text helper that you type a sentence into, from a dashboard or the entity dialog, and the panel
speaks it.

Settings → Devices & services → Helpers → Create helper → Text:

| Field | Value |
| --- | --- |
| Name | `hassctrl_7e7d_talk` (the entity becomes `input_text.hassctrl_7e7d_talk`); rename it afterwards if you like, the entity ID stays |
| Maximum length | `255` (the most Home Assistant allows) |
| Icon | optional, e.g. `mdi:account-voice` |

Then the automation (Settings → Automations & scenes → Create automation → ⋮ → Edit in YAML,
paste, change the entities, save):

```yaml
alias: Kitchen panel talks
description: Speaks what is typed into input_text.hassctrl_7e7d_talk, then empties it
triggers:
  - trigger: state
    entity_id: input_text.hassctrl_7e7d_talk
conditions:
  - condition: template
    value_template: "{{ trigger.to_state.state not in ['', 'unknown', 'unavailable'] }}"
actions:
  - event: hassctrl_say
    event_data:
      device: hassctrl-7e7d
      message: "{{ trigger.to_state.state }}"
      engine: tts.google_en_com
  - action: input_text.set_value
    target:
      entity_id: input_text.hassctrl_7e7d_talk
    data:
      value: ""
mode: queued
```

The helper is emptied after each sentence: a state trigger fires only on a change, so the same
sentence twice would speak once. The condition keeps the empty text (and a restart of Home
Assistant) from speaking. One helper for several panels: put a list in `device`, or leave it out.
