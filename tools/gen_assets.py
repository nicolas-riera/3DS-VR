#!/usr/bin/env python3
"""Generates every image, sound and 3D model used by 3DS VR.

Pure standard library (no Pillow needed). Run from anywhere:
    python3 tools/gen_assets.py

Outputs:
    console/meta/icon.png, banner.png, banner.wav          HOME Menu icon, banner and jingle
    streamer/app/app.ico                                   PC app icon
    streamer/driver/resources/icons/<model>_status_*.png   SteamVR status icons (+ _2x, animated .gif)
    streamer/driver/resources/driver.vrresources           maps each console model to its icons
    streamer/driver/resources/rendermodels/3dsvr_<model>/  SteamVR render models (OBJ)

The status icons follow SteamVR's own style (see drivers/htc/resources/icons in
a SteamVR install): a flat #85B717 silhouette with the details cut out, grey
when off, outline only in standby, a pulsing animated GIF while searching, and
round badges in the bottom-right corner ("i" alert, "!" error, lightning bolt
for low battery) separated from the silhouette by a transparent gap.
"""
import json
import math
import os
import struct
import wave
import zlib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
META = os.path.join(ROOT, "console", "meta")
DRIVER_RES = os.path.join(ROOT, "streamer", "driver", "resources")
APP = os.path.join(ROOT, "streamer", "app")


def hexc(h, a=1.0):
    return (int(h[0:2], 16) / 255, int(h[2:4], 16) / 255, int(h[4:6], 16) / 255, a)


def mix(c0, c1, t):
    t = max(0.0, min(1.0, t))
    return tuple(c0[i] + (c1[i] - c0[i]) * t for i in range(4))


# ------------------------------------------------------------------ SDF
#
# Shapes are signed distance functions (negative inside), in the units of a
# reference canvas. Rendering scales them, so every icon is drawn once and
# exported at 1x and 2x with analytic antialiasing.


def sd_round_rect(x0, y0, x1, y1, r):
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    hx, hy = (x1 - x0) / 2 - r, (y1 - y0) / 2 - r

    def f(x, y):
        qx, qy = abs(x - cx) - hx, abs(y - cy) - hy
        return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r
    return f


def sd_circle(cx, cy, r):
    return lambda x, y: math.hypot(x - cx, y - cy) - r


def sd_ellipse(cx, cy, rx, ry):
    # approximation, good enough for small shapes
    def f(x, y):
        k = math.hypot((x - cx) / rx, (y - cy) / ry)
        return (k - 1) * min(rx, ry)
    return f


def sd_segment(ax, ay, bx, by, r):
    def f(x, y):
        px, py, dx, dy = x - ax, y - ay, bx - ax, by - ay
        h = max(0.0, min(1.0, (px * dx + py * dy) / (dx * dx + dy * dy)))
        return math.hypot(px - dx * h, py - dy * h) - r
    return f


