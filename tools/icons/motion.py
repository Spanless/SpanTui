"""Per-icon motion specs (docs/ui/icons.md). Emitted by generate.py.

Model (mirrors IconMotionPlayer in src/application/icon_motion.cpp):
  icon   = parts; each an independently transformable path, optional parent.
           A part with no path is a pure transform group.
  layers = hover  (held while hovered, settles on leave; may loop)
           press  (one-shot per press, must end at rest)
           active (held while active, settles on deactivate; never loops —
                   the active pose is static)
  track  = part.prop keyframes [(t, value, ease)]; values are deltas from rest
           and layers add up. Keys owned by the active layer silence the hover
           layer while active.
  props: rotate tx ty scale sx sy opacity draw morph tint fill

Active pose language: semantic tint + one element that becomes solid.
"""
import math
import re

import icons as I
from geo import Path, cat
import glyphs as G

SPECS = {}


class Part:
    def __init__(self, pid, path, origin=(12, 12), parent=None, fill=False, tint=None,
                 rest=None, to=None, wrap=None, extra=False):
        self.id, self.path, self.origin, self.parent = pid, path, origin, parent
        self.fill, self.tint, self.rest, self.to, self.wrap = fill, tint, rest or {}, to, wrap
        self.extra = extra or path is None


def group(pid, origin=(12, 12), parent=None, wrap=None):
    return Part(pid, None, origin=origin, parent=parent, wrap=wrap)


def solid(pid, path, parent, tint, origin=(12, 12)):
    """Filled twin of a stroked part, hidden at rest."""
    return Part(pid, path, origin=origin, parent=parent, fill=True, tint=tint, rest={"fill": 0}, extra=True)


def T(part, prop, keys, dur, delay=0, loop=False):
    return {"part": part, "prop": prop, "keys": [list(k) for k in keys], "dur": dur,
            "delay": delay, "loop": loop}


def tint(*parts, dur=360, delay=0, step=0):
    return [T(p, "tint", [(0, 0), (1, 1, "o")], dur, delay + i * step) for i, p in enumerate(parts)]


def hold(part, prop, value, dur, delay=0, ease="o"):
    return T(part, prop, [(0, 0), (1, value, ease)], dur, delay)


def fill_in(part, amount=1.0, dur=420, delay=0):
    return T(part, "fill", [(0, 0), (1, amount, "o")], dur, delay)


def pop_in(part, dur=460, delay=0):
    """Solid twin grows from its origin."""
    return [T(part, "scale", [(0, -1), (1, 0, "back")], dur, delay), T(part, "fill", [(0, 0), (0.01, 1)], dur, delay)]


SKEL = re.compile(r"-?\d*\.?\d+")


def length(path):
    """Arc length of an open single-sub-path stroke (the dash trim needs it)."""
    assert sum(1 for k, _ in path.segs if k == "M") == 1, "draw tracks need a single sub-path"
    assert all(k != "Z" for k, _ in path.segs), "draw tracks need an open path"
    pts = path.samples()
    return sum(math.hypot(pts[j][0] - pts[j - 1][0], pts[j][1] - pts[j - 1][1]) for j in range(1, len(pts)))


def spec(name, parts, hover=(), press=(), active=(), note="", active_note=""):
    entry = I.ICONS.get(name) or I.ICONS[name + "-"]
    static = entry[1]()
    sx0, sy0, sx1, sy1 = static.bbox()
    body = cat(*[p.path for p in parts if not p.extra])
    ux0, uy0, ux1, uy1 = body.bbox()
    assert abs((sx1 - sx0) - (ux1 - ux0)) < 0.06 and abs((sy1 - sy0) - (uy1 - uy0)) < 0.06, \
        f"{name}: parts bbox {ux0:.2f},{uy0:.2f},{ux1:.2f},{uy1:.2f} vs {sx0:.2f},{sy0:.2f},{sx1:.2f},{sy1:.2f}"
    dx, dy = sx0 - ux0, sy0 - uy0
    ids = {p.id for p in parts}
    drawn = {t["part"] for layer in (hover, press, active) for t in layer if t["prop"] == "draw"}
    out, seen = [], set()
    for p in parts:
        moved = p.path.translate(dx, dy) if p.path is not None else None
        d = moved.d() if moved is not None else ""
        e = {"id": p.id, "d": d, "origin": [round(p.origin[0] + dx, 3), round(p.origin[1] + dy, 3)]}
        if not p.extra:
            assert p.fill == entry[2], f"{name}.{p.id}: visible parts share the icon's fill mode"
        if p.id in drawn:
            e["length"] = round(length(moved), 3)
        if p.parent:
            assert p.parent in seen, f"{name}.{p.id}: parent must precede the part"
            e["parent"] = p.parent
        seen.add(p.id)
        if p.extra: e["extra"] = True
        if p.fill: e["fill"] = True
        if p.tint: e["tint"] = p.tint
        if p.rest: e["rest"] = p.rest
        if p.wrap: e["wrap"] = p.wrap
        if p.to is not None:
            to = p.to.translate(dx, dy).d()
            assert SKEL.sub("#", d) == SKEL.sub("#", to), f"{name}.{p.id}: morph structure differs"
            e["to"] = to
        out.append(e)
    for layer in (hover, press, active):
        for t in layer:
            assert t["part"] in ids, f"{name}: track on unknown part {t['part']}"
    for t in active:
        assert not t["loop"], f"{name}: active layer must be static"
    for t in press:
        wrap = next((p.wrap for p in parts if p.id == t["part"]), None)
        end = t["keys"][-1][1]
        ok = end == 0 or (t["prop"] == "rotate" and wrap and abs(end / wrap - round(end / wrap)) < 1e-6)
        assert ok, f"{name}: press track {t['part']}.{t['prop']} must end at rest"
    SPECS[name] = {"folder": entry[0], "filled": entry[2], "parts": out, "hover": list(hover),
                   "press": list(press), "active": list(active), "note": note, "activeNote": active_note}


def wobble(amps, ease="io"):
    """Evenly spaced damped oscillation keys ending at rest."""
    n = len(amps) + 1
    return [(0, 0)] + [((i + 1) / n, a, ease) for i, a in enumerate(amps)] + [(1, 0, ease)]


def scaled(keys, k):
    return [(t[0], t[1] * k) + tuple(t[2:]) for t in keys]


BUMP = [(0, 0), (0.3, -0.16, "o"), (1, 0, "back")]           # press squash


def arc_wave(cx, cy, r, a0, a1):
    return Path().arc(cx, cy, r, r, a0, a1, move=True)


def bell():
    body = Path().M(6.5, 16.5).L(6.5, 11).arc(12, 11, 5.5, 5.5, 180, 360).L(17.5, 16.5)
    body.M(4.5, 16.5).L(19.5, 16.5).M(12, 3.5).L(12, 5.5)
    dome = Path().M(6.5, 16.5).L(6.5, 11).arc(12, 11, 5.5, 5.5, 180, 360).L(17.5, 16.5).Z()
    clap = Path().M(10.2, 19).arc(12, 19, 1.8, 1.5, 180, 0)
    wl = arc_wave(12, 10.5, 8.6, 232, 202)
    wr = arc_wave(12, 10.5, 8.6, 308, 338)
    ring = wobble([15, -11, 7, -4, 1.5])
    spec("bell", [Part("body", body, origin=(12, 3.5), tint="warning"),
                  solid("dome", dome, "body", "warning"),
                  Part("clapper", clap, origin=(12, 16.5), parent="body", tint="warning"),
                  Part("wl", wl, origin=(12, 10.5), rest={"opacity": 0}, extra=True, tint="warning"),
                  Part("wr", wr, origin=(12, 10.5), rest={"opacity": 0}, extra=True, tint="warning")],
         hover=[T("body", "rotate", ring, 1000),
                T("clapper", "tx", wobble([-1.8, 1.9, -1.3, 0.8, -0.3]), 1000, delay=70),
                *tint("body", "clapper", dur=250),
                *[T(w, "opacity", [(0, 0), (0.1, 1, "o"), (0.55, 1), (1, 0, "io")], 900, delay=60 + i * 60) for i, w in enumerate(("wl", "wr"))],
                *[T(w, "draw", [(0, -1), (0.45, 0, "o")], 900, delay=60 + i * 60) for i, w in enumerate(("wl", "wr"))],
                *[T(w, "scale", [(0, -0.2), (1, 0.12, "o")], 900, delay=60 + i * 60) for i, w in enumerate(("wl", "wr"))],
                *tint("wl", "wr", dur=1)],
         press=[T("body", "scale", BUMP, 260), T("clapper", "ty", [(0, 0), (0.3, 0.9, "o"), (1, 0, "back")], 300)],
         active=[T("body", "rotate", wobble([10, -6, 2.5]), 700),
                 T("clapper", "tx", wobble([-1.4, 1.2, -0.5]), 700, delay=60),
                 *tint("body", "clapper", "dome", "wl", "wr", dur=300),
                 T("dome", "fill", [(0, 0), (0.25, 0), (0.7, 1, "o")], 700),
                 *[T(w, "opacity", [(0, 0), (1, 1, "o")], 300, delay=250) for w in ("wl", "wr")],
                 *[T(w, "draw", [(0, -1), (1, 0, "o")], 420, delay=250) for w in ("wl", "wr")]],
         note="铃身摆动衰减、铃舌滞后反摆，两侧声波一圈圈荡开；变琥珀色",
         active_note="摇两下后停住：铃身实心琥珀色，两侧声波保留")


