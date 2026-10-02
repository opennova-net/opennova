#include "table_document.h"

#include <algorithm>

namespace opennova::editor {

void TableRow::for_each_identity(const std::function<void(NodeId &)> &fn) {
	for (std::vector<RecordIds> &list : ids.lists)
		for (RecordIds &entry : list) editor::for_each_identity(entry, fn);
}

// --- finding a record ----------------------------------------------------------------------------

bool TableDocument::descend(const PathStep *steps, size_t size, Located &out) const {
	for (size_t i = 0; i < size; ++i) {
		const TableKind *kind = table().kind(out.record.kind);
		const size_t list = steps[i].collection, index = steps[i].index;
		if (!kind || list >= kind->lists().size() || list >= out.ids->lists.size() ||
		    index >= out.ids->lists[list].size())
			return false;
		const ListOps &ops = kind->lists()[list].ops;
		const RecordHandle next = ops.at(out.record, index);
		if (!next) return false;
		out.present = out.present && (!ops.present || ops.present(out.record));
		out.trail.push_back({out.record, list, index});
		out.owner_ids.push_back(out.ids);
		out.ids = &out.ids->lists[list][index];
		out.record = next;
	}
	return true;
}

bool TableDocument::locate(const Node &node, NodeId record, Located &out) const {
	const TableRow &row = static_cast<const TableRow &>(node);
	out = Located();
	out.record = row.record();
	out.ids = const_cast<RecordIds *>(&row.ids);
	if (!out.record) return false;
	if (!record || record == node.id) return true;
	const RecordPath path = path_in(node, record);
	return !path.empty() && descend(path.begin(), path.size(), out);
}

TableDocument::Located TableDocument::owner_of(const Located &record) const {
	Located owner;
	if (record.trail.empty()) return owner;
	owner.trail.assign(record.trail.begin(), record.trail.end() - 1);
	owner.owner_ids.assign(record.owner_ids.begin(), record.owner_ids.end() - 1);
	owner.record = record.step().owner;
	owner.ids = record.step_ids();
	// Written while every list above the owner is: the trail's own lists, the owner's excepted.
	for (const OwnerStep &step : owner.trail) {
		const TableKind *kind = table().kind(step.owner.kind);
		const ListOps *ops = kind && step.list < kind->lists().size() ? &kind->lists()[step.list].ops : nullptr;
		owner.present = owner.present && (!ops || !ops->present || ops->present(step.owner));
	}
	return owner;
}

RecordHandle TableDocument::record_in(const Node &row, const NodeAddress &address) const {
	Located at;
	if (!locate(row, address.child, at) || at.record.kind != address.kind) return {};
	return at.record;
}

bool TableDocument::list_of(const Located &owner, NodeKind kind, size_t &list) const {
	const TableKind *held = table().kind(owner.record.kind);
	if (!held) return false;
	for (size_t i = 0; i < held->lists().size(); ++i)
		if (held->lists()[i].holds(kind)) {
			list = i;
			return true;
		}
	return false;
}

// --- the declarations ------------------------------------------------------------------------------

std::vector<Document::Collection> TableDocument::collections(const Node &row, const NodeAddress &owner) const {
	Located at;
	if (!locate(row, owner.child, at)) return {};
	const TableKind *kind = table().kind(at.record.kind);
	if (!kind) return {};
	std::vector<Collection> out;
	out.reserve(kind->lists().size());
	for (size_t l = 0; l < kind->lists().size(); ++l) {
		const TableList &list = kind->lists()[l];
		Collection collection{list.spec, {}, {}};
		collection.spec.applies = list_applies(row, at, l);
		if (!list.kinds.empty()) {
			collection.spec.kinds = list.kinds.data();
			collection.spec.kind_count = list.kinds.size();
		}
		if (l < at.ids->lists.size()) {
			for (const RecordIds &ids : at.ids->lists[l]) collection.ids.push_back(ids.id);
			// A list of several kinds names each record's own (the kind its handle says).
			if (!list.kinds.empty())
				for (size_t i = 0; i < collection.ids.size(); ++i)
					collection.kinds.push_back(list.ops.at(at.record, i).kind);
		}
		out.push_back(std::move(collection));
	}
	return out;
}

bool TableDocument::walk(const Node &row, Located &owner, const NodeAddress &self, const RecordVisitor &visit) const {
	const TableKind *kind = table().kind(owner.record.kind);
	if (!kind) return true;
	for (size_t l = 0; l < kind->lists().size() && l < owner.ids->lists.size(); ++l) {
		const TableList &list = kind->lists()[l];
		CollectionSpec spec = list.spec;
		spec.applies = list_applies(row, owner, l);
		if (!list.kinds.empty()) {
			spec.kinds = list.kinds.data();
			spec.kind_count = list.kinds.size();
		}
		const bool written = owner.present && (!list.ops.present || list.ops.present(owner.record));
		std::vector<RecordIds> &held = owner.ids->lists[l];
		for (size_t i = 0; i < held.size(); ++i) {
			const RecordHandle child = list.ops.at(owner.record, i);
			if (!child || !list.holds(child.kind)) return false;
			// A list of several kinds places each record as the kind its handle says.
			spec.kind = child.kind;
			const NodeAddress address{row.id, child.kind, held[i].id};
			if (!visit(address, Placement{self, spec, i, l})) return false;
			// Down one step and back: the walk keeps one trail.
			const RecordHandle record = owner.record;
			RecordIds *ids = owner.ids;
			const bool present = owner.present;
			owner.trail.push_back({record, l, i});
			owner.owner_ids.push_back(ids);
			owner.record = child;
			owner.ids = &held[i];
			owner.present = written;
			const bool more = walk(row, owner, address, visit);
			owner.trail.pop_back();
			owner.owner_ids.pop_back();
			owner.record = record;
			owner.ids = ids;
			owner.present = present;
			if (!more) return false;
		}
	}
	return true;
}

void TableDocument::walk_records(const Node &row, const RecordVisitor &visit) const {
	Located at;
	if (!locate(row, 0, at)) return;
	walk(row, at, {row.id, row.kind, 0}, visit);
}

Applicability TableDocument::list_applies(const Node &, const Located &owner, size_t list) const {
	const TableKind *kind = table().kind(owner.record.kind);
	return kind && list < kind->lists().size() ? kind->lists()[list].spec.applies : Applicability::Reads;
}

void TableDocument::shape(TableRow &row) const {
	row.ids = shape_ids(table(), row.record());
	row.ids.id = 0;
}

// --- the fields ----------------------------------------------------------------------------------

void TableDocument::count_field(const FieldSchema &field) const {
	++stats_.resolved;
	if (stats_.several || std::find(stats_.distinct.begin(), stats_.distinct.end(), &field) != stats_.distinct.end())
		return;
	if (stats_.distinct.size() < TableStats::kDistinctKept) stats_.distinct.push_back(&field);
	else stats_.several = true;
}

const TableKind *TableDocument::resolve_kind(NodeKind kind) const {
	++stats_.kinds;
	return table().kind(kind);
}

size_t TableDocument::resolve_field(const TableKind &kind, const std::string &id) const {
	++stats_.probes;
	const size_t place = kind.find(id);
	if (place != TableKind::npos) count_field(kind.fields()[place]);
	return place;
}

const FieldSchema *TableDocument::find_field(NodeKind kind, const std::string &id) const {
	const TableKind *held = resolve_kind(kind);
	const size_t place = held ? resolve_field(*held, id) : TableKind::npos;
	return place != TableKind::npos ? &held->fields()[place] : nullptr;
}

bool TableDocument::read(const Node &row, const NodeAddress &address, const std::string &field, Value &out) const {
	Located at;
	if (!locate(row, address.child, at) || at.record.kind != address.kind) return false;
	const TableKind *kind = resolve_kind(address.kind);
	const size_t place = kind ? resolve_field(*kind, field) : TableKind::npos;
	return place != TableKind::npos && kind->value(place).get(at.record, out);
}

bool TableDocument::read_present(const Node &row, const NodeAddress &address, const std::string &field) const {
	Located at;
	if (!locate(row, address.child, at) || at.record.kind != address.kind) return false;
	if (field.empty()) return at.present;
	const TableKind *kind = resolve_kind(address.kind);
	const size_t place = kind ? resolve_field(*kind, field) : TableKind::npos;
	if (place == TableKind::npos) return false;
	const FieldValue &value = kind->value(place);
	return !value.present || value.present(at.record);
}

bool TableDocument::set_value(Node &, const Located &at, size_t field, const Value &value, std::string &error) {
	const FieldValue &own = table().kind(at.record.kind)->value(field);
	if (!own.set) {
		error = "This field is shown only: it is derived, or what the game does with it is not witnessed.";
		return false;
	}
	return own.set(at.record, value, error);
}

bool TableDocument::set_written(Node &, const Located &at, size_t field, bool present, std::string &error) {
	const FieldValue &own = table().kind(at.record.kind)->value(field);
	if (!own.set_present) {
		error = "This field is always written.";
		return false;
	}
	return own.set_present(at.record, present, error);
}

bool TableDocument::set_field(Node &row, const NodeAddress &address, const std::string &field, const Value &value,
                              std::string &error) {
	Located at;
	if (!locate(row, address.child, at) || at.record.kind != address.kind) {
		error = "The record no longer exists.";
		return false;
	}
	const TableKind *kind = resolve_kind(address.kind);
	const size_t place = kind ? resolve_field(*kind, field) : TableKind::npos;
	if (place == TableKind::npos) {
		error = "Unknown field.";
		return false;
	}
	return set_value(row, at, place, value, error);
}

bool TableDocument::set_present(Node &row, const NodeAddress &address, const std::string &field, bool present,
                                std::string &error) {
	Located at;
	if (!locate(row, address.child, at) || at.record.kind != address.kind) {
		error = "The record no longer exists.";
		return false;
	}
	const TableKind *kind = resolve_kind(address.kind);
	const size_t place = kind ? resolve_field(*kind, field) : TableKind::npos;
	if (place == TableKind::npos) {
		error = "Unknown field.";
		return false;
	}
	return set_written(row, at, place, present, error);
}

void TableDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	const TableKind *kind = table().kind(address.kind);
	if (!kind) return;
	// The schema the use points at is the table's own (Document::fields), or a copy of it a caller
	// kept, which its id finds (a probe).
	size_t place = kind->place(use.schema);
	if (place == TableKind::npos) {
		++stats_.probes;
		place = kind->find(use.schema->id);
	}
	if (place == TableKind::npos || (!kind->applies(place) && !kind->reference(place))) return;
	const Node *node = row(address.row);
	Located at;
	if (!node || !locate(*node, address.child, at) || at.record.kind != address.kind) return;
	count_field(kind->fields()[place]);
	const RecordOwners owners = at.owners();
	if (kind->applies(place)) use.applies = kind->applies(place)(at.record, owners);
	if (kind->reference(place)) use.reference = kind->reference(place)(at.record, owners);
}

