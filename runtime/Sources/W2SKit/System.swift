// The app's own state on macOS, not a window's (map: taskbar.progress).
import AppKit

@_cdecl("w2s_swift_app_state")
public func w2s_swift_app_state(_ json: UnsafePointer<CChar>?, _ len: UInt32) {
    guard let spec = W2S.object(json, len) else { return }
    DispatchQueue.main.async {
        switch spec["t"] as? String {
        case "progress":
            DockProgress.show(state: spec["state"] as? String ?? "none",
                              value: (spec["value"] as? NSNumber)?.doubleValue ?? 0)
        default:
            break
        }
    }
}

/// Taskbar progress (ITaskbarList3) on the Dock icon, where a Mac app shows
/// its own: the app's icon with a progress bar along its bottom. macOS has no
/// API that draws one; an app gives its Dock tile a content view and redraws
/// the tile when it changes.
enum DockProgress {
    // main thread
    private(set) static var state = "none"
    private(set) static var value = 0.0
    private static var view: TileView?
    private static var timer: Timer?

    static func show(state: String, value: Double) {
        self.state = state
        self.value = value
        let tile = NSApp.dockTile
        guard state != "none" else {
            timer?.invalidate()
            timer = nil
            view = nil
            tile.contentView = nil
            tile.display()
            return
        }
        let view = self.view ?? TileView(frame: NSRect(origin: .zero, size: tile.size))
        self.view = view
        view.update(state: state, value: value)
        if tile.contentView !== view { tile.contentView = view }
        tile.display()
        // an indeterminate bar moves, so the tile is redrawn while it shows
        if state == "indeterminate" {
            if timer == nil {
                let t = Timer(timeInterval: 1.0 / 15, repeats: true) { _ in NSApp.dockTile.display() }
                RunLoop.main.add(t, forMode: .common)
                timer = t
            }
        } else {
            timer?.invalidate()
            timer = nil
        }
    }

    /// The program's icon as winemac gave it to the Dock (from its icon
    /// resource), else macOS's generic application icon. Never AppKit's
    /// default: wine isn't a bundle, so that is the icon of its folder.
    static func programIcon() -> NSImage? {
        if let controller = (NSClassFromString("WineApplicationController") as? NSObject.Type)?
            .perform(NSSelectorFromString("sharedController"))?.takeUnretainedValue() as? NSObject,
           controller.responds(to: NSSelectorFromString("applicationIcon")),
           let image = controller.value(forKey: "applicationIcon") as? NSImage {
            return image
        }
        return Icons.image("uttype:com.apple.application-bundle", size: NSSize(width: 128, height: 128))
    }

    /// Tests: the tile as it stands, and optionally drawn into a PNG.
    static func debug(capture path: String?) -> [String: Any] {
        var out: [String: Any] = ["state": state, "percent": Int((value * 100).rounded()),
                                  "shown": NSApp.dockTile.contentView != nil]
        if let view = view {
            out["indeterminate"] = view.bar.isIndeterminate
            out["barPercent"] = Int((view.bar.doubleValue * 100).rounded())
            out["hasIcon"] = view.icon.image != nil
            if let path = path, let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                view.cacheDisplay(in: view.bounds, to: rep)
                out["captured"] = (try? rep.representation(using: .png, properties: [:])?
                    .write(to: URL(fileURLWithPath: path))) != nil
            }
        }
        return out
    }

    final class TileView: NSView {
        let icon = NSImageView()
        let bar = NSProgressIndicator()

        override init(frame: NSRect) {
            super.init(frame: frame)
            icon.frame = bounds
            icon.imageScaling = .scaleProportionallyUpOrDown
            icon.autoresizingMask = [.width, .height]
            addSubview(icon)
            bar.style = .bar
            bar.isIndeterminate = false
            bar.minValue = 0
            bar.maxValue = 1
            // along the bottom of the icon, inside its rounded square
            bar.frame = NSRect(x: bounds.width * 0.12, y: bounds.height * 0.1,
                               width: bounds.width * 0.76, height: bar.intrinsicContentSize.height)
            bar.autoresizingMask = [.width, .maxYMargin]
            addSubview(bar)
        }

        required init?(coder: NSCoder) { nil }

        func update(state: String, value: Double) {
            icon.image = DockProgress.programIcon()
            bar.isIndeterminate = state == "indeterminate"
            if bar.isIndeterminate {
                bar.startAnimation(nil)
            } else {
                bar.stopAnimation(nil)
                bar.doubleValue = min(max(value, 0), 1)
            }
        }
    }
}
