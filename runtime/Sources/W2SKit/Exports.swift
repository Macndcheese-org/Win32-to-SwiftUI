// The C entry points unixlib.c calls. They run on Win32 threads: nothing here
// touches AppKit directly, UI work goes to the main queue.
import AppKit
import SwiftUI

@_cdecl("w2s_swift_init")
public func w2s_swift_init(_ version: UInt32, _ osMajor: UnsafeMutablePointer<UInt32>?,
                           _ osMinor: UnsafeMutablePointer<UInt32>?) -> Int32 {
    let os = W2S.osVersion
    osMajor?.pointee = UInt32(os.major)
    osMinor?.pointee = UInt32(os.minor)
    guard version == W2S.protocolVersion else { return -1 }
    // wine's system colours follow the macOS appearance from now on
    DispatchQueue.main.async { Look.start() }
    // debugging: W2S_CAPTURE_DIR=<dir> draws this process's windows there every few
    // seconds (offscreen: works with the screen locked)
    if let dir = ProcessInfo.processInfo.environment["W2S_CAPTURE_DIR"], !dir.isEmpty {
        DispatchQueue.main.async { Capture.start(dir) }
    }
    return 0
}

enum Capture {
    private static var timer: Timer?

    /// tests: every tab control that is a property sheet's goes to its next page
    static func nextPages() {
        W2S.lock.lock()
        let hosts = Array(W2S.controls.values)
        W2S.lock.unlock()
        for host in hosts where host.entry == "tab" {
            let count = host.model.snap.items?.count ?? 0
            guard count > 1 else { continue }
            host.model.emit(["t": "select", "v": ((host.model.snap.selection ?? 0) + 1) % count])
        }
    }

    static func start(_ dir: String) {
        guard timer == nil else { return }
        let exe = (CommandLine.arguments.first as NSString?)?.lastPathComponent ?? "wine"
        // W2S_TOUR=1: after each capture, property sheets move to their next page
        let tour = ProcessInfo.processInfo.environment["W2S_TOUR"] == "1"
        var shot = 0
        timer = Timer.scheduledTimer(withTimeInterval: 4, repeats: true) { _ in
            shot += 1
            defer { if tour { Capture.nextPages() } }
            for (i, window) in NSApp.windows.enumerated() where window.isVisible && window.frame.width > 80 {
                guard let view = window.contentView?.superview ?? window.contentView,
                      let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { continue }
                view.cacheDisplay(in: view.bounds, to: rep)
                let title = window.title.isEmpty ? "untitled" : window.title
                let name = (tour ? "\(exe)-\(i)-\(title)-\(shot)" : "\(exe)-\(i)-\(title)").replacingOccurrences(of: "/", with: "_")
                try? rep.representation(using: .png, properties: [:])?
                    .write(to: URL(fileURLWithPath: dir).appendingPathComponent(name + ".png"))
                // and the view tree, for what a picture can't tell
                var lines: [String] = []
                func dump(_ v: NSView, _ depth: Int) {
                    guard depth < 7 else { return }
                    let entry = W2S.entry(forHostView: v) ?? ""
                    let look = v.effectiveAppearance.bestMatch(from: [.aqua, .darkAqua])?.rawValue ?? "?"
                    var backdrop = W2S.backdrop(forHostView: v).map { String(format: " backdrop=%06x", $0) } ?? ""
                    // what wine really painted under a control: its surface's pixel there
                    if !entry.isEmpty, let wine = v.superview, let layer = wine.layer,
                       let contents = layer.contents, CFGetTypeID(contents as CFTypeRef) == CGImage.typeID {
                        let image = contents as! CGImage
                        let scale = CGFloat(image.width) / max(1, wine.bounds.width)
                        let x = Int((v.frame.minX + 3) * scale), y = Int((v.frame.midY) * scale)
                        if x >= 0, y >= 0, x < image.width, y < image.height,
                           let data = image.dataProvider?.data, let bytes = CFDataGetBytePtr(data) {
                            let o = y * image.bytesPerRow + x * (image.bitsPerPixel / 8)
                            backdrop += String(format: " painted=%02x%02x%02x", bytes[o + 2], bytes[o + 1], bytes[o])
                        }
                    }
                    lines.append(String(repeating: "  ", count: depth) + "\(type(of: v)) \(entry) \(v.frame) hidden=\(v.isHidden) \(look)\(backdrop)")
                    for sub in v.subviews { dump(sub, depth + 1) }
                }
                dump(view, 0)
                try? lines.joined(separator: "\n").write(to: URL(fileURLWithPath: dir).appendingPathComponent(name + ".txt"),
                                                          atomically: true, encoding: .utf8)
            }
        }
    }
}

