import Foundation

/// A SwiftUI outline (List with DisclosureGroup rows) closes a group by walking the
/// rows it had open and asking its data for their children, and traps ("Fatal
/// error" in TableViewListCore) when the data in the same update has lost them.
/// When the user closes a node, TreeRows closes it first and the snapshot that
/// drops its children comes later, so the two never meet; when the app closes
/// it (TVM_EXPAND, or a folder tree that follows its list), one snapshot does both.
/// That snapshot is shown in two steps: the node closed with the children it had,
/// then, an update later, without them.
enum TreeStaging {
    /// `new` with, under every node it closes, the children the node had open in
    /// `old`; nil when no node closes with children going.
    static func stage(old: [Snapshot.TreeNode]?, new: [Snapshot.TreeNode]?) -> [Snapshot.TreeNode]? {
        guard let old = old, let new = new else { return nil }
        var before: [Int: Snapshot.TreeNode] = [:]
        index(old, into: &before)
        var now = Set<Int>()
        collect(new, into: &now)

        var changed = false
        func walk(_ nodes: [Snapshot.TreeNode]) -> [Snapshot.TreeNode] {
            nodes.map { node in
                var node = node
                if let was = before[node.id], was.open ?? false, !(node.open ?? false),
                   let kept = was.children, !kept.isEmpty, (node.children ?? []).isEmpty {
                    // a row that would show twice in the list is no row to keep
                    var ids = Set<Int>()
                    collect(kept, into: &ids)
                    if ids.isDisjoint(with: now) {
                        node.children = kept
                        changed = true
                    }
                } else if let children = node.children {
                    node.children = walk(children)
                }
                return node
            }
        }
        let result = walk(new)
        return changed ? result : nil
    }

    /// The updates that show `new` after `old` without the list ever closing a node that has
    /// open nodes below it: those close first, the deepest first, a step each; then the node
    /// itself, with its children still there. Empty when no node closes with children going.
    static func steps(old: [Snapshot.TreeNode]?, new: [Snapshot.TreeNode]?) -> [[Snapshot.TreeNode]] {
        guard let staged = stage(old: old, new: new) else { return [] }
        var deepest = 0, shallowest = Int.max
        func scan(_ nodes: [Snapshot.TreeNode], depth: Int, kept: Bool) {
            for node in nodes {
                if kept, node.open ?? false {
                    deepest = max(deepest, depth)
                    shallowest = min(shallowest, depth)
                }
                let closing = !kept && !(node.open ?? false) && !(node.children ?? []).isEmpty
                if let children = node.children { scan(children, depth: depth + 1, kept: kept || closing) }
            }
        }
        scan(staged, depth: 0, kept: false)

        /// `staged` with the nodes kept below a closing node closed from `level` down; the
        /// closing nodes themselves open until `last`
        func build(_ nodes: [Snapshot.TreeNode], depth: Int, kept: Bool, level: Int, last: Bool) -> [Snapshot.TreeNode] {
            nodes.map { node in
                var node = node
                let closing = !kept && !(node.open ?? false) && !(node.children ?? []).isEmpty
                if closing { node.open = !last }
                if kept, depth >= level { node.open = false }
                if let children = node.children {
                    node.children = build(children, depth: depth + 1, kept: kept || closing, level: level, last: last)
                }
                return node
            }
        }
        var result: [[Snapshot.TreeNode]] = []
        if deepest > 0 {
            for level in stride(from: deepest, through: shallowest, by: -1) {
                result.append(build(staged, depth: 0, kept: false, level: level, last: false))
            }
        }
        result.append(build(staged, depth: 0, kept: false, level: 0, last: true))
        return result
    }

    private static func index(_ nodes: [Snapshot.TreeNode], into map: inout [Int: Snapshot.TreeNode]) {
        for node in nodes {
            map[node.id] = node
            if let children = node.children { index(children, into: &map) }
        }
    }

    private static func collect(_ nodes: [Snapshot.TreeNode], into ids: inout Set<Int>) {
        for node in nodes {
            ids.insert(node.id)
            if let children = node.children { collect(children, into: &ids) }
        }
    }
}
