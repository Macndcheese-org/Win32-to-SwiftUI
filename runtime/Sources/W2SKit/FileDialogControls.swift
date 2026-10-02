// The app's own controls in its open/save dialog (IFileDialogCustomize, map:
// filedialog.open/.save) -> the panel's accessory view, under the file list,
// where a Mac app puts its options (TextEdit's encoding, Preview's format).
// comdlg32 keeps the controls: the panel shows them as they are, and a change
// made in it goes back to comdlg32, which raises the app's events
// (IFileDialogControlEvents) as a click in wine's dialog would.
import AppKit

final class FileDialogControls: NSObject, NSTextFieldDelegate {
    private final class Target: NSObject {
        let run: (Any) -> Void
        init(_ run: @escaping (Any) -> Void) { self.run = run }
        @objc func fire(_ sender: Any) { run(sender) }
    }

    private let request: Request
    /// what the accessory view holds: rebuilt when the controls change, not their state
    let view = NSStackView()
    private var grid: NSGridView?
    private var shape: [String] = []
    private var targets: [Target] = []
    private var controls: [Int: NSView] = [:]       // id -> its view (a group: its row's label)
    private var rows: [Int: NSGridRow] = [:]        // id of an ungrouped control or a group -> its row
    private var radios: [Int: [Int: NSButton]] = [:]
    private var fields: [ObjectIdentifier: Int] = [:]

    init(request: Request) {
        self.request = request
        super.init()
        view.orientation = .vertical
        view.alignment = .centerX
    }

    private func emit(_ id: Int, _ value: Int, text: String? = nil) {
        var event: [String: Any] = ["t": "control", "a": [Int32(truncatingIfNeeded: id), Int32(truncatingIfNeeded: value)]]
        if let text = text { event["s"] = text }
        request.emit(event)
    }

    private func target(_ run: @escaping (Any) -> Void) -> Target {
        let t = Target(run)
        targets.append(t)
        return t
    }

    private static func label(_ c: [String: Any]) -> String { stripMnemonic(c["text"] as? String ?? "") }
    private static func items(_ c: [String: Any]) -> [[String: Any]] { c["items"] as? [[String: Any]] ?? [] }

    /// The controls as comdlg32 has them now.
    func apply(_ list: [[String: Any]]) {
        let now = list.map { c in
            "\(c["id"] ?? 0):\(c["kind"] ?? ""):\(c["group"] ?? -1):" +
                Self.items(c).map { "\($0["id"] ?? 0)" }.joined(separator: ",")
        }
        if now != shape {
            shape = now
            build(list)
        }
        for c in list { state(c) }
    }

    private func build(_ list: [[String: Any]]) {
        grid?.removeFromSuperview()
        targets.removeAll()
        controls.removeAll()
        rows.removeAll()
        radios.removeAll()
        fields.removeAll()

        let grid = NSGridView(numberOfColumns: 2, rows: 0)
        grid.rowSpacing = 8
        grid.columnSpacing = 8
        var members: [Int: NSStackView] = [:]      // a visual group's controls, side by side
        for c in list {
            guard let id = c["id"] as? Int, let kind = c["kind"] as? String else { continue }
            if kind == "group" {
                let label = NSTextField(labelWithString: Self.label(c))
                let row = NSStackView()
                row.orientation = .horizontal
                row.alignment = .firstBaseline
                row.spacing = 8
                members[id] = row
                controls[id] = label
                rows[id] = grid.addRow(with: [label, row])
                continue
            }
            guard let control = make(c, id: id, kind: kind) else { continue }
            controls[id] = control
            if let group = c["group"] as? Int, let row = members[group] {
                row.addArrangedSubview(control)
            } else if kind == "separator" {
                let row = grid.addRow(with: [control])
                row.mergeCells(in: NSRange(location: 0, length: 2))
                rows[id] = row
            } else {
                rows[id] = grid.addRow(with: [NSGridCell.emptyContentView, control])
            }
        }
        grid.column(at: 0).xPlacement = .trailing
        grid.column(at: 1).xPlacement = .leading
        grid.rowAlignment = .firstBaseline
        view.addArrangedSubview(grid)
        self.grid = grid
    }