// --- the list edits --------------------------------------------------------------------------------

bool TableDocument::accept_list_edit(const Node &, const ListChange &, std::string &) const { return true; }

size_t TableDocument::list_position(const Node &, const ListChange &, size_t position) const { return position; }

void TableDocument::prepare_record(const Node &, const ListChange &, DetachedRecord &) const {}

void TableDocument::after_add(Node &, const ListChange &, const RecordHandle &) {}

bool TableDocument::insert_record(const Located &owner, size_t list, size_t position, const DetachedRecord &record,
                                  const IdAllocator &allocate, NodeId &added, std::string &error) {
	const TableKind *kind = table().kind(owner.record.kind);
	if (!kind || list >= kind->lists().size() || list >= owner.ids->lists.size() ||
	    !kind->lists()[list].holds(record.kind)) {
		error = "This record holds no such records.";
		return false;
	}
	const ListOps &ops = kind->lists()[list].ops;
	position = std::min(position, ops.size(owner.record));
	if (!ops.insert(owner.record, position, &record, error)) return false;
	RecordIds fresh = shape_ids(table(), ops.at(owner.record, position));
	editor::for_each_identity(fresh, [&](NodeId &id) { id = allocate(); });
	std::vector<RecordIds> &ids = owner.ids->lists[list];
	ids.insert(ids.begin() + std::ptrdiff_t(std::min(position, ids.size())), std::move(fresh));
	added = ids[position].id;
	return true;
}

