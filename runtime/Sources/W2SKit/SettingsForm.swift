// A property sheet as a macOS settings window (map: propsheet): a tab view
// across the top, the page's controls laid out again as a settings form
// (labels right-aligned, controls in a column, as Safari's settings) and the
// sheet's buttons at the bottom right. The Win32 sheet stays underneath as the
// model: every control in the form is the native view of a Win32 control,
// bound to its ControlModel, and the sheet is sized to the form.
import AppKit
import SwiftUI

/// One control on the page, as the PE side sends it: its handle, map entry
/// and rectangle in the sheet's client area (Win32 pixels).
struct FormItem: Codable, Equatable, Hashable {
    var h: UInt64
    var e: String                          // the map entry, or "slot": a window of the app's own
    var r: [Double]
    var w: UInt64?                         // a slot's window (HWND)

    var rect: CGRect { r.count == 4 ? CGRect(x: r[0], y: r[1], width: r[2], height: r[3]) : .zero }
    var host: ControlHost? { W2S.control(h) }
    /// the control's text as it shows (access keys taken out, "..." as "…")
    var label: String {
        guard let snap = host?.model.snap else { return "" }
        return stripMnemonic(snap.display ?? snap.text ?? "", keep: snap.noPrefix ?? false)
    }
    /// a control of its own height: lists, trees, text views
    var wide: Bool { e.hasPrefix("listview.") || e.hasPrefix("listbox.") || e == "treeview" || e == "edit.multiline" }
    /// something a label names: fields, pop-ups, lists, sliders
    var input: Bool { wide || e.hasPrefix("edit.") || e.hasPrefix("combobox.") || e == "comboboxex" || e == "trackbar" || e == "datetime" }
}

/// How the page's controls read as rows of a settings form.
enum FormRow: Hashable {
    case labeled(FormItem, [FormItem])     // "Label:" and its controls
    case plain([FormItem])                 // check boxes, radio buttons, buttons: in the controls' column
    case note(FormItem)                    // a paragraph of text
}

struct FormSection: Hashable {
    var title: FormItem?                   // a group box
    var rows: [FormRow]
}

enum FormLayout {
    /// The page's controls as sections (group boxes) of rows: controls whose
    /// rectangles overlap vertically are one row; a text on the left of a row
    /// labels it, and a one-line text right above a field labels the field
    /// (the Windows habit, which macOS puts beside it). Lines and frames go:
    /// the form's sections and spacing take their place.
    static func sections(_ all: [FormItem]) -> [FormSection] {
        let items = all.filter { $0.e != "static.separator" && $0.e != "static.frame" }
        let groups = items.filter { $0.e == "button.groupbox" }
        var inGroup: [UInt64: FormItem] = [:]       // control -> its group box
        for item in items where item.e != "button.groupbox" {
            let c = CGPoint(x: item.rect.midX, y: item.rect.midY)
            // the innermost box holding the control's centre
            if let box = groups.filter({ $0.rect.contains(c) }).min(by: { $0.rect.width * $0.rect.height < $1.rect.width * $1.rect.height }) {
                inGroup[item.h] = box
            }
        }
        // in reading order: a box where it starts, loose controls between boxes
        enum Unit { case box(FormItem), loose(FormItem) }
        var units: [(y: Double, unit: Unit)] = groups.map { ($0.rect.minY, .box($0)) }
        units += items.filter { $0.e != "button.groupbox" && inGroup[$0.h] == nil }.map { ($0.rect.minY, .loose($0)) }
        units.sort { $0.y < $1.y }
        var sections: [FormSection] = []
        var loose: [FormItem] = []
        func flush() {
            if !loose.isEmpty { sections.append(FormSection(title: nil, rows: rows(loose))) }
            loose = []
        }
        for (_, unit) in units {
            switch unit {
            case .loose(let item):
                loose.append(item)
            case .box(let box):
                flush()
                let content = items.filter { inGroup[$0.h] == box }
                sections.append(FormSection(title: box, rows: rows(content)))
            }
        }
        flush()
        return sections.filter { !$0.rows.isEmpty || $0.title != nil }
    }

