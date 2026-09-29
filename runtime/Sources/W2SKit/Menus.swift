// Win32 menus as NSMenus (map: menu.bar, menu.popup). SwiftUI's .commands only
// exists for SwiftUI App scenes and wine's application is AppKit, so these are
// NSMenus: the window's menu in the Mac menu bar, TrackPopupMenu as a popup.
import AppKit

/// An NSMenu that remembers the Win32 menu it shows.
final class W2SMenu: NSMenu {
    var win32Menu = 0
    var position = 0
    var topLevel = false
    var textCommands = false        // the app's Edit menu: the Mac's text commands at its top
}

/// macOS's own words for the standard menus and commands, in every language it
/// has (AppKit's tables): to know an app's Edit and Help menus whatever its
/// language, and to title the commands the Mac adds as the Mac does.
enum MenuWords {
    private static var tables: [String: [String: [String: String]]] = [:]

    private static func table(_ name: String) -> [String: [String: String]] {
        if let t = tables[name] { return t }
        var out: [String: [String: String]] = [:]
        if let path = Bundle(for: NSApplication.self).path(forResource: name, ofType: "loctable"),
           let dict = NSDictionary(contentsOfFile: path) as? [String: Any] {
            for (lang, value) in dict { if let words = value as? [String: String] { out[lang] = words } }
        }
        tables[name] = out
        return out
    }

    static func folded(_ s: String) -> String {
        s.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: nil).trimmingCharacters(in: .whitespaces)
    }

    /// every language's word for key
    static func all(_ key: String, _ names: [String]) -> Set<String> {
        var out: Set<String> = [folded(key)]
        for name in names { for words in table(name).values { if let w = words[key] { out.insert(folded(w)) } } }
        return out
    }

    /// the user's language's word for key
    static func local(_ key: String, _ names: [String]) -> String {
        for lang in Locale.preferredLanguages {
            let code = lang.replacingOccurrences(of: "-", with: "_")
            for candidate in [code, String(code.prefix { $0 != "_" })] {
                for name in names { if let w = table(name)[candidate]?[key] { return w } }
            }
        }
        return key
    }

    static let edit = all("Edit", ["InputManager"])
    // "?" is a Help menu's title in many Windows apps
    static let help = all("Help", ["MenuCommands", "HelpManager"]).union(["?"])
}

enum MenuBuild {
    /// "&Save\tCtrl+S" -> ("Save", "s", [.command]). Ctrl becomes Command, as Mac
    /// apps expect; plain letters never become key equivalents (they're typing).
    static func split(_ text: String) -> (title: String, key: String, mods: NSEvent.ModifierFlags) {
        let parts = text.split(separator: "\t", maxSplits: 1, omittingEmptySubsequences: false)
        let title = stripMnemonic(String(parts.first ?? ""))
        guard parts.count > 1 else { return (title, "", []) }
        var mods: NSEvent.ModifierFlags = []
        var key = ""
        let tokens = parts[1].split(separator: "+").map { $0.trimmingCharacters(in: .whitespaces) }
        for (i, raw) in tokens.enumerated() {
            let t = raw.lowercased()
            if i < tokens.count - 1 {
                switch t {
                case "ctrl", "control", "strg", "ctl": mods.insert(.command)
                case "shift", "maj", "umschalt", "mayús", "mayus": mods.insert(.shift)
                case "alt", "option": mods.insert(.option)
                default: break
                }
                continue
            }
            switch t {
            case "del", "delete", "suppr", "entf", "supr": key = String(UnicodeScalar(NSDeleteFunctionKey)!)
            case "ins", "insert", "inser", "einfg": key = String(UnicodeScalar(NSInsertFunctionKey)!)
            case "home", "début", "pos1": key = String(UnicodeScalar(NSHomeFunctionKey)!)
            case "end", "fin", "ende": key = String(UnicodeScalar(NSEndFunctionKey)!)
            case "esc", "échap", "echap": key = "\u{1b}"
            case "tab": key = "\t"
            case "enter", "entrée", "return": key = "\r"
            case "space", "espace": key = " "
            case "backspace", "retour arrière": key = "\u{8}"
            default:
                if t.count > 1, t.first == "f", let n = Int(t.dropFirst()), (1...24).contains(n) {
                    key = String(UnicodeScalar(NSF1FunctionKey + n - 1)!)
                } else if raw.count == 1 {
                    key = t
                }
            }
        }
        // a letter or digit alone would steal typing
        let isFunction = key.unicodeScalars.first.map { $0.value >= 0xF700 } ?? false
        if !mods.contains(.command) && !isFunction { return (title, "", []) }
        return (title, key, mods)
    }