def sd_polygon(pts):
    def f(x, y):
        d = min(sd_segment(*pts[i], *pts[(i + 1) % len(pts)], 0)(x, y) for i in range(len(pts)))
        inside = False
        j = len(pts) - 1
        for i in range(len(pts)):
            xi, yi = pts[i]
            xj, yj = pts[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                inside = not inside
            j = i
        return -d if inside else d
    return f


def union(*fs):
    return lambda x, y: min(f(x, y) for f in fs)


def subtract(a, *bs):
    return lambda x, y: max([a(x, y)] + [-b(x, y) for b in bs])


def outline(f, w):
    return lambda x, y: abs(f(x, y)) - w / 2


def grow(f, d):
    return lambda x, y: f(x, y) - d


class Image:
    """Float RGBA image; shapes are given in reference units and scaled by `scale`."""

    def __init__(self, w, h, scale=1.0, ss=3):
        self.w, self.h, self.scale, self.ss = w, h, scale, ss
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(w * h)]

    def paint(self, sdf, color, bbox=None):
        s = self.scale
        x0, y0, x1, y1 = (0, 0, self.w, self.h) if bbox is None else (
            max(0, int(bbox[0] * s) - 1), max(0, int(bbox[1] * s) - 1),
            min(self.w, int(math.ceil(bbox[2] * s)) + 1), min(self.h, int(math.ceil(bbox[3] * s)) + 1))
        ss = self.ss
        for py in range(y0, y1):
            for px in range(x0, x1):
                cov = 0.0
                # analytic AA per sub-sample (smooth even for tiny details)
                for sy in range(ss):
                    for sx in range(ss):
                        rx = (px + (sx + 0.5) / ss) / s
                        ry = (py + (sy + 0.5) / ss) / s
                        d = sdf(rx, ry) * s * ss
                        cov += max(0.0, min(1.0, 0.5 - d))
                cov /= ss * ss
                if cov <= 0:
                    continue
                c = color((px + 0.5) / s, (py + 0.5) / s) if callable(color) else color
                a = c[3] * cov
                p = self.px[py * self.w + px]
                na = a + p[3] * (1 - a)
                if na > 0:
                    for i in range(3):
                        p[i] = (c[i] * a + p[i] * p[3] * (1 - a)) / na
                p[3] = na

    def rgba8(self):
        return [tuple(int(round(max(0, min(1, v)) * 255)) for v in p) for p in self.px]

    def png(self):
        return encode_png(self.w, self.h, self.rgba8())

    def save_png(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(self.png())


def encode_png(w, h, pixels):
    raw = b"".join(b"\x00" + bytes(v for p in pixels[y * w:(y + 1) * w] for v in p) for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


# ------------------------------------------------------------------ GIF


def lzw_encode(indices, min_code_size):
    clear, eoi = 1 << min_code_size, (1 << min_code_size) + 1
    out, bitbuf, bitcount = bytearray(), 0, 0
    code_size = min_code_size + 1

    def emit(code):
        nonlocal bitbuf, bitcount
        bitbuf |= code << bitcount
        bitcount += code_size
        while bitcount >= 8:
            out.append(bitbuf & 0xFF)
            bitbuf >>= 8
            bitcount -= 8

    table = {(i,): i for i in range(clear)}
    next_code = eoi + 1
    emit(clear)
    w = ()
    for k in indices:
        wk = w + (k,)
        if wk in table:
            w = wk
            continue
        emit(table[w])
        if next_code < 4096:
            table[wk] = next_code
            next_code += 1
            if next_code > (1 << code_size) and code_size < 12:
                code_size += 1
        else:
            emit(clear)
            table = {(i,): i for i in range(clear)}
            next_code = eoi + 1
            code_size = min_code_size + 1
        w = (k,)
    if w:
        emit(table[w])
    emit(eoi)
    if bitcount:
        out.append(bitbuf & 0xFF)
    return bytes(out)


GIF_MATTE = hexc("1d1e23")  # SteamVR's dark background, used to flatten antialiased edges


def encode_gif(w, h, frames, delay_cs=4):
    """frames: lists of RGBA8 tuples. Binary transparency, one local palette per frame."""
    out = bytearray(b"GIF89a" + struct.pack("<HHBBB", w, h, 0, 0, 0))
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"  # loop forever
    for pixels in frames:
        palette = {(0, 0, 0): 0}  # index 0: transparent
        idx = []
        for r, g, b, a in pixels:
            if a < 90:
                idx.append(0)
                continue
            t = a / 255
            c = tuple(int(round(v * 255 * (1 - t) + comp * t)) for v, comp in zip(GIF_MATTE[:3], (r, g, b)))
            c = (c[0] & 0xFC, c[1] & 0xFC, c[2] & 0xFC)
            if c not in palette:
                if len(palette) >= 256:
                    c = min(palette, key=lambda p: sum((p[i] - c[i]) ** 2 for i in range(3)) if palette[p] else 1e9)
                else:
                    palette[c] = len(palette)
            idx.append(palette[c])
        bits = max(1, (len(palette) - 1).bit_length())
        size = 1 << bits
        table = [(0, 0, 0)] * size
        for c, i in palette.items():
            table[i] = c
        out += b"\x21\xF9\x04" + struct.pack("<BHBB", 0x09, delay_cs, 0, 0)  # restore bg, transparent idx 0
        out += b"\x2C" + struct.pack("<HHHHB", 0, 0, w, h, 0x80 | (bits - 1))
        out += bytes(v for c in table for v in c)
        mcs = max(2, bits)
        data = lzw_encode(idx, mcs)
        out.append(mcs)
        for i in range(0, len(data), 255):
            block = data[i:i + 255]
            out.append(len(block))
            out += block
        out.append(0)
    out.append(0x3B)
    return bytes(out)


# ------------------------------------------------- console silhouettes
#
# Front view of each console, open, in a 100x64 reference box (the size of a
# SteamVR 2x headset icon). Real proportions, scaled to fill the height.

MODELS = {
    "3ds": dict(name="Nintendo 3DS", w=134, d=74, clam=True, new=False, r=0.10, color="3aa7d0", trim="1f6f91"),
    "3dsxl": dict(name="Nintendo 3DS XL", w=156, d=93, clam=True, new=False, r=0.16, color="b8273a", trim="1c1c22"),
    "new3ds": dict(name="New Nintendo 3DS", w=142, d=81, clam=True, new=True, r=0.12, color="e9e9ec", trim="b9bac2"),
    "2ds": dict(name="Nintendo 2DS", w=144, d=127, clam=False, new=False, r=0.10, color="2b2d33", trim="2f6fd6"),
    "new3dsxl": dict(name="New Nintendo 3DS XL", w=160, d=94, clam=True, new=True, r=0.17, color="2a5a9e",
                     trim="16243c"),
    "new2dsxl": dict(name="New Nintendo 2DS XL", w=160, d=86, clam=True, new=True, r=0.26, color="232428",
                     trim="2ec4bd"),
}
GENERIC = dict(name="3DS VR", w=142, d=81, clam=True, new=False, r=0.12, color="5ca8ff", trim="2b5f9e")

BADGE_R = 11.5  # radius in SteamVR's 2x headset icons
BADGE_GAP = 2.6


def badge_pos(m):
    """Badge centre: over the bottom-right corner of the console, as SteamVR does."""
    _, _, _, (x0, y0, x1, y1) = console_parts(m)
    return (min(x1 - 1.0, 100 - BADGE_R - 1), y1 - BADGE_R + 1.5, BADGE_R)


def console_parts(m, cx=50.0, top=2.0, height=60.0):
    """Returns (body, cutouts, frame, bounds). `frame` is the outer outline used for standby."""
    if m["clam"]:
        hinge_gap = 2.0
        half_h = (height - hinge_gap) / 2
        s = half_h / m["d"]
        w = m["w"] * s
        r = m["r"] * half_h * 1.4
        x0, x1 = cx - w / 2, cx + w / 2
        lid = sd_round_rect(x0, top, x1, top + half_h, r)
        b0 = top + half_h + hinge_gap
        base = sd_round_rect(x0, b0, x1, b0 + half_h, r)
        body = union(lid, base)
        sw, sh = w * 0.66, half_h * 0.66
        top_screen = sd_round_rect(cx - sw / 2, top + half_h * 0.17, cx + sw / 2, top + half_h * 0.17 + sh, 1.2)
        bw, bh = w * 0.46, half_h * 0.62
        bottom_screen = sd_round_rect(cx - bw / 2, b0 + half_h * 0.16, cx + bw / 2, b0 + half_h * 0.16 + bh, 1.0)
        side = (w / 2 + bw / 2) / 2  # between the touch screen and the edge
        pad = sd_circle(cx - side, b0 + half_h * 0.30, w * 0.065)
        dpad = union(sd_round_rect(cx - side - w * 0.05, b0 + half_h * 0.66 - 0.9, cx - side + w * 0.05,
                                   b0 + half_h * 0.66 + 0.9, 0.5),
                     sd_round_rect(cx - side - 0.9, b0 + half_h * 0.66 - w * 0.05, cx - side + 0.9,
                                   b0 + half_h * 0.66 + w * 0.05, 0.5))
        bx, by, k = cx + side, b0 + half_h * (0.40 if m["new"] else 0.34), w * 0.045
        abxy = union(*(sd_circle(bx + dx * k, by + dy * k, w * 0.022) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))))
        cuts = [top_screen, bottom_screen, pad, dpad, abxy]
        if m["new"]:
            cuts.append(sd_circle(bx, b0 + half_h * 0.14, w * 0.018))  # C-Stick
        return body, cuts, union(lid, base), (x0, top, x1, top + height)
    # 2DS: one slab, wider at the top (the screens) than a clamshell half
    s = height / m["d"]
    w = m["w"] * s
    x0, x1 = cx - w / 2, cx + w / 2
    body = sd_round_rect(x0, top, x1, top + height, m["r"] * height * 0.7)
    sw, sh = w * 0.58, height * 0.36
    top_screen = sd_round_rect(cx - sw / 2, top + height * 0.07, cx + sw / 2, top + height * 0.07 + sh, 1.2)
    bw, bh = w * 0.42, height * 0.33
    bottom_screen = sd_round_rect(cx - bw / 2, top + height * 0.55, cx + bw / 2, top + height * 0.55 + bh, 1.0)
    split = sd_round_rect(x0 - 1, top + height * 0.47, x1 + 1, top + height * 0.47 + 1.1, 0.5)  # seam
    side = (w / 2 + bw / 2) / 2
    pad = sd_circle(cx - side, top + height * 0.62, w * 0.06)
    bx, by, k = cx + side, top + height * 0.64, w * 0.042
    abxy = union(*(sd_circle(bx + dx * k, by + dy * k, w * 0.02) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))))
    return body, [top_screen, bottom_screen, split, pad, abxy], body, (x0, top, x1, top + height)