bool TableDocument::edit_collection(Node &row, const Edit &edit, const IdAllocator &allocate, NodeId &added,
                                    std::string &error) {
	if (edit.operation == EditOperation::Add) {
		Located owner;
		if (!locate(row, edit.parent, owner)) {
			error = "The record to add into no longer exists.";
			return false;
		}
		size_t list = 0;
		if (!list_of(owner, edit.address.kind, list)) {
			error = "This record holds no such records.";
			return false;
		}
		ListChange change;
		change.operation = edit.operation;
		change.owner = &owner;
		change.list = list;
		if (!accept_list_edit(row, change, error)) return false;
		const TableList &into = table().kind(owner.record.kind)->lists()[list];
		const size_t position = std::min(list_position(row, change, edit.position), into.ops.size(owner.record));
		// A list of one kind makes its new record; a list of several makes one of the kind the Add names.
		const bool made = into.kinds.empty() ? into.ops.insert(owner.record, position, nullptr, error)
		                                     : into.ops.make(owner.record, position, edit.address.kind, error);
		if (!made) return false;
		const RecordHandle record = into.ops.at(owner.record, position);
		after_add(row, change, record);
		RecordIds fresh = shape_ids(table(), record);
		editor::for_each_identity(fresh, [&](NodeId &id) { id = allocate(); });
		std::vector<RecordIds> &ids = owner.ids->lists[list];
		ids.insert(ids.begin() + std::ptrdiff_t(std::min(position, ids.size())), std::move(fresh));
		added = ids[position].id;
		return true;
	}
	Located at;
	if (!locate(row, edit.address.child, at) || at.is_row()) {
		error = "The record no longer exists.";
		return false;
	}
	Located owner = owner_of(at);
	const size_t list = at.step().list, index = at.step().index;
	const ListOps &ops = table().kind(owner.record.kind)->lists()[list].ops;
	std::vector<RecordIds> &source_ids = owner.ids->lists[list];
	ListChange change;
	change.operation = edit.operation;
	change.owner = &owner;
	change.list = list;
	change.record = &at;
	switch (edit.operation) {
	case EditOperation::Duplicate: {
		if (!accept_list_edit(row, change, error)) return false;
		DetachedRecord copy = ops.copy(owner.record, index);
		if (!copy.data) {
			error = "The record no longer exists.";
			return false;
		}
		prepare_record(row, change, copy);
		return insert_record(owner, list, list_position(row, change, edit.position), copy, allocate, added, error);
	}
	case EditOperation::Remove:
		if (!accept_list_edit(row, change, error)) return false;
		if (!ops.erase(owner.record, index)) {
			error = "The list keeps the record: it gives none up here.";
			return false;
		}
		source_ids.erase(source_ids.begin() + std::ptrdiff_t(index));
		return true;
	case EditOperation::Move: {
		// The destination as the edit found it (the owner edit.parent names, 0 = the row), checked
		// with the record before anything moves.
		Located destination;
		if (!locate(row, edit.parent, destination)) {
			error = "The destination no longer exists.";
			return false;
		}
		size_t destination_list = 0;
		if (!list_of(destination, at.record.kind, destination_list)) {
			error = "This record holds no such records.";
			return false;
		}
		change.destination = &destination;
		change.destination_list = destination_list;
		if (!accept_list_edit(row, change, error)) return false;
		// The destination's path as the edit found it, then as the record's removal leaves it: a step
		// through the list the record leaves, past the record's index, moves up one. The row's index
		// answers for the row as the edit found it, so the place after the removal is worked out here.
		std::vector<PathStep> to;
		if (edit.parent) {
			const RecordPath from = path_in(row, edit.address.child);
			const RecordPath there = path_in(row, edit.parent);
			to.assign(there.begin(), there.end());
			const size_t depth = from.size() - 1; // the record's own step
			bool through = !from.empty() && to.size() > depth;
			for (size_t d = 0; through && d < depth; ++d)
				through = to[d].collection == from[d].collection && to[d].index == from[d].index;
			if (through && to[depth].collection == from[depth].collection && to[depth].index > from[depth].index)
				--to[depth].index;
		}
		// The record and its identities come out together, or neither does; the destination, found by
		// that path, takes them at the position.
		DetachedRecord moved = ops.copy(owner.record, index);
		if (!moved.data) {
			error = "The record no longer exists.";
			return false;
		}
		prepare_record(row, change, moved);
		if (!ops.erase(owner.record, index)) {
			error = "The list keeps the record: it gives none up here.";
			return false;
		}
		RecordIds moved_ids = std::move(source_ids[index]);
		source_ids.erase(source_ids.begin() + std::ptrdiff_t(index));
		Located there;
		if (!locate(row, 0, there) || (edit.parent && !descend(to.data(), to.size(), there))) {
			error = "The destination no longer exists.";
			return false;
		}
		const ListOps &into = table().kind(there.record.kind)->lists()[destination_list].ops;
		const size_t position = std::min(edit.position, into.size(there.record));
		if (!into.insert(there.record, position, &moved, error)) return false;
		std::vector<RecordIds> &destination_ids = there.ids->lists[destination_list];
		destination_ids.insert(destination_ids.begin() + std::ptrdiff_t(std::min(position, destination_ids.size())),
		                       std::move(moved_ids));
		return true;
	}
	default:
		error = "This collection cannot accept that edit.";
		return false;
	}
}

} // namespace opennova::editor
