// A tree along a main window's leading edge (map: treeview, sidebarPane) as the
// window's own sidebar: Finder's (NSSplitViewController's sidebar item).
import AppKit
import SwiftUI

/// The tree's outline goes in the window's sidebar (winemac: w2sAttachSidebar),
/// which floats over wine's content on macOS 26 and later: the content keeps its
/// size underneath, and the sidebar covers the part the app laid its tree out
/// in, up to the pane beside it. The outline in the window is hidden meanwhile.
/// The sidebar follows the app's layout; a drag of its divider drags the app's
/// own splitter once the button is up, and the app lays itself out again.
final class FrameSidebar: NSObject, WindowChrome {
    weak var host: ControlHost?
    /// a sidebar of several pages (HTML Help's navigation pane): the controls of the pages, not the one tree
    var group: SidebarGroup?
    private var controller: NSHostingController<AnyView>?     // a SidebarHostingController for a sidebar of pages
    private weak var window: NSWindow?
    private var retries = 0
    private var dragged: CGFloat?
    private var lastMove = Date.distantPast
    private(set) var collapsed = false
    private var attachedAt = Date.distantPast
    private var fitted: CGFloat = 0

    init(host: ControlHost) { self.host = host }

    var attached: Bool { controller != nil }
    var sidebarView: NSView? { controller?.view }

    func update() {
        guard #available(macOS 26.0, *) else { return }
        guard let host = host, let pane = host.model.snap.sidebarPane, pane > 0 else {
            detach()
            return
        }
        guard let hosting = host.hosting, let window = hosting.window else {
            guard W2S.control(host.handle) != nil, host.hosting != nil, retries < 100 else { return }
            retries += 1
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in self?.update() }
            return
        }
        let width = NSNumber(value: Double(CGFloat(pane) * host.scale))
        if let _ = controller {
            // the app laid itself out again (or the window resized): the sidebar follows,
            // unless the user is the one dragging it
            if dragged == nil && !collapsed { window.perform(NSSelectorFromString("w2sSetSidebarWidth:"), with: width) }
            DispatchQueue.main.async { [weak self] in self?.fitRows() }
            return
        }
        // the Mac's sidebar text, whatever the tree's Win32 size
        let content: AnyView
        if let group = group {
            content = AnyView(SidebarGroupView(group: group))
        } else {
            content = AnyView(ControlRoot(model: host.model, entry: host.entry, fixed: FormMetrics.metrics(scale: host.scale))
                .environment(\.w2sInSidebar, true))
        }
        let controller: NSHostingController<AnyView>
        if let group = group {
            let paged = SidebarHostingController(rootView: content)
            paged.group = group
            controller = paged
        } else {
            controller = NSHostingController(rootView: content)
        }
        controller.sizingOptions = []
        self.controller = controller
        self.window = window
        hosting.isHidden = true
        var spec: [String: Any] = ["controller": controller, "width": width, "target": self]
        if host.model.snap.sbFixed == true {
            // a window of a fixed size: no splitter follows the sidebar, and the floating sidebar would cover
            // the app's pane; it keeps the width the app laid its tree out in
            spec["minWidth"] = width
            spec["maxWidth"] = width
            // ... and with nothing to follow it, hiding it would only leave the app's layout with a gap and
            // the categories out of reach: it is not hidden
            spec["canCollapse"] = false
        } else if group != nil {
            // AppKit's own limits: a sidebar keeps its default minimum (144 pt) and has no maximum; so that
            // the content cannot be squeezed out by a wide one, it keeps AppKit's content list minimum (200 pt)
            spec["contentMinWidth"] = 200
        }
        window.perform(NSSelectorFromString("w2sAttachSidebar:"), with: spec as NSDictionary)
        if host.model.snap.sbFixed != true { FrameToolbar.setSidebarToggle(true, window: window) }
        attachedAt = Date()
        for delay in [0.2, 0.6, 1.2, 1.8] {
            DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in self?.fitRows() }
        }
        // the sidebar's safe area is there once the window has laid out, which takes its time
        if group != nil {
            for step in 0..<48 {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.25 * Double(step)) { [weak self] in self?.syncInset() }
            }
        }
    }

    /// a sidebar of pages: its content starts below the title bar, where the sidebar's safe area does
    private func syncInset() {
        guard let group = group, let side = sidebarView else { return }
        // the safe area, or what the title bar and toolbar take of the window's top (the safe area is late)
        var inset = side.safeAreaInsets.top
        if let window = side.window { inset = max(inset, window.frame.height - window.contentLayoutRect.maxY) }
        if abs(group.topInset - inset) > 0.5 { group.topInset = inset }
    }

    /// The window's list in the sidebar (SwiftUI's outline view).
    static func table(in view: NSView) -> NSTableView? {
        if let t = view as? NSTableView { return t }
        for sub in view.subviews { if let t = table(in: sub) { return t } }
        return nil
    }

    /// The rows a window opens with are shown whole: in its first moments, the
    /// sidebar widens by what the rows on screen fall short of (never past half
    /// the window), and the app's splitter follows, as after a drag. The app's
    /// own width is in Win32 pixels, laid out for its text, not the Mac's.
    private func fitRows() {
        guard Date().timeIntervalSince(attachedAt) < 2, dragged == nil, !collapsed, let host = host,
              let side = sidebarView, let window = side.window else { return }
        let short = host.model.rowShortfall.values.max() ?? 0
        guard short > 0.5 else { return }
        let need = min(ceil(side.bounds.width + short), (window.frame.width / 2).rounded(.down))
        guard need > side.bounds.width + 1, need > fitted else { return }
        fitted = need
        window.perform(NSSelectorFromString("w2sSetSidebarWidth:"), with: NSNumber(value: Double(need)))
        w2sSidebarResized(NSNumber(value: Double(need)))
    }

    /// winemac: the user moved the divider, or hid or showed the sidebar (0: hidden)
    @objc func w2sSidebarResized(_ width: NSNumber) {
        dragged = CGFloat(width.doubleValue)
        collapsed = width.doubleValue == 0
        lastMove = Date()
        settle()
    }

    /// once the button is up and the divider has stopped (the sidebar's animation)
    private func settle() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
            guard let self = self, let width = self.dragged else { return }
            if NSEvent.pressedMouseButtons & 1 != 0 || Date().timeIntervalSince(self.lastMove) < 0.1 {
                return self.settle()
            }
            self.dragged = nil
            guard let host = self.host, host.scale > 0 else { return }
            host.model.emit(["t": "sidebarWidth", "v": Int((width / host.scale).rounded())])
        }
    }

    func detach() {
        guard controller != nil else { return }
        if let window = window { FrameToolbar.setSidebarToggle(false, window: window) }
        window?.perform(NSSelectorFromString("w2sDetachSidebar"))
        controller = nil
        host?.hosting?.isHidden = false
    }
}

struct InSidebarKey: EnvironmentKey { static let defaultValue = false }
extension EnvironmentValues {
    /// in the window's sidebar (FrameSidebar), not the control's own place in the window
    var w2sInSidebar: Bool {
        get { self[InSidebarKey.self] }
        set { self[InSidebarKey.self] = newValue }
    }
}

struct ScaleKey: EnvironmentKey { static let defaultValue: CGFloat = 1 }
extension EnvironmentValues {
    /// points per Win32 pixel for the control being drawn (Metrics.scale)
    var w2sScale: CGFloat {
        get { self[ScaleKey.self] }
        set { self[ScaleKey.self] = newValue }
    }
}