    /// A line of one-line texts over a line of controls, each text starting where
    /// a control starts (the Windows way of labelling side-by-side fields): each
    /// text labels the control under it, and what sits flush against that control
    /// (an up-down on its field); the rest of the line is a row of its own.
    static func columnLabels(_ labels: [FormItem], over next: [FormItem]?) -> [FormRow]? {
        guard let next = next, labels.count >= 2 || next.count >= 2,
              labels.allSatisfy({ $0.e == "static.text" && $0.rect.height <= 20 }),
              next.contains(where: { $0.e != "static.text" }),
              let bottom = labels.map({ $0.rect.maxY }).max(), let top = next.map({ $0.rect.minY }).min(),
              top - bottom <= 14,
              // each text over a control of its own (a line starting with its own label isn't that)
              labels.allSatisfy({ label in next.contains { $0.e != "static.text" && abs($0.rect.minX - label.rect.minX) <= 12 } })
        else { return nil }
        var rows: [FormRow] = []
        var used = Set<FormItem>()
        for label in labels {
            guard let head = next.first(where: { $0.e != "static.text" && abs($0.rect.minX - label.rect.minX) <= 12 && !used.contains($0) })
            else { continue }
            var group = [head]
            used.insert(head)
            while let last = group.last,
                  let flush = next.first(where: { !used.contains($0) && abs($0.rect.minX - last.rect.maxX) <= 2 }) {
                group.append(flush)
                used.insert(flush)
            }
            rows.append(.labeled(label, group))
        }
        let rest = next.filter { !used.contains($0) }
        if !rest.isEmpty { rows.append(.plain(rest)) }
        return rows
    }

    static func rows(_ items: [FormItem]) -> [FormRow] {
        // lines: controls centred on the same height, left to right. The smaller of
        // two heights decides, so a tall image or list doesn't take in everything beside it.
        var lines: [[FormItem]] = []
        for item in items.sorted(by: { ($0.rect.minY, $0.rect.minX) < ($1.rect.minY, $1.rect.minX) }) {
            // an image (an app's logo, an icon) is a row of its own
            if let ref = lines.last?.first, ref.e != "static.image", item.e != "static.image",
               abs(item.rect.midY - ref.rect.midY) <= max(6, min(item.rect.height, ref.rect.height) / 2 + 2) {
                lines[lines.count - 1].append(item)
            } else {
                lines.append([item])
            }
        }
        lines = lines.map { $0.sorted { $0.rect.minX < $1.rect.minX } }

        var rows: [FormRow] = []
        var i = 0
        while i < lines.count {
            let line = lines[i]
            let first = line[0]
            if let paired = columnLabels(line, over: i + 1 < lines.count ? lines[i + 1] : nil) {
                // labels over the fields of the next line: one setting per row, as a Mac form has
                rows += paired
                i += 2
                continue
            }
            if first.e == "static.text" && line.count > 1 {
                rows.append(.labeled(first, Array(line.dropFirst())))
            } else if first.e == "static.text" && line.count == 1 {
                let next = i + 1 < lines.count ? lines[i + 1].first : nil
                if let next = next, next.input, first.rect.height <= 20,
                   abs(next.rect.minX - first.rect.minX) <= 12, next.rect.minY - first.rect.maxY <= 12 {
                    // a one-line text right above a field, starting where it starts: its label
                    rows.append(.labeled(first, lines[i + 1]))
                    i += 1
                } else if let next = next, next.input, first.rect.height <= 20,
                          next.rect.minX >= first.rect.maxX - 4, abs(next.rect.minY - first.rect.minY) <= 6 {
                    // a one-line text beside the top of a taller field (a list): its label
                    rows.append(.labeled(first, lines[i + 1]))
                    i += 1
                } else {
                    rows.append(.note(first))
                }
            } else {
                rows.append(.plain(line))
            }
            i += 1
        }
        return rows
    }
}

