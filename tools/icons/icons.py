"""Static geometry of every SpanTui icon (docs/ui/icons.md). Emitted by generate.py."""
import math

from geo import Path, cat, circle_intersections, angle, norm
import glyphs as G

ICONS = {}   # name -> (folder, Path, filled)


def icon(folder, filled=False):
    def deco(fn):
        ICONS[fn.__name__.replace("_", "-")] = (folder, fn, filled)
        return fn
    return deco


def frame(x=3, y=5, w=18, h=14, r=3):
    """Landscape container (windows, panels, media)."""
    return Path().rrect(x, y, w, h, r)


def page(x=5, y=3, w=14, h=18, fold=4.5, r=3):
    """Document silhouette with folded top-right corner."""
    p = Path().M(x + w, y + fold).L(x + w, y + h - r)
    p.arc(x + w - r, y + h - r, r, r, 0, 90).L(x + r, y + h)
    p.arc(x + r, y + h - r, r, r, 90, 180).L(x, y + r)
    p.arc(x + r, y + r, r, r, 180, 270).L(x + w - fold, y).Z()
    q = Path().M(x + w - fold, y).L(x + w - fold, y + fold - 1.5)
    q.arc(x + w - fold + 1.5, y + fold - 1.5, 1.5, 1.5, 180, 90).L(x + w, y + fold)
    return cat(p, q)


def folder_shape(x=3, y=4.5, w=18, h=15, r=3):
    tab_w = 6.5
    p = Path().M(x + r, y).L(x + tab_w - 1.2, y)
    p.L(x + tab_w + 0.8, y + 2).L(x + w - r, y + 2)
    p.arc(x + w - r, y + 2 + r, r, r, -90, 0).L(x + w, y + h - r)
    p.arc(x + w - r, y + h - r, r, r, 0, 90).L(x + r, y + h)
    p.arc(x + r, y + h - r, r, r, 90, 180).L(x, y + r)
    p.arc(x + r, y + r, r, r, 180, 270)
    return p.Z()


def arrowhead(tip, direction, size=3.6):
    """Right-angle arrowhead at `tip` pointing along `direction`."""
    d = norm(direction)
    out = []
    for s in (1, -1):
        a = math.radians(135 * s)
        v = (d[0] * math.cos(a) - d[1] * math.sin(a), d[0] * math.sin(a) + d[1] * math.cos(a))
        out.append((tip[0] + v[0] * size, tip[1] + v[1] * size))
    return Path().M(*out[0]).L(*tip).L(*out[1])


def plus(cx, cy, s):
    h = s / 2
    return Path().M(cx - h, cy).L(cx + h, cy).M(cx, cy - h).L(cx, cy + h)


def gear(cx=12, cy=12, teeth=8, ro=9, ri=6.9, top=9.0, base=15.0, hole=3):
    p = Path()
    pitch = 360 / teeth
    for i in range(teeth):
        c = -90 + i * pitch
        a_base0, a_top0 = c - base, c - top
        a_top1, a_base1 = c + top, c + base
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
    p.Z()
    return cat(p, Path().circle(cx, cy, hole))


def arc_pick(p, c, r, pa, pb, test):
    """Append the arc of circle (c, r) from pa to pb whose midpoint passes test."""
    a0, a1 = angle(c, pa), angle(c, pb)
    cw = (a1 - a0) % 360
    for span in (cw, cw - 360):
        m = math.radians(a0 + span / 2)
        if test((c[0] + r * math.cos(m), c[1] + r * math.sin(m))):
            return p.arc(c[0], c[1], r, r, a0, a0 + span)
    raise ValueError("no arc passes test")


def star_pts(cx, cy, ro, ri):
    pts = []
    for i in range(10):
        a = math.radians(-90 + i * 36)
        r = ro if i % 2 == 0 else ri
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def star():
    return Path().rpoly(star_pts(12, 12.9, 9.2, 4.3), [1.2, 0.4] * 5)


def triangle_play():
    return Path().rpoly([(7.5, 4.8), (20, 12), (7.5, 19.2)], 2.2).centered()


@icon("actions")
def check():
    return Path().M(4.5, 12.5).L(9.5, 17.5).L(19.5, 6.5)


