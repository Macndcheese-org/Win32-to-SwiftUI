#!/usr/bin/env python3
"""Compile every Swift snippet in the map against the SDK at the macOS 12 floor.

Each snippet is wrapped in its own declaration, guarded by
`@available(macOS <since>, *)`, and the whole file is type-checked with
`-target x86_64-apple-macos12.0`, so the compiler enforces both the API names
and that nothing is used below the macOS version the map claims.

usage: typecheck_snippets.py [--keep FILE]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

from maplib import load_map, snippets

PRELUDE = r"""
import SwiftUI
import AppKit
import UniformTypeIdentifiers
import UserNotifications

struct Row: Identifiable, Hashable { let id: Int; var name: String; var detail: String; var checked: Bool }
struct Node: Identifiable, Hashable { let id: Int; var name: String; var children: [Node]? }
struct Choice: Identifiable, Hashable { let id: Int; var title: String }
"""

VIEW_FIXTURE = """
  @State var text = ""
  @State var isOn = false
  @State var flags = [true, false]
  @State var value = 0.5
  @State var intValue = 0
  @State var date = Date()
  @State var selection: Int? = nil
  @State var multiSelection = Set<Int>()
  @State var choice = 0
  @State var color = Color.accentColor
  @State var isPresented = false
  @State var sortOrder = [KeyPathComparator(\\Row.name)]
  let title = "Title"
  let note = "Note"
  let rows: [Row] = []
  let tree: [Node] = []
  let choices: [Choice] = []
  let image = NSImage()
  let attributed = AttributedString("Text")
  func action() {}
"""
VIEW_FIXTURE_15 = """
  @State var textSelection: TextSelection? = nil
"""

APPKIT_FIXTURE = """
  let window = NSWindow()
  let view = NSView()
  let menu = NSMenu()
  let title = "Title"
  let note = "Note"
  let items = ["One", "Two"]
  let image = NSImage()
  func action() {}
  _ = (window, view, menu, title, note, items, image)
"""


def ident(where):
    return "S_" + re.sub(r"\W", "_", where)


def avail(since):
    return f"@available(macOS {since}, *)\n" if float(since) > 12 else ""


def wrap(where, kind, since, code):
    name = ident(where)
    code = code.strip()
    if kind in ("view", "mod"):
        body = code if kind == "view" else "Color.clear" + code
        fixture = VIEW_FIXTURE + (VIEW_FIXTURE_15 if float(since) >= 15 else "")
        return f"// {where}\n{avail(since)}struct {name}: View {{\n{fixture}\n  var body: some View {{\n{body}\n  }}\n}}\n"
    if kind == "appkit":
        return f"// {where}\n{avail(since)}@MainActor func {name}() {{\n{APPKIT_FIXTURE}\n{code}\n}}\n"
    if kind == "nscolor":
        return f"// {where}\n@MainActor func {name}() -> NSColor {{ {code} }}\n"
    if kind == "nsfont":
        return f"// {where}\n@MainActor func {name}() -> NSFont? {{ {code} }}\n"
    raise ValueError(kind)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", help="write the generated Swift file here")
    args = ap.parse_args()
    m, _ = load_map()
    parts = [PRELUDE]
    spans = []  # (first line, last line, where)
    line = PRELUDE.count("\n") + 1
    for where, kind, since, code in snippets(m):
        text = wrap(where, kind, since, code)
        spans.append((line, line + text.count("\n"), where))
        parts.append(text)
        line += text.count("\n")
    src = "".join(parts)
    path = args.keep or os.path.join(tempfile.mkdtemp(prefix="w2s-"), "snippets.swift")
    with open(path, "w") as f:
        f.write(src)
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True).stdout.strip()
    r = subprocess.run(["xcrun", "swiftc", "-typecheck", "-swift-version", "5", "-target", "x86_64-apple-macos12.0",
                        "-sdk", sdk, path], capture_output=True, text=True)
    errors = {}
    for l in (r.stdout + r.stderr).splitlines():
        mm = re.match(re.escape(path) + r":(\d+):\d+: error: (.*)", l)
        if mm:
            n = int(mm.group(1))
            where = next((w for a, b, w in spans if a <= n <= b), "prelude")
            errors.setdefault(where, []).append(mm.group(2))
    total = len(spans)
    if r.returncode != 0 and not errors:
        print(r.stderr[-3000:])
        sys.exit("swiftc failed without attributable errors")
    for where, errs in errors.items():
        print(f"FAIL {where}")
        for e in dict.fromkeys(errs):
            print(f"     {e}")
    print(f"{total - len(errors)}/{total} snippets type-check at the macOS 12 floor"
          + (f" ({path})" if args.keep or errors else ""))
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
