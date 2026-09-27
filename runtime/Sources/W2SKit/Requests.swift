// Whole dialogs as macOS panels (map: messagebox, taskdialog, filedialog.*,
// colordialog, fontdialog, printdialog). Never an AppKit modal session: a
// sheet on the owner, or the panel's own window; the Win32 thread provides the
// modality.
import AppKit
import SwiftUI
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
        case "taskdialog": taskDialog(params, owner: sheetOwner, request: request)
        case "open", "save": panel(params, save: kind == "save", owner: sheetOwner, request: request)
        case "color": colorPanel(params, request: request)
        case "font": fontPanel(params, request: request)
        case "print": printPanel(params, owner: sheetOwner, request: request)
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

    /// Shows an alert without a modal session: a sheet on the owner, or its own window.
    static func present(_ alert: NSAlert, owner: NSWindow?, floating: Bool, keep: [AnyObject]) {
        let kept: [AnyObject] = [alert] + keep
        retained[ObjectIdentifier(alert)] = kept as NSArray
        if let owner = owner {
            alert.beginSheetModal(for: owner) { _ in }
            return
        }
        alert.layout()
        let window = alert.window
        if floating { window.level = .floating }
        window.center()
        NSApp.activate(ignoringOtherApps: true)
        window.makeKeyAndOrderFront(nil)
    }

    static func dismiss(_ alert: NSAlert) {
        if let parent = alert.window.sheetParent { parent.endSheet(alert.window) } else { alert.window.orderOut(nil) }
        retained.removeValue(forKey: ObjectIdentifier(alert))
    }

    // MARK: task dialog -> NSAlert + NSHostingView accessory (map: taskdialog)

    final class ButtonTarget: NSObject {
        let pressed: (Int) -> Void
        init(pressed: @escaping (Int) -> Void) { self.pressed = pressed }
        @objc func press(_ sender: NSButton) { pressed(sender.tag) }
        @objc func toggle(_ sender: NSButton) { pressed(sender.state == .on ? 1 : 0) }
    }

    static func taskDialog(_ params: [String: Any], owner: NSWindow?, request: Request) {
        let alert = NSAlert()
        alert.messageText = params["messageText"] as? String ?? ""
        alert.informativeText = params["informativeText"] as? String ?? ""
        switch params["style"] as? String {
        case "critical": alert.alertStyle = .critical
        case "warning": alert.alertStyle = .warning
        default: alert.alertStyle = .informational
        }
        // no main instruction: the content went into messageText
        let contentIsMessage = params["informativeText"] == nil && params["content"] == nil
            && !(params["messageText"] as? String ?? "").isEmpty && params["links"] as? Bool != true

        let model = TaskModel(params)
        model.emit = { request.emit($0) }
        var closed = false
        let buttons = ButtonTarget { id in request.emit(["t": "button", "v": id]) }
        for (i, b) in (params["buttons"] as? [[String: Any]] ?? []).enumerated() {
            let button = alert.addButton(withTitle: b["title"] as? String ?? "OK")
            button.tag = b["id"] as? Int ?? 1
            button.target = buttons
            button.action = #selector(ButtonTarget.press(_:))
            if b["cancel"] as? Bool == true && i != 0 { button.keyEquivalent = "\u{1b}" }
        }
        let verify = ButtonTarget { on in request.emit(["t": "verify", "v": on]) }
        if let text = params["verification"] as? String {
            alert.showsSuppressionButton = true
            alert.suppressionButton?.title = text
            alert.suppressionButton?.state = params["verified"] as? Bool == true ? .on : .off
            alert.suppressionButton?.target = verify
            alert.suppressionButton?.action = #selector(ButtonTarget.toggle(_:))
        }
        var hosting: NSHostingView<TaskAccessory>?
        func relayout() {
            guard let hosting = hosting else { return }
            hosting.setFrameSize(hosting.fittingSize)
            alert.layout()
        }
        if model.hasAccessory {
            let view = NSHostingView(rootView: TaskAccessory(model: model))
            view.setFrameSize(view.fittingSize)
            alert.accessoryView = view
            hosting = view
        }
        model.relayout = { DispatchQueue.main.async { relayout() } }

        func close(_ id: Int) {
            guard !closed else { return }
            closed = true
            finish(request, ["button": id, "radio": model.radio ?? 0,
                             "verified": alert.suppressionButton?.state == .on])
            dismiss(alert)
        }
        request.update = { p in
            if let id = p["close"] as? Int { close(id); return }
            if let e = p["enable"] as? [Int], e.count == 2 {
                alert.buttons.first(where: { $0.tag == e[0] })?.isEnabled = e[1] != 0
            }
            if let element = p["element"] as? Int, let text = p["text"] as? String {
                switch element {
                case 0:     // TDE_CONTENT
                    if model.links || !model.content.isEmpty { model.content = text }
                    else if contentIsMessage { alert.messageText = text }
                    else { alert.informativeText = text }
                case 1: model.expandedText = text
                case 2: model.footer = text
                case 3: alert.messageText = text
                default: break
                }
            }
            if let v = p["progressPos"] as? Double { model.progressPos = v }
            if let v = p["progressMin"] as? Double { model.progressMin = v }
            if let v = p["progressMax"] as? Double { model.progressMax = v }
            if let v = p["progressState"] as? Int { model.progressState = v }
            if let v = p["marquee"] as? Bool { model.marquee = v; model.progress = true }
            if let v = p["radio"] as? Int { model.radio = v }
            if let v = p["enableRadio"] as? [Int], v.count == 2, let i = model.radios.firstIndex(where: { $0.id == v[0] }) {
                model.radios[i].enabled = v[1] != 0
            }
            if let v = p["verified"] as? Bool { alert.suppressionButton?.state = v ? .on : .off }
            relayout()
        }
        request.inject = { event in
            // tests: {"t":"press","v":id}, {"t":"radio","v":id}, {"t":"verify","v":0|1}, {"t":"expando","v":0|1}
            guard let e = event as? [String: Any], let v = e["v"] as? Int else { return }
            switch e["t"] as? String {
            case "press": alert.buttons.first(where: { $0.tag == v })?.performClick(nil)
            case "radio": model.radio = v; request.emit(["t": "radio", "v": v])
            case "verify":
                alert.suppressionButton?.state = v != 0 ? .on : .off
                request.emit(["t": "verify", "v": v])
            case "expando": model.expanded = v != 0; request.emit(["t": "expando", "v": v])
            default: break
            }
        }
        request.query = {
            ["messageText": alert.messageText, "informativeText": alert.informativeText,
             "content": model.content, "footer": model.footer, "expandedText": model.expandedText,
             "buttons": alert.buttons.map { $0.title },
             "enabled": alert.buttons.map { $0.isEnabled },
             "radio": model.radio ?? 0, "verified": alert.suppressionButton?.state == .on,
             "progressPos": model.progressPos, "progressMax": model.progressMax, "marquee": model.marquee]
        }
        present(alert, owner: owner, floating: false, keep: [buttons, verify, model])
        request.emit(["t": "created"])
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

    /// Reports what the user looks at (SHBrowseForFolder's BFFM_SELCHANGED) and
    /// holds the OK button back while the app says so (BFFM_ENABLEOK).
    final class PanelDelegate: NSObject, NSOpenSavePanelDelegate {
        var okEnabled = true
        var events = false
        let request: Request
        init(request: Request) { self.request = request }

        func panelSelectionDidChange(_ sender: Any?) {
            guard events, let panel = sender as? NSSavePanel else { return }
            if let url = panel.url ?? panel.directoryURL { request.emit(["t": "selchange", "s": url.path]) }
        }

        func panel(_ sender: Any, didChangeToDirectoryURL url: URL?) {
            if events, let url = url { request.emit(["t": "selchange", "s": url.path]) }
        }

        func panel(_ sender: Any, validate url: URL) throws {
            // a cancelled-by-user error blocks OK without an alert
            if !okEnabled { throw CocoaError(.userCancelled) }
        }
    }

    static func panel(_ params: [String: Any], save: Bool, owner: NSWindow?, request: Request) {
        let filters = params["filters"] as? [[String: Any]] ?? []
        let folders = params["folders"] as? Bool ?? false
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
            // folder pickers (SHBrowseForFolder, FOS_PICKFOLDERS) are the same panel
            o.canChooseFiles = !folders || (params["files"] as? Bool ?? false)
            o.canChooseDirectories = folders
            o.canCreateDirectories = folders && (params["newFolder"] as? Bool ?? true)
            o.allowsMultipleSelection = params["multi"] as? Bool ?? false
            o.resolvesAliases = params["resolveLinks"] as? Bool ?? true
            panel = o
        }
        panel.showsHiddenFiles = params["showHidden"] as? Bool ?? false
        let message = params["message"] as? String ?? params["title"] as? String ?? ""
        var status = ""
        func showMessage() {
            panel.message = [message, status].filter { !$0.isEmpty }.joined(separator: "\n")
        }
        showMessage()
        if let prompt = params["prompt"] as? String, !prompt.isEmpty { panel.prompt = prompt }
        if let title = params["windowTitle"] as? String, !title.isEmpty { panel.title = title }
        if let dir = params["dir"] as? String { panel.directoryURL = URL(fileURLWithPath: dir, isDirectory: true) }
        let delegate = PanelDelegate(request: request)
        delegate.events = params["events"] as? Bool ?? false
        delegate.okEnabled = params["okEnabled"] as? Bool ?? true
        panel.delegate = delegate

        func applyFilter(_ index: Int) {
            filterIndex = index + 1
            let types = index < filters.count ? contentTypes(filters[index]) : []
            panel.allowedContentTypes = types
            if save { panel.allowsOtherFileTypes = types.isEmpty || !(params["strict"] as? Bool ?? false) }
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
            _ = delegate
        }
        request.update = { p in
            // the app, while the panel is up (BFFM_SETSELECTION, BFFM_SETOKTEXT, ...)
            if let dir = p["dir"] as? String { panel.directoryURL = URL(fileURLWithPath: dir, isDirectory: true) }
            if let prompt = p["prompt"] as? String { panel.prompt = prompt }
            if let text = p["status"] as? String { status = text; showMessage() }
            if let ok = p["okEnabled"] as? Bool { delegate.okEnabled = ok }
        }
        request.inject = { event in
            // tests: {"t":"choose","s":"/unix/path"} or {"t":"cancel"}
            guard let e = event as? [String: Any] else { return }
            if e["t"] as? String == "choose", let path = e["s"] as? String {
                if !delegate.okEnabled { return }
                finish(request, ["paths": [path], "filterIndex": filterIndex])
                panel.cancel(nil)
            } else if e["t"] as? String == "look", let path = e["s"] as? String {
                // what browsing to a folder reports
                panel.directoryURL = URL(fileURLWithPath: path, isDirectory: true)
                if delegate.events { request.emit(["t": "selchange", "s": path]) }
            } else {
                panel.cancel(nil)
            }
        }
        request.query = {
            ["message": panel.message ?? "", "prompt": panel.prompt ?? "", "dir": panel.directoryURL?.path ?? "",
             "folders": (panel as? NSOpenPanel)?.canChooseDirectories ?? false,
             "multi": (panel as? NSOpenPanel)?.allowsMultipleSelection ?? false,
             "okEnabled": delegate.okEnabled]
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

/// A task dialog's radio buttons, progress bar, expanded information and footer.
final class TaskModel: ObservableObject {
    struct Radio: Identifiable, Equatable { let id: Int; let title: String; var enabled = true }

    @Published var content: String
    @Published var radios: [Radio]
    @Published var radio: Int?
    @Published var expandedText: String
    @Published var expanded: Bool
    @Published var footer: String
    @Published var progress: Bool
    @Published var marquee: Bool
    @Published var progressPos = 0.0
    @Published var progressMin = 0.0
    @Published var progressMax = 100.0
    @Published var progressState = 1      // PBST_NORMAL
    let links: Bool
    let expandFooter: Bool
    let expandLabel: String
    let collapseLabel: String
    var emit: ([String: Any]) -> Void = { _ in }
    var relayout: () -> Void = {}

    init(_ p: [String: Any]) {
        content = p["content"] as? String ?? ""
        links = p["links"] as? Bool ?? false
        radios = (p["radios"] as? [[String: Any]] ?? []).map { Radio(id: $0["id"] as? Int ?? 0, title: $0["title"] as? String ?? "") }
        radio = (p["radio"] as? Int).flatMap { $0 != 0 ? $0 : nil }
        expandedText = p["expandedText"] as? String ?? ""
        expanded = p["expanded"] as? Bool ?? false
        expandFooter = p["expandFooter"] as? Bool ?? false
        expandLabel = p["expandLabel"] as? String ?? "Show Details"
        collapseLabel = p["collapseLabel"] as? String ?? "Hide Details"
        footer = p["footer"] as? String ?? ""
        progress = p["progress"] as? Bool ?? false
        marquee = p["marquee"] as? Bool ?? false
    }

    var hasAccessory: Bool {
        !content.isEmpty || !radios.isEmpty || progress || !expandedText.isEmpty || !footer.isEmpty
    }
}

struct TaskAccessory: View {
    @ObservedObject var model: TaskModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if !model.content.isEmpty { LinkedText(text: model.content, links: model.links, emit: model.emit) }
            if !model.radios.isEmpty {
                Picker("", selection: Binding<Int?>(
                    get: { model.radio },
                    set: { id in
                        model.radio = id
                        if let id = id { model.emit(["t": "radio", "v": id]) }
                    })) {
                    ForEach(model.radios) { r in Text(r.title).tag(Optional(r.id)) }
                }
                .pickerStyle(.radioGroup)
                .labelsHidden()
            }
            if model.progress {
                let tint: Color? = model.progressState == 2 ? .red : model.progressState == 3 ? .yellow : nil
                Group {
                    if model.marquee {
                        ProgressView()
                    } else {
                        let span = max(1, model.progressMax - model.progressMin)
                        ProgressView(value: min(max(model.progressPos - model.progressMin, 0), span), total: span)
                    }
                }
                .progressViewStyle(.linear)
                .tint(tint)
            }
            if !model.expandedText.isEmpty && !model.expandFooter { expander }
            if !model.footer.isEmpty {
                Divider()
                LinkedText(text: model.footer, links: model.links, emit: model.emit)
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            if !model.expandedText.isEmpty && model.expandFooter { expander }
        }
        .frame(width: 320, alignment: .leading)
        .fixedSize(horizontal: false, vertical: true)
    }

    var expander: some View {
        DisclosureGroup(isExpanded: Binding(
            get: { model.expanded },
            set: { on in
                model.expanded = on
                model.emit(["t": "expando", "v": on ? 1 : 0])
                model.relayout()
            })) {
            LinkedText(text: model.expandedText, links: model.links, emit: model.emit).font(.callout)
        } label: {
            Text(model.expanded ? model.collapseLabel : model.expandLabel)
        }
    }
}

/// Text with <a href="...">links</a> (TDF_ENABLE_HYPERLINKS, SysLink): a click
/// tells the app, which opens the link itself, as on Windows.
struct LinkedText: View {
    let text: String
    let links: Bool
    let emit: ([String: Any]) -> Void

    var body: some View {
        if links {
            Text(LinkMarkup.attributed(text))
                .environment(\.openURL, OpenURLAction { url in
                    emit(["t": "link", "s": LinkMarkup.href(url)])
                    return .handled
                })
        } else {
            Text(text)
        }
    }
}

enum LinkMarkup {
    /// The href travels in a w2s-link: URL (it needn't be a URL itself).
    static func attributed(_ markup: String) -> AttributedString {
        var out = AttributedString()
        var rest = Substring(markup)
        while let open = rest.range(of: "<a", options: .caseInsensitive) {
            guard let tagEnd = rest[open.upperBound...].firstIndex(of: ">"),
                  let close = rest[tagEnd...].range(of: "</a>", options: .caseInsensitive)
            else { break }
            out.append(AttributedString(String(rest[..<open.lowerBound])))
            let label = String(rest[rest.index(after: tagEnd)..<close.lowerBound])
            var run = AttributedString(label)
            var link = URLComponents()
            link.scheme = "w2s-link"
            link.path = attribute("href", in: rest[open.upperBound..<tagEnd]) ?? label
            run.link = link.url
            out.append(run)
            rest = rest[close.upperBound...]
        }
        out.append(AttributedString(String(rest)))
        return out
    }

    static func attribute(_ name: String, in tag: Substring) -> String? {
        guard let key = tag.range(of: name + "=", options: .caseInsensitive) else { return nil }
        var value = tag[key.upperBound...]
        guard let quote = value.first, quote == "\"" || quote == "'" else {
            return String(value.prefix { !$0.isWhitespace })
        }
        value = value.dropFirst()
        return String(value.prefix { $0 != quote })
    }

    static func href(_ url: URL) -> String {
        URLComponents(url: url, resolvingAgainstBaseURL: false)?.path ?? url.absoluteString
    }
}

/// tests/gallery drives the native side through these (W2SDebugQuery/Inject).
enum Debug {
    static func handle(handle: UInt64, op: [String: Any]) -> String {
        switch op["op"] as? String {
        case "query":
            if let host = W2S.control(handle), let bar = host.owned as? MenuBar { return W2S.json(bar.debugState()) }
            if handle == 0 {
                // the open alert or panel
                guard let request = Requests.latestOpen() else { return "{\"error\":\"no open request\"}" }
                var out = request.query?() ?? [:]
                out["open"] = true
                return W2S.json(out)
            }
            guard let host = W2S.control(handle) else { return "{\"error\":\"no control\"}" }
            let snap = host.model.snap
            // the native model as it stands (optimistic changes included), minus the pixels
            var out: [String: Any] = (try? JSONEncoder().encode(snap))
                .flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] } ?? [:]
            out["imageBGRA"] = nil
            out["images"] = nil
            out["symbols"] = nil
            out["imageCount"] = host.model.images.count
            out["imageSymbols"] = Dictionary(uniqueKeysWithValues: host.model.imageSymbols.map { (String($0.key), $0.value) })
            out["entry"] = host.entry
            out["attached"] = host.hosting?.superview != nil || host.owned != nil
            out["os"] = W2S.osVersion.major
            W2S.lock.lock()
            out["look"] = ["version": Look.version, "dark": Look.dark,
                           "btnFace": Look.colors.count > 15 ? Int(Look.colors[15]) : -1,
                           "window": Look.colors.count > 5 ? Int(Look.colors[5]) : -1]
            W2S.lock.unlock()
            if let v = snap.rows { out["rowCount"] = v.count }
            if let v = snap.columns { out["columns"] = v.filter { ($0.width ?? 1) > 0 }.map { $0.title } }
            if let hosting = host.hosting {
                out["frame"] = [hosting.frame.origin.x, hosting.frame.origin.y, hosting.frame.width, hosting.frame.height]
                out["hidden"] = hosting.superview?.isHidden ?? true
                out["inWindow"] = hosting.window != nil
                // clicks in the middle of the control: native, or through to wine?
                // (hitTest takes the point in the superview's coordinates, as frame is)
                out["passThrough"] = hosting.hitTest(NSPoint(x: hosting.frame.midX, y: hosting.frame.midY)) == nil
                out["appearance"] = hosting.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua])?.rawValue ?? ""
            }
            return W2S.json(out)
        case "inject":
            guard let event = op["event"] as? [String: Any] else { return "{\"error\":\"no event\"}" }
            if event["t"] as? String == "lookOverride" {
                // tests: {"t":"lookOverride","s":"dark"|"light"|""}: as if macOS switched
                switch event["s"] as? String {
                case "dark": Look.override = .darkAqua
                case "light": Look.override = .aqua
                default: Look.override = nil
                }
                Look.refresh()
                return "{\"ok\":true}"
            }
            if let host = W2S.control(handle), let bar = host.owned as? MenuBar {
                bar.debugInject(event)
                return "{\"ok\":true}"
            }
            if handle == 0 || ["alertButton", "choose", "cancel", "press", "look", "popupChoose"].contains(event["t"] as? String ?? "") {
                guard let request = Requests.latestOpen() else { return "{\"error\":\"no open request\"}" }
                request.inject?(event)
                return "{\"ok\":true}"
            }
            guard let host = W2S.control(handle) else { return "{\"error\":\"no control\"}" }
            if event["t"] as? String == "realClick" {
                // a mouse click at the view's centre through AppKit, as the mouse makes one:
                // winemac's routing, hit-testing and the SwiftUI control's own tracking
                guard let view = host.hosting, let window = view.window else { return "{\"error\":\"not in a window\"}" }
                let rect = view.convert(view.bounds, to: nil)
                let point = NSPoint(x: rect.midX, y: rect.midY)
                let hit = window.contentView.flatMap { $0.hitTest($0.superview?.convert(point, from: nil) ?? point) }
                var chain: [String] = []
                var v: NSView? = hit
                while let x = v, chain.count < 12 { chain.append(String(describing: type(of: x))); v = x.superview }
                let probe = NSEvent.mouseEvent(with: .leftMouseDown, location: point, modifierFlags: [], timestamp: 0,
                                               windowNumber: window.windowNumber, context: nil, eventNumber: 0,
                                               clickCount: 1, pressure: 1)
                let diag: [String: Any] = [
                    "ok": true, "key": window.isKeyWindow, "main": window.isMainWindow, "appActive": NSApp.isActive,
                    "canBecomeKey": window.canBecomeKey, "windowClass": String(describing: type(of: window)),
                    "hit": chain, "firstMouse": probe.map { hit?.acceptsFirstMouse(for: $0) ?? false } ?? false,
                ]
                DispatchQueue.main.async {
                    func mouse(_ type: NSEvent.EventType) -> NSEvent? {
                        NSEvent.mouseEvent(with: type, location: point, modifierFlags: [],
                                           timestamp: ProcessInfo.processInfo.systemUptime,
                                           windowNumber: window.windowNumber, context: nil, eventNumber: 0,
                                           clickCount: 1, pressure: type == .leftMouseDown ? 1 : 0)
                    }
                    // the up waits in the queue for the control's tracking loop
                    if let up = mouse(.leftMouseUp) { NSApp.postEvent(up, atStart: false) }
                    if let down = mouse(.leftMouseDown) { NSApp.sendEvent(down) }
                }
                return W2S.json(diag)
            }
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
