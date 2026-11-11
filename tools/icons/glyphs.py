"""Monoline letterforms for monogram icons.

Metrics (24 grid): cap top 6.5, baseline 17.5, x-height top 10, descender 20.5.
Each glyph is drawn from x=0; layout() measures and centres them.
"""
from geo import Path, cat

CAP, BASE, XH, DESC = 6.5, 17.5, 10.0, 20.5
MID = (CAP + BASE) / 2


def S_curve(w, top, bot):
    h = bot - top
    m = (top + bot) / 2
    p = Path()
    p.M(w, top + h * 0.17)
    p.C(w * 0.86, top + h * 0.05, w * 0.7, top, w * 0.5, top)
    p.C(w * 0.2, top, 0, top + h * 0.1, 0, top + h * 0.27)
    p.C(0, top + h * 0.45, w * 0.22, m - h * 0.02, w * 0.5, m)
    p.C(w * 0.8, m + h * 0.02, w, top + h * 0.6, w, top + h * 0.74)
    p.C(w, top + h * 0.92, w * 0.78, bot, w * 0.48, bot)
    p.C(w * 0.25, bot, w * 0.07, bot - h * 0.07, 0, bot - h * 0.2)
    return p


def g_C():
    rx, ry = 3.9, (BASE - CAP) / 2
    return Path().arc(rx, MID, rx, ry, -42, -318, move=True)


def g_G():
    rx, ry = 4.0, (BASE - CAP) / 2
    p = Path().arc(rx, MID, rx, ry, -42, -360, move=True)
    return p.M(2 * rx, MID).L(rx + 0.6, MID)


def g_o():
    return Path().ellipse(3.3, (XH + BASE) / 2, 3.3, (BASE - XH) / 2)


def g_R():
    r = 2.8
    p = Path().M(0, BASE).L(0, CAP).L(3.2, CAP)
    p.arc(3.2, CAP + r, r, r, -90, 90)
    p.L(0, CAP + 2 * r)
    return p.M(2.9, CAP + 2 * r).L(6.2, BASE)


def g_P():
    r = 2.9
    p = Path().M(0, BASE).L(0, CAP).L(3.3, CAP)
    p.arc(3.3, CAP + r, r, r, -90, 90)
    return p.L(0, CAP + 2 * r)


def g_S():
    return S_curve(6.2, CAP, BASE)


def g_s():
    return S_curve(5.0, XH, BASE)


def g_y():
    return Path().M(0, XH).L(3.1, BASE - 0.3).M(6, XH).L(1.9, DESC)


def g_J():
    r = 2.6
    p = Path().M(5.2, CAP).L(5.2, BASE - r)
    return p.arc(5.2 - r, BASE - r, r, r, 0, 165)


def g_T():
    return Path().M(0, CAP).L(7, CAP).M(3.5, CAP).L(3.5, BASE)


def g_K():
    return Path().M(0, CAP).L(0, BASE).M(6, CAP).L(0.2, 13.3).M(2.2, 11.2).L(6.3, BASE)


def g_t():
    r = 2.0
    p = Path().M(1.8, 7.3).L(1.8, BASE - r)
    p.arc(1.8 + r, BASE - r, r, r, 180, 90)
    p.L(5.0, BASE)
    return p.M(0, XH).L(4.8, XH)


def g_b():
    ry = (BASE - XH) / 2
    return Path().M(0, CAP).L(0, BASE).add(Path().ellipse(3.3, XH + ry, 3.3, ry))


def g_h(r=2.8):
    p = Path().M(0, CAP).L(0, BASE)
    p.M(0, 13.2).arc(r, 13.2, r, 3.2, 180, 360).L(2 * r, BASE)
    return p


def g_p(rx=3.3):
    ry = (BASE - XH) / 2
    return Path().M(0, XH).L(0, DESC).add(Path().ellipse(rx, XH + ry, rx, ry))


def g_w():
    return Path().M(0, XH).L(1.8, BASE).L(3.6, 11.6).L(5.4, BASE).L(7.2, XH)


def g_Q():
    ry = (BASE - CAP) / 2
    return Path().ellipse(4.1, MID, 4.1, ry).M(5.2, 14.6).L(8.2, BASE + 0.8)


def g_A():
    return Path().M(0, BASE).L(3.8, CAP).L(7.6, BASE).M(1.35, 13.8).L(6.25, 13.8)


def g_a():
    ry = (BASE - XH) / 2
    return Path().ellipse(3.2, XH + ry, 3.2, ry).M(6.4, XH).L(6.4, BASE)


def g_V():
    return Path().M(0, CAP).L(3.7, BASE).L(7.4, CAP)


def g_hash(h=10.0, w=6.8):
    top, bot = MID - h / 2, MID + h / 2
    y1, y2 = MID - h * 0.19, MID + h * 0.19
    return (Path().M(2.6, top).L(1.6, bot).M(6.0, top).L(5.0, bot)
            .M(0.2, y1).L(w, y1).M(-0.2, y2).L(w - 0.4, y2))


def g_plus(s=4.4):
    return Path().M(0, MID).L(s, MID).M(s / 2, MID - s / 2).L(s / 2, MID + s / 2)


def g_0():
    return Path().ellipse(3.1, MID, 3.1, (BASE - CAP) / 2)


def g_1():
    return Path().M(0, 8.8).L(3, CAP).L(3, BASE)


def layout(glyphs, gap=2.8, cx=12.0, dy=0.0):
    """Place glyphs left→right with `gap` between centre-line extents."""
    placed = []
    x = 0.0
    for g in glyphs:
        x0, _, x1, _ = g.bbox()
        placed.append(g.translate(x - x0, 0))
        x += (x1 - x0) + gap
    total = cat(*placed)
    x0, _, x1, _ = total.bbox()
    return total.translate(cx - (x0 + x1) / 2, dy)
