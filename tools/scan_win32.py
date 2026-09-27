#!/usr/bin/env python3
"""Inventory every Windows UI element a wine tree implements.

Reads the wine source tree (read-only) and writes map/win32-inventory.yaml:
window classes, their style/flag bits, messages and notifications,
dialog/system entry points, and the visual parts from vsstyle.h.

usage: scan_win32.py <wine-tree> [out.yaml]
"""
import os
import re
import subprocess
import sys

import yaml

# (header, prefix, kind, owner)
#   kind: style | exstyle | flag -> every name must be covered by ui-map.yaml
#         message | notification -> listed for the runtime bridge only
GROUPS = [
    # user32 controls
    ("winuser.h", "BS_", "style", "Button"),
    ("commctrl.h", "BS_", "style", "Button"),
    ("commctrl.h", "BCSS_", "style", "Button"),
    ("winuser.h", "BM_", "message", "Button"),
    ("commctrl.h", "BCM_", "message", "Button"),
    ("winuser.h", "BN_", "notification", "Button"),
    ("commctrl.h", "BCN_", "notification", "Button"),
    ("winuser.h", "ES_", "style", "Edit"),
    ("winuser.h", "EM_", "message", "Edit"),
    ("commctrl.h", "EM_", "message", "Edit"),
    ("winuser.h", "EN_", "notification", "Edit"),
    ("winuser.h", "SS_", "style", "Static"),
    ("winuser.h", "STM_", "message", "Static"),
    ("winuser.h", "STN_", "notification", "Static"),
    ("winuser.h", "LBS_", "style", "ListBox"),
    ("winuser.h", "LB_", "message", "ListBox"),
    ("winuser.h", "LBN_", "notification", "ListBox"),
    ("winuser.h", "CBS_", "style", "ComboBox"),
    ("winuser.h", "CB_", "message", "ComboBox"),
    ("winuser.h", "CBN_", "notification", "ComboBox"),
    ("winuser.h", "SBS_", "style", "ScrollBar"),
    ("winuser.h", "SBM_", "message", "ScrollBar"),
    ("winuser.h", "DS_", "style", "#32770"),
    ("winuser.h", "DM_", "message", "#32770"),
    ("winuser.h", "MDIS_", "style", "MDIClient"),
    ("winuser.h", "WS_", "style", "window"),
    ("winuser.h", "WS_EX_", "exstyle", "window"),
    # comctl32
    ("commctrl.h", "CCS_", "style", "common"),
    ("commctrl.h", "NM_", "notification", "common"),
    ("commctrl.h", "LVS_", "style", "SysListView32"),
    ("commctrl.h", "LVS_EX_", "exstyle", "SysListView32"),
    ("commctrl.h", "LVM_", "message", "SysListView32"),
    ("commctrl.h", "LVN_", "notification", "SysListView32"),
    ("commctrl.h", "TVS_", "style", "SysTreeView32"),
    ("commctrl.h", "TVS_EX_", "exstyle", "SysTreeView32"),
    ("commctrl.h", "TVM_", "message", "SysTreeView32"),
    ("commctrl.h", "TVN_", "notification", "SysTreeView32"),
    ("commctrl.h", "TCS_", "style", "SysTabControl32"),
    ("commctrl.h", "TCS_EX_", "exstyle", "SysTabControl32"),
    ("commctrl.h", "TCM_", "message", "SysTabControl32"),
    ("commctrl.h", "TCN_", "notification", "SysTabControl32"),
    ("commctrl.h", "TBSTYLE_", "style", "ToolbarWindow32"),
    ("commctrl.h", "TBSTYLE_EX_", "exstyle", "ToolbarWindow32"),
    ("commctrl.h", "BTNS_", "style", "ToolbarWindow32"),
    ("commctrl.h", "TB_", "message", "ToolbarWindow32"),
    ("commctrl.h", "TBN_", "notification", "ToolbarWindow32"),
    ("commctrl.h", "RBS_", "style", "ReBarWindow32"),
    ("commctrl.h", "RBBS_", "style", "ReBarWindow32"),
    ("commctrl.h", "RB_", "message", "ReBarWindow32"),
    ("commctrl.h", "RBN_", "notification", "ReBarWindow32"),
    ("commctrl.h", "SBARS_", "style", "msctls_statusbar32"),
    ("commctrl.h", "SBT_", "style", "msctls_statusbar32"),
    ("commctrl.h", "SB_", "message", "msctls_statusbar32"),
    ("commctrl.h", "SBN_", "notification", "msctls_statusbar32"),
    ("commctrl.h", "PBS_", "style", "msctls_progress32"),
    ("commctrl.h", "PBST_", "style", "msctls_progress32"),
    ("commctrl.h", "PBM_", "message", "msctls_progress32"),
    ("commctrl.h", "TBS_", "style", "msctls_trackbar32"),
    ("commctrl.h", "TBM_", "message", "msctls_trackbar32"),
    ("commctrl.h", "TRBN_", "notification", "msctls_trackbar32"),
    ("commctrl.h", "UDS_", "style", "msctls_updown32"),
    ("commctrl.h", "UDM_", "message", "msctls_updown32"),
    ("commctrl.h", "UDN_", "notification", "msctls_updown32"),
    ("commctrl.h", "HDS_", "style", "SysHeader32"),
    ("commctrl.h", "HDM_", "message", "SysHeader32"),
    ("commctrl.h", "HDN_", "notification", "SysHeader32"),
    ("commctrl.h", "TTS_", "style", "tooltips_class32"),
    ("commctrl.h", "TTM_", "message", "tooltips_class32"),
    ("commctrl.h", "TTN_", "notification", "tooltips_class32"),
    ("commctrl.h", "DTS_", "style", "SysDateTimePick32"),
    ("commctrl.h", "DTM_", "message", "SysDateTimePick32"),
    ("commctrl.h", "DTN_", "notification", "SysDateTimePick32"),
    ("commctrl.h", "MCS_", "style", "SysMonthCal32"),
    ("commctrl.h", "MCM_", "message", "SysMonthCal32"),
    ("commctrl.h", "MCN_", "notification", "SysMonthCal32"),
    ("commctrl.h", "HKM_", "message", "msctls_hotkey32"),
    ("commctrl.h", "HOTKEYF_", "flag", "msctls_hotkey32"),
    ("commctrl.h", "IPM_", "message", "SysIPAddress32"),
    ("commctrl.h", "IPN_", "notification", "SysIPAddress32"),
    ("commctrl.h", "PGS_", "style", "SysPager"),
    ("commctrl.h", "PGM_", "message", "SysPager"),
    ("commctrl.h", "PGN_", "notification", "SysPager"),
    ("commctrl.h", "ACS_", "style", "SysAnimate32"),
    ("commctrl.h", "ACM_", "message", "SysAnimate32"),
    ("commctrl.h", "ACN_", "notification", "SysAnimate32"),
    ("commctrl.h", "CBES_EX_", "exstyle", "ComboBoxEx32"),
    ("commctrl.h", "CBEM_", "message", "ComboBoxEx32"),
    ("commctrl.h", "CBEN_", "notification", "ComboBoxEx32"),
    ("commctrl.h", "LWS_", "style", "SysLink"),
    ("commctrl.h", "LM_", "message", "SysLink"),
    # dialogs and system UI
    ("commctrl.h", "TDF_", "flag", "TaskDialog"),
    ("commctrl.h", "TDCBF_", "flag", "TaskDialog"),
    ("commctrl.h", "TDM_", "message", "TaskDialog"),
    ("commctrl.h", "TDN_", "notification", "TaskDialog"),
    ("prsht.h", "PSH_", "flag", "PropertySheet"),
    ("prsht.h", "PSP_", "flag", "PropertySheet"),
    ("prsht.h", "PSM_", "message", "PropertySheet"),
    ("prsht.h", "PSN_", "notification", "PropertySheet"),
    ("winuser.h", "MB_", "flag", "MessageBox"),
    ("commdlg.h", "OFN_", "flag", "FileDialog"),
    ("shobjidl.idl", "FOS_", "flag", "FileDialog"),
    ("commdlg.h", "CDN_", "notification", "FileDialog"),
    ("commdlg.h", "CC_", "flag", "ChooseColor"),
    ("commdlg.h", "CF_", "flag", "ChooseFont"),
    ("commdlg.h", "PD_", "flag", "PrintDlg"),
    ("commdlg.h", "PSD_", "flag", "PageSetupDlg"),
    ("commdlg.h", "FR_", "flag", "FindReplace"),
    ("shlobj.h", "BIF_", "flag", "BrowseForFolder"),
    ("winuser.h", "MF_", "flag", "Menu"),
    ("winuser.h", "MFT_", "flag", "Menu"),
    ("winuser.h", "MFS_", "flag", "Menu"),
    ("winuser.h", "TPM_", "flag", "Menu"),
    ("shellapi.h", "NIF_", "flag", "NotifyIcon"),
    ("shellapi.h", "NIIF_", "flag", "NotifyIcon"),
    ("shellapi.h", "NIS_", "flag", "NotifyIcon"),
    ("shellapi.h", "NIN_", "notification", "NotifyIcon"),
    ("shobjidl.idl", "TBPF_", "flag", "TaskbarList"),
    ("winuser.h", "FLASHW_", "flag", "FlashWindow"),
    ("winuser.h", "COLOR_", "flag", "SystemColors"),
]

