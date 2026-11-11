"""Regenerates assets/icons from the geometry and motion sources here.

    python tools/icons/generate.py
    python tools/icons/generate.py --preview .dev/icon-motion.html

Static icons become one-path SVGs. Animated icons (motion.py) become SVGs whose
visible parts are id'd <path> elements in paint order plus a <defs> block for
geometry only the motion uses, and a sibling <name>.motion.json. Formats and
rules: docs/ui/icons.md.
"""
import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True  # keep tools/icons free of __pycache__

import icons as I  # noqa: E402
import motion as M  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
ASSETS = os.path.join(ROOT, "assets", "icons")

STROKE = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" '
          'stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">')
FILLED = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="currentColor" stroke="none">'
PROPS = {"rotate", "tx", "ty", "scale", "sx", "sy", "opacity", "draw", "morph", "tint", "fill"}
EASES = {"l", "i", "o", "io", "back"}
TINTS = {"accent", "danger", "success", "warning", "cursor"}


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)


def num(v):
    v = round(float(v), 4)
    return int(v) if v == int(v) else v


def motion_files(spec):
    """(svg text, motion json text) for one animated icon."""
    ids = {}          # d -> svg id
    visible, defs = [], []
    for p in spec["parts"]:
        if p["d"] and not p.get("extra"):
            ids.setdefault(p["d"], p["id"])
            visible.append(f'  <path id="{p["id"]}" d="{p["d"]}"/>')
    parts = []
    for p in spec["parts"]:
        e = {"id": p["id"]}
        if p["d"]:
            if p["d"] not in ids:
                ids[p["d"]] = p["id"]
                defs.append(f'    <path id="{p["id"]}" d="{p["d"]}"/>')
            e["path"] = ids[p["d"]]
        if "to" in p:
            tid = p["id"] + "-to"
            defs.append(f'    <path id="{tid}" d="{p["to"]}"/>')
            e["morph"] = tid
        e["origin"] = [num(p["origin"][0]), num(p["origin"][1])]
        for k in ("parent", "wrap", "length"):
            if k in p:
                e[k] = num(p[k]) if k != "parent" else p[k]
        if p.get("fill"):
            e["solid"] = True
        if p.get("tint"):
            assert p["tint"] in TINTS, p["tint"]
            e["tint"] = p["tint"]
        if p.get("rest"):
            e["rest"] = {k: num(v) for k, v in p["rest"].items()}
        parts.append(e)
    svg = [FILLED if spec["filled"] else STROKE] + visible
    if defs:
        svg += ["  <defs>"] + defs + ["  </defs>"]
    svg.append("</svg>")

    def track(t):
        assert t["prop"] in PROPS, t["prop"]
        out = {"part": t["part"], "prop": t["prop"], "dur": num(t["dur"])}
        if t["delay"]:
            out["delay"] = num(t["delay"])
        if t["loop"]:
            out["loop"] = True
        keys = []
        for k in t["keys"]:
            if len(k) > 2:
                assert k[2] in EASES, k[2]
            keys.append([num(k[0]), num(k[1])] + list(k[2:]))
        out["keys"] = keys
        return out

    def block(key, items, last=False):
        body = ",\n".join("    " + json.dumps(i, ensure_ascii=False, separators=(", ", ": ")) for i in items)
        return f'  "{key}": [\n{body}\n  ]' + ("" if last else ",") if items else f'  "{key}": []' + ("" if last else ",")

    js = "{\n" + "\n".join([block("parts", parts), block("hover", [track(t) for t in spec["hover"]]),
                            block("press", [track(t) for t in spec["press"]]),
                            block("active", [track(t) for t in spec["active"]], last=True)]) + "\n}\n"
    return "\n".join(svg) + "\n", js


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", help="also write an interactive motion preview page here")
    args = ap.parse_args()

    specs = M.build()
    written = set()
    for key, (folder, fn, filled) in sorted(I.ICONS.items()):
        name = key.rstrip("-")
        base = os.path.join(ASSETS, folder, name)
        if name in specs:
            svg, js = motion_files(specs[name])
            write(base + ".svg", svg)
            write(base + ".motion.json", js)
            written.add(os.path.normcase(base + ".motion.json"))
        else:
            d = fn().d()
            write(base + ".svg", f'{FILLED if filled else STROKE}\n  <path d="{d}"/>\n</svg>\n')
    for stale in glob.glob(os.path.join(ASSETS, "**", "*.motion.json"), recursive=True):
        if os.path.normcase(stale) not in written:
            os.remove(stale)
    print(f"{len(I.ICONS)} icons, {len(specs)} animated")

    if args.preview:
        tpl = open(os.path.join(HERE, "preview_tpl.html"), encoding="utf-8").read()
        data = json.dumps(specs, ensure_ascii=False)
        html = tpl.replace("/*SPECS*/", data).replace("/*GROUPS*/", json.dumps(M.GROUPS, ensure_ascii=False))
        write(os.path.abspath(args.preview), html)
        print("preview:", os.path.abspath(args.preview))


if __name__ == "__main__":
    main()