def gear_teeth(cx=12, cy=12, teeth=8, ro=9, ri=6.9, top=9.0, base=15.0):
    p = Path()
    pitch = 360 / teeth
    prev = None
    for i in range(teeth):
        c = -90 + i * pitch
        a_base0, a_top0, a_top1, a_base1 = c - base, c - top, c + top, c + base
        pb0 = (cx + ri * math.cos(math.radians(a_base0)), cy + ri * math.sin(math.radians(a_base0)))
        if i == 0:
            p.M(*pb0)
        else:
            p.arc(cx, cy, ri, ri, prev, a_base0)
        p.L(cx + ro * math.cos(math.radians(a_top0)), cy + ro * math.sin(math.radians(a_top0)))
        p.arc(cx, cy, ro, ro, a_top0, a_top1)
        p.L(cx + ri * math.cos(math.radians(a_base1)), cy + ri * math.sin(math.radians(a_base1)))
        prev = a_base1
    p.arc(cx, cy, ri, ri, prev, -90 - base + 360)
    return p.Z()


def settings():
    hole = Path().circle(12, 12, 3)
    spec("settings", [Part("teeth", gear_teeth(), wrap=45, tint="accent"),
                      Part("hole", hole, parent="teeth", tint="accent"),
                      solid("core", hole, "teeth", "accent")],
         hover=[T("teeth", "rotate", [(0, 0), (0.2, -12, "io"), (1, 90, "back")], 950),
                T("hole", "scale", [(0, 0), (0.2, 0.22, "o"), (0.6, -0.1, "io"), (1, 0, "io")], 950)],
         press=[T("teeth", "rotate", [(0, 0), (1, 45, "o")], 520), T("teeth", "scale", BUMP, 260)],
         active=[T("teeth", "rotate", [(0, 0), (0.2, -12, "io"), (1, 90, "back")], 900),
                 *tint("teeth", "hole", "core"), *pop_in("core", 480, delay=320)],
         note="先反拧蓄力，再转过两个齿距带回弹，轴心一胀",
         active_note="转两齿后停住：齿轮强调色，轴心变实心")


def sliders():
    rows = [(7, 15, 8.5), (12, 8.5, 15.5), (17, 13, 9.5)]
    parts, hov, act = [], [], []

    def track(y, kx):
        return Path().M(3.5, y).L(kx - 2.2, y).M(kx + 2.2, y).L(20.5, y)

    for i, (y, kx, kx2) in enumerate(rows):
        knob = Path().circle(kx, y, 2.2)
        parts += [Part(f"t{i}", track(y, kx), to=track(y, kx2)),
                  Part(f"k{i}", knob, origin=(kx, y), tint="accent"),
                  solid(f"f{i}", knob, f"k{i}", "accent", origin=(kx, y))]
        slide = [T(f"t{i}", "morph", [(0, 0), (1, 1, "back")], 560, delay=i * 70),
                 T(f"k{i}", "tx", [(0, 0), (1, kx2 - kx, "back")], 560, delay=i * 70)]
        hov += slide + [T(f"k{i}", "tint", [(0, 0), (1, 1, "o")], 400, delay=i * 70),
                        T(f"k{i}", "scale", [(0, 0), (0.5, 0.18, "o"), (1, 0, "io")], 560, delay=i * 70)]
        act += slide + tint(f"k{i}", f"f{i}", delay=i * 70) + pop_in(f"f{i}", 420, delay=300 + i * 70)
    spec("sliders", parts, hover=hov,
         press=[T(f"k{i}", "scale", [(0, 0), (0.35, 0.28, "o"), (1, 0, "io")], 320, delay=i * 40) for i in range(3)],
         active=act,
         note="三个旋钮错峰滑到新位置并微胀，轨道缺口跟着走",
         active_note="旋钮停在新位置，依次变成实心强调色")


def palette():
    p = Path().M(12, 20.5)
    p.arc(12, 12, 8.5, 8.5, 90, 360)
    p.C(20.5, 14.1, 19.1, 15.2, 17.3, 15.2).L(15.6, 15.2)
    p.C(14.1, 15.2, 13.4, 16.9, 14.4, 17.9).C(15.5, 19, 14.6, 20.5, 12, 20.5)
    dots = [((7.6, 12.2), "danger"), ((9.4, 7.9), "warning"), ((14.4, 7.6), "accent")]
    parts = [Part("board", p)]
    hov, act = [T("board", "rotate", wobble([-9, 4]), 700)], [T("board", "rotate", wobble([-7, 3]), 600)]
    for i, (q, col) in enumerate(dots):
        parts.append(Part(f"d{i}", Path().circle(*q, 0.7), origin=q, parent="board", tint=col))
        hov += [T(f"d{i}", "scale", [(0, 0), (0.4, 1.1, "o"), (1, 0.35, "io")], 520, delay=80 + i * 90),
                T(f"d{i}", "tint", [(0, 0), (0.4, 1, "o")], 520, delay=80 + i * 90)]
        act += [T(f"d{i}", "scale", [(0, 0), (0.4, 1.3, "o"), (1, 0.45, "io")], 520, delay=60 + i * 90),
                T(f"d{i}", "tint", [(0, 0), (0.4, 1, "o")], 520, delay=60 + i * 90)]
    spec("palette", parts, hover=hov, active=act,
         press=[T("board", "scale", BUMP, 260)],
         note="调色板轻晃，三个色点依次弹起，染成红、黄、蓝",
         active_note="色点保持彩色并略放大")


def layout():
    col0 = Path().M(9, 9.5).L(9, 19)
    col1 = Path().M(13, 8).L(13, 19)
    keys = [(0, 0), (0.3, 1, "o"), (0.62, 1), (1, 0, "io")]
    spec("layout", [Part("frame", I.frame(), origin=(12, 19)),
                    Part("head", Path().M(3, 9.5).L(21, 9.5), tint="accent"),
                    Part("col", col0, to=col1, tint="accent")],
         hover=[T("head", "ty", scaled(keys, -1.5), 950), T("col", "morph", keys, 950),
                T("frame", "sy", [(0, 0), (0.28, 0), (0.36, -0.04, "o"), (0.55, 0, "back")], 950),
                T("frame", "sx", [(0, 0), (0.28, 0), (0.36, 0.025, "o"), (0.55, 0, "back")], 950)],
         press=[T("frame", "scale", BUMP, 260)],
         active=[hold("head", "ty", -1.5, 520, ease="back"), hold("col", "morph", 1, 520, ease="back"),
                 *tint("head", "col")],
         note="页头上移、侧栏线右移，框体被“撞”得一颤，随后复位",
         active_note="停在新布局，内部分隔线着强调色")


def panel(name, x, sign):
    frame = I.frame()
    if sign < 0:
        region = Path().M(x, 5).L(6, 5).arc(6, 8, 3, 3, -90, -180).L(3, 16).arc(6, 16, 3, 3, 180, 90).L(x, 19).Z()
        edge = (3, 12)
    else:
        region = Path().M(x, 5).L(18, 5).arc(18, 8, 3, 3, -90, 0).L(21, 16).arc(18, 16, 3, 3, 0, 90).L(x, 19).Z()
        edge = (21, 12)
    spec(name, [Part("frame", frame), Part("region", region, origin=edge, fill=True, rest={"fill": 0}, tint="accent", extra=True),
                Part("div", Path().M(x, 5).L(x, 19), tint="accent")],
         hover=[T("div", "tx", [(0, 0), (0.35, sign * 2.6, "o"), (0.7, -sign * 0.5, "io"), (1, 0, "io")], 700),
                T("region", "fill", [(0, 0), (0.35, 0.22, "o"), (1, 0, "io")], 700),
                T("region", "sx", [(0, -1), (0.35, -0.6, "o"), (0.7, 0.1, "io"), (1, 0, "io")], 700)],
         press=[T("div", "tx", [(0, 0), (0.3, sign * 1.2, "o"), (1, 0, "back")], 320)],
         active=[T("div", "tx", [(0, 0), (0.3, sign * 2.6, "o"), (1, 0, "back")], 620),
                 T("region", "sx", [(0, -1), (0.3, -1), (1, 0, "back")], 620),
                 T("region", "fill", [(0, 0), (0.3, 0), (1, 0.35, "o")], 620),
                 *tint("div", "region", dur=300)],
         note="分栏线向边缘收起再弹回，面板区域随之闪现",
         active_note="分栏线弹开，面板区域浅色填充——面板已展开")