# prefixes that would otherwise swallow a longer, separately listed one
LONGER = {
    "WS_": ["WS_EX_"], "LVS_": ["LVS_EX_"], "TVS_": ["TVS_EX_"], "TCS_": ["TCS_EX_"],
    "TBSTYLE_": ["TBSTYLE_EX_"], "PBS_": ["PBST_"], "TB_": [], "SB_": [], "PSD_": [],
    "CB_": [], "LB_": [],
}

# dialog / system entry points: (spec or idl, name regex, owner)
APIS = [
    ("dlls/user32/user32.spec", r"MessageBox\w*", "MessageBox"),
    ("dlls/user32/user32.spec", r"MessageBeep", "MessageBox"),
    ("dlls/user32/user32.spec", r"(DialogBox|CreateDialog)\w*", "#32770"),
    ("dlls/user32/user32.spec", r"(SetMenu|TrackPopupMenu\w*|GetSystemMenu|CreateMenu|CreatePopupMenu)", "Menu"),
    ("dlls/user32/user32.spec", r"FlashWindow\w*", "FlashWindow"),
    ("dlls/comctl32_v6/comctl32_v6.spec", r"TaskDialog\w*", "TaskDialog"),
    ("dlls/comctl32/comctl32.spec", r"(PropertySheet\w*|CreatePropertySheetPage\w*)", "PropertySheet"),
    ("dlls/comdlg32/comdlg32.spec", r"(GetOpenFileName\w*|GetSaveFileName\w*)", "FileDialog"),
    ("dlls/comdlg32/comdlg32.spec", r"ChooseColor\w*", "ChooseColor"),
    ("dlls/comdlg32/comdlg32.spec", r"ChooseFont\w*", "ChooseFont"),
    ("dlls/comdlg32/comdlg32.spec", r"PrintDlg\w*", "PrintDlg"),
    ("dlls/comdlg32/comdlg32.spec", r"PageSetupDlg\w*", "PageSetupDlg"),
    ("dlls/comdlg32/comdlg32.spec", r"(FindText|ReplaceText)\w*", "FindReplace"),
    ("dlls/shell32/shell32.spec", r"SHBrowseForFolder\w*", "BrowseForFolder"),
    ("dlls/shell32/shell32.spec", r"ShellAbout\w*", "ShellAbout"),
    ("dlls/shell32/shell32.spec", r"Shell_NotifyIcon\w*", "NotifyIcon"),
    ("dlls/shell32/shell32.spec", r"SHFileOperation\w*", "FileOperation"),
    ("dlls/shell32/shell32.spec", r"(RunFileDlg|PickIconDlg|RestartDialog\w*)", "ShellDialogs"),
]
COM = [
    ("include/shobjidl.idl", "IFileOpenDialog", "FileDialog"),
    ("include/shobjidl.idl", "IFileSaveDialog", "FileDialog"),
    ("include/shobjidl.idl", "IFileOperation", "FileOperation"),
    ("include/shlobj.h", "IProgressDialog", "ProgressDialog"),
    ("include/shobjidl.idl", "IProgressDialog", "ProgressDialog"),
    ("include/shobjidl.idl", "ITaskbarList3", "TaskbarList"),
    ("include/windows.ui.notifications.idl", "IToastNotifier", "Notifications"),
]

