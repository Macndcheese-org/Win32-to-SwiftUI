// Stock icons (map: 75-icons.yaml). The PE side recognises wine's own icons
// (icons.c) and sends, instead of their Windows artwork, a spec for the macOS
// image with the same meaning: "kind:value[;option]".
//   sf:<SF Symbol>[;multicolor | ;palette=<colour>,<colour>...]
//   uttype:<identifier>   Finder's icon for that kind of file
//   file:<path>           Finder's icon for that file or folder (~ is home)
//   nsimage:<name>        an AppKit image (NSImage.Name)
//   app:<bundle id>       that application's icon
import AppKit
import UniformTypeIdentifiers

enum Icons {
    private static var cache: [String: NSImage] = [:]

    /// The image for a spec at a size in points (main thread); nil for a spec
    /// this Mac can't show, which leaves the place empty rather than Windows-like.
    static func image(_ spec: String, size: NSSize) -> NSImage? {
        let key = "\(spec)@\(Int(size.width))x\(Int(size.height))"
        if let image = cache[key] { return image }
        guard let image = make(spec, size: size) else { return nil }
        cache[key] = image
        return image
    }

    private static func make(_ spec: String, size: NSSize) -> NSImage? {
        let parts = spec.split(separator: ";", maxSplits: 1).map(String.init)
        guard let colon = parts[0].firstIndex(of: ":") else { return nil }
        let kind = String(parts[0][..<colon])
        let value = String(parts[0][parts[0].index(after: colon)...])
        let option = parts.count > 1 ? parts[1] : ""
        var image: NSImage?

        switch kind {
        case "sf":
            // a symbol sits inside its em square with some room: this fills an icon's box as Windows' did
            let config = NSImage.SymbolConfiguration(pointSize: max(size.height * 0.8, 8), weight: .regular)
            guard let symbol = NSImage(systemSymbolName: value, accessibilityDescription: nil) else { return nil }
            let colours: NSImage.SymbolConfiguration
            if option == "multicolor" {
                colours = .preferringMulticolor()
            } else if option.hasPrefix("palette=") {
                colours = NSImage.SymbolConfiguration(paletteColors:
                    option.dropFirst("palette=".count).split(separator: ",").map { colour(String($0)) })
            } else {
                colours = NSImage.SymbolConfiguration(hierarchicalColor: .controlAccentColor)
            }
            return symbol.withSymbolConfiguration(config.applying(colours))
        case "uttype":
            guard let type = UTType(value) else { return nil }
            image = NSWorkspace.shared.icon(for: type)
        case "file":
            let path = (value as NSString).expandingTildeInPath
            guard FileManager.default.fileExists(atPath: path) else { return nil }
            image = NSWorkspace.shared.icon(forFile: path)
        case "nsimage":
            image = NSImage(named: NSImage.Name(value))
        case "app":
            guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: value) else { return nil }
            image = NSWorkspace.shared.icon(forFile: url.path)
        default:
            return nil
        }
        // shared images: size a copy
        guard let copy = image?.copy() as? NSImage else { return nil }
        copy.size = size
        return copy
    }

    private static func colour(_ name: String) -> NSColor {
        switch name {
        case "white": return .white
        case "black": return .black
        case "systemBlue": return .systemBlue
        case "systemRed": return .systemRed
        case "systemYellow": return .systemYellow
        case "systemOrange": return .systemOrange
        case "systemGreen": return .systemGreen
        case "systemGray": return .systemGray
        case "labelColor": return .labelColor
        case "secondaryLabelColor": return .secondaryLabelColor
        default: return .controlAccentColor
        }
    }
}
