// FindText / ReplaceText as AppKit's own Find panel (map: findreplace).
//
// The panel (NSFindPanel, the one TextEdit shows with "Use Find Panel") only
// opens through a text view, and its buttons search that text view. Here the
// app searches: the panel is opened through a text view in no window, and its
// buttons are pointed at the Win32 side, which sends the app FINDMSGSTRING as
// the dialog did. AppKit enables the buttons for a text view in the main
// window, which there isn't, so their state is set again after each of the
// panel's updates. The controls are found by kind, action and tag, never by
// title (they're localized).
import AppKit

enum FindPanel {
    // main thread
    private static var proxy: NSTextView?
    private static weak var panel: NSWindow?
    private static var current: Session?

    final class Session: NSObject {
        let request: Request
        let replaceMode: Bool
        let noUpDown: Bool
        let panel: NSWindow
        var buttons: [Int: NSButton] = [:]     // NSTextFinder.Action raw values: 2 next ... 6 replace & find
        var findField: NSComboBox?
        var replaceField: NSComboBox?
        var ignoreCase: NSButton?
        var matching: NSPopUpButton?           // Contains, Starts With, Full Word
        var observers: [NSObjectProtocol] = []
        var ended = false

        init(request: Request, params: [String: Any], panel: NSWindow) {
            self.request = request
            self.panel = panel
            replaceMode = params["replaceMode"] as? Bool ?? false
            noUpDown = params["noUpDown"] as? Bool ?? false
            super.init()
        }

        func press(_ tag: Int) {
            guard let action = [2: "next", 3: "previous", 4: "replaceAll", 5: "replace", 6: "replace"][tag] else { return }
            request.emit(["t": "findWhat", "s": findField?.stringValue ?? ""])
            if replaceMode { request.emit(["t": "replaceWith", "s": replaceField?.stringValue ?? ""]) }
            let matchCase = ignoreCase.map { $0.state != .on } ?? false
            let wholeWord = matching.map { $0.indexOfSelectedItem == 2 } ?? false
            request.emit(["t": action, "v": (matchCase ? 1 : 0) | (wholeWord ? 2 : 0)])
        }

        @objc func pressed(_ sender: NSButton) { press(sender.tag) }

        /// What the buttons allow, as the dialog did: nothing to find, nothing to do.
        func applyEnabled() {
            let hasText = !(findField?.stringValue.isEmpty ?? true)
            for (tag, button) in buttons {
                let on: Bool
                switch tag {
                case 2: on = hasText
                case 3: on = hasText && !noUpDown && !replaceMode
                default: on = hasText && replaceMode
                }
                if button.isEnabled != on { button.isEnabled = on }
            }
            if let field = replaceField, field.isEnabled != replaceMode { field.isEnabled = replaceMode }
        }

        func end(_ result: [String: Any]) {
            guard !ended else { return }
            ended = true
            observers.forEach { NotificationCenter.default.removeObserver($0) }
            observers = []
            Requests.finish(request, result)
        }
    }