    static func fill(_ menu: NSMenu, items: [[String: Any]], target: AnyObject, action: Selector,
                     delegate: NSMenuDelegate?) {
        menu.removeAllItems()
        menu.autoenablesItems = false
        for spec in items {
            if spec["sep"] as? Bool == true {
                if menu.items.last.map({ !$0.isSeparatorItem }) ?? false { menu.addItem(.separator()) }
                continue
            }
            let parts = split(spec["text"] as? String ?? "")
            let item = NSMenuItem(title: parts.title, action: action, keyEquivalent: parts.key)
            item.keyEquivalentModifierMask = parts.mods
            item.target = target
            item.tag = spec["id"] as? Int ?? 0
            item.isEnabled = spec["disabled"] as? Bool != true
            item.state = spec["checked"] as? Bool == true ? .on : .off
            if let sub = spec["sub"] as? [String: Any] {
                let submenu = W2SMenu(title: parts.title)
                submenu.win32Menu = sub["menu"] as? Int ?? 0
                submenu.position = sub["pos"] as? Int ?? 0
                submenu.delegate = delegate
                fill(submenu, items: sub["items"] as? [[String: Any]] ?? [], target: target, action: action,
                     delegate: delegate)
                item.submenu = submenu
                item.action = nil
            }
            menu.addItem(item)
        }
        if menu.items.last?.isSeparatorItem == true { menu.removeItem(at: menu.items.count - 1) }
    }

