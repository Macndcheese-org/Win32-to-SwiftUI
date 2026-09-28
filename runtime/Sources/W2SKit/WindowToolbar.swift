// A Win32 toolbar in the window's frame (map: toolbar, "inFrame").
import AppKit

/// What a control puts in its window's frame (WindowPanes, WindowToolbar):
/// made once the host view is in a window, kept in step with the snapshot,
/// taken down with the control.
protocol WindowChrome: AnyObject {
    func update()
    func detach()
}

/// A toolbar across the top of an app's main window: its buttons are the
/// window's NSToolbar, in the frame where a Mac app's toolbar is (HIG,
/// Toolbars), with the window's title beside them (unified style). The Win32
/// toolbar is kept at no height, so the app lays its content out without it.
/// Same images (SF Symbols for the standard ones, the app's own pixels
/// otherwise), tooltips, enabled and checked states; a click is comctl32's
/// own click, as the toolbar in the window gets.
final class WindowToolbar: NSObject, NSToolbarDelegate, WindowChrome {
    weak var host: ControlHost?
    private(set) var toolbar: NSToolbar?
    private var buttons: [Snapshot.ToolbarButton] = []
    private var retries = 0

    init(host: ControlHost) { self.host = host }

    private func id(_ b: Snapshot.ToolbarButton) -> NSToolbarItem.Identifier {
        b.sep == true ? .space : .init("w2s.button.\(b.i)")
    }

    /// what makes the items: another set means other items (states update in place)
    private func shape(_ list: [Snapshot.ToolbarButton]) -> [String] {
        list.map { "\($0.i)|\($0.sep ?? false)|\($0.id ?? 0)|\($0.sym ?? "")|\($0.img ?? -1)|\($0.check ?? false)|\($0.dropdown ?? 0)" }
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
        if let toolbar = toolbar {
            if shape(list) != shape(buttons) {
                buttons = list
                while !toolbar.items.isEmpty { toolbar.removeItem(at: 0) }
                for (n, b) in list.enumerated() { toolbar.insertItem(withItemIdentifier: id(b), at: n) }
            } else {
                buttons = list
                for item in toolbar.items { refresh(item) }
            }
            return
        }
        buttons = list
        // an identifier of its own: toolbars sharing one keep their items in sync
        let toolbar = NSToolbar(identifier: "org.winehq.w2s.toolbar.\(host.handle)")
        toolbar.delegate = self
        toolbar.displayMode = .iconOnly
        toolbar.allowsUserCustomization = false
        self.toolbar = toolbar
        window.perform(NSSelectorFromString("w2sAttachToolbar:"), with: [
            "toolbar": toolbar,
            "style": NSNumber(value: NSWindow.ToolbarStyle.unified.rawValue),
            "extraWidth": NSNumber(value: 0),
        ] as NSDictionary)
    }

    func detach() {
        guard toolbar != nil else { return }
        toolbar = nil
        host?.hosting?.window?.perform(NSSelectorFromString("w2sDetachToolbar"))
    }

    private func button(for item: NSToolbarItem) -> Snapshot.ToolbarButton? {
        buttons.first { $0.sep != true && $0.i == item.tag && id($0) == item.itemIdentifier }
    }

    private func image(_ b: Snapshot.ToolbarButton) -> NSImage? {
        if let sym = b.sym, sym.hasPrefix("sf:"), !sym.contains(";") {
            // toolbar symbols take the toolbar's own look (size, colour)
            return NSImage(systemSymbolName: String(sym.dropFirst(3)), accessibilityDescription: b.tip ?? b.text)
        }
        if let sym = b.sym, let image = Icons.image(sym, size: NSSize(width: 18, height: 18)) { return image }
        if let index = b.img, let image = host?.model.images[index] { return image }
        return nil
    }

    /// the enabled and checked states the Win32 toolbar has now
    private func refresh(_ item: NSToolbarItem) {
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

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] { buttons.map(id) }
    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] { buttons.map(id) + [.space] }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier identifier: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        guard let b = buttons.first(where: { $0.sep != true && id($0) == identifier }) else { return nil }
        let item = NSToolbarItem(itemIdentifier: identifier)
        let title = stripMnemonic(b.tip ?? b.text ?? "")
        item.label = title
        item.paletteLabel = title
        item.toolTip = title.isEmpty ? nil : title
        item.tag = b.i
        item.autovalidates = false
        if b.check == true {
            // a check button toggles, as a Mac toolbar's toggles do
            let toggle = NSButton(image: image(b) ?? NSImage(), target: self, action: #selector(clicked(_:)))
            toggle.setButtonType(.pushOnPushOff)
            toggle.bezelStyle = .texturedRounded
            toggle.tag = b.i
            toggle.toolTip = item.toolTip
            item.view = toggle
        } else {
            item.image = image(b)
            item.isBordered = true
            item.target = self
            item.action = #selector(clicked(_:))
        }
        refresh(item)
        return item
    }
}
