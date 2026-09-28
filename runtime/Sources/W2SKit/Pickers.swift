// ChooseColor, ChooseFont and PrintDlg as the macOS panels (map: colordialog,
// fontdialog, printdialog). The colour and font panels are app-wide panels
// that macOS never runs modally: they show as plain windows with a Cancel/OK
// accessory, and the Win32 thread supplies the modality (dialogs.c). The print
// panel only runs as a sheet on its owner; without one the PE side keeps
// wine's dialog.
import AppKit
import SwiftUI

/// Cancel and OK under a panel, in wine's language (the labels come from the
/// PE side); Return and Escape as in any macOS dialog.
struct OKCancel: View {
    let okTitle: String
    let cancelTitle: String
    var okEnabled = true
    var note = ""
    let ok: () -> Void
    let cancel: () -> Void

    var body: some View {
        HStack {
            if !note.isEmpty { Text(note).foregroundColor(.secondary).lineLimit(2) }
            Spacer()
            Button(stripMnemonic(cancelTitle), action: cancel).keyboardShortcut(.cancelAction)
            Button(stripMnemonic(okTitle), action: ok).keyboardShortcut(.defaultAction).disabled(!okEnabled)
        }
    }
}

extension Requests {
    /// [r, g, b] 0-255 <-> an sRGB colour (COLORREF is sRGB)
    static func rgbColor(_ value: Any?) -> NSColor {
        let c = (value as? [Any])?.compactMap { ($0 as? NSNumber)?.doubleValue } ?? []
        guard c.count >= 3 else { return .black }
        return NSColor(srgbRed: CGFloat(c[0]) / 255, green: CGFloat(c[1]) / 255, blue: CGFloat(c[2]) / 255, alpha: 1)
    }

    static func rgbComponents(_ color: NSColor) -> [Int] {
        guard let c = color.usingColorSpace(.sRGB) else { return [0, 0, 0] }
        func byte(_ v: CGFloat) -> Int { Int((min(max(v, 0), 1) * 255).rounded()) }
        return [byte(c.redComponent), byte(c.greenComponent), byte(c.blueComponent)]
    }

    /// Only one request owns a shared panel; a newer one closes the older.
    static var colorOwner: (() -> Void)?
    static var fontOwner: (() -> Void)?

    static func accessory<V: View>(_ view: V, width: CGFloat) -> NSView {
        let hosting = NSHostingView(rootView: view.padding(8).frame(width: width))
        hosting.setFrameSize(hosting.fittingSize)
        return hosting
    }

    // MARK: ChooseColor -> NSColorPanel (map: colordialog)

