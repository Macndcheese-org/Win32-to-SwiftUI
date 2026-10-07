// A window's sidebar of several pages (map: tab, treeview, listview...): HTML Help's navigation pane,
// a tab control of Contents, Index and Search along the window's leading edge. The sidebar shows the
// controls on the chosen page under a segmented control, which is the Mac's switch between views in a
// sidebar (HIG: Segmented controls), instead of the app's tabs.
import AppKit
import SwiftUI

final class SidebarGroup: ObservableObject {
    struct Member: Identifiable {
        let host: ControlHost
        var page: Int
        var order: Int
        var foot = false
        var id: UInt64 { host.handle }
    }

    private static var groups: [Int: SidebarGroup] = [:]

    /// the group of the pane with this id (the app's window that holds the tabs), made at its first control
    static func group(_ id: Int) -> SidebarGroup {
        if let group = groups[id] { return group }
        let group = SidebarGroup(id: id)
        groups[id] = group
        return group
    }

    let id: Int
    @Published var tabs: [String] = []
    @Published var tab = 0
    @Published var members: [Member] = []
    /// how far the title bar and toolbar reach down over the sidebar (the content starts below)
    @Published var topInset: CGFloat = 0
    /// the window's sidebar, made by the first control of the group
    var sidebar: FrameSidebar?
    private weak var primary: ControlHost?
    private var chosen: Int?
    private var chosenAt = Date.distantPast

    init(id: Int) { self.id = id }

    /// A control of the pages shows: it is on its page, and the sidebar shows it there.
    func join(_ host: ControlHost, _ snap: Snapshot) {
        if primary == nil { primary = host }
        host.hosting?.isHidden = true       // the sidebar shows it, not the window
        update(host, snap)
    }

    func update(_ host: ControlHost, _ snap: Snapshot) {
        if let titles = snap.sbTabs, titles != tabs { tabs = titles }
        // the app's tab, unless the user has just chosen another and the app has not caught up
        if let current = snap.sbTab {
            if let want = chosen, Date().timeIntervalSince(chosenAt) < 3 {
                if current == want { chosen = nil; if tab != current { tab = current } }
            } else if current != tab {
                chosen = nil
                tab = current
            }
        }
        let member = Member(host: host, page: snap.sbPage ?? 0, order: snap.sbOrder ?? 0, foot: snap.sbFoot ?? false)
        if let index = members.firstIndex(where: { $0.id == host.handle }) {
            if members[index].page != member.page || members[index].order != member.order || members[index].foot != member.foot {
                members[index] = member
            }
        } else {
            members.append(member)
        }
    }

    func leave(_ host: ControlHost) {
        members.removeAll { $0.id == host.handle }
        if members.isEmpty { SidebarGroup.groups.removeValue(forKey: id) }
    }

    /// the user chose a page: the app's tab changes as a click on it would, and its controls show
    func choose(_ page: Int) {
        guard page != tab else { return }
        tab = page
        chosen = page
        chosenAt = Date()
        primary?.model.emit(["t": "sidebarTab", "v": page])
    }
}

/// The sidebar's content: the switch between the pages and the controls on the chosen one.
@available(macOS 13.0, *)
struct SidebarGroupView: View {
    @ObservedObject var group: SidebarGroup

    var body: some View {
        VStack(spacing: 8) {
            if group.tabs.count > 1 {
                // the names while they fit, their symbols when the sidebar is narrow (HIG: Segmented controls)
                ViewThatFits(in: .horizontal) {
                    switcher(symbols: false)
                    if group.tabs.allSatisfy({ tabSymbol($0) != nil }) { switcher(symbols: true) }
                }
                .padding(.horizontal, 12)
                .padding(.top, 4)
            }
            let page = group.members.filter { $0.page == group.tab }.sorted { $0.order < $1.order }
            if let search = SidebarSearch(page: page.map(\.host)) {
                // a page of a field, a button and a list is a search: the Mac's search field and its results
                SidebarSearchPage(search: search,
                                  prompt: group.tabs.indices.contains(group.tab) ? group.tabs[group.tab] : "")
            } else {
                ForEach(page.filter { !$0.foot }) { member in SidebarMember(host: member.host) }
            }
            Spacer(minLength: 0)
            // what the app put under its tree (About, Help), kept within reach at the sidebar's foot
            let foot = group.members.filter { $0.foot }.sorted { $0.order < $1.order }
            if !foot.isEmpty {
                HStack(spacing: 8) {
                    ForEach(foot) { member in
                        ControlRoot(model: member.host.model, entry: member.host.entry,
                                    fixed: FormMetrics.metrics(scale: member.host.scale))
                            .environment(\.w2sInSidebar, true)
                            .frame(height: 24)
                    }
                }
                .padding(.horizontal, 12)
                .padding(.bottom, 10)
            }
        }
    }

    private func switcher(symbols: Bool) -> some View {
        Picker("", selection: Binding(get: { group.tab }, set: { group.choose($0) })) {
            ForEach(Array(group.tabs.enumerated()), id: \.offset) { index, title in
                if symbols, let name = tabSymbol(title) {
                    Image(systemName: name).help(title).accessibilityLabel(title).tag(index)
                } else {
                    Text(title).lineLimit(1).tag(index)
                }
            }
        }
        .pickerStyle(.segmented)
        .labelsHidden()
        .controlSize(.small)
    }
}