/// The form's look: the system font and regular controls, as a Mac settings
/// window has, whatever the Win32 dialog's font was.
enum FormMetrics {
    static let fontSize = NSFont.systemFontSize
    static func metrics(scale: CGFloat) -> Metrics { Metrics(fontSize: fontSize, controlSize: .regular, scale: scale) }

    static func textWidth(_ s: String, bold: Bool = false) -> CGFloat {
        let font = bold ? NSFont.boldSystemFont(ofSize: fontSize) : NSFont.systemFont(ofSize: fontSize)
        return ceil((s as NSString).size(withAttributes: [.font: font]).width)
    }

    static func textHeight(_ s: String, width: CGFloat) -> CGFloat {
        let font = NSFont.systemFont(ofSize: fontSize)
        let r = (s as NSString).boundingRect(with: NSSize(width: width, height: 10_000),
                                              options: [.usesLineFragmentOrigin, .usesFontLeading], attributes: [.font: font])
        return ceil(r.height) + 2
    }

    /// a control's size in the form: its Win32 size where that is what matters
    /// (lists, images), the regular size of its kind otherwise, wide enough for its text
    static func size(_ item: FormItem, scale: CGFloat) -> CGSize {
        let w = item.rect.width * scale, h = item.rect.height * scale
        let text = item.label
        switch item.e {
        case "button.push", "button.default", "button.pushlike":
            return CGSize(width: max(w, textWidth(text) + 32), height: 24)
        case "button.checkbox", "button.radio", "button.3state":
            return CGSize(width: max(w, textWidth(text) + 26), height: 20)
        case "static.text", "syslink":
            // its text's width, wrapped at a form's text width
            let ww = min(max(textWidth(text) + 4, 20), 420)
            return CGSize(width: ww, height: textHeight(text, width: ww))
        case "static.image":
            return CGSize(width: w, height: h)
        case "edit.single", "edit.password", "edit.readonly", "combobox.dropdownlist", "combobox.editable",
             "comboboxex", "datetime":
            // a form's width for a field, but a small one (a size, a count) stays small
            return CGSize(width: w < 80 ? max(w * 1.3, 56) : max(w, 240), height: 24)
        case "edit.number":
            return CGSize(width: max(w, 80), height: 24)
        case "trackbar", "progress":
            return CGSize(width: max(w, 240), height: 24)
        case "trackbar.vertical":
            return CGSize(width: 24, height: max(h, 80))
        case "slot":
            return CGSize(width: w, height: h)
        case "updown":
            return CGSize(width: max(w, 16), height: 24)
        default:    // lists, trees, text views: the Win32 height, a form's width
            return CGSize(width: max(w, 380), height: max(h, 80))
        }
    }
}

/// Where each control sits in the form (tests: a real click aims there, its
/// own view being hidden). Main thread.
enum FormPlaces {
    static let space = "w2s.form"
    static var frames: [UInt64: (form: UInt64, rect: CGRect)] = [:]
    /// a form's slots (the app's own windows show through), in its host view: clicks go to wine there
    static var holes: [UInt64: [CGRect]] = [:]
    /// where each form is in its window (.global), to bring window places into the form
    static var origins: [UInt64: CGPoint] = [:]

    static func local(_ r: CGRect, _ form: UInt64) -> CGRect {
        let o = origins[form] ?? .zero
        return r.offsetBy(dx: -o.x, dy: -o.y)
    }
}

/// A slot's place in the view tree: a clear AppKit view, which clicks go through.
final class SlotMarkerView: NSView {
    var window32: UInt64 = 0
    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
}

