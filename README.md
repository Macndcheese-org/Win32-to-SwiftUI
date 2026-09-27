# Win32-to-SwiftUI

A translation layer for MacNdCheese wine, in the spirit of DXMT. When a Windows program
uses the Win32 UI framework (the `user32` controls, the Common Controls in `comctl32`, the
common dialogs, menus), it should get SwiftUI instead and look like a real Mac app.

**Status: step 1, the map.** This repository holds the translation map: every Windows UI
element that wine implements, linked to its SwiftUI equivalent for each macOS version
from 12 to 27, plus the tools that prove the map is complete and that its Swift code
compiles. The runtime layer that uses the map comes next (see "Runtime design" below).

Read the result in [UI-MAP.md](UI-MAP.md).

## What gets translated

Everything an app creates through the UI framework: window classes and their style
variants, dialogs, menus and system UI. What an app paints itself (owner-draw controls,
custom-drawn windows, game UIs) has no control behind it to translate, so it stays as the
app drew it, the same as custom UI in a Mac app. The map marks those `not_translated`.

## The tier rule

For each element and each macOS version, use what Apple's own apps use for that role on
that version. The floor is macOS 12; each entry lists code per tier (`since: 12`, `13`,
... `27`) and the newest tier at or below the running macOS wins.

Liquid Glass (macOS 26+) is mostly automatic: the wine loader is linked against the 26+
SDK, so standard controls (buttons, pop-ups, sliders, menus, popovers, sidebars) turn to
glass by themselves. The explicit glass APIs (`.glassEffect`, `GlassEffectContainer`,
`.buttonStyle(.glass)`/`.glassProminent`) are used only where Apple uses them: toolbar
buttons and controls floating over content.

Where SwiftUI has no equivalent (editable combo box, font panel, print panel, menu bar,
scroll bar), the entry uses the AppKit view, usually the one SwiftUI itself draws with.
Where macOS has no counterpart at all, the entry is `improvised: true`.

## Layout

```
map/win32-inventory.yaml     generated: every Windows UI element in the wine tree
map/swiftui-inventory.yaml   generated: every public SwiftUI view, style member and modifier
                             (+ the AppKit classes the map uses), with its macOS version and doc
map/ui-map/*.yaml            hand-written: the links (source of truth), one file per area
UI-MAP.md                    generated: readable tables
tools/scan_win32.py          builds the Win32 inventory from a wine tree
tools/scan_swiftui.py        builds the SwiftUI inventory with swift-symbolgraph-extract
tools/check_map.py           gate: coverage, macOS-12 paths, tier sanity
tools/typecheck_snippets.py  gate: compiles every snippet at the macOS 12 floor
tools/render_map.py          writes UI-MAP.md
tools/quote_yaml.py          authoring helper: quotes code/prose values in the YAML
```

## Regenerating

```bash
python3 tools/scan_win32.py <wine tree>     # after a wine base bump
python3 tools/scan_swiftui.py               # after an Xcode/SDK update (takes ~2 min)
python3 tools/check_map.py                  # must print "map OK"
python3 tools/typecheck_snippets.py         # must print N/N
python3 tools/render_map.py
```

A new wine or SDK version shows up as check failures (new style bits, new parts) or as
new rows in the "SwiftUI with no Windows counterpart" table.

## Map schema

An entry:

```yaml
- id: button.default
  kind: control                 # control | dialog | menu | system
  title: Default push button
  win32: {class: Button, styles: [BS_DEFPUSHBUTTON], apis: [], parts: [BUTTON/BP_PUSHBUTTON]}
  runtime: swiftui              # swiftui | appkit | existing | not_translated
  swiftui:                      # View expressions, one per tier
    - {since: 12, code: 'Button(title, action: action).keyboardShortcut(.defaultAction)'}
  appkit:                       # Swift statements, one per tier (optional)
    - {since: 12, code: '...'}
  glass: automatic              # explicit | automatic | none
  state_in: [WM_SETTEXT, ...]   # Windows messages the view must follow
  events_out: {activate: WM_COMMAND/BN_CLICKED}   # what the view sends back
  not_translated_when: [BS_OWNERDRAW]
  improvised: false
  notes: ...
```

