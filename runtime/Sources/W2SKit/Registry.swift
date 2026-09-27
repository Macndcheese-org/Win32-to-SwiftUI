// The live translated controls and pending requests, shared between Win32
// threads (the unix calls) and the main thread (AppKit/SwiftUI).
import AppKit
import SwiftUI

typealias PostWake = @convention(c) (UnsafeMutableRawPointer?, UInt64) -> Void

/// What the native view knows about its Win32 control; mirrors the PE side's
/// JSON snapshot (runtime/pe/controls.c).
struct Snapshot: Decodable, Equatable {
    struct Column: Decodable, Equatable { var title: String; var width: Int?; var index: Int?; var align: String? }

    var entry: String?
    var text: String?
    var enabled: Bool?
    var fontPx: Double?
    var widthPx: Double?
    var heightPx: Double?
    // buttons
    var checked: Int?
    var isDefault: Bool?
    var leftText: Bool?
    var isCancel: Bool?
    // static / edit
    var align: String?
    var wrap: Bool?
    var noPrefix: Bool?
    var centerVertically: Bool?
    var vertical: Bool?
    var imageWidth: Int?
    var imageHeight: Int?
    var imageBGRA: String?
    var cue: String?
    var readonly: Bool?
    var limit: Int?
    // lists
    var items: [String]?
    var selection: Int?
    var selections: [Int]?
    var columns: [Column]?
    var rows: [[String]]?
    var single: Bool?
    var noHeader: Bool?
    var sortHeader: Bool?
    // values
    var value: Double?
    var min: Double?
    var max: Double?
    var marquee: Bool?
    var state: Int?
    var ticks: Int?
}

/// Observed by the SwiftUI view of one control.
final class ControlModel: ObservableObject {
    @Published var snap: Snapshot
    @Published var focusRequest = 0
    let emit: ([String: Any]) -> Void

    init(snap: Snapshot, emit: @escaping ([String: Any]) -> Void) {
        self.snap = snap
        self.emit = emit
    }
}

/// NSHostingView that lets clicks through where it draws nothing, so the
/// Win32 content under a translated control (a tab page, a group's contents)
/// stays clickable.
final class PassThroughHostingView<Content: View>: NSHostingView<Content> {
    override func hitTest(_ point: NSPoint) -> NSView? {
        let hit = super.hitTest(point)
        return hit === self ? nil : hit
    }
}

final class ControlHost {
    let handle: UInt64
    let entry: String
    let hostView: UnsafeMutableRawPointer   // retained NSView (WineW2SHostView)
    let postWake: PostWake?
    var model: ControlModel!
    var hosting: NSView?
    var pending: [[String: Any]] = []       // guarded by W2S.lock

    init(handle: UInt64, entry: String, hostView: UnsafeMutableRawPointer, postWake: PostWake?) {
        self.handle = handle
        self.entry = entry
        self.hostView = hostView
        self.postWake = postWake
    }

    /// Queue a native event for the Win32 side and wake the control's thread.
    func emit(_ event: [String: Any]) {
        W2S.lock.lock()
        pending.append(event)
        W2S.lock.unlock()
        postWake?(hostView, 0)
    }
}

final class Request {
    let id: UInt64
    var result: String?                     // guarded by W2S.lock
    var inject: ((Any) -> Void)?            // main thread: tests drive the open panel/alert
    init(id: UInt64) { self.id = id }
}

enum W2S {
    static let lock = NSLock()
    static var controls: [UInt64: ControlHost] = [:]
    static var requests: [UInt64: Request] = [:]
    static var nextID: UInt64 = 1

    static func newID() -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        let id = nextID
        nextID += 1
        return id
    }

    static func control(_ handle: UInt64) -> ControlHost? {
        lock.lock()
        defer { lock.unlock() }
        return controls[handle]
    }

    static func decode(_ json: UnsafePointer<CChar>?, _ len: UInt32) -> Snapshot? {
        guard let json = json else { return nil }
        let data = Data(bytes: json, count: Int(len))
        return try? JSONDecoder().decode(Snapshot.self, from: data)
    }

    /// Copies a UTF-8 string into a caller buffer; returns the size needed
    /// (with the terminator), so the caller can retry with a bigger buffer.
    static func copyOut(_ string: String, _ buffer: UnsafeMutablePointer<CChar>?, _ size: UInt32) -> UInt32 {
        let bytes = Array(string.utf8)
        let needed = UInt32(bytes.count + 1)
        if let buffer = buffer, needed <= size {
            bytes.withUnsafeBufferPointer { src in
                buffer.withMemoryRebound(to: UInt8.self, capacity: Int(size)) { dst in
                    dst.update(from: src.baseAddress!, count: bytes.count)
                }
            }
            buffer[bytes.count] = 0
        }
        return needed
    }

    static func json(_ object: Any) -> String {
        guard JSONSerialization.isValidJSONObject(object),
              let data = try? JSONSerialization.data(withJSONObject: object, options: []) else { return "{}" }
        return String(decoding: data, as: UTF8.self)
    }
}