@_cdecl("w2s_swift_control_create")
public func w2s_swift_control_create(_ hostView: UInt64, _ window: UInt64, _ postWake: UInt64, _ hwnd: UInt64,
                                     _ entry: UnsafePointer<CChar>?, _ json: UnsafePointer<CChar>?,
                                     _ jsonLen: UInt32) -> UInt64 {
    guard hostView != 0, let entry = entry,
          let viewPtr = UnsafeMutableRawPointer(bitPattern: UInt(hostView)) else { return 0 }
    let entryID = String(cString: entry)
    if entryID == "menubar" { return createMenuBar(viewPtr, postWake, json, jsonLen) }
    guard let snap = W2S.decode(json, jsonLen), ControlViews.supports(entryID) else { return 0 }

    // keep the host view alive until destroy, whatever winemac does meanwhile
    _ = Unmanaged<NSView>.fromOpaque(viewPtr).retain()
    let wake = postWake == 0 ? nil : unsafeBitCast(UInt(postWake), to: PostWake.self)
    let handle = W2S.newID()
    let host = ControlHost(handle: handle, entry: entryID, hostView: viewPtr, postWake: wake)
    host.model = ControlModel(snap: snap) { [weak host] event in host?.emit(event) }
    host.model.publish = { [weak host] state in host?.publish(state) }
    host.model.absorbImages(snap)

    W2S.lock.lock()
    W2S.controls[handle] = host
    W2S.lock.unlock()

    DispatchQueue.main.async {
        guard W2S.control(handle) != nil else { return }
        let container = Unmanaged<NSView>.fromOpaque(viewPtr).takeUnretainedValue()
        let hosting = PassThroughHostingView(rootView: ControlViews.root(for: host.model, entry: entryID))
        hosting.native = PassThroughHostingView<AnyView>.region(for: entryID, model: host.model)
        hosting.frame = container.bounds
        hosting.autoresizingMask = [.width, .height]
        // light or dark as what wine draws behind it (wine's colours follow macOS: Look)
        Look.apply(host.model.snap.backdrop, to: hosting)
        container.addSubview(hosting)
        host.hosting = hosting
        if entryID == "button.groupbox" { GroupBoxTitle.apply(host) }
        // what a control puts in its window's frame lives exactly as long as the control
        if entryID == "tab" || entryID == "listbox.single" {
            // a property sheet's tab control or a pane list: the window's settings form
            let form = SettingsFormController(host: host)
            host.owned = form
            form.update()
        } else if entryID == "toolbar" {
            let toolbar = WindowToolbar(host: host)
            host.owned = toolbar
            toolbar.update()
        } else if entryID == "scrollbar" {
            let bars = ScrollBarsController(host: host)
            host.owned = bars
            bars.update()
        } else if entryID == "treeview" {
            let sidebar = FrameSidebar(host: host)
            host.owned = sidebar
            sidebar.update()
        }
    }
    return handle
}

/// A window's menu (map: menu.bar): no view of its own, it lives in the Mac menu bar.
private func createMenuBar(_ viewPtr: UnsafeMutableRawPointer, _ postWake: UInt64,
                           _ json: UnsafePointer<CChar>?, _ jsonLen: UInt32) -> UInt64 {
    guard let spec = W2S.object(json, jsonLen) else { return 0 }
    _ = Unmanaged<NSView>.fromOpaque(viewPtr).retain()
    let wake = postWake == 0 ? nil : unsafeBitCast(UInt(postWake), to: PostWake.self)
    let handle = W2S.newID()
    let host = ControlHost(handle: handle, entry: "menubar", hostView: viewPtr, postWake: wake)
    host.owned = MenuBar(host: host, spec: spec)
    W2S.lock.lock()
    W2S.controls[handle] = host
    W2S.lock.unlock()
    return handle
}

