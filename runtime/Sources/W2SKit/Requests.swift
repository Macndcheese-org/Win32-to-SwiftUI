// Whole dialogs as macOS panels (map: messagebox, filedialog.open,
// filedialog.save). Never an AppKit modal session: a sheet on the owner, or
// the panel's own window; the Win32 thread provides the modality.
import AppKit
import UniformTypeIdentifiers

enum Requests {
    static func finish(_ request: Request, _ result: [String: Any]) {
        W2S.lock.lock()
        if request.result == nil { request.result = W2S.json(result) }
        W2S.lock.unlock()
    }

    static func start(kind: String, params: [String: Any], owner: NSWindow?, request: Request) {
        let sheetOwner = (owner?.isVisible ?? false) ? owner : nil
        switch kind {
        case "alert": alert(params, owner: sheetOwner, request: request)
        case "open", "save": panel(params, save: kind == "save", owner: sheetOwner, request: request)
        default: finish(request, ["error": "unknown request \(kind)"])
        }
    }

    // MARK: message box -> NSAlert (map: messagebox)

    final class AlertTarget: NSObject {
        let done: (Int) -> Void
        init(done: @escaping (Int) -> Void) { self.done = done }
        @objc func pressed(_ sender: NSButton) { done(sender.tag) }
    }

    static var retained: [ObjectIdentifier: AnyObject] = [:]

    static func alert(_ params: [String: Any], owner: NSWindow?, request: Request) {
        let alert = NSAlert()
        alert.messageText = params["messageText"] as? String ?? ""
        alert.informativeText = params["informativeText"] as? String ?? ""
        switch params["style"] as? String {
        case "critical": alert.alertStyle = .critical
        case "warning": alert.alertStyle = .warning
        default: alert.alertStyle = .informational
        }
        let buttons = params["buttons"] as? [[String: Any]] ?? [["title": "OK", "id": 1, "default": true]]
        var ids: [Int] = []
        for (i, b) in buttons.enumerated() {
            let button = alert.addButton(withTitle: b["title"] as? String ?? "OK")
            ids.append(b["id"] as? Int ?? 1)
            if b["cancel"] as? Bool == true && i != 0 { button.keyEquivalent = "\u{1b}" }
        }

        let complete: (Int) -> Void = { index in
            let id = index >= 0 && index < ids.count ? ids[index] : (ids.last ?? 2)
            finish(request, ["button": id])
        }
        request.inject = { event in
            // tests: {"t":"alertButton","v":index}
            guard let e = event as? [String: Any], let i = e["v"] as? Int, i < alert.buttons.count else { return }
            alert.buttons[i].performClick(nil)
        }

        if let owner = owner {
            alert.beginSheetModal(for: owner) { response in
                complete(response.rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue)
            }
            return
        }
        // no owner: the alert's own window, without a modal session
        let target = AlertTarget { index in
            complete(index)
            alert.window.orderOut(nil)
            retained.removeValue(forKey: ObjectIdentifier(alert))
        }
        for (i, button) in alert.buttons.enumerated() {
            button.tag = i
            button.target = target
            button.action = #selector(AlertTarget.pressed(_:))
        }
        retained[ObjectIdentifier(alert)] = [alert, target] as NSArray
        alert.layout()
        let window = alert.window
        if params["floating"] as? Bool == true { window.level = .floating }
        window.center()
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
    }

    // MARK: open/save -> NSOpenPanel/NSSavePanel (map: filedialog.open/.save)

    final class FilterTarget: NSObject {
        let apply: (Int) -> Void
        init(apply: @escaping (Int) -> Void) { self.apply = apply }
        @objc func changed(_ sender: NSPopUpButton) { apply(sender.indexOfSelectedItem) }
    }

    static func contentTypes(_ filter: [String: Any]?) -> [UTType] {
        let exts = filter?["exts"] as? [String] ?? []
        return exts.compactMap { UTType(filenameExtension: $0) }
    }

