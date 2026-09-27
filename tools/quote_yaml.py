#!/usr/bin/env python3
"""Authoring helper: quote plain `code:` values and one-key flow dispositions so
Swift/prose punctuation (': ', ', ') can't break the YAML. Idempotent."""
import re
import sys


def q(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


for path in sys.argv[1:]:
    out = []
    for line in open(path).read().split("\n"):
        m = re.match(r"^(\s+code: )(?![|'\">])(.+)$", line)
        if m:
            line = m.group(1) + q(m.group(2))
        m = re.match(r"^(\s+[A-Z][A-Za-z0-9_/]*: \{)(\w+): (?!['\"])(.*)\}\s*$", line)
        if m and not re.search(r"\b(note|since|fallback|appkit_since): ", m.group(3)):
            line = f"{m.group(1)}{m.group(2)}: {q(m.group(3))}}}"
        out.append(line)
    open(path, "w").write("\n".join(out))