@icon("actions")
def copy():
    front = Path().rrect(8.5, 8.5, 12.5, 12.5, 2.5)
    back = Path().M(5.5, 15.5).arc(5.5, 13, 2.5, 2.5, 90, 180).L(3, 5.5)
    back.arc(5.5, 5.5, 2.5, 2.5, 180, 270).L(13, 3).arc(13, 5.5, 2.5, 2.5, -90, 0)
    return cat(front, back)


@icon("actions")
def paste():
    board = Path().M(8.5, 4.75).L(8, 4.75).arc(8, 7.75, 3, 3, -90, -180).L(5, 18)
    board.arc(8, 18, 3, 3, 180, 90).L(16, 21).arc(16, 18, 3, 3, 90, 0).L(19, 7.75)
    board.arc(16, 7.75, 3, 3, 0, -90).L(15.5, 4.75)
    clip = Path().rrect(8.5, 3, 7, 3.5, 1.25)
    return cat(board, clip)


@icon("actions")
def eraser():
    f = lambda u, v: (12 + (u + v) * 0.7071, 11 + (-u + v) * 0.7071)
    body = [f(-8.5, -3.8), f(5.5, -3.8), f(5.5, 3.8), f(-8.5, 3.8)]
    p = Path().rpoly(body, 2.2)
    div = Path().M(*f(-3, -3.8)).L(*f(-3, 3.8))
    ground = Path().M(12, 20.5).L(20, 20.5)
    return cat(p, div, ground).centered()


@icon("actions")
def minus():
    return Path().M(5, 12).L(19, 12)


@icon("actions")
def pencil():
    t = (4, 20)
    d = norm((1, -1)); n = (d[1] * -1, d[0])
    f = lambda u, v: (t[0] + d[0] * u + n[0] * v, t[1] + d[1] * u + n[1] * v)
    w, L = 2.3, 19.6
    c = f(L - w, 0)
    a0 = angle(c, f(L - w, -w))
    far = angle(c, f(L, 0))
    sweep = 180 if (far - a0) % 360 < 180 else -180
    p = Path().M(*f(0, 0)).L(*f(4.2, -w)).L(*f(L - w, -w))
    p.arc(c[0], c[1], w, w, a0, a0 + sweep)
    p.L(*f(4.2, w)).Z()
    band = Path().M(*f(L - 6.2, -w)).L(*f(L - 6.2, w))
    return cat(p, band)


@icon("actions")
def pin():
    # Push pin: cap, barrel flaring to the collar, needle.
    cap = Path().M(9, 3.75).L(15, 3.75)
    barrel = Path().M(10, 3.75).L(10, 9.25).L(7, 13.25).L(17, 13.25).L(14, 9.25).L(14, 3.75)
    needle = Path().M(12, 13.25).L(12, 20.25)
    return cat(cap, barrel, needle)


@icon("actions", filled=True)
def play():
    return triangle_play()


@icon("actions")
def plus_():
    return plus(12, 12, 14)


@icon("actions")
def refresh():
    r = 8
    p = Path().arc(12, 12, r, r, 160, 322, move=True)
    e1 = p.cur()
    t1 = (-math.sin(math.radians(322)), math.cos(math.radians(322)))
    q = Path().arc(12, 12, r, r, 340, 502, move=True)
    e2 = q.cur()
    t2 = (-math.sin(math.radians(502)), math.cos(math.radians(502)))
    return cat(p, arrowhead(e1, t1, 3.4), q, arrowhead(e2, t2, 3.4))


@icon("actions")
def search():
    c, r = (10.5, 10.5), 6.5
    k = r * 0.7071
    return cat(Path().circle(c[0], c[1], r), Path().M(c[0] + k, c[1] + k).L(20, 20))


@icon("actions")
def trash():
    p = Path().M(3.5, 6.5).L(20.5, 6.5)
    p.M(9, 6.5).L(9, 5).arc(10.5, 5, 1.5, 1.5, 180, 270).L(13.5, 3.5).arc(13.5, 5, 1.5, 1.5, -90, 0).L(15, 6.5)
    body = Path().M(5.5, 6.5).L(6.35, 18.6)
    body.arc(8.85, 18.43, 2.5, 2.5, 176, 90).L(15.15, 20.93)
    body.arc(15.15, 18.43, 2.5, 2.5, 90, 4).L(18.5, 6.5)
    lines = Path().M(10, 10.5).L(10, 16.5).M(14, 10.5).L(14, 16.5)
    return cat(p, body, lines)


