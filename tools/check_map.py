#!/usr/bin/env python3
"""Gate: the map must cover every Windows UI element in the inventory.

Checks that every window class, style/flag bit, dialog entry point, vsstyle.h
part and non-client font is either an entry or has a disposition; that every
entry has a macOS 12 path; that nothing cites a SwiftUI/AppKit symbol at a
tier below the macOS version that introduced it; that every name the map
mentions exists in the inventory; and that every stock icon has one macOS
image, its SF Symbol available at macOS 12.
"""
import re
import sys
from collections import Counter

from maplib import DISPOSITIONS, ICON_COLORS, ICON_KINDS, IDENT, RUNTIMES, aslist, inventories, load_map, snippets, vkey

errors = []

SF_AVAILABILITY = "/System/Library/CoreServices/CoreGlyphs.bundle/Contents/Resources/name_availability.plist"


def sf_symbols():
    """SF Symbol name -> the macOS version that introduced it, from the system's own
    list (None where there is none, e.g. on Linux)."""
    import plistlib
    try:
        with open(SF_AVAILABILITY, "rb") as f:
            d = plistlib.load(f)
    except OSError:
        return None
    releases = {k: v["macOS"] for k, v in d["year_to_release"].items()}
    return {n: releases.get(y, "99") for n, y in d["symbols"].items()}


def err(msg):
    errors.append(msg)


