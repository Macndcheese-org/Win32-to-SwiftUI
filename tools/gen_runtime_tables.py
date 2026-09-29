#!/usr/bin/env python3
"""Generate runtime/pe/w2s_map_tables.h from the map.

For every entry: its id, the Windows messages in its `state_in` list, which
tell the PE side when to send the native view a fresh snapshot, and those in
its `answers` list, the queries the PE answers from the native view instead of
the Win32 control. Names with ANSI and Unicode variants (LVM_INSERTITEM) get
both. Every name is checked against the wine headers, so a typo in the map
fails here instead of silently never matching.

usage: gen_runtime_tables.py <wine source tree>
"""
import glob
import os
import re
import sys

from maplib import ICON_MODULES, ROOT, STRING_NAMED, aslist, icon_spec, load_map

TOKEN = re.compile(r"^([A-Z][A-Z0-9]*_[A-Z0-9_]+)\b")


def defined_names(tree):
    names = set()
    for f in glob.glob(os.path.join(tree, "include", "*.h")) + glob.glob(os.path.join(tree, "include", "*.rh")):
        text = open(f, errors="replace").read()
        names.update(re.findall(r"#\s*define\s+(\w+)", text))
        for body in re.findall(r"\benum\b[^{;]*\{([^}]*)\}", text):
            names.update(re.findall(r"^\s*(\w+)", body, re.M))
    return names


def resource_ids(tree):
    """module -> {name: id} from its headers"""
    values = {}
    for module, (_, headers) in ICON_MODULES.items():
        values[module] = {}
        for header in headers:
            text = open(os.path.join(tree, header), errors="replace").read()
            values[module].update({n: int(v, 16 if v.startswith("0x") else 10) for n, v in re.findall(r"#\s*define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)\b", text)})
    return values


def icon_rows(tree, icons):
    """{ L"module.dll", id, "spec", L"name" } for every stock icon; names resolved in wine's
    headers (a string-named resource has id 0 and its name)."""
    values = resource_ids(tree)
    rows = []
    for key, d in icons.items():
        module, name = key.split("/")
        spec = icon_spec(d).replace("\\", "\\\\").replace('"', '\\"')
        if name in values[module]:
            rows.append(f'    {{ L"{ICON_MODULES[module][0]}", {values[module][name]}, "{spec}" }},')
        elif module in STRING_NAMED:
            rows.append(f'    {{ L"{ICON_MODULES[module][0]}", 0, "{spec}", L"{name}" }},')
        else:
            sys.exit(f"icons.{key}: {name} is not defined in {', '.join(ICON_MODULES[module][1])}")
    return rows


# comctl32's standard strips: the image name's prefix -> the strip (IDB_*_SMALL_COLOR; the
# large one is the next id and holds the same images)
STANDARD_STRIPS = {"STD": 0, "VIEW": 4, "HIST": 8}


def toolbar_rows(tree, images):
    """{ L"module", bitmap, index, "spec" }: comctl32's standard strips (bitmap = the strip's
    IDB_*_SMALL_COLOR) and wine's programs' own strips (bitmap = the resource id)."""
    values = resource_ids(tree)
    rows = []
    for key, d in images.items():
        parts = key.split("/")
        module = parts[0]
        if module == "comctl32":
            name = parts[1]
            if name not in values[module]:
                sys.exit(f"toolbar_images.{key}: {name} is not defined in the comctl32 headers")
            bitmap, index = STANDARD_STRIPS[name.split("_")[0]], values[module][name]
        else:
            if parts[1].isdigit():
                bitmap = int(parts[1])
            elif parts[1] in values[module]:
                bitmap = values[module][parts[1]]
            else:
                sys.exit(f"toolbar_images.{key}: {parts[1]} is not defined in {', '.join(ICON_MODULES[module][1])}")
            index = int(parts[2])
        spec = icon_spec(d).replace("\\", "\\\\").replace('"', '\\"')
        rows.append(f'    {{ L"{ICON_MODULES[module][0]}", {bitmap}, {index}, "{spec}" }},')
    return rows


def main():
    tree = sys.argv[1]
    names = defined_names(tree)
    m, _ = load_map()
    out, skipped = [], []
    rows = []

    def messages(e, key):
        msgs = []
        for item in aslist(e.get(key)):
            t = TOKEN.match(str(item))
            if not t:
                skipped.append(f"{e['id']}: {item}")
                continue
            n = t.group(1)
            variants = [v for v in (n + "A", n + "W") if v in names]
            if variants:
                msgs += variants
            elif n in names:
                msgs.append(n)
            else:
                sys.exit(f"{e['id']}: {key} {n} is not defined in the wine headers")
        return list(dict.fromkeys(msgs))

    # A message that changes a Win32 ListBox changes it whatever the variant
    # (listbox.multi only lists its selection extras), so an entry follows the
    # state_in of every entry of its window class.
    by_class = {}
    for e in m["entries"]:
        for c in aslist((e.get("win32") or {}).get("class")):
            by_class.setdefault(c, []).append(e)

    for e in m["entries"]:
        base = re.sub(r"\W", "_", e["id"])
        state = messages(e, "state_in")
        for c in aslist((e.get("win32") or {}).get("class")):
            for other in by_class[c]:
                if other is not e:
                    state += [s for s in messages(other, "state_in") if s not in state]
        answers = messages(e, "answers")
        out.append(f"static const UINT w2s_state_{base}[] = {{ {', '.join(state) or '0'} }};")
        if answers:
            out.append(f"static const UINT w2s_answers_{base}[] = {{ {', '.join(answers)} }};")
        rows.append(f'    {{ "{e["id"]}", w2s_state_{base}, {len(state)}, '
                    + (f"w2s_answers_{base}, {len(answers)} }},"  if answers else "NULL, 0 },"))
    header = f"""/* Generated by tools/gen_runtime_tables.py from map/ui-map. Don't edit. */
#ifndef W2S_MAP_TABLES_H
#define W2S_MAP_TABLES_H

struct w2s_map_entry
{{
    const char *id;
    const UINT *state_in;
    unsigned int state_in_count;
    const UINT *answers;
    unsigned int answers_count;
}};

{chr(10).join(out)}

static const struct w2s_map_entry w2s_map_entries[] =
{{
{chr(10).join(rows)}
}};

/* stock icons (map: 75-icons.yaml): a module's icon resource -> the macOS image */
struct w2s_icon_entry
{{
    const WCHAR *module;
    unsigned int id;
    const char *spec;
    const WCHAR *name;          /* a string-named resource (id 0) */
}};

static const struct w2s_icon_entry w2s_icon_entries[] =
{{
{chr(10).join(icon_rows(tree, m["icons"]))}
}};

/* toolbar images (map: 75-icons.yaml toolbar_images): an image strip's image -> the macOS image */
struct w2s_toolbar_image
{{
    const WCHAR *module;
    unsigned int bitmap;        /* comctl32: the standard strip's IDB_*_SMALL_COLOR; else the resource id */
    unsigned int index;
    const char *spec;
}};

static const struct w2s_toolbar_image w2s_toolbar_images[] =
{{
{chr(10).join(toolbar_rows(tree, m["toolbar_images"]))}
}};

#endif
"""
    path = os.path.join(ROOT, "runtime", "pe", "w2s_map_tables.h")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(header)
    print(f"{path}: {len(rows)} entries; prose state_in/answers items left out: {len(skipped)}")
    for s in skipped:
        print("  ", s)


if __name__ == "__main__":
    main()