    static func colorPanel(_ params: [String: Any], request: Request) {
        colorOwner?()
        let panel = NSColorPanel.shared
        let saved = (accessory: panel.accessoryView, alpha: panel.showsAlpha, title: panel.title)
        panel.showsAlpha = false
        panel.isContinuous = true
        panel.color = rgbColor(params["color"])
        if let title = params["title"] as? String, !title.isEmpty { panel.title = title }

        // the app's 16 custom colours are a colour list in the panel; "+" adds
        // the current colour to it, which fills the slots from the first, as
        // "Add to Custom Colors" does on Windows
        let list = NSColorList(name: params["listName"] as? String ?? "Custom Colors")
        // flat: r, g, b of each of the 16 slots
        let custom = params["custom"] as? [Int] ?? []
        for i in 0..<16 {
            let rgb = custom.count >= i * 3 + 3 ? Array(custom[(i * 3)..<(i * 3 + 3)]) : [255, 255, 255]
            list.setColor(rgbColor(rgb), forKey: "\(i + 1)")
        }
        panel.attachColorList(list)

        func customColors() -> [Int] {
            var slots = (0..<16).map { rgbComponents(list.color(withKey: "\($0 + 1)") ?? .white) }
            let added = list.allKeys.filter { Int($0) == nil }
            for (i, key) in added.prefix(16).enumerated() { slots[i] = rgbComponents(list.color(withKey: key) ?? .white) }
            return slots.flatMap { $0 }
        }

        var done = false
        var observer: NSObjectProtocol?
        func close(_ ok: Bool) {
            guard !done else { return }
            done = true
            var result: [String: Any] = ["ok": ok, "custom": customColors()]
            if ok { result["color"] = rgbComponents(panel.color) }
            finish(request, result)
            if let observer = observer { NotificationCenter.default.removeObserver(observer) }
            colorOwner = nil
            panel.detachColorList(list)
            panel.accessoryView = saved.accessory
            panel.showsAlpha = saved.alpha
            panel.title = saved.title
            panel.orderOut(nil)
        }
        colorOwner = { close(false) }
        panel.accessoryView = accessory(
            OKCancel(okTitle: params["ok"] as? String ?? "OK", cancelTitle: params["cancel"] as? String ?? "Cancel",
                     ok: { close(true) }, cancel: { close(false) }),
            width: 240)
        // its close button is Cancel
        observer = NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: panel,
                                                          queue: .main) { _ in close(false) }
        request.inject = { event in
            // tests: {"t":"pick","a":[r,g,b]}, {"t":"custom","a":[slot,r,g,b]}, {"t":"ok"}, {"t":"cancel"}
            guard let e = event as? [String: Any] else { return }
            let a = e["a"] as? [Int] ?? []
            switch e["t"] as? String {
            case "pick": panel.color = rgbColor(a)
            case "custom" where a.count == 4 && a[0] >= 0 && a[0] < 16:
                list.setColor(rgbColor(Array(a[1...])), forKey: "\(a[0] + 1)")
            case "ok": close(true)
            default: close(false)
            }
        }
        request.query = {
            ["color": rgbComponents(panel.color), "custom": customColors(), "visible": panel.isVisible,
             "alpha": panel.showsAlpha]
        }
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
    }

    // MARK: ChooseFont -> NSFontPanel (map: fontdialog)

    static func fontPanel(_ params: [String: Any], request: Request) {
        fontOwner?()
        let manager = NSFontManager.shared
        let panel = manager.fontPanel(true) ?? NSFontPanel.shared
        let model = FontModel(params)
        let target = FontTarget(model: model)
        let saved = (accessory: panel.accessoryView, target: manager.target, action: manager.action)
        manager.target = target
        manager.action = #selector(FontTarget.changeFont(_:))
        manager.setSelectedFont(model.font, isMultiple: false)
        panel.setPanelFont(model.font, isMultiple: false)

        var done = false
        var observer: NSObjectProtocol?
        func close(_ ok: Bool) {
            guard !done else { return }
            done = true
            var result: [String: Any] = ["ok": ok]
            if ok { result.merge(model.result()) { $1 } }
            finish(request, result)
            if let observer = observer { NotificationCenter.default.removeObserver(observer) }
            fontOwner = nil
            manager.target = saved.target
            manager.action = saved.action
            panel.accessoryView = saved.accessory
            panel.orderOut(nil)
            _ = target
        }
        fontOwner = { close(false) }
        panel.accessoryView = accessory(FontAccessory(model: model, ok: { close(true) }, cancel: { close(false) }),
                                        width: 380)
        observer = NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: panel,
                                                          queue: .main) { _ in close(false) }
        request.inject = { event in
            // tests: {"t":"font","s":family,"v":points}, {"t":"underline","v":0|1},
            // {"t":"strikeout","v":0|1}, {"t":"pick","a":[r,g,b]}, {"t":"ok"}, {"t":"cancel"}
            guard let e = event as? [String: Any] else { return }
            let v = (e["v"] as? NSNumber)?.doubleValue ?? 0
            switch e["t"] as? String {
            case "font":
                if let family = e["s"] as? String, let font = manager.font(withFamily: family, traits: [], weight: 5,
                                                                            size: CGFloat(v)) {
                    model.font = font
                    panel.setPanelFont(font, isMultiple: false)
                }
            case "underline": model.underline = v != 0
            case "strikeout": model.strikeout = v != 0
            case "pick": model.color = Color(nsColor: rgbColor(e["a"]))
            case "ok": if model.valid { close(true) }
            default: close(false)
            }
        }
        request.query = {
            var out = model.result()
            out["visible"] = panel.isVisible
            out["valid"] = model.valid
            out["effects"] = model.effects
            return out
        }
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
    }

    // MARK: PrintDlg / PrintDlgEx -> NSPrintPanel (map: printdialog)

    final class PrintTarget: NSObject {
        let done: (Bool) -> Void
        init(done: @escaping (Bool) -> Void) { self.done = done }
        @objc func printPanelDidEnd(_ panel: NSPrintPanel, returnCode: Int, contextInfo: UnsafeMutableRawPointer?) {
            done(returnCode == NSApplication.ModalResponse.OK.rawValue)
        }
        @objc func pageLayoutDidEnd(_ layout: NSPageLayout, returnCode: Int, contextInfo: UnsafeMutableRawPointer?) {
            done(returnCode == NSApplication.ModalResponse.OK.rawValue)
        }
    }

    // MARK: PageSetupDlg -> NSPageLayout (map: pagesetup)

    /// The macOS Page Setup sheet: the printer to format for, paper size and
    /// orientation. A sheet or nothing: the PE side then shows wine's dialog.
    static func pageLayout(_ params: [String: Any], owner: NSWindow?, request: Request) {
        guard let owner = owner, owner.attachedSheet == nil else {
            finish(request, ["declined": true])
            return
        }
        let info = (NSPrintInfo.shared.copy() as? NSPrintInfo) ?? NSPrintInfo()
        if let name = params["printer"] as? String, let printer = NSPrinter(name: name) { info.printer = printer }
        if let paper = params["paper"] as? String, !paper.isEmpty { info.paperName = NSPrinter.PaperName(paper) }
        info.orientation = params["landscape"] as? Bool == true ? .landscape : .portrait

        let layout = NSPageLayout()
        var done = false
        var keep: ObjectIdentifier?
        func result() -> [String: Any] {
            ["printer": info.printer.name, "landscape": info.orientation == .landscape,
             "paper": info.paperName?.rawValue ?? "",
             "paperWidth": Double(info.paperSize.width), "paperHeight": Double(info.paperSize.height)]
        }
        func close(_ ok: Bool) {
            guard !done else { return }
            done = true
            var out = result()
            out["ok"] = ok
            finish(request, out)
            if let keep = keep { retained.removeValue(forKey: keep) }
        }
        let target = PrintTarget { ok in close(ok) }
        request.inject = { event in
            // tests: {"t":"pagesetup","landscape":bool} or {"t":"cancel"}
            guard let e = event as? [String: Any] else { return }
            if e["t"] as? String == "pagesetup" {
                if let landscape = e["landscape"] as? Bool { info.orientation = landscape ? .landscape : .portrait }
                close(true)
            } else {
                close(false)
            }
            if let sheet = owner.attachedSheet { owner.endSheet(sheet) }
        }
        request.query = { result() }
        // the sheet doesn't keep its delegate
        keep = ObjectIdentifier(target)
        retained[ObjectIdentifier(target)] = [target, layout, info] as NSArray
        layout.beginSheet(with: info, modalFor: owner, delegate: target,
                          didEnd: #selector(PrintTarget.pageLayoutDidEnd(_:returnCode:contextInfo:)), contextInfo: nil)
    }

    static func printPanel(_ params: [String: Any], owner: NSWindow?, request: Request) {
        // a sheet or nothing: the PE side then shows wine's dialog
        guard let owner = owner, owner.attachedSheet == nil else {
            finish(request, ["declined": true])
            return
        }
        let info = (NSPrintInfo.shared.copy() as? NSPrintInfo) ?? NSPrintInfo()
        if let name = params["printer"] as? String, let printer = NSPrinter(name: name) { info.printer = printer }
        info.orientation = params["landscape"] as? Bool == true ? .landscape : .portrait
        let settings = info.dictionary()
        func set(_ key: NSPrintInfo.AttributeKey, _ value: Any) { settings[key.rawValue] = value }
        let pageNums = params["pageNums"] as? Bool ?? false
        set(.copies, max(1, params["copies"] as? Int ?? 1))
        set(.mustCollate, params["collate"] as? Bool ?? false)
        set(.allPages, !pageNums)
        set(.firstPage, max(1, params["from"] as? Int ?? 1))
        set(.lastPage, max(1, params["to"] as? Int ?? 1))
        set(.selectionOnly, params["selection"] as? Bool ?? false)

        let panel = NSPrintPanel()
        var options: NSPrintPanel.Options = [.showsCopies, .showsPaperSize, .showsOrientation]
        if params["noPageNums"] as? Bool != true { options.insert(.showsPageRange) }
        if params["noSelection"] as? Bool == false { options.insert(.showsPrintSelection) }
        panel.options = options

        var done = false
        var keep: ObjectIdentifier?
        func result() -> [String: Any] {
            let s = info.dictionary()
            func int(_ key: NSPrintInfo.AttributeKey, _ def: Int) -> Int { (s[key.rawValue] as? NSNumber)?.intValue ?? def }
            func bool(_ key: NSPrintInfo.AttributeKey) -> Bool { (s[key.rawValue] as? NSNumber)?.boolValue ?? false }
            return ["printer": info.printer.name,
                    "copies": int(.copies, 1), "collate": bool(.mustCollate),
                    "allPages": bool(.allPages), "from": int(.firstPage, 1), "to": int(.lastPage, 1),
                    "selection": bool(.selectionOnly),
                    "landscape": info.orientation == .landscape,
                    "paper": info.paperName?.rawValue ?? "",
                    "paperSize": [Double(info.paperSize.width), Double(info.paperSize.height)]]
        }
        func close(_ ok: Bool) {
            guard !done else { return }
            done = true
            var out = result()
            out["ok"] = ok
            finish(request, out)
            if let keep = keep { retained.removeValue(forKey: keep) }
        }
        let target = PrintTarget { ok in close(ok) }
        request.inject = { event in
            // tests: {"t":"print","copies":n,"from":a,"to":b} or {"t":"cancel"}
            guard let e = event as? [String: Any] else { return }
            if e["t"] as? String == "print" {
                if let n = e["copies"] as? Int { set(.copies, n) }
                if let a = e["from"] as? Int, let b = e["to"] as? Int {
                    set(.allPages, false)
                    set(.firstPage, a)
                    set(.lastPage, b)
                }
                close(true)
            } else {
                close(false)
            }
            if let sheet = owner.attachedSheet { owner.endSheet(sheet) }
        }
        request.query = { result() }
        // the panel doesn't keep its delegate
        keep = ObjectIdentifier(target)
        retained[ObjectIdentifier(target)] = [target, panel, info] as NSArray
        panel.beginSheet(with: info, modalFor: owner, delegate: target,
                         didEnd: #selector(PrintTarget.printPanelDidEnd(_:returnCode:contextInfo:)),
                         contextInfo: nil)
    }
}

