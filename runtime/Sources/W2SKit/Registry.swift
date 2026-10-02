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
        var img: Int?           // its image list index (ControlModel.images)
        var children: [TreeNode]?
    }

    var entry: String?
    var ack: UInt64?            // the last native event the PE side has taken
    var text: String?
    var enabled: Bool?
    var fontPx: Double?
    var widthPx: Double?
    var heightPx: Double?
    var help: String?           // the text of the tooltip tool this control is
    var display: String?        // shown instead of text (a native wizard's Go Back / Continue)
    var backdrop: Int?          // COLORREF the app paints behind the control (Look.appearance)
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
    var imageSymbol: String?    // a stock icon: the macOS image's spec (Icons.swift) instead of imageBGRA
    var titleAbove: Bool?       // group box: room for its title above the box
    // single-line edit: EM_SHOWBALLOONTIP while it shows (map: edit.balloon)
    struct Balloon: Codable, Equatable {
        var title: String
        var text: String
        var icon: String        // none, info, warning, error
        var serial: Int         // each EM_SHOWBALLOONTIP shows it again
    }
    var balloon: Balloon?
    var document: Bool?         // multi-line edit: a document window's text, drawn without a border
    var inFrame: Bool?          // toolbar: its buttons are the window frame's toolbar (WindowToolbar)
    var bars: [ScrollBarState]? // scrollbar: a window's own, or the ScrollBar control's (ScrollBars.swift)
    // toolbar
    struct ToolbarButton: Codable, Equatable {
        var i: Int
        var rect: [Int]         // Win32: left, top, width, height in the toolbar
        var hidden: Bool?
        var sep: Bool?
        var id: Int?
        var enabled: Bool?
        var check: Bool?
        var group: Bool?
        var checked: Bool?
        var dropdown: Int?      // 1: an arrow beside the button, 2: the whole button drops down
        var showText: Bool?
        var text: String?
        var tip: String?
        var sym: String?        // a standard image's macOS image (Icons.swift)
        var img: Int?           // else its image list index (ControlModel.images)
    }
    var buttons: [ToolbarButton]?
    var children: [[Int]]?      // toolbar, rebar: the app's own windows in it (client left, top, right, bottom)
    var editable: Bool?         // comboboxex: CBS_DROPDOWN
    var list: Bool?
    var mixed: Bool?
    var bottom: Bool?           // tab control: TCS_BOTTOM
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
    var report: Bool?           // list view in details mode
    var checks: [Bool]?         // LVS_EX_CHECKBOXES
    var icons: [Int]?           // icon views: each row's image index
    var images: [String: String]?   // icons not sent before (index: BGRA base64); see ControlModel.images
    var symbols: [String: String]?  // stock icons not sent before (index: macOS image spec)
    var imageGen: Int?
    var imageSize: [Int]?
    var small: Bool?
    // values
    var value: Double?
    var min: Double?
    var max: Double?
    var marquee: Bool?
    var state: Int?
    var ticks: Int?
    var tickSide: String?       // a vertical trackbar's ticks: "leading" (TBS_LEFT) or "trailing"
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
    var sidebarPane: Double?    // tree: the window's sidebar, up to this pane (client px; FrameSidebar)
    var mode: String?           // tab: "strip", "form" (a property sheet as a settings form) or "wizard"
    var sheetPx: [Double]?      // tab, form: the sheet's client size
    var tabRect: [Double]?      // tab, form: the tab control in the sheet
    var page: [FormItem]?       // tab, form: the current page's controls
    var sheetButtons: [FormItem]?   // tab, form: OK, Cancel, Apply...
    var sidebarPx: Double?      // wizard: the steps' width
    // wizard: the active page's header, drawn above the page from headerPx (x, page top)
    var heading: String?
    var subheading: String?
    var headerPx: [Double]?
    // status bar
    var panes: [Pane]?
    var simple: Bool?
}

