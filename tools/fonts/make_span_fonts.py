"""Turn the Geist Mono and Figtree releases into SpanTui's bundled fonts.

Invoked by tools/fonts/build-fonts.ps1. Span Mono (from Geist Mono) and
Span Sans (from Figtree) get every upstream weight, each with an italic;
every face is renamed into a single family and gets the stylistic sets listed
in FAMILIES frozen into its cmap, so the alternates show up without OpenType
feature support. Span Mono faces also get SpanTui's own Powerline separators
(U+E0B0..U+E0BF, drawn to the exact cell) and a replacement character
(U+FFFD) built from the face's own "?". The Nerd Fonts icons go into one
shared Span Symbols face with Span Mono's cell, used as a terminal fallback,
instead of being copied into all eighteen Span Mono faces.
"""

import argparse
import math
import sys
from pathlib import Path

from fontTools import subset
from fontTools.pens.areaPen import AreaPen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.pens.cu2quPen import Cu2QuPen
from fontTools.pens.recordingPen import DecomposingRecordingPen, RecordingPen
from fontTools.pens.reverseContourPen import ReverseContourPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._c_m_a_p import cmap_format_4, cmap_format_12


def styles(weights):
    return list(weights) + ["Italic" if w == "Regular" else w + "Italic" for w in weights]


class Family:
    def __init__(self, name, upstream, source, output, weights, features):
        self.name = name
        self.upstream = upstream
        self.source = source
        self.output = output
        self.styles = styles(weights)
        self.features = features
        self.postscript_name = name.replace(" ", "")
        self.upstream_postscript = upstream.replace(" ", "")

    def renamed(self, name_id, text):
        if name_id in POSTSCRIPT_NAME_IDS:
            return text.replace(self.upstream, self.postscript_name).replace(
                self.upstream_postscript, self.postscript_name)
        return text.replace(self.upstream, self.name)


FAMILIES = {
    # Span Mono: ss03 tailed l, ss08 round dots — the rounded, SF Mono-like look.
    "mono": Family("Span Mono", "Geist Mono", "GeistMono/ttf/GeistMono-%s.ttf",
                   "SpanMono-%s.ttf",
                   ("Thin", "ExtraLight", "Light", "Regular", "Medium",
                    "SemiBold", "Bold", "ExtraBold", "Black"),
                   ("ss03", "ss08")),
    # Span Sans: Figtree with ss01 single-storey a and ss02 disambiguation
    # (serifed I, tailed l like Span Mono, slashed 0).
    "sans": Family("Span Sans", "Figtree", "fonts/ttf/Figtree-%s.ttf",
                   "SpanSans-%s.ttf",
                   ("Light", "Regular", "Medium", "SemiBold", "Bold",
                    "ExtraBold", "Black"),
                   ("ss01", "ss02")),
}
SYMBOLS_SOURCE = "GeistMono/ttf/GeistMono-Regular.ttf"
SYMBOLS_OUTPUT = "SpanSymbols-Regular.ttf"
# Name IDs that carry the family name; copyright, vendor and license stay.
FAMILY_NAME_IDS = (1, 3, 4, 6, 16, 17, 18, 21, 22, 25)
# Unique ID, PostScript name and variations PostScript prefix: no spaces.
POSTSCRIPT_NAME_IDS = (3, 6, 25)

# Bezier constant for a quarter ellipse.
KAPPA = 0.5522847498
# Flat sides of filled separators bleed past the cell so adjacent cells of the
# same colour meet without a hairline seam (Nerd Fonts uses a similar bleed).
FLAT_BLEED = 30
VERTICAL_BLEED = 10

OWN_POWERLINE = range(0xE0B0, 0xE0C0)
# Powerline extras stretch to fill the whole cell instead of keeping aspect.
CELL_FILLING = range(0xE0C0, 0xE0D8)
# Powerline symbols (branch, line number, lock) are sized to the line height
# like in Nerd Fonts, not shrunk to a cell-wide square like regular icons.
LINE_HEIGHT_SYMBOLS = range(0xE0A0, 0xE0A4)

REPLACEMENT_CHARACTER = 0xFFFD
# The diamond keeps this side bearing and reaches this share of the line
# height above and below its centre (a little past the cap height and below
# the baseline); the knocked-out "?" fills this share of its inscribed box.
REPLACEMENT_SIDE_BEARING = 20
REPLACEMENT_HALF_HEIGHT = 0.37
REPLACEMENT_MARK_FILL = 0.78


class Cell:
    """The terminal cell: advance of "0" by the typo line box (USE_TYPO_METRICS)."""

    def __init__(self, font):
        os2 = font["OS/2"]
        self.width = font["hmtx"][font.getBestCmap()[ord("0")]][0]
        self.top = os2.sTypoAscender
        self.bottom = os2.sTypoDescender
        self.height = self.top - self.bottom
        self.mid = (self.top + self.bottom) / 2
        # Separator stroke follows the face weight: 400 -> 70, 700 -> 115.
        self.stroke = 70 + (os2.usWeightClass - 400) * 0.15


