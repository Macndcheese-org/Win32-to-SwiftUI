# HIG audit of the conversions

Each conversion in the map checked against Apple's Human Interface Guidelines
(macOS sections; local copy `~/Downloads/hig/apple-hig-markdown`), 2026-09-28.
The rule of the project: a real macOS component, never a copy of one.

## Conforms

| Area | HIG | What the runtime does |
|---|---|---|
| Push buttons | push button, default button, ellipsis when more input follows | NSButton / SwiftUI Button; BS_DEFPUSHBUTTON is the default button; "..." shows as "…" |
| Checkboxes, radio buttons | checkboxes and radio buttons in the window body, never switches in place of checkboxes | checkboxes stay checkboxes, radios stay radios |
| Group boxes | "By default, macOS displays a box's title above it" | NSBox, title above when there is room |
| Tab views | at most six tabs; no pop-up to switch tabs | NSTabView up to 6 pages |
| Settings windows | a familiar layout, labels and controls in columns | property sheets: a tab view across a wide window, each page laid out again as a columns-style form (as Safari's settings); a toolbar of panes needs icons, which Win32 pages don't have |
| Pop-up buttons | flat list of exclusive options | drop-down lists are NSPopUpButton |
| Combo boxes | text input paired with a list | editable combos are NSComboBox |
| Segmented controls | switching views in the window body is a tab view's job | TCS_BUTTONS tabs only become segmented controls |
| Sliders | tick marks where the app has them; vertical only when needed | NSSlider with the trackbar's ticks, vertical NSSlider for TBS_VERT |
| Steppers | Shift-click for large ranges | Shift-click steps by 10 |
| Progress | determinate when possible; bar or spinner | NSProgressIndicator; marquee is indeterminate |
| Scroll bars | the system's scrollers | NSScroller where Windows has a scroll bar; the app's own scrolling (wheel, keys) is unchanged |
| Tables | sortable headings, resizable columns, alternating rows | SwiftUI Table; a heading click is LVN_COLUMNCLICK (the app sorts) |
| Outline views | hierarchy in the first column, keep expansion | tree views are outlines with the app's expansion state |
| Text fields | hints, secure fields | EM_SETCUEBANNER is the placeholder; ES_PASSWORD is a SecureField |
| Text views | a document's text fills its window | a document window's multi-line edit has no border, as TextEdit's |
| Popovers | small, transient information near its source | edit balloon tips are popovers on the field |
| Alerts | NSAlert, button order, Escape cancels | MessageBox is NSAlert (sheet on its owner) |
| Panels | prefer the standard panels | Open/Save, Colours, Fonts, Print, Page Setup, Find are AppKit's own |
| Menus | the menu bar, key equivalents, check marks, ellipsis | the window's menu is the Mac menu bar; Ctrl becomes Command; "..." shows as "…" |
| About | the standard About panel | ShellAbout is AppKit's About panel |
| Dock | progress and attention on the Dock icon | taskbar progress is on the Dock tile; FlashWindowEx bounces the icon |

## Fixed in this audit

- Titles ending in "..." show macOS's ellipsis character ("Font…").
- Steppers: Shift-click steps by 10.
- A document window's text view (notepad's) has no border.
- Settings windows (property sheets, winecfg) are a tab view over a settings form in
  a landscape window, instead of a sidebar; the page's controls are laid out again as
  a macOS form.
- A toolbar across the top of an app's main window is in the window's frame; toolbars
  stacked there (winefile's drive bar and window buttons) share it, a group each, with
  the text they show (drive letters) beside their symbols.
- Every icon and toolbar image of wine's (213 icons, 124 strip images: shell32, comctl32,
  comdlg32, wine's programs, IE's and the help viewer's toolbars, cryptui, aclui) is an
  SF Symbol, a Finder icon or an AppKit image with the same meaning; report lists show
  their items' icons, as Finder's list view does.
- A tree along a main window's leading edge (regedit's keys) is the window's real
  sidebar (a split view's sidebar item, full height), not a list styled as one.
- The menu bar has one Edit menu (the app's, with the Mac's text commands when it has
  none) and the Help menu last.
- Windows that scroll themselves (WS_VSCROLL/WS_HSCROLL on a view of the app's own, an
  owner-drawn list) and the ScrollBar control have NSScrollers instead of Windows
  scroll bars.

## Left open (need a decision or a screen)

1. **Alert button titles.** HIG: avoid "OK" as the default unless the alert
   only informs. MessageBox's buttons are the app's choice (MB_OKCANCEL...);
   the titles stay as Windows gives them.
2. **Status bars.** HIG: avoid critical information in a bottom bar. Status
   bars are the app's; they show as a macOS bottom bar. Nothing to change.
