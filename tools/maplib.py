"""Shared loading for the map tools."""
import glob
import os
import re

import yaml

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
MAP_DIR = os.path.join(ROOT, "map", "ui-map")

# disposition keys a flag / part / class / api / font may use
DISPOSITIONS = {
    "entry": "handled as that entry",
    "mod": "SwiftUI modifier on the owning entry's view",
    "view": "SwiftUI view that replaces the entry's default",
    "appkit": "AppKit statements (where SwiftUI has no API)",
    "nscolor": "system NSColor",
    "nsfont": "system NSFont",
    "existing": "already native in winemac today",
    "behavior": "handled by the bridge; nothing to draw",
    "ignore": "no macOS counterpart needed",
    "not_translated": "stays drawn by wine",
}
CODE_KEYS = ("mod", "view", "appkit", "nscolor", "nsfont")
RUNTIMES = ("swiftui", "appkit", "existing", "not_translated")


def load_yaml(path):
    with open(path) as f:
        return yaml.safe_load(f)


def inventories():
    return (load_yaml(os.path.join(ROOT, "map", "win32-inventory.yaml")),
            load_yaml(os.path.join(ROOT, "map", "swiftui-inventory.yaml")))


def load_map():
    """Merge map/ui-map/*.yaml, remembering which file each item came from."""
    merged = {"entries": [], "flags": {}, "parts": {}, "classes": {}, "apis": {}, "fonts": {}, "icons": {}}
    origin = {}
    for path in sorted(glob.glob(os.path.join(MAP_DIR, "*.yaml"))):
        doc = load_yaml(path) or {}
        name = os.path.basename(path)
        for e in doc.get("entries", []) or []:
            e["_file"] = name
            merged["entries"].append(e)
        for sect in ("flags", "parts", "classes", "apis", "fonts", "icons"):
            for k, v in (doc.get(sect) or {}).items():
                if k in merged[sect]:
                    raise SystemExit(f"{name}: {sect}.{k} already set in {origin[(sect, k)]}")
                merged[sect][k] = v
                origin[(sect, k)] = name
    return merged, origin


# what a stock icon (map section `icons`) may become
ICON_KINDS = ("sf", "uttype", "file", "nsimage", "app")
ICON_COLORS = ("white", "black", "systemBlue", "systemRed", "systemYellow", "systemOrange", "systemGreen",
               "systemGray", "labelColor", "secondaryLabelColor", "controlAccentColor")


def icon_spec(d):
    """An icon's disposition as the runtime's spec string: kind:value[;option]."""
    kind = next(k for k in ICON_KINDS if k in d)
    spec = f"{kind}:{d[kind]}"
    if d.get("multicolor"):
        spec += ";multicolor"
    elif d.get("palette"):
        spec += ";palette=" + ",".join(d["palette"])
    return spec


def aslist(x):
    if x is None:
        return []
    return x if isinstance(x, list) else [x]


def vkey(v):
    parts = [int(p) for p in str(v).split(".") if p.isdigit()] or [0]
    return tuple((parts + [0, 0])[:3])


def snippets(m):
    """Every (where, kind, since, code) the map asks the compiler to check."""
    out = []
    for e in m["entries"]:
        for i, s in enumerate(aslist(e.get("swiftui"))):
            out.append((f"{e['id']}.swiftui[{i}]", "view", s.get("since", 12), s["code"]))
        for i, s in enumerate(aslist(e.get("appkit"))):
            out.append((f"{e['id']}.appkit[{i}]", "appkit", s.get("since", 12), s["code"]))
    for sect in ("flags", "parts", "classes", "apis", "fonts"):
        for k, d in m[sect].items():
            for key in CODE_KEYS:
                if key in d:
                    # `since` belongs to mod/view; an appkit item is the macOS 12 fallback unless appkit_since says otherwise
                    since = d.get("since", 12) if key in ("mod", "view") else d.get("appkit_since", 12) if key == "appkit" else 12
                    out.append((f"{sect}.{k}.{key}", key, since, d[key]))
    return out


IDENT = re.compile(r"(?<![\w.])\.([a-z]\w*)\s*\(|\b([A-Z]\w+)\b")