@icon("actions")
def undo():
    p = Path().M(8.5, 13.5).L(4, 9).L(8.5, 4.5)
    p.M(4, 9).L(14.5, 9).arc(14.5, 14.25, 5.25, 5.25, -90, 90).L(10.5, 19.5)
    return p.centered(axis="y")


@icon("actions")
def update():
    return cat(Path().circle(12, 12, 8.5),
               Path().M(12, 7.5).L(12, 16),
               arrowhead((12, 16), (0, 1), 3.5))


@icon("actions")
def x():
    return Path().M(6, 6).L(18, 18).M(18, 6).L(6, 18)


@icon("editor")
def code():
    return Path().M(8, 7.5).L(3.5, 12).L(8, 16.5).M(16, 7.5).L(20.5, 12).L(16, 16.5).M(13.6, 5).L(10.4, 19)


def Aa():
    return G.layout([G.g_A(), G.g_a()], gap=1.8)


@icon("editor")
def match_case():
    return Aa()


@icon("editor")
def match_regex():
    dot = Path().circle(6.5, 17, 0.6)
    c = (14, 9.5); r = 4.2
    p = Path()
    for a in (90, 30, 150):
        ra = math.radians(a)
        p.M(c[0] + r * math.cos(ra), c[1] + r * math.sin(ra)).L(c[0] - r * math.cos(ra), c[1] - r * math.sin(ra))
    return cat(dot, p).centered()


@icon("editor")
def match_word():
    letters = G.layout([G.g_a(), G.g_b()], gap=2.6, dy=-1.8)
    x0, _, x1, _ = letters.bbox()
    under = Path().M(x0 - 1, 18.2).L(x0 - 1, 19.7).L(x1 + 1, 19.7).L(x1 + 1, 18.2)
    return cat(letters, under).centered()


@icon("editor")
def settings():
    return gear()


@icon("files")
def file():
    return page()


@icon("files")
def file_plus():
    return cat(page(), plus(12, 14.5, 5.5))


@icon("files")
def files():
    front = page(x=8, y=6, w=12.5, h=15, fold=4)
    back = Path().M(5, 17.5).L(5, 6).arc(8, 6, 3, 3, 180, 270).L(14.5, 3)
    return cat(front, back).centered()


@icon("files")
def folder():
    return folder_shape()


@icon("files")
def folder_open():
    pts = [(5.43, 15.5), (6.6, 10.5), (21.3, 10.5), (19.2, 19.5), (3, 19.5), (3, 4.5),
           (8.8, 4.5), (10.8, 6.5), (18.5, 6.5), (18.5, 10.5)]
    return Path().rpoly(pts, [0, 1.5, 1.5, 2, 3, 3, 0.8, 0.8, 3, 0], closed=False)


@icon("files")
def folder_plus():
    return cat(folder_shape(), plus(12, 12.75, 5.5))


@icon("git")
def git_branch():
    r = 2.3
    a, b = (6.5, 17.5), (17.5, 6.5)
    p = Path().M(6.5, 3.5).L(6.5, a[1] - r)
    curve = Path().M(b[0], b[1] + r).C(b[0], 13, 13.5, a[1], a[0] + r, a[1])
    return cat(Path().circle(*a, r), Path().circle(*b, r), p, curve)


@icon("git")
def git_commit():
    return cat(Path().circle(12, 12, 3.5), Path().M(3, 12).L(8.5, 12).M(15.5, 12).L(21, 12))


@icon("git")
def git_remote():
    # Cloud: a large and a small lobe standing on one flat base.
    c1, r1 = (9.5, 12), 5.5
    base = c1[1] + r1
    r2 = 4.2
    c2 = (16.3, base - r2)
    up = min(circle_intersections(c1, r1, c2, r2), key=lambda q: q[1])
    a1 = angle(c1, up) % 360
    if a1 < 90:
        a1 += 360
    a2 = angle(c2, up) % 360
    p = Path().M(c1[0], base)
    p.arc(c1[0], c1[1], r1, r1, 90, a1)
    p.arc(c2[0], c2[1], r2, r2, a2, 450)
    p.Z()
    return p.centered()