/// What the font panel and its accessory show; the chosen face comes from the
/// panel (FontTarget), the effects from the accessory.
final class FontModel: ObservableObject {
    @Published var font: NSFont
    @Published var underline: Bool
    @Published var strikeout: Bool
    @Published var color: Color
    let effects: Bool
    let fixedOnly: Bool
    let minSize: Double
    let maxSize: Double
    let okTitle: String
    let cancelTitle: String
    let underlineTitle: String
    let strikeoutTitle: String
    let colorTitle: String
    let fixedNote: String

    init(_ p: [String: Any]) {
        let size = CGFloat(max(1, (p["size"] as? NSNumber)?.doubleValue ?? 12))
        let manager = NSFontManager.shared
        var traits: NSFontTraitMask = []
        if p["italic"] as? Bool == true { traits.insert(.italicFontMask) }
        let weight = FontModel.appKitWeight(p["weight"] as? Int ?? 400)
        font = (p["family"] as? String).flatMap { manager.font(withFamily: $0, traits: traits, weight: weight, size: size) }
            ?? NSFont.systemFont(ofSize: size)
        underline = p["underline"] as? Bool ?? false
        strikeout = p["strikeout"] as? Bool ?? false
        color = Color(nsColor: Requests.rgbColor(p["color"]))
        effects = p["effects"] as? Bool ?? false
        fixedOnly = p["fixedOnly"] as? Bool ?? false
        minSize = (p["minSize"] as? NSNumber)?.doubleValue ?? 0
        maxSize = (p["maxSize"] as? NSNumber)?.doubleValue ?? 0
        okTitle = p["ok"] as? String ?? "OK"
        cancelTitle = p["cancel"] as? String ?? "Cancel"
        underlineTitle = p["underlineTitle"] as? String ?? "Underline"
        strikeoutTitle = p["strikeoutTitle"] as? String ?? "Strikeout"
        colorTitle = p["colorTitle"] as? String ?? "Color:"
        fixedNote = p["fixedNote"] as? String ?? "Choose a fixed-width font."
    }