    private func make(_ c: [String: Any], id: Int, kind: String) -> NSView? {
        let title = Self.label(c)
        switch kind {
        case "check":
            let t = target { [weak self] s in self?.emit(id, (s as? NSButton)?.state == .on ? 1 : 0) }
            return NSButton(checkboxWithTitle: title, target: t, action: #selector(Target.fire(_:)))
        case "button":
            let t = target { [weak self] _ in self?.emit(id, 0) }
            return NSButton(title: title, target: t, action: #selector(Target.fire(_:)))
        case "combo", "menu":
            let pullDown = kind == "menu"
            let popup = NSPopUpButton(frame: .zero, pullsDown: pullDown)
            if pullDown { popup.addItem(withTitle: title) }      // a pull-down's title is its first item
            for item in Self.items(c) {
                let menuItem = NSMenuItem(title: stripMnemonic(item["label"] as? String ?? ""), action: nil, keyEquivalent: "")
                menuItem.tag = item["id"] as? Int ?? 0
                popup.menu?.addItem(menuItem)
            }
            let t = target { [weak self] s in
                if let tag = (s as? NSPopUpButton)?.selectedItem?.tag { self?.emit(id, tag) }
            }
            popup.target = t
            popup.action = #selector(Target.fire(_:))
            return popup
        case "radios":
            let stack = NSStackView()
            stack.orientation = .vertical
            stack.alignment = .leading
            stack.spacing = 6
            var buttons: [Int: NSButton] = [:]
            for item in Self.items(c) {
                let itemID = item["id"] as? Int ?? 0
                // radio buttons with one action in one view are one set
                let t = target { [weak self] _ in self?.emit(id, itemID) }
                let b = NSButton(radioButtonWithTitle: stripMnemonic(item["label"] as? String ?? ""), target: t,
                                 action: #selector(Target.fire(_:)))
                buttons[itemID] = b
                stack.addArrangedSubview(b)
            }
            radios[id] = buttons
            return stack
        case "edit":
            let field = NSTextField(string: c["text"] as? String ?? "")
            field.widthAnchor.constraint(equalToConstant: 220).isActive = true
            field.delegate = self
            fields[ObjectIdentifier(field)] = id
            return field
        case "text":
            let label = NSTextField(wrappingLabelWithString: title)
            label.preferredMaxLayoutWidth = 360
            return label
        case "separator":
            let box = NSBox()
            box.boxType = .separator
            return box
        default:
            return nil
        }
    }

    private func state(_ c: [String: Any]) {
        guard let id = c["id"] as? Int, let view = controls[id] else { return }
        let enabled = c["enabled"] as? Bool ?? true, visible = c["visible"] as? Bool ?? true
        if let row = rows[id] { row.isHidden = !visible } else { view.isHidden = !visible }
        let value = c["value"] as? Int
        switch c["kind"] as? String {
        case "group":
            (view as? NSTextField)?.stringValue = Self.label(c)
        case "check":
            guard let b = view as? NSButton else { return }
            b.title = Self.label(c)
            b.state = value == 1 ? .on : .off
            b.isEnabled = enabled
        case "button":
            (view as? NSButton)?.title = Self.label(c)
            (view as? NSButton)?.isEnabled = enabled
        case "combo", "menu":
            guard let popup = view as? NSPopUpButton else { return }
            popup.isEnabled = enabled
            let first = popup.pullsDown ? 1 : 0
            if popup.pullsDown { popup.item(at: 0)?.title = Self.label(c) }
            for (i, item) in Self.items(c).enumerated() where first + i < popup.numberOfItems {
                let menuItem = popup.item(at: first + i)
                menuItem?.title = stripMnemonic(item["label"] as? String ?? "")
                menuItem?.isHidden = !(item["visible"] as? Bool ?? true)
                menuItem?.isEnabled = item["enabled"] as? Bool ?? true
            }
            popup.autoenablesItems = false
            if !popup.pullsDown {
                let index = value.map { popup.indexOfItem(withTag: $0) } ?? -1
                if index >= 0 { popup.selectItem(at: index) } else { popup.select(nil) }
            }
        case "radios":
            for item in Self.items(c) {
                guard let itemID = item["id"] as? Int, let b = radios[id]?[itemID] else { continue }
                b.title = stripMnemonic(item["label"] as? String ?? "")
                b.state = itemID == value ? .on : .off
                b.isHidden = !(item["visible"] as? Bool ?? true)
                b.isEnabled = enabled && (item["enabled"] as? Bool ?? true)
            }
        case "edit":
            guard let field = view as? NSTextField else { return }
            field.isEnabled = enabled
            // not under the user's typing: what they typed is what comdlg32 has, or soon will
            if field.currentEditor() == nil, let text = c["text"] as? String, field.stringValue != text {
                field.stringValue = text
            }
        case "text":
            (view as? NSTextField)?.stringValue = Self.label(c)
        default:
            break
        }
    }

    func controlTextDidChange(_ note: Notification) {
        guard let field = note.object as? NSTextField, let id = fields[ObjectIdentifier(field)] else { return }
        emit(id, 0, text: field.stringValue)
    }

    // MARK: tests

    /// What the panel shows of them.
    func query() -> [[String: Any]] {
        shape.compactMap { Int($0.split(separator: ":").first ?? "") }.compactMap { id -> [String: Any]? in
            guard let view = controls[id] else { return nil }
            var out: [String: Any] = ["id": id, "hidden": rows[id]?.isHidden ?? view.isHidden]
            switch view {
            case let popup as NSPopUpButton:
                out["title"] = popup.pullsDown ? popup.item(at: 0)?.title ?? "" : popup.titleOfSelectedItem ?? ""
                out["items"] = popup.itemTitles.dropFirst(popup.pullsDown ? 1 : 0).map { $0 }
                out["enabled"] = popup.isEnabled
            case let b as NSButton:
                out["title"] = b.title
                out["on"] = b.state == .on
                out["enabled"] = b.isEnabled
            case let f as NSTextField:
                out["title"] = f.stringValue
                out["enabled"] = f.isEnabled
            case let s as NSStackView:
                out["on"] = (radios[id] ?? [:]).filter { $0.value.state == .on }.map { $0.key }
                out["items"] = s.arrangedSubviews.compactMap { ($0 as? NSButton)?.title }
            default:
                break
            }
            return out
        }
    }

    /// What the user would do: {"t":"control","v":id} clicks it; "a":[item] picks
    /// an item; "s" types into an edit box.
    func inject(_ e: [String: Any]) {
        guard let id = e["v"] as? Int, let view = controls[id] else { return }
        let item = (e["a"] as? [Int])?.first
        switch view {
        case let popup as NSPopUpButton:
            guard let item = item, popup.indexOfItem(withTag: item) >= 0 else { return }
            popup.selectItem(withTag: item)
            _ = popup.sendAction(popup.action, to: popup.target)
        case let b as NSButton:
            b.performClick(nil)
        case let f as NSTextField:
            f.stringValue = e["s"] as? String ?? ""
            emit(id, 0, text: f.stringValue)
        default:
            if let item = item, let b = radios[id]?[item] { b.performClick(nil) }
        }
    }
}