NCM_FONTS = ["lfCaptionFont", "lfSmCaptionFont", "lfMenuFont", "lfStatusFont", "lfMessageFont"]


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def collect(tree, header, prefix, skip):
    path = os.path.join(tree, "include", header)
    text = read(path)
    rh = os.path.splitext(path)[0] + ".rh"  # winuser.rh etc. hold the resource-visible bits
    if os.path.exists(rh):
        text += "\n" + read(rh)
    out = {}
    if header.endswith(".idl"):
        pat = re.compile(r"^\s*(" + re.escape(prefix) + r"\w+)\s*=\s*([^,\n]+)", re.M)
    else:
        pat = re.compile(r"^\s*#\s*define\s+(" + re.escape(prefix) + r"\w+)[ \t]+([^\n]*)", re.M)
    found = pat.findall(text)
    if not header.endswith(".idl"):
        # C enums carry some groups (TaskDialog's TDF_/TDM_/TDN_)
        for body in re.findall(r"\benum\b[^{;]*\{([^}]*)\}", text):
            for name, value in re.findall(r"^\s*(" + re.escape(prefix) + r"\w+)\s*(?:=\s*([^,\n]+))?", body, re.M):
                found.append((name, value or "(enum)"))
    for name, value in found:
        if any(name.startswith(s) for s in skip):
            continue
        value = re.sub(r"/\*.*?\*/|//.*", "", value).strip()
        out.setdefault(name, value)
    # fold A/W pairs onto their neutral name
    for name in list(out):
        if name[-1] in "AW" and name[:-1] in out:
            del out[name]
        elif name[-1] == "W" and name[:-1] + "A" in out:
            base = name[:-1]
            out[base] = out.pop(name)
            out.pop(base + "A", None)
    return out