@icon("git")
def history():
    c, r = (12, 12), 8.5
    p = Path().arc(12, 12, r, r, 180, 180 - 315, move=True)  # left → CCW around → top-left
    tip = p.cur()
    a = math.radians(180 - 315)
    tangent = (math.sin(a), -math.cos(a))
    head = arrowhead(tip, tangent, 3.4)
    hands = Path().M(12, 7.5).L(12, 12).L(15, 14)
    return cat(p, head, hands)


@icon("git")
def star_():
    return star()


@icon("git", filled=True)
def star_filled():
    return star()


@icon("git")
def tag():
    f = lambda u, v: (12 + (u - v) * 0.7071, 12 + (u + v) * 0.7071)
    pts = [f(-8.2, -4.8), f(3.6, -4.8), f(8.4, 0), f(3.6, 4.8), f(-8.2, 4.8)]
    body = Path().rpoly(pts, [2.2, 1.6, 1.6, 1.6, 2.2])
    hole = Path().circle(*f(-4.4, 0), 1.2)
    return cat(body, hole).centered()


@icon("layout")
def diff_unified():
    return cat(frame(), Path().M(3, 12).L(21, 12))


@icon("layout")
def diff_split():
    return cat(frame(), Path().M(12, 5).L(12, 19))


@icon("layout")
def panel_left():
    return cat(frame(), Path().M(9.5, 5).L(9.5, 19))


@icon("layout")
def panel_right():
    return cat(frame(), Path().M(14.5, 5).L(14.5, 19))


@icon("layout")
def layout():
    return cat(frame(), Path().M(3, 9.5).L(21, 9.5).M(9, 9.5).L(9, 19))


@icon("misc")
def terminal():
    return cat(frame(), Path().M(7.5, 9.5).L(10, 12).L(7.5, 14.5).M(12.5, 14.5).L(16.5, 14.5))


@icon("misc")
def server():
    units = cat(Path().rrect(4, 4, 16, 7, 2), Path().rrect(4, 13, 16, 7, 2))
    lights = Path().dot(7.75, 7.5).dot(7.75, 16.5)
    slots = Path().M(11.5, 7.5).L(16.25, 7.5).M(11.5, 16.5).L(16.25, 16.5)
    return cat(units, lights, slots)


KEY_ROWS = [9.5, 12.5]
KEY_XS = [7, 11.5, 16]
SPACE_BAR = ((8.5, 15.5), (15.5, 15.5))


@icon("misc")
def keyboard():
    p = frame()
    for y in KEY_ROWS:
        for x in KEY_XS:
            p.M(x, y).L(x + 1, y)
    a, b = SPACE_BAR
    return p.M(*a).L(*b)


@icon("misc")
def palette():
    c, r = (12, 12), 8.5
    p = Path().M(12, 20.5)
    p.arc(12, 12, r, r, 90, 360)
    p.C(20.5, 14.1, 19.1, 15.2, 17.3, 15.2)
    p.L(15.6, 15.2)
    p.C(14.1, 15.2, 13.4, 16.9, 14.4, 17.9)
    p.C(15.5, 19, 14.6, 20.5, 12, 20.5)
    dots = cat(Path().circle(7.6, 12.2, 0.7), Path().circle(9.4, 7.9, 0.7), Path().circle(14.4, 7.6, 0.7))
    return cat(p, dots)


@icon("misc")
def sliders():
    p = Path()
    rows = [(7, 15), (12, 8.5), (17, 13)]
    for y, kx in rows:
        p.M(3.5, y).L(kx - 2.2, y).M(kx + 2.2, y).L(20.5, y)
        p.add(Path().circle(kx, y, 2.2))
    return p


@icon("misc")
def bell():
    p = Path().M(6.5, 16.5).L(6.5, 11).arc(12, 11, 5.5, 5.5, 180, 360).L(17.5, 16.5)
    p.M(4.5, 16.5).L(19.5, 16.5)
    p.M(10.2, 19).arc(12, 19, 1.8, 1.5, 180, 0)
    p.M(12, 3.5).L(12, 5.5)
    return p.centered(axis="y")