    static func start(_ params: [String: Any], request: Request) {
        // the panel opens through a text view; this one is in no window
        let proxy = self.proxy ?? {
            let text = NSTextView(frame: NSRect(x: 0, y: 0, width: 10, height: 10))
            text.usesFindPanel = true
            text.usesFindBar = false
            return text
        }()
        self.proxy = proxy
        let show = NSMenuItem()
        show.tag = Int(NSFindPanelAction.showFindPanel.rawValue)
        NSApp.activate(ignoringOtherApps: true)
        proxy.performFindPanelAction(show)
        if panel == nil { panel = NSApp.windows.first { String(describing: type(of: $0)) == "NSFindPanel" } }
        guard let panel = panel, let content = panel.contentView else {
            Requests.finish(request, ["declined": true])
            return
        }
        // one FindText at a time: an older one ends as if closed
        current?.end(["closed": true])

        let session = Session(request: request, params: params, panel: panel)
        func all(_ view: NSView) -> [NSView] { [view] + view.subviews.flatMap(all) }
        let views = all(content)
        let finderAction = NSSelectorFromString("performTextFinderAction:")
        let optionsAction = NSSelectorFromString("_setDefaultSearchOptions:")
        for button in views.compactMap({ $0 as? NSButton }) where !(button is NSPopUpButton) {
            if button.action == finderAction || button.action == #selector(Session.pressed(_:)),
               (2...6).contains(button.tag) {
                session.buttons[button.tag] = button
            } else if button.action == optionsAction {
                session.ignoreCase = button
            } else if button.action == nil {
                // "Wrap around": Windows' dialog has none, the app decides
                button.isHidden = true
            }
        }
        session.matching = views.compactMap { $0 as? NSPopUpButton }.first { $0.numberOfItems == 3 }
        // the Find field is above the Replace field
        let combos = views.compactMap { $0 as? NSComboBox }
            .sorted { $0.convert($0.bounds, to: nil).minY > $1.convert($1.bounds, to: nil).minY }
        session.findField = combos.first
        session.replaceField = combos.count > 1 ? combos[1] : nil
        guard session.findField != nil, session.buttons[2] != nil else {
            Requests.finish(request, ["declined": true])
            return
        }

        for button in session.buttons.values {
            button.target = session
            button.action = #selector(Session.pressed(_:))
        }
        session.findField?.stringValue = params["find"] as? String ?? ""
        session.replaceField?.stringValue = session.replaceMode ? params["replace"] as? String ?? "" : ""
        if let box = session.ignoreCase {
            box.state = params["matchCase"] as? Bool == true ? .off : .on
            box.isHidden = params["hideMatchCase"] as? Bool ?? false
            box.isEnabled = !(params["noMatchCase"] as? Bool ?? false)
        }
        if let popup = session.matching {
            popup.autoenablesItems = false
            popup.item(at: 1)?.isEnabled = false        // "Starts With": Windows has no such search
            popup.selectItem(at: params["wholeWord"] as? Bool == true ? 2 : 0)
            popup.isHidden = params["hideWholeWord"] as? Bool ?? false
            popup.isEnabled = !(params["noWholeWord"] as? Bool ?? false)
        }
        session.applyEnabled()

        let center = NotificationCenter.default
        session.observers.append(center.addObserver(forName: NSWindow.didUpdateNotification, object: panel,
                                                    queue: nil) { [weak session] _ in session?.applyEnabled() })
        session.observers.append(center.addObserver(forName: NSControl.textDidChangeNotification,
                                                    object: session.findField, queue: nil) { [weak session] _ in
            session?.applyEnabled()
        })
        session.observers.append(center.addObserver(forName: NSWindow.willCloseNotification, object: panel,
                                                    queue: .main) { [weak session] _ in
            session?.end(["closed": true])
            if current === session { current = nil }
        })
        request.update = { [weak session] spec in
            guard let session = session else { return }
            if spec["close"] as? Bool == true { session.panel.close() }
            if spec["front"] as? Bool == true { session.panel.makeKeyAndOrderFront(nil) }
        }
        request.inject = { [weak session] event in
            // tests: {"t":"press","v":tag,"find":..,"replace":..,"matchCase":..,"wholeWord":..}, {"t":"close"}
            guard let session = session, let e = event as? [String: Any] else { return }
            if e["t"] as? String == "press" {
                if let s = e["find"] as? String { session.findField?.stringValue = s }
                if let s = e["replace"] as? String { session.replaceField?.stringValue = s }
                if let b = e["matchCase"] as? Bool { session.ignoreCase?.state = b ? .off : .on }
                if let b = e["wholeWord"] as? Bool { session.matching?.selectItem(at: b ? 2 : 0) }
                session.applyEnabled()
                if let button = session.buttons[e["v"] as? Int ?? 0], button.isEnabled { button.performClick(nil) }
            } else {
                session.panel.close()
            }
        }
        request.query = { [weak session] in
            guard let session = session else { return [:] }
            return ["visible": session.panel.isVisible, "find": session.findField?.stringValue ?? "",
                    "replace": session.replaceField?.stringValue ?? "",
                    "matchCase": session.ignoreCase.map { $0.state != .on } ?? false,
                    "wholeWord": session.matching.map { $0.indexOfSelectedItem == 2 } ?? false,
                    "replaceEnabled": session.replaceField?.isEnabled ?? false,
                    "previousEnabled": session.buttons[3]?.isEnabled ?? false,
                    "nextEnabled": session.buttons[2]?.isEnabled ?? false]
        }
        request.window = panel
        current = session
        panel.makeKeyAndOrderFront(nil)
        request.emit(["t": "shown"])
    }
}
