#!/usr/bin/env python3
"""Generate the GuitarMidi modgui artwork (pedal plate, knob/switch film strips).

The MOD modgui has no vector primitives: knobs and switches are film strips
(one frame per value step) that mod-ui shifts with background-position, and the
pedal body is a single background image.  All of the artwork here is drawn
procedurally so the palette stays in sync with the desktop UI (see COL_* in
ui/guitarmidi_ui.cpp) and the images can be regenerated without a design tool.

Images are rendered at 2x the CSS size and referenced with a 1x background-size,
which is what mod-ui expects for crisp output on HiDPI screens (same convention
as the other Dataforte modguis).

usage: gen_modgui_art.py [modgui-dir]   (default: the modgui next to this dir)
requires: pillow
"""

import os
import sys

from PIL import Image, ImageDraw

# --- palette (mirrors ui/guitarmidi_ui.cpp) --------------------------------
BG_TOP = (29, 35, 44, 255)      # #1d232c
BG_BOT = (12, 15, 20, 255)      # #0c0f14
EDGE = (57, 66, 79, 255)        # #39424f
TEXT = (232, 236, 242, 255)
DIM = (125, 135, 148, 255)
ACCENT_BLUE = (89, 160, 255, 255)     # COL_ACCENT3 (input / tracking)
ACCENT_TEAL = (0, 199, 154, 255)      # COL_ACCENT  (onset / filter)
ACCENT_ORANGE = (255, 158, 51, 255)   # COL_ACCENT2 (offset / routing)

KNOB_SIZE = 50          # CSS px
KNOB_FRAMES = 61        # 1 frame per 4.5 degrees of a 270 degree sweep
SWITCH_SIZE = 32        # CSS px
PEDAL_W, PEDAL_H = 420, 660   # CSS px

SS = 4                  # supersampling factor for the vector-ish drawing


def lerp(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def vertical_gradient(w, h, top, bottom):
    img = Image.new("RGB", (1, h))
    px = img.load()
    for y in range(h):
        px[0, y] = lerp(top, bottom, y / max(1, h - 1))
    return img.resize((w, h), Image.BILINEAR).convert("RGBA")


def rounded_mask(size, radius):
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, size[0] - 1, size[1] - 1], radius=radius, fill=255)
    return mask


