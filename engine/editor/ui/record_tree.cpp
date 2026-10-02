#include "record_tree.h"

#include <algorithm>
#include <utility>

namespace opennova::editor {

const RecordTree::Entry *RecordTree::find(NodeId id) const {
	const auto found = at_.find(id);
	return found == at_.end() ? nullptr : &entries[found->second];
}

const std::vector<size_t> &RecordTree::children_of(NodeId owner) const {
	static const std::vector<size_t> none;
	if (!owner) return roots;
	const Entry *entry = find(owner);
	return entry ? entry->children : none;
}

bool RecordTree::inside(NodeId record, NodeId ancestor) const {
	for (const Entry *entry = find(record); entry; entry = entry->owner ? find(entry->owner) : nullptr)
		if (entry->address.child == ancestor) return true;
	return false;
}

RecordTree build_record_tree(const Document &document, const Node &row, NodeKind kind) {
	RecordTree tree;
	tree.row = row.id;
	tree.kind = kind;
	document.walk_records(row, [&](const NodeAddress &record, const Document::Placement &at) {
		if (record.kind != kind) return true;
		size_t parent = SIZE_MAX;
		if (at.owner.child) {
			const auto owner = tree.at_.find(at.owner.child);
			if (owner == tree.at_.end()) return true; // held by a record outside the tree
			parent = owner->second;
		}
		RecordTree::Entry entry;
		entry.address = record;
		entry.owner = at.owner.child;
		entry.index = at.index;
		entry.depth = parent == SIZE_MAX ? 0 : tree.entries[parent].depth + 1;
		const size_t index = tree.entries.size();
		tree.at_.emplace(record.child, index);
		tree.entries.push_back(std::move(entry));
		(parent == SIZE_MAX ? tree.roots : tree.entries[parent].children).push_back(index);
		return true;
	});
	return tree;
}

namespace {

Edit move_of(const RecordTree::Entry &record, NodeId parent, size_t position) {
	Edit edit;
	edit.operation = EditOperation::Move;
	edit.address = record.address;
	edit.parent = parent;
	edit.position = position;
	return edit;
}

// The destination owner as a Move names it: the row by its own id.
NodeId owner_id(const RecordTree &tree, NodeId owner) { return owner ? owner : tree.row; }

} // namespace

bool drop_edit(const RecordTree &tree, NodeId dragged, NodeId target, DropPlace place, Edit &out) {
	const RecordTree::Entry *record = tree.find(dragged);
	const RecordTree::Entry *on = tree.find(target);
	if (!record || !on || tree.inside(target, dragged)) return false;
	if (place == DropPlace::Inside) {
		out = move_of(*record, target, SIZE_MAX);
		return true;
	}
	// The position counts the record out of its list first: past it in the same list, the
	// target's index is one less by the time the record goes back in.
	size_t position = on->index + (place == DropPlace::After ? 1 : 0);
	if (record->owner == on->owner && record->index < on->index) --position;
	out = move_of(*record, owner_id(tree, on->owner), position);
	return true;
}

bool indent_edit(const RecordTree &tree, NodeId record, Edit &out) {
	const RecordTree::Entry *entry = tree.find(record);
	if (!entry) return false;
	const std::vector<size_t> &siblings = tree.children_of(entry->owner);
	const auto self = std::find_if(siblings.begin(), siblings.end(),
	                               [&](size_t index) { return tree.entries[index].address.child == record; });
	if (self == siblings.end() || self == siblings.begin()) return false;
	out = move_of(*entry, tree.entries[*(self - 1)].address.child, SIZE_MAX);
	return true;
}

bool outdent_edit(const RecordTree &tree, NodeId record, Edit &out) {
	const RecordTree::Entry *entry = tree.find(record);
	if (!entry || !entry->owner) return false;
	const RecordTree::Entry *owner = tree.find(entry->owner);
	if (!owner) return false;
	out = move_of(*entry, owner_id(tree, owner->owner), owner->index + 1);
	return true;
}

} // namespace opennova::editor
