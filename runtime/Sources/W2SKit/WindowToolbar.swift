// A Win32 toolbar in the window's frame (map: toolbar, "inFrame").
import AppKit

/// What a control puts in or over its window (SettingsFormController, WindowToolbar):
/// made once the host view is in a window, kept in step with the snapshot,
/// taken down with the control.
protocol WindowChrome: AnyObject {
    func update()
    func detach()
}

/// One window's NSToolbar, in the frame where a Mac app's toolbar is (HIG,
/// Toolbars), with the window's title beside it (unified style): the buttons of
/// each Win32 toolbar of the window that is in the frame, a group each, in the
/// order the app made them (winefile's drive bar and its window buttons).
final class FrameToolbar: NSObject, NSToolbarDelegate {
    private static var byWindow: [ObjectIdentifier: FrameToolbar] = [:]
    let toolbar: NSToolbar
    private weak var window: NSWindow?
    private(set) var members: [WindowToolbar] = []

    /// the window's sidebar button (FrameSidebar), in the sidebar's part of the toolbar
    private(set) var sidebarToggle = false

    private init(window: NSWindow) {
        // an identifier of its own: toolbars sharing one keep their items in sync
        toolbar = NSToolbar(identifier: "org.winehq.w2s.toolbar.\(window.windowNumber)")
        self.window = window
        super.init()
        toolbar.delegate = self
        toolbar.displayMode = .iconOnly
        toolbar.allowsUserCustomization = false
    }

    /// the window's, attached to it the first time
    private static func of(_ window: NSWindow) -> FrameToolbar {
        let key = ObjectIdentifier(window)
        if let frame = byWindow[key] { return frame }
        let frame = FrameToolbar(window: window)
        byWindow[key] = frame
        window.perform(NSSelectorFromString("w2sAttachToolbar:"), with: [
            "toolbar": frame.toolbar,
            "style": NSNumber(value: NSWindow.ToolbarStyle.unified.rawValue),
            "extraWidth": NSNumber(value: 0),
        ] as NSDictionary)
        return frame
    }

    static func join(_ member: WindowToolbar, window: NSWindow) -> FrameToolbar {
        let frame = of(window)
        if !frame.members.contains(where: { $0 === member }) {
            frame.members.append(member)
            frame.members.sort { $0.handle < $1.handle }
        }
        frame.reload(force: true)
        return frame
    }

    /// A Mac window with a sidebar has its button in the toolbar: a window without
    /// a toolbar of the app's gets one for it.
    static func setSidebarToggle(_ on: Bool, window: NSWindow) {
        if on {
            let frame = of(window)
            frame.sidebarToggle = true
            frame.reload(force: true)
        } else if let frame = byWindow[ObjectIdentifier(window)] {
            frame.sidebarToggle = false
            frame.dropIfEmpty()
        }
    }

    func leave(_ member: WindowToolbar) {
        members.removeAll { $0 === member }
        dropIfEmpty()
    }

    private func dropIfEmpty() {
        guard let window = window else { return }
        if members.isEmpty && !sidebarToggle {
            FrameToolbar.byWindow[ObjectIdentifier(window)] = nil
            window.perform(NSSelectorFromString("w2sDetachToolbar"))
        } else {
            reload(force: true)
        }
    }

    private var identifiers: [NSToolbarItem.Identifier] {
        var ids: [NSToolbarItem.Identifier] = sidebarToggle ? [.toggleSidebar, .sidebarTrackingSeparator] : []
        var groups = 0
        for member in members {
            // the app's own "hide the navigation" button is the Mac's, in a window with a sidebar
            let shown = member.buttons.filter { !(sidebarToggle && member.isSidebarButton($0)) }
            if shown.isEmpty { continue }
            if groups > 0 { ids.append(.space) }
            ids += shown.map(member.id)
            groups += 1
        }
        return ids
    }

    /// the items again when a toolbar's buttons are others; else their states
    func reload(force: Bool) {
        let want = identifiers
        if force || toolbar.items.map(\.itemIdentifier) != want {
            while !toolbar.items.isEmpty { toolbar.removeItem(at: 0) }
            for (n, id) in want.enumerated() { toolbar.insertItem(withItemIdentifier: id, at: n) }
        } else {
            for item in toolbar.items { member(of: item.itemIdentifier)?.refresh(item) }
        }
    }

    private func member(of id: NSToolbarItem.Identifier) -> WindowToolbar? {
        members.first { id.rawValue.hasPrefix("w2s.\($0.handle).") }
    }

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] { identifiers }
    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        identifiers + [.space, .toggleSidebar, .sidebarTrackingSeparator]
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier identifier: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        member(of: identifier)?.item(identifier)
    }
}

/// A toolbar across the top of an app's main window: its buttons are in the
/// window's frame toolbar (FrameToolbar). The Win32 toolbar is kept at no
/// height, so the app lays its content out without it. Same images (SF Symbols
/// for the standard ones, the app's own pixels otherwise), text where the
/// toolbar shows it, tooltips, enabled and checked states; a click is
/// comctl32's own click, as the toolbar in the window gets.
final class WindowToolbar: NSObject, WindowChrome {
    weak var host: ControlHost?
    let handle: UInt64
    private var frame: FrameToolbar?
    private(set) var buttons: [Snapshot.ToolbarButton] = []
    private var retries = 0

    var toolbar: NSToolbar? { frame?.toolbar }

    init(host: ControlHost) {
        self.host = host
        handle = host.handle
    }

    func id(_ b: Snapshot.ToolbarButton) -> NSToolbarItem.Identifier {
        b.sep == true ? .space : .init("w2s.\(handle).\(b.i)")
    }