def sun():
    rays = Path()
    for i in range(8):
        a = math.radians(i * 45)
        rays.M(12 + 6.8 * math.cos(a), 12 + 6.8 * math.sin(a)).L(12 + 8.8 * math.cos(a), 12 + 8.8 * math.sin(a))
    core = Path().circle(12, 12, 4.2)
    spec("sun", [Part("core", core, tint="warning"), solid("disc", core, None, "warning"),
                 Part("rays", rays, wrap=45, tint="warning")],
         hover=[T("rays", "rotate", [(0, 0), (1, 45, "back")], 700),
                T("core", "scale", [(0, 0), (0.4, 0.14, "o"), (1, 0, "io")], 700),
                T("rays", "scale", [(0, 0), (0.3, -0.12, "o"), (0.7, 0.06, "io"), (1, 0, "io")], 700),
                *tint("core", "rays", dur=300)],
         press=[T("rays", "rotate", [(0, 0), (1, 90, "o")], 600), T("core", "scale", BUMP, 260)],
         active=[T("rays", "rotate", [(0, 0), (1, 45, "back")], 700),
                 T("rays", "scale", [(0, 0), (0.3, -0.16, "o"), (1, 0.07, "back")], 700),
                 T("core", "scale", [(0, 0), (0.3, -0.12, "o"), (1, 0, "back")], 700),
                 T("disc", "scale", [(0, -0.12), (0.3, -0.12, "o"), (1, 0, "back")], 700),
                 fill_in("disc", 1, 400, delay=200), *tint("core", "rays", "disc", dur=300)],
         note="光芒转过 45° 并收放，日核一胀；染成暖黄",
         active_note="日核变实心，光芒略微外放")


def moon():
    c1, r1 = (11.5, 12.5), 8.4
    c2, r2 = (16.8, 7.6), 6.4
    pa, pb = I.circle_intersections(c1, r1, c2, r2)
    in2 = lambda q: (q[0] - c2[0]) ** 2 + (q[1] - c2[1]) ** 2 < r2 * r2
    in1 = lambda q: (q[0] - c1[0]) ** 2 + (q[1] - c1[1]) ** 2 < r1 * r1
    body = Path().M(*pa)
    I.arc_pick(body, c1, r1, pa, pb, lambda q: not in2(q))
    I.arc_pick(body, c2, r2, pb, pa, in1)
    body.Z()
    x0, y0, x1, y1 = body.bbox()
    mc = ((x0 + x1) / 2, (y0 + y1) / 2)
    spark = Path().M(17.8, 5.2).L(17.8, 8.4).M(16.2, 6.8).L(19.4, 6.8)
    dot = Path().dot(14.6, 5)
    tw = [(0, 0), (0.35, 0), (0.7, 1, "o"), (1, 1)]
    spec("moon", [Part("moon", body, origin=mc, tint="accent"), solid("face", body, "moon", "accent", origin=mc),
                  Part("spark", spark, origin=(17.8, 6.8), rest={"opacity": 0}, extra=True, tint="accent"),
                  Part("dot", dot, origin=(14.6, 5), rest={"opacity": 0}, extra=True, tint="accent")],
         hover=[T("moon", "rotate", [(0, 0), (0.35, -20, "o"), (0.7, 5, "io"), (1, 0, "io")], 850),
                *tint("moon", dur=420), *tint("spark", "dot", dur=1),
                T("spark", "opacity", tw, 850), T("spark", "scale", [(0, -1), (0.35, -1), (0.7, 0.25, "o"), (1, 0, "io")], 850),
                T("spark", "rotate", [(0, 0), (1, 90, "o")], 1100),
                T("dot", "opacity", tw, 850, delay=180)],
         press=[T("moon", "scale", BUMP, 260)],
         active=[T("moon", "rotate", [(0, 0), (0.4, -20, "o"), (1, 0, "back")], 800),
                 *tint("moon", "face", "spark", "dot", dur=300), fill_in("face", 1, 420, delay=260),
                 T("spark", "opacity", tw, 800), T("spark", "scale", [(0, -1), (0.35, -1), (1, 0, "back")], 800),
                 T("dot", "opacity", tw, 800, delay=150)],
         note="月牙摇一下，缺口处亮起一颗星和一个小点；染月光蓝",
         active_note="月牙变实心，星星常亮")


def refresh():
    r = 8
    a1 = Path().arc(12, 12, r, r, 322, 160, move=True)
    e1 = (12 + r * math.cos(math.radians(322)), 12 + r * math.sin(math.radians(322)))
    t1 = (-math.sin(math.radians(322)), math.cos(math.radians(322)))
    a2 = Path().arc(12, 12, r, r, 502, 340, move=True)
    e2 = (12 + r * math.cos(math.radians(502)), 12 + r * math.sin(math.radians(502)))
    t2 = (-math.sin(math.radians(502)), math.cos(math.radians(502)))
    arcs = ("a1", "a2")
    spec("refresh", [group("spin", wrap=180),
                     Part("a1", a1, parent="spin", tint="accent"), Part("h1", I.arrowhead(e1, t1, 3.4), parent="spin", tint="accent"),
                     Part("a2", a2, parent="spin", tint="accent"), Part("h2", I.arrowhead(e2, t2, 3.4), parent="spin", tint="accent")],
         hover=[T("spin", "rotate", [(0, 0), (0.18, -20, "io"), (1, 180, "back")], 950),
                *[T(a, "draw", [(0, 0), (0.2, 0), (0.55, -0.45, "io"), (1, 0, "io")], 950) for a in arcs]],
         press=[T("spin", "rotate", [(0, 0), (1, 180, "o")], 560),
                *[T(a, "draw", [(0, 0), (0.4, -0.35, "o"), (1, 0, "io")], 560) for a in arcs]],
         active=[T("spin", "rotate", [(0, 0), (0.18, -20, "io"), (1, 180, "back")], 900),
                 *[T(a, "draw", [(0, 0), (0.55, -0.45, "io"), (1, -0.22, "io")], 900) for a in arcs],
                 *tint("a1", "h1", "a2", "h2")],
         note="先反拧再转半圈，高速时弧线像彗尾一样收短，停下再伸展",
         active_note="弧线收短停住，着强调色——处于刷新中的样子")


def history():
    ring = Path().arc(12, 12, 8.5, 8.5, 180, 180 - 315, move=True)
    tip = ring.cur()
    a = math.radians(180 - 315)
    ring = cat(ring, I.arrowhead(tip, (math.sin(a), -math.cos(a)), 3.4))
    spec("history", [Part("ring", ring, tint="accent"),
                     Part("min", Path().M(12, 7.5).L(12, 12), wrap=360, tint="accent"),
                     Part("hour", Path().M(12, 12).L(15, 14), tint="accent")],
         hover=[T("ring", "rotate", [(0, 0), (0.4, -28, "o"), (1, 0, "io")], 900),
                T("min", "rotate", [(0, 0), (1, -360, "io")], 1000),
                T("hour", "rotate", [(0, 0), (1, -30, "io")], 1000)],
         press=[T("ring", "scale", BUMP, 260)],
         active=[T("ring", "rotate", [(0, 0), (0.4, -28, "o"), (1, 0, "io")], 900),
                 T("min", "rotate", [(0, 0), (1, -450, "io")], 1000),
                 T("hour", "rotate", [(0, 0), (1, -75, "io")], 1000),
                 *tint("ring", "min", "hour")],
         note="外圈往回拧，分针倒转一圈、时针退一格——时光倒流",
         active_note="指针停在倒拨后的时刻，着强调色")


def trash():
    lid = Path().M(3.5, 6.5).L(20.5, 6.5)
    lid.M(9, 6.5).L(9, 5).arc(10.5, 5, 1.5, 1.5, 180, 270).L(13.5, 3.5).arc(13.5, 5, 1.5, 1.5, -90, 0).L(15, 6.5)
    body = Path().M(5.5, 6.5).L(6.35, 18.6)
    body.arc(8.85, 18.43, 2.5, 2.5, 176, 90).L(15.15, 20.93)
    body.arc(15.15, 18.43, 2.5, 2.5, 90, 4).L(18.5, 6.5)
    lines = Path().M(10, 10.5).L(10, 16.5).M(14, 10.5).L(14, 16.5)
    lift = [T("lid", "rotate", [(0, 0), (0.18, 1.5, "io"), (0.6, -9, "o"), (1, -7, "io")], 560),
            T("lid", "ty", [(0, 0), (0.18, 0.3, "io"), (0.6, -1.5, "o"), (1, -1.1, "io")], 560)]
    spec("trash", [Part("body", body, origin=(12, 21), tint="danger"),
                   Part("lines", lines, parent="body", origin=(12, 13.5), tint="danger"),
                   Part("lid", lid, origin=(3.5, 6.5), tint="danger")],
         hover=lift + tint("lid", "body", "lines", dur=280) +
               [T("lines", "sy", [(0, 0), (0.5, 0), (0.7, -0.12, "o"), (1, 0, "back")], 700)],
         press=[T("lid", "rotate", [(0, 0), (0.28, 7, "i"), (1, 0, "o")], 380),
                T("lid", "ty", [(0, 0), (0.28, 1.1, "i"), (1, 0, "o")], 380),
                T("body", "scale", [(0, 0), (0.28, 0), (0.45, -0.07, "o"), (1, 0, "back")], 380)],
         active=lift + tint("lid", "body", "lines", dur=280),
         note="先微压再掀盖，桶身竖纹一缩；变危险红。按下盖子砰地合上",
         active_note="盖子保持掀开，整体危险红")