    /// The Mac's text commands at the top of an app's Edit menu that has none: for
    /// the field being typed in, else the app (wine turns them into Ctrl+X/C/V).
    /// A key the app's menus already use stays the app's.
    static func addTextCommands(_ menu: NSMenu, taken: Set<String>) {
        let commands: [(String, [String], Selector, String, NSEvent.ModifierFlags)] = [
            ("Undo", ["FunctionKeyNames"], Selector(("undo:")), "z", [.command]),
            ("Redo", ["FunctionKeyNames"], Selector(("redo:")), "z", [.command, .shift]),
            ("", [], Selector(("")), "", []),
            ("Cut", ["MenuCommands"], #selector(NSText.cut(_:)), "x", [.command]),
            ("Copy", ["MenuCommands"], #selector(NSText.copy(_:)), "c", [.command]),
            ("Paste", ["MenuCommands"], #selector(NSText.paste(_:)), "v", [.command]),
            ("Select All", ["MenuCommands"], #selector(NSText.selectAll(_:)), "a", [.command]),
            ("", [], Selector(("")), "", []),
        ]
        var at = 0
        for (key, names, action, keyEquivalent, mods) in commands {
            if key.isEmpty {
                menu.insertItem(.separator(), at: at)
            } else {
                let taken = taken.contains(keyString(keyEquivalent, mods))
                let item = NSMenuItem(title: MenuWords.local(key, names), action: action,
                                      keyEquivalent: taken ? "" : keyEquivalent)
                item.keyEquivalentModifierMask = mods
                item.target = nil          // the first responder
                item.isEnabled = NSApp.target(forAction: action, to: nil, from: item) != nil
                menu.insertItem(item, at: at)
            }
            at += 1
        }
    }

    static func keyString(_ key: String, _ mods: NSEvent.ModifierFlags) -> String {
        "\(mods.intersection([.command, .shift, .option, .control]).rawValue):\(key.lowercased())"
    }

    /// the key equivalents an app's menus use
    static func keys(_ items: [[String: Any]]) -> Set<String> {
        var out: Set<String> = []
        for spec in items {
            let parts = split(spec["text"] as? String ?? "")
            if !parts.key.isEmpty { out.insert(keyString(parts.key, parts.mods)) }
            if let sub = spec["sub"] as? [String: Any] { out.formUnion(keys(sub["items"] as? [[String: Any]] ?? [])) }
        }
        return out
    }

    static func title(_ spec: [String: Any]) -> String { MenuWords.folded(split(spec["text"] as? String ?? "").title) }

    static func isHelp(_ spec: [String: Any]) -> Bool {
        spec["sub"] != nil && (spec["right"] as? Bool == true || MenuWords.help.contains(title(spec)))
    }

    static func isEdit(_ spec: [String: Any]) -> Bool {
        guard let sub = spec["sub"] as? [String: Any] else { return false }
        return MenuWords.edit.contains(title(spec)) || hasCopyPaste(sub["items"] as? [[String: Any]] ?? [])
    }

    /// the submenu spec for a Win32 menu handle, anywhere in the tree
    static func find(_ items: [[String: Any]], menu: Int) -> [[String: Any]]? {
        for spec in items {
            guard let sub = spec["sub"] as? [String: Any] else { continue }
            let subItems = sub["items"] as? [[String: Any]] ?? []
            if sub["menu"] as? Int == menu { return subItems }
            if let found = find(subItems, menu: menu) { return found }
        }
        return nil
    }

    static func hasCopyPaste(_ items: [[String: Any]]) -> Bool {
        for spec in items {
            let parts = split(spec["text"] as? String ?? "")
            if parts.mods == [.command] && (parts.key == "c" || parts.key == "v") { return true }
            if let sub = spec["sub"] as? [String: Any], hasCopyPaste(sub["items"] as? [[String: Any]] ?? []) {
                return true
            }
        }
        return false
    }
}

/// One Win32 window's menu, shown in the Mac menu bar while that window is active.
final class MenuBar: NSObject, NSMenuDelegate {
    unowned let host: ControlHost
    private var spec: [String: Any]               // guarded by W2S.lock
    private var waiting = false                   // guarded by W2S.lock
    private let refreshed = DispatchSemaphore(value: 0)
    private var items: [NSMenuItem] = []          // main thread: ours in NSApp.mainMenu
    private var hiddenEdit: [NSMenuItem] = []     // winemac's Edit menu while ours has one
    private var helpItem: NSMenuItem?             // the app's Help menu, last (after Window)
    private var previousHelp: NSMenu?
    private var shown = false
    private var tracking = 0

    static var current: MenuBar?                  // main thread

    init(host: ControlHost, spec: [String: Any]) {
        self.host = host
        self.spec = spec
        super.init()
        let nc = NotificationCenter.default
        nc.addObserver(self, selector: #selector(beganTracking), name: NSMenu.didBeginTrackingNotification, object: nil)
        nc.addObserver(self, selector: #selector(endedTracking), name: NSMenu.didEndTrackingNotification, object: nil)
    }

    deinit { NotificationCenter.default.removeObserver(self) }

    @objc func beganTracking() { tracking += 1 }
    @objc func endedTracking() {
        tracking = max(0, tracking - 1)
        if tracking == 0 && shown { rebuild() }
    }

    /// Win32 thread: a new snapshot of the menu.
    func update(_ newSpec: [String: Any]) {
        W2S.lock.lock()
        spec = newSpec
        let wake = waiting
        waiting = false
        W2S.lock.unlock()
        if wake { refreshed.signal() }
        DispatchQueue.main.async {
            // an open menu is refreshed through menuNeedsUpdate; don't pull items from under it
            if self.shown && self.tracking == 0 { self.rebuild() }
        }
    }

    private func currentItems() -> [[String: Any]] {
        W2S.lock.lock()
        defer { W2S.lock.unlock() }
        return spec["items"] as? [[String: Any]] ?? []
    }

    func show(_ on: Bool) {
        if on {
            if let other = MenuBar.current, other !== self { other.show(false) }
            MenuBar.current = self
            shown = true
            rebuild()
        } else {
            shown = false
            removeItems()
            if MenuBar.current === self { MenuBar.current = nil }
        }
    }

    func remove() { show(false) }

    private func removeItems() {
        guard let main = NSApp.mainMenu else { return }
        for item in items where main.items.contains(item) { main.removeItem(item) }
        items = []
        for item in hiddenEdit { item.isHidden = false }
        hiddenEdit = []
        if let help = helpItem, NSApp.helpMenu === help.submenu { NSApp.helpMenu = previousHelp }
        helpItem = nil
    }

    private func rebuild() {
        guard let main = NSApp.mainMenu else { return }
        removeItems()
        let specs = currentItems()
        let taken = MenuBuild.keys(specs)
        // the Help menu is the last one, after Window (HIG); one Edit menu: the app's
        let helpIndex = specs.lastIndex(where: MenuBuild.isHelp)
        let editIndex = specs.firstIndex(where: MenuBuild.isEdit)
        var index = min(1, main.items.count)          // after the application menu
        for (n, spec) in specs.enumerated() {
            guard let sub = spec["sub"] as? [String: Any] else { continue }
            let title = MenuBuild.split(spec["text"] as? String ?? "").title
            let top = NSMenuItem(title: title, action: nil, keyEquivalent: "")
            let submenu = W2SMenu(title: title)
            submenu.win32Menu = sub["menu"] as? Int ?? 0
            submenu.position = sub["pos"] as? Int ?? 0
            submenu.topLevel = true
            submenu.delegate = self
            let subItems = sub["items"] as? [[String: Any]] ?? []
            submenu.textCommands = n == editIndex && !MenuBuild.hasCopyPaste(subItems)
            MenuBuild.fill(submenu, items: subItems, target: self, action: #selector(choose(_:)), delegate: self)
            if submenu.textCommands { MenuBuild.addTextCommands(submenu, taken: taken) }
            top.submenu = submenu
            top.isEnabled = spec["disabled"] as? Bool != true
            if n == helpIndex {
                main.addItem(top)
                helpItem = top
                if NSApp.helpMenu !== submenu { previousHelp = NSApp.helpMenu }
                NSApp.helpMenu = submenu
            } else {
                main.insertItem(top, at: index)
                index += 1
            }
            items.append(top)
        }
        // one Edit menu: winemac's goes while the app has its own, or Copy/Paste elsewhere
        if editIndex != nil || MenuBuild.hasCopyPaste(specs) {
            for item in main.items where !items.contains(item) {
                if let sub = item.submenu, sub.items.contains(where: { $0.keyEquivalent == "v" && $0.keyEquivalentModifierMask == [.command] }) {
                    item.isHidden = true
                    hiddenEdit.append(item)
                }
            }
        }
    }

    @objc func choose(_ item: NSMenuItem) {
        host.emit(["t": "menu", "v": item.tag])
    }

    /// tests/gallery: what is in the menu bar, and the state of an item
    func debugState() -> [String: Any] {
        var out: [String: Any] = ["entry": "menubar", "shown": shown, "titles": items.map { $0.title },
                                  "editHidden": !hiddenEdit.isEmpty,
                                  "bar": (NSApp.mainMenu?.items ?? []).filter { !$0.isHidden }.map { $0.title },
                                  "helpMenu": NSApp.helpMenu.map { h in items.contains { $0.submenu === h } } ?? false,
                                  "helpLast": helpItem != nil && NSApp.mainMenu?.items.last(where: { !$0.isHidden }) === helpItem,
                                  "textCommands": items.contains { ($0.submenu as? W2SMenu)?.textCommands == true }]
        var checked: [Int] = [], keys: [String] = []
        func walk(_ menu: NSMenu?) {
            for item in menu?.items ?? [] {
                if item.state == .on { checked.append(item.tag) }
                if !item.keyEquivalent.isEmpty { keys.append("\(item.tag):\(item.keyEquivalent)") }
                walk(item.submenu)
            }
        }
        for item in items { walk(item.submenu) }
        out["checked"] = checked
        out["keys"] = keys
        return out
    }

    /// tests/gallery: {"t":"menu","v":id} as if chosen, {"t":"open","v":index} as if
    /// the index-th top-level menu opened (the WM_INITMENUPOPUP round trip)
    func debugInject(_ event: [String: Any]) {
        switch event["t"] as? String {
        case "menu": host.emit(["t": "menu", "v": event["v"] as? Int ?? 0])
        case "open":
            let i = event["v"] as? Int ?? 0
            if !shown { rebuild() }             // the test may run while another app is in front
            // later, not inside the debug call: menuNeedsUpdate waits for the Win32
            // thread, which is waiting for this call to return
            DispatchQueue.main.async {
                if i < self.items.count, let sub = self.items[i].submenu { self.menuNeedsUpdate(sub) }
            }
        default: break
        }
    }

    /// Before a submenu opens: the app gets WM_INITMENU(POPUP) and sends it again,
    /// as Windows apps check and enable items there. Wait for it briefly.
    func menuNeedsUpdate(_ menu: NSMenu) {
        guard let menu = menu as? W2SMenu, menu.win32Menu != 0 else { return }
        W2S.lock.lock()
        waiting = true
        W2S.lock.unlock()
        host.emit(["t": "menuOpen", "a": [menu.win32Menu, menu.position, menu.topLevel ? 1 : 0]])
        if refreshed.wait(timeout: .now() + .milliseconds(150)) == .timedOut {
            W2S.lock.lock()
            waiting = false
            W2S.lock.unlock()
        }
        if let specs = MenuBuild.find(currentItems(), menu: menu.win32Menu) {
            MenuBuild.fill(menu, items: specs, target: self, action: #selector(choose(_:)), delegate: self)
            if menu.textCommands { MenuBuild.addTextCommands(menu, taken: MenuBuild.keys(currentItems())) }
        }
    }
}

/// TrackPopupMenuEx (map: menu.popup). NSMenu.popUp tracks in a nested run loop;
/// it runs from a run-loop block, not the GCD main queue, so the other
/// main-queue work (control updates) keeps flowing meanwhile.
enum PopupMenu {
    final class Target: NSObject, NSMenuDelegate {
        var chosen = 0
        var highlighted = 0
        @objc func choose(_ item: NSMenuItem) { chosen = item.tag }
        func menu(_ menu: NSMenu, willHighlight item: NSMenuItem?) { highlighted = item?.tag ?? 0 }
    }

    static func run(_ params: [String: Any], request: Request) {
        guard let ptr = (params["view"] as? NSNumber).flatMap({ UnsafeMutableRawPointer(bitPattern: UInt($0.uint64Value)) }),
              let spec = params["menu"] as? [String: Any] else {
            Requests.finish(request, ["id": 0])
            return
        }
        let view = Unmanaged<NSView>.fromOpaque(ptr).takeUnretainedValue()
        let target = Target()
        let menu = W2SMenu(title: "")
        menu.delegate = target
        MenuBuild.fill(menu, items: spec["items"] as? [[String: Any]] ?? [], target: target,
                       action: #selector(Target.choose(_:)), delegate: target)
        let w = CGFloat((params["widthPx"] as? NSNumber)?.doubleValue ?? 1)
        let h = CGFloat((params["heightPx"] as? NSNumber)?.doubleValue ?? 1)
        let sx = w > 0 ? view.bounds.width / w : 1, sy = h > 0 ? view.bounds.height / h : 1
        var point = NSPoint(x: CGFloat((params["x"] as? NSNumber)?.doubleValue ?? 0) * sx,
                            y: CGFloat((params["y"] as? NSNumber)?.doubleValue ?? 0) * sy)
        if !view.isFlipped { point.y = view.bounds.height - point.y }
        request.inject = { event in
            // tests: {"t":"popupChoose","v":id}
            guard let e = event as? [String: Any] else { return }
            target.chosen = e["v"] as? Int ?? 0
            menu.cancelTracking()
        }
        let picked = menu.popUp(positioning: nil, at: point, in: view)
        Requests.finish(request, ["id": target.chosen != 0 ? target.chosen : (picked ? target.highlighted : 0)])
    }
}