/// Observed by the SwiftUI view of one control.
final class ControlModel: ObservableObject {
    @Published var snap: Snapshot
    @Published var focusRequest = 0
    /// shown in a settings form: the form's copy of its view takes the keyboard, not its own
    @Published var shownInForm = false
    /// the Win32 control has the focus (a view made later takes the keyboard too)
    var hasWin32Focus = false
    let emit: ([String: Any]) -> Void
    /// What the Win32 control answers its `answers` queries from (main thread).
    var publish: ([String: Any]) -> Void = { _ in }
    private var clickQueued = false

    init(snap: Snapshot, emit: @escaping ([String: Any]) -> Void) {
        self.snap = snap
        self.emit = emit
    }

    /// A tree in the window's sidebar: by how much each row on screen falls short
    /// of showing its name whole (main thread; TreeRows, FrameSidebar).
    var rowShortfall: [Int: CGFloat] = [:]

    /// Icons of an icon view, sent once each (main thread).
    var images: [Int: NSImage] = [:]
    /// Which of them are stock icons shown as macOS images (for the tests).
    var imageSymbols: [Int: String] = [:]
    private var imageGen: Int?

    /// Keeps the icons a snapshot brings, even one dropped as stale: they are
    /// never sent again.
    func absorbImages(_ snap: Snapshot) {
        if snap.imageGen != imageGen {
            imageGen = snap.imageGen
            images = [:]
            imageSymbols = [:]
        }
        guard let size = snap.imageSize, size.count == 2 else { return }
        for (key, base64) in snap.images ?? [:] {
            if let index = Int(key), let image = bgraImage(base64, width: size[0], height: size[1]) {
                images[index] = image
            }
        }
        for (key, spec) in snap.symbols ?? [:] {
            guard let index = Int(key) else { continue }
            imageSymbols[index] = spec
            if let image = Icons.image(spec, size: NSSize(width: size[0], height: size[1])) { images[index] = image }
        }
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

/// 32bpp premultiplied BGRA (GDI's layout, as the PE side sends it) -> NSImage.
func bgraImage(_ base64: String, width w: Int, height h: Int) -> NSImage? {
    guard w > 0, h > 0, let data = Data(base64Encoded: base64), data.count == w * h * 4,
          let provider = CGDataProvider(data: data as CFData),
          let cg = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                           space: CGColorSpaceCreateDeviceRGB(),
                           bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue
                                                    | CGBitmapInfo.byteOrder32Little.rawValue),
                           provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    else { return nil }
    return NSImage(cgImage: cg, size: NSSize(width: w, height: h))
}

/// NSHostingView that lets clicks through where the Win32 content under a
/// translated control has to stay clickable (a tab page, a group's contents).
/// SwiftUI draws its controls in the hosting view itself (no NSView each), so
/// hitTest can't tell a button from empty space: `native` says where the view
/// is a control, in its own coordinates. nil: everywhere.
final class PassThroughHostingView<Content: View>: NSHostingView<Content> {
    var native: ((NSPoint, NSSize) -> Bool)?

    override func hitTest(_ point: NSPoint) -> NSView? {
        guard let hit = super.hitTest(point) else { return nil }
        guard let native = native else { return hit }
        // AppKit's views inside too: a tab control's NSTabView covers its page,
        // where the app's controls lie (Notepad++'s Find dialog has its buttons there)
        return native(convert(point, from: superview), bounds.size) ? hit : nil
    }