def copy():
    front = Path().rrect(8.5, 8.5, 12.5, 12.5, 2.5)
    back = Path().M(5.5, 15.5).arc(5.5, 13, 2.5, 2.5, 90, 180).L(3, 5.5)
    back.arc(5.5, 5.5, 2.5, 2.5, 180, 270).L(13, 3).arc(13, 5.5, 2.5, 2.5, -90, 0)
    apart = [hold("back", "tx", -0.8, 320), hold("back", "ty", -0.8, 320),
             hold("front", "tx", 0.7, 320), hold("front", "ty", 0.7, 320)]
    spec("copy", [Part("back", back), Part("front", front, origin=(14.75, 14.75), tint="success"),
                  solid("sheet", front, "front", "success", origin=(14.75, 14.75))],
         hover=apart,
         press=[T("front", "tx", [(0, 0), (0.35, -2.6, "io"), (1, 0, "back")], 520),
                T("front", "ty", [(0, 0), (0.35, -2.6, "io"), (1, 0, "back")], 520),
                T("front", "tint", [(0, 0), (0.3, 1, "o"), (0.7, 1), (1, 0, "io")], 900),
                T("sheet", "fill", [(0, 0), (0.3, 0.35, "o"), (0.7, 0.35), (1, 0, "io")], 900),
                T("sheet", "tint", [(0, 0), (0.01, 1), (0.99, 1), (1, 0)], 900)],
         active=[T("front", "tx", [(0, 0), (0.35, -2.6, "io"), (1, 0.7, "back")], 560),
                 T("front", "ty", [(0, 0), (0.35, -2.6, "io"), (1, 0.7, "back")], 560),
                 hold("back", "tx", -0.8, 320), hold("back", "ty", -0.8, 320),
                 *tint("front", "sheet"), fill_in("sheet", 0.3, 400, delay=200)],
         note="两页纸微微错开；按下时前页压到后页上“印”一份，闪一下绿",
         active_note="印完停在错开位置，前页淡绿填充")


def paste():
    board = Path().M(8.5, 4.75).L(8, 4.75).arc(8, 7.75, 3, 3, -90, -180).L(5, 18)
    board.arc(8, 18, 3, 3, 180, 90).L(16, 21).arc(16, 18, 3, 3, 90, 0).L(19, 7.75)
    board.arc(16, 7.75, 3, 3, 0, -90).L(15.5, 4.75)
    lines = [Path().M(8.5, 11).L(15.5, 11), Path().M(8.5, 14).L(15.5, 14), Path().M(8.5, 17).L(12.5, 17)]
    parts = [Part("board", board, origin=(12, 21)), Part("clip", Path().rrect(8.5, 3, 7, 3.5, 1.25), tint="accent")]
    parts += [Part(f"l{i}", l, parent="board", rest={"opacity": 0}, extra=True, tint="accent") for i, l in enumerate(lines)]
    write = []
    for i in range(3):
        write += [T(f"l{i}", "opacity", [(0, 0), (0.01, 1)], 300, delay=380 + i * 110),
                  T(f"l{i}", "draw", [(0, -1), (1, 0, "o")], 300, delay=380 + i * 110)]
    spec("paste", parts,
         hover=[T("clip", "ty", [(0, 0), (0.3, -1.5, "o"), (0.55, 0, "i"), (0.72, -0.45, "o"), (0.88, 0, "i")], 700),
                T("board", "scale", [(0, 0), (0.55, 0), (0.62, -0.05, "o"), (1, 0, "back")], 700)] + write,
         press=[T("board", "scale", BUMP, 260)],
         active=[T("clip", "ty", [(0, 0), (0.3, -1.5, "o"), (0.55, 0, "i"), (0.72, -0.45, "o"), (0.88, 0, "i")], 700)]
                + write + tint("clip", "l0", "l1", "l2", step=110),
         note="夹子弹两下，板子一震，三行内容依次写进板里",
         active_note="内容行保留并着强调色")


def search():
    c, r = (10.5, 10.5), 6.5
    k = r * 0.7071
    lens = Path().circle(*c, r)
    glint = arc_wave(c[0], c[1], 3.9, 195, 250)
    scan_x = [(0, 0), (0.25, -1, "io"), (0.5, 0.3, "io"), (0.75, 0.9, "io"), (1, 0, "io")]
    scan_y = [(0, 0), (0.25, -0.5, "io"), (0.5, -1.2, "io"), (0.75, -0.3, "io"), (1, 0, "io")]
    spec("search", [group("g", origin=(20, 20)),
                    Part("lens", lens, parent="g", origin=c, tint="accent"),
                    solid("glass", lens, "g", "accent", origin=c),
                    Part("handle", Path().M(c[0] + k, c[1] + k).L(20, 20), parent="g", tint="accent"),
                    Part("glint", glint, parent="g", rest={"opacity": 0}, extra=True, tint="accent")],
         hover=[T("g", "tx", scan_x, 1000), T("g", "ty", scan_y, 1000),
                T("glint", "opacity", [(0, 0), (0.3, 0), (0.31, 1)], 1000),
                T("glint", "draw", [(0, -1), (0.3, -1), (0.7, 0, "o")], 1000)],
         press=[T("g", "scale", BUMP, 260)],
         active=[T("g", "rotate", [(0, 0), (0.4, -14, "o"), (1, -8, "io")], 600),
                 T("glint", "opacity", [(0, 0), (0.01, 1)], 500, delay=200),
                 T("glint", "draw", [(0, -1), (1, 0, "o")], 500, delay=200),
                 *tint("lens", "handle", "glint", "glass"), fill_in("glass", 0.16, 400)],
         note="放大镜沿小圆圈巡视，镜片上划过一道高光",
         active_note="微微倾斜停住，镜片淡色填充带高光")


def x_():
    # Close: a calm settle, no spin or redraw — shrinks a little and turns red
    # while hovered (the hover layer holds its end values).
    parts = [group("x"),
             Part("d1", Path().M(6, 6).L(18, 18), parent="x", tint="danger"),
             Part("d2", Path().M(18, 6).L(6, 18), parent="x", tint="danger")]
    spec("x", parts,
         hover=[hold("x", "scale", -0.12, 240), *tint("d1", "d2", dur=240)],
         press=[T("x", "scale", BUMP, 300)],
         active=[hold("x", "scale", -0.12, 240), *tint("d1", "d2", dur=240)],
         note="叉号平滑收缩到 88% 并转危险红，悬浮期间保持；按下轻压回弹",
         active_note="收缩并着危险红")


def plus_():
    arms = [(12, 5), (19, 12), (12, 19), (5, 12)]
    parts = [group("p", wrap=90)] + [Part(f"h{i}", Path().M(12, 12).L(*a), parent="p", tint="accent") for i, a in enumerate(arms)]
    ids = [f"h{i}" for i in range(4)]
    spec("plus", parts,
         hover=[T("p", "rotate", [(0, 0), (1, 90, "back")], 480), T("p", "scale", [(0, 0), (0.45, 0.12, "o"), (1, 0, "io")], 480)],
         press=[T(h, "draw", [(0, 0), (0.3, -0.45, "o"), (1, 0, "back")], 380, delay=i * 20) for i, h in enumerate(ids)],
         active=[T("p", "rotate", [(0, 0), (1, 90, "back")], 520), hold("p", "scale", 0.06, 520, ease="back"), *tint(*ids)],
         note="加号转 90° 并一胀；按下四臂依次缩回再弹出",
         active_note="转完略放大并着强调色")


def file_plus():
    spec("file-plus", [Part("page", I.page(), origin=(12, 21), tint="success"),
                       Part("p", I.plus(12, 14.5, 5.5), origin=(12, 14.5), parent="page", wrap=90, tint="success")],
         hover=[T("page", "ty", [(0, 0), (0.4, -0.9, "o"), (1, 0, "io")], 560),
                T("p", "rotate", [(0, 0), (1, 90, "back")], 520), T("p", "scale", [(0, 0), (0.45, 0.25, "o"), (1, 0, "io")], 520),
                *tint("p", dur=260)],
         press=[T("page", "scale", BUMP, 280)],
         active=[T("page", "ty", [(0, 0), (0.4, -0.9, "o"), (1, 0, "io")], 560),
                 T("p", "rotate", [(0, 0), (1, 90, "back")], 520), hold("p", "scale", 0.15, 520, ease="back"),
                 *tint("p", "page")],
         note="纸页一抬，中间加号转 90° 变绿",
         active_note="加号放大，整体绿色")


