"""Tiny path builder: absolute commands, sampled bbox, rigid transforms."""
import math

def fmt(v):
    v = round(v + 0.0, 2)
    if abs(v) < 0.005:
        v = 0.0
    s = f"{v:.2f}".rstrip("0").rstrip(".")
    if s == "-0":
        s = "0"
    return s


class Path:
    def __init__(self):
        self.segs = []  # (kind, data)

    def M(self, x, y):
        self.segs.append(("M", (x, y)))
        return self

    def L(self, x, y):
        self.segs.append(("L", (x, y)))
        return self

    def H(self, x):
        return self.L(x, self.cur()[1])

    def V(self, y):
        return self.L(self.cur()[0], y)

    def l(self, dx, dy):
        c = self.cur()
        return self.L(c[0] + dx, c[1] + dy)

    def C(self, x1, y1, x2, y2, x, y):
        self.segs.append(("C", (x1, y1, x2, y2, x, y)))
        return self

    def Z(self):
        self.segs.append(("Z", None))
        return self

    def cur(self):
        start = None
        pt = None
        for k, d in self.segs:
            if k == "M":
                start = d
                pt = d
            elif k == "L":
                pt = d
            elif k == "C":
                pt = (d[4], d[5])
            elif k == "A":
                pt = d["end"]
            elif k == "Z":
                pt = start
        return pt

    # Elliptic arc on centre (cx,cy), angles in degrees, clockwise-positive on
    # screen (y down). a1 > a0 sweeps clockwise.
    def arc(self, cx, cy, rx, ry, a0, a1, move=False):
        p0 = (cx + rx * math.cos(math.radians(a0)), cy + ry * math.sin(math.radians(a0)))
        if move:
            self.M(*p0)
        span = a1 - a0
        n = max(1, int(math.ceil(abs(span) / 179.0)))
        for i in range(n):
            b0 = a0 + span * i / n
            b1 = a0 + span * (i + 1) / n
            end = (cx + rx * math.cos(math.radians(b1)), cy + ry * math.sin(math.radians(b1)))
            self.segs.append(("A", {"c": (cx, cy), "rx": rx, "ry": ry, "rot": 0.0,
                                    "a0": b0, "a1": b1, "end": end}))
        return self

    def circle(self, cx, cy, r):
        self.M(cx - r, cy)
        self.arc(cx, cy, r, r, 180, 360)
        self.arc(cx, cy, r, r, 0, 180)
        return self.Z()

    def ellipse(self, cx, cy, rx, ry):
        self.M(cx - rx, cy)
        self.arc(cx, cy, rx, ry, 180, 360)
        self.arc(cx, cy, rx, ry, 0, 180)
        return self.Z()

    def dot(self, x, y):
        return self.M(x, y).L(x + 0.01, y)

    def rrect(self, x, y, w, h, r):
        r = min(r, w / 2, h / 2)
        self.M(x + r, y).L(x + w - r, y)
        self.arc(x + w - r, y + r, r, r, -90, 0)
        self.L(x + w, y + h - r)
        self.arc(x + w - r, y + h - r, r, r, 0, 90)
        self.L(x + r, y + h)
        self.arc(x + r, y + h - r, r, r, 90, 180)
        self.L(x, y + r)
        self.arc(x + r, y + r, r, r, 180, 270)
        return self.Z()

    # Closed polygon with a fillet radius per vertex (scalar or list).
    def rpoly(self, pts, r, closed=True):
        n = len(pts)
        rs = r if isinstance(r, (list, tuple)) else [r] * n
        corners = []
        for i in range(n):
            p = pts[i]
            if not closed and (i == 0 or i == n - 1):
                corners.append((p, p, None))
                continue
            a = pts[i - 1]
            b = pts[(i + 1) % n]
            u1 = norm((a[0] - p[0], a[1] - p[1]))
            u2 = norm((b[0] - p[0], b[1] - p[1]))
            cosang = max(-1, min(1, u1[0] * u2[0] + u1[1] * u2[1]))
            theta = math.acos(cosang)
            ri = rs[i]
            if ri <= 0 or theta < 1e-6:
                corners.append((p, p, None))
                continue
            t = ri / math.tan(theta / 2)
            t1 = (p[0] + u1[0] * t, p[1] + u1[1] * t)
            t2 = (p[0] + u2[0] * t, p[1] + u2[1] * t)
            bis = norm((u1[0] + u2[0], u1[1] + u2[1]))
            d = ri / math.sin(theta / 2)
            c = (p[0] + bis[0] * d, p[1] + bis[1] * d)
            corners.append((t1, t2, (c, ri)))
        first = corners[0][1] if closed else corners[0][0]
        self.M(*first)
        order = list(range(1, n)) + ([0] if closed else [])
        for i in order:
            t1, t2, fil = corners[i]
            self.L(*t1)
            if fil:
                (c, ri) = fil
                a0 = math.degrees(math.atan2(t1[1] - c[1], t1[0] - c[0]))
                a1 = math.degrees(math.atan2(t2[1] - c[1], t2[0] - c[0]))
                d = a1 - a0
                while d > 180:
                    d -= 360
                while d < -180:
                    d += 360
                self.arc(c[0], c[1], ri, ri, a0, a0 + d)
        if closed:
            self.Z()
        return self

    def add(self, other):
        self.segs.extend(other.segs)
        return self

    def transformed(self, fn, rot_deg=0.0, scale=1.0):
        out = Path()
        for k, d in self.segs:
            if k in ("M", "L"):
                out.segs.append((k, fn(*d)))
            elif k == "C":
                a = fn(d[0], d[1]); b = fn(d[2], d[3]); e = fn(d[4], d[5])
                out.segs.append(("C", (*a, *b, *e)))
            elif k == "A":
                nd = dict(d)
                nd["c"] = fn(*d["c"])
                nd["end"] = fn(*d["end"])
                nd["rx"] = d["rx"] * scale
                nd["ry"] = d["ry"] * scale
                nd["rot"] = d["rot"] + rot_deg
                nd["a0"] = d["a0"]
                nd["a1"] = d["a1"]
                out.segs.append(("A", nd))
            else:
                out.segs.append((k, d))
        return out

    def translate(self, dx, dy):
        return self.transformed(lambda x, y: (x + dx, y + dy))

    def scale_about(self, s, cx=12, cy=12):
        return self.transformed(lambda x, y: (cx + (x - cx) * s, cy + (y - cy) * s), scale=s)

    def rotate(self, deg, cx=12, cy=12):
        c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
        def f(x, y):
            dx, dy = x - cx, y - cy
            return (cx + dx * c - dy * s, cy + dx * s + dy * c)
        return self.transformed(f, rot_deg=deg)

    def samples(self):
        pts = []
        cur = None
        for k, d in self.segs:
            if k in ("M", "L"):
                pts.append(d); cur = d
            elif k == "C":
                p0 = cur
                for i in range(1, 17):
                    t = i / 16
                    mt = 1 - t
                    x = mt**3*p0[0] + 3*mt*mt*t*d[0] + 3*mt*t*t*d[2] + t**3*d[4]
                    y = mt**3*p0[1] + 3*mt*mt*t*d[1] + 3*mt*t*t*d[3] + t**3*d[5]
                    pts.append((x, y))
                cur = (d[4], d[5])
            elif k == "A":
                c = d["c"]; rot = math.radians(d["rot"])
                for i in range(0, 25):
                    a = math.radians(d["a0"] + (d["a1"] - d["a0"]) * i / 24)
                    ex, ey = d["rx"] * math.cos(a), d["ry"] * math.sin(a)
                    pts.append((c[0] + ex * math.cos(rot) - ey * math.sin(rot),
                                c[1] + ex * math.sin(rot) + ey * math.cos(rot)))
                cur = d["end"]
        return pts

    def bbox(self):
        pts = self.samples()
        xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
        return (min(xs), min(ys), max(xs), max(ys))

    def centered(self, cx=12, cy=12, axis="xy"):
        x0, y0, x1, y1 = self.bbox()
        dx = cx - (x0 + x1) / 2 if "x" in axis else 0
        dy = cy - (y0 + y1) / 2 if "y" in axis else 0
        return self.translate(dx, dy)

    def d(self):
        out = []
        for k, d in self.segs:
            if k == "M":
                out.append(f"M{fmt(d[0])} {fmt(d[1])}")
            elif k == "L":
                out.append(f"L{fmt(d[0])} {fmt(d[1])}")
            elif k == "C":
                out.append("C" + " ".join(fmt(v) for v in d))
            elif k == "A":
                span = d["a1"] - d["a0"]
                large = 1 if abs(span) > 180 else 0
                sweep = 1 if span > 0 else 0
                rx, ry = d["rx"], d["ry"]
                rot = d["rot"] % 180 if abs(rx - ry) > 1e-6 else 0
                out.append(f"A{fmt(rx)} {fmt(ry)} {fmt(rot)} {large} {sweep} {fmt(d['end'][0])} {fmt(d['end'][1])}")
            elif k == "Z":
                out.append("Z")
        return "".join(out)


def norm(v):
    l = math.hypot(*v)
    return (v[0] / l, v[1] / l)


def circle_intersections(c1, r1, c2, r2):
    dx, dy = c2[0] - c1[0], c2[1] - c1[1]
    d = math.hypot(dx, dy)
    a = (r1 * r1 - r2 * r2 + d * d) / (2 * d)
    h = math.sqrt(max(0, r1 * r1 - a * a))
    ux, uy = dx / d, dy / d
    px, py = c1[0] + a * ux, c1[1] + a * uy
    return (px + h * uy, py - h * ux), (px - h * uy, py + h * ux)


def angle(c, p):
    return math.degrees(math.atan2(p[1] - c[1], p[0] - c[0]))


def cat(*paths):
    out = Path()
    for p in paths:
        out.add(p)
    return out