    static func panel(_ params: [String: Any], save: Bool, owner: NSWindow?, request: Request) {
        let filters = params["filters"] as? [[String: Any]] ?? []
        var filterIndex = max(1, params["filterIndex"] as? Int ?? 1)
        if filterIndex > filters.count { filterIndex = max(1, filters.count) }

        let panel: NSSavePanel
        if save {
            let s = NSSavePanel()
            s.canCreateDirectories = true
            s.nameFieldStringValue = params["name"] as? String ?? ""
            panel = s
        } else {
            let o = NSOpenPanel()
            o.canChooseFiles = true
            o.canChooseDirectories = false
            o.allowsMultipleSelection = params["multi"] as? Bool ?? false
            o.resolvesAliases = params["resolveLinks"] as? Bool ?? true
            panel = o
        }
        panel.showsHiddenFiles = params["showHidden"] as? Bool ?? false
        if let title = params["title"] as? String, !title.isEmpty { panel.message = title }
        if let dir = params["dir"] as? String { panel.directoryURL = URL(fileURLWithPath: dir, isDirectory: true) }

        func applyFilter(_ index: Int) {
            filterIndex = index + 1
            let types = index < filters.count ? contentTypes(filters[index]) : []
            panel.allowedContentTypes = types
            if save { panel.allowsOtherFileTypes = types.isEmpty }
        }
        var target: FilterTarget?
        if filters.count > 1 {
            // several filters: a pop-up like TextEdit's file-format menu
            let popup = NSPopUpButton(frame: .zero, pullsDown: false)
            popup.addItems(withTitles: filters.map { $0["name"] as? String ?? "" })
            popup.selectItem(at: filterIndex - 1)
            target = FilterTarget { applyFilter($0) }
            popup.target = target
            popup.action = #selector(FilterTarget.changed(_:))
            popup.sizeToFit()
            let box = NSStackView(views: [popup])
            box.edgeInsets = NSEdgeInsets(top: 8, left: 8, bottom: 8, right: 8)
            panel.accessoryView = box
            if let open = panel as? NSOpenPanel { open.isAccessoryViewDisclosed = true }
        }
        if !filters.isEmpty { applyFilter(filterIndex - 1) }

        let complete: (NSApplication.ModalResponse) -> Void = { response in
            var paths: [String] = []
            if response == .OK {
                if let open = panel as? NSOpenPanel { paths = open.urls.map { $0.path } }
                else if let url = panel.url { paths = [url.path] }
            }
            finish(request, ["paths": paths, "filterIndex": filterIndex])
            _ = target
        }
        request.inject = { event in
            // tests: {"t":"choose","s":"/unix/path"} or {"t":"cancel"}
            guard let e = event as? [String: Any] else { return }
            if e["t"] as? String == "choose", let path = e["s"] as? String {
                finish(request, ["paths": [path], "filterIndex": filterIndex])
                panel.cancel(nil)
            } else {
                panel.cancel(nil)
            }
        }
        if let owner = owner {
            panel.beginSheetModal(for: owner, completionHandler: complete)
        } else {
            NSApp.activate(ignoringOtherApps: true)
            panel.begin(completionHandler: complete)
        }
    }

    /// The newest request still open (for the test hooks).
    static func latestOpen() -> Request? {
        W2S.lock.lock()
        defer { W2S.lock.unlock() }
        return W2S.requests.values.filter { $0.result == nil }.max { $0.id < $1.id }
    }
}

/// tests/gallery drives the native side through these (W2SDebugQuery/Inject).
enum Debug {
    static func handle(handle: UInt64, op: [String: Any]) -> String {
        switch op["op"] as? String {
        case "query":
            guard let host = W2S.control(handle) else { return "{\"error\":\"no control\"}" }
            let snap = host.model.snap
            var out: [String: Any] = ["entry": host.entry, "attached": host.hosting?.superview != nil]
            if let v = snap.text { out["text"] = v }
            if let v = snap.checked { out["checked"] = v }
            if let v = snap.selection { out["selection"] = v }
            if let v = snap.selections { out["selections"] = v }
            if let v = snap.items { out["items"] = v }
            if let v = snap.value { out["value"] = v }
            if let v = snap.enabled { out["enabled"] = v }
            if let v = snap.rows { out["rowCount"] = v.count }
            if let hosting = host.hosting {
                out["frame"] = [hosting.frame.origin.x, hosting.frame.origin.y, hosting.frame.width, hosting.frame.height]
                out["hidden"] = hosting.superview?.isHidden ?? true
                out["inWindow"] = hosting.window != nil
            }
            return W2S.json(out)
        case "inject":
            guard let event = op["event"] as? [String: Any] else { return "{\"error\":\"no event\"}" }
            if handle == 0 || ["alertButton", "choose", "cancel"].contains(event["t"] as? String ?? "") {
                guard let request = Requests.latestOpen() else { return "{\"error\":\"no open request\"}" }
                request.inject?(event)
                return "{\"ok\":true}"
            }
            guard let host = W2S.control(handle) else { return "{\"error\":\"no control\"}" }
            // what the native control would do: update its own state, then tell Win32
            switch event["t"] as? String {
            case "text": host.model.snap.text = event["s"] as? String
            case "select": host.model.snap.selection = event["v"] as? Int
            case "selectMany": host.model.snap.selections = event["a"] as? [Int]
            case "value": host.model.snap.value = (event["v"] as? NSNumber)?.doubleValue
            default: break
            }
            host.emit(event)
            return "{\"ok\":true}"
        default:
            return "{\"error\":\"unknown op\"}"
        }
    }
}