def folder_plus():
    spec("folder-plus", [Part("folder", I.folder_shape(), origin=(12, 19.5), tint="success"),
                         Part("p", I.plus(12, 12.75, 5.5), origin=(12, 12.75), parent="folder", wrap=90, tint="success")],
         hover=[T("folder", "scale", [(0, 0), (0.35, 0.04, "o"), (1, 0, "io")], 560),
                T("p", "rotate", [(0, 0), (1, 90, "back")], 520), T("p", "scale", [(0, 0), (0.45, 0.25, "o"), (1, 0, "io")], 520),
                *tint("p", dur=260)],
         press=[T("folder", "scale", BUMP, 280)],
         active=[T("folder", "scale", [(0, 0), (0.35, 0.04, "o"), (1, 0, "io")], 560),
                 T("p", "rotate", [(0, 0), (1, 90, "back")], 520), hold("p", "scale", 0.15, 520, ease="back"),
                 *tint("p", "folder")],
         note="文件夹一胀，加号转 90° 变绿",
         active_note="加号放大，整体绿色")


def external_link():
    box = Path().M(18, 13.5).L(18, 18).arc(15, 18, 3, 3, 0, 90).L(6, 21)
    box.arc(6, 18, 3, 3, 90, 180).L(3, 9).arc(6, 9, 3, 3, 180, 270).L(10.5, 6)
    arrow = Path().M(14, 3).L(21, 3).L(21, 10).M(21, 3).L(11.5, 12.5)

    def fly(end):
        return [(0, 0), (0.38, 3.2, "i"), (0.38, -3.2), (1, end, "back" if end else "o")]
    fade = [(0, 0), (0.38, -1, "i"), (0.38, -1), (0.6, 0, "o")]
    spec("external-link", [Part("box", box, origin=(10.5, 13.5)), Part("arrow", arrow, tint="accent")],
         hover=[T("arrow", "tx", fly(0), 640), T("arrow", "ty", scaled(fly(0), -1), 640),
                T("arrow", "opacity", fade, 640), T("arrow", "tint", [(0, 0), (0.6, 1, "o")], 640),
                T("box", "scale", [(0, 0), (0.38, 0), (0.5, -0.05, "o"), (1, 0, "back")], 640)],
         press=[T("box", "scale", BUMP, 260)],
         active=[T("arrow", "tx", fly(0.7), 700), T("arrow", "ty", scaled(fly(0.7), -1), 700),
                 T("arrow", "opacity", fade, 700), *tint("arrow", "box")],
         note="箭头从右上飞出、从左下飞回，落地时框体一颤",
         active_note="箭头停在略微探出框外的位置")


def undo():
    def hook(x0):
        p = Path().M(x0, 9).L(14.5, 9).arc(14.5, 14.25, 5.25, 5.25, -90, 90).L(10.5, 19.5)
        return p
    head = Path().M(8.5, 13.5).L(4, 9).L(8.5, 4.5)
    back = [(0, 0), (0.3, 1, "o"), (0.65, -0.19, "io"), (1, 0, "io")]
    spec("undo", [group("u", origin=(14.5, 14.25)),
                  Part("hook", hook(4), parent="u", to=hook(2.4), tint="accent"),
                  Part("head", head, parent="u", tint="accent")],
         hover=[T("u", "rotate", [(0, 0), (0.3, -10, "o"), (0.65, 3, "io"), (1, 0, "io")], 760),
                T("head", "tx", scaled(back, -1.6), 760), T("hook", "morph", back, 760)],
         press=[T("head", "tx", [(0, 0), (0.3, -1.2, "o"), (1, 0, "back")], 360),
                T("hook", "morph", [(0, 0), (0.3, 0.75, "o"), (1, 0, "back")], 360)],
         active=[T("u", "rotate", [(0, 0), (0.4, -20, "o"), (1, -8, "io")], 700),
                 T("head", "tx", scaled(back, -1.6), 760), T("hook", "morph", back, 760), *tint("hook", "head")],
         note="箭头往回一扯，线段随之拉长，整体逆时针回拨",
         active_note="逆时针偏转停住，着强调色")


def pencil():
    line = Path().M(4, 21.6).L(7.2, 21.6)
    write_tx = [(0, 0), (0.55, 3.2, "io"), (1, 0, "io")]
    write_ty = [(0, 0), (0.55, 0), (0.75, -1.2, "o"), (1, 0, "io")]
    write_r = [(0, 0), (0.12, -5, "io"), (0.25, 3, "io"), (0.38, -4, "io"), (0.55, 0, "io"), (0.75, -6, "o"), (1, 0, "io")]
    ink = [T("line", "opacity", [(0, 0), (0.05, 1)], 1100), T("line", "draw", [(0, -1), (0.05, -1), (0.55, 0, "io")], 1100)]
    spec("pencil", [Part("p", I.pencil(), origin=(4, 20), tint="accent"),
                    Part("line", line, rest={"opacity": 0}, extra=True, tint="accent")],
         hover=[T("p", "tx", write_tx, 1100), T("p", "ty", write_ty, 1100), T("p", "rotate", write_r, 1100)] + ink,
         press=[T("p", "scale", BUMP, 280)],
         active=[T("p", "tx", write_tx, 1100), T("p", "ty", write_ty, 1100), T("p", "rotate", write_r, 1100)] + ink
                + tint("p", "line"),
         note="笔尖抖动着向右写出一道线，写完抬笔回位，线留在纸上",
         active_note="留下的笔迹与笔身着强调色")


def eraser():
    f = lambda u, v: (12 + (u + v) * 0.7071, 11 + (-u + v) * 0.7071)
    body = Path().rpoly([f(-8.5, -3.8), f(5.5, -3.8), f(5.5, 3.8), f(-8.5, 3.8)], 2.2)
    body.add(Path().M(*f(-3, -3.8)).L(*f(-3, 3.8)))
    rub_x = [(0, 0), (0.2, -1.2, "io"), (0.4, 1.0, "io"), (0.6, -1.0, "io"), (0.8, 0.7, "io"), (1, 0, "io")]
    rub_r = [(0, 0), (0.2, -3, "io"), (0.4, 3, "io"), (0.6, -3, "io"), (0.8, 2, "io"), (1, 0, "io")]
    spec("eraser", [Part("e", body, origin=(12, 11), tint="accent"), Part("g", Path().M(20, 20.5).L(12, 20.5))],
         hover=[T("e", "tx", rub_x, 900), T("e", "rotate", rub_r, 900), T("g", "draw", [(0, 0), (1, -0.55, "io")], 900)],
         press=[T("e", "scale", BUMP, 280)],
         active=[T("e", "tx", rub_x, 900), T("e", "rotate", rub_r, 900), T("g", "draw", [(0, 0), (1, -0.55, "io")], 900),
                 *tint("e")],
         note="橡皮左右擦，底线被一点点擦短",
         active_note="底线保持擦短，橡皮着强调色")


def play():
    spec("play", [Part("tri", I.triangle_play(), fill=True, tint="success")],
         hover=[hold("tri", "tx", 0.9, 380, ease="back"), *tint("tri", dur=380)],
         press=[T("tri", "scale", [(0, 0), (0.3, -0.2, "o"), (1, 0, "back")], 340)],
         active=[T("tri", "scale", [(0, 0), (0.3, -0.2, "o"), (1, 0.08, "back")], 520),
                 hold("tri", "tx", 0.6, 520, ease="back"), *tint("tri")],
         note="三角向前一探并变绿，按下缩一下弹回",
         active_note="绿色三角略放大、前移")


def update():
    circ = Path().circle(12, 12, 8.5)
    arrow = cat(Path().M(12, 7.5).L(12, 16), I.arrowhead((12, 16), (0, 1), 3.5))
    drop = [(0, 0), (0.4, 4.5, "i"), (0.4, -4.5), (1, 0, "o")]
    fade = [(0, 0), (0.36, -1, "i"), (0.44, -1), (1, 0, "o")]
    land = [(0, 0), (0.82, 0), (0.9, -0.05, "o"), (1, 0, "io")]
    spec("update", [Part("circ", circ, tint="accent"), solid("disc", circ, None, "accent"), Part("arrow", arrow, tint="accent")],
         hover=[T("arrow", "ty", drop, 720), T("arrow", "opacity", fade, 720), T("circ", "scale", land, 720),
                T("arrow", "tint", [(0, 0), (0.6, 1, "o")], 720)],
         press=[T("circ", "scale", BUMP, 260)],
         active=[T("arrow", "ty", drop, 720), T("arrow", "opacity", fade, 720), T("circ", "scale", land, 720),
                 *tint("arrow", "circ", "disc"), fill_in("disc", 0.16, 400, delay=500)],
         note="箭头往下落出，再从上方落回，落地时圆圈一震",
         active_note="圆内淡色填充，着强调色")


def arrow_down():
    drop = [(0, 0), (0.4, 5, "i"), (0.4, -5), (1, 0, "o")]
    fade = [(0, 0), (0.36, -1, "i"), (0.44, -1), (1, 0, "o")]
    spec("arrow-down", [Part("a", I.ICONS["arrow-down"][1](), origin=(12, 19.5), tint="accent")],
         hover=[T("a", "ty", drop, 700), T("a", "opacity", fade, 700),
                T("a", "sy", [(0, 0), (0.85, 0), (0.92, -0.1, "o"), (1, 0, "io")], 700)],
         press=[T("a", "ty", [(0, 0), (0.3, 1.5, "o"), (1, 0, "back")], 300)],
         active=[T("a", "ty", [(0, 0), (0.4, 5, "i"), (0.4, -5), (1, 0.8, "back")], 700), T("a", "opacity", fade, 700), *tint("a")],
         note="箭头下落穿出再从上方回来，落地时一压",
         active_note="停在略偏下的位置，着强调色")