def rename(font, family):
    for record in font["name"].names:
        if record.nameID not in FAMILY_NAME_IDS:
            continue
        record.string = family.renamed(record.nameID, record.toUnicode())
    unify_family(font["name"])
    font["name"].setName("%s: a customized build of %s for SpanTui."
                         % (family.name, family.upstream), 10, 3, 1, 0x409)


def unify_family(name_table):
    """Give every weight the same legacy family name (ID 1).

    Static faces outside Regular/Italic/Bold/Bold Italic carry the weight
    in ID 1 ("Figtree Medium"), and Qt registers that Win32 name as an
    extra family next to the typographic one. Moving the typographic names
    (IDs 16/17) into IDs 1/2 leaves one family with real weights.
    """
    for record in list(name_table.names):
        if record.nameID in (16, 17):
            name_table.setName(record.toUnicode(), record.nameID - 15,
                               record.platformID, record.platEncID, record.langID)
    name_table.removeNames(nameID=16)
    name_table.removeNames(nameID=17)


def single_substitutions(font, tags):
    gsub = font["GSUB"].table
    mapping = {}
    for record in gsub.FeatureList.FeatureRecord:
        if record.FeatureTag not in tags:
            continue
        for index in record.Feature.LookupListIndex:
            lookup = gsub.LookupList.Lookup[index]
            for subtable in lookup.SubTable:
                if lookup.LookupType == 7:
                    subtable = subtable.ExtSubTable
                mapping.update(getattr(subtable, "mapping", {}))
    return mapping


def freeze_features(font, tags):
    mapping = single_substitutions(font, tags)
    for table in font["cmap"].tables:
        if hasattr(table, "cmap"):
            table.cmap = {cp: mapping.get(name, name) for cp, name in table.cmap.items()}


def ellipse_arc(pen, cx, cy, rx, ry, start_deg, end_deg):
    """Append quarter-step cubic arcs from start to end (multiples of 90)."""
    step = 90 if end_deg > start_deg else -90
    angle = start_deg
    while angle != end_deg:
        a0 = math.radians(angle)
        a1 = math.radians(angle + step)
        sign = 1 if step > 0 else -1
        p0 = (cx + rx * math.cos(a0), cy + ry * math.sin(a0))
        p3 = (cx + rx * math.cos(a1), cy + ry * math.sin(a1))
        t0 = (-math.sin(a0) * sign, math.cos(a0) * sign)
        t1 = (-math.sin(a1) * sign, math.cos(a1) * sign)
        p1 = (p0[0] + KAPPA * rx * t0[0], p0[1] + KAPPA * ry * t0[1])
        p2 = (p3[0] - KAPPA * rx * t1[0], p3[1] - KAPPA * ry * t1[1])
        pen.curveTo(p1, p2, p3)
        angle += step


def half_disc(pen, cell):
    """Right-facing semicircle whose flat side is the cell's left edge."""
    w, mid = cell.width, cell.mid
    ry = cell.height / 2 + VERTICAL_BLEED
    pen.moveTo((-FLAT_BLEED, mid - ry))
    pen.lineTo((0, mid - ry))
    ellipse_arc(pen, 0, mid, w, ry, -90, 90)
    pen.lineTo((-FLAT_BLEED, mid + ry))
    pen.closePath()


def half_ring(pen, cell):
    """Outline of the semicircle: the arc only, without the flat side."""
    w, mid, t = cell.width, cell.mid, cell.stroke
    ry = cell.height / 2
    pen.moveTo((0, mid - ry))
    ellipse_arc(pen, 0, mid, w, ry, -90, 90)
    pen.lineTo((0, mid + ry - t))
    ellipse_arc(pen, 0, mid, w - t, ry - t, 90, -90)
    pen.closePath()


def polygon(pen, points):
    pen.moveTo(points[0])
    for point in points[1:]:
        pen.lineTo(point)
    pen.closePath()


def diagonal(pen, cell, rising):
    w, top, bottom, t = cell.width, cell.top, cell.bottom, cell.stroke
    tx = t * math.hypot(w, cell.height) / cell.height
    if rising:
        polygon(pen, [(0, bottom), (tx, bottom), (w, top), (w - tx, top)])
    else:
        polygon(pen, [(0, top), (tx, top), (w, bottom), (w - tx, bottom)])


def mirrored(draw):
    """Mirror a right-facing drawing horizontally inside the cell."""

    def wrapper(pen, cell):
        recording = RecordingPen()
        draw(recording, cell)
        recording.replay(TransformPen(pen, (-1, 0, 0, 1, cell.width, 0)))

    return wrapper