    /// what makes the items: another set means other items (states update in place)
    private func shape(_ list: [Snapshot.ToolbarButton]) -> [String] {
        list.map { "\($0.i)|\($0.sep ?? false)|\($0.id ?? 0)|\($0.sym ?? "")|\($0.img ?? -1)|\($0.check ?? false)|\($0.dropdown ?? 0)|\(text($0) ?? "")" }
    }

    func update() {
        guard let host = host else { return }
        guard host.model.snap.inFrame == true else {
            detach()
            return
        }
        guard let window = host.hosting?.window else {
            guard W2S.control(host.handle) != nil, host.hosting != nil, retries < 100 else { return }
            retries += 1
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in self?.update() }
            return
        }
        let list = (host.model.snap.buttons ?? []).filter { $0.hidden != true }
        if let frame = frame {
            let changed = shape(list) != shape(buttons)
            buttons = list
            frame.reload(force: changed)
            return
        }
        buttons = list
        frame = FrameToolbar.join(self, window: window)
    }

    /// a button that hides or shows the app's navigation pane (the stock sidebar image)
    func isSidebarButton(_ b: Snapshot.ToolbarButton) -> Bool {
        (b.sym ?? b.img.flatMap { host?.model.imageSymbols[$0] }) == "sf:sidebar.left"
    }

    func detach() {
        guard let frame = frame else { return }
        self.frame = nil
        frame.leave(self)
    }

    private func button(for item: NSToolbarItem) -> Snapshot.ToolbarButton? {
        buttons.first { $0.sep != true && $0.i == item.tag && id($0) == item.itemIdentifier }
    }

    /// the text the Win32 toolbar shows on a button: TBSTYLE_LIST with
    /// TBSTYLE_EX_MIXEDBUTTONS shows only BTNS_SHOWTEXT buttons' (the others' is their tooltip)
    private func text(_ b: Snapshot.ToolbarButton) -> String? {
        guard let text = b.text, !text.isEmpty else { return nil }
        let hidden = (host?.model.snap.list ?? false) && (host?.model.snap.mixed ?? false) && !(b.showText ?? false)
        return hidden ? nil : stripMnemonic(text)
    }

    private func image(_ b: Snapshot.ToolbarButton) -> NSImage? {
        // a strip's image the app put in an image list of its own is known by its pixels
        let sym = b.sym ?? b.img.flatMap { host?.model.imageSymbols[$0] }
        if let sym = sym, sym.hasPrefix("sf:"), !sym.contains(";") {
            // toolbar symbols take the toolbar's own look (size, colour)
            return NSImage(systemSymbolName: String(sym.dropFirst(3)), accessibilityDescription: b.tip ?? b.text)
        }
        if let sym = sym, let image = Icons.image(sym, size: NSSize(width: 18, height: 18)) { return image }
        if let index = b.img, let image = host?.model.images[index] { return inPoints(image) }
        return nil
    }

    /// A Win32 image is as many pixels as the app drew; a pixel is not a point when the app
    /// is DPI aware on a Retina display (32 px icons for 16 pt buttons), and the image's
    /// size is in points. The pixels stay, so they are drawn one to one.
    private func inPoints(_ image: NSImage) -> NSImage {
        let scale = host?.scale ?? 1
        // not smaller than a Mac toolbar's 16 points: an app that is DPI aware on a Retina display
        // draws 16 pixel images for 8 points
        let factor = max(scale, 16 / max(1, max(image.size.width, image.size.height)))
        guard factor > 0, abs(factor - 1) > 0.01, let copy = image.copy() as? NSImage else { return image }
        copy.size = NSSize(width: image.size.width * factor, height: image.size.height * factor)
        return copy
    }

    /// the enabled and checked states the Win32 toolbar has now
    func refresh(_ item: NSToolbarItem) {
        guard let b = button(for: item) else { return }
        let enabled = b.enabled ?? true
        if let toggle = item.view as? NSButton {
            toggle.isEnabled = enabled
            toggle.state = b.checked == true ? .on : .off
        }
        item.isEnabled = enabled
        if item.image == nil, item.view == nil { item.image = image(b) }
    }

    @objc func clicked(_ sender: Any) {
        let tag = (sender as? NSToolbarItem)?.tag ?? (sender as? NSButton)?.tag ?? -1
        guard let b = buttons.first(where: { $0.i == tag && $0.sep != true }) else { return }
        host?.model.emit(["t": b.dropdown == 2 ? "dropdown" : "click", "v": b.i])
    }

    func item(_ identifier: NSToolbarItem.Identifier) -> NSToolbarItem? {
        guard let b = buttons.first(where: { $0.sep != true && id($0) == identifier }) else { return nil }
        let item = NSToolbarItem(itemIdentifier: identifier)
        let shown = text(b)
        let title = stripMnemonic(b.tip ?? b.text ?? "")
        item.label = shown ?? title
        item.paletteLabel = item.label
        item.toolTip = title.isEmpty ? nil : title
        item.tag = b.i
        item.autovalidates = false
        if b.check == true {
            // a check button toggles, as a Mac toolbar's toggles do
            let toggle = NSButton(image: image(b) ?? NSImage(), target: self, action: #selector(clicked(_:)))
            toggle.setButtonType(.pushOnPushOff)
            toggle.bezelStyle = .texturedRounded
            if let shown = shown {
                toggle.title = shown
                toggle.imagePosition = .imageLeading
            }
            toggle.tag = b.i
            toggle.toolTip = item.toolTip
            item.view = toggle
        } else {
            // text beside the image, as the button has it in the window (drive letters)
            item.image = image(b)
            if let shown = shown { item.title = shown }
            item.isBordered = true
            item.target = self
            item.action = #selector(clicked(_:))
        }
        refresh(item)
        return item
    }
}