def chevrons():
    up = [(0, 0), (0.3, -1.4, "o"), (0.55, 0.3, "io"), (1, 0, "io")]
    spec("chevrons-up-down", [Part("up", Path().M(8, 9.5).L(12, 5.5).L(16, 9.5), tint="accent"),
                              Part("down", Path().M(8, 14.5).L(12, 18.5).L(16, 14.5), tint="accent")],
         hover=[T("up", "ty", up, 620), T("down", "ty", scaled(up, -1), 620, delay=90)],
         press=[T("up", "ty", [(0, 0), (0.3, 1.2, "o"), (1, 0, "back")], 320),
                T("down", "ty", [(0, 0), (0.3, -1.2, "o"), (1, 0, "back")], 320)],
         active=[hold("up", "ty", -0.9, 460, ease="back"), hold("down", "ty", 0.9, 460, delay=70, ease="back"), *tint("up", "down")],
         note="上箭头先弹、下箭头紧跟，张开再合拢",
         active_note="上下箭头保持张开")


def files():
    front = I.page(x=8, y=6, w=12.5, h=15, fold=4)
    back = Path().M(5, 17.5).L(5, 6).arc(8, 6, 3, 3, 180, 270).L(14.5, 3)
    fan = [hold("back", "rotate", -7, 420, ease="back"), hold("front", "rotate", 3, 420, ease="back"),
           T("front", "ty", [(0, 0), (0.4, -0.9, "o"), (1, -0.4, "io")], 420)]
    spec("files", [Part("back", back, origin=(5, 17.5), tint="accent"), Part("front", front, origin=(14, 21), tint="accent")],
         hover=fan, press=[T("front", "scale", BUMP, 260)], active=fan + tint("front", "back"),
         note="两页像扇子一样展开，前页一抬",
         active_note="保持展开，着强调色")


def code():
    spec("code", [Part("l", Path().M(8, 7.5).L(3.5, 12).L(8, 16.5)),
                  Part("r", Path().M(16, 7.5).L(20.5, 12).L(16, 16.5)),
                  Part("s", Path().M(13.6, 5).L(10.4, 19), tint="accent")],
         hover=[hold("l", "tx", -1.2, 420, ease="back"), hold("r", "tx", 1.2, 420, delay=70, ease="back"),
                T("s", "sx", [(0, 0), (0.5, -2, "io"), (1, 0, "io")], 620), *tint("s", dur=300)],
         press=[T("s", "scale", BUMP, 260)],
         active=[hold("l", "tx", -1.2, 420, ease="back"), hold("r", "tx", 1.2, 420, delay=70, ease="back"),
                 T("s", "sx", [(0, 0), (0.5, -2, "io"), (1, 0, "io")], 620), *tint("s")],
         note="尖括号依次向两侧撑开，斜杠翻转一下并着色",
         active_note="括号保持撑开，斜杠强调色")


def terminal():
    blink = [(0, 0), (0.25, 0), (0.25, -1), (0.5, -1), (0.5, 0), (1, 0)]
    typing = [(0, -0.75), (0.35, -0.75), (0.35, -0.5), (0.6, -0.5), (0.6, -0.25), (0.85, -0.25), (0.85, 0), (1, 0)]
    spec("terminal", [Part("frame", I.frame()),
                      Part("prompt", Path().M(7.5, 9.5).L(10, 12).L(7.5, 14.5), tint="success"),
                      Part("cursor", Path().M(12.5, 14.5).L(16.5, 14.5), origin=(12.5, 14.5), tint="cursor")],
         hover=[T("prompt", "tx", [(0, 0), (0.3, 1.2, "o"), (1, 0, "back")], 520),
                *tint("prompt", dur=160), *tint("cursor", dur=300),
                T("cursor", "opacity", blink, 1060, delay=300, loop=True)],
         press=[T("cursor", "draw", typing, 420)],
         active=[T("prompt", "tx", [(0, 0), (0.3, 1.2, "o"), (1, 0.5, "back")], 520),
                 T("cursor", "draw", typing, 520, delay=120), *tint("prompt", "cursor")],
         note="提示符向前一顿变绿，光标变红并持续闪烁；按下光标像在打字",
         active_note="打出一行后停住：提示符绿、光标红，不闪烁")


def keyboard():
    parts = [Part("frame", I.frame())]
    keys = []
    for r, y in enumerate(I.KEY_ROWS):
        for c, x in enumerate(I.KEY_XS):
            kid = f"k{r}{c}"
            keys.append(kid)
            parts.append(Part(kid, Path().M(x, y).L(x + 1, y), origin=(x + 0.5, y), tint="accent"))
    a, b = I.SPACE_BAR
    parts.append(Part("space", Path().M(*a).L(*b), tint="accent"))
    order = ["k00", "k11", "k02", "k10", "k01", "k12"]
    tap = [(0, 0), (0.3, 0.8, "o"), (1, 0, "back")]
    spec("keyboard", parts,
         hover=[*[T(k, "ty", tap, 360, delay=i * 90) for i, k in enumerate(order)],
                *[T(k, "tint", [(0, 0), (0.3, 1, "o"), (1, 0, "io")], 360, delay=i * 90) for i, k in enumerate(order)],
                T("space", "ty", tap, 360, delay=len(order) * 90)],
         press=[T("space", "scale", BUMP, 260)],
         active=[*tint(*keys, dur=300, step=40), *tint("space", delay=240)],
         note="按键依次被敲下并闪一下强调色，最后按下空格键",
         active_note="所有按键与空格键保持强调色")


def cpu():
    die = Path().rrect(9.5, 9.5, 5, 5, 1)
    parts = [Part("body", Path().rrect(6, 6, 12, 12, 2.5)), Part("die", die, tint="accent"), solid("core", die, "die", "accent")]
    pins = [((10, 3), (10, 6)), ((14, 3), (14, 6)), ((21, 10), (18, 10)), ((21, 14), (18, 14)),
            ((14, 21), (14, 18)), ((10, 21), (10, 18)), ((3, 14), (6, 14)), ((3, 10), (6, 10))]
    hov = [T("die", "scale", [(0, 0), (0.5, 0.14, "io"), (1, 0, "io")], 1200, loop=True), *tint("die", dur=400)]
    act = [T("die", "scale", [(0, 0), (0.4, 0.2, "o"), (1, 0, "back")], 600), *tint("die", "core"), fill_in("core", 1, 400, delay=150)]
    for i, (a, b) in enumerate(pins):
        parts.append(Part(f"p{i}", Path().M(*a).L(*b), tint="accent"))
        hov.append(T(f"p{i}", "tint", [(0, 0), (0.12, 1, "o"), (0.45, 0, "io"), (1, 0)], 1200, delay=i * 150, loop=True))
        act.append(T(f"p{i}", "tint", [(0, 0), (1, 1, "o")], 240, delay=i * 50))
    spec("cpu", parts, hover=hov, press=[T("body", "scale", BUMP, 260)], active=act,
         note="芯片呼吸，引脚顺时针依次点亮（悬浮期间循环）",
         active_note="引脚一圈点亮后常亮，芯片核心变实心")


def git_branch():
    r = 2.3
    curve = Path().M(8.8, 17.5).C(13.5, 17.5, 17.5, 13, 17.5, 8.8)
    node = Path().circle(17.5, 6.5, r)
    grow = [T("a", "scale", [(0, 0), (0.15, 0.25, "o"), (0.45, 0, "io")], 900),
            T("curve", "draw", [(0, -1), (0.12, -1), (0.6, 0, "io")], 900),
            T("b", "scale", [(0, -1), (0.55, -1), (0.8, 0.3, "o"), (1, 0, "io")], 900)]
    spec("git-branch", [Part("trunk", Path().M(6.5, 3.5).L(6.5, 15.2)),
                        Part("a", Path().circle(6.5, 17.5, r), origin=(6.5, 17.5)),
                        Part("curve", curve, tint="success"),
                        Part("b", node, origin=(17.5, 6.5), tint="success"),
                        solid("bf", node, "b", "success", origin=(17.5, 6.5))],
         hover=grow + [T("b", "tint", [(0, 0), (0.6, 0), (0.85, 1, "o")], 900)],
         press=[T("b", "scale", BUMP, 280)],
         active=grow + tint("curve", "b", "bf", delay=500) + [fill_in("bf", 1, 300, delay=700)],
         note="从主干重新长出分支曲线，末端节点弹出并变绿",
         active_note="分支绿色，末端节点实心")