struct SlotMarker: NSViewRepresentable {
    let window32: UInt64
    func makeNSView(context: Context) -> SlotMarkerView {
        let view = SlotMarkerView()
        view.window32 = window32
        return view
    }
    func updateNSView(_ view: SlotMarkerView, context: Context) { view.window32 = window32 }
}

extension FormPlaces {
    /// the slots under a view, in its coordinates (by window)
    static func slots(in root: NSView) -> [UInt64: CGRect] {
        var out: [UInt64: CGRect] = [:]
        func walk(_ v: NSView) {
            if let marker = v as? SlotMarkerView, marker.window != nil, !marker.isHiddenOrHasHiddenAncestor {
                out[marker.window32] = marker.convert(marker.bounds, to: root)
            }
            v.subviews.forEach(walk)
        }
        walk(root)
        return out
    }
}

/// One Win32 control in the form: its own native view, at the form's size.
struct FormControl: View {
    let item: FormItem
    let scale: CGFloat
    var form: UInt64 = 0

    var body: some View {
        if item.e == "slot", let window = item.w {
            // a window of the app's own: room for it, left clear; it is moved here and draws itself.
            // An AppKit marker holds the place: the tab view hosts its page in a view of its own,
            // which SwiftUI's coordinate spaces don't cross, so the place is read from the views.
            SlotMarker(window32: window)
                .frame(width: item.rect.width * scale, height: item.rect.height * scale)
        } else if let host = item.host {
            let size = FormMetrics.size(item, scale: scale)
            ControlRoot(model: host.model, entry: host.entry, fixed: FormMetrics.metrics(scale: scale))
                .frame(width: size.width, height: size.height)
                // where its text sits, which a form lines its label up with: the middle of a
                // one-line control, the first line of a list (the native views don't say)
                .alignmentGuide(.firstTextBaseline) { d in d.height > 30 ? 16 : d.height / 2 + FormMetrics.fontSize * 0.35 }
                .background(GeometryReader { geo in
                    Color.clear
                        .onAppear { FormPlaces.frames[item.h] = (form, FormPlaces.local(geo.frame(in: .global), form)) }
                        .onChange(of: geo.frame(in: .global)) { r in FormPlaces.frames[item.h] = (form, FormPlaces.local(r, form)) }
                })
        }
    }
}

/// A text of the page: a label beside its controls, or a paragraph.
struct FormText: View {
    @ObservedObject var model: ControlModel
    let note: Bool
    let width: CGFloat?

    var body: some View {
        let snap = model.snap
        Text(stripMnemonic(snap.display ?? snap.text ?? "", keep: snap.noPrefix ?? false))
            .foregroundColor((snap.enabled ?? true) ? (note ? .secondary : .primary) : .secondary)
            .fixedSize(horizontal: false, vertical: true)
            .frame(width: width, alignment: .leading)
    }
}

@available(macOS 13, *)
struct FormPage: View {
    let items: [FormItem]
    let scale: CGFloat
    var form: UInt64 = 0

    var body: some View {
        Form {
            ForEach(FormLayout.sections(items), id: \.self) { section in
                Section {
                    ForEach(section.rows, id: \.self) { row in rowView(row) }
                } header: {
                    if let title = section.title, let host = title.host {
                        FormText(model: host.model, note: false, width: nil).font(.headline)
                    }
                }
            }
        }
        .formStyle(.columns)
        .font(.system(size: FormMetrics.fontSize))
    }

    @ViewBuilder
    func rowView(_ row: FormRow) -> some View {
        switch row {
        case .labeled(let label, let controls):
            LabeledContent {
                controlsView(controls)
            } label: {
                if let host = label.host { FormText(model: host.model, note: false, width: nil) }
            }
        case .plain(let controls):
            controlsView(controls)
        case .note(let text):
            if let host = text.host {
                FormText(model: host.model, note: text.rect.height > 20,
                         width: FormMetrics.size(text, scale: scale).width)
            }
        }
    }

