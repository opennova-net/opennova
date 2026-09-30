#include <editor/session/selection.h>

#include <algorithm>
#include <atomic>
#include <variant>

#include <editor/model/document.h>

namespace opennova::editor {

namespace {

std::atomic<uint64_t> g_next_serial{ 0 };

bool among(const std::vector<NodeId> &sorted, NodeId id) {
	return std::binary_search(sorted.begin(), sorted.end(), id);
}

} // namespace

bool has_record(const Document &document, const NodeAddress &address) {
	if (!address.row)
		return false;
	if (!address.child) {
		const Node *row = document.row(address.row);
		return row && row->kind == address.kind;
	}
	return document.address_of(address.child) == address;
}

void Selection::changed() {
	serial = ++g_next_serial;
}

bool Selection::holds(const NodeAddress &address) const {
	return std::find(records.begin(), records.end(), address) != records.end();
}

void Selection::select_only(const std::string &path, const NodeAddress &address) {
	document = path;
	primary = address.row ? address : NodeAddress();
	records.clear();
	if (address.row)
		records.push_back(address);
	changed();
}

void Selection::select(const std::string &path, const NodeAddress &named_primary,
		const std::vector<NodeAddress> &others, SelectMode mode) {
	std::vector<NodeAddress> named;
	for (const NodeAddress &address : others)
		if (address.row && std::find(named.begin(), named.end(), address) == named.end())
			named.push_back(address);
	if (named_primary.row && std::find(named.begin(), named.end(), named_primary) == named.end())
		named.push_back(named_primary);
	if (named.empty())
		return select_only(path, NodeAddress());
	if (mode == SelectMode::Replace || path != document) {
		document = path;
		records = named;
		primary = named_primary.row ? named_primary : named.front();
		return changed();
	}
	if (mode == SelectMode::Add) {
		for (const NodeAddress &address : named)
			if (!holds(address))
				records.push_back(address);
		primary = named_primary.row ? named_primary : named.back();
		return changed();
	}
	for (const NodeAddress &address : named) {
		const auto found = std::find(records.begin(), records.end(), address);
		if (found != records.end())
			records.erase(found);
		else
			records.push_back(address);
	}
	if (named_primary.row && holds(named_primary))
		primary = named_primary;
	else if (!holds(primary))
		primary = records.empty() ? NodeAddress() : records.back();
	changed();
}

void Selection::select_added(const Document &made_in) {
	document = made_in.path();
	std::vector<NodeAddress> added;
	for (const NodeId id : made_in.last_added_records()) {
		const NodeAddress address = made_in.address_of(id);
		if (address.row)
			added.push_back(address);
	}
	records = made_in.outermost(added);
	primary = records.empty() ? NodeAddress() : records.front();
	changed();
}

void Selection::make_primary(const NodeAddress &address) {
	if (address == primary || !holds(address))
		return;
	primary = address;
	changed();
}

void Selection::restore(const Selection &kept) {
	*this = kept;
	changed();
}

size_t Selection::repair(
		const Document &edited, const ChangeSet *changes, const NodeAddress &owner) {
	if (edited.path() != document)
		return 0;
	const RowChanges *rows = changes ? std::get_if<RowChanges>(changes) : nullptr;
	size_t asked = 0;
	const auto exists = [&](const NodeAddress &address) {
		++asked;
		return has_record(edited, address);
	};
	std::vector<NodeAddress> kept;
	kept.reserve(records.size());
	for (const NodeAddress &address : records) {
		bool keep = true;
		if (!rows)
			keep = exists(address);
		else if (among(rows->removed, address.row))
			keep = false;
		else if (among(rows->changed, address.row))
			keep = exists(address);
		if (keep)
			kept.push_back(address);
	}
	const bool primary_gone =
			primary.row && std::find(kept.begin(), kept.end(), primary) == kept.end();
	if (!primary_gone && kept.size() == records.size())
		return asked;
	if (primary_gone && owner.row && exists(owner)) {
		select_only(document, owner);
		return asked;
	}
	records = std::move(kept);
	if (primary_gone)
		primary = records.empty() ? NodeAddress() : records.back();
	changed();
	return asked;
}

} // namespace opennova::editor