def draw_pedal():
    tw, th, ss = PEDAL_W * 2, PEDAL_H * 2, 2   # 2x target, drawn at 2x again
    w, h = tw * ss, th * ss
    body = vertical_gradient(w, h, BG_TOP, BG_BOT)

    top = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(top)
    # accent line along the top edge, like the desktop UI header
    d.rectangle([0, 0, w, 3 * ss], fill=ACCENT_TEAL)
    # header band separator
    d.line([16 * ss, 68 * ss, w - 16 * ss, 68 * ss], fill=(255, 255, 255, 18),
           width=ss // 2 + 1)
    # bottom footer separator
    d.line([16 * ss, 578 * ss, w - 16 * ss, 578 * ss], fill=(255, 255, 255, 14),
           width=ss // 2 + 1)
    body = Image.alpha_composite(body, top)

    # bevel: dark outer edge, faint inner highlight
    frame = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(frame)
    d.rounded_rectangle([1, 1, w - 2, h - 2], radius=28, outline=EDGE, width=2)
    d.rounded_rectangle([5, 5, w - 6, h - 6], radius=25,
                        outline=(255, 255, 255, 12), width=1)
    body = Image.alpha_composite(body, frame)

    body = body.resize((tw, th), Image.LANCZOS)
    mask = rounded_mask(body.size, 14 * 2)
    out = Image.new("RGBA", body.size, (0, 0, 0, 0))
    out.paste(body, (0, 0), mask)
    return out


def draw_knob_frame(size, angle_deg, accent):
    """One film frame: body, value arc from -135 deg, pointer, centre cap."""
    big = size * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = big / 2.0
    body_r = 0.86 * big / 2.0

    # drop shadow
    d.ellipse([c - body_r, c - body_r + big * 0.02,
               c + body_r, c + body_r + big * 0.02], fill=(0, 0, 0, 70))
    # body gradient (lighter towards the top-left)
    body = vertical_gradient(big, big, (58, 66, 79, 255), (22, 26, 33, 255))
    bmask = Image.new("L", (big, big), 0)
    ImageDraw.Draw(bmask).ellipse(
        [c - body_r, c - body_r, c + body_r, c + body_r], fill=255)
    img.paste(body, (0, 0), bmask)

    d.ellipse([c - body_r, c - body_r, c + body_r, c + body_r],
              outline=(74, 84, 98, 255), width=max(1, big // 50))
    d.ellipse([c - body_r + big * 0.055, c - body_r + big * 0.055,
               c + body_r - big * 0.055, c + body_r - big * 0.055],
              outline=(255, 255, 255, 16), width=max(1, big // 90))

    arc_r = body_r * 0.72
    box = [c - arc_r, c - arc_r, c + arc_r, c + arc_r]
    track_w = max(2, int(big * 0.048))
    # 0 deg points up, -135 deg sweeps to +135 deg
    start = -135 - 90
    end = -135 + 270 - 90
    d.arc(box, start, end, fill=(255, 255, 255, 26), width=track_w)
    if angle_deg > -135.0:
        d.arc(box, start, angle_deg - 90, fill=accent, width=track_w)

    # pointer
    import math
    a = math.radians(angle_deg)
    sx, sy = c + math.sin(a) * big * 0.10, c - math.cos(a) * big * 0.10
    ex, ey = c + math.sin(a) * body_r * 0.74, c - math.cos(a) * body_r * 0.74
    d.line([sx, sy, ex, ey], fill=accent, width=max(2, int(big * 0.038)))

    # centre cap
    cr = big * 0.075
    d.ellipse([c - cr, c - cr, c + cr, c + cr], fill=(14, 17, 22, 255),
              outline=(70, 79, 92, 255), width=max(1, big // 64))
    return img.resize((size, size), Image.LANCZOS)


def draw_knob_strip(accent):
    size = KNOB_SIZE * 2
    strip = Image.new("RGBA", (size * KNOB_FRAMES, size), (0, 0, 0, 0))
    for i in range(KNOB_FRAMES):
        angle = -135.0 + 270.0 * i / (KNOB_FRAMES - 1)
        strip.paste(draw_knob_frame(size, angle, accent), (i * size, 0))
    return strip


def draw_switch_frame(size, on, accent):
    big = size * SS
    img = Image.new("RGBA", (big, size * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = big * 0.06
    d.rounded_rectangle([pad, pad, big - pad, big - pad], radius=big * 0.14,
                        fill=(26, 31, 39, 255),
                        outline=accent if on else (59, 69, 82, 255),
                        width=max(1, big // 32))
    # paddle: lever at the bottom (off) or top (on)
    if on:
        led_y = big * 0.24
        lever = [big * 0.30, big * 0.52, big * 0.70, big * 0.80]
    else:
        led_y = big * 0.76
        lever = [big * 0.30, big * 0.20, big * 0.70, big * 0.48]
    d.rounded_rectangle(lever, radius=big * 0.06,
                        fill=(58, 66, 79, 255) if not on else (72, 82, 97, 255),
                        outline=(20, 24, 30, 255), width=max(1, big // 48))
    r = big * 0.075
    d.ellipse([big * 0.5 - r, led_y - r, big * 0.5 + r, led_y + r],
              fill=accent if on else (74, 84, 98, 255))
    return img.resize((size, size), Image.LANCZOS)


def draw_switch_strip():
    size = SWITCH_SIZE * 2
    strip = Image.new("RGBA", (size * 2, size), (0, 0, 0, 0))
    for i, on in enumerate((False, True)):
        strip.paste(draw_switch_frame(size, on, ACCENT_ORANGE), (i * size, 0))
    return strip


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    modgui = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "modgui")
    modgui = os.path.abspath(modgui)

    draw_pedal().save(os.path.join(modgui, "pedal.png"))
    draw_knob_strip(ACCENT_BLUE).save(os.path.join(modgui, "knob-blue.png"))
    draw_knob_strip(ACCENT_TEAL).save(os.path.join(modgui, "knob-teal.png"))
    draw_knob_strip(ACCENT_ORANGE).save(os.path.join(modgui, "knob-orange.png"))
    draw_switch_strip().save(os.path.join(modgui, "switch.png"))
    print("wrote pedal.png, knob-{blue,teal,orange}.png, switch.png to", modgui)


if __name__ == "__main__":
    main()