    func controlsView(_ controls: [FormItem]) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            ForEach(controls, id: \.self) { item in
                if item.e == "static.text", let host = item.host {
                    FormText(model: host.model, note: false, width: FormMetrics.size(item, scale: scale).width)
                } else {
                    FormControl(item: item, scale: scale, form: form)
                }
            }
        }
    }
}

/// The whole sheet: the tab view with the current page as a form, the sheet's
/// buttons below it. It stands in the tab control's host view, which covers
/// the sheet (outsets) above every other control's.
@available(macOS 13, *)
struct SettingsForm: View {
    @ObservedObject var model: ControlModel
    let scale: CGFloat
    var form: UInt64 = 0                // the tab control's handle
    @State private var placed = false
    @State private var slots: [UInt64: CGRect] = [:]
    @State private var reportedSlots: [Int] = []

    static func tabsWidth(_ titles: [String]) -> CGFloat {
        titles.reduce(CGFloat(40)) { $0 + FormMetrics.textWidth(stripMnemonic($1)) + 28 }
    }

    var body: some View {
        content
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
            // the window's background, with holes where the app's own windows show through
            .background(Path { path in
                path.addRect(CGRect(x: -10_000, y: -10_000, width: 20_000, height: 20_000))
                for r in slots.values { path.addRect(r) }
            }.fill(Color(nsColor: .windowBackgroundColor), style: FillStyle(eoFill: true)))
            .background(GeometryReader { geo in
                Color.clear
                    .onAppear { FormPlaces.origins[form] = geo.frame(in: .global).origin }
                    .onChange(of: geo.frame(in: .global).origin) { FormPlaces.origins[form] = $0 }
            })
            .coordinateSpace(name: FormPlaces.space)
            // the slots, once laid out (and again when the page or the sheet changes)
            .onAppear { scanSlots() }
            .onChange(of: model.snap.page ?? []) { _ in scanSlots() }
            .onChange(of: model.snap.sheetPx ?? []) { _ in scanSlots() }
    }

    private func scanSlots() {
        for delay in [0.15, 0.5, 1.0] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
                guard let hosting = W2S.control(form)?.hosting else { return }
                let found = FormPlaces.slots(in: hosting)
                guard found != slots else { return }
                slots = found
                FormPlaces.holes[form] = Array(found.values)
                reportSlots(found)
            }
        }
    }

    /// the app's windows go to their slots: [hwnd, x, y, ...] in the sheet (Win32 pixels)
    private func reportSlots(_ slots: [UInt64: CGRect]) {
        guard scale > 0, model.snap.mode == "form", !slots.isEmpty else { return }
        let a = slots.sorted { $0.key < $1.key }.flatMap { [Int($0.key), Int(($0.value.minX / scale).rounded()), Int(($0.value.minY / scale).rounded())] }
        guard a != reportedSlots else { return }
        reportedSlots = a
        DispatchQueue.main.async { model.emit(["t": "formSlots", "a": a]) }
    }

    var content: some View {
        let snap = model.snap
        let titles = snap.items ?? []
        let selection = snap.selection ?? 0
        let page = snap.page ?? []
        return VStack(spacing: 0) {
            TabView(selection: Binding(
                get: { selection },
                set: { i in
                    guard i != model.snap.selection else { return }
                    model.snap.selection = i
                    model.emit(["t": "select", "v": i])
                })) {
                ForEach(titles.indices, id: \.self) { i in
                    Group {
                        if i == selection {
                            FormPage(items: page, scale: scale, form: form).padding(.horizontal, 24).padding(.vertical, 16)
                        } else {
                            Color.clear
                        }
                    }
                    .tabItem { Text(stripMnemonic(titles[i])) }
                    .tag(i)
                }
            }
            .frame(minWidth: SettingsForm.tabsWidth(titles))
            // the tab bar sizes its tabs when it's made: made again once placed, and when the tabs change
            .id("\(placed)|\(titles.joined(separator: "|"))")
            .onAppear { DispatchQueue.main.async { placed = true } }
            .padding(.horizontal, 20)
            .padding(.top, 14)
            HStack(spacing: 12) {
                Spacer()
                ForEach(snap.sheetButtons ?? [], id: \.self) { button in
                    FormControl(item: button, scale: scale, form: form)
                }
            }
            .padding(20)
        }
        .font(.system(size: FormMetrics.fontSize))
    }
}