@icon("navigation")
def arrow_down():
    return Path().M(12, 4.5).L(12, 19.5).add(arrowhead((12, 19.5), (0, 1), 6.4))


@icon("navigation")
def arrow_up():
    return Path().M(12, 19.5).L(12, 4.5).add(arrowhead((12, 4.5), (0, -1), 6.4))


@icon("navigation")
def home():
    roof = Path().M(3.5, 11).L(12, 4).L(20.5, 11)
    body = Path().M(6, 9.2).L(6, 17.5).arc(8.5, 17.5, 2.5, 2.5, 180, 90).L(15.5, 20)
    body.arc(15.5, 17.5, 2.5, 2.5, 90, 0).L(18, 9.2)
    door = Path().M(10, 20).L(10, 15).L(14, 15).L(14, 20)
    return cat(roof, body, door)


def tray():
    """Open-top tray the transfer arrows land in."""
    p = Path().M(4, 14.5).L(4, 17.5).arc(6.5, 17.5, 2.5, 2.5, 180, 90).L(17.5, 20)
    return p.arc(17.5, 17.5, 2.5, 2.5, 90, 0).L(20, 14.5)


@icon("files")
def upload():
    return cat(tray(), Path().M(12, 15.5).L(12, 4).add(arrowhead((12, 4), (0, -1), 4.5)))


@icon("files")
def download():
    return cat(tray(), Path().M(12, 4).L(12, 15.5).add(arrowhead((12, 15.5), (0, 1), 4.5)))


@icon("navigation")
def chevron_down():
    return Path().M(6, 9).L(12, 15).L(18, 9)


@icon("navigation")
def chevron_right():
    return Path().M(9, 6).L(15, 12).L(9, 18)


@icon("navigation")
def chevrons_up_down():
    return Path().M(8, 9.5).L(12, 5.5).L(16, 9.5).M(8, 14.5).L(12, 18.5).L(16, 14.5)


@icon("navigation")
def external_link():
    box = Path().M(18, 13.5).L(18, 18).arc(15, 18, 3, 3, 0, 90).L(6, 21)
    box.arc(6, 18, 3, 3, 90, 180).L(3, 9).arc(6, 9, 3, 3, 180, 270).L(10.5, 6)
    arrow = Path().M(14, 3).L(21, 3).L(21, 10).M(21, 3).L(11.5, 12.5)
    return cat(box, arrow)


@icon("navigation", filled=True)
def tree_chevron():
    return Path().rpoly([(8.5, 5.5), (16.5, 12), (8.5, 18.5)], 1.6).centered()


@icon("status")
def alert():
    tri = Path().rpoly([(12, 3.5), (21, 19.5), (3, 19.5)], 2.2)
    return cat(tri, Path().M(12, 9.5).L(12, 13.5), Path().dot(12, 16.5))


@icon("status")
def cpu():
    body = Path().rrect(6, 6, 12, 12, 2.5)
    die = Path().rrect(9.5, 9.5, 5, 5, 1)
    pins = Path()
    for k in (10, 14):
        pins.M(k, 3).L(k, 6).M(k, 18).L(k, 21).M(3, k).L(6, k).M(18, k).L(21, k)
    return cat(body, die, pins)


@icon("status")
def gpu():
    board = Path().rrect(3, 5.5, 18, 11.5, 2.5)
    fans = cat(Path().circle(8.25, 11.25, 2.6), Path().circle(15.75, 11.25, 2.6))
    edge = Path().M(7, 17).L(7, 19.5).L(13, 19.5).L(13, 17)
    return cat(board, fans, edge).centered(axis="y")


@icon("status")
def help():
    q = Path().arc(12, 9.6, 2.5, 2.5, 180, 400, move=True)
    q.C(13.9, 11.7, 12, 12.2, 12, 13.8)
    return cat(Path().circle(12, 12, 8.5), q, Path().dot(12, 16.8))


@icon("status")
def info():
    return cat(Path().circle(12, 12, 8.5), Path().M(12, 11).L(12, 16.5), Path().dot(12, 7.8))