def powerline_drawers():
    b = VERTICAL_BLEED

    def lower_left(pen, cell):
        polygon(pen, [(-FLAT_BLEED, cell.bottom - b), (cell.width, cell.bottom - b),
                      (-FLAT_BLEED, cell.top + b)])

    def upper_left(pen, cell):
        polygon(pen, [(-FLAT_BLEED, cell.top + b), (-FLAT_BLEED, cell.bottom - b),
                      (cell.width, cell.top + b)])

    return {
        # SpanTui's default separators are rounded: the classic arrow code
        # points share the semicircle design of U+E0B4..U+E0B7.
        0xE0B0: half_disc,
        0xE0B1: half_ring,
        0xE0B2: mirrored(half_disc),
        0xE0B3: mirrored(half_ring),
        0xE0B4: half_disc,
        0xE0B5: half_ring,
        0xE0B6: mirrored(half_disc),
        0xE0B7: mirrored(half_ring),
        0xE0B8: lower_left,
        0xE0B9: lambda pen, cell: diagonal(pen, cell, False),
        0xE0BA: mirrored(lower_left),
        0xE0BB: lambda pen, cell: diagonal(pen, cell, True),
        0xE0BC: upper_left,
        0xE0BD: lambda pen, cell: diagonal(pen, cell, True),
        0xE0BE: mirrored(upper_left),
        0xE0BF: lambda pen, cell: diagonal(pen, cell, False),
    }


def to_tt_glyph(recording):
    """Convert recorded cubic outlines into a clockwise TrueType glyph."""
    area = AreaPen()
    recording.replay(area)
    tt_pen = TTGlyphPen(None)
    target = Cu2QuPen(tt_pen, max_err=0.5, reverse_direction=False)
    # TrueType outer contours run clockwise, which AreaPen reports as negative.
    if area.value > 0:
        target = ReverseContourPen(target)
    recording.replay(target)
    return tt_pen.glyph()


def add_glyph(font, name, glyph, advance):
    glyf = font["glyf"]
    order = font.getGlyphOrder()
    if name not in glyf.glyphs:
        order.append(name)
        font.setGlyphOrder(order)
    glyf.glyphs[name] = glyph
    glyf.glyphOrder = font.getGlyphOrder()
    glyph.recalcBounds(glyf)
    font["hmtx"][name] = (advance, getattr(glyph, "xMin", 0))


def icon_transform(cp, source_cell, cell, ink_bounds):
    src_w, src_bottom, src_h = source_cell
    src_mid = src_bottom + src_h / 2
    if cp in LINE_HEIGHT_SYMBOLS and ink_bounds:
        ink_left, _, ink_right, _ = ink_bounds
        scale = min(cell.height / src_h, 0.92 * cell.width / (ink_right - ink_left))
        dx = cell.width / 2 - (ink_left + ink_right) / 2 * scale
        return (scale, 0, 0, scale, dx, cell.mid - src_mid * scale)
    if cp in CELL_FILLING:
        sx = cell.width / src_w
        sy = cell.height / src_h
        return (sx, 0, 0, sy, 0, cell.bottom - src_bottom * sy)
    scale = cell.width / src_w
    return (scale, 0, 0, scale, 0, cell.mid - src_mid * scale)


def merge_icons(font, cell, symbols, cmap):
    source_cmap = symbols.getBestCmap()
    source_glyphs = symbols.getGlyphSet()
    src_os2 = symbols["OS/2"]
    src_bottom = -src_os2.usWinDescent
    source_cell = (symbols["hmtx"][source_cmap[0xF07B]][0],
                   src_bottom, src_os2.usWinAscent - src_bottom)
    copied = 0
    for cp, source_name in sorted(source_cmap.items()):
        if cp in cmap or cp in OWN_POWERLINE:
            continue
        recording = DecomposingRecordingPen(source_glyphs)
        source_glyphs[source_name].draw(recording)
        bounds = BoundsPen(source_glyphs)
        recording.replay(bounds)
        transform = icon_transform(cp, source_cell, cell, bounds.bounds)
        tt_pen = TTGlyphPen(None)
        recording.replay(TransformPen(tt_pen, transform))
        name = "nf.%s" % source_name
        add_glyph(font, name, tt_pen.glyph(), cell.width)
        cmap[cp] = name
        copied += 1
    return copied


def draw_powerline(font, cell, cmap):
    for cp, draw in powerline_drawers().items():
        recording = RecordingPen()
        draw(recording, cell)
        name = "span.powerline.%04X" % cp
        add_glyph(font, name, to_tt_glyph(recording), cell.width)
        cmap[cp] = name


