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
    private var controller: NSHostingController<AnyView>?
    private weak var window: NSWindow?
    private var retries = 0
    private var dragged: CGFloat?

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
            if dragged == nil { window.perform(NSSelectorFromString("w2sSetSidebarWidth:"), with: width) }
            return
        }
        // the Mac's sidebar text, whatever the tree's Win32 size
        let view = ControlRoot(model: host.model, entry: host.entry, fixed: FormMetrics.metrics(scale: host.scale))
        let controller = NSHostingController(rootView: AnyView(view))
        controller.sizingOptions = []
        self.controller = controller
        self.window = window
        hosting.isHidden = true
        window.perform(NSSelectorFromString("w2sAttachSidebar:"),
                       with: ["controller": controller, "width": width, "target": self] as NSDictionary)
    }

    /// winemac: the user moved the divider
    @objc func w2sSidebarResized(_ width: NSNumber) {
        dragged = CGFloat(width.doubleValue)
        settle()
    }

    private func settle() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in
            guard let self = self, let width = self.dragged else { return }
            if NSEvent.pressedMouseButtons & 1 != 0 { return self.settle() }
            self.dragged = nil
            guard let host = self.host, host.scale > 0 else { return }
            host.model.emit(["t": "sidebarWidth", "v": Int((width / host.scale).rounded())])
        }
    }

    func detach() {
        guard controller != nil else { return }
        window?.perform(NSSelectorFromString("w2sDetachSidebar"))
        controller = nil
        host?.hosting?.isHidden = false
    }
}