# Filled disc with the symbol cut out (even-odd): one path, so the badge
# keeps the single-color icon contract and the symbol shows the surface
# behind it. Symbols are filled outlines, never overlapping. They are drawn
# for an r=9 disc (2.8 wide, dots r=1.6) and scaled with the disc, so the badge fills the
# grid like other filled symbol sets and the cut-out stays about 2.3 px wide
# at 16 px instead of blurring into a thin line.
BADGE_R = 11
BADGE_K = BADGE_R / 9
BADGE_W = 2.8
BADGE_DOT = 1.6


def badge_point(p):
    return (12 + (p[0] - 12) * BADGE_K, 12 + (p[1] - 12) * BADGE_K)


def thick_polyline(pts, w=BADGE_W, r=0.7):
    """Outline of a mitered stroke through `pts`, corners rounded by `r`."""
    h = w / 2
    normals = []
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        d = norm((bx - ax, by - ay))
        normals.append((-d[1], d[0]))
    left, right = [], []
    for i, p in enumerate(pts):
        if i == 0 or i == len(pts) - 1:
            n = normals[0] if i == 0 else normals[-1]
            left.append((p[0] + n[0] * h, p[1] + n[1] * h))
            right.append((p[0] - n[0] * h, p[1] - n[1] * h))
            continue
        n1, n2 = normals[i - 1], normals[i]
        m = norm((n1[0] + n2[0], n1[1] + n2[1]))
        k = h / (m[0] * n1[0] + m[1] * n1[1])
        left.append((p[0] + m[0] * k, p[1] + m[1] * k))
        right.append((p[0] - m[0] * k, p[1] - m[1] * k))
    return Path().rpoly(left + right[::-1], r)


def badge_disc():
    return Path().circle(12, 12, BADGE_R)


CHECK_PTS = [(7.6, 12.3), (10.6, 15.3), (16.4, 9.1)]


# Where the check starts, in badge space (the entrance grows it from here).
CHECK_START = badge_point(CHECK_PTS[0])


def check_symbol():
    return thick_polyline(CHECK_PTS).scale_about(BADGE_K)


def x_symbol(arm=4.3):
    h = BADGE_W / 2
    plus = [(-h, -arm), (h, -arm), (h, -h), (arm, -h), (arm, h), (h, h),
            (h, arm), (-h, arm), (-h, h), (-arm, h), (-arm, -h), (-h, -h)]
    c, s = math.cos(math.radians(45)), math.sin(math.radians(45))
    pts = [(12 + x * c - y * s, 12 + x * s + y * c) for x, y in plus]
    return Path().rpoly(pts, 0.6).scale_about(BADGE_K)


def bang_symbol():
    h = BADGE_W / 2
    return cat(Path().rrect(12 - h, 6.4, BADGE_W, 7.0, h),
               Path().circle(12, 16.8, BADGE_DOT)).scale_about(BADGE_K)


def info_symbol():
    h = BADGE_W / 2
    return cat(Path().circle(12, 7.2, BADGE_DOT),
               Path().rrect(12 - h, 10.4, BADGE_W, 7.2, h)).scale_about(BADGE_K)


@icon("status", filled=True)
def status_success():
    return cat(badge_disc(), check_symbol())


@icon("status", filled=True)
def status_error():
    return cat(badge_disc(), x_symbol())


@icon("status", filled=True)
def status_warning():
    return cat(badge_disc(), bang_symbol())


@icon("status", filled=True)
def status_info():
    return cat(badge_disc(), info_symbol())


# The brand mark at chrome size: a prompt with a comet of dust around it. The
# head leads at the top; the trail follows anticlockwise behind it (it travels
# clockwise), spiralling slightly inward and thinning out. Filled, so the dust
# can step down in size freely; the prompt is a filled outline of a stroke.
LOADING_DUST = [  # (degrees behind the head, orbit radius, disc radius)
    (0, 8.0, 1.9), (32, 7.75, 1.55), (64, 7.5, 1.25),
    (96, 7.25, 1.0), (128, 7.0, 0.8), (160, 6.75, 0.62)]
LOADING_PROMPT = [(10.15, 8.9), (13.25, 12), (10.15, 15.1)]


def loading_dust(i):
    behind, orbit, r = LOADING_DUST[i]
    a = math.radians(-90 - behind)
    return Path().circle(12 + orbit * math.cos(a), 12 + orbit * math.sin(a), r)