def git_commit():
    ring = Path().circle(12, 12, 3.5)
    grow = [T("ring", "scale", [(0, 0), (0.45, 0.32, "back"), (1, 0.16, "io")], 520),
            T("l", "draw", [(0, 0), (0.45, -0.2, "o"), (1, -0.1, "io")], 520),
            T("r", "draw", [(0, 0), (0.45, -0.2, "o"), (1, -0.1, "io")], 520)]
    spec("git-commit", [Part("l", Path().M(3, 12).L(8.5, 12)), Part("r", Path().M(21, 12).L(15.5, 12)),
                        Part("ring", ring, tint="accent"), solid("dot", ring, "ring", "accent")],
         hover=grow + tint("ring", dur=260),
         press=[T("ring", "scale", [(0, 0), (0.3, -0.25, "o"), (1, 0, "back")], 340)],
         active=grow + tint("ring", "dot") + [fill_in("dot", 1, 300, delay=250)],
         note="提交点胀大着色，两侧连线让位",
         active_note="提交点变实心——当前所在的提交")


def git_remote():
    cloud = I.git_remote()
    arrow = cat(Path().M(11.75, 15.5).L(11.75, 10.5), I.arrowhead((11.75, 10.5), (0, -1), 2.4))
    spec("git-remote", [Part("cloud", cloud, tint="accent"),
                        Part("arrow", arrow, parent="cloud", rest={"opacity": 0}, extra=True, tint="accent")],
         hover=[T("cloud", "ty", [(0, 0), (0.5, -0.9, "io"), (1, 0, "io")], 1200),
                T("arrow", "opacity", [(0, 0), (0.2, 1, "o"), (0.75, 1), (1, 0, "i")], 1100, loop=True),
                T("arrow", "ty", [(0, 2), (0.6, 0, "o"), (1, -2, "i")], 1100, loop=True),
                *tint("arrow", dur=1)],
         press=[T("cloud", "scale", BUMP, 260)],
         active=[T("cloud", "ty", [(0, 0), (0.5, -0.9, "io"), (1, 0, "io")], 900),
                 T("arrow", "opacity", [(0, 0), (1, 1, "o")], 400, delay=200),
                 T("arrow", "ty", [(0, 2), (1, 0, "back")], 500, delay=200),
                 *tint("cloud", "arrow")],
         note="云朵上浮，云里的上传箭头一趟趟往上送（悬浮期间循环）",
         active_note="箭头停在云中，着强调色")


def tag():
    f = lambda u, v: (12 + (u - v) * 0.7071, 12 + (u + v) * 0.7071)
    pts = [f(-8.2, -4.8), f(3.6, -4.8), f(8.4, 0), f(3.6, 4.8), f(-8.2, 4.8)]
    body = cat(Path().rpoly(pts, [2.2, 1.6, 1.6, 1.6, 2.2]), Path().circle(*f(-4.4, 0), 1.2))
    hole = f(-4.4, 0)
    spec("tag", [Part("t", body, origin=hole, tint="accent"), solid("tf", body, "t", "accent", origin=hole)],
         hover=[T("t", "rotate", wobble([14, -9, 5, -2]), 1000)],
         press=[T("t", "scale", BUMP, 260)],
         active=[T("t", "rotate", [(0, 0), (0.3, 14, "o"), (0.6, -14, "io"), (1, -8, "io")], 800),
                 *tint("t", "tf"), fill_in("tf", 1, 360, delay=350)],
         note="标签挂在孔上来回摆",
         active_note="微微歪着停住，实心强调色（孔保留）")


def star():
    s = I.star()
    c = (12, 12.9)
    rays = Path()
    for i in range(5):
        a = math.radians(-54 + i * 72)
        rays.M(c[0] + 6.9 * math.cos(a), c[1] + 6.9 * math.sin(a)).L(c[0] + 9.0 * math.cos(a), c[1] + 9.0 * math.sin(a))
    spec("star", [Part("s", s, origin=c, tint="warning"),
                  solid("f", s, "s", "warning", origin=c),
                  Part("rays", rays, origin=c, rest={"opacity": 0}, extra=True, tint="warning")],
         hover=[T("s", "rotate", [(0, 0), (0.3, -12, "io"), (0.65, 6, "io"), (1, 0, "io")], 640)],
         press=[T("s", "scale", [(0, 0), (0.3, -0.18, "o"), (1, 0, "back")], 320)],
         active=[T("s", "scale", [(0, 0), (0.25, -0.28, "io"), (0.6, 0.22, "o"), (1, 0, "io")], 560),
                 T("s", "tint", [(0, 0), (0.4, 1, "o")], 560), *tint("f", "rays", dur=1),
                 T("f", "fill", [(0, 0), (0.3, 0), (0.55, 1, "o")], 560),
                 T("rays", "opacity", [(0, 0), (0.42, 0), (0.42, 1), (0.8, 1), (1, 0, "o")], 640),
                 T("rays", "scale", [(0, -0.35), (0.42, -0.35), (1, 0.12, "o")], 640)],
         note="悬浮轻晃；点击收藏：先收缩蓄力，再弹出填满金色，同时迸出五道光芒",
         active_note="实心金色星")


def glyph_row(gs, gap):
    placed, x = [], 0.0
    for g in gs:
        x0, _, x1, _ = g.bbox()
        placed.append(g.translate(x - x0, 0))
        x += (x1 - x0) + gap
    return placed


def match_case():
    A, a = glyph_row([G.g_A(), G.g_a()], 1.8)
    hop = [(0, 0), (0.3, -1.6, "o"), (0.6, 0, "i"), (0.75, -0.3, "o"), (0.9, 0, "i")]
    ax0, _, ax1, _ = A.bbox()
    spec("match-case", [Part("A", A, origin=((ax0 + ax1) / 2, 17.5), tint="accent"), Part("a", a, origin=(0, 17.5), tint="accent")],
         hover=[T("A", "ty", hop, 640), T("a", "ty", hop, 640, delay=110)],
         press=[T("A", "scale", BUMP, 260), T("a", "scale", BUMP, 260, delay=40)],
         active=[T("A", "ty", hop, 640), T("a", "ty", hop, 640, delay=110), *tint("A", "a", step=110),
                 T("A", "sy", [(0, 0), (0.55, 0), (0.62, -0.12, "o"), (0.8, 0, "back")], 640)],
         note="大小写字母依次跳一下",
         active_note="跳完着强调色")


def match_regex():
    ast = Path()
    c, r = (14, 9.5), 4.2
    for a in (90, 30, 150):
        ra = math.radians(a)
        ast.M(c[0] + r * math.cos(ra), c[1] + r * math.sin(ra)).L(c[0] - r * math.cos(ra), c[1] - r * math.sin(ra))
    bounce = [(0, 0), (0.3, -1.6, "o"), (0.6, 0, "i"), (0.78, -0.4, "o"), (0.95, 0, "i")]
    spec("match-regex", [Part("dot", Path().circle(6.5, 17, 0.6), origin=(6.5, 17), tint="accent"),
                         Part("ast", ast, origin=c, wrap=60, tint="accent")],
         hover=[T("ast", "rotate", [(0, 0), (1, 60, "back")], 560), T("dot", "ty", bounce, 640)],
         press=[T("ast", "scale", BUMP, 260)],
         active=[T("ast", "rotate", [(0, 0), (1, 120, "back")], 700), T("ast", "scale", [(0, 0), (0.4, 0.18, "o"), (1, 0, "io")], 700),
                 T("dot", "ty", bounce, 640), *tint("ast", "dot")],
         note="星号转 60°，点跳一下",
         active_note="星号转两格并一胀，着强调色")


def match_word():
    a, b = glyph_row([G.g_a(), G.g_b()], 2.6)
    letters = cat(a, b)
    lx0, _, lx1, _ = letters.bbox()
    letters = letters.translate(12 - (lx0 + lx1) / 2, -1.8)
    x0, _, x1, _ = letters.bbox()
    under = Path().M(x0 - 1, 18.2).L(x0 - 1, 19.7).L(x1 + 1, 19.7).L(x1 + 1, 18.2)
    lift = [(0, 0), (0.3, -0.9, "o"), (0.7, 0, "back")]
    spec("match-word", [Part("ab", letters, tint="accent"), Part("u", under, tint="accent")],
         hover=[T("u", "draw", [(0, -1), (0.1, -1), (1, 0, "io")], 560), T("ab", "ty", lift, 640)],
         press=[T("ab", "scale", BUMP, 260)],
         active=[T("u", "draw", [(0, -1), (0.1, -1), (1, 0, "io")], 560), T("ab", "ty", lift, 640), *tint("ab", "u")],
         note="下方括线重新描出，字母一抬",
         active_note="描完着强调色")


def diff(name, a, b, axis):
    halves = [Part("h0", Path().M(12, 12).L(*a), tint="accent"), Part("h1", Path().M(12, 12).L(*b), tint="accent")]
    nudge = [(0, 0), (0.3, -1.5, "io"), (0.65, 1, "io"), (1, 0, "io")]
    act = [T(h, p, k, 420) for h in ("h0", "h1") for p, k in (("draw", [(0, -1), (1, 0, "o")]), ("tint", [(0, 0), (1, 1, "o")]))]
    spec(name, [Part("frame", I.frame())] + halves,
         hover=[T(h, axis, nudge, 700) for h in ("h0", "h1")],
         press=[T("frame", "scale", BUMP, 260)],
         active=act,
         note="分隔线左右（或上下）试探性地挪一挪",
         active_note="分隔线从中心向两端重新画出并着色")


