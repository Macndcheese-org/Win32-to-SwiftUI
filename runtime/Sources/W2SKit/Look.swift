// Light and Dark mode (map: 70-look.yaml). wine draws its windows with its
// system colour table, so that table is written from NSColor: resolved here,
// on the main thread, whenever the appearance or the system colours (the
// accent colour) change, then handed to the Win32 side, which calls
// SetSysColors (WM_SYSCOLORCHANGE) and sends WM_THEMECHANGED. Each native
// view takes the appearance that reads on what the app paints behind it
// (Snapshot.backdrop), so a control on an app's own white page stays light.
import AppKit

enum Look {
    /// COLOR_* index -> the NSColor the map gives it. COLOR_BACKGROUND (the
    /// desktop) is left alone: macOS's desktop picture belongs to macOS.
    static let table: [(index: Int, color: () -> NSColor)] = [
        (0, { .controlBackgroundColor }),           // COLOR_SCROLLBAR
        (2, { .windowBackgroundColor }),            // COLOR_ACTIVECAPTION
        (3, { .windowBackgroundColor }),            // COLOR_INACTIVECAPTION
        (4, { .windowBackgroundColor }),            // COLOR_MENU
        (5, { .textBackgroundColor }),              // COLOR_WINDOW
        (6, { .separatorColor }),                   // COLOR_WINDOWFRAME
        (7, { .labelColor }),                       // COLOR_MENUTEXT
        (8, { .textColor }),                        // COLOR_WINDOWTEXT
        (9, { .labelColor }),                       // COLOR_CAPTIONTEXT
        (10, { .separatorColor }),                  // COLOR_ACTIVEBORDER
        (11, { .separatorColor }),                  // COLOR_INACTIVEBORDER
        (12, { .underPageBackgroundColor }),        // COLOR_APPWORKSPACE
        (13, { .selectedContentBackgroundColor }),  // COLOR_HIGHLIGHT (the accent colour)
        (14, { .alternateSelectedControlTextColor }), // COLOR_HIGHLIGHTTEXT
        (15, { .windowBackgroundColor }),           // COLOR_BTNFACE, the dialog background
        (16, { .separatorColor }),                  // COLOR_BTNSHADOW
        (17, { .disabledControlTextColor }),        // COLOR_GRAYTEXT
        (18, { .controlTextColor }),                // COLOR_BTNTEXT
        (19, { .secondaryLabelColor }),             // COLOR_INACTIVECAPTIONTEXT
        (20, { .controlBackgroundColor }),          // COLOR_BTNHIGHLIGHT
        (21, { .shadowColor }),                     // COLOR_3DDKSHADOW
        (22, { .controlColor }),                    // COLOR_3DLIGHT
        (23, { .labelColor }),                      // COLOR_INFOTEXT
        (24, { .windowBackgroundColor }),           // COLOR_INFOBK
        (25, { NSColor.alternatingContentBackgroundColors.last ?? .controlBackgroundColor }), // COLOR_ALTERNATEBTNFACE
        (26, { .linkColor }),                       // COLOR_HOTLIGHT
        (27, { .windowBackgroundColor }),           // COLOR_GRADIENTACTIVECAPTION
        (28, { .windowBackgroundColor }),           // COLOR_GRADIENTINACTIVECAPTION
        (29, { .selectedContentBackgroundColor }),  // COLOR_MENUHILIGHT
        (30, { .windowBackgroundColor }),           // COLOR_MENUBAR
    ]

    static let invalid: UInt32 = 0xffff_ffff

    // guarded by W2S.lock: what the Win32 side reads (w2s_swift_system_colors)
    static var colors: [UInt32] = []
    static var dark = false
    static var version: UInt64 = 0

    // main thread
    private static var observation: NSKeyValueObservation?
    private static var observer: NSObjectProtocol?
    /// tests: resolve against this appearance instead of the app's
    static var override: NSAppearance.Name?

