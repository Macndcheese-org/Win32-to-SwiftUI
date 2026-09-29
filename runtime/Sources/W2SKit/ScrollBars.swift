// Scroll bars (map: scrollbar): a window's own (WS_VSCROLL/WS_HSCROLL) and the
// ScrollBar control, as NSScrollers. A window's host view covers its client
// area, which stays wine's; it reaches over the scroll bars (outsets), where
// the scrollers are, in the legacy style: always shown, as with "Show scroll
// bars: Always", so they cover wine's. Clicks elsewhere go to wine.
import AppKit
import SwiftUI

extension Snapshot {
    struct ScrollBarState: Codable, Equatable {
        var vert: Bool
        var rect: [Double]      // Win32: in the client area's coordinates (a window's are outside it)
        var min: Int
        var max: Int
        var page: Int
        var pos: Int
        var enabled: Bool

        var cgRect: CGRect { rect.count == 4 ? CGRect(x: rect[0], y: rect[1], width: rect[2], height: rect[3]) : .zero }
    }
}

enum ScrollGeometry {
    /// how far the bars reach outside the client area (Win32 pixels): top, left, bottom, right
    static func outsets(_ snap: Snapshot) -> [Double] {
        let bars = snap.bars ?? []
        let w = snap.widthPx ?? 0, h = snap.heightPx ?? 0
        var o = [0.0, 0.0, 0.0, 0.0]
        for bar in bars {
            let r = bar.cgRect
            o[0] = max(o[0], -Double(r.minY))
            o[1] = max(o[1], -Double(r.minX))
            o[2] = max(o[2], Double(r.maxY) - h)
            o[3] = max(o[3], Double(r.maxX) - w)
        }
        return o
    }

    /// a bar in the host view (points), which starts at the outsets' corner
    static func frame(_ bar: Snapshot.ScrollBarState, _ snap: Snapshot, scale: CGFloat) -> CGRect {
        let o = outsets(snap), r = bar.cgRect
        return CGRect(x: (r.minX + CGFloat(o[1])) * scale, y: (r.minY + CGFloat(o[0])) * scale,
                      width: r.width * scale, height: r.height * scale)
    }

    /// points per Win32 pixel in a host view covering the client area and the outsets
    static func scale(_ snap: Snapshot, height: CGFloat) -> CGFloat {
        let o = outsets(snap)
        let px = (snap.heightPx ?? 0) + o[0] + o[2]
        let s = px > 0 ? height / CGFloat(px) : 1
        return s.isFinite && s > 0 ? s : 1
    }
}

/// The bars of one window (or the one of a ScrollBar control), where wine has them.
struct ScrollBars: View {
    @ObservedObject var model: ControlModel

    var body: some View {
        GeometryReader { geo in
            let snap = model.snap
            let bars = snap.bars ?? []
            let s = ScrollGeometry.scale(snap, height: geo.size.height)
            ZStack(alignment: .topLeading) {
                ForEach(bars.indices, id: \.self) { i in
                    let f = ScrollGeometry.frame(bars[i], snap, scale: s)
                    // the track is see-through (macOS 26 and later): over wine's bar, the view's background
                    NativeScroller(model: model, bar: bars[i])
                        .frame(width: f.width, height: f.height)
                        .background(Color(nsColor: .controlBackgroundColor))
                        .offset(x: f.minX, y: f.minY)
                }
                // with both bars, the corner between them is the window's, not wine's grey square
                if let v = bars.first(where: { $0.vert }), let h = bars.first(where: { !$0.vert }) {
                    let fv = ScrollGeometry.frame(v, snap, scale: s), fh = ScrollGeometry.frame(h, snap, scale: s)
                    Rectangle().fill(Color(nsColor: .controlBackgroundColor))
                        .frame(width: fv.width, height: fh.height)
                        .offset(x: fv.minX, y: fh.minY)
                }
            }
            .frame(width: geo.size.width, height: geo.size.height, alignment: .topLeading)
        }
    }
}