/// Puts the settings form over its sheet: the tab control's host view covers
/// the sheet and stays above the other controls' (whose own views are hidden
/// while the form shows them), and the sheet is sized to the form. A page the
/// form can't show gets the plain tab strip back.
final class SettingsFormController: WindowChrome {
    weak var host: ControlHost?
    private var hidden: Set<UInt64> = []
    private var outsets: [CGFloat] = [0, 0, 0, 0]     // top, left, bottom, right (points)
    private var askedSize: [Int] = []

    init(host: ControlHost) { self.host = host }

    /// points per Win32 pixel, from the control's own size in the host view
    private var scale: CGFloat {
        guard let host = host, let view = host.hosting?.superview, let px = host.model.snap.heightPx, px > 0 else { return 1 }
        let s = (view.frame.height - outsets[0] - outsets[2]) / CGFloat(px)
        return s.isFinite && s > 0 ? s : 1
    }

    func update() {
        guard let host = host, let hostView = host.hosting?.superview else { return }
        let snap = host.model.snap
        guard snap.mode == "form", #available(macOS 13, *), let sheet = snap.sheetPx, sheet.count == 2,
              let tab = snap.tabRect, tab.count == 4
        else {
            leave()
            return
        }
        let s = scale
        let want: [CGFloat] = [tab[1], tab[0], sheet[1] - tab[1] - tab[3], sheet[0] - tab[0] - tab[2]].map { max(0, CGFloat($0) * s) }
        if want != outsets {
            outsets = want
            hostView.perform(NSSelectorFromString("w2sSetOutsets:"), with: want.map { NSNumber(value: Double($0)) } as NSArray)
        }
        hostView.perform(NSSelectorFromString("w2sSetFront:"), with: NSNumber(value: true))

        // the page's and the sheet's controls show in the form, not in their own views
        let shown = Set((snap.page ?? []).map { $0.h } + (snap.sheetButtons ?? []).map { $0.h })
        for h in hidden.subtracting(shown) { W2S.control(h)?.hosting?.isHidden = false }
        for h in shown { W2S.control(h)?.hosting?.isHidden = true }
        hidden = shown

        // the sheet takes the form's size (measured on its own; the tab view's
        // box and the window's frame leave it a little short, hence the margin)
        let fitting = NSHostingView(rootView: SettingsForm(model: host.model, scale: s).content
            .controlSize(.regular)).fittingSize
        let size = [Int(ceil(fitting.width / s)), Int(ceil((fitting.height + 12) / s))]
        if size != askedSize, abs(Double(size[0]) - sheet[0]) > 2 || abs(Double(size[1]) - sheet[1]) > 2 {
            askedSize = size
            host.model.emit(["t": "formSize", "a": size])
        }
    }

    private func leave() {
        for h in hidden { W2S.control(h)?.hosting?.isHidden = false }
        hidden = []
        askedSize = []
        guard let hostView = host?.hosting?.superview else { return }
        if outsets != [0, 0, 0, 0] {
            outsets = [0, 0, 0, 0]
            hostView.perform(NSSelectorFromString("w2sSetOutsets:"), with: [0, 0, 0, 0].map { NSNumber(value: $0) } as NSArray)
        }
        hostView.perform(NSSelectorFromString("w2sSetFront:"), with: NSNumber(value: false))
    }

    func detach() { leave() }
}