@_cdecl("w2s_swift_control_update")
public func w2s_swift_control_update(_ handle: UInt64, _ json: UnsafePointer<CChar>?, _ jsonLen: UInt32) {
    if let host = W2S.control(handle), let bar = host.owned as? MenuBar {
        if let spec = W2S.object(json, jsonLen) { bar.update(spec) }
        return
    }
    guard let host = W2S.control(handle), let snap = W2S.decode(json, jsonLen) else { return }
    DispatchQueue.main.async {
        host.model.absorbImages(snap)
        Look.apply(snap.backdrop, to: host.hosting)
        // taken before the PE side saw the user's latest events: showing it
        // would undo what the user just did; a fresh one follows once they're applied
        if (snap.ack ?? 0) < host.emittedSeq {
            (host.owned as? WindowChrome)?.update()
            return
        }
        if host.model.snap != snap {
            let titleMoved = snap.titleAbove != host.model.snap.titleAbove || snap.text != host.model.snap.text
            host.model.snap = snap
            if titleMoved && host.entry == "button.groupbox" { GroupBoxTitle.apply(host) }
        }
        // a window sidebar attaches once the host view sits in a window
        (host.owned as? WindowChrome)?.update()
    }
}

@_cdecl("w2s_swift_control_destroy")
public func w2s_swift_control_destroy(_ handle: UInt64) {
    W2S.lock.lock()
    let host = W2S.controls.removeValue(forKey: handle)
    W2S.lock.unlock()
    guard let host = host else { return }
    DispatchQueue.main.async {
        (host.owned as? WindowChrome)?.detach()  // before the hosting view leaves
        host.hosting?.removeFromSuperview()
        host.hosting = nil
        (host.owned as? MenuBar)?.remove()     // its items leave the menu bar with it
        host.owned = nil
        Unmanaged<NSView>.fromOpaque(host.hostView).release()
    }
}

@_cdecl("w2s_swift_control_focus")
public func w2s_swift_control_focus(_ handle: UInt64, _ focused: UInt32) {
    guard let host = W2S.control(handle) else { return }
    if let bar = host.owned as? MenuBar {
        // the window became active (or not): its menu goes in (or out of) the menu bar
        DispatchQueue.main.async { bar.show(focused != 0) }
        return
    }
    DispatchQueue.main.async {
        host.model.hasWin32Focus = focused != 0
        if focused != 0 {
            host.model.focusRequest += 1
        } else if host.model.shownInForm {
            // in a settings form: back to wine if the form's copy of this control still has the keyboard
            if FormFocus.owner == ObjectIdentifier(host.model), let window = host.hosting?.window {
                FormFocus.owner = nil
                let wine = window.perform(NSSelectorFromString("wineContentView"))?.takeUnretainedValue() as? NSView
                window.makeFirstResponder(wine ?? window.contentView)
            }
        } else if let window = host.hosting?.window, let responder = window.firstResponder as? NSView,
                  let hosting = host.hosting, responder.isDescendant(of: hosting) {
            // wine moved focus elsewhere: give the keyboard back to wine
            window.makeFirstResponder(window.contentView)
        }
    }
}

@_cdecl("w2s_swift_pop_events")
public func w2s_swift_pop_events(_ handle: UInt64, _ buffer: UnsafeMutablePointer<CChar>?, _ size: UInt32) -> UInt32 {
    guard let host = W2S.control(handle) else { return 0 }
    W2S.lock.lock()
    defer { W2S.lock.unlock() }
    if host.pending.isEmpty { return 0 }
    let text = W2S.json(host.pending)
    let needed = W2S.copyOut(text, buffer, size)
    if needed <= size { host.pending.removeAll() }
    return needed
}