GREEN = hexc("85b717")
GREEN_DARK = hexc("3c4a24")
GREY = hexc("49494f")
ERROR_A, ERROR_B = hexc("e8128a"), hexc("ec153d")
LOW_A, LOW_B = hexc("ed2925"), hexc("f15a29")


def badge_shape(kind, pos):
    bx, by, r = pos
    circle = sd_circle(bx, by, r)
    if kind == "info":
        i_shape = union(sd_circle(bx, by - r * 0.45, r * 0.15),
                        sd_round_rect(bx - r * 0.13, by - r * 0.16, bx + r * 0.13, by + r * 0.58, r * 0.06))
        return subtract(circle, i_shape), None
    if kind == "error":
        mark = union(sd_round_rect(bx - r * 0.13, by - r * 0.62, bx + r * 0.13, by + r * 0.22, r * 0.1),
                     sd_circle(bx, by + r * 0.48, r * 0.15))
        return circle, mark
    if kind == "low":
        bolt = sd_polygon([(bx + r * 0.18, by - r * 0.68), (bx - r * 0.38, by + r * 0.10), (bx - r * 0.02, by + r * 0.10),
                           (bx - r * 0.20, by + r * 0.70), (bx + r * 0.40, by - r * 0.12), (bx + r * 0.04, by - r * 0.12)])
        return subtract(circle, bolt), None
    raise ValueError(kind)


