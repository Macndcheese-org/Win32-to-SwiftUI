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
    return version == W2S.protocolVersion ? 0 : -1
}

@_cdecl("w2s_swift_control_create")
public func w2s_swift_control_create(_ hostView: UInt64, _ window: UInt64, _ postWake: UInt64, _ hwnd: UInt64,
                                     _ entry: UnsafePointer<CChar>?, _ json: UnsafePointer<CChar>?,
                                     _ jsonLen: UInt32) -> UInt64 {
    guard hostView != 0, let entry = entry, let snap = W2S.decode(json, jsonLen),
          let viewPtr = UnsafeMutableRawPointer(bitPattern: UInt(hostView)) else { return 0 }
    let entryID = String(cString: entry)
    guard ControlViews.supports(entryID) else { return 0 }

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
        hosting.frame = container.bounds
        hosting.autoresizingMask = [.width, .height]
        // wine's dialogs are still drawn in its light theme; keep the controls on it
        hosting.appearance = NSAppearance(named: .aqua)
        container.addSubview(hosting)
        host.hosting = hosting
    }
    return handle
}

@_cdecl("w2s_swift_control_update")
public func w2s_swift_control_update(_ handle: UInt64, _ json: UnsafePointer<CChar>?, _ jsonLen: UInt32) {
    guard let host = W2S.control(handle), let snap = W2S.decode(json, jsonLen) else { return }
    DispatchQueue.main.async {
        host.model.absorbImages(snap)
        // taken before the PE side saw the user's latest events: showing it
        // would undo what the user just did; a fresh one follows once they're applied
        if (snap.ack ?? 0) < host.emittedSeq { return }
        if host.model.snap != snap { host.model.snap = snap }
    }
}

@_cdecl("w2s_swift_control_destroy")
public func w2s_swift_control_destroy(_ handle: UInt64) {
    W2S.lock.lock()
    let host = W2S.controls.removeValue(forKey: handle)
    W2S.lock.unlock()
    guard let host = host else { return }
    DispatchQueue.main.async {
        host.hosting?.removeFromSuperview()
        host.hosting = nil
        host.owned = nil
        Unmanaged<NSView>.fromOpaque(host.hostView).release()
    }
}

@_cdecl("w2s_swift_control_focus")
public func w2s_swift_control_focus(_ handle: UInt64, _ focused: UInt32) {
    guard let host = W2S.control(handle) else { return }
    DispatchQueue.main.async {
        if focused != 0 {
            host.model.focusRequest += 1
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
    if let result = request.result {
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
