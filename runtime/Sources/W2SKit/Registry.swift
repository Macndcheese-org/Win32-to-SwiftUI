// The live translated controls and pending requests, shared between Win32
// threads (the unix calls) and the main thread (AppKit/SwiftUI).
import AppKit
import SwiftUI

typealias PostWake = @convention(c) (UnsafeMutableRawPointer?, UInt64) -> Void

/// What the native view knows about its Win32 control; mirrors the PE side's
/// JSON snapshot (runtime/pe/controls.c). Encodable for the tests' queries.
struct Snapshot: Codable, Equatable {
    struct Column: Codable, Equatable { var title: String; var width: Int?; var index: Int?; var align: String? }
    struct Pane: Codable, Equatable { var text: String; var right: Int; var ownerDraw: Bool?; var tip: String? }
    /// A tree view item; `id` is its HTREEITEM. Children are listed only under open nodes.
    struct TreeNode: Codable, Equatable, Identifiable {
        var id: Int
        var text: String
        var kids: Bool?
        var open: Bool?
        var children: [TreeNode]?
    }

    var entry: String?
    var ack: UInt64?            // the last native event the PE side has taken
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
    var note: String?           // command link
    var noSplit: Bool?          // split button: BCSS_NOSPLIT
    // static / edit
    var align: String?
    var wrap: Bool?
    var noPrefix: Bool?
    var centerVertically: Bool?
    var vertical: Bool?
    var imageWidth: Int?
    var imageHeight: Int?
    var imageBGRA: String?
    var fill: String?           // static.frame: "none" (an outline) or the rectangle's colour
    var cue: String?
    var readonly: Bool?
    var limit: Int?
    // multi-line edit (native offsets: every line break is one "\n")
    var sel: [Int]?
    var wantReturn: Bool?
    var caretGen: Int?
    var scrollGen: Int?
    var scrollLine: Int?
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
    var buddy: Bool?            // up-down
    // date and time: [year, month, day, hour, minute, second], local, Gregorian
    var date: [Int]?
    var dateMin: [Int]?
    var dateMax: [Int]?
    var dateValid: Bool?
    var showNone: Bool?
    var timeOnly: Bool?
    var upDown: Bool?
    // tree view
    var nodes: [TreeNode]?
    var sidebar: Bool?
    // status bar
    var panes: [Pane]?
    var simple: Bool?
}

/// Observed by the SwiftUI view of one control.
final class ControlModel: ObservableObject {
    @Published var snap: Snapshot
    @Published var focusRequest = 0
    let emit: ([String: Any]) -> Void
    /// What the Win32 control answers its `answers` queries from (main thread).
    var publish: ([String: Any]) -> Void = { _ in }
    private var clickQueued = false

    init(snap: Snapshot, emit: @escaping ([String: Any]) -> Void) {
        self.snap = snap
        self.emit = emit
    }

    /// One click per user action, however many times SwiftUI writes the binding
    /// (a mixed Toggle(sources:) sets each of its sources).
    func clickOnce() {
        guard !clickQueued else { return }
        clickQueued = true
        DispatchQueue.main.async {
            self.clickQueued = false
            self.emit(["t": "click"])
        }
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
    private var seq: UInt64 = 0             // guarded by W2S.lock
    var published: String?                 // guarded by W2S.lock: for the entry's answers
    var publishedVersion: UInt64 = 0        // guarded by W2S.lock
    var owned: AnyObject?                   // main thread: what the view keeps alive (a menu bar)

    init(handle: UInt64, entry: String, hostView: UnsafeMutableRawPointer, postWake: PostWake?) {
        self.handle = handle
        self.entry = entry
        self.hostView = hostView
        self.postWake = postWake
    }

    /// The sequence number of the last event sent (snapshots below it are stale).
    var emittedSeq: UInt64 {
        W2S.lock.lock()
        defer { W2S.lock.unlock() }
        return seq
    }

    /// Queue a native event for the Win32 side and wake the control's thread.
    func emit(_ event: [String: Any]) {
        var event = event
        W2S.lock.lock()
        seq += 1
        event["n"] = seq
        pending.append(event)
        W2S.lock.unlock()
        postWake?(hostView, 0)
    }

    /// Main thread: what the Win32 control answers its `answers` queries from.
    func publish(_ state: [String: Any]) {
        let text = W2S.json(state)
        W2S.lock.lock()
        if text != published {
            published = text
            publishedVersion += 1
        }
        W2S.lock.unlock()
    }

    /// Main thread: points per Win32 pixel, from the host view's height.
    var scale: CGFloat {
        guard let view = hosting, let px = model.snap.heightPx, px > 0, view.bounds.height > 0 else { return 1 }
        return view.bounds.height / CGFloat(px)
    }
}

final class Request {
    let id: UInt64
    var result: String?                     // guarded by W2S.lock
    var events: [[String: Any]] = []        // guarded by W2S.lock: raised while open
    var inject: ((Any) -> Void)?            // main thread: tests drive the open panel/alert
    var update: (([String: Any]) -> Void)?  // main thread: the Win32 side changes the open panel
    var query: (() -> [String: Any])?       // main thread: tests read the open panel
    init(id: UInt64) { self.id = id }

    /// Tell the Win32 thread waiting on this panel that something happened.
    func emit(_ event: [String: Any]) {
        W2S.lock.lock()
        events.append(event)
        W2S.lock.unlock()
    }
}

enum W2S {
    static let protocolVersion: UInt32 = 2

    /// The running macOS; an app linked against an SDK older than 26 is told
    /// 16 for 26 (the version compatibility shim), so 16 means 26.
    static let osVersion: (major: Int, minor: Int) = {
        let v = ProcessInfo.processInfo.operatingSystemVersion
        return (v.majorVersion == 16 ? 26 : v.majorVersion, v.minorVersion)
    }()

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
