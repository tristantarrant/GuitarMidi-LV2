#!/usr/bin/env python3
"""Render the GuitarMidi modgui in headless Chrome.

Builds a standalone page from icon-guitar-midi.html + stylesheet-guitar-midi.css
(stripping the mustache jack sections and the {{{ns}}} cache buster), places the
film strips on the frames mod-ui would show for the default port values, and
writes the two images referenced from modgui.ttl:

    screenshot.png   full pedal, 1x (CSS px, rendered at 2x and downsampled)
    thumbnail.png    plugin-list thumbnail

It doubles as a smoke test: every [mod-port-symbol] must resolve to a known
port in the GUI map below, both monitored outputs must be wired to the meter,
and the rendered image is a visual check of the layout without a MOD device.

usage: render_modgui.py [modgui-dir] [--out-dir DIR] [--keep]
requires: pillow and google-chrome (or chromium)
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

PEDAL_W, PEDAL_H = 420, 660
THUMB_W, THUMB_H = 84, 132

# symbol: (default, minimum, maximum) - values from guitarmidi.ttl
PORTS = {
    "gain": (28.0, -20.0, 40.0),
    "expressivity": (3.0, 0.0, 20.0),
    "onsetsmoothing": (0.3, 0.0, 0.9),
    "onsetconfthr": (0.95, 0.0, 0.99),
    "onsetlevel": (-66.0, -80.0, 0.0),
    "offsetsmoothing": (0.3, 0.0, 0.9),
    "offsetconfthr": (0.3, 0.0, 0.99),
    "offsetlevel": (-66.0, -80.0, 0.0),
    "scale": (0, 0, 17),
    "scaleroot": (0, 0, 11),
    "transpose": (0, -24, 24),
    "splitnote": (0, 0, 127),
    "lowchan": (1, 1, 16),
    "highchan": (1, 1, 16),
    "tuning": (0, -12, 12),
    "mpe": (0, 0, 1),
    "bendrange": (2.0, 0.0, 24.0),
    "perstring": (0, 0, 1),
}

# watched output ports (modgui:monitoredOutputs), drawn at fixed demo levels in
# the screenshot: (symbol, demo dB) - RMS level and block peak
MONITORS = {"inlevel": -18.0, "inpeak": -9.0}

CHROME = None
for candidate in ("google-chrome", "chromium", "chromium-browser"):
    path = shutil.which(candidate)
    if path:
        CHROME = path
        break


def build_page(modgui):
    html = open(os.path.join(modgui, "icon-guitar-midi.html")).read()
    css = open(os.path.join(modgui, "stylesheet-guitar-midi.css")).read()
    html = re.sub(r"\{\{#.*?\}\}.*?\{\{/.*?\}\}", "", html, flags=re.S)
    html = html.replace("{{{ns}}}", "")
    css = css.replace("{{{ns}}}", "").replace("/resources/", "")

    symbols = set(re.findall(r'mod-port-symbol="([^"]+)"', html))
    unknown = symbols - set(PORTS)
    if unknown:
        raise SystemExit("modgui references unknown ports: " + ", ".join(sorted(unknown)))
    missing = set(PORTS) - symbols
    if missing:
        raise SystemExit("modgui is missing ports: " + ", ".join(sorted(missing)))

    script_js = open(os.path.join(modgui, "script-guitar-midi.js")).read()
    for symbol in MONITORS:
        if symbol not in script_js:
            raise SystemExit("script-guitar-midi.js does not handle " + symbol)
    for role in ("meter-mask", "meter-peak"):
        if "mod-role=\"" + role + "\"" not in html:
            raise SystemExit("icon template has no mod-role=" + role)

    values = {s: (v - lo) / (hi - lo) for s, (v, lo, hi) in PORTS.items()}
    script = """
<script>
const values = %s;
document.querySelectorAll('[mod-widget=film]').forEach(el => {
  const sym = el.getAttribute('mod-port-symbol');
  const cs = getComputedStyle(el);
  const size = parseFloat(cs.width);
  const bs = cs.backgroundSize.split(' ').map(parseFloat);
  const steps = Math.max(1, Math.round(bs[0] / size) - 1);
  const rot = Math.round(values[sym] * steps);
  el.style.backgroundPosition = (-rot * size) + 'px 0px';
});
document.querySelectorAll('select[mod-port-symbol]').forEach(el => {
  el.value = String(%s[el.getAttribute('mod-port-symbol')]);
});
// same arithmetic as script-guitar-midi.js, at the demo levels
const monitors = %s, MIN_DB = -80.0, MAX_DB = 0.0;
const pct = db => (100.0 * (Math.max(MIN_DB, Math.min(MAX_DB, db)) - MIN_DB) / (MAX_DB - MIN_DB)).toFixed(2) + '%%';
document.querySelectorAll('[mod-role=meter-mask]').forEach(el => { el.style.left = pct(monitors.inlevel); });
document.querySelectorAll('[mod-role=meter-peak]').forEach(el => { el.style.left = pct(monitors.inpeak); });
</script>""" % (repr(values).replace("'", '"'),
               repr({s: PORTS[s][0] for s in PORTS}).replace("'", '"'),
               repr(MONITORS).replace("'", '"'))
    page = ("<!DOCTYPE html><html><head><meta charset='utf-8'><style>"
            "html,body{margin:0;padding:0;background:#000}"
            "*{font-family:'DejaVu Sans',sans-serif}"
            "%s</style></head><body>%s%s</body></html>" % (css, html, script))
    return page, symbols


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("modgui", nargs="?", default=os.path.join(here, "..", "modgui"))
    ap.add_argument("--out-dir", default=None)
    ap.add_argument("--keep", action="store_true", help="keep the temporary page")
    args = ap.parse_args()
    modgui = os.path.abspath(args.modgui)
    out = os.path.abspath(args.out_dir or modgui)
    if CHROME is None:
        raise SystemExit("no chrome/chromium found")

    page, symbols = build_page(modgui)
    # the page references the modgui images by relative name, so it has to live
    # next to them
    fd, pagepath = tempfile.mkstemp(prefix="_render", suffix=".html", dir=modgui)
    with os.fdopen(fd, "w") as f:
        f.write(page)
    shot = pagepath[:-5] + ".png"
    try:
        subprocess.run([CHROME, "--headless", "--disable-gpu", "--no-sandbox",
                        "--hide-scrollbars", "--force-device-scale-factor=2",
                        "--window-size=%d,%d" % (PEDAL_W, PEDAL_H),
                        "--screenshot=" + shot, "file://" + pagepath],
                       check=True, capture_output=True, timeout=120)
        got = Image.open(shot).convert("RGBA")
    finally:
        if not args.keep:
            os.unlink(pagepath)
            if os.path.exists(shot):
                os.unlink(shot)
    got = got.resize((PEDAL_W, PEDAL_H), Image.LANCZOS)
    got.save(os.path.join(out, "screenshot.png"))
    got.resize((THUMB_W, THUMB_H), Image.LANCZOS).save(os.path.join(out, "thumbnail.png"))
    print("%d ports exposed: %s" % (len(symbols), " ".join(sorted(symbols))))
    print("meter ports: %s" % " ".join(sorted(MONITORS)))
    print("wrote screenshot.png (%dx%d) and thumbnail.png (%dx%d) to %s"
          % (PEDAL_W, PEDAL_H, THUMB_W, THUMB_H, out))


if __name__ == "__main__":
    main()