    /// Main thread: watch the appearance and the system colours.
    static func start() {
        guard observation == nil else { return }
        observation = NSApplication.shared.observe(\.effectiveAppearance, options: [.new]) { _, _ in
            // after the change has gone through
            DispatchQueue.main.async { refresh() }
        }
        observer = NotificationCenter.default.addObserver(forName: NSColor.systemColorsDidChangeNotification,
                                                          object: nil, queue: .main) { _ in refresh() }
        refresh()
    }

    static var appearance: NSAppearance {
        override.flatMap { NSAppearance(named: $0) } ?? NSApplication.shared.effectiveAppearance
    }

    static func isDark(_ appearance: NSAppearance) -> Bool {
        appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua
    }

    /// An sRGB COLORREF; a translucent colour (the separators) as it shows on
    /// the window background.
    static func colorref(_ color: NSColor, over background: NSColor?) -> UInt32 {
        guard let c = color.usingColorSpace(.sRGB) else { return 0 }
        var r = c.redComponent, g = c.greenComponent, b = c.blueComponent
        if c.alphaComponent < 1, let bg = background?.usingColorSpace(.sRGB) {
            let a = c.alphaComponent
            r = r * a + bg.redComponent * (1 - a)
            g = g * a + bg.greenComponent * (1 - a)
            b = b * a + bg.blueComponent * (1 - a)
        }
        func byte(_ v: CGFloat) -> UInt32 { UInt32((min(max(v, 0), 1) * 255).rounded()) }
        return byte(r) | byte(g) << 8 | byte(b) << 16
    }

    /// Main thread: resolve the table; on a change, wake the Win32 side.
    static func refresh() {
        let appearance = self.appearance
        var table = [UInt32](repeating: invalid, count: 32)
        appearance.performAsCurrentDrawingAppearance {
            let background = NSColor.windowBackgroundColor.usingColorSpace(.sRGB)
            for entry in Look.table { table[entry.index] = colorref(entry.color(), over: background) }
        }
        let isDark = self.isDark(appearance)
        W2S.lock.lock()
        let changed = table != colors || isDark != dark
        if changed {
            colors = table
            dark = isDark
            version += 1
        }
        W2S.lock.unlock()
        if changed { W2S.wakeAny(cookie: wakeLook) }
    }

    /// w2s_wake_message's wparam for "the system colours changed" (w2s_pe.h W2S_WAKE_LOOK)
    static let wakeLook: UInt64 = 3

    /// The appearance of a native view over what the app paints behind it (a
    /// COLORREF), or nil to follow the window: dark on a dark backdrop.
    static func appearance(backdrop: Int?) -> NSAppearance? {
        guard let c = backdrop, c >= 0, UInt32(truncatingIfNeeded: c) != invalid else { return nil }
        let r = Double(c & 0xff) / 255, g = Double((c >> 8) & 0xff) / 255, b = Double((c >> 16) & 0xff) / 255
        let luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b
        return NSAppearance(named: luminance < 0.5 ? .darkAqua : .aqua)
    }

    static func apply(_ backdrop: Int?, to view: NSView?) {
        guard let view = view else { return }
        let want = appearance(backdrop: backdrop)
        if view.appearance?.name != want?.name { view.appearance = want }
    }
}

@_cdecl("w2s_swift_system_colors")
public func w2s_swift_system_colors(_ version: UnsafeMutablePointer<UInt64>?, _ colors: UnsafeMutablePointer<UInt32>?,
                                    _ size: UInt32, _ dark: UnsafeMutablePointer<UInt32>?) -> UInt32 {
    guard let version = version, let colors = colors else { return 0 }
    W2S.lock.lock()
    defer { W2S.lock.unlock() }
    guard !Look.colors.isEmpty, version.pointee != Look.version else { return 0 }
    let count = min(Int(size), Look.colors.count)
    for i in 0..<count { colors[i] = Look.colors[i] }
    dark?.pointee = Look.dark ? 1 : 0
    version.pointee = Look.version
    return UInt32(count)
}