def classes(tree):
    res = []
    text = read(os.path.join(tree, "dlls/win32u/class.c"))
    table = text[text.index("builtin_classes[] ="):]
    table = table[:table.index("};")]
    for proc, name in re.findall(r"\[NTUSER_WNDPROC_(\w+)\]\s*=\s*\{\s*\.name\s*=\s*\"([^\"]+)\"", table):
        res.append({"name": name, "registered_by": "win32u", "wndproc": "NTUSER_WNDPROC_" + proc})
    consts = {}
    for h in ("commctrl.h", "winuser.h"):
        for n, v in re.findall(r"#\s*define\s+(\w+)\s+L\"([^\"]+)\"", read(os.path.join(tree, "include", h))):
            consts.setdefault(n, v)
    for dll in ("comctl32", "comctl32_v6"):
        d = os.path.join(tree, "dlls", dll)
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".c"):
                continue
            for c in re.findall(r"lpszClassName\s*=\s*(L\"[^\"]+\"|\w+)", read(os.path.join(d, fn))):
                name = c[2:-1] if c.startswith('L"') else consts.get(c, c)
                res.append({"name": name, "registered_by": dll, "source": f"dlls/{dll}/{fn}"})
    return res


def apis(tree):
    res = []
    for spec, rx, owner in APIS:
        text = read(os.path.join(tree, spec))
        names = set()
        for line in text.splitlines():
            m = re.match(r"^\s*(?:@|\d+)\s+\w+\s+(?:-\w+\s+)*(?:\w+\()?[^()]*?\b(" + rx + r")\b", line)
            if m and not line.lstrip().startswith("#"):
                names.add(m.group(1))
        for n in sorted(names):
            # fold ANSI/Unicode twins onto the neutral name
            base = n[:-4] if n.endswith("AorW") else n[:-1] if n[-1] in "AW" and (n[:-1] + ("W" if n[-1] == "A" else "A")) in names else n
            if any(r["name"] == base for r in res):
                continue
            res.append({"name": base, "owner": owner, "spec": spec})
    seen = set()
    for idl, iface, owner in COM:
        text = read(os.path.join(tree, idl))
        if re.search(r"\binterface\s+" + iface + r"\b|DECLARE_INTERFACE_?\(\s*" + iface + r"\b", text) and iface not in seen:
            seen.add(iface)
            res.append({"name": iface, "owner": owner, "com": True, "declared_in": idl})
    for _, iface, _ in COM:
        if iface not in seen:
            print(f"warning: {iface} not declared in the tree", file=sys.stderr)
    return res


