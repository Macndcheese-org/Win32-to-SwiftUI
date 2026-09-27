#!/usr/bin/env python3
"""Inventory the SwiftUI (and needed AppKit) API of the installed macOS SDK.

Runs Apple's swift-symbolgraph-extract on SwiftUI, SwiftUICore, AppKit and
UserNotifications, then writes map/swiftui-inventory.yaml:
every public SwiftUI view, style (with its static members such as `.glass`),
view modifier and type, each with the macOS version that introduced it,
its deprecation, and its one-line doc; plus the AppKit classes the map uses.

usage: scan_swiftui.py [--cache DIR] [out.yaml]
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

import yaml

MODULES = ["SwiftUI", "SwiftUICore", "AppKit", "UserNotifications"]

# AppKit classes the map relies on where SwiftUI has no standalone equivalent,
# the views SwiftUI itself draws with, and the Liquid Glass views.
APPKIT_CLASSES = [
    "NSAlert", "NSOpenPanel", "NSSavePanel", "NSColorPanel", "NSColorWell", "NSFontPanel",
    "NSFontManager", "NSPrintPanel", "NSPageLayout", "NSPrintOperation", "NSComboBox",
    "NSTextView", "NSTextField", "NSSecureTextField", "NSSearchField", "NSTokenField",
    "NSScroller", "NSScrollView", "NSSlider", "NSStepper", "NSDatePicker", "NSProgressIndicator",
    "NSButton", "NSPopUpButton", "NSSegmentedControl", "NSSwitch", "NSBox", "NSImageView",
    "NSTableView", "NSOutlineView", "NSTableHeaderView", "NSCollectionView", "NSTabView",
    "NSTabViewController", "NSSplitView", "NSSplitViewController", "NSToolbar", "NSToolbarItem",
    "NSToolbarItemGroup", "NSMenu", "NSMenuItem", "NSStatusBar", "NSStatusItem", "NSDockTile",
    "NSPopover", "NSPanel", "NSWindow", "NSApplication", "NSWorkspace", "NSSound", "NSColor",
    "NSFont", "NSAppearance", "NSVisualEffectView", "NSGlassEffectView",
    "NSGlassEffectContainerView", "NSBackgroundExtensionView", "NSTextFinder", "NSPathControl",
    "NSLevelIndicator", "NSRuleEditor", "NSBrowser", "NSGridView", "NSStackView",
    "NSTitlebarAccessoryViewController", "NSHelpManager", "NSCursor",
]
UN_CLASSES = ["UNUserNotificationCenter", "UNMutableNotificationContent", "UNNotificationRequest"]


def extract(module, sdk, out_dir, target):
    subprocess.run(["xcrun", "swift-symbolgraph-extract", "-module-name", module, "-target", target,
                    "-sdk", sdk, "-output-dir", out_dir, "-minimum-access-level", "public",
                    "-skip-synthesized-members", "-skip-inherited-docs"], check=True)


def load(out_dir, module):
    syms, rels = {}, []
    for f in glob.glob(os.path.join(out_dir, module + ".symbols.json")) + \
            glob.glob(os.path.join(out_dir, module + "@*.symbols.json")):
        d = json.load(open(f))
        for s in d["symbols"]:
            syms[s["identifier"]["precise"]] = s
        rels += d["relationships"]
    return syms, rels


def macos(sym):
    """(introduced 'M.m' | None, deprecated 'M.m' | None, unavailable bool) for macOS."""
    intro = dep = None
    unavailable = False
    for a in sym.get("availability", []):
        if a.get("domain") not in ("macOS", "*"):
            continue
        if a.get("isUnconditionallyUnavailable") and a.get("domain") == "macOS":
            unavailable = True
        if a.get("isUnconditionallyDeprecated"):
            dep = "yes"
        if "introduced" in a and a.get("domain") == "macOS":
            v = a["introduced"]
            intro = f"{v['major']}.{v.get('minor', 0)}"
        if "deprecated" in a:
            v = a["deprecated"]
            if v["major"] < 100000:  # 100000 means "to be deprecated": not yet
                dep = f"{v['major']}.{v.get('minor', 0)}"
    return intro, dep, unavailable


def vkey(v):
    return tuple(int(x) for x in v.split(".")) if v else (0,)


def doc(sym):
    lines = sym.get("docComment", {}).get("lines") or []
    text = " ".join(l["text"].strip() for l in lines[:3]).strip()
    text = re.split(r"(?<=\.)\s", text)[0] if text else ""
    return text[:160]


def build(syms, rels, baseline):
    parent = {r["source"]: r["target"] for r in rels if r["kind"] == "memberOf"}
    conforms = {}
    for r in rels:
        if r["kind"] == "conformsTo":
            conforms.setdefault(r["source"], set()).add(r.get("targetFallback") or syms.get(r["target"], {}).get("names", {}).get("title", r["target"]))
        if r["kind"] == "inheritsFrom":
            conforms.setdefault(r["source"], set()).add(r.get("targetFallback") or r["target"])

    def eff(usr):
        """effective macOS availability, inheriting from the enclosing type or extension."""
        s = syms[usr]
        intro, dep, un = macos(s)
        p = parent.get(usr)
        seen = 0
        while intro is None and p in syms and seen < 8:
            pi, pd, pu = macos(syms[p])
            intro, dep, un = pi, dep or pd, un or pu
            p = parent.get(p)
            seen += 1
        return intro or baseline, dep, un

    types, views, styles, modifiers, members = {}, {}, {}, {}, {}
    for usr, s in syms.items():
        kind = s["kind"]["identifier"]
        path = s["pathComponents"]
        name = ".".join(path)
        if name.startswith("_") or any(p.startswith("_") for p in path):
            continue
        intro, dep, un = eff(usr)
        if un:
            continue
        entry = {"macos": intro}
        if dep:
            entry["deprecated"] = dep
        d = doc(s)
        if d:
            entry["doc"] = d
        if kind in ("swift.struct", "swift.class", "swift.enum", "swift.protocol") and len(path) == 1:
            types[name] = entry
            conf = {c.split(".")[-1] for c in conforms.get(usr, ())}
            if "View" in conf and kind != "swift.protocol":
                views[name] = entry
        elif kind == "swift.type.property" and len(path) == 2 and path[0].endswith("Style"):
            # `extension PickerStyle where Self == TabsPickerStyle { static var tabs }`
            styles.setdefault(path[0], {})["." + path[1]] = entry
        elif kind == "swift.method" and len(path) == 2 and path[0] in ("View", "Scene"):
            base = path[1].split("(")[0]
            m = modifiers.setdefault(base, {"macos": intro, "overloads": {}})
            m["overloads"][path[1]] = intro if not dep else f"{intro} (deprecated {dep})"
            if vkey(intro) < vkey(m["macos"]):
                m["macos"] = intro
            if "doc" not in m and d:
                m["doc"] = d
        elif len(path) == 2 and kind in ("swift.init", "swift.property", "swift.type.property",
                                         "swift.method", "swift.type.method", "swift.enum.case"):
            key = f"{path[0]}.{path[1].split('(')[0]}"
            old = members.get(key)
            if old is None or vkey(intro) < vkey(old):
                members[key] = intro
    for m in modifiers.values():
        m["overloads"] = dict(sorted(m["overloads"].items()))
    return (dict(sorted(types.items())), dict(sorted(views.items())),
            {k: dict(sorted(v.items())) for k, v in sorted(styles.items())},
            dict(sorted(modifiers.items())), dict(sorted(members.items())))


def appkit(syms, rels, wanted, baseline):
    parent = {r["source"]: r["target"] for r in rels if r["kind"] == "memberOf"}
    by_name = {}
    for usr, s in syms.items():
        by_name.setdefault(".".join(s["pathComponents"]), []).append(usr)
    out = {}
    for cls in wanted:
        usrs = by_name.get(cls)
        if not usrs:
            print(f"warning: {cls} not in the SDK", file=sys.stderr)
            continue
        intro, dep, un = macos(syms[usrs[0]])
        entry = {"macos": intro or baseline}
        if dep:
            entry["deprecated"] = dep
        d = doc(syms[usrs[0]])
        if d:
            entry["doc"] = d
        mem = {}
        for usr, s in syms.items():
            p = s["pathComponents"]
            if len(p) >= 2 and p[0] == cls and not p[-1].startswith("_"):
                mi, md, mu = macos(s)
                if mu:
                    continue
                key = ".".join(x.split("(")[0] for x in p[1:])
                v = mi or entry["macos"]
                if key not in mem or vkey(v) < vkey(mem[key].split(" ")[0]):
                    mem[key] = v + (f" (deprecated {md})" if md else "")
        # only keep members newer than the floor, or deprecated ones: the rest is noise
        entry["members_since_12"] = {k: v for k, v in sorted(mem.items()) if vkey(v.split(" ")[0]) >= (12,) or "deprecated" in v}
        entry["member_count"] = len(mem)
        out[cls] = entry
    return out


def tool(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", help="reuse/keep symbol graphs in this directory")
    ap.add_argument("out", nargs="?", default=os.path.join(os.path.dirname(__file__), "..", "map", "swiftui-inventory.yaml"))
    args = ap.parse_args()

    sdk = tool("xcrun", "--show-sdk-path")
    sdk_ver = tool("xcrun", "--show-sdk-version")
    target = f"x86_64-apple-macos{sdk_ver}"
    work = args.cache or tempfile.mkdtemp(prefix="symgraph-")
    os.makedirs(work, exist_ok=True)
    for m in MODULES:
        if not glob.glob(os.path.join(work, m + ".symbols.json")):
            extract(m, sdk, work, target)

    # SwiftUI's baseline is macOS 10.15: symbols with no macOS annotation date from it.
    sw_syms, sw_rels = {}, []
    for m in ("SwiftUI", "SwiftUICore"):
        s, r = load(work, m)
        sw_syms.update(s)
        sw_rels += r
    types, views, styles, modifiers, members = build(sw_syms, sw_rels, "10.15")
    ak_syms, ak_rels = load(work, "AppKit")
    un_syms, un_rels = load(work, "UserNotifications")

    inv = {
        "generated_by": "tools/scan_swiftui.py",
        "sdk": {"version": sdk_ver, "build": tool("xcrun", "--show-sdk-build-version"),
                "xcode": tool("xcodebuild", "-version").replace("\n", ", "),
                "swift": tool("xcrun", "swiftc", "--version").splitlines()[0] if tool("xcrun", "swiftc", "--version") else ""},
        "tiers": {"12": "floor", "13": "", "14": "", "15": "", "26": "Liquid Glass", "27": "current SDK"},
        "views": views,
        "styles": styles,
        "modifiers": modifiers,
        "types": types,
        "members": members,
        "appkit": appkit(ak_syms, ak_rels, APPKIT_CLASSES, "10.0"),
        "usernotifications": appkit(un_syms, un_rels, UN_CLASSES, "10.14"),
    }
    with open(args.out, "w") as f:
        yaml.safe_dump(inv, f, sort_keys=False, width=140, allow_unicode=True)
    newer = lambda d: sum(1 for v in d.values() if vkey(v["macos"]) >= (26,))
    print(f"{args.out}: SDK {sdk_ver}: {len(views)} views ({newer(views)} from 26+), "
          f"{sum(len(v) for v in styles.values())} style members in {len(styles)} style protocols, "
          f"{len(modifiers)} modifiers ({newer(modifiers)} from 26+), {len(types)} types, "
          f"{len(inv['appkit'])} AppKit classes")


if __name__ == "__main__":
    main()
