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
        let member = Member(host: host, page: snap.sbPage ?? 0, order: snap.sbOrder ?? 0)
        if let index = members.firstIndex(where: { $0.id == host.handle }) {
            if members[index].page != member.page || members[index].order != member.order { members[index] = member }
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
struct SidebarGroupView: View {
    @ObservedObject var group: SidebarGroup

    var body: some View {
        VStack(spacing: 8) {
            if group.tabs.count > 1 {
                Picker("", selection: Binding(get: { group.tab }, set: { group.choose($0) })) {
                    ForEach(Array(group.tabs.enumerated()), id: \.offset) { index, title in
                        Text(title).tag(index)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .padding(.horizontal, 10)
                .padding(.top, 8)
            }
            ForEach(group.members.filter { $0.page == group.tab }.sorted { $0.order < $1.order }) { member in
                SidebarMember(host: member.host)
            }
            Spacer(minLength: 0)
        }
        .padding(.top, group.topInset)
    }
}

/// One control of a page in the sidebar: a field or a button its own height, a list the rest.
struct SidebarMember: View {
    let host: ControlHost

    var body: some View {
        let view = ControlRoot(model: host.model, entry: host.entry, fixed: FormMetrics.metrics(scale: host.scale))
            .environment(\.w2sInSidebar, true)
        if host.entry.hasPrefix("edit.") || host.entry.hasPrefix("button.") {
            view.frame(height: 24).padding(.horizontal, 10)
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