def status_icon(m, state, scale, fill=None):
    """state: off, ready, standby, error, ready_alert, standby_alert, ready_low, searching."""
    img = Image(int(100 * scale), int(64 * scale), scale)
    body, cuts, frame, _ = console_parts(m)
    pos = badge_pos(m)
    badge = {"ready_alert": "info", "standby_alert": "info", "searching_alert": "info", "error": "error",
             "ready_low": "low"}.get(state)
    color = fill or (GREY if state == "off" else GREEN)
    gap = grow(sd_circle(*pos), BADGE_GAP) if badge else None

    if state.startswith("standby"):
        shape = union(outline(frame, 3.0), *[outline(c, 2.0) for c in cuts[:2]])
    else:
        shape = subtract(body, *cuts)
    if gap:
        shape = subtract(shape, gap)
    img.paint(shape, color)

    if badge:
        disc, mark = badge_shape(badge, pos)
        bx, by, r = pos
        box = (bx - r - 1, by - r - 1, bx + r + 1, by + r + 1)
        if badge == "info":
            img.paint(disc, color, box)
        elif badge == "error":
            img.paint(disc, lambda x, y: mix(ERROR_A, ERROR_B, (x - bx + y - by) / (4 * r) + 0.5), box)
            img.paint(mark, hexc("ffffff"), box)
        else:
            img.paint(disc, lambda x, y: mix(LOW_A, LOW_B, (y - by) / (2 * r) + 0.5), box)
    return img


ICON_STATES = ("off", "ready", "ready_alert", "standby", "standby_alert", "error", "ready_low")


def make_status_icons():
    out = os.path.join(DRIVER_RES, "icons")
    os.makedirs(out, exist_ok=True)
    for slug, m in list(MODELS.items()) + [("generic", GENERIC)]:
        for scale, suffix in ((0.5, ""), (1.0, "_2x")):
            for state in ICON_STATES:
                status_icon(m, state, scale).save_png(os.path.join(out, "%s_status_%s%s.png" % (slug, state, suffix)))
            # searching: SteamVR's linear pulse to dark green and back, 29 frames
            for state in ("searching", "searching_alert"):
                # one coverage mask, recoloured per frame
                img = status_icon(m, "ready_alert" if state == "searching_alert" else "ready", scale,
                                  fill=(1.0, 1.0, 1.0, 1.0))
                mask = [p[3] for p in img.px]
                frames = []
                for k in range(29):
                    c = mix(GREEN, GREEN_DARK, (k if k <= 14 else 28 - k) / 14.0)
                    rgb8 = tuple(int(round(v * 255)) for v in c[:3])
                    frames.append([rgb8 + (int(round(a * 255)),) for a in mask])
                with open(os.path.join(out, "%s_status_%s%s.gif" % (slug, state, suffix)), "wb") as f:
                    f.write(encode_gif(img.w, img.h, frames))