def loading_prompt():
    return thick_polyline(LOADING_PROMPT, w=1.7, r=0.6)


@icon("status", filled=True)
def loading():
    return cat(loading_prompt(), *[loading_dust(i) for i in range(len(LOADING_DUST))])


@icon("status")
def moon():
    c1, r1 = (11.5, 12.5), 8.4
    c2, r2 = (16.8, 7.6), 6.4
    pa, pb = circle_intersections(c1, r1, c2, r2)
    inside2 = lambda q: (q[0] - c2[0]) ** 2 + (q[1] - c2[1]) ** 2 < r2 * r2
    inside1 = lambda q: (q[0] - c1[0]) ** 2 + (q[1] - c1[1]) ** 2 < r1 * r1
    p = Path().M(*pa)
    arc_pick(p, c1, r1, pa, pb, lambda q: not inside2(q))
    arc_pick(p, c2, r2, pb, pa, inside1)
    return p.Z().centered()


@icon("status")
def sun():
    p = Path().circle(12, 12, 4.2)
    for i in range(8):
        a = math.radians(i * 45)
        p.M(12 + 6.8 * math.cos(a), 12 + 6.8 * math.sin(a)).L(12 + 8.8 * math.cos(a), 12 + 8.8 * math.sin(a))
    return p


@icon("window")
def chrome_maximize():
    return Path().rrect(5.5, 5.5, 13, 13, 2.5)


@icon("window")
def chrome_minimize():
    return Path().M(5.5, 12).L(18.5, 12)


@icon("window")
def chrome_restore():
    back = Path().M(9, 5).L(16.5, 5).arc(16.5, 7.5, 2.5, 2.5, -90, 0).L(19, 15)
    front = Path().rrect(5, 9, 10, 10, 2.5)
    return cat(back, front)


def mono(*gs, gap=2.8):
    return G.layout(list(gs), gap=gap)


@icon("types")
def type_c():
    return mono(G.g_C())


@icon("types")
def type_cpp():
    return mono(G.g_C(), G.g_plus(4.2), G.g_plus(4.2), gap=1.6)


@icon("types")
def type_csharp():
    return mono(G.g_C(), G.g_hash(), gap=2.2)


@icon("types")
def type_go():
    return mono(G.g_G(), G.g_o(), gap=2.2)


@icon("types")
def type_rust():
    return mono(G.g_R(), G.g_s())


@icon("types")
def type_python():
    return mono(G.g_P(), G.g_y(), gap=2.0)


@icon("types")
def type_js():
    return mono(G.g_J(), G.g_S())


@icon("types")
def type_ts():
    return mono(G.g_T(), G.g_S(), gap=2.2)


@icon("types")
def type_java():
    return mono(G.g_J())


@icon("types")
def type_kotlin():
    return mono(G.g_K(), G.g_t(), gap=2.0)


@icon("types")
def type_ruby():
    return mono(G.g_R(), G.g_b())


@icon("types")
def type_php():
    return mono(G.g_p(2.6), G.g_h(2.4), G.g_p(2.6), gap=1.7)


@icon("types")
def type_swift():
    return mono(G.g_S(), G.g_w(), gap=2.2)


@icon("types")
def type_qml():
    return mono(G.g_Q(), G.g_t(), gap=1.4)


@icon("types")
def type_vue():
    return mono(G.g_V())


@icon("types")
def type_binary():
    return mono(G.g_0(), G.g_1(), gap=3.0)


@icon("types")
def type_font():
    return Aa()


@icon("types")
def type_css():
    h = Path().M(10, 4.5).L(8, 19.5).M(16, 4.5).L(14, 19.5).M(4.5, 9.5).L(19.5, 9.5).M(4.5, 14.5).L(19.5, 14.5)
    return h.centered()


def brace(xs, top, bot, s=1):
    """Curly brace: spine at xs, tips toward +s, point toward -s."""
    m = (top + bot) / 2
    xt, xp = xs + s * 2.3, xs - s * 2.3
    p = Path().M(xt, top)
    p.C(xs, top, xs, top, xs, top + 2.3)
    p.L(xs, m - 2.2)
    p.C(xs, m - 0.6, xs - s * 0.9, m, xp, m)
    p.C(xs - s * 0.9, m, xs, m + 0.6, xs, m + 2.2)
    p.L(xs, bot - 2.3)
    p.C(xs, bot, xs, bot, xt, bot)
    return p


