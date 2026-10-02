#!/usr/bin/env python3
"""Settings panes' icons: runtime/Sources/W2SKit/PaneIconTable.swift.

A settings window's panes are its toolbar's buttons, and a Mac settings toolbar
has an icon on each (HIG, Settings). A Win32 app gives its panes titles only.
CONCEPTS below is the reviewed part: what a pane is called in English, and the
SF Symbol for it. The pane's title is in the app's language, so each concept's
words are looked up in macOS's own localizations (the .loctable files of AppKit,
System Settings' panes and the system's apps): wherever an English string is one
of a concept's words, the same string in every other language is that concept
too. Nothing is translated by hand.

Run on the Mac the runtime is built on; the output is checked in.
"""
import glob
import os
import plistlib
import re
import sys
import unicodedata

# (SF Symbol, English names of the pane) -- in order: an earlier concept wins a word
CONCEPTS = [
    ("gearshape", ["General"]),
    ("moon", ["Dark Mode", "Dark mode"]),
    ("display", ["Display", "Displays", "Screen", "Graphics", "Video"]),
    ("circle.lefthalf.filled", ["Appearance", "Theme", "Themes"]),
    ("speaker.wave.2", ["Sound", "Sounds", "Audio"]),
    ("books.vertical", ["Library", "Libraries"]),
    ("internaldrive", ["Drive", "Drives", "Disk", "Disks", "Storage"]),
    ("info.circle", ["About"]),
    ("square.grid.2x2", ["Applications", "Apps", "Programs"]),
    ("menubar.dock.rectangle", ["Desktop", "Desktop Integration", "Desktop & Dock"]),
    ("menubar.rectangle", ["Toolbar", "Toolbars", "Menu Bar"]),
    ("square.on.square", ["Tabs", "Tab Bar"]),
    ("pencil", ["Editing", "Editor", "Edit"]),
    ("rectangle.dashed", ["Margins", "Border", "Borders"]),
    ("doc.badge.plus", ["New Document"]),
    ("folder", ["Folder", "Folders", "Default Folder", "Directory"]),
    ("clock.arrow.circlepath", ["Recent", "Recents", "Recent Files", "History"]),
    ("puzzlepiece.extension", ["Extensions", "File Extensions", "Plug-ins", "Plugins", "Add-ons"]),
    ("globe", ["Language", "Languages", "Language & Region"]),
    ("increase.indent", ["Indentation", "Indent"]),
    ("paintpalette", ["Colors", "Colours", "Coloring", "Highlighting", "Syntax Highlighting", "Styles", "Style"]),
    ("printer", ["Print", "Printing", "Printers"]),
    ("magnifyingglass", ["Search", "Find", "Search Engine"]),
    ("externaldrive.badge.timemachine", ["Backup", "Backups"]),
    ("text.badge.checkmark", ["Auto-Completion", "Autocompletion", "AutoComplete", "Completion"]),
    ("macwindow.on.rectangle", ["Windows", "Window", "Multi-Instance"]),
    ("curlybraces", ["Delimiter", "Delimiters"]),
    ("speedometer", ["Performance"]),
    ("cloud", ["Cloud", "iCloud", "Sync"]),
    ("ellipsis.circle", ["Misc", "Miscellaneous", "Other"]),
    ("keyboard", ["Keyboard", "Shortcuts", "Keyboard Shortcuts", "Hotkeys"]),
    ("computermouse", ["Mouse"]),
    ("network", ["Network", "Internet", "Connection", "Connections", "Proxy"]),
    ("lock.shield", ["Security", "Privacy", "Privacy & Security"]),
    ("arrow.triangle.2.circlepath", ["Update", "Updates", "Software Update"]),
    ("person.crop.circle", ["Account", "Accounts", "Users", "Profile"]),
    ("bell", ["Notifications"]),
    ("textformat", ["Font", "Fonts", "Text"]),
    ("gamecontroller", ["Controllers", "Game Controllers", "Joystick", "Gamepad", "Input"]),
    ("gearshape.2", ["Advanced"]),
    ("square.and.arrow.up", ["Sharing", "Export"]),
    ("testtube.2", ["Staging", "Experimental"]),
]

TABLES = (glob.glob("/System/Library/Frameworks/AppKit.framework/Resources/*.loctable") +
          glob.glob("/System/Library/ExtensionKit/Extensions/*.appex/Contents/Resources/*.loctable") +
          glob.glob("/System/Applications/*.app/Contents/Resources/*.loctable") +
          glob.glob("/System/Applications/Utilities/*.app/Contents/Resources/*.loctable"))

OUT = os.path.join(os.path.dirname(__file__), "..", "runtime", "Sources", "W2SKit", "PaneIconTable.swift")


def fold(s):
    """as the runtime folds a title: lower case, no accents, no trailing dots or colon"""
    s = unicodedata.normalize("NFD", s.lower())
    s = "".join(c for c in s if unicodedata.category(c) != "Mn")
    return s.strip().rstrip(".…:").strip().replace("’", "'")


def main():
    symbols = set(plistlib.load(open("/System/Library/CoreServices/CoreGlyphs.bundle/Contents/Resources/"
                                     "name_availability.plist", "rb"))["symbols"])
    concept_of = {}
    for symbol, words in CONCEPTS:
        if symbol not in symbols:
            sys.exit(f"{symbol}: no such SF Symbol here")
        for w in words:
            concept_of.setdefault(fold(w), symbol)

    # localized word -> {symbol: how many strings say so}
    votes = {w: {s: 1000} for w, s in concept_of.items()}       # the English words themselves
    for path in TABLES:
        try:
            table = plistlib.load(open(path, "rb"))
        except Exception:
            continue
        en = table.get("en") or table.get("en_US") or table.get("Base")
        if not isinstance(en, dict):
            continue
        for key, english in en.items():
            if not isinstance(english, str):
                continue
            # "About %@": the app's name left out ("À propos de %@" -> "à propos")
            placeholder = "%@" in english
            if (symbol := concept_of.get(fold(english.replace("%@", "")))) is None:
                continue
            for lang, words in table.items():
                text = words.get(key) if isinstance(words, dict) else None
                if not isinstance(text, str) or not text.strip() or len(text) > 40:
                    continue
                if placeholder:
                    text = re.sub(r"%(\d\$)?@", "", text).strip()
                    text = re.sub(r"\s+(de|d'|di|von|del|do|da|du|van|av|af|z|ze|od)$", "", text, flags=re.I).strip()
                if not text or "%" in text:
                    continue
                word = fold(text)
                if len(word.split()) > 4:
                    continue
                votes.setdefault(word, {})
                votes[word][symbol] = votes[word].get(symbol, 0) + 1

    lines = sorted(f"{w}\t{max(v.items(), key=lambda kv: kv[1])[0]}" for w, v in votes.items() if w)
    with open(OUT, "w") as f:
        f.write("// Generated by tools/gen_pane_icons.py from macOS's localizations: do not edit.\n")
        f.write("// A settings pane's name (folded, every language macOS has) -> its SF Symbol.\n\n")
        f.write("enum PaneIconTable {\n    static let words = \"\"\"\n")
        for line in lines:
            f.write("        " + line.replace("\\", "\\\\").replace('"""', '\\"\\"\\"') + "\n")
        f.write("        \"\"\"\n}\n")
    print(f"{len(TABLES)} tables, {len(lines)} words -> {os.path.relpath(OUT)}")


if __name__ == "__main__":
    main()