    /// GDI weights (100-900) <-> NSFontManager's 0-15 scale (5 regular, 9 bold)
    static func appKitWeight(_ gdi: Int) -> Int {
        switch gdi {
        case ..<150: return 2
        case ..<250: return 3
        case ..<350: return 4
        case ..<450: return 5
        case ..<550: return 6
        case ..<650: return 8
        case ..<750: return 9
        case ..<850: return 11
        default: return 12
        }
    }

    static func gdiWeight(_ appKit: Int) -> Int {
        switch appKit {
        case ...2: return 100
        case 3: return 200
        case 4: return 300
        case 5: return 400
        case 6: return 500
        case 7, 8: return 600
        case 9, 10: return 700
        case 11: return 800
        default: return 900
        }
    }

    var sizeOK: Bool {
        let size = Double(font.pointSize)
        return (minSize <= 0 || size >= minSize) && (maxSize <= 0 || size <= maxSize)
    }

    var valid: Bool { (!fixedOnly || font.isFixedPitch) && sizeOK }

    var note: String {
        if fixedOnly && !font.isFixedPitch { return fixedNote }
        if !sizeOK { return "\(Int(minSize))–\(Int(maxSize)) pt" }
        return ""
    }

    func result() -> [String: Any] {
        let manager = NSFontManager.shared
        return ["family": font.familyName ?? font.fontName,
                "face": font.fontDescriptor.object(forKey: .face) as? String ?? "",
                "postscript": font.fontName,
                "size": Double(font.pointSize),
                "weight": FontModel.gdiWeight(manager.weight(of: font)),
                "italic": manager.traits(of: font).contains(.italicFontMask),
                "fixed": font.isFixedPitch,
                "underline": underline, "strikeout": strikeout,
                "color": Requests.rgbComponents(NSColor(color))]
    }
}