def help_():
    q = Path().arc(12, 9.6, 2.5, 2.5, 180, 400, move=True)
    q.C(13.9, 11.7, 12, 12.2, 12, 13.8)
    circ = Path().circle(12, 12, 8.5)
    tilt = [(0, 0), (0.25, -16, "io"), (0.55, 11, "io"), (0.8, -4, "io"), (1, 0, "io")]
    bounce = [(0, 0), (0.55, 0), (0.7, -1.4, "o"), (0.85, 0, "i"), (0.93, -0.3, "o"), (1, 0, "i")]
    spec("help", [Part("circ", circ, tint="accent"), solid("disc", circ, None, "accent"),
                  group("qm", origin=(12, 16.8)),
                  Part("q", q, parent="qm", tint="accent"), Part("dot", Path().dot(12, 16.8), parent="qm", tint="accent")],
         hover=[T("qm", "rotate", tilt, 820), T("dot", "ty", bounce, 820), *tint("q", "dot", dur=320)],
         press=[T("circ", "scale", BUMP, 260)],
         active=[T("qm", "rotate", tilt, 820), T("dot", "ty", bounce, 820), *tint("q", "dot", "circ", "disc"),
                 fill_in("disc", 0.16, 400, delay=300)],
         note="问号歪头晃一晃，底下的点再跳一下",
         active_note="圆内淡色填充，着强调色")


def info():
    circ = Path().circle(12, 12, 8.5)
    bounce = [(0, 0), (0.3, -2.2, "o"), (0.55, 0, "i"), (0.7, -0.6, "o"), (0.85, 0, "i")]
    squash = [(0, 0), (0.55, 0), (0.62, -0.1, "o"), (0.8, 0, "io")]
    spec("info", [Part("circ", circ, tint="accent"), solid("disc", circ, None, "accent"),
                  Part("stem", Path().M(12, 11).L(12, 16.5), origin=(12, 16.5), tint="accent"),
                  Part("dot", Path().dot(12, 7.8), tint="accent")],
         hover=[T("dot", "ty", bounce, 760), T("stem", "sy", squash, 760), *tint("dot", dur=300)],
         press=[T("circ", "scale", BUMP, 260)],
         active=[T("dot", "ty", bounce, 760), T("stem", "sy", squash, 760), *tint("dot", "stem", "circ", "disc"),
                 fill_in("disc", 0.16, 400, delay=300)],
         note="i 上的点跳起落下，竖笔被压一下",
         active_note="圆内淡色填充，着强调色")


def alert():
    tri = Path().rpoly([(12, 3.5), (21, 19.5), (3, 19.5)], 2.2)
    shake = [(0, 0), (0.12, -7, "io"), (0.3, 6, "io"), (0.48, -4, "io"), (0.66, 2.5, "io"), (0.84, -1, "io"), (1, 0, "io")]
    spec("alert", [Part("tri", tri, origin=(12, 19.5), tint="warning"), solid("plate", tri, "tri", "warning", origin=(12, 19.5)),
                   Part("bang", cat(Path().M(12, 9.5).L(12, 13.5), Path().dot(12, 16.5)), parent="tri", origin=(12, 16.5), tint="warning")],
         hover=[T("tri", "rotate", shake, 620), *tint("tri", "bang", dur=200),
                T("bang", "scale", [(0, 0), (0.1, 0.25, "o"), (0.5, 0, "io")], 620)],
         press=[T("bang", "scale", [(0, 0), (0.3, 0.2, "o"), (1, 0, "back")], 300)],
         active=[T("tri", "rotate", shake, 620), *tint("tri", "bang", "plate", dur=200), fill_in("plate", 0.2, 400, delay=300)],
         note="三角以底边为轴抖动，感叹号一胀；变警告黄",
         active_note="三角内淡黄填充")


def check():
    spec("check", [Part("c", Path().M(4.5, 12.5).L(9.5, 17.5).L(19.5, 6.5), tint="success")],
         hover=[T("c", "draw", [(0, -1), (0.15, -1), (1, 0, "o")], 520), *tint("c", dur=260)],
         press=[T("c", "scale", BUMP, 300)],
         active=[T("c", "draw", [(0, -1), (1, 0, "o")], 420), *tint("c"),
                 T("c", "scale", [(0, 0), (0.6, 0.14, "o"), (1, 0.06, "io")], 620)],
         note="对勾重新一笔写出，转绿色",
         active_note="写完略放大，绿色")


def badge(name, symbol, anchor, hover):
    """Status badge: the disc pops in and the cut-out symbol grows out of
    `anchor` (the check from its first stroke, the others from the center).
    One solid part, so the symbol stays a hole while it moves."""
    disc = I.badge_disc()
    seed = symbol().scale_about(0.04, *anchor)
    grow = [(0, 1), (0.3, 1), (1, 0, "o")]
    spec(name, [Part("b", cat(disc, symbol()), fill=True, to=cat(I.badge_disc(), seed))],
         hover=hover,
         press=[T("b", "scale", BUMP, 300)],
         active=[T("b", "scale", [(0, -0.4), (1, 0, "back")], 460), T("b", "morph", grow, 560)],
         note="悬浮时按各自语义动一下",
         active_note="入场：圆盘弹出，符号从中心（对勾从起笔）长出；姿态即静止形")


def status_badges():
    regrow = [(0, 0), (0.3, 1, "i"), (1, 0, "o")]
    badge("status-success", I.check_symbol, I.CHECK_START, [T("b", "morph", regrow, 640)])
    badge("status-error", I.x_symbol, (12, 12),
          [T("b", "tx", wobble([-1.4, 1.2, -0.8, 0.4]), 520)])
    badge("status-warning", I.bang_symbol, (12, 12),
          [T("b", "rotate", wobble([-10, 8, -5, 2.5]), 620)])
    badge("status-info", I.info_symbol, (12, 12),
          [T("b", "ty", [(0, 0), (0.3, -1.4, "o"), (0.55, 0, "i"), (0.7, -0.4, "o"), (0.85, 0, "i")], 700)])


def loading():
    """LoadingSpinner holds `hovered` for as long as it spins: hover is the
    one layer that may loop. Every loop is a fixed period, so the speed is
    the same at any frame rate."""
    dust = [f"d{i}" for i in range(len(I.LOADING_DUST))]
    parts = [Part("prompt", I.loading_prompt(), fill=True), group("orbit", wrap=360)]
    parts += [Part(d, I.loading_dust(i), parent="orbit", fill=True, tint="accent") for i, d in enumerate(dust)]
    spec("loading", parts,
         hover=[T("orbit", "rotate", [(0, 0), (1, 360, "l")], 1100, loop=True),
                *tint(*dust, dur=360, step=40),
                # The trail fades out behind the head, and each speck flickers
                # in turn, a wave running down the trail.
                *[T(d, "opacity", [(0, 0), (1, -0.13 * i, "o")], 420) for i, d in enumerate(dust)],
                *[T(d, "opacity", [(0, 0), (0.5, -0.2, "io"), (1, 0, "io")], 900, delay=i * 110, loop=True)
                  for i, d in enumerate(dust) if i > 0],
                T("prompt", "opacity", [(0, 0), (0.5, -0.45, "io"), (1, 0, "io")], 2200, loop=True)],
         note="加载中：尘埃彗星绕提示符匀速旋转并转强调色，彗尾渐隐、逐粒闪烁，提示符缓慢呼吸")


BUILDERS = [bell, settings, sliders, palette, layout,
            lambda: panel("panel-left", 9.5, -1), lambda: panel("panel-right", 14.5, 1), sun, moon,
            refresh, history, trash, copy, paste, search, x_, check, plus_, file_plus, folder_plus,
            external_link, undo, pencil, eraser, play, update, arrow_down, chevrons, files, code, terminal, keyboard, cpu,
            git_branch, git_commit, git_remote, tag, star,
            match_case, match_regex, match_word,
            lambda: diff("diff-split", (12, 5), (12, 19), "tx"), lambda: diff("diff-unified", (3, 12), (21, 12), "ty"),
            help_, info, alert, status_badges, loading]

GROUPS = [
    ("侧栏 / 设置导航", ["bell", "settings", "sliders", "layout", "palette", "keyboard", "panel-left", "panel-right", "sun", "moon"]),
    ("工具栏操作", ["refresh", "history", "trash", "copy", "paste", "search", "x", "check", "plus", "file-plus",
                "folder-plus", "external-link", "undo", "pencil", "eraser", "play", "update", "arrow-down",
                "chevrons-up-down", "files", "code", "terminal", "cpu"]),
    ("Git", ["git-branch", "git-commit", "git-remote", "tag", "star"]),
    ("切换按钮", ["match-case", "match-regex", "match-word", "diff-unified", "diff-split"]),
    ("状态", ["help", "info", "alert", "loading"]),
    ("状态徽标", ["status-success", "status-error", "status-warning", "status-info"]),
]


def build():
    SPECS.clear()
    for b in BUILDERS:
        b()
    return SPECS