@_cdecl("w2s_swift_control_state")
public func w2s_swift_control_state(_ handle: UInt64, _ version: UnsafeMutablePointer<UInt64>?,
                                    _ buffer: UnsafeMutablePointer<CChar>?, _ size: UInt32) -> UInt32 {
    guard let host = W2S.control(handle), let version = version else { return 0 }
    W2S.lock.lock()
    defer { W2S.lock.unlock() }
    guard let text = host.published, host.publishedVersion != version.pointee else { return 0 }
    let needed = W2S.copyOut(text, buffer, size)
    if needed <= size { version.pointee = host.publishedVersion }
    return needed
}

@_cdecl("w2s_swift_request_start")
public func w2s_swift_request_start(_ kind: UnsafePointer<CChar>?, _ window: UInt64,
                                    _ json: UnsafePointer<CChar>?, _ jsonLen: UInt32) -> UInt64 {
    guard let kind = kind, let json = json else { return 0 }
    let kindString = String(cString: kind)
    let data = Data(bytes: json, count: Int(jsonLen))
    guard let params = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] else { return 0 }
    let id = W2S.newID()
    let request = Request(id: id)
    W2S.lock.lock()
    W2S.requests[id] = request
    W2S.lock.unlock()
    let windowPtr = UnsafeMutableRawPointer(bitPattern: UInt(window))
    if kindString == "popup" {
        // NSMenu.popUp tracks in a nested run loop: from a GCD main-queue block it
        // would hold the queue, and every other main-queue update, until it closes
        CFRunLoopPerformBlock(CFRunLoopGetMain(), CFRunLoopMode.commonModes.rawValue) {
            PopupMenu.run(params, request: request)
        }
        CFRunLoopWakeUp(CFRunLoopGetMain())
        return id
    }
    DispatchQueue.main.async {
        let owner = windowPtr.map { Unmanaged<NSWindow>.fromOpaque($0).takeUnretainedValue() }
        Requests.start(kind: kindString, params: params, owner: owner, request: request)
    }
    return id
}

@_cdecl("w2s_swift_request_poll")
public func w2s_swift_request_poll(_ id: UInt64, _ buffer: UnsafeMutablePointer<CChar>?, _ size: UInt32,
                                   _ done: UnsafeMutablePointer<UInt32>?) -> UInt32 {
    W2S.lock.lock()
    defer { W2S.lock.unlock() }
    guard let request = W2S.requests[id] else {
        done?.pointee = 1
        return 0
    }
    // the events raised before the panel closed first (a box ticked just before Save)
    if let result = request.result, request.events.isEmpty {
        done?.pointee = 1
        let needed = W2S.copyOut(result, buffer, size)
        if needed <= size { W2S.requests.removeValue(forKey: id) }
        return needed
    }
    // still open: hand over what the panel raised meanwhile
    done?.pointee = 0
    if request.events.isEmpty { return 0 }
    let needed = W2S.copyOut(W2S.json(request.events), buffer, size)
    if needed <= size { request.events.removeAll() }
    return needed
}

@_cdecl("w2s_swift_request_update")
public func w2s_swift_request_update(_ id: UInt64, _ json: UnsafePointer<CChar>?, _ jsonLen: UInt32) {
    guard let json = json,
          let params = (try? JSONSerialization.jsonObject(with: Data(bytes: json, count: Int(jsonLen)))) as? [String: Any]
    else { return }
    W2S.lock.lock()
    let request = W2S.requests[id]
    W2S.lock.unlock()
    guard let request = request else { return }
    DispatchQueue.main.async { request.update?(params) }
}

@_cdecl("w2s_swift_debug")
public func w2s_swift_debug(_ handle: UInt64, _ json: UnsafePointer<CChar>?, _ jsonLen: UInt32,
                            _ buffer: UnsafeMutablePointer<CChar>?, _ size: UInt32) -> UInt32 {
    guard let json = json,
          let op = (try? JSONSerialization.jsonObject(with: Data(bytes: json, count: Int(jsonLen)))) as? [String: Any]
    else { return 0 }
    var answer = "{}"
    let done = DispatchSemaphore(value: 0)
    DispatchQueue.main.async {
        answer = Debug.handle(handle: handle, op: op)
        done.signal()
    }
    if done.wait(timeout: .now() + 5) == .timedOut { answer = "{\"error\":\"main thread busy\"}" }
    return W2S.copyOut(answer, buffer, size)
}