/// One NSScroller. A page click is SB_PAGEUP/SB_PAGEDOWN; dragging the knob (or
/// clicking in its slot, which jumps there) is SB_THUMBTRACK as it moves, then
/// SB_THUMBPOSITION and SB_ENDSCROLL once the button is up. macOS scrollers have
/// no arrows, so SB_LINEUP/SB_LINEDOWN only come from the app's own wheel handling.
struct NativeScroller: NSViewRepresentable {
    @ObservedObject var model: ControlModel
    let bar: Snapshot.ScrollBarState

    // the Win32 codes (winuser.h)
    static let pageUp = 2, pageDown = 3, thumbPosition = 4, thumbTrack = 5, endScroll = 8

    final class Coordinator: NSObject {
        var model: ControlModel
        var bar: Snapshot.ScrollBarState
        var tracking = false
        var lastPos = 0
        init(model: ControlModel, bar: Snapshot.ScrollBarState) {
            self.model = model
            self.bar = bar
        }

        private var event: String { bar.vert ? "vscroll" : "hscroll" }

        /// the Win32 position a knob value stands for
        func position(_ value: Double) -> Int {
            let range = max(0, bar.max - bar.min + 1 - max(bar.page, 1))
            return bar.min + Int((value * Double(range)).rounded())
        }

        @objc func scrolled(_ scroller: NSScroller) {
            switch scroller.hitPart {
            case .decrementPage:
                model.emit(["t": event, "v": NativeScroller.pageUp])
            case .incrementPage:
                model.emit(["t": event, "v": NativeScroller.pageDown])
            case .knob, .knobSlot:
                let pos = position(scroller.doubleValue)
                tracking = true
                if pos != lastPos {
                    lastPos = pos
                    model.emit(["t": event, "v": NativeScroller.thumbTrack, "a": [pos]])
                }
                settle(scroller)
            default:
                break
            }
        }

        /// the drag ends when the button is up: where it ended, then the end
        private func settle(_ scroller: NSScroller) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self, weak scroller] in
                guard let self = self, let scroller = scroller, self.tracking else { return }
                if NSEvent.pressedMouseButtons & 1 != 0 {
                    self.settle(scroller)
                    return
                }
                self.tracking = false
                let pos = self.position(scroller.doubleValue)
                self.model.emit(["t": self.event, "v": NativeScroller.thumbPosition, "a": [pos]])
                self.model.emit(["t": self.event, "v": NativeScroller.endScroll])
            }
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator(model: model, bar: bar) }

    func makeNSView(context: Context) -> NSScroller {
        let scroller = NSScroller(frame: NSRect(x: 0, y: 0, width: bar.vert ? 15 : 100, height: bar.vert ? 100 : 15))
        scroller.scrollerStyle = .legacy
        scroller.target = context.coordinator
        scroller.action = #selector(Coordinator.scrolled(_:))
        return scroller
    }

    func updateNSView(_ scroller: NSScroller, context: Context) {
        let c = context.coordinator
        c.model = model
        c.bar = bar
        scroller.isEnabled = bar.enabled
        let total = Double(max(1, bar.max - bar.min + 1))
        scroller.knobProportion = bar.page > 0 ? CGFloat(min(1, Double(bar.page) / total)) : 0.1
        // while the user drags, the knob is theirs
        guard !c.tracking else { return }
        let range = Double(max(1, bar.max - bar.min + 1 - max(bar.page, 1)))
        scroller.doubleValue = min(1, max(0, Double(bar.pos - bar.min) / range))
        c.lastPos = bar.pos
    }
}

/// A window's host view reaches over its scroll bars (they are outside its client area).
final class ScrollBarsController: WindowChrome {
    weak var host: ControlHost?
    private var outsets: [CGFloat] = [0, 0, 0, 0]

    init(host: ControlHost) { self.host = host }

    func update() {
        guard let host = host, let hostView = host.hosting?.superview, let px = host.model.snap.heightPx, px > 0 else { return }
        // points per pixel from the client area the host view covers without its outsets
        let s = (hostView.frame.height - outsets[0] - outsets[2]) / CGFloat(px)
        guard s.isFinite, s > 0 else { return }
        let want = ScrollGeometry.outsets(host.model.snap).map { CGFloat($0) * s }
        guard want != outsets else { return }
        outsets = want
        hostView.perform(NSSelectorFromString("w2sSetOutsets:"), with: want.map { NSNumber(value: Double($0)) } as NSArray)
    }

    func detach() {}
}