Every style/flag bit, visual part, class, entry point or font that isn't an entry's
variant gets a disposition under `flags:`, `parts:`, `classes:`, `apis:` or `fonts:`:
`entry`, `mod` (a SwiftUI modifier on the entry's view), `view` (a replacement view),
`appkit`, `nscolor`, `nsfont`, `existing` (winemac already does it), `behavior` (bridge
logic, nothing to draw), `ignore` (no macOS counterpart needed, with the reason) or
`not_translated`. A `mod`/`view` newer than 12 carries `since:` and a `fallback:` (or an
`appkit:` alternative).

Snippet placeholders (declared by `typecheck_snippets.py`): `title`, `note`, `text`,
`isOn`, `flags`, `value`, `intValue`, `date`, `selection` (`Int?`), `multiSelection`
(`Set<Int>`), `choice`, `color`, `isPresented`, `sortOrder`, `rows` (`[Row]`), `tree`
(`[Node]`), `choices` (`[Choice]`), `image` (`NSImage`), `attributed`, `action()`; in
AppKit code also `window`, `view`, `menu`, `items`.

## Walkthrough: winecfg's Applications tab

winecfg is `PropertySheetW` with `PSH_PROPSHEETPAGE | PSH_USEICONID | PSH_USECALLBACK`
and 8 pages; the Applications page is `IDD_APPCFG` in `programs/winecfg/winecfg.rc`.

| winecfg piece | Win32 | Entry | Becomes |
|---|---|---|---|
| The sheet, 8 pages | `PropertySheetW` | `propsheet` | macOS 13+: a System Settings-style sidebar with the 8 page names (glass on 26); macOS 12: tabs |
| Title-bar icon | `PSH_USEICONID` | flag | dropped (macOS windows have no title icons) |
| The page | `WS_CHILD` dialog | `dialog` | window background colour; controls translated one by one |
| "Application settings" frame | `GROUPBOX` | `button.groupbox` | `GroupBox` |
| Explanation text | `LTEXT` | `static.text` | `Text` in the system font |
| Application list | `SysListView32` `LVS_LIST \| LVS_SINGLESEL \| LVS_SHOWSELALWAYS` | `listview.list` | `List` with single selection; SHOWSELALWAYS is macOS's own behaviour |
| "Add application..." / "Remove application" | `PUSHBUTTON` | `button.push` | bordered `Button`s; the `&` mnemonics are dropped |
| "Windows Version:" | `LTEXT` | `static.text` | `Text` |
| Version box | `COMBOBOX` `CBS_DROPDOWNLIST` | `combobox.dropdownlist` | pop-up button (`Picker` `.menu`) |
| OK / Cancel / Apply | property sheet buttons | `propsheet` | standard buttons, OK as the default (Return) and Cancel on Escape |
| Title bar | `WS_CAPTION` | `window.frame` | already native today |

## Runtime design (next step, not built yet)

- **Split like DXMT.** Wine gets small hooks:
  - the control window procedures (user32, comctl32, comctl32_v6)
  - MessageBox, TaskDialog, comdlg32 and shell32 entry points
  - win32u menus

  Each hook reaches the unix side through a new winemac `ExtEscape` code (next to
  `MACDRV_ESCAPE_GET_SURFACE`). That side calls `libWin32ToSwiftUI.dylib`: Swift with a
  C ABI, x86_64, deployment target macOS 12, dlopen'd by `winemac.so`. Without the dylib,
  everything stays exactly as today.
- **Placement.** A translated control is an `NSHostingView` inside the window's
  `WineContentView`, at the control's rect (winemac's child-view machinery,
  `macdrv_create_view`), kept in sync on WindowPosChanged.
- **State and events.**
  - The Win32 control keeps the authoritative state, so synchronous queries
    (`BM_GETCHECK`, `EM_GETSEL`) still work.
  - State is mirrored into an `ObservableObject`. `@Observable` needs macOS 14, so it
    can't be the floor.
  - Native events come back through the macdrv event queue as the usual
    `WM_COMMAND`/`WM_NOTIFY`, so an app's own subclasses still see them.
- **Shipping.** `wine-unified/win32-to-swiftui/`, next to `mnc-d3d`, behind
  `WINE_MNC_NATIVE_UI` and a per-app setting, off by default until verified.

## Known open points

- **Notifications.** Tray balloons and toasts use `UNUserNotificationCenter`, which
  needs the process to belong to an app bundle. Verify from the wine loader inside the
  MacNdCheese app.
- **Menu bar.** Moving a window's menu to the Mac menu bar makes its client area taller
  by the menu height. That is the one layout change, so it is a per-app switch.
- **Toolbars.** They are drawn in place by default. Moving them into the unified title
  bar (`NSToolbar`) is opt-in for the same reason as the menu bar.
- **Engine binaries.** The wine binaries themselves are currently built with a minimum of
  macOS 27, so the engine as a whole doesn't yet honour the macOS 12 floor that this
  layer targets.
