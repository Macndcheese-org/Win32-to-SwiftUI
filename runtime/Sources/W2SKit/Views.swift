// The SwiftUI view of each translated control, one per map entry
// (map/ui-map/*.yaml). Tiers follow the map's `since` ladder.
import AppKit
import SwiftUI

enum ControlViews {
    // map: button.push button.default button.checkbox button.pushlike button.radio button.groupbox
    // map: static.text static.separator static.image edit.single edit.password edit.number edit.readonly
    // map: combobox.dropdownlist listbox.single listbox.multi listview.list listview.report
    // map: progress trackbar tab
    static let entries: Set<String> = [
        "button.push", "button.default", "button.checkbox", "button.pushlike", "button.radio", "button.groupbox",
        "static.text", "static.separator", "static.image",
        "edit.single", "edit.password", "edit.number", "edit.readonly",
        "combobox.dropdownlist", "listbox.single", "listbox.multi", "listview.list", "listview.report",
        "progress", "trackbar", "tab",
    ]

    static func supports(_ entry: String) -> Bool { entries.contains(entry) }

    static func root(for model: ControlModel, entry: String) -> AnyView {
        AnyView(ControlRoot(model: model, entry: entry))
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

    init(snap: Snapshot, scale: CGFloat) {
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
                .font(.system(size: metrics.fontSize))
                .controlSize(metrics.controlSize)
                .disabled(!(snap.enabled ?? true))
                .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
        }
    }

    @ViewBuilder
    func content(snap: Snapshot, metrics: Metrics) -> some View {
        let label = stripMnemonic(snap.text ?? "", keep: snap.noPrefix ?? false)
        switch entry {
        case "button.push", "button.default":
            PushButton(model: model, label: label, prominent: snap.isDefault ?? (entry == "button.default"))
        case "button.checkbox":
            CheckBox(model: model, label: label)
        case "button.pushlike":
            PushLike(model: model, label: label)
        case "button.radio":
            RadioButton(model: model, label: label, fontSize: metrics.fontSize, controlSize: metrics.controlSize)
        case "button.groupbox":
            GroupBox(label: Text(label)) { Color.clear.frame(maxWidth: .infinity, maxHeight: .infinity) }
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
        case "edit.single", "edit.password", "edit.number":
            EditField(model: model, secure: entry == "edit.password")
        case "edit.readonly":
            Text(snap.text ?? "").textSelection(.enabled)
                .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .leading)
        case "combobox.dropdownlist":
            DropDownList(model: model)
        case "listbox.single", "listbox.multi":
            ListBoxView(model: model, multi: entry == "listbox.multi")
        case "listview.list":
            ListBoxView(model: model, multi: !(snap.single ?? false), fromRows: true)
        case "listview.report":
            ReportView(model: model)
        case "progress":
            ProgressBar(snap: snap)
        case "trackbar":
            TrackBar(model: model)
        case "tab":
            TabStrip(model: model)
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
        button.controlSize = controlSize == .mini ? .mini : controlSize == .small ? .small : .regular
        button.state = (model.snap.checked ?? 0) == 1 ? .on : .off
        button.isEnabled = model.snap.enabled ?? true
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
        guard let w = snap.imageWidth, let h = snap.imageHeight, w > 0, h > 0,
              let b64 = snap.imageBGRA, var data = Data(base64Encoded: b64), data.count == w * h * 4 else { return nil }
        // BGRA premultiplied (GDI's layout) -> CGImage
        let provider = data.withUnsafeMutableBytes { raw -> CGDataProvider? in
            CGDataProvider(data: Data(raw) as CFData)
        }
        guard let provider = provider,
              let cg = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                               space: CGColorSpaceCreateDeviceRGB(),
                               bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue
                                                        | CGBitmapInfo.byteOrder32Little.rawValue),
                               provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
        else { return nil }
        return NSImage(cgImage: cg, size: NSSize(width: w, height: h))
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

// MARK: - lists (map: combobox.dropdownlist, listbox.*, listview.*)

struct DropDownList: View {
    @ObservedObject var model: ControlModel

    var body: some View {
        let items = model.snap.items ?? []
        Picker("", selection: Binding(
            get: { model.snap.selection ?? -1 },
            set: { i in
                model.snap.selection = i
                model.emit(["t": "select", "v": i])
            })) {
            ForEach(items.indices, id: \.self) { i in
                Text(stripMnemonic(items[i], keep: true)).tag(i)
            }
        }
        .pickerStyle(.menu)
        .labelsHidden()
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }
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
                    ForEach(items.indices, id: \.self) { Text(items[$0]).tag($0) }
                }
            } else {
                List(selection: Binding<Int?>(
                    get: { fromRows ? model.snap.selections?.first : model.snap.selection.flatMap { $0 >= 0 ? $0 : nil } },
                    set: { sel in
                        if fromRows { model.snap.selections = sel.map { [$0] } ?? [] } else { model.snap.selection = sel ?? -1 }
                        model.emit(["t": "select", "v": sel ?? -1])
                    })) {
                    ForEach(items.indices, id: \.self) { Text(items[$0]).tag(Optional($0)) }
                }
            }
        }
        .listStyle(.bordered)
        .modifier(DoubleClickRows(model: model))
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

struct ReportView: View {
    @ObservedObject var model: ControlModel

    var body: some View {
        let rows = (model.snap.rows ?? []).enumerated().map { ReportRow(id: $0.offset, cells: $0.element) }
        let columns = model.snap.columns ?? [Snapshot.Column(title: "", width: nil)]
        if #available(macOS 14.4, *) {
            Table(rows, selection: Binding(
                get: { Set(model.snap.selections ?? []) },
                set: { sel in
                    model.snap.selections = sel.sorted()
                    model.emit(["t": "selectMany", "a": sel.sorted()])
                })) {
                TableColumnForEach(Array(columns.indices), id: \.self) { c in
                    TableColumn(columns[c].title) { (row: ReportRow) in
                        Text(c < row.cells.count ? row.cells[c] : "")
                    }
                    .width(ideal: CGFloat(columns[c].width ?? 100))
                }
            }
            .modifier(DoubleClickRows(model: model))
        } else {
            // before 14.4: Table's columns are static; a list of rows keeps every cell
            List(selection: Binding(
                get: { Set(model.snap.selections ?? []) },
                set: { sel in model.emit(["t": "selectMany", "a": sel.sorted()]) })) {
                ForEach(rows) { row in
                    HStack {
                        ForEach(columns.indices, id: \.self) { c in
                            Text(c < row.cells.count ? row.cells[c] : "")
                                .frame(width: CGFloat(columns[c].width ?? 100), alignment: .leading)
                        }
                    }.tag(row.id)
                }
            }
            .listStyle(.bordered)
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

// MARK: - tab (map: tab)

/// Only the strip is native: a Win32 tab control's pages are separate windows
/// the app shows and hides, drawn under this view, so the rest stays clear.
struct TabStrip: View {
    @ObservedObject var model: ControlModel

    var body: some View {
        let items = model.snap.items ?? []
        let selection = Binding(
            get: { model.snap.selection ?? 0 },
            set: { i in model.emit(["t": "select", "v": i]) })
        VStack(spacing: 0) {
            Group {
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
            .frame(maxWidth: .infinity)
            Spacer(minLength: 0)
        }
    }
}