/// The symbol of a page by what its name says, in the languages the Mac has (a help viewer's Contents,
/// Index and Search); nil for a name it doesn't know, which keeps the names.
func tabSymbol(_ title: String) -> String? {
    let name = title.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: nil)
    func has(_ words: [String]) -> Bool { words.contains { name.contains($0) } }
    if has(["somm", "content", "inhalt", "indice general", "contenido", "contenu", "contenuti", "inhoud", "indhold", "innhold", "innehall", "toc"]) { return "book" }
    if has(["index", "indice", "register", "verzeichnis", "stikord"]) { return "character.book.closed" }
    if has(["rech", "search", "such", "busc", "cerc", "zoek", "sog", "sok", "etsi"]) { return "magnifyingglass" }
    return nil
}

/// One control of a page in the sidebar: a field or a button its own height, a list the rest.
struct SidebarMember: View {
    let host: ControlHost

    var body: some View {
        let view = ControlRoot(model: host.model, entry: host.entry, fixed: FormMetrics.metrics(scale: host.scale))
            .environment(\.w2sInSidebar, true)
        if host.entry.hasPrefix("edit.") || host.entry.hasPrefix("button.") {
            view.frame(height: 24).padding(.horizontal, 10)
        } else if host.entry.hasPrefix("listview."), (host.model.snap.columns?.count ?? 1) <= 1 {
            // a list of one column in the sidebar is the sidebar's list: on its material, not a white table
            ListBoxView(model: host.model, multi: false, fromRows: true).environment(\.w2sInSidebar, true)
        } else {
            view
        }
    }
}

/// The sidebar's hosting controller: tells the group when the sidebar lays out, which is when its safe area
/// (the title bar and toolbar over its top) changes.
final class SidebarHostingController: NSHostingController<AnyView> {
    weak var group: SidebarGroup?

    override func viewDidLayout() {
        super.viewDidLayout()
        guard let group = group else { return }
        var inset = view.safeAreaInsets.top
        if let window = view.window { inset = max(inset, window.frame.height - window.contentLayoutRect.maxY) }
        if abs(group.topInset - inset) > 0.5 { DispatchQueue.main.async { group.topInset = inset } }
    }
}

/// The controls of a search page: its field, the button that lists the results, and the list.
struct SidebarSearch {
    let field: ControlHost
    let button: ControlHost
    let results: ControlHost

    init?(page: [ControlHost]) {
        guard let field = page.first(where: { $0.entry.hasPrefix("edit.") }),
              let button = page.first(where: { $0.entry.hasPrefix("button.") }),
              let results = page.first(where: { $0.entry.hasPrefix("listview.") || $0.entry.hasPrefix("listbox.") })
        else { return nil }
        self.field = field
        self.button = button
        self.results = results
    }
}

/// A search as the Mac has it (HIG: Search fields): a field with the magnifier, a clear button and the name
/// of what is searched; Return lists the results, which are the list below, with nothing in it until then.
struct SidebarSearchPage: View {
    let search: SidebarSearch
    let prompt: String
    @ObservedObject private var field: ControlModel
    @ObservedObject private var results: ControlModel

    init(search: SidebarSearch, prompt: String) {
        self.search = search
        self.prompt = prompt
        field = search.field.model
        results = search.results.model
    }

    var body: some View {
        let empty = (results.snap.rows ?? []).isEmpty && (results.snap.items ?? []).isEmpty
        VStack(spacing: 8) {
            SearchFieldView(
                text: Binding(
                    get: { field.snap.text ?? "" },
                    set: { new in
                        guard new != field.snap.text else { return }
                        field.snap.text = new
                        field.emit(["t": "text", "s": new])
                    }),
                prompt: prompt,
                onFocus: { field.emit(["t": "focus"]) },
                onSubmit: { search.button.model.emit(["t": "click"]) })
                .frame(height: 24)
                .padding(.horizontal, 12)
                .padding(.top, 2)
            // no results: nothing, not a list of empty rows
            if !empty { SidebarMember(host: search.results) }
        }
    }
}

/// NSSearchField: SwiftUI's searchable is for a navigation stack's toolbar.
struct SearchFieldView: NSViewRepresentable {
    @Binding var text: String
    let prompt: String
    let onFocus: () -> Void
    let onSubmit: () -> Void

    /// the app's keyboard focus follows the field's (Return, and the keys a Win32 control takes, go to the app's)
    final class Field: NSSearchField {
        var onFocus: (() -> Void)?
        override func becomeFirstResponder() -> Bool {
            let ok = super.becomeFirstResponder()
            if ok { onFocus?() }
            return ok
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSSearchField {
        let field = Field()
        field.onFocus = onFocus
        field.placeholderString = prompt
        field.delegate = context.coordinator
        field.target = context.coordinator
        field.action = #selector(Coordinator.submitted(_:))
        field.sendsSearchStringImmediately = false
        field.sendsWholeSearchString = true
        return field
    }

    func updateNSView(_ field: NSSearchField, context: Context) {
        context.coordinator.parent = self
        (field as? Field)?.onFocus = onFocus
        if field.stringValue != text { field.stringValue = text }
        if field.placeholderString != prompt { field.placeholderString = prompt }
    }

    final class Coordinator: NSObject, NSSearchFieldDelegate {
        var parent: SearchFieldView
        init(_ parent: SearchFieldView) { self.parent = parent }

        func controlTextDidChange(_ note: Notification) {
            if let field = note.object as? NSSearchField { parent.text = field.stringValue }
        }

        @objc func submitted(_ field: NSSearchField) {
            parent.text = field.stringValue
            parent.onSubmit()
        }
    }
}
