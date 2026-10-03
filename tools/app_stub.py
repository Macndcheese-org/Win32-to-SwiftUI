#!/usr/bin/env python3
"""A Windows program as a Mac app: a stub bundle to run wine from.

macOS names a process, and draws its Dock icon (with the user's icon style:
Liquid Glass, clear, tinted...), from the app bundle it runs in. A program started
through wine is the wine loader, which has no bundle, so the Dock says "wine" with
a flat picture. This builds `<Name>.app` with
  - Contents/MacOS/wine      an APFS clone of the engine's loader,
  - Contents/MacOS/ntdll.so  a symlink to the engine's (the loader looks next to itself),
  - Contents/Resources/AppIcon.icns  made from the program's own icon,
  - Contents/Info.plist      the name and the icon.
Start the program with the stub's `wine` as the loader and WINELOADER set to it
(without that wine starts itself again from the engine and leaves the bundle):

    stub=$(app_stub.py --engine .../wine-unified --exe 'C:\\...\\notepad++.exe' --unix-exe /path/notepad++.exe --out ~/Library/.../Apps)
    WINELOADER="$stub" "$stub" 'C:\\...\\notepad++.exe'

Pure Python: the icon is read out of the PE file's RT_GROUP_ICON / RT_ICON
resources, so the user needs nothing installed.
"""
import argparse
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

RT_ICON, RT_GROUP_ICON = 3, 14


# ---------- the PE file's resources ----------

class PE:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        nsections = struct.unpack_from("<H", d, pe + 6)[0]
        optsize = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", d, opt)[0]
        dd = opt + (112 if magic == 0x20B else 96)
        self.rsrc_rva, self.rsrc_size = struct.unpack_from("<II", d, dd + 2 * 8)
        self.sections = []
        sec = opt + optsize
        for i in range(nsections):
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, sec + i * 40 + 8)
            self.sections.append((vaddr, max(vsize, rawsize), rawptr))

    def offset(self, rva):
        for vaddr, size, rawptr in self.sections:
            if vaddr <= rva < vaddr + size:
                return rawptr + rva - vaddr
        raise ValueError("rva outside the sections")

    def _entries(self, base, rel):
        d = self.data
        off = base + rel
        named, ids = struct.unpack_from("<HH", d, off + 12)
        out = []
        for i in range(named + ids):
            name, target = struct.unpack_from("<II", d, off + 16 + i * 8)
            out.append((name, target))
        return out

    def resources(self, rtype):
        """[(id, data)] of one resource type, first language of each name"""
        if not self.rsrc_rva:
            return []
        base = self.offset(self.rsrc_rva)
        d = self.data
        found = []
        for tname, ttarget in self._entries(base, 0):
            if tname != rtype or not ttarget & 0x80000000:
                continue
            for nid, ntarget in self._entries(base, ttarget & 0x7FFFFFFF):
                if not ntarget & 0x80000000:
                    continue
                langs = self._entries(base, ntarget & 0x7FFFFFFF)
                if not langs:
                    continue
                leaf = base + langs[0][1]
                rva, size = struct.unpack_from("<II", d, leaf)
                found.append((nid, d[self.offset(rva):self.offset(rva) + size]))
        return found

    def icon_group(self):
        """[(width, height, bits, data)] of the first icon group, the program's icon"""
        groups = sorted(self.resources(RT_GROUP_ICON), key=lambda g: g[0])
        if not groups:
            return []
        icons = dict(self.resources(RT_ICON))
        group = groups[0][1]
        count = struct.unpack_from("<H", group, 4)[0]
        out = []
        for i in range(count):
            w, h, _, _, _, bits, _, ident = struct.unpack_from("<BBBBHHIH", group, 6 + i * 14)
            if ident in icons:
                out.append((w or 256, h or 256, bits, icons[ident]))
        return out


# ---------- an icon image as a PNG ----------