/// The font panel's choices come here (NSFontManager's target), not to
/// whatever wine window is first responder.
final class FontTarget: NSObject {
    let model: FontModel
    init(model: FontModel) { self.model = model }

    @objc func changeFont(_ sender: Any?) {
        guard let manager = sender as? NSFontManager else { return }
        model.font = manager.convert(model.font)
    }

    /// The panel's own effects stay off: the accessory has the ones ChooseFont knows.
    @objc func validModesForFontPanel(_ fontPanel: NSFontPanel) -> NSFontPanel.ModeMask {
        [.face, .size, .collection]
    }
}

struct FontAccessory: View {
    @ObservedObject var model: FontModel
    let ok: () -> Void
    let cancel: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            // the sample, as the Windows dialog has one
            Text(model.font.displayName ?? model.font.fontName)
                .font(Font(model.font as CTFont))
                .underline(model.effects && model.underline)
                .strikethrough(model.effects && model.strikeout)
                .foregroundColor(model.effects ? model.color : .primary)
                .lineLimit(1)
                .frame(maxWidth: .infinity, minHeight: 28, alignment: .leading)
            if model.effects {
                HStack(spacing: 12) {
                    Toggle(stripMnemonic(model.underlineTitle), isOn: $model.underline).toggleStyle(.checkbox)
                    Toggle(stripMnemonic(model.strikeoutTitle), isOn: $model.strikeout).toggleStyle(.checkbox)
                    Spacer()
                    ColorPicker(stripMnemonic(model.colorTitle), selection: $model.color, supportsOpacity: false)
                }
            }
            OKCancel(okTitle: model.okTitle, cancelTitle: model.cancelTitle, okEnabled: model.valid,
                     note: model.note, ok: ok, cancel: cancel)
        }
    }
}