def make_vrresources():
    """SteamVR picks status icons by Prop_ModelNumber_String: one entry per console model."""
    def entry(slug):
        p = "{3dsvr}/icons/" + slug + "_status_"
        return {
            "Prop_NamedIconPathDeviceOff_String": p + "off.png",
            "Prop_NamedIconPathDeviceSearching_String": p + "searching.gif",
            "Prop_NamedIconPathDeviceSearchingAlert_String": p + "searching_alert.gif",
            "Prop_NamedIconPathDeviceReady_String": p + "ready.png",
            "Prop_NamedIconPathDeviceReadyAlert_String": p + "ready_alert.png",
            "Prop_NamedIconPathDeviceNotReady_String": p + "error.png",
            "Prop_NamedIconPathDeviceStandby_String": p + "standby.png",
            "Prop_NamedIconPathDeviceStandbyAlert_String": p + "standby_alert.png",
            "Prop_NamedIconPathDeviceAlertLow_String": p + "ready_low.png",
        }
    icons = {"HMD": entry("generic"), "3DS VR": {"Alias": "HMD"}}
    for slug, m in MODELS.items():
        icons[m["name"]] = entry(slug)
    with open(os.path.join(DRIVER_RES, "driver.vrresources"), "w", newline="\n") as f:
        json.dump({"jsonid": "vrresources", "statusicons": icons}, f, indent="\t")
        f.write("\n")


# ------------------------------------------------------------ brand art
#
# The app icon is an open 3DS whose top screen shows a stereo VR view (two
# lenses), on a dark rounded square.

NAVY_A, NAVY_B = hexc("1f2747"), hexc("0c0f1a")
SHELL_A, SHELL_B = hexc("f4f6fb"), hexc("c9d0de")
SCREEN = hexc("0b0f1c")
BLUE, BLUE_HI = hexc("4f9dff"), hexc("a8d4ff")


def draw_brand_console(img, cx, top, height, lens=True):
    half = (height - 2.4) / 2
    w = half * 1.82
    x0, x1 = cx - w / 2, cx + w / 2
    r = half * 0.16
    b0 = top + half + 2.4
    shell = lambda x, y: mix(SHELL_A, SHELL_B, (y - top) / height)
    # soft shadow
    img.paint(sd_round_rect(x0 + 1.2, top + 1.6, x1 + 1.2, b0 + half + 1.6, r), hexc("000000", 0.35))
    img.paint(sd_round_rect(x0, top, x1, top + half, r), shell)
    img.paint(sd_round_rect(x0 + w * 0.08, b0 - 1.0, x1 - w * 0.08, b0 + 0.6, 0.4), hexc("8d96aa"))  # hinge
    img.paint(sd_round_rect(x0, b0, x1, b0 + half, r), shell)
    sw, sh = w * 0.70, half * 0.70
    sx0, sy0 = cx - sw / 2, top + half * 0.15
    img.paint(sd_round_rect(sx0, sy0, sx0 + sw, sy0 + sh, 1.0), SCREEN)
    if lens:
        # stereo view: two lenses with a glow, the "3D" of 3DS VR
        for s in (-1, 1):
            lx = cx + s * sw * 0.235
            ly = sy0 + sh * 0.52
            img.paint(sd_ellipse(lx, ly, sw * 0.205, sh * 0.36), lambda x, y: mix(BLUE_HI, BLUE, (y - sy0) / sh))
            img.paint(sd_ellipse(lx - sw * 0.06, ly - sh * 0.12, sw * 0.05, sh * 0.08), hexc("ffffff", 0.7))
    bw, bh = w * 0.48, half * 0.62
    img.paint(sd_round_rect(cx - bw / 2, b0 + half * 0.16, cx + bw / 2, b0 + half * 0.16 + bh, 0.8), SCREEN)
    side = (w / 2 + bw / 2) / 2
    img.paint(sd_circle(cx - side, b0 + half * 0.32, w * 0.07), hexc("6f7891"))
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        img.paint(sd_circle(cx + side + dx * w * 0.045, b0 + half * 0.36 + dy * w * 0.045, w * 0.021), hexc("6f7891"))


def app_icon(size):
    s = size / 48.0
    img = Image(size, size, s, ss=3 if size <= 64 else 2)
    img.paint(sd_round_rect(1, 1, 47, 47, 10), lambda x, y: mix(NAVY_A, NAVY_B, (x + y) / 96))
    draw_brand_console(img, 24, 6.5, 35)
    return img


