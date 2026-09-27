// The SwiftUI view of each translated control, one per map entry
// (map/ui-map/*.yaml). Tiers follow the map's `since` ladder.
import AppKit
import SwiftUI

enum ControlViews {
    // map: button.push button.default button.checkbox button.pushlike button.radio button.groupbox
    // map: button.3state button.split button.commandlink
    // map: static.text static.separator static.image edit.single edit.password edit.number edit.readonly
    // map: combobox.dropdownlist listbox.single listbox.multi listview.list listview.report
    // map: progress trackbar tab static.frame statusbar edit.multiline combobox.editable treeview
    // map: updown datetime monthcal tooltip (as .help on every control: HelpText)
    // map: listview.checkboxes listview.icon
    // map: propsheet propsheet.wizard (modes of the sheet's tab control: WindowSidebar, WizardSteps)
    static let entries: Set<String> = [
        "button.push", "button.default", "button.checkbox", "button.pushlike", "button.radio", "button.groupbox",
        "button.3state", "button.split", "button.commandlink",
        "static.text", "static.separator", "static.image",
        "edit.single", "edit.password", "edit.number", "edit.readonly",
        "combobox.dropdownlist", "listbox.single", "listbox.multi", "listview.list", "listview.report",
        "progress", "trackbar", "tab", "static.frame", "statusbar", "edit.multiline", "combobox.editable",
        "treeview", "updown", "datetime", "monthcal", "listview.checkboxes", "listview.icon",
    ]

    static func supports(_ entry: String) -> Bool { entries.contains(entry) }

    static func root(for model: ControlModel, entry: String) -> AnyView {
        AnyView(ControlRoot(model: model, entry: entry))
    }
}

/// map: tooltip. A translated control that is a tooltip tool shows the tool's
/// text the macOS way, after the usual hover delay.
struct HelpText: ViewModifier {
    let text: String?

    func body(content: Content) -> some View {
        if let text = text, !text.isEmpty {
            content.help(text)
        } else {
            content
        }
    }
}

/// Win32 text uses '&' for access keys; macOS has none. "&&" is a literal '&'.
func stripMnemonic(_ text: String, keep: Bool = false) -> String {
    if keep { return text }
    var out = ""
    var chars = text.makeIterator()
    while let c = chars.next() {
        if c == "&" {
            if let n = chars.next() { out.append(n) }
        } else {
            out.append(c)
        }
    }
    return out
}

/// Sizes the SwiftUI control like the Win32 one: the app laid its dialog out
/// for its font (usually 8pt MS Shell Dlg, 11px), so the native control uses
/// the system font at that size and the matching control size.
struct Metrics {
    var fontSize: CGFloat
    var controlSize: ControlSize
    var scale: CGFloat          // points per Win32 pixel

    init(snap: Snapshot, scale: CGFloat) {
        self.scale = scale
        let px = CGFloat(snap.fontPx ?? 11)
        fontSize = max(8, px * scale)
        controlSize = fontSize <= 10 ? .mini : fontSize <= 12 ? .small : .regular
    }
}

struct ControlRoot: View {
    @ObservedObject var model: ControlModel
    let entry: String

    var body: some View {
        GeometryReader { geo in
            let snap = model.snap
            let scale = geo.size.height / CGFloat(max(1, snap.heightPx ?? Double(geo.size.height)))
            let metrics = Metrics(snap: snap, scale: scale.isFinite && scale > 0 ? scale : 1)
            content(snap: snap, metrics: metrics)
                .modifier(HelpText(text: snap.help))
                .font(.system(size: metrics.fontSize))
                .controlSize(metrics.controlSize)
                // a wizard's tab control is disabled; its steps only show where the wizard is
                .disabled(!(snap.enabled ?? true) && snap.mode != "wizard")
                .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
        }
    }

    @ViewBuilder
    func content(snap: Snapshot, metrics: Metrics) -> some View {
        let label = stripMnemonic(snap.display ?? snap.text ?? "", keep: snap.noPrefix ?? false)
        switch entry {
        case "button.push", "button.default":
            PushButton(model: model, label: label, prominent: snap.isDefault ?? (entry == "button.default"))
        case "button.checkbox":
            CheckBox(model: model, label: label)
        case "button.pushlike":
            PushLike(model: model, label: label)
        case "button.radio":
            RadioButton(model: model, label: label, fontSize: metrics.fontSize, controlSize: metrics.controlSize)
        case "button.3state":
            TriStateBox(model: model, label: label, fontSize: metrics.fontSize, controlSize: metrics.controlSize)
        case "button.split":
            SplitButton(model: model, label: label, fontSize: metrics.fontSize, controlSize: metrics.controlSize)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        case "button.commandlink":
            CommandLink(model: model, label: label)
        case "button.groupbox":
            NativeGroupBox(label: label, above: (snap.titleAbove ?? false) && !label.isEmpty)
                .allowsHitTesting(false)
        case "static.text":
            StaticText(snap: snap, label: label)
        case "static.separator":
            Rectangle().fill(Color(nsColor: .separatorColor))
                .frame(width: (snap.vertical ?? false) ? 1 : nil, height: (snap.vertical ?? false) ? nil : 1)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .allowsHitTesting(false)
        case "static.image":
            StaticImage(snap: snap)
        case "static.frame":
            StaticFrame(fill: snap.fill ?? "none")
        case "statusbar":
            StatusBar(model: model, scale: metrics.scale)
        case "edit.single", "edit.password", "edit.number":
            EditField(model: model, secure: entry == "edit.password")
        case "edit.multiline":
            MultilineEdit(model: model, fontSize: metrics.fontSize, scale: metrics.scale)
        case "edit.readonly":
            Text(snap.text ?? "").textSelection(.enabled)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
        case "combobox.dropdownlist":
            DropDownList(model: model, fontSize: metrics.fontSize, controlSize: metrics.controlSize)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        case "combobox.editable":
            EditableCombo(model: model, fontSize: metrics.fontSize, controlSize: metrics.controlSize)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
        case "listbox.single", "listbox.multi":
            ListBoxView(model: model, multi: entry == "listbox.multi")
        case "listview.list":
            ListBoxView(model: model, multi: !(snap.single ?? false), fromRows: true)
        case "treeview":
            TreeList(model: model)
        case "listview.report":
            ReportView(model: model, scale: metrics.scale)
        case "listview.checkboxes":
            if snap.report ?? true {
                ReportView(model: model, scale: metrics.scale)
            } else {
                ListBoxView(model: model, multi: !(snap.single ?? false), fromRows: true)
            }
        case "listview.icon":
            IconGrid(model: model)
        case "progress":
            ProgressBar(snap: snap)
        case "trackbar":
            TrackBar(model: model)
        case "tab":
            TabStrip(model: model)
        case "updown":
            Stepper("", onIncrement: { model.emit(["t": "step", "v": 1]) },
                    onDecrement: { model.emit(["t": "step", "v": -1]) })
                .labelsHidden()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        case "datetime":
            DateTimePicker(model: model, graphical: false)
        case "monthcal":
            DateTimePicker(model: model, graphical: true)
        default:
            EmptyView()
        }
    }
}

// MARK: - buttons (map: button.*)

struct PushButton: View {
    @ObservedObject var model: ControlModel
    let label: String
    let prominent: Bool

