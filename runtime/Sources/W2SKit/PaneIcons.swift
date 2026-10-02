// A settings pane's icon (SettingsToolbar): a Mac settings window's toolbar has
// an icon on each pane's button (HIG, Settings), a Win32 app's panes only titles.
import AppKit

enum PaneIcons {
    /// a pane's name in any language macOS has -> its SF Symbol (tools/gen_pane_icons.py)
    private static let table: [String: String] = {
        var out: [String: String] = [:]
        for line in PaneIconTable.words.split(separator: "\n") {
            let parts = line.split(separator: "\t")
            if parts.count == 2 { out[String(parts[0])] = String(parts[1]) }
        }
        return out
    }()

    /// words that name nothing on their own ("Intégration avec le bureau": avec, le)
    private static let small: Set<String> = [
        "a", "d", "de", "des", "du", "en", "et", "l", "la", "le", "les", "par", "pour", "avec", "sur",
        "the", "of", "and", "for", "to", "in", "on", "by", "with", "an",
        "der", "die", "das", "und", "von", "fur", "mit", "el", "los", "las", "y", "del", "con", "di", "e", "il", "lo",
        "gli", "per", "da", "do", "dos", "das", "o", "os", "van", "het", "en", "och", "og",
    ]

    /// as the table's words: lower case, no accents, no trailing dots or colon
    static func fold(_ s: String) -> String {
        var out = s.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: nil)
            .replacingOccurrences(of: "\u{2019}", with: "'").trimmingCharacters(in: .whitespaces)
        while let last = out.last, "….: ".contains(last) { out.removeLast() }
        return out
    }

    private static func singular(_ w: String) -> String {
        if w.hasSuffix("ies"), w.count > 4 { return String(w.dropLast(3)) + "y" }
        if w.hasSuffix("s"), w.count > 3 { return String(w.dropLast()) }
        return w
    }

    /// The pane's title as a whole, then its words in order, through the table; then
    /// Apple's own search index of SF Symbols (as the SF Symbols app searches it) for
    /// the whole title, one character shorter at a time down to four ("Coloration"
    /// -> ... "Color"); else a settings pane's generic symbol.
    static func symbol(for title: String, mnemonics: Bool = true) -> String {
        let folded = fold(mnemonics ? stripMnemonic(title) : title)
        let words = folded.split(whereSeparator: { !$0.isLetter && !$0.isNumber && $0 != "-" }).map(String.init)
            .filter { !small.contains($0) && !$0.allSatisfy(\.isNumber) }
        var candidates = [folded]
        for w in words { candidates += [w, singular(w)] }
        for c in candidates {
            if let s = table[c], NSImage(systemSymbolName: s, accessibilityDescription: nil) != nil { return s }
        }
        var prefix = folded
        while prefix.count >= 4 {
            if let s = SymbolIndex.first(prefix.trimmingCharacters(in: .whitespaces)) { return s }
            prefix.removeLast()
        }
        return "slider.horizontal.3"
    }
}

/// Apple's search terms for SF Symbols (CoreGlyphs, as the SF Symbols app has them)
private enum SymbolIndex {
    private static let resources = "/System/Library/CoreServices/CoreGlyphs.bundle/Contents/Resources/"
    private static let terms: [String: [String]] =
        NSDictionary(contentsOfFile: resources + "symbol_search.plist") as? [String: [String]] ?? [:]
    private static let order: [String: Int] = {
        let names = NSArray(contentsOfFile: resources + "symbol_order.plist") as? [String] ?? []
        return Dictionary(names.enumerated().map { ($1, $0) }, uniquingKeysWith: { a, _ in a })
    }()

    /// the symbol of that name, else the first (in the app's order) whose terms have the word
    static func first(_ word: String) -> String? {
        if NSImage(systemSymbolName: word, accessibilityDescription: nil) != nil { return word }
        return terms.filter { $0.value.contains { $0.lowercased() == word } }.keys
            .sorted { (order[$0] ?? .max, $0) < (order[$1] ?? .max, $1) }
            .first { NSImage(systemSymbolName: $0, accessibilityDescription: nil) != nil }
    }
}