def make_icon():
    app_icon(48).save_png(os.path.join(META, "icon.png"))


def ico_dib(img):
    """32-bit BGRA DIB entry for an .ico file (bottom-up, with an empty AND mask)."""
    w, h, px = img.w, img.h, img.rgba8()
    data = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[y * w + x]
            data += bytes((b, g, r, a))
    mask_row = ((w + 31) // 32) * 4
    data += bytes(mask_row * h)
    return data


def make_app_ico():
    images = []
    for size in (16, 24, 32, 48, 64, 128, 256):
        img = app_icon(size)
        images.append((size, img.png() if size >= 256 else ico_dib(img)))
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for size, data in images:
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        blobs += data
    os.makedirs(APP, exist_ok=True)
    with open(os.path.join(APP, "app.ico"), "wb") as f:
        f.write(header + entries + blobs)


FONT = {
    "3": ["#### ", "    #", "    #", " ### ", "    #", "    #", "#### "],
    "D": ["#### ", "#   #", "#   #", "#   #", "#   #", "#   #", "#### "],
    "S": [" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "],
    "V": ["#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "],
    "R": ["#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"],
    "F": ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "],
    "O": [" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "],
    "T": ["#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "],
    "E": ["#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"],
    "A": [" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"],
    "M": ["#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"],
    " ": ["     "] * 7,
}


def draw_text(img, x, y, s, k, color, shadow=None):
    for dx, dy, col in ([(k * 0.35, k * 0.35, shadow)] if shadow else []) + [(0, 0, color)]:
        cx = x
        for ch in s:
            for gy, line in enumerate(FONT[ch]):
                for gx, p in enumerate(line):
                    if p == "#":
                        px, py = cx + gx * k + dx, y + gy * k + dy
                        img.paint(sd_round_rect(px, py, px + k * 0.92, py + k * 0.92, k * 0.25), col,
                                  (px, py, px + k, py + k))
            cx += 6 * k


def text_width(s, k):
    return (6 * len(s) - 1) * k


def make_banner():
    img = Image(256, 128, 1.0, ss=2)
    img.paint(lambda x, y: -1, lambda x, y: mix(NAVY_A, NAVY_B, y / 128))
    for i in range(6):
        img.paint(sd_ellipse(64, 66, 60 - i * 8, 50 - i * 7), hexc("4f9dff", 0.05), (0, 0, 130, 128))
    draw_brand_console(img, 64, 18, 92)
    title, sub = "3DS VR", "FOR STEAMVR"
    draw_text(img, 190 - text_width(title, 3.4) / 2, 42, title, 3.4, hexc("eef3ff"), hexc("000000", 0.45))
    draw_text(img, 190 - text_width(sub, 1.6) / 2, 78, sub, 1.6, BLUE_HI, hexc("000000", 0.45))
    img.save_png(os.path.join(META, "banner.png"))


def make_jingle():
    """HOME Menu banner sound: a rising "power on" sweep into a wide major
    chord with a slow stereo shimmer. Stereo PCM16 at 32728 Hz, under 3 s."""
    rate = 32728
    n = int(2.2 * rate)
    left = [0.0] * n
    right = [0.0] * n
    phase = 0.0
    for i in range(int(0.45 * rate)):
        t = i / rate
        f = 220 * 2 ** (t / 0.45 * 2)
        phase += 2 * math.pi * f / rate
        env = min(1.0, t * 60) * (1 - t / 0.45) ** 0.5
        v = 0.35 * env * math.sin(phase)
        left[i] += v
        right[i] += v
    for k, f in enumerate((440.0, 554.37, 659.25, 880.0)):
        start = 0.4 + k * 0.06
        for i in range(int(start * rate), n):
            t = i / rate - start
            env = min(1.0, t * 30) * math.exp(-t * 1.6)
            a = 2 * math.pi * f * t
            v_l = math.sin(a * 0.998) + 0.25 * math.sin(2 * a)
            v_r = math.sin(a * 1.002) + 0.25 * math.sin(2 * a)
            shimmer = 0.5 + 0.5 * math.sin(2 * math.pi * 3 * t + k)
            left[i] += 0.16 * env * v_l * (0.6 + 0.4 * shimmer)
            right[i] += 0.16 * env * v_r * (1.0 - 0.4 * shimmer)
    peak = max(max(abs(x) for x in left), max(abs(x) for x in right))
    gain = 0.85 / peak
    fade = int(0.08 * rate)
    data = bytearray()
    for i in range(n):
        g = gain * min(1.0, (n - i) / fade)
        data += struct.pack("<hh", int(left[i] * g * 32767), int(right[i] * g * 32767))
    os.makedirs(META, exist_ok=True)
    with wave.open(os.path.join(META, "banner.wav"), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(data))


# --------------------------------------------------------- render models

# 4x4 palette texture, 16 px per swatch.
SWATCH = {"body": 0, "trim": 1, "screen": 2, "bezel": 3, "button": 4, "red": 5, "blue": 6, "green": 7, "yellow": 8,
          "hinge": 9}


def palette_png(m):
    cols = {
        "body": hexc(m["color"]), "trim": hexc(m["trim"]), "screen": hexc("07080c"), "bezel": hexc("15171d"),
        "button": hexc("3a3c44"), "red": hexc("d8423a"), "blue": hexc("3b74d8"), "green": hexc("2ea865"),
        "yellow": hexc("e8b923"), "hinge": tuple(v * 0.75 for v in hexc(m["color"])[:3]) + (1.0,),
    }
    pixels = []
    for y in range(64):
        for x in range(64):
            idx = (y // 16) * 4 + x // 16
            name = next((k for k, v in SWATCH.items() if v == idx), "body")
            pixels.append(tuple(int(round(v * 255)) for v in cols[name][:3]) + (255,))
    return encode_png(64, 64, pixels)


def sub3(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def norm(a):
    l = math.sqrt(sum(x * x for x in a)) or 1.0
    return (a[0] / l, a[1] / l, a[2] / l)


class Mesh:
    def __init__(self):
        self.v, self.vt, self.vn, self.f = [], [], [], []

    def uv(self, swatch):
        i = SWATCH[swatch]
        self.vt.append((((i % 4) * 16 + 8) / 64.0, 1.0 - ((i // 4) * 16 + 8) / 64.0))
        return len(self.vt)

    def quad(self, a, b, c, d, swatch):
        """Counter-clockwise quad seen from the front."""
        self.vn.append(norm(cross(sub3(b, a), sub3(d, a))))
        ni, ti = len(self.vn), self.uv(swatch)
        idx = []
        for p in (a, b, c, d):
            self.v.append(p)
            idx.append(len(self.v))
        self.f.append((idx[0], idx[1], idx[2], ti, ni))
        self.f.append((idx[0], idx[2], idx[3], ti, ni))

    def box(self, center, size, xf, swatch, front=None):
        """Box in local space, transformed by xf. `front` paints the +Z face."""
        cx, cy, cz = center
        hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
        k = {(sx, sy, sz): xf((cx + sx * hx, cy + sy * hy, cz + sz * hz))
             for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)}
        self.quad(k[(-1, -1, 1)], k[(1, -1, 1)], k[(1, 1, 1)], k[(-1, 1, 1)], front or swatch)
        self.quad(k[(1, -1, -1)], k[(-1, -1, -1)], k[(-1, 1, -1)], k[(1, 1, -1)], swatch)
        self.quad(k[(1, -1, 1)], k[(1, -1, -1)], k[(1, 1, -1)], k[(1, 1, 1)], swatch)
        self.quad(k[(-1, -1, -1)], k[(-1, -1, 1)], k[(-1, 1, 1)], k[(-1, 1, -1)], swatch)
        self.quad(k[(-1, 1, 1)], k[(1, 1, 1)], k[(1, 1, -1)], k[(-1, 1, -1)], swatch)
        self.quad(k[(-1, -1, -1)], k[(1, -1, -1)], k[(1, -1, 1)], k[(-1, -1, 1)], swatch)

    def write(self, obj_path, mtl_name, tex_name):
        with open(obj_path, "w", newline="\n") as f:
            f.write("# generated by tools/gen_assets.py\nmtllib %s\nusemtl console\n" % mtl_name)
            for p in self.v:
                f.write("v %.5f %.5f %.5f\n" % p)
            for t in self.vt:
                f.write("vt %.5f %.5f\n" % t)
            for n in self.vn:
                f.write("vn %.5f %.5f %.5f\n" % n)
            for a, b, c, t, n in self.f:
                f.write("f %d/%d/%d %d/%d/%d %d/%d/%d\n" % (a, t, n, b, t, n, c, t, n))
        with open(os.path.join(os.path.dirname(obj_path), mtl_name), "w", newline="\n") as f:
            f.write("newmtl console\nKa 1 1 1\nKd 1 1 1\nKs 0 0 0\nmap_Kd %s\n" % tex_name)


def make_transform(pitch_deg, offset):
    """Rotation around X, then translation (metres)."""
    a = math.radians(pitch_deg)
    ca, sa = math.cos(a), math.sin(a)
    return lambda p: (p[0] + offset[0], p[1] * ca - p[2] * sa + offset[1], p[1] * sa + p[2] * ca + offset[2])


def build_console_mesh(m):
    """The console as held in front of the face, 25 cm ahead of the eyes.
    SteamVR head space: +X right, +Y up, +Z towards the player. Units: metres."""
    mesh = Mesh()
    mm = 0.001
    w, d = m["w"] * mm, m["d"] * mm
    t = 0.021
    half = t / 2
    dist = 0.25
    if m["clam"]:
        lid = make_transform(0, (0, 0, -dist))  # vertical, screen facing the player
        mesh.box((0, d / 2, -half / 2), (w, d, half), lid, "body", front="bezel")
        mesh.box((0, d * 0.52, 0.0004), (w * 0.62, d * 0.68, 0.0008), lid, "screen")
        mesh.box((0, d * 0.94, 0.0003), (w * 0.12, d * 0.03, 0.0006), lid, "trim")  # camera strip
        base = make_transform(-30, (0, 0, -dist))  # tilted 30 degrees towards the player
        mesh.box((0, -d / 2, -half / 2), (w, d, half), base, "body")
        mesh.box((0, -0.004, -half / 2), (w * 0.98, 0.008, half * 1.02), base, "hinge")
        mesh.box((0, -d * 0.52, 0.0004), (w * 0.48, d * 0.62, 0.0008), base, "screen")
        side = w * 0.5 - w * 0.12
        mesh.box((-side, -d * 0.24, 0.002), (0.016, 0.016, 0.004), base, "button")  # Circle Pad
        mesh.box((-side, -d * 0.62, 0.001), (0.013, 0.004, 0.002), base, "button")  # D-Pad
        mesh.box((-side, -d * 0.62, 0.001), (0.004, 0.013, 0.002), base, "button")
        cols = ("red", "blue", "green", "yellow") if m["new"] else ("button",) * 4
        for (dx, dy), col in zip(((1, 0), (0, -1), (0, 1), (-1, 0)), cols):
            mesh.box((side + dx * 0.0065, -d * 0.30 + dy * 0.0065, 0.001), (0.005, 0.005, 0.002), base, col)
        if m["new"]:
            mesh.box((side, -d * 0.12, 0.0015), (0.004, 0.004, 0.003), base, "button")  # C-Stick
        mesh.box((-w / 2 + 0.012, 0.0, -half * 1.05), (0.022, 0.005, 0.004), base, "trim")  # L
        mesh.box((w / 2 - 0.012, 0.0, -half * 1.05), (0.022, 0.005, 0.004), base, "trim")  # R
    else:
        slab = make_transform(0, (0, -d * 0.35, -dist))  # 2DS: one slab, upright
        mesh.box((0, d / 2, -t / 2), (w, d, t), slab, "body")
        mesh.box((0, d * 0.28, -t / 2), (w * 1.002, d * 0.56, t * 1.01), slab, "trim")
        mesh.box((0, d * 0.75, 0.0004), (w * 0.62, d * 0.38, 0.0008), slab, "screen")
        mesh.box((0, d * 0.30, 0.0004), (w * 0.45, d * 0.34, 0.0008), slab, "screen")
        mesh.box((-w * 0.38, d * 0.42, 0.002), (0.016, 0.016, 0.004), slab, "button")
        for dx, dy in ((1, 0), (0, -1), (0, 1), (-1, 0)):
            mesh.box((w * 0.38 + dx * 0.0065, d * 0.32 + dy * 0.0065, 0.001), (0.005, 0.005, 0.002), slab, "button")
    return mesh


def make_render_models():
    base = os.path.join(DRIVER_RES, "rendermodels")
    for slug, m in list(MODELS.items()) + [("generic", GENERIC)]:
        name = "3dsvr_" + slug
        folder = os.path.join(base, name)
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, name + "_diffuse.png"), "wb") as f:
            f.write(palette_png(m))
        build_console_mesh(m).write(os.path.join(folder, name + ".obj"), name + ".mtl", name + "_diffuse.png")


if __name__ == "__main__":
    make_icon()
    make_banner()
    make_jingle()
    make_app_ico()
    make_status_icons()
    make_vrresources()
    make_render_models()
    print("assets written under", ROOT)
