# GuitarMidi-LV2 — MOD Devices GUI

The MOD ("modgui") front end of the plugin: a custom pedal face for mod-ui /
MOD Duo / MOD Dwarf, in the same dark panel style as the X11/Cairo UI.

The GUI is a hand-written icon template instead of one of the stock
`modgui:panel` layouts (`1-select-4-knobs`, …). The plugin has 18 control
ports and no stock panel can hold them, so `modgui.ttl` points at
`modgui/icon-guitar-midi.html` + `modgui/stylesheet-guitar-midi.css` and binds
every widget to its LV2 port by `mod-port-symbol`.

## What is exposed

All 18 control ports, grouped like `ui/guitarmidi_ui.cpp`:

| Panel | Control | `lv2:symbol` | Widget | Range (default) |
|---|---|---|---|---|
| INPUT | Gain | `gain` | knob | -20 … 40 dB (28) |
| INPUT | Expressivity | `expressivity` | knob | 0 … 20 dB (3) |
| NOTE ONSET | Smoothing | `onsetsmoothing` | knob | 0 … 0.9 (0.3) |
| NOTE ONSET | Confidence | `onsetconfthr` | knob | 0 … 0.99 (0.95) |
| NOTE ONSET | Level | `onsetlevel` | knob | -80 … 0 dBFS (-66) |
| NOTE OFFSET | Smoothing | `offsetsmoothing` | knob | 0 … 0.9 (0.3) |
| NOTE OFFSET | Confidence | `offsetconfthr` | knob | 0 … 0.99 (0.3) |
| NOTE OFFSET | Level | `offsetlevel` | knob | -80 … 0 dBFS (-66) |
| NOTE FILTER | Scale | `scale` | select (18 scale points) | 0 … 17 (0 = Off) |
| NOTE FILTER | Root | `scaleroot` | select (12 scale points) | 0 … 11 (C) |
| NOTE FILTER | Transpose | `transpose` | knob | -24 … 24 st (0) |
| ROUTING | Low Ch | `lowchan` | select (1 … 16) | 1 … 16 (1) |
| ROUTING | Split Note | `splitnote` | select (Off + MIDI 24 … 127) | 0 … 127 (0 = Off) |
| ROUTING | High Ch | `highchan` | select (1 … 16) | 1 … 16 (1) |
| ROUTING | MPE | `mpe` | switch | 0 / 1 (0) |
| ROUTING | One Per String | `perstring` | switch | 0 / 1 (0) |
| TRACKING | Tuning | `tuning` | knob | -12 … 12 st (0) |
| TRACKING | Bend Range | `bendrange` | knob | 0 … 24 st (2) |

The **input level meter** at the bottom of the pedal is fed by two *output*
control ports (`inlevel` = block RMS, `inpeak` = block peak, both dBFS in
-80 … 0, post gain). `modgui:monitoredOutputs` in `modgui.ttl` makes mod-host
and mod-ui push those values into `script-guitar-midi.js` (`event.ports` on
`start`, `event.symbol`/`event.value` on `change`), which moves the bar. The
zones follow the desktop meter: blue < -40 dB, green -40 … -15, amber -15 … -5,
red >= -5, with a white peak tick. The desktop UI is unchanged: it still gets
its level from the debug packet.

The knob and switch values are film strips: `knob-{blue,teal,orange}.png`
(61 frames, 270° sweep, arc + pointer in the group colour of the desktop UI)
and `switch.png` (2 frames). The selects are native `<select>` widgets bound to
the enumeration/integer ports; the scale/root/split-note lists repeat the
`lv2:scalePoint` labels from `guitarmidi.ttl`, and the split-note values are the
MIDI note numbers, so a pedalboard stores the same values a desktop host does.

Not exposed on purpose:

- `enabled` — driven by the host bypass (`mod-role="bypass"` footswitch and
  light), not a user control.
- `midiin` — an atom input port, shown as a jack, not a widget.
- `inlevel` / `inpeak` — output control ports, not widgets: they drive the
  meter through `modgui:monitoredOutputs` (see above).

`modgui:port` `lv2:index` values in `modgui.ttl` are the *widget* indices of
the MODGUI spec (GUI order), not the LV2 port indices of `guitarmidi.ttl`.

## Files

```
gui/
├── modgui.ttl                      GUI description (resources + 18 ports)
├── manifest.ttl                    MOD-flavoured bundle manifest (see below)
├── modgui/
│   ├── icon-guitar-midi.html       pedal layout, one widget per port symbol
│   ├── stylesheet-guitar-midi.css  layout coordinates, panels, widget styles
│   ├── script-guitar-midi.js       meter update from the monitored outputs
│   ├── pedal.png                   pedal plate, 2x (420x660 CSS)
│   ├── knob-blue.png               film strips, 61 frames of 50px, 2x
│   ├── knob-teal.png
│   ├── knob-orange.png
│   ├── switch.png                  2 frames of 32px, 2x
│   ├── pedals/footswitch.png       stock MOD footswitch (2 frames)
│   ├── screenshot.png              full pedal, from the headless render
│   └── thumbnail.png               plugin-list thumbnail
└── tools/
    ├── gen_modgui_art.py           draws pedal/knobs/switch with pillow
    └── render_modgui.py            renders the GUI in headless Chrome,
                                    checks the port map, writes screenshot/
                                    thumbnail
```

The artwork is generated, not hand-painted:

```sh
python3 tools/gen_modgui_art.py     # pedal.png, knob-*.png, switch.png
python3 tools/render_modgui.py      # screenshot.png, thumbnail.png
```

`render_modgui.py` fails if a `mod-port-symbol` in the template is not in its
port table, if one of the 18 ports is missing a widget, or if the meter is not
wired to both monitored outputs, so it doubles as the "all controls are on the
pedal" check. The screenshot shows the meter at a fixed demo level (-18 dB RMS,
-9 dB peak).

## How it reaches a bundle

`src/CMakeLists.txt` installs `modgui.ttl` and the `modgui/` directory next to
the plugin binary (this also covers the MOD buildroot package, whose Makefile
just runs `cmake --install`). The plugin's `manifest.ttl` advertises the GUI
with `rdfs:seeAlso <modgui.ttl>`, which is how lilv/mod-ui discover it.
`manifest.ttl` in this directory is the MOD-only variant of the bundle
manifest (no X11 UI entry) and is kept for reference; the installed one is the
top-level `manifest.ttl`.

Layout mirrors the desktop UI, which also comes from the panel spec in
`ui/guitarmidi_ui.cpp`; if a control moves there, move it here too (CSS
positions, `render_modgui.py` port table, `modgui.ttl` port list).

## Validation

- `modgui.ttl` and both manifests parse with rdflib.
- the 18 widget symbols in the template/`modgui.ttl` match `guitarmidi.ttl`.
- `lv2info` lists `inlevel` (index 24) and `inpeak` (index 25), and
  `guitarmidi-audiooutput.ttl` carries the same two ports at 28/29.
- `ninja -C build` and `ctest` pass, and `cmake --install` puts `modgui.ttl` +
  `modgui/` into `guitarmidi.lv2/`.
- the headless render (`render_modgui.py`) shows every widget in place and the
  meter drawn from the monitored outputs.

Still unverified without a MOD device: the actual behaviour of mod-ui's film
widget (drag/step rounding) at these step counts, the native `<select>`
dropdowns on the MOD's Chromium build, whether the meter update rate is
comfortable in practice (the ports carry a per-block value and mod-host pushes
every change), and whether 420x660 px is a comfortable pedal size on the Dwarf
screen. Those need one pass on hardware.