    var body: some View {
        // Return stays with the Win32 dialog (IsDialogMessage), so no
        // .keyboardShortcut(.defaultAction) here; the look is the default button's.
        if prominent {
            Button(action: click) { Text(label).lineLimit(1).frame(maxWidth: .infinity) }
                .buttonStyle(.borderedProminent)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            Button(action: click) { Text(label).lineLimit(1).frame(maxWidth: .infinity) }
                .buttonStyle(.bordered)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    func click() { model.emit(["t": "click"]) }
}

struct CheckBox: View {
    @ObservedObject var model: ControlModel
    let label: String

    var body: some View {
        Toggle(isOn: Binding(
            get: { (model.snap.checked ?? 0) == 1 },
            set: { on in
                model.snap.checked = on ? 1 : 0     // shown now; the Win32 answer follows
                model.emit(["t": "click"])
            })) {
            Text(label).lineLimit(1)
        }
        .toggleStyle(.checkbox)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
    }
}

struct PushLike: View {
    @ObservedObject var model: ControlModel
    let label: String

    var body: some View {
        Toggle(isOn: Binding(
            get: { (model.snap.checked ?? 0) == 1 },
            set: { _ in model.emit(["t": "click"]) })) {
            Text(label).lineLimit(1).frame(maxWidth: .infinity)
        }
        .toggleStyle(.button)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// One Win32 radio button is one window, so it is one native radio button
/// (NSButton's radio style: SwiftUI has no single radio). The group behaviour
/// stays with the Win32 buttons (BS_AUTORADIOBUTTON unchecks the others).
struct RadioButton: NSViewRepresentable {
    @ObservedObject var model: ControlModel
    let label: String
    let fontSize: CGFloat
    let controlSize: ControlSize

    final class Coordinator: NSObject {
        var model: ControlModel
        init(model: ControlModel) { self.model = model }
        @objc func clicked(_ sender: NSButton) { model.emit(["t": "click"]) }
    }

    func makeCoordinator() -> Coordinator { Coordinator(model: model) }

    func makeNSView(context: Context) -> NSButton {
        let button = NSButton(radioButtonWithTitle: label, target: context.coordinator,
                              action: #selector(Coordinator.clicked(_:)))
        button.lineBreakMode = .byTruncatingTail
        return button
    }

    func updateNSView(_ button: NSButton, context: Context) {
        context.coordinator.model = model
        button.title = label
        button.font = NSFont.systemFont(ofSize: fontSize)
        button.controlSize = nsControlSize(controlSize)
        button.state = (model.snap.checked ?? 0) == 1 ? .on : .off
        button.isEnabled = model.snap.enabled ?? true
    }
}

/// BS_3STATE: a Toggle(sources:) whose sources disagree shows the mixed dash
/// (13+), so an indeterminate button is fed [true, false]; macOS 12 uses
/// NSButton's mixed state. The Win32 button cycles its own state
/// (BS_AUTO3STATE) or the app sets it: the view only sends the click.
struct TriStateBox: View {
    @ObservedObject var model: ControlModel
    let label: String
    let fontSize: CGFloat
    let controlSize: ControlSize

    var body: some View {
        Group {
            if #available(macOS 13, *) {
                Toggle(sources: Binding(
                    get: { model.snap.checked == 2 ? [true, false] : [(model.snap.checked ?? 0) == 1] },
                    set: { (_: [Bool]) in model.clickOnce() }),
                       isOn: \.self) {
                    Text(label).lineLimit(1)
                }
                .toggleStyle(.checkbox)
            } else {
                MixedCheckBox(model: model, label: label, fontSize: fontSize, controlSize: controlSize)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
    }
}

struct MixedCheckBox: NSViewRepresentable {
    @ObservedObject var model: ControlModel
    let label: String
    let fontSize: CGFloat
    let controlSize: ControlSize

    final class Coordinator: NSObject {
        var model: ControlModel
        init(model: ControlModel) { self.model = model }
        @objc func clicked(_ sender: NSButton) { model.emit(["t": "click"]) }
    }

    func makeCoordinator() -> Coordinator { Coordinator(model: model) }

    func makeNSView(context: Context) -> NSButton {
        let button = NSButton(checkboxWithTitle: label, target: context.coordinator,
                              action: #selector(Coordinator.clicked(_:)))
        button.allowsMixedState = true
        button.lineBreakMode = .byTruncatingTail
        return button
    }

    func updateNSView(_ button: NSButton, context: Context) {
        context.coordinator.model = model
        button.title = label
        button.font = NSFont.systemFont(ofSize: fontSize)
        button.controlSize = nsControlSize(controlSize)
        switch model.snap.checked ?? 0 {
        case 1: button.state = .on
        case 2: button.state = .mixed
        default: button.state = .off
        }
        button.isEnabled = model.snap.enabled ?? true
    }
}

/// BS_SPLITBUTTON. The arrow half raises BCN_DROPDOWN and the app shows its own
/// menu (TrackPopupMenu, native too: menu.popup), so the arrow can't open a
/// SwiftUI Menu whose items would have to be known beforehand: the button is a
/// momentary two-segment control, the main part and a chevron.
struct SplitButton: NSViewRepresentable {
    @ObservedObject var model: ControlModel
    let label: String
    let fontSize: CGFloat
    let controlSize: ControlSize
    static let arrowWidth: CGFloat = 22

    final class Coordinator: NSObject {
        var model: ControlModel
        init(model: ControlModel) { self.model = model }
        @objc func pressed(_ sender: NSSegmentedControl) {
            let arrow = sender.selectedSegment == 1 || (model.snap.noSplit ?? false)
            model.emit(["t": arrow ? "dropdown" : "click"])
        }
    }

    /// The main segment takes the width the arrow leaves.
    final class Control: NSSegmentedControl {
        override func setFrameSize(_ newSize: NSSize) {
            super.setFrameSize(newSize)
            let main = max(24, newSize.width - SplitButton.arrowWidth - 8)
            if segmentCount == 2 && abs(width(forSegment: 0) - main) > 0.5 { setWidth(main, forSegment: 0) }
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator(model: model) }

    func makeNSView(context: Context) -> NSSegmentedControl {
        let control = Control(labels: [label, ""], trackingMode: .momentary, target: context.coordinator,
                              action: #selector(Coordinator.pressed(_:)))
        control.setImage(NSImage(systemSymbolName: "chevron.down", accessibilityDescription: nil), forSegment: 1)
        control.setWidth(SplitButton.arrowWidth, forSegment: 1)
        control.setContentHuggingPriority(.defaultLow, for: .horizontal)
        control.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return control
    }

    func updateNSView(_ control: NSSegmentedControl, context: Context) {
        context.coordinator.model = model
        control.setLabel(label, forSegment: 0)
        control.font = NSFont.systemFont(ofSize: fontSize)
        control.controlSize = nsControlSize(controlSize)
        control.isEnabled = model.snap.enabled ?? true
    }
}

/// BS_COMMANDLINK: macOS has none; Setup Assistant's choices (a large button
/// with a title and a secondary line) are the closest. Glass on 26.
struct CommandLink: View {
    @ObservedObject var model: ControlModel
    let label: String

    var body: some View {
        let note = model.snap.note ?? ""
        let content = VStack(alignment: .leading, spacing: 2) {
            Text(label).font(.headline).lineLimit(1)
            if !note.isEmpty { Text(note).font(.callout).foregroundStyle(.secondary).lineLimit(2) }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        let click = { model.emit(["t": "click"]) }
        Group {
            if model.snap.isDefault == true {
                if #available(macOS 26, *) {
                    Button(action: click) { content }.buttonStyle(.glassProminent)
                } else {
                    Button(action: click) { content }.buttonStyle(.borderedProminent)
                }
            } else {
                if #available(macOS 26, *) {
                    Button(action: click) { content }.buttonStyle(.glass)
                } else {
                    Button(action: click) { content }.buttonStyle(.bordered)
                }
            }
        }
        .controlSize(.large)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// map: button.groupbox. macOS shows a box's title above it (HIG Boxes), but
/// the native view is exactly the Win32 group box's rectangle and can't draw
/// outside it, while the Win32 content inside starts right at the box's top
/// edge. So the rounded box covers the whole rectangle and the title sits as
/// a small caption inside its top-left, in the title zone the app leaves
/// clear, leaving the content room below. The Win32 controls are siblings
/// drawn over this view, on the dialog background.
/// map: button.groupbox. A real NSBox (what SwiftUI's GroupBox is on macOS),
/// its title above the box as macOS shows it (HIG: Boxes) when the app leaves
/// room there: the host view then reaches above the Win32 rectangle by the
/// title's band (GroupBoxTitle.band) and the box takes the rectangle, whose
/// caption band becomes padding. Without room, the title goes inside the
/// box's top (belowTop). The box's fill is translucent, so what wine draws in
/// it shows through; it stays behind the native controls it contains.
struct NativeGroupBox: NSViewRepresentable {
    let label: String
    let above: Bool

    func makeNSView(context: Context) -> NSBox {
        let box = NSBox()
        box.boxType = .primary
        return box
    }

    func updateNSView(_ box: NSBox, context: Context) {
        box.title = label
        box.titlePosition = label.isEmpty ? .noTitle : (above ? .aboveTop : .belowTop)
    }
}

enum GroupBoxTitle {
    private static var bands: [String: CGFloat] = [:]

    /// How far an NSBox's title above it reaches over the box's border (main thread).
    static func band(_ label: String) -> CGFloat {
        if let band = bands[label] { return band }
        let box = NSBox(frame: NSRect(x: 0, y: 0, width: 400, height: 200))
        box.boxType = .primary
        box.title = label
        box.titlePosition = .aboveTop
        box.layoutSubtreeIfNeeded()
        let band = max(0, box.bounds.height - box.borderRect.maxY)
        bands[label] = band
        return band
    }

    /// The host view reaches above the Win32 rectangle by the title's band when
    /// the title goes above, and stays behind the controls in the box.
    static func apply(_ host: ControlHost) {
        let view = Unmanaged<NSView>.fromOpaque(host.hostView).takeUnretainedValue()
        let label = stripMnemonic(host.model.snap.text ?? "")
        let above = (host.model.snap.titleAbove ?? false) && !label.isEmpty
        let outset = above ? band(label) : 0
        if view.responds(to: NSSelectorFromString("w2sSetOutsetTop:")) {
            view.perform(NSSelectorFromString("w2sSetOutsetTop:"), with: NSNumber(value: Double(outset)))
            view.perform(NSSelectorFromString("w2sSetBehind:"), with: NSNumber(value: true))
        }
    }
}

// MARK: - static (map: static.*)

struct StaticText: View {
    let snap: Snapshot
    let label: String

    var body: some View {
        let align: TextAlignment = snap.align == "center" ? .center : snap.align == "trailing" ? .trailing : .leading
        let frameAlign: Alignment = snap.align == "center" ? (snap.centerVertically == true ? .center : .top)
            : snap.align == "trailing" ? (snap.centerVertically == true ? .trailing : .topTrailing)
            : (snap.centerVertically == true ? .leading : .topLeading)
        Text(label)
            .multilineTextAlignment(align)
            .lineLimit((snap.wrap ?? true) ? nil : 1)
            .foregroundColor((snap.enabled ?? true) ? .primary : .secondary)
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: frameAlign)
            .allowsHitTesting(false)
    }
}

struct StaticImage: View {
    let snap: Snapshot

    var body: some View {
        if let image = makeImage() {
            Image(nsImage: image).resizable().interpolation(.high).scaledToFit()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .allowsHitTesting(false)
        } else {
            Color.clear
        }
    }

    func makeImage() -> NSImage? {
        guard let w = snap.imageWidth, let h = snap.imageHeight else { return nil }
        if let spec = snap.imageSymbol { return Icons.image(spec, size: NSSize(width: w, height: h)) }
        guard let b64 = snap.imageBGRA else { return nil }
        return bgraImage(b64, width: w, height: h)
    }
}

/// SS_*FRAME: macOS has no bevelled frames; a separator-coloured rounded
/// outline has that role. SS_*RECT: a filled rectangle in the matching colour.
struct StaticFrame: View {
    let fill: String

    var body: some View {
        Group {
            switch fill {
            case "label": Rectangle().fill(Color(nsColor: .labelColor))
            case "separator": Rectangle().fill(Color(nsColor: .separatorColor))
            case "window": Rectangle().fill(Color(nsColor: .windowBackgroundColor))
            default: RoundedRectangle(cornerRadius: 6).strokeBorder(Color(nsColor: .separatorColor))
            }
        }
        .allowsHitTesting(false)
    }
}

// MARK: - edit (map: edit.*)

struct EditField: View {
    @ObservedObject var model: ControlModel
    let secure: Bool
    @FocusState private var focused: Bool

    var body: some View {
        let text = Binding(
            get: { model.snap.text ?? "" },
            set: { new in
                guard new != model.snap.text else { return }
                model.snap.text = new
                model.emit(["t": "text", "s": new])
            })
        let cue = model.snap.cue ?? ""
        Group {
            if secure {
                SecureField(cue, text: text)
            } else {
                TextField(cue, text: text)
                    .multilineTextAlignment(model.snap.align == "center" ? .center
                                            : model.snap.align == "trailing" ? .trailing : .leading)
            }
        }
        .textFieldStyle(.roundedBorder)
        .labelsHidden()
        .focused($focused)
        .onChange(of: focused) { now in
            if now { model.emit(["t": "focus"]) }
        }
        .onChange(of: model.focusRequest) { _ in focused = true }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

/// ES_MULTILINE. An NSTextView on every version: the Win32 control answers
/// EM_GETSEL, EM_LINEFROMCHAR, EM_POSFROMCHAR... from this view (the map's
/// `answers`), which needs its layout manager; TextEditor (15+) draws the same
/// view but exposes no line layout. TextKit 1 is set up explicitly so the
/// layout manager is there from the start.
///
/// Line breaks are one "\n" here; the PE side converts to and from CRLF. Each
/// edit goes to Win32 as the replacement of a range (EM_SETSEL + EM_REPLACESEL,
/// so EN_UPDATE/EN_CHANGE happen as for typing), the selection as EM_SETSEL,
/// and after every change the view publishes its selection and line table.
struct MultilineEdit: NSViewRepresentable {
    @ObservedObject var model: ControlModel
    let fontSize: CGFloat
    let scale: CGFloat

    final class TextView: NSTextView {
        var onReturn: (() -> Bool)?         // true: handled (the dialog's default button)
        var onFocus: (() -> Void)?

        override func insertNewline(_ sender: Any?) {
            if let handler = onReturn, handler() { return }
            super.insertNewline(sender)
        }

        override func becomeFirstResponder() -> Bool {
            let ok = super.becomeFirstResponder()
            if ok { onFocus?() }
            return ok
        }
    }

    final class Coordinator: NSObject, NSTextViewDelegate {
        var model: ControlModel
        var scale: CGFloat = 1
        weak var textView: TextView?
        weak var scroll: NSScrollView?
        var updating = false
        var edits: [(NSRange, String)] = []
        var lastSnapSel: [Int]?
        var lastEmittedSel: NSRange?
        var lastCaretGen: Int?
        var lastScrollGen: Int?
        var lastFocus: Int
        var wraps: Bool?
        var publishQueued = false

        init(model: ControlModel) {
            self.model = model
            lastFocus = model.focusRequest
        }

        /// A Win32 edit limit counts a line break as two characters.
        func win32Length(_ s: String) -> Int {
            s.utf16.count + s.utf16.reduce(0) { $0 + ($1 == 10 ? 1 : 0) }
        }

        func textView(_ tv: NSTextView, shouldChangeTextIn range: NSRange, replacementString text: String?) -> Bool {
            guard !updating, let text = text else { return true }
            let clean = text.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
            if let limit = model.snap.limit, limit > 0 {
                let current = tv.string as NSString
                let after = win32Length(current as String) - win32Length(current.substring(with: range)) + win32Length(clean)
                if after > limit && clean.utf16.count > range.length {
                    NSSound.beep()
                    return false
                }
            }
            if clean != text {
                tv.insertText(clean, replacementRange: range)
                return false
            }
            edits.append((range, clean))
            return true
        }

        func textDidChange(_ notification: Notification) {
            guard !updating else { return }
            // several ranges in one change all refer to the text before it: last first
            let ordered = edits.count > 1 ? edits.sorted { $0.0.location > $1.0.location } : edits
            for (range, text) in ordered {
                model.emit(["t": "replace", "a": [range.location, range.length], "s": text])
            }
            edits.removeAll()
            emitSelection()
            schedulePublish()
        }

        func textViewDidChangeSelection(_ notification: Notification) {
            guard !updating else { return }
            emitSelection()
            schedulePublish()
        }

        func emitSelection() {
            guard let tv = textView else { return }
            let range = tv.selectedRange()
            guard range != lastEmittedSel else { return }
            lastEmittedSel = range
            model.emit(["t": "sel", "a": [range.location, NSMaxRange(range)]])
        }

        func returnPressed() -> Bool {
            if model.snap.wantReturn ?? true { return false }
            model.emit(["t": "return"])
            return true
        }

        @objc func viewChanged(_ notification: Notification) { schedulePublish() }

        func schedulePublish() {
            guard !publishQueued else { return }
            publishQueued = true
            DispatchQueue.main.async {
                self.publishQueued = false
                self.publishNow()
            }
        }

        /// The selection, line starts and line tops (Win32 pixels, from the top
        /// of the visible area) the Win32 control answers queries from.
        func publishNow() {
            guard let tv = textView, let scroll = scroll, let lm = tv.layoutManager, let tc = tv.textContainer
            else { return }
            lm.ensureLayout(for: tc)
            let string = tv.string as NSString
            let s = max(scale, 0.01)
            let origin = tv.textContainerOrigin
            let visible = scroll.contentView.bounds
            func px(_ v: CGFloat) -> Int { Int((v / s).rounded()) }

            var lines: [Int] = []
            var tops: [Int] = []
            let glyphs = lm.numberOfGlyphs
            var glyph = 0
            while glyph < glyphs {
                var range = NSRange(location: 0, length: 0)
                let rect = lm.lineFragmentRect(forGlyphAt: glyph, effectiveRange: &range)
                lines.append(lm.characterIndexForGlyph(at: range.location))
                tops.append(px(rect.minY + origin.y - visible.minY))
                glyph = max(NSMaxRange(range), glyph + 1)
            }
            let extra = lm.extraLineFragmentRect
            if lm.extraLineFragmentTextContainer != nil || lines.isEmpty {
                // after a final line break, or no text at all: Win32 counts that line
                lines.append(string.length)
                tops.append(px(extra.minY + origin.y - visible.minY))
            }
            let first = tops.lastIndex { $0 <= 0 } ?? 0

            let selection = tv.selectedRange()
            let caretChar = NSMaxRange(selection)
            var caret = NSPoint(x: extra.minX, y: extra.minY)
            if caretChar < string.length {
                let g = lm.glyphIndexForCharacter(at: caretChar)
                let line = lm.lineFragmentRect(forGlyphAt: g, effectiveRange: nil)
                caret = NSPoint(x: line.minX + lm.location(forGlyphAt: g).x, y: line.minY)
            } else if lm.extraLineFragmentTextContainer == nil && glyphs > 0 {
                let last = lm.boundingRect(forGlyphRange: NSRange(location: glyphs - 1, length: 1), in: tc)
                caret = NSPoint(x: last.maxX, y: last.minY)
            }
            let font = tv.font ?? NSFont.systemFont(ofSize: NSFont.systemFontSize)
            let average = ("x" as NSString).size(withAttributes: [.font: font]).width

            // FNV-1a over the UTF-16 units, as the PE side hashes the Win32 text
            var hash: UInt32 = 2166136261
            for unit in (string as String).utf16 {
                hash = (hash ^ UInt32(unit & 0xff)) &* 16777619
                hash = (hash ^ UInt32(unit >> 8)) &* 16777619
            }
            model.publish([
                "len": string.length, "hash": hash,
                "sel": [selection.location, NSMaxRange(selection)],
                "lines": lines, "tops": tops, "first": first,
                "caret": [px(caret.x + origin.x - visible.minX), px(caret.y + origin.y - visible.minY)],
                "caretChar": caretChar,
                "avg": max(1, px(average)),
                "left": px(origin.x + tc.lineFragmentPadding - visible.minX),
            ])
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator(model: model) }

    func makeNSView(context: Context) -> NSScrollView {
        let coordinator = context.coordinator
        let storage = NSTextStorage()
        let layout = NSLayoutManager()
        storage.addLayoutManager(layout)
        let container = NSTextContainer(containerSize: NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude))
        container.widthTracksTextView = true
        layout.addTextContainer(container)

        let tv = TextView(frame: .zero, textContainer: container)
        tv.isRichText = false
        tv.importsGraphics = false
        tv.allowsUndo = true
        // a Win32 edit never rewrites what is typed
        tv.isAutomaticQuoteSubstitutionEnabled = false
        tv.isAutomaticDashSubstitutionEnabled = false
        tv.isAutomaticTextReplacementEnabled = false
        tv.isAutomaticSpellingCorrectionEnabled = false
        tv.isAutomaticLinkDetectionEnabled = false
        tv.isVerticallyResizable = true
        tv.isHorizontallyResizable = false
        tv.autoresizingMask = [.width]
        tv.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        tv.textContainerInset = NSSize(width: 2, height: 2)
        tv.delegate = coordinator
        tv.onReturn = { [weak coordinator] in coordinator?.returnPressed() ?? false }
        tv.onFocus = { [weak coordinator] in coordinator?.model.emit(["t": "focus"]) }
        tv.postsFrameChangedNotifications = true

        let scroll = NSScrollView()
        scroll.borderType = .bezelBorder
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.documentView = tv
        scroll.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(coordinator, selector: #selector(Coordinator.viewChanged(_:)),
                                               name: NSView.boundsDidChangeNotification, object: scroll.contentView)
        NotificationCenter.default.addObserver(coordinator, selector: #selector(Coordinator.viewChanged(_:)),
                                               name: NSView.frameDidChangeNotification, object: tv)
        coordinator.textView = tv
        coordinator.scroll = scroll
        return scroll
    }

    static func dismantleNSView(_ scroll: NSScrollView, coordinator: Coordinator) {
        NotificationCenter.default.removeObserver(coordinator)
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        let c = context.coordinator
        c.model = model
        c.scale = scale
        guard let tv = c.textView, let container = tv.textContainer else { return }
        let snap = model.snap
        c.updating = true
        defer { c.updating = false }

        let font = NSFont.systemFont(ofSize: fontSize)
        if tv.font != font { tv.font = font }
        let enabled = snap.enabled ?? true
        tv.isEditable = enabled && !(snap.readonly ?? false)
        tv.isSelectable = enabled
        tv.textColor = enabled ? .textColor : .disabledControlTextColor
        tv.alignment = snap.align == "center" ? .center : snap.align == "trailing" ? .right : .natural

        let wraps = snap.wrap ?? true
        if c.wraps != wraps {
            c.wraps = wraps
            container.widthTracksTextView = wraps
            container.containerSize = NSSize(width: wraps ? scroll.contentSize.width : CGFloat.greatestFiniteMagnitude,
                                             height: CGFloat.greatestFiniteMagnitude)
            tv.isHorizontallyResizable = !wraps
            tv.autoresizingMask = wraps ? [.width] : []
            scroll.hasHorizontalScroller = !wraps
        }

        // a snapshot that got here has seen every edit made in this view
        // (see ack), so a different text is the Win32 side's
        let text = snap.text ?? ""
        if tv.string != text {
            tv.string = text
            c.lastSnapSel = nil
        }
        let sel = snap.sel ?? [0, 0]
        if c.lastSnapSel != sel {
            c.lastSnapSel = sel
            let length = (tv.string as NSString).length
            let start = min(max(sel.first ?? 0, 0), length), end = min(max(sel.last ?? 0, start), length)
            let range = NSRange(location: start, length: end - start)
            if tv.selectedRange() != range { tv.setSelectedRange(range) }
            tv.scrollRangeToVisible(range)
            c.lastEmittedSel = range
        }
        if c.lastCaretGen != snap.caretGen {
            if c.lastCaretGen != nil { tv.scrollRangeToVisible(tv.selectedRange()) }
            c.lastCaretGen = snap.caretGen
        }
        if c.lastScrollGen != snap.scrollGen {
            if c.lastScrollGen != nil, let lm = tv.layoutManager {
                // EM_LINESCROLL: that line at the top
                lm.ensureLayout(for: container)
                let target = snap.scrollLine ?? 0
                var glyph = 0, line = 0
                var top: CGFloat = 0
                while glyph < lm.numberOfGlyphs && line < target {
                    var range = NSRange(location: 0, length: 0)
                    top = lm.lineFragmentRect(forGlyphAt: glyph, effectiveRange: &range).maxY
                    glyph = max(NSMaxRange(range), glyph + 1)
                    line += 1
                }
                scroll.contentView.scroll(to: NSPoint(x: scroll.contentView.bounds.minX, y: top))
                scroll.reflectScrolledClipView(scroll.contentView)
            }
            c.lastScrollGen = snap.scrollGen
        }
        if c.lastFocus != model.focusRequest {
            c.lastFocus = model.focusRequest
            if tv.window?.firstResponder !== tv { tv.window?.makeFirstResponder(tv) }
        }
        c.schedulePublish()
    }
}

// MARK: - lists (map: combobox.dropdownlist, listbox.*, listview.*)

/// The pop-up button a menu Picker draws with. A Picker keeps its ideal width
/// (the longest item), whatever frame it is given; the representable, with a
/// low horizontal hugging priority, takes the Win32 combo box's whole width.
struct DropDownList: NSViewRepresentable {
    @ObservedObject var model: ControlModel
    let fontSize: CGFloat
    let controlSize: ControlSize

    final class Coordinator: NSObject {
        var model: ControlModel
        init(model: ControlModel) { self.model = model }
        @objc func chosen(_ sender: NSPopUpButton) {
            let i = sender.indexOfSelectedItem
            guard i != model.snap.selection else { return }
            model.snap.selection = i
            model.emit(["t": "select", "v": i])
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator(model: model) }

    func makeNSView(context: Context) -> NSPopUpButton {
        let popup = NSPopUpButton(frame: .zero, pullsDown: false)
        popup.target = context.coordinator
        popup.action = #selector(Coordinator.chosen(_:))
        popup.setContentHuggingPriority(.defaultLow, for: .horizontal)
        popup.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return popup
    }

    func updateNSView(_ popup: NSPopUpButton, context: Context) {
        context.coordinator.model = model
        popup.controlSize = nsControlSize(controlSize)
        popup.font = NSFont.systemFont(ofSize: fontSize)
        let items = model.snap.items ?? []
        if popup.itemTitles != items {
            // NSPopUpButton.addItem(withTitle:) drops duplicate titles; a Win32
            // list may have them, so the items go into the menu directly
            popup.removeAllItems()
            for title in items { popup.menu?.addItem(NSMenuItem(title: title, action: nil, keyEquivalent: "")) }
        }
        let selection = model.snap.selection ?? -1
        if selection >= 0 && selection < popup.numberOfItems {
            if popup.indexOfSelectedItem != selection { popup.selectItem(at: selection) }
        } else if popup.indexOfSelectedItem != -1 {
            popup.select(nil)           // CB_ERR: nothing chosen, as the Win32 box shows it
        }
        popup.isEnabled = model.snap.enabled ?? true
    }

    /// 13+: say it outright instead of relying on the hugging priority.
    @available(macOS 13, *)
    func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSPopUpButton, context: Context) -> CGSize? {
        let natural = nsView.intrinsicContentSize
        guard let width = proposal.width, width.isFinite else { return natural }
        return CGSize(width: width, height: natural.height)
    }
}

/// CBS_DROPDOWN. SwiftUI has no editable combo box on macOS; NSComboBox is the
/// native control. It doesn't complete as you type: a Win32 combo box doesn't
/// either (apps that want it call SHAutoComplete and do it themselves).
struct EditableCombo: NSViewRepresentable {
    @ObservedObject var model: ControlModel
    let fontSize: CGFloat
    let controlSize: ControlSize

    final class Coordinator: NSObject, NSComboBoxDelegate {
        var model: ControlModel
        var updating = false
        init(model: ControlModel) { self.model = model }

        func controlTextDidBeginEditing(_ obj: Notification) { model.emit(["t": "focus"]) }

        func controlTextDidChange(_ obj: Notification) {
            guard !updating, let box = obj.object as? NSComboBox else { return }
            let text = box.stringValue
            model.snap.text = text
            model.emit(["t": "text", "s": text])
        }

        func comboBoxSelectionDidChange(_ notification: Notification) {
            guard !updating, let box = notification.object as? NSComboBox else { return }
            let i = box.indexOfSelectedItem
            guard i >= 0, i != model.snap.selection else { return }
            model.snap.selection = i
            model.emit(["t": "select", "v": i])
        }

        func comboBoxWillPopUp(_ notification: Notification) { model.emit(["t": "open"]) }
        func comboBoxWillDismiss(_ notification: Notification) { model.emit(["t": "close"]) }
    }

    func makeCoordinator() -> Coordinator { Coordinator(model: model) }

    func makeNSView(context: Context) -> NSComboBox {
        let box = NSComboBox()
        box.usesDataSource = false
        box.completes = false
        box.isEditable = true
        box.delegate = context.coordinator
        box.setContentHuggingPriority(.defaultLow, for: .horizontal)
        box.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        return box
    }

    func updateNSView(_ box: NSComboBox, context: Context) {
        let c = context.coordinator
        c.model = model
        c.updating = true
        defer { c.updating = false }
        box.controlSize = nsControlSize(controlSize)
        box.font = NSFont.systemFont(ofSize: fontSize)
        box.isEnabled = model.snap.enabled ?? true
        let items = model.snap.items ?? []
        if (box.objectValues as? [String]) ?? [] != items {
            box.removeAllItems()
            box.addItems(withObjectValues: items)
        }
        let selection = model.snap.selection ?? -1
        if selection >= 0 && selection < box.numberOfItems {
            if box.indexOfSelectedItem != selection { box.selectItem(at: selection) }
        } else if box.indexOfSelectedItem >= 0 {
            box.deselectItem(at: box.indexOfSelectedItem)
        }
        // after the selection: selecting an item sets the text, the Win32 text wins
        let text = model.snap.text ?? ""
        if box.stringValue != text { box.stringValue = text }
    }

    @available(macOS 13, *)
    func sizeThatFits(_ proposal: ProposedViewSize, nsView: NSComboBox, context: Context) -> CGSize? {
        let natural = nsView.intrinsicContentSize
        guard let width = proposal.width, width.isFinite else { return natural }
        return CGSize(width: width, height: natural.height)
    }
}

func nsControlSize(_ size: ControlSize) -> NSControl.ControlSize {
    size == .mini ? .mini : size == .small ? .small : .regular
}

struct ListBoxView: View {
    @ObservedObject var model: ControlModel
    let multi: Bool
    var fromRows = false

    var items: [String] {
        fromRows ? (model.snap.rows ?? []).map { $0.first ?? "" } : (model.snap.items ?? [])
    }

    var body: some View {
        let items = self.items
        Group {
            if multi {
                List(selection: Binding(
                    get: { Set(model.snap.selections ?? []) },
                    set: { sel in
                        model.snap.selections = sel.sorted()
                        model.emit(["t": "selectMany", "a": sel.sorted()])
                    })) {
                    ForEach(items.indices, id: \.self) { row($0, items[$0]).tag($0) }
                }
            } else {
                List(selection: Binding<Int?>(
                    get: { fromRows ? model.snap.selections?.first : model.snap.selection.flatMap { $0 >= 0 ? $0 : nil } },
                    set: { sel in
                        if fromRows { model.snap.selections = sel.map { [$0] } ?? [] } else { model.snap.selection = sel ?? -1 }
                        model.emit(["t": "select", "v": sel ?? -1])
                    })) {
                    ForEach(items.indices, id: \.self) { row($0, items[$0]).tag(Optional($0)) }
                }
            }
        }
        .listStyle(.bordered)
        .modifier(DoubleClickRows(model: model))
    }

    /// A list view with LVS_EX_CHECKBOXES has a check box before each item.
    @ViewBuilder
    func row(_ i: Int, _ text: String) -> some View {
        if let checks = model.snap.checks {
            HStack(spacing: 4) {
                CheckCell(model: model, row: i, checks: checks)
                Text(text)
            }
        } else {
            Text(text)
        }
    }
}

/// The check box of a list view row: the state image a click on it sets
/// (LVN_ITEMCHANGING/LVN_ITEMCHANGED follow on the Win32 side).
struct CheckCell: View {
    @ObservedObject var model: ControlModel
    let row: Int
    let checks: [Bool]

    var body: some View {
        Toggle("", isOn: Binding(
            get: { row < checks.count && checks[row] },
            set: { on in
                if var now = model.snap.checks, row < now.count {
                    now[row] = on
                    model.snap.checks = now
                }
                model.emit(["t": "check", "a": [row, on ? 1 : 0]])
            }))
            .toggleStyle(.checkbox)
            .labelsHidden()
    }
}

/// macOS 13+: the native double-click on a row (the map's contextMenu(forSelectionType:primaryAction:)).
struct DoubleClickRows: ViewModifier {
    let model: ControlModel

    func body(content: Content) -> some View {
        if #available(macOS 13, *) {
            content.contextMenu(forSelectionType: Int.self, menu: { _ in EmptyView() },
                                primaryAction: { rows in
                                    if let row = rows.first { model.emit(["t": "activate", "v": row]) }
                                })
        } else {
            content
        }
    }
}

struct ReportRow: Identifiable {
    let id: Int
    let cells: [String]
}

/// A report column the view shows: `id` is the Win32 column index (what
/// LVN_COLUMNCLICK reports), `slot` its cell in each row (rows come in display
/// order), `width` the Win32 width in points.
struct ReportColumn: Identifiable {
    let id: Int
    let slot: Int
    let title: String
    let width: CGFloat
    let alignment: Alignment

    static func from(_ snap: Snapshot, scale: CGFloat) -> [ReportColumn] {
        (snap.columns ?? []).enumerated().compactMap { slot, c in
            let px = c.width ?? 100
            guard px > 0 else { return nil }        // a zero-width column is hidden in Win32
            let alignment: Alignment = c.align == "trailing" ? .trailing : c.align == "center" ? .center : .leading
            return ReportColumn(id: c.index ?? slot, slot: slot, title: c.title,
                                width: CGFloat(px) * scale, alignment: alignment)
        }
    }

    func cell(_ row: ReportRow) -> String { slot < row.cells.count ? row.cells[slot] : "" }
}

/// A header click only tells the app (LVN_COLUMNCLICK), which sorts its items
/// itself; the view never reorders rows, it shows the order the app keeps.
struct ColumnClick: SortComparator {
    let column: Int
    var order: SortOrder = .forward
    func compare(_ lhs: ReportRow, _ rhs: ReportRow) -> ComparisonResult { .orderedSame }
}

struct ReportView: View {
    @ObservedObject var model: ControlModel
    let scale: CGFloat
    @State private var sortOrder: [ColumnClick] = []

    var body: some View {
        let rows = (model.snap.rows ?? []).enumerated().map { ReportRow(id: $0.offset, cells: $0.element) }
        let columns = ReportColumn.from(model.snap, scale: scale)
        let selection = Binding(
            get: { Set(model.snap.selections ?? []) },
            set: { (sel: Set<Int>) in
                model.snap.selections = sel.sorted()
                model.emit(["t": "selectMany", "a": sel.sorted()])
            })
        // A table sets its columns up once: a column added later lands after
        // the first one, which already fills the view. So a new set of columns
        // (the app inserts, removes or resizes one) makes a new table, and every
        // column starts at its Win32 width.
        let key = columns.map { "\($0.id):\(Int($0.width)):\($0.title)" }.joined(separator: "|")
        if columns.isEmpty {
            // no column yet, and a Win32 report list without columns shows nothing
            Color(nsColor: .controlBackgroundColor).border(Color(nsColor: .separatorColor))
        } else if #available(macOS 14.4, *) {
            let order = Binding(
                get: { sortOrder },
                set: { (new: [ColumnClick]) in
                    sortOrder = new
                    // column -1 is the check boxes' own
                    if let first = new.first, first.column >= 0, model.snap.sortHeader ?? true {
                        model.emit(["t": "column", "v": first.column])
                    }
                })
            Group {
                if let checks = model.snap.checks {
                    Table(rows, selection: selection, sortOrder: order) {
                        TableColumn("", sortUsing: ColumnClick(column: -1)) { (row: ReportRow) in
                            CheckCell(model: model, row: row.id, checks: checks)
                        }
                        .width(22)
                        TableColumnForEach(columns) { column in
                            TableColumn(column.title, sortUsing: ColumnClick(column: column.id)) { (row: ReportRow) in
                                Text(column.cell(row)).lineLimit(1).frame(maxWidth: .infinity, alignment: column.alignment)
                            }
                            .width(min: 12, ideal: column.width)
                        }
                    }
                } else {
                    Table(rows, selection: selection, sortOrder: order) {
                        TableColumnForEach(columns) { column in
                            TableColumn(column.title, sortUsing: ColumnClick(column: column.id)) { (row: ReportRow) in
                                Text(column.cell(row)).lineLimit(1).frame(maxWidth: .infinity, alignment: column.alignment)
                            }
                            .width(min: 12, ideal: column.width)
                        }
                    }
                }
            }
            .tableColumnHeaders((model.snap.noHeader ?? false) ? .hidden : .automatic)
            .modifier(DoubleClickRows(model: model))
            .id(key + (model.snap.checks == nil ? "" : "|checks"))
        } else {
            ReportFallback(model: model, rows: rows, columns: columns, selection: selection)
        }
    }
}

/// Before 14.4 a Table's columns are fixed at compile time, so the report is a
/// header row over a list whose rows lay the cells out at the same widths.
struct ReportFallback: View {
    @ObservedObject var model: ControlModel
    let rows: [ReportRow]
    let columns: [ReportColumn]
    let selection: Binding<Set<Int>>

    var body: some View {
        VStack(spacing: 0) {
            if !(model.snap.noHeader ?? false) {
                HStack(spacing: 0) {
                    if model.snap.checks != nil { Color.clear.frame(width: 22) }
                    ForEach(columns) { column in
                        Button {
                            if model.snap.sortHeader ?? true { model.emit(["t": "column", "v": column.id]) }
                        } label: {
                            Text(column.title).lineLimit(1).padding(.horizontal, 4)
                                .frame(width: column.width, alignment: column.alignment)
                        }
                        .buttonStyle(.plain)
                        Divider()
                    }
                    Spacer(minLength: 0)
                }
                .font(.system(size: NSFont.smallSystemFontSize))
                .frame(height: 22)
                .background(Color(nsColor: .controlBackgroundColor))
                Divider()
            }
            List(selection: selection) {
                ForEach(rows) { row in
                    HStack(spacing: 0) {
                        if let checks = model.snap.checks {
                            CheckCell(model: model, row: row.id, checks: checks).frame(width: 22)
                        }
                        ForEach(columns) { column in
                            Text(column.cell(row)).lineLimit(1).padding(.horizontal, 4)
                                .frame(width: column.width, alignment: column.alignment)
                            Color.clear.frame(width: 1)     // the header's divider
                        }
                        Spacer(minLength: 0)
                    }
                    .listRowInsets(EdgeInsets())
                    .tag(row.id)
                }
            }
            .listStyle(.plain)
            .modifier(DoubleClickRows(model: model))
        }
        .border(Color(nsColor: .separatorColor))
    }
}

/// LVS_ICON / LVS_SMALLICON: Finder's icon view, a grid of icons with their
/// names (small icons: the name beside). SwiftUI grids have no selection, so
/// the cells draw it: a click selects, Command-click toggles, Shift-click
/// extends; a double click activates.
struct IconGrid: View {
    @ObservedObject var model: ControlModel

    var body: some View {
        let rows = model.snap.rows ?? []
        let small = model.snap.small ?? false
        let selected = Set(model.snap.selections ?? [])
        ScrollView {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: small ? 150 : 88), spacing: 8)],
                      alignment: .leading, spacing: 8) {
                ForEach(rows.indices, id: \.self) { i in
                    cell(i, name: rows[i].first ?? "", small: small, selected: selected.contains(i))
                }
            }
            .padding(8)
        }
        .background(Color(nsColor: .controlBackgroundColor))
        .border(Color(nsColor: .separatorColor))
    }

    func cell(_ i: Int, name: String, small: Bool, selected: Bool) -> some View {
        let icons = model.snap.icons ?? []
        let side: CGFloat = small ? 16 : 32
        let icon = Group {
            if i < icons.count, let image = model.images[icons[i]] {
                Image(nsImage: image).resizable().interpolation(.high).frame(width: side, height: side)
            } else {
                Color.clear.frame(width: side, height: side)
            }
        }
        return Group {
            if small {
                HStack(spacing: 4) {
                    icon
                    Text(name).lineLimit(1)
                    Spacer(minLength: 0)
                }
            } else {
                VStack(spacing: 4) {
                    icon
                    Text(name).lineLimit(2).multilineTextAlignment(.center)
                }
                .frame(maxWidth: .infinity)
            }
        }
        .padding(4)
        .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Color.accentColor.opacity(0.25) : Color.clear))
        .contentShape(Rectangle())
        .gesture(TapGesture(count: 2).onEnded { model.emit(["t": "activate", "v": i]) }
            .exclusively(before: TapGesture().onEnded { tap(i) }))
    }

    func tap(_ i: Int) {
        let flags = NSEvent.modifierFlags
        var selection = Set(model.snap.selections ?? [])
        if (model.snap.single ?? false) || !(flags.contains(.command) || flags.contains(.shift)) {
            selection = [i]
        } else if flags.contains(.command) {
            if selection.contains(i) { selection.remove(i) } else { selection.insert(i) }
        } else if let anchor = selection.min() {
            selection = Set(min(anchor, i)...max(anchor, i))
        } else {
            selection = [i]
        }
        model.snap.selections = selection.sorted()
        model.emit(["t": "selectMany", "a": selection.sorted()])
    }
}

// MARK: - tree (map: treeview)

/// Rows are DisclosureGroup(isExpanded:) rather than List(children:): apps
/// fill a node's children on TVN_ITEMEXPANDING, so the expansion goes to Win32
/// first and the children arrive with the next snapshot. The node shows open
/// at once (and closes again if the app vetoes).
struct TreeList: View {
    @ObservedObject var model: ControlModel

    var body: some View {
        let list = List(selection: Binding<Int?>(
            get: { model.snap.selection.flatMap { $0 != 0 ? $0 : nil } },
            set: { id in
                guard let id = id, id != model.snap.selection else { return }
                model.snap.selection = id
                model.emit(["t": "select", "v": id])
            })) {
            TreeRows(model: model, nodes: model.snap.nodes ?? [])
        }
        .modifier(DoubleClickRows(model: model))
        if model.snap.sidebar ?? false {
            list.listStyle(.sidebar)
        } else {
            list.listStyle(.bordered)
        }
    }
}

struct TreeRows: View {
    @ObservedObject var model: ControlModel
    let nodes: [Snapshot.TreeNode]

    var body: some View {
        ForEach(nodes) { node in
            if node.kids ?? false {
                DisclosureGroup(isExpanded: Binding(
                    get: { node.open ?? false },
                    set: { open in
                        guard open != (node.open ?? false) else { return }
                        model.snap.nodes = TreeRows.setting(node.id, open: open, in: model.snap.nodes ?? [])
                        model.emit(["t": open ? "expand" : "collapse", "v": node.id])
                    })) {
                    // the recursion goes through AnyView so the view type stays finite
                    AnyView(TreeRows(model: model, nodes: node.children ?? []))
                } label: {
                    Text(node.text).lineLimit(1)
                }
                .tag(Optional(node.id))     // the selection is Int? (as the list boxes')
            } else {
                Text(node.text).lineLimit(1).tag(Optional(node.id))
            }
        }
    }

    static func setting(_ id: Int, open: Bool, in nodes: [Snapshot.TreeNode]) -> [Snapshot.TreeNode] {
        nodes.map { node in
            var node = node
            if node.id == id { node.open = open }
            else if let children = node.children { node.children = setting(id, open: open, in: children) }
            return node
        }
    }
}

// MARK: - values (map: progress, trackbar)

struct ProgressBar: View {
    let snap: Snapshot

    var body: some View {
        let lo = snap.min ?? 0, hi = max((snap.max ?? 100), lo + 1)
        let tint: Color? = snap.state == 2 ? .red : snap.state == 3 ? .yellow : nil
        Group {
            if snap.marquee == true {
                ProgressView().progressViewStyle(.linear)
            } else {
                ProgressView(value: min(max((snap.value ?? 0) - lo, 0), hi - lo), total: hi - lo)
                    .progressViewStyle(.linear)
            }
        }
        .tint(tint)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .allowsHitTesting(false)
    }
}

struct TrackBar: View {
    @ObservedObject var model: ControlModel

    var body: some View {
        let lo = model.snap.min ?? 0, hi = max(model.snap.max ?? 100, lo + 1)
        let value = Binding(
            get: { model.snap.value ?? lo },
            set: { v in
                let rounded = v.rounded()
                guard rounded != model.snap.value else { return }
                model.snap.value = rounded
                model.emit(["t": "value", "v": rounded])
            })
        let ended: (Bool) -> Void = { editing in
            if !editing { model.emit(["t": "valueEnd", "v": model.snap.value ?? lo]) }
        }
        Group {
            if #available(macOS 26, *), let ticks = model.snap.ticks, ticks > 1 {
                let values = (0..<ticks).map { lo + (hi - lo) * Double($0) / Double(ticks - 1) }
                Slider(value: value, in: lo...hi, label: { EmptyView() }, ticks: {
                    SliderTickContentForEach(values, id: \.self) { SliderTick($0) }
                }, onEditingChanged: ended)
            } else {
                Slider(value: value, in: lo...hi, onEditingChanged: ended)
            }
        }
        .labelsHidden()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

// MARK: - status bar (map: statusbar)

/// A Finder-style bottom bar: small secondary text, panes split by dividers.
/// Pane widths come from SB_SETPARTS (right edges, -1 to the end); a leading
/// tab centres a pane's text, two right-align it, as in Win32. Owner-drawn
/// panes stay the app's: wine draws them, so the view leaves them clear.
struct StatusBar: View {
    @ObservedObject var model: ControlModel
    let scale: CGFloat

    var body: some View {
        let panes = model.snap.panes ?? []
        HStack(spacing: 0) {
            ForEach(panes.indices, id: \.self) { i in
                pane(panes[i], index: i, left: i == 0 ? 0 : panes[i - 1].right)
                if i < panes.count - 1 { Divider().padding(.vertical, 3) }
            }
            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    @ViewBuilder
    func pane(_ pane: Snapshot.Pane, index: Int, left: Int) -> some View {
        let width: CGFloat? = pane.right < 0 ? nil : CGFloat(max(0, pane.right - left)) * scale
        if pane.ownerDraw == true {
            Color.clear.frame(width: width).frame(maxWidth: width == nil ? .infinity : nil)
                .allowsHitTesting(false)
        } else {
            let tabs = pane.text.prefix { $0 == "\t" }.count
            let alignment: Alignment = tabs == 1 ? .center : tabs >= 2 ? .trailing : .leading
            Text(String(pane.text.dropFirst(tabs)))
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .padding(.horizontal, 6)
                .frame(width: width, alignment: alignment)
                .frame(maxWidth: width == nil ? .infinity : nil, maxHeight: .infinity, alignment: alignment)
                .background(.bar)
                .contentShape(Rectangle())
                .help(pane.tip ?? "")
                .gesture(TapGesture(count: 2).onEnded { model.emit(["t": "dblclick", "v": index]) }
                    .exclusively(before: TapGesture().onEnded { model.emit(["t": "click", "v": index]) }))
        }
    }
}

// MARK: - date and time (map: datetime, monthcal)

/// SYSTEMTIME is local and Gregorian, whatever calendar the user reads dates in.
enum W2SDate {
    static let calendar: Calendar = {
        var c = Calendar(identifier: .gregorian)
        c.timeZone = .current
        return c
    }()

    static func date(_ c: [Int]?) -> Date? {
        guard let c = c, c.count >= 3 else { return nil }
        var parts = DateComponents()
        parts.year = c[0]
        parts.month = c[1]
        parts.day = c[2]
        parts.hour = c.count > 3 ? c[3] : 0
        parts.minute = c.count > 4 ? c[4] : 0
        parts.second = c.count > 5 ? c[5] : 0
        return calendar.date(from: parts)
    }

    static func components(_ date: Date) -> [Int] {
        let c = calendar.dateComponents([.year, .month, .day, .hour, .minute, .second], from: date)
        return [c.year ?? 1601, c.month ?? 1, c.day ?? 1, c.hour ?? 0, c.minute ?? 0, c.second ?? 0]
    }

    static func range(_ low: [Int]?, _ high: [Int]?) -> ClosedRange<Date> {
        let lo = date(low) ?? .distantPast, hi = date(high) ?? .distantFuture
        return lo <= hi ? lo...hi : hi...lo
    }
}

/// datetime: the compact field that opens a calendar (the Win32 field with its
/// drop-down); a stepper field for DTS_UPDOWN and DTS_TIMEFORMAT; a check box
/// before it for DTS_SHOWNONE. monthcal: the graphical calendar.
struct DateTimePicker: View {
    @ObservedObject var model: ControlModel
    let graphical: Bool

    var body: some View {
        let snap = model.snap
        let shown = W2SDate.date(snap.date) ?? Date()
        let valid = snap.dateValid ?? true
        let selection = Binding<Date>(
            get: { shown },
            set: { new in
                let c = W2SDate.components(new)
                guard c != model.snap.date else { return }
                model.snap.date = c
                model.snap.dateValid = true
                model.emit(["t": "date", "a": c])
            })
        HStack(spacing: 4) {
            if snap.showNone ?? false {
                Toggle("", isOn: Binding(
                    get: { valid },
                    set: { on in
                        model.snap.dateValid = on
                        let event: [String: Any] = on ? ["t": "date", "a": W2SDate.components(shown)] : ["t": "none"]
                        model.emit(event)
                    }))
                    .toggleStyle(.checkbox)
                    .labelsHidden()
            }
            picker(selection, in: W2SDate.range(snap.dateMin, snap.dateMax))
                .labelsHidden()
                .disabled(!valid)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: graphical ? .center : .leading)
    }

    @ViewBuilder
    func picker(_ selection: Binding<Date>, in range: ClosedRange<Date>) -> some View {
        if graphical {
            DatePicker("", selection: selection, in: range, displayedComponents: .date).datePickerStyle(.graphical)
        } else if model.snap.timeOnly ?? false {
            DatePicker("", selection: selection, in: range, displayedComponents: .hourAndMinute)
                .datePickerStyle(.stepperField)
        } else if model.snap.upDown ?? false {
            DatePicker("", selection: selection, in: range, displayedComponents: .date).datePickerStyle(.stepperField)
        } else {
            DatePicker("", selection: selection, in: range, displayedComponents: .date).datePickerStyle(.compact)
        }
    }
}

// MARK: - tab (map: tab)

/// Only the strip is native: a Win32 tab control's pages are separate windows
/// the app shows and hides, drawn under this view, so the rest stays clear.
struct TabStrip: View {
    @ObservedObject var model: ControlModel

    var body: some View {
        if model.snap.mode == "sidebar" || model.snap.mode == "window" {
            // the sidebar is a real window sidebar, outside wine's content:
            // nothing is drawn here
            EmptyView()
        } else if model.snap.mode == "wizard" {
            WizardSteps(model: model)
        } else {
            strip
        }
    }

    var strip: some View {
        let items = model.snap.items ?? []
        let selection = Binding(
            get: { model.snap.selection ?? 0 },
            set: { i in model.emit(["t": "select", "v": i]) })
        let picker = Group {
            if #available(macOS 27, *) {
                Picker("", selection: selection) {
                    ForEach(items.indices, id: \.self) { Text(stripMnemonic(items[$0])).tag($0) }
                }
                .pickerStyle(.tabs)
            } else {
                Picker("", selection: selection) {
                    ForEach(items.indices, id: \.self) { Text(stripMnemonic(items[$0])).tag($0) }
                }
                .pickerStyle(.segmented)
            }
        }
        .labelsHidden()
        .fixedSize()
        return VStack(spacing: 0) {
            // more tabs than fit (a multi-row Win32 strip) scroll sideways
            if #available(macOS 13, *) {
                ViewThatFits(in: .horizontal) {
                    picker.frame(maxWidth: .infinity)
                    ScrollView(.horizontal, showsIndicators: false) { picker }
                }
            } else {
                ScrollView(.horizontal, showsIndicators: false) { picker }
            }
            Spacer(minLength: 0)
        }
    }
}

/// map: propsheet (more than 5 pages, macOS 13+): the window's native sidebar.
/// This is not drawn in the tab control's in-window view (EmptyView above):
/// it is hosted as the sidebar of the window's split view (WindowSidebarAttach),
/// a real List in the sidebar style (native row height, rounded selection,
/// accent colour, keyboard) on the system sidebar material. Plain rows, no
/// icons: Win32 pages have none. Selecting a row emits the same "select" event
/// the tab strip emits.
struct WindowSidebar: View {
    @ObservedObject var model: ControlModel

    var body: some View {
        let items = model.snap.items ?? []
        let selection = Binding<Int?>(
            get: { model.snap.selection.flatMap { $0 >= 0 ? $0 : nil } },
            set: { i in
                guard let i = i, i != model.snap.selection else { return }
                model.snap.selection = i
                model.emit(["t": "select", "v": i])
            })
        List(selection: selection) {
            ForEach(items.indices, id: \.self) { i in
                Text(stripMnemonic(items[i])).tag(Optional(i))
            }
        }
        .listStyle(.sidebar)
    }
}

/// map: propsheet.wizard: the macOS Installer layout. The wizard's tab control
/// spans the sheet above the buttons; it told wine to lay the pages out right
/// of the steps (TCM_ADJUSTRECT), and wine hands it the active page's header.
/// So this draws the steps and the header, and stays clear over the page.
/// The steps can't be clicked, as in the Installer: Continue moves on.
struct WizardSteps: View {
    @ObservedObject var model: ControlModel

    var body: some View {
        GeometryReader { geo in
            let snap = model.snap
            let raw = geo.size.width / CGFloat(max(1, snap.widthPx ?? Double(geo.size.width)))
            let scale = raw.isFinite && raw > 0 ? raw : 1
            let metrics = Metrics(snap: snap, scale: scale)
            let width = CGFloat(snap.sidebarPx ?? 0) * scale
            ZStack(alignment: .topLeading) {
                if width > 0 {
                    steps(snap: snap, fontSize: metrics.fontSize)
                        .frame(width: width, height: geo.size.height, alignment: .topLeading)
                        .background(.bar)
                }
                if let heading = snap.heading, let at = snap.headerPx, at.count == 2,
                   !heading.isEmpty || !(snap.subheading ?? "").isEmpty {
                    let x = CGFloat(at[0]) * scale
                    header(heading: heading, subheading: snap.subheading ?? "", fontSize: metrics.fontSize)
                        .frame(width: max(0, geo.size.width - x - 8), height: max(0, CGFloat(at[1]) * scale),
                               alignment: .leading)
                        .offset(x: x)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
            .allowsHitTesting(false)
        }
    }

    func steps(snap: Snapshot, fontSize: CGFloat) -> some View {
        let items = snap.items ?? []
        let current = snap.selection ?? 0
        return VStack(alignment: .leading, spacing: fontSize * 0.8) {
            ForEach(items.indices, id: \.self) { i in
                HStack(spacing: 8) {
                    // done and current steps are filled; the current one in the accent colour
                    Image(systemName: i <= current ? "circle.fill" : "circle")
                        .font(.system(size: fontSize * 0.6))
                        .foregroundColor(i == current ? .accentColor : .secondary)
                    Text(stripMnemonic(items[i]))
                        .fontWeight(i == current ? .semibold : .regular)
                        .foregroundColor(i == current ? .primary : .secondary)
                        .lineLimit(2)
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.vertical, 20)
        .padding(.horizontal, 16)
    }

    func header(heading: String, subheading: String, fontSize: CGFloat) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            if !heading.isEmpty {
                Text(heading).font(.system(size: max(13, fontSize * 1.4), weight: .bold)).lineLimit(1)
            }
            if !subheading.isEmpty {
                Text(subheading).foregroundColor(.secondary).lineLimit(2)
            }
        }
    }
}