    /// Where an entry's view takes clicks: decoration lets them all through, a
    /// tab control only takes them on its strip, not on the page (a sidebar
    /// lives in the window, outside wine's content, and takes none here).
    static func region(for entry: String, model: ControlModel) -> ((NSPoint, NSSize) -> Bool)? {
        switch entry {
        case "static.text", "static.frame", "static.separator", "static.image", "button.groupbox", "progress", "rebar":
            return { _, _ in false }
        case "toolbar":
            // the buttons; between them, the app's own windows (a font list) take the clicks
            return { [weak model] point, size in
                guard let snap = model?.snap else { return false }
                let scale = size.width / CGFloat(max(1, snap.widthPx ?? Double(size.width)))
                return (snap.buttons ?? []).contains { b in
                    !(b.sep ?? false) && !(b.hidden ?? false) && b.rect.count == 4 &&
                        NSRect(x: CGFloat(b.rect[0]) * scale, y: CGFloat(b.rect[1]) * scale,
                               width: CGFloat(b.rect[2]) * scale, height: CGFloat(b.rect[3]) * scale).contains(point)
                }
            }
        case "scrollbar":
            // the scrollers only: the window's client area under them is wine's
            return { [weak model] point, size in
                guard let snap = model?.snap else { return false }
                let s = ScrollGeometry.scale(snap, height: size.height)
                return (snap.bars ?? []).contains { ScrollGeometry.frame($0, snap, scale: s).contains(point) }
            }
        case "tab":
            return { [weak model] point, size in
                guard let snap = model?.snap else { return false }
                switch snap.mode {
                case "wizard":
                    return false        // the steps and header can't be clicked
                case "form":
                    // the whole sheet is the settings form, but for the app's own windows in its slots
                    guard let model = model else { return true }
                    return !(FormPlaces.holes[W2S.handle(of: model)] ?? []).contains { $0.contains(point) }
                default:
                    // the tabs, straddling the box's top (or bottom) edge (flipped)
                    return (snap.bottom ?? false) ? point.y > size.height - 34 : point.y < 34
                }
            }
        default:
            return nil
        }
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

    /// Main thread: points per Win32 pixel, from the host view's height. While
    /// the control has no size (a window's sidebar hidden squeezes its tree to
    /// nothing), the last one measured: 1 there would halve every Retina width.
    var scale: CGFloat {
        guard let view = hosting, let px = model.snap.heightPx, px > 0, view.bounds.height > 0 else { return lastScale ?? 1 }
        lastScale = view.bounds.height / CGFloat(px)
        return lastScale!
    }
    private var lastScale: CGFloat?
}

final class Request {
    let id: UInt64
    var result: String?                     // guarded by W2S.lock
    var events: [[String: Any]] = []        // guarded by W2S.lock: raised while open
    var inject: ((Any) -> Void)?            // main thread: tests drive the open panel/alert
    var update: (([String: Any]) -> Void)?  // main thread: the Win32 side changes the open panel
    var query: (() -> [String: Any])?       // main thread: tests read the open panel
    weak var window: NSWindow?              // main thread: the panel's own window (tests capture it)
    init(id: UInt64) { self.id = id }

    /// Tell the Win32 thread waiting on this panel that something happened.
    func emit(_ event: [String: Any]) {
        W2S.lock.lock()
        events.append(event)
        W2S.lock.unlock()
    }
}

enum W2S {
    static let protocolVersion: UInt32 = 4     // w2s_protocol.h

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

    /// Wake the Win32 side through any live control (Look: the system colours changed).
    static func wakeAny(cookie: UInt64) {
        lock.lock()
        let host = controls.values.filter { $0.postWake != nil }.min { $0.handle < $1.handle }
        lock.unlock()
        if let host = host { host.postWake?(host.hostView, cookie) }
    }

    /// The map entry of the control a host view holds (debug dumps).
    static func entry(forHostView view: NSView) -> String? {
        let ptr = Unmanaged.passUnretained(view).toOpaque()
        lock.lock()
        defer { lock.unlock() }
        return controls.values.first { $0.hostView == ptr }?.entry
    }

    static func backdrop(forHostView view: NSView) -> Int? {
        let ptr = Unmanaged.passUnretained(view).toOpaque()
        lock.lock()
        let host = controls.values.first { $0.hostView == ptr }
        lock.unlock()
        return host?.model?.snap.backdrop
    }

    static func control(_ handle: UInt64) -> ControlHost? {
        lock.lock()
        defer { lock.unlock() }
        return controls[handle]
    }

    /// The handle of the control a model belongs to (0: none).
    static func handle(of model: ControlModel) -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        return controls.first { $0.value.model === model }?.key ?? 0
    }

    static func object(_ json: UnsafePointer<CChar>?, _ len: UInt32) -> [String: Any]? {
        guard let json = json else { return nil }
        return (try? JSONSerialization.jsonObject(with: Data(bytes: json, count: Int(len)))) as? [String: Any]
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