def visual(tree):
    text = read(os.path.join(tree, "include/vsstyle.h"))
    classes_ = sorted(set(re.findall(r"VSCLASS_(\w+)\b", text)))
    enums = re.findall(r"enum\s+(\w+)\s*\{([^}]*)\}", text)
    parts, states = {}, {}
    for name, body in enums:
        members = re.findall(r"(\w+)\s*=", body)
        if name.endswith("PARTS"):
            parts[name[:-5]] = members
        else:
            states[name] = members
    return {"classes": classes_, "parts": parts, "states": states}


def main():
    tree = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..", "map", "win32-inventory.yaml")
    head = subprocess.run(["git", "-C", tree, "log", "-1", "--format=%h %s"], capture_output=True, text=True).stdout.strip()
    version = read(os.path.join(tree, "VERSION")).strip()

    groups = []
    for header, prefix, kind, owner in GROUPS:
        skip = LONGER.get(prefix, [])
        # a prefix must not swallow names of other listed groups from the same header
        skip = skip + [p for h, p, _, _ in GROUPS if h == header and p != prefix and p.startswith(prefix)]
        names = collect(tree, header, prefix, skip)
        if not names:
            sys.exit(f"no {prefix}* in {header}: inventory spec is stale")
        same = next((g for g in groups if (g["prefix"], g["kind"], g["owner"]) == (prefix, kind, owner)), None)
        if same:  # e.g. BS_ lives in both winuser.rh and commctrl.h
            same["header"] += ", " + header
            for n, v in names.items():
                same["names"].setdefault(n, v)
            same["names"] = dict(sorted(same["names"].items()))
            continue
        groups.append({"prefix": prefix, "header": header, "kind": kind, "owner": owner,
                       "names": dict(sorted(names.items()))})

    inv = {
        "generated_by": "tools/scan_win32.py",
        "wine": {"version": version, "commit": head},
        "classes": classes(tree),
        "groups": groups,
        "apis": apis(tree),
        "visual": visual(tree),
        "nonclient_fonts": NCM_FONTS,
    }
    with open(out, "w") as f:
        yaml.safe_dump(inv, f, sort_keys=False, width=120, allow_unicode=True)
    must = sum(len(g["names"]) for g in groups if g["kind"] in ("style", "exstyle", "flag"))
    info = sum(len(g["names"]) for g in groups if g["kind"] not in ("style", "exstyle", "flag"))
    nparts = sum(len(v) for v in inv["visual"]["parts"].values())
    print(f"{out}: {len(inv['classes'])} class registrations, {must} style/flag bits, "
          f"{info} messages/notifications, {len(inv['apis'])} entry points, {nparts} visual parts")


if __name__ == "__main__":
    main()