def draw_replacement_character(font, cell, cmap):
    """U+FFFD: a filled diamond with the face's own "?" knocked out.

    Geist Mono has no replacement character, so every undecodable byte in
    terminal output made Qt try all system fonts for one. The diamond follows
    the face's italic angle; the "?" brings the face's weight and slant.
    """
    glyphs = font.getGlyphSet()
    question = DecomposingRecordingPen(glyphs)
    glyphs[cmap[ord("?")]].draw(question)
    bounds = BoundsPen(None)
    question.replay(bounds)
    left, bottom, right, top = bounds.bounds

    cx = cell.width / 2
    cy = font["OS/2"].sCapHeight / 2
    half_width = cell.width / 2 - REPLACEMENT_SIDE_BEARING
    half_height = cell.height * REPLACEMENT_HALF_HEIGHT
    slant = math.tan(math.radians(-font["post"].italicAngle))
    tt_pen = TTGlyphPen(None)
    # Clockwise outer contour; the reversed "?" contours below cut holes.
    polygon(TransformPen(tt_pen, (1, 0, slant, 1, -slant * cy, 0)), [
        (cx, cy + half_height), (cx + half_width, cy),
        (cx, cy - half_height), (cx - half_width, cy)])

    # Largest "?" whose bounding box fits inside the diamond, then shrunk.
    scale = REPLACEMENT_MARK_FILL / ((right - left) / (2 * half_width)
                                     + (top - bottom) / (2 * half_height))
    transform = (scale, 0, 0, scale, cx - scale * (left + right) / 2,
                 cy - scale * (bottom + top) / 2)
    question.replay(ReverseContourPen(TransformPen(tt_pen, transform)))

    name = "uniFFFD"
    add_glyph(font, name, tt_pen.glyph(), cell.width)
    cmap[REPLACEMENT_CHARACTER] = name


def rebuild_cmap(font, cmap):
    bmp = {cp: name for cp, name in cmap.items() if cp <= 0xFFFF}
    tables = []
    for platform_id, encoding_id, fmt, mapping in (
        (0, 3, 4, bmp),
        (0, 4, 12, cmap),
        (3, 1, 4, bmp),
        (3, 10, 12, cmap),
    ):
        table = cmap_format_4(4) if fmt == 4 else cmap_format_12(12)
        table.platformID = platform_id
        table.platEncID = encoding_id
        table.language = 0
        table.cmap = dict(mapping)
        tables.append(table)
    new_cmap = newTable("cmap")
    new_cmap.tableVersion = 0
    new_cmap.tables = tables
    font["cmap"] = new_cmap


def process(source, output, family):
    font = TTFont(source)
    rename(font, family)
    if family.features:
        freeze_features(font, family.features)
    if family is FAMILIES["mono"]:
        cell = Cell(font)
        cmap = dict(font.getBestCmap())
        draw_powerline(font, cell, cmap)
        draw_replacement_character(font, cell, cmap)
        rebuild_cmap(font, cmap)
    font.save(output)
    print("%s: done" % output.name)


def build_symbols(source, output, symbols):
    """Span Symbols: Powerline and Nerd icons on Span Mono Regular's cell."""
    font = TTFont(source)
    cell = Cell(font)
    options = subset.Options()
    options.hinting = False
    options.layout_features = []
    # Keep Geist's copyright, designer and license records.
    options.name_IDs = [0, 7, 8, 9, 11, 12, 13, 14]
    options.notdef_outline = True
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=[0x20])
    subsetter.subset(font)
    cmap = dict(font.getBestCmap())
    draw_powerline(font, cell, cmap)
    copied = merge_icons(font, cell, symbols, cmap)
    rebuild_cmap(font, cmap)
    names = font["name"]
    for name_id, text in ((1, "Span Symbols"), (2, "Regular"),
                          (3, "SpanSymbols-Regular"), (4, "Span Symbols"),
                          (6, "SpanSymbols-Regular"),
                          (10, "Span Symbols: Nerd Fonts icons and SpanTui "
                               "Powerline separators on Span Mono's cell.")):
        names.setName(text, name_id, 3, 1, 0x409)
    font.save(output)
    print("%s: %d icons, cell %dx%d" % (output.name, copied, cell.width, cell.height))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--geist", required=True, type=Path,
                        help="the geist-font directory of the Geist release")
    parser.add_argument("--figtree", required=True, type=Path,
                        help="the root directory of the Figtree release")
    parser.add_argument("--symbols", required=True, type=Path,
                        help="SymbolsNerdFontMono-Regular.ttf")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    roots = {"mono": args.geist, "sans": args.figtree}
    for key, family in FAMILIES.items():
        for style in family.styles:
            process(roots[key] / (family.source % style),
                    args.output / (family.output % style), family)
    build_symbols(args.geist / SYMBOLS_SOURCE, args.output / SYMBOLS_OUTPUT,
                  TTFont(args.symbols))
    return 0


if __name__ == "__main__":
    sys.exit(main())