@icon("types")
def type_json():
    return cat(brace(7, 4.5, 19.5, 1), brace(17, 4.5, 19.5, -1))


@icon("types")
def type_html():
    return Path().M(8, 7).L(3.5, 12).L(8, 17).M(16, 7).L(20.5, 12).L(16, 17).M(13.5, 5.5).L(10.5, 18.5)


@icon("types")
def type_xml():
    br = Path().M(7.5, 7).L(3, 12).L(7.5, 17).M(16.5, 7).L(21, 12).L(16.5, 17)
    xx = Path().M(9.6, 9.3).L(14.4, 14.7).M(14.4, 9.3).L(9.6, 14.7)
    return cat(br, xx)


@icon("types")
def type_yaml():
    p = Path().M(4, 6).L(11, 6).M(8.5, 12).L(20, 12).M(8.5, 18).L(20, 18)
    p.add(Path().dot(5, 12)).add(Path().dot(5, 18))
    return p


@icon("types")
def type_shell():
    return Path().M(5, 7).L(10, 12).L(5, 17).M(12.5, 17.5).L(19, 17.5)


@icon("types")
def type_doc():
    return cat(page(), Path().M(8.5, 12).L(15.5, 12).M(8.5, 15).L(15.5, 15).M(8.5, 18).L(12.5, 18))


@icon("types")
def type_config():
    return gear()


@icon("types")
def type_git():
    d = Path().rpoly([(12, 2.8), (21.2, 12), (12, 21.2), (2.8, 12)], 2.4)
    ring = Path().circle(12, 12, 2.6)
    return cat(d, ring, Path().M(6.2, 12).L(9.4, 12).M(14.6, 12).L(17.8, 12))


@icon("types")
def type_sql():
    cx, rx, ry, top, bot = 12, 7, 2.6, 6.2, 17.8
    p = Path().ellipse(cx, top, rx, ry)
    p.M(cx - rx, top).L(cx - rx, bot).arc(cx, bot, rx, ry, 180, 0).L(cx + rx, top)
    p.M(cx - rx, 12).arc(cx, 12, rx, ry, 180, 0)
    return p.centered()


@icon("types")
def type_image():
    return cat(frame(), Path().M(3.5, 17.5).L(9, 12).L(12.5, 15.5).L(15.5, 12.5).L(20.5, 17.5),
               Path().circle(8.5, 9.5, 1.6))


@icon("types")
def type_audio():
    r = 2.3
    a, b = (7, 17.5), (17.5, 15.5)
    p = Path().M(a[0] + r, a[1]).L(a[0] + r, 6.3).L(b[0] + r, 4.3).L(b[0] + r, b[1])
    p.M(a[0] + r, 9.3).L(b[0] + r, 7.3)
    return cat(Path().circle(*a, r), Path().circle(*b, r), p).centered()


@icon("types")
def type_video():
    tri = Path().rpoly([(10, 8.8), (15.2, 12), (10, 15.2)], 1.0)
    return cat(frame(), tri.centered(cx=12.4))


@icon("types")
def type_archive():
    lid = Path().rrect(3, 4.5, 18, 4.5, 1.5)
    box = Path().M(4.5, 9).L(4.5, 17).arc(7.5, 17, 3, 3, 180, 90).L(16.5, 20)
    box.arc(16.5, 17, 3, 3, 90, 0).L(19.5, 9)
    return cat(lid, box, Path().M(10, 12.5).L(14, 12.5))


@icon("types")
def type_build():
    # Hammer: u runs down-left along the handle, head at the top-right.
    f = lambda u, v: (12 + (-u + v) * 0.7071, 12 + (u + v) * 0.7071)
    head = Path().rpoly([f(-9.6, -7.4), f(-4.8, -7.4), f(-4.8, 4.0), f(-9.6, 4.0)], 1.4)
    handle = Path().M(*f(-4.8, -1.7)).L(*f(8.8, -1.7))
    return cat(head, handle).centered()