def main():
    w32, sw = inventories()
    m, origin = load_map()
    entries = {}
    for e in m["entries"]:
        if e["id"] in entries:
            err(f"duplicate entry id {e['id']}")
        entries[e["id"]] = e

    # ---- what the inventory says exists
    flag_owner, flag_value, flag_group = {}, {}, {}
    for g in w32["groups"]:
        if g["kind"] in ("style", "exstyle", "flag"):
            for n, v in g["names"].items():
                flag_owner[n] = g["owner"]
                flag_value[n] = v
                flag_group[n] = g["prefix"]
    classes = {c["name"] for c in w32["classes"]}
    apis = {a["name"] for a in w32["apis"]}
    parts = {f"{c}/{p}" for c, ps in w32["visual"]["parts"].items() for p in ps}
    fonts = set(w32["nonclient_fonts"])

    # ---- entries
    claimed = {"flag": {}, "class": {}, "api": {}, "part": {}}
    for e in m["entries"]:
        eid = e["id"]
        for k in ("id", "kind", "title", "runtime"):
            if k not in e:
                err(f"{eid}: missing {k}")
        rt = e.get("runtime")
        if rt not in RUNTIMES:
            err(f"{eid}: runtime must be one of {RUNTIMES}")
        w = e.get("win32") or {}
        for kind, key, universe in (("flag", "styles", flag_owner), ("class", "class", classes),
                                    ("api", "apis", apis), ("part", "parts", parts)):
            for n in aslist(w.get(key)):
                if n not in universe:
                    err(f"{eid}: unknown {kind} {n}")
                elif kind != "class" and n in claimed[kind]:
                    err(f"{eid}: {kind} {n} already claimed by {claimed[kind][n]}")
                claimed[kind].setdefault(n, eid)
        floors = [s for s in aslist(e.get("swiftui")) + aslist(e.get("appkit")) if float(s.get("since", 12)) <= 12]
        if rt in ("swiftui", "appkit") and not floors:
            err(f"{eid}: no macOS 12 path (needs a swiftui or appkit item with since: 12)")
        if rt == "swiftui" and not aslist(e.get("swiftui")):
            err(f"{eid}: runtime swiftui without swiftui code")
        if rt == "appkit" and not any(float(s.get("since", 12)) <= 12 for s in aslist(e.get("appkit"))):
            err(f"{eid}: runtime appkit without a macOS 12 appkit path")
        if rt in ("existing", "not_translated") and not e.get("notes"):
            err(f"{eid}: {rt} needs notes saying where/why")
        if e.get("glass") not in ("explicit", "automatic", "none"):
            err(f"{eid}: glass must be explicit | automatic | none")
        if e.get("glass") == "explicit" and not any(re.search(r"glass|Glass", s["code"]) for s in aslist(e.get("swiftui")) + aslist(e.get("appkit"))):
            err(f"{eid}: glass: explicit but no glass API in its code")
        # state_in drives when the runtime re-reads the control: a query there
        # re-reads on every read, forever. Queries the control must answer go in `answers`.
        for m_ in aslist(e.get("state_in")):
            if re.match(r"[A-Z]+_(GET|.*FROMCHAR|LINEINDEX|ADJUSTRECT|HITTEST)", str(m_)):
                err(f"{eid}: state_in {m_} is a query; list it under answers")
        for n in aslist(e.get("not_translated_when")):
            if n not in flag_owner:
                err(f"{eid}: not_translated_when names unknown flag {n}")

    # ---- dispositions
    def check_disp(sect, name, d):
        if not isinstance(d, dict) or not d:
            err(f"{sect}.{name}: disposition must be a mapping")
            return
        keys = [k for k in d if k in DISPOSITIONS]
        extra = [k for k in d if k not in DISPOSITIONS and k not in ("since", "appkit_since", "fallback", "note")]
        if len(keys) < 1 or extra:
            err(f"{sect}.{name}: needs one of {list(DISPOSITIONS)} (got {list(d)})")
        if "entry" in d and d["entry"] not in entries:
            err(f"{sect}.{name}: unknown entry {d['entry']}")
        if float(d.get("since", 12)) > 12 and not any(k in d for k in ("fallback", "appkit")):
            err(f"{sect}.{name}: since {d['since']} needs a fallback for macOS 12")

    for sect, universe, kind in (("flags", flag_owner, "flag"), ("classes", classes, "class"),
                                 ("apis", apis, "api"), ("parts", parts, "part"), ("fonts", fonts, None)):
        for name, d in m[sect].items():
            if name not in universe:
                err(f"{origin[(sect, name)]}: {sect}.{name} is not in the inventory")
            if kind and name in claimed[kind]:
                err(f"{sect}.{name}: also claimed by entry {claimed[kind][name]}")
            check_disp(sect, name, d)

    # ---- coverage
    covered = set(claimed["flag"]) | set(m["flags"])
    auto = {}
    changed = True
    while changed:  # masks, aliases and composites resolve through what is covered
        changed = False
        for n, v in flag_value.items():
            if n in covered or n in auto:
                continue
            toks = re.findall(r"[A-Z][A-Z0-9_]+", v)
            if "MASK" in n:
                auto[n] = "mask"
            elif toks and all(t in covered or t in auto for t in toks) and all(t in flag_value for t in toks):
                auto[n] = "alias of " + " | ".join(toks) if len(toks) == 1 else "composite of " + " | ".join(toks)
            else:
                continue
            changed = True
    missing = sorted(set(flag_owner) - covered - set(auto))
    for n in missing:
        err(f"uncovered flag {n} ({flag_owner[n]} {flag_value[n]})")
    for n in sorted(classes - set(claimed["class"]) - set(m["classes"])):
        err(f"uncovered class {n}")
    for n in sorted(apis - set(claimed["api"]) - set(m["apis"])):
        err(f"uncovered entry point {n}")
    for n in sorted(parts - set(claimed["part"]) - set(m["parts"])):
        err(f"uncovered visual part {n}")
    for n in sorted(fonts - set(m["fonts"])):
        err(f"uncovered font {n}")

    # ---- availability of cited symbols (the typecheck is exact; this catches tier slips early)
    known = {}
    for sect in ("views", "types"):
        for n, v in sw[sect].items():
            known[n] = v["macos"]
    for n, v in sw["modifiers"].items():
        known["." + n] = v["macos"]
    for path, v in sw["members"].items():  # Shape.fill, Text.font ... share names with View modifiers
        k = "." + path.split(".", 1)[1]
        if k not in known or vkey(v) < vkey(known[k]):
            known[k] = v
    for proto, members in sw["styles"].items():
        for n, v in members.items():
            known.setdefault(n, v["macos"])
            if vkey(v["macos"]) < vkey(known[n]):
                known[n] = v["macos"]
    for n, v in sw["appkit"].items():
        known[n] = v["macos"]
    for n, v in sw["usernotifications"].items():
        known[n] = v["macos"]
    for where, kind, since, code in snippets(m):
        for mod, typ in IDENT.findall(code):
            sym = "." + mod if mod else typ
            if sym in known and vkey(known[sym]) > vkey(since):
                err(f"{where}: {sym} needs macOS {known[sym]} but is placed at {since}")

    # ---- stock icons: one macOS image each, SF Symbols that exist at the macOS 12 floor
    symbols = sf_symbols()
    for k, d in m["icons"].items():
        if not re.fullmatch(r"(user32|shell32)/[A-Z0-9_]+", k):
            err(f"icons.{k}: key is <user32|shell32>/<resource name>")
        kinds = [x for x in ICON_KINDS if x in d]
        if len(kinds) != 1:
            err(f"icons.{k}: needs exactly one of {', '.join(ICON_KINDS)}")
        for c in aslist(d.get("palette")):
            if c not in ICON_COLORS:
                err(f"icons.{k}: palette colour {c} isn't one of {', '.join(ICON_COLORS)}")
        if "sf" in d and symbols is not None:
            if d["sf"] not in symbols:
                err(f"icons.{k}: no SF Symbol named {d['sf']}")
            elif vkey(symbols[d["sf"]]) > vkey(12):
                err(f"icons.{k}: SF Symbol {d['sf']} needs macOS {symbols[d['sf']]}")

    # ---- report
    kinds = Counter(e["kind"] for e in m["entries"])
    runtimes = Counter(e["runtime"] for e in m["entries"])
    disp = Counter(k for sect in ("flags", "parts", "classes", "apis", "fonts") for d in m[sect].values() for k in d if k in DISPOSITIONS)
    improvised = sum(1 for e in m["entries"] if e.get("improvised"))
    tiers = Counter(str(s.get("since", 12)) for e in m["entries"] for s in aslist(e.get("swiftui")) + aslist(e.get("appkit")))
    print(f"entries: {len(m['entries'])} ({dict(kinds)}); runtime {dict(runtimes)}; improvised {improvised}")
    print(f"code per macOS tier: {dict(sorted(tiers.items(), key=lambda t: vkey(t[0])))}")
    print(f"flags: {len(flag_owner)} = {len(claimed['flag'])} as entry variants + {len(m['flags'])} dispositions + {len(auto)} masks/aliases/composites")
    print(f"dispositions by kind: {dict(disp)}")
    print(f"classes {len(classes)}, entry points {len(apis)}, visual parts {len(parts)}, fonts {len(fonts)}")
    print(f"stock icons: {len(m['icons'])} ({dict(Counter(k for d in m['icons'].values() for k in ICON_KINDS if k in d))})"
          + ("" if symbols is not None else "; SF Symbol names not checked (no CoreGlyphs here)"))
    if errors:
        for e in errors[:400]:
            print("ERROR", e)
        print(f"{len(errors)} errors")
        sys.exit(1)
    print("map OK: every inventory item is covered")


if __name__ == "__main__":
    main()