def png(width, height, rgba):
    def chunk(kind, body):
        c = struct.pack(">I", len(body)) + kind + body
        return c + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def dib_to_rgba(data):
    """an icon's DIB (XOR bitmap + AND mask) as (width, height, RGBA bytes)"""
    hsize, width, height2, _, bits, comp, _, _, _, ncolors, _ = struct.unpack_from("<IiiHHIIiiII", data, 0)
    height = height2 // 2
    pos = hsize
    palette = []
    if bits <= 8:
        n = ncolors or (1 << bits)
        palette = [struct.unpack_from("<BBBB", data, pos + i * 4) for i in range(n)]
        pos += n * 4
    stride = ((width * bits + 31) // 32) * 4
    xor = data[pos:pos + stride * height]
    mask_pos = pos + stride * height
    mask_stride = ((width + 31) // 32) * 4
    mask = data[mask_pos:mask_pos + mask_stride * height]
    out = bytearray(width * height * 4)
    any_alpha = False
    for y in range(height):
        row = xor[(height - 1 - y) * stride:(height - y) * stride]
        for x in range(width):
            if bits == 32:
                b, g, r, a = row[x * 4:x * 4 + 4]
            elif bits == 24:
                b, g, r = row[x * 3:x * 3 + 3]
                a = 255
            elif bits == 8:
                b, g, r, _ = palette[row[x]]
                a = 255
            elif bits == 4:
                v = row[x // 2]
                b, g, r, _ = palette[(v >> 4) if x % 2 == 0 else (v & 15)]
                a = 255
            else:
                raise ValueError("icon depth %d" % bits)
            any_alpha |= a != 0
            o = (y * width + x) * 4
            out[o:o + 4] = bytes((r, g, b, a))
    if bits != 32 or not any_alpha:
        for y in range(height):
            mrow = mask[(height - 1 - y) * mask_stride:(height - y) * mask_stride]
            for x in range(width):
                if len(mrow) > x // 8 and mrow[x // 8] & (0x80 >> (x % 8)):
                    out[(y * width + x) * 4 + 3] = 0
                elif bits != 32:
                    out[(y * width + x) * 4 + 3] = 255
    return width, height, bytes(out)


def icon_png(entry):
    """(width, height, PNG bytes) of one image of the icon group"""
    w, h, bits, data = entry
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return w, h, data
    width, height, rgba = dib_to_rgba(data)
    return width, height, png(width, height, rgba)


# ---------- the bundle ----------

SIZES = [(16, "icon_16x16"), (32, "icon_16x16@2x"), (32, "icon_32x32"), (64, "icon_32x32@2x"),
         (128, "icon_128x128"), (256, "icon_128x128@2x"), (256, "icon_256x256"), (512, "icon_256x256@2x"),
         (512, "icon_512x512"), (1024, "icon_512x512@2x")]


def make_icns(group, dest):
    """AppIcon.icns from an icon group: the biggest image, scaled to each size (a smaller
    image of the group is used where it is the exact size)"""
    images = sorted((icon_png(e) for e in group), key=lambda i: (i[0] * i[1], len(i[2])))
    biggest = images[-1]
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.mkdir(iconset)
        big = os.path.join(tmp, "big.png")
        open(big, "wb").write(biggest[2])
        for size, name in SIZES:
            out = os.path.join(iconset, name + ".png")
            exact = [i for i in images if i[0] == size and i[1] == size]
            if exact:
                open(out, "wb").write(exact[-1][2])
            else:
                subprocess.run(["sips", "-z", str(size), str(size), big, "--out", out], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", dest], check=True)


def product_name(pe_path):
    """the program's name, as the runtime names it (System.swift AppName): its ProductName
    ("Notepad++", "Steam"), else its file name without .exe; a system tool's ProductName is the OS's"""
    base = os.path.basename(pe_path)
    fallback = base[:-4] if base.lower().endswith(".exe") else base
    try:
        for _, data in PE(pe_path).resources(16):          # RT_VERSION
            key = "ProductName".encode("utf-16-le") + b"\0\0"
            at = data.find(key)
            if at < 0:
                continue
            pos = (at + len(key) + 3) & ~3                  # the value follows, 4-byte aligned
            end = pos
            while end + 1 < len(data) and data[end:end + 2] != b"\0\0":
                end += 2
            value = data[pos:end].decode("utf-16-le", "replace").strip()
            if value and len(value) < 40 and "Operating System" not in value and "/" not in value:
                return value
    except Exception:
        pass
    return fallback


def build(engine, unix_exe, out_dir, name=None, identifier=None):
    name = name or product_name(unix_exe)
    bundle = os.path.join(out_dir, name + ".app")
    macos = os.path.join(bundle, "Contents", "MacOS")
    resources = os.path.join(bundle, "Contents", "Resources")
    os.makedirs(macos, exist_ok=True)
    os.makedirs(resources, exist_ok=True)

    loader = os.path.join(engine, "loader", "wine")
    stub = os.path.join(macos, "wine")
    if not os.path.exists(stub) or os.path.getmtime(stub) < os.path.getmtime(loader):
        if os.path.exists(stub):
            os.remove(stub)
        # an APFS clone: no space, and the same signed code
        if subprocess.run(["cp", "-c", loader, stub], stderr=subprocess.DEVNULL).returncode:
            shutil.copy2(loader, stub)
    link = os.path.join(macos, "ntdll.so")
    target = os.path.join(engine, "dlls", "ntdll", "ntdll.so")
    if os.path.islink(link) or os.path.exists(link):
        os.remove(link)
    os.symlink(target, link)

    has_icon = False
    try:
        group = PE(unix_exe).icon_group()
        if group:
            make_icns(group, os.path.join(resources, "AppIcon.icns"))
            has_icon = True
    except Exception as exc:        # a program without an icon, or one we can't read: the generic icon
        print("no icon from %s: %s" % (unix_exe, exc), file=sys.stderr)

    info = {
        "CFBundleName": name,
        "CFBundleDisplayName": name,
        "CFBundleIdentifier": identifier or "org.macncheese.app." + "".join(c if c.isalnum() else "-" for c in name.lower()),
        "CFBundleExecutable": "wine",
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": "1.0",
        "NSHighResolutionCapable": True,
        # an agent: the helpers wine starts in the bundle (explorer, services) get no Dock tile;
        # the program's own process makes itself a regular app when it has a window (winemac)
        "LSUIElement": True,
    }
    if has_icon:
        info["CFBundleIconFile"] = "AppIcon"
    with open(os.path.join(bundle, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump(info, f)
    return stub


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", required=True, help="the wine tree (the app's wine-unified)")
    ap.add_argument("--unix-exe", required=True, help="the program's .exe, as a Unix path")
    ap.add_argument("--out", required=True, help="the folder to put <Name>.app in")
    ap.add_argument("--name", help="the app's name (default: the exe's)")
    args = ap.parse_args()
    print(build(args.engine, args.unix_exe, args.out, args.name))


if __name__ == "__main__":
    main()
