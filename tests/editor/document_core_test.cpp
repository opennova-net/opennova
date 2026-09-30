// The neutral editing core at any depth (ADR 0046 S9g), over a fake document type: a
// group row holds a fixed header and its items, an item holds leaves and items of its
// own, a leaf holds nothing, and every record carries its identity beside its data (the
// type owns its tree). Pinned here: the walk, placements, ancestors, names, paths and
// locators (a locator finds the same record again); an Add at depth; a reorder and a
// reparent; the refusals (out of the row, into itself, a fixed collection, a wrong kind,
// a stale identity, Clear on a field that is always written); a Move that changes
// nothing records nothing (B5); a batch is one undo step and undo swaps the identical row
// back; coalesced batches and gestures fold (never across rows, never over the saved
// checkpoint); the clipboard round trip through copy() and a Paste; the selection model
// the session keeps (SessionView) repaired after edits and undo; the type's veto on a
// change (accept_change) refusing any edit before it commits; a typing burst one step, a new
// name its record's edit alone; one step over several changes undone and redone as one; a
// batch filling in the records it makes (batch_made). S11a: the saved baseline and what
// changed since it (a field, a record, the file-wide state, the edits that give a field
// back), and the canonical rewrite. S11f: a document read from bytes alone has the rows a
// file's load gives and no file to save to. S13 D6: a change the type makes in C++ (an Apply
// edit's payload) through apply_payload, to the record, what it holds and the file-wide state in
// one step, one step with the batch's other edits and folding under its gesture, no step when it
// changes nothing, and, naming no row, to the file-wide state alone (apply_file_payload); a
// payload of another kind or none refused (document.payload); a load that fails leaving the
// document as it was; and a document of another kind than records (DocumentBase alone, over a
// byte blob) loaded, saved with its fingerprint, refusing a save over a file changed outside the
// editor, changed by a payload its type made and undone through its own history, held by the
// base to its rules (a snapshot and a blocked document take no edit, undo or redo, a blocked one
// no save), holding no records.
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <editor/model/document.h>
#include <editor/model/document_search.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>

#include "common/test_expect.h"
#include "editor/blob_document.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;

namespace {

constexpr NodeKind kGroup = 0, kItem = 1, kLeaf = 2, kHeader = 3;

struct FakeLeaf {
	NodeId id = 0;
	std::string name;
};

struct FakeItem {
	NodeId id = 0;
	std::string name;
	int64_t weight = 0;
	bool has_weight = false;
	std::vector<FakeLeaf> leaves;
	std::vector<FakeItem> items;
};

struct FakeGroup : Node {
	std::string title;
	NodeId header_id = 0;
	std::string header;
	std::vector<FakeItem> items;

	FakeGroup() { kind = kGroup; }
	std::shared_ptr<Node> clone() const override { return std::make_shared<FakeGroup>(*this); }
	std::string name() const override { return title; }
	void for_each_identity(const std::function<void(NodeId &)> &fn) override {
		fn(header_id);
		std::function<void(FakeItem &)> item = [&](FakeItem &entry) {
			fn(entry.id);
			for (FakeLeaf &leaf : entry.leaves) fn(leaf.id);
			for (FakeItem &child : entry.items) item(child);
		};
		for (FakeItem &entry : items) item(entry);
	}
};

// The file-wide state: a note ("N <note>" before the groups; none written when empty).
struct FakeState : FileState {
	std::string note;
	std::shared_ptr<FileState> clone() const override { return std::make_shared<FakeState>(*this); }
};

// A row's group, whichever row it is (the committed one, a clone, the saved baseline's).
FakeGroup &group_in(const Node &node) { return const_cast<FakeGroup &>(static_cast<const FakeGroup &>(node)); }

// An item anywhere in the group, with the list that holds it.
FakeItem *find_item(std::vector<FakeItem> &items, NodeId id, std::vector<FakeItem> **holder = nullptr) {
	for (FakeItem &item : items) {
		if (item.id == id) {
			if (holder) *holder = &items;
			return &item;
		}
		if (FakeItem *found = find_item(item.items, id, holder)) return found;
	}
	return nullptr;
}

FakeLeaf *find_leaf(std::vector<FakeItem> &items, NodeId id, std::vector<FakeLeaf> **holder = nullptr) {
	for (FakeItem &item : items) {
		for (FakeLeaf &leaf : item.leaves)
			if (leaf.id == id) {
				if (holder) *holder = &item.leaves;
				return &leaf;
			}
		if (FakeLeaf *found = find_leaf(item.items, id, holder)) return found;
	}
	return nullptr;
}

// One record per line: "I <depth> <name> [weight]", "L <depth> <name>" (depth 1 = in
// the group), after "G <name>" and "H <header>".
void write_item(const FakeItem &item, int depth, std::string &out) {
	out += "I " + std::to_string(depth) + " " + item.name + (item.has_weight ? " " + std::to_string(item.weight) : "") + "\n";
	for (const FakeLeaf &leaf : item.leaves) out += "L " + std::to_string(depth + 1) + " " + leaf.name + "\n";
	for (const FakeItem &child : item.items) write_item(child, depth + 1, out);
}

// Items and leaves from lines of that form, `base` the depth of the first level.
bool read_records(std::istream &in, int base, std::vector<FakeItem> &items, std::vector<FakeLeaf> &leaves) {
	std::vector<std::vector<FakeItem> *> levels{&items};
	std::string line;
	while (std::getline(in, line)) {
		std::istringstream words(line);
		std::string tag, name;
		int depth = 0;
		if (!(words >> tag >> depth)) continue;
		words >> name;
		const size_t level = size_t(depth - base);
		if (depth < base || level >= levels.size() + 1) return false;
		if (tag == "L") {
			if (level == 0) { leaves.push_back({0, name}); continue; }
			std::vector<FakeItem> &owner = *levels[level - 1];
			if (owner.empty()) return false;
			owner.back().leaves.push_back({0, name});
			continue;
		}
		if (tag != "I" || level >= levels.size()) return false;
		FakeItem item;
		item.name = name;
		if (words >> item.weight) item.has_weight = true;
		levels.resize(level + 1);
		levels[level]->push_back(item);
		levels.push_back(&levels[level]->back().items);
	}
	return true;
}

// A change made in C++, which an Apply edit carries to the fake type (S13 D6): an item's new name,
// a leaf added to it with a fresh identity, the file's note; each left empty changes nothing.
struct FakeChange : EditPayload {
	std::string name, leaf, note;
	const char *token() const override { return "fake.change"; }
};

std::shared_ptr<FakeChange> fake_change(std::string name, std::string leaf = {},
                                        std::string note = {}) {
	auto change = std::make_shared<FakeChange>();
	change->name = std::move(name);
	change->leaf = std::move(leaf);
	change->note = std::move(note);
	return change;
}

class FakeDocument : public Document {
public:
	const std::vector<RecordKindRow> &kinds() const override {
		static const std::vector<RecordKindRow> table = {
		        {kGroup, "group", "Group", "Add group", true},
		        {kItem, "item", "Item"},
		        {kLeaf, "leaf", "Leaf"},
		        {kHeader, "header", "Header"},
		};
		return table;
	}
	std::vector<Collection> collections(const Node &node, const NodeAddress &owner) const override {
		auto &group = const_cast<FakeGroup &>(static_cast<const FakeGroup &>(node));
		std::vector<NodeId> ids;
		if (!owner.child) {
			for (const FakeItem &item : group.items) ids.push_back(item.id);
			return {{{kHeader, "Header", "", true}, {group.header_id}},
			        {{kItem, "Items", "name"}, ids}};
		}
		if (owner.kind != kItem) return {};
		const FakeItem *item = find_item(group.items, owner.child);
		if (!item) return {};
		std::vector<NodeId> leaves;
		for (const FakeLeaf &leaf : item->leaves) leaves.push_back(leaf.id);
		for (const FakeItem &child : item->items) ids.push_back(child.id);
		return {{{kLeaf, "Leaves", "name"}, leaves}, {{kItem, "Items", "name"}, ids}};
	}
	const std::vector<FieldSchema> &fields(NodeKind kind) const override {
		static const std::vector<FieldSchema> group = {{"name", FieldType::Text, 32}};
		static const std::vector<FieldSchema> header = {{"text", FieldType::Text, 32}};
		static const std::vector<FieldSchema> item = [] {
			std::vector<FieldSchema> out = {{"name", FieldType::Text, 32}, {"weight", FieldType::Integer, 0}};
			out.back().optional = true;
			return out;
		}();
		static const std::vector<FieldSchema> leaf = {{"name", FieldType::Text, 32}};
		static const std::vector<FieldSchema> none;
		return kind == kGroup ? group : kind == kHeader ? header : kind == kItem ? item : kind == kLeaf ? leaf : none;
	}
	SerializeResult serialize() const override {
		SerializeResult result;
		const auto *state = static_cast<const FakeState *>(file_state());
		if (state && !state->note.empty()) result.text += "N " + state->note + "\n";
		for (const auto &node : rows()) {
			const auto &group = static_cast<const FakeGroup &>(*node);
			result.text += "G " + group.title + "\nH " + group.header + "\n";
			for (const FakeItem &item : group.items) write_item(item, 1, result.text);
		}
		return result;
	}
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<FakeDocument>(*this); }
	// The payload: "items" or "leaves", then the records in the file's own form.
	std::string copy(const std::vector<NodeAddress> &records) const override {
		if (records.empty()) return std::string();
		const NodeKind kind = records.front().kind;
		if (kind != kItem && kind != kLeaf) return std::string();
		std::string payload = kind == kItem ? "items\n" : "leaves\n";
		for (const NodeAddress &record : records) {
			if (record.kind != kind || !row(record.row)) return std::string();
			FakeGroup &group = group_of(record.row);
			if (kind == kItem) {
				const FakeItem *item = find_item(group.items, record.child);
				if (!item) return std::string();
				write_item(*item, 1, payload);
			} else {
				const FakeLeaf *leaf = find_leaf(group.items, record.child);
				if (!leaf) return std::string();
				payload += "L 1 " + leaf->name + "\n";
			}
		}
		return payload;
	}

protected:
	bool read_present(const Node &node, const NodeAddress &address, const std::string &field) const override {
		const FakeItem *item = address.kind == kItem ? find_item(group_in(node).items, address.child) : nullptr;
		return !item || field != "weight" || item->has_weight;
	}
	bool read(const Node &node, const NodeAddress &address, const std::string &field, Value &out) const override {
		FakeGroup &group = group_in(node);
		if (address.kind == kGroup && !address.child && field == "name") { out = group.title; return true; }
		if (address.kind == kHeader && address.child == group.header_id && field == "text") { out = group.header; return true; }
		if (address.kind == kItem) {
			const FakeItem *item = find_item(group.items, address.child);
			if (!item) return false;
			if (field == "name") { out = item->name; return true; }
			if (field == "weight") { out = item->weight; return true; }
		}
		if (address.kind == kLeaf && field == "name") {
			const FakeLeaf *leaf = find_leaf(group.items, address.child);
			if (leaf) { out = leaf->name; return true; }
		}
		return false;
	}
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
	           std::shared_ptr<const FileState> &state, std::vector<SourceIssue> &, Diagnostic &error) override {
		std::istringstream in(std::string(bytes.begin(), bytes.end()));
		std::string line, body;
		std::shared_ptr<FakeGroup> group;
		auto note = std::make_shared<FakeState>();
		state = note;
		auto flush = [&]() {
			if (!group) return true;
			std::istringstream records(body);
			std::vector<FakeLeaf> loose;
			body.clear();
			return read_records(records, 1, group->items, loose) && loose.empty();
		};
		while (std::getline(in, line)) {
			if (line.rfind("N ", 0) == 0 && !group) {
				note->note = line.substr(2);
			} else if (line.rfind("G ", 0) == 0) {
				if (!flush()) break;
				group = std::make_shared<FakeGroup>();
				group->title = line.substr(2);
				rows.push_back(group);
			} else if (line.rfind("H ", 0) == 0 && group) {
				group->header = line.substr(2);
			} else if (!line.empty() && line[0] != '\0') {
				body += line + "\n";
			}
		}
		if (!flush()) {
			error = editor_test::finding_of(DiagnosticSeverity::Error, "document.parse", "Malformed fake document.", path());
			return false;
		}
		return true;
	}
	std::shared_ptr<Node> make_node(NodeKind kind, NodeId, std::string &error) override {
		if (kind != kGroup) { error = "Groups are the rows."; return nullptr; }
		auto group = std::make_shared<FakeGroup>();
		group->title = "new";
		return group;
	}
	bool set_field(Node &node, const NodeAddress &address, const std::string &field, const Value &value,
	               std::string &error) override {
		FakeGroup &group = static_cast<FakeGroup &>(node);
		const auto *text = std::get_if<std::string>(&value);
		const auto *number = std::get_if<int64_t>(&value);
		if (address.kind == kGroup && field == "name" && text) { group.title = *text; return true; }
		if (address.kind == kHeader && field == "text" && text) { group.header = *text; return true; }
		if (address.kind == kItem) {
			FakeItem *item = find_item(group.items, address.child);
			if (item && field == "name" && text) { item->name = *text; return true; }
			if (item && field == "weight" && number) { item->weight = *number; item->has_weight = true; return true; }
		}
		if (address.kind == kLeaf && field == "name" && text) {
			if (FakeLeaf *leaf = find_leaf(group.items, address.child)) { leaf->name = *text; return true; }
		}
		error = "Unknown field.";
		return false;
	}
	bool set_present(Node &node, const NodeAddress &address, const std::string &field, bool present,
	                 std::string &error) override {
		FakeItem *item = address.kind == kItem ? find_item(static_cast<FakeGroup &>(node).items, address.child) : nullptr;
		if (!item || field != "weight") { error = "This field is always written."; return false; }
		item->has_weight = present;
		return true;
	}
	bool edit_collection(Node &node, const Edit &edit, const IdAllocator &allocate, NodeId &added,
	                     std::string &error) override {
		FakeGroup &group = static_cast<FakeGroup &>(node);
		// The owner's list of the edit's kind: the group's items, or an item's items or leaves.
		FakeItem *owner = edit.parent ? find_item(group.items, edit.parent) : nullptr;
		if (edit.parent && !owner) { error = "No such owner."; return false; }
		if (edit.address.kind == kItem) {
			std::vector<FakeItem> &into = owner ? owner->items : group.items;
			std::vector<FakeItem> *from = nullptr;
			FakeItem *item = edit.operation == EditOperation::Add ? nullptr : find_item(group.items, edit.address.child, &from);
			if (edit.operation != EditOperation::Add && !item) { error = "No such item."; return false; }
			switch (edit.operation) {
			case EditOperation::Add: {
				FakeItem fresh;
				fresh.id = added = allocate();
				fresh.name = "item";
				into.insert(into.begin() + std::ptrdiff_t(std::min(edit.position, into.size())), fresh);
				return true;
			}
			case EditOperation::Duplicate: {
				FakeItem copy = *item;
				std::function<void(FakeItem &)> fresh = [&](FakeItem &entry) {
					entry.id = allocate();
					for (FakeLeaf &leaf : entry.leaves) leaf.id = allocate();
					for (FakeItem &child : entry.items) fresh(child);
				};
				fresh(copy);
				added = copy.id;
				into.insert(into.begin() + std::ptrdiff_t(std::min(edit.position, into.size())), copy);
				return true;
			}
			case EditOperation::Remove:
				from->erase(from->begin() + (item - from->data()));
				return true;
			case EditOperation::Move: {
				const FakeItem moved = *item;
				from->erase(from->begin() + (item - from->data()));
				FakeItem *destination = edit.parent ? find_item(group.items, edit.parent) : nullptr;
				std::vector<FakeItem> &target = destination ? destination->items : group.items;
				target.insert(target.begin() + std::ptrdiff_t(std::min(edit.position, target.size())), moved);
				return true;
			}
			default: break;
			}
		}
		if (edit.address.kind == kLeaf && owner) {
			std::vector<FakeLeaf> *from = nullptr;
			FakeLeaf *leaf = edit.operation == EditOperation::Add ? nullptr : find_leaf(group.items, edit.address.child, &from);
			if (edit.operation != EditOperation::Add && !leaf) { error = "No such leaf."; return false; }
			std::vector<FakeLeaf> &into = owner->leaves;
			switch (edit.operation) {
			case EditOperation::Add:
				added = allocate();
				into.insert(into.begin() + std::ptrdiff_t(std::min(edit.position, into.size())), FakeLeaf{added, "leaf"});
				return true;
			case EditOperation::Duplicate: {
				const FakeLeaf copy{added = allocate(), leaf->name};
				into.insert(into.begin() + std::ptrdiff_t(std::min(edit.position, into.size())), copy);
				return true;
			}
			case EditOperation::Remove:
				from->erase(from->begin() + (leaf - from->data()));
				return true;
			case EditOperation::Move: {
				const FakeLeaf moved = *leaf;
				from->erase(from->begin() + (leaf - from->data()));
				FakeItem *destination = find_item(group.items, edit.parent);
				destination->leaves.insert(destination->leaves.begin() +
				                                   std::ptrdiff_t(std::min(edit.position, destination->leaves.size())), moved);
				return true;
			}
			default: break;
			}
		}
		error = "This collection cannot accept that edit.";
		return false;
	}
	bool paste_records(Node &node, const Edit &edit, const IdAllocator &allocate, std::vector<NodeId> &added,
	                   std::string &error) override {
		FakeGroup &group = static_cast<FakeGroup &>(node);
		const auto *payload = std::get_if<std::string>(&edit.value);
		std::istringstream in(payload ? *payload : std::string());
		std::string kind;
		std::getline(in, kind);
		std::vector<FakeItem> items;
		std::vector<FakeLeaf> leaves;
		if ((kind != "items" && kind != "leaves") || !read_records(in, 1, items, leaves)) {
			error = "Not a fake clipboard.";
			return false;
		}
		FakeItem *owner = edit.parent ? find_item(group.items, edit.parent) : nullptr;
		if (kind == "leaves") {
			if (!owner) { error = "Leaves are pasted into an item."; return false; }
			size_t at = std::min(edit.position, owner->leaves.size());
			for (FakeLeaf &leaf : leaves) {
				leaf.id = allocate();
				added.push_back(leaf.id);
				owner->leaves.insert(owner->leaves.begin() + std::ptrdiff_t(at++), leaf);
			}
			return true;
		}
		std::vector<FakeItem> &into = owner ? owner->items : group.items;
		size_t at = std::min(edit.position, into.size());
		std::function<void(FakeItem &)> fresh = [&](FakeItem &entry) {
			entry.id = allocate();
			for (FakeLeaf &leaf : entry.leaves) leaf.id = allocate();
			for (FakeItem &child : entry.items) fresh(child);
		};
		for (FakeItem &item : items) {
			fresh(item);
			added.push_back(item.id);
			into.insert(into.begin() + std::ptrdiff_t(at++), item);
		}
		return true;
	}
	// The note: a text value.
	bool set_file_value(std::shared_ptr<const FileState> &state, const Edit &edit, Diagnostic &error) override {
		const auto *text = std::get_if<std::string>(&edit.value);
		if (!text) {
			error = editor_test::finding_of(DiagnosticSeverity::Error, "document.value", "The note is text.", path());
			return false;
		}
		auto note = std::make_shared<FakeState>();
		note->note = *text;
		state = note;
		return true;
	}
	bool accept_change(const Change &change, std::string &error) const override {
		if (!veto || veto(change)) return true;
		error = "Vetoed.";
		return false;
	}
	// A change made in C++ (FakeChange) of an item and the note; any other change refused.
	bool apply_payload(Node &node, const NodeAddress &address, const EditPayload &payload,
	                   std::shared_ptr<const FileState> &state, const IdAllocator &allocate,
	                   bool &changed, std::string &error) override {
		const auto *change = dynamic_cast<const FakeChange *>(&payload);
		FakeItem *item = nullptr;
		if (change && address.kind == kItem) item = find_item(group_in(node).items, address.child);
		if (!item) {
			error = "Not a fake change.";
			return false;
		}
		changed = false;
		if (!change->name.empty() && change->name != item->name) {
			item->name = change->name;
			changed = true;
		}
		if (!change->leaf.empty()) {
			item->leaves.push_back({allocate(), change->leaf});
			changed = true;
		}
		bool noted = false;
		if (!set_note(state, change->note, noted, error)) return false;
		changed = changed || noted;
		return true;
	}
	// The note alone (an Apply naming no row); a change naming an item's parts refused.
	bool apply_file_payload(std::shared_ptr<const FileState> &state, const EditPayload &payload,
	                        bool &changed, std::string &error) override {
		const auto *change = dynamic_cast<const FakeChange *>(&payload);
		if (!change || !change->name.empty() || !change->leaf.empty()) {
			error = "A name or a leaf needs its item.";
			return false;
		}
		return set_note(state, change->note, changed, error);
	}
	// The note set in a copy of `state` when it differs ("" leaves it).
	static bool set_note(std::shared_ptr<const FileState> &state, const std::string &text,
	                     bool &changed, std::string &) {
		const auto *now = static_cast<const FakeState *>(state.get());
		changed = !text.empty() && (!now || now->note != text);
		if (!changed) return true;
		auto note = now ? std::make_shared<FakeState>(*now) : std::make_shared<FakeState>();
		note->note = text;
		state = note;
		return true;
	}

public:
	std::function<bool(const Change &)> veto; // false refuses the change (unset: every change is accepted)
	const std::string &game_name() const { return game(); }

private:
	FakeGroup &group_of(NodeId id) const { return const_cast<FakeGroup &>(static_cast<const FakeGroup &>(*row(id))); }
};

const char *const kFile =
        "G alpha\nH top\n"
        "I 1 a1 5\nL 2 x\nL 2 y\nI 2 a1b\n"
        "I 1 a2\n"
        "G beta\nH bottom\n"
        "I 1 b1\n";

struct Loaded {
	editor_test::TempProjectDir dir{"opennova_document_core_test"};
	FakeDocument document;
	NodeAddress alpha, beta, header, a1, x, y, a1b, a2, b1;
	std::string original;

	bool load() {
		if (!editor_test::write_text(dir.file("fake.txt"), kFile)) return false;
		Diagnostic error;
		if (!document.load(dir.file("fake.txt"), "fake.txt", opennova::editor::AssetKind::Unknown, "jo", error)) return false;
		if (document.rows().size() != 2) return false;
		const Node &first = *document.rows()[0], &second = *document.rows()[1];
		alpha = {first.id, kGroup, 0};
		beta = {second.id, kGroup, 0};
		std::vector<NodeAddress> records;
		document.walk_records(first, [&](const NodeAddress &record, const Document::Placement &) {
			records.push_back(record);
			return true;
		});
		if (records.size() != 6) return false;
		header = records[0]; a1 = records[1]; x = records[2]; y = records[3]; a1b = records[4]; a2 = records[5];
		b1 = {second.id, kItem, static_cast<const FakeGroup &>(second).items[0].id};
		original = document.serialize().text;
		return true;
	}
};

Edit make(EditOperation operation, NodeAddress address, NodeId parent = 0, size_t position = SIZE_MAX) {
	Edit edit;
	edit.operation = operation;
	edit.address = address;
	edit.parent = parent;
	edit.position = position;
	return edit;
}

Edit set(NodeAddress address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

// --- a flat table of many lines (S13 D2): the core's per-call costs, counted -----------------

constexpr NodeKind kLine = 0;
// The tags a line's tag field offers: as many as an animation table's slot keys, which a use of
// the field must never copy.
constexpr size_t kTags = 252;
size_t g_line_clones = 0, g_line_reads = 0;

struct FlatLine : Node {
	std::string title;
	int64_t weight = 0;
	std::string tag;
	FlatLine() { kind = kLine; }
	std::shared_ptr<Node> clone() const override {
		++g_line_clones;
		return std::make_shared<FlatLine>(*this);
	}
	std::string name() const override { return title; }
};

// One line per row, "L <title> <weight> <tag>"; each read of a field counted.
class FlatDocument : public Document {
public:
	const std::vector<RecordKindRow> &kinds() const override {
		static const std::vector<RecordKindRow> table = {{kLine, "line", "Line", "Add line", true}};
		return table;
	}
	std::vector<Collection> collections(const Node &, const NodeAddress &) const override { return {}; }
	const std::vector<FieldSchema> &fields(NodeKind kind) const override {
		static const std::vector<FieldSchema> line = [] {
			FieldSchema title{"title", FieldType::Text, 32};
			FieldSchema weight{"weight", FieldType::Integer};
			FieldSchema tag{"tag", FieldType::Text, 32};
			for (size_t i = 0; i < kTags; ++i) tag.choices.push_back({"TAG" + std::to_string(i), int64_t(i), ""});
			tag.open_choices = true;
			FieldSchema serial{"serial", FieldType::Integer};
			serial.read_only = true;
			return std::vector<FieldSchema>{title, weight, tag, serial};
		}();
		static const std::vector<FieldSchema> none;
		return kind == kLine ? line : none;
	}
	// A line titled "own..." offers two tags of its own.
	bool record_choices(const NodeAddress &, const FieldUse &use, std::vector<FieldChoice> &out) const override {
		if (!use.own_choices) return false;
		out = {{"MINE", 0, "Mine"}, {"YOURS", 1, "Yours"}};
		return true;
	}
	SerializeResult serialize() const override {
		SerializeResult result;
		for (const auto &node : rows()) {
			const auto &line = static_cast<const FlatLine &>(*node);
			result.text += "L " + line.title + " " + std::to_string(line.weight) + " " + line.tag + "\n";
		}
		return result;
	}
	std::unique_ptr<DocumentBase> snapshot() const override { return std::make_unique<FlatDocument>(*this); }

protected:
	// A refinement that would make the read-only serial writable (the base puts the schema's
	// read_only back), and the own tags of a line titled "own...".
	void refine_field(const NodeAddress &address, FieldUse &use) const override {
		if (use.schema->id == "serial") use.read_only = false;
		if (use.schema->id != "tag") return;
		const Node *line = row(address.row);
		use.own_choices = line && line->name().rfind("own", 0) == 0;
	}
	bool read(const Node &node, const NodeAddress &address, const std::string &field, Value &out) const override {
		++g_line_reads;
		const auto &line = static_cast<const FlatLine &>(node);
		if (address.child) return false;
		if (field == "title") out = line.title;
		else if (field == "weight") out = line.weight;
		else if (field == "tag") out = line.tag;
		else if (field == "serial") out = int64_t(line.id);
		else return false;
		return true;
	}
	bool parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows, std::shared_ptr<const FileState> &,
	           std::vector<SourceIssue> &, Diagnostic &error) override {
		std::istringstream in(std::string(bytes.begin(), bytes.end()));
		std::string tag;
		while (in >> tag) {
			auto line = std::make_shared<FlatLine>();
			if (tag != "L" || !(in >> line->title >> line->weight >> line->tag)) {
				error = editor_test::finding_of(DiagnosticSeverity::Error, "document.parse", "Not a line.", path());
				return false;
			}
			rows.push_back(line);
		}
		return true;
	}
	std::shared_ptr<Node> make_node(NodeKind, NodeId, std::string &) override { return std::make_shared<FlatLine>(); }
	bool set_field(Node &node, const NodeAddress &, const std::string &field, const Value &value,
	               std::string &error) override {
		auto &line = static_cast<FlatLine &>(node);
		const auto *text = std::get_if<std::string>(&value);
		const auto *number = std::get_if<int64_t>(&value);
		if (field == "title" && text) line.title = *text;
		else if (field == "weight" && number) line.weight = *number;
		else if (field == "tag" && text) line.tag = *text;
		else {
			error = "Unknown field.";
			return false;
		}
		return true;
	}
	bool edit_collection(Node &, const Edit &, const IdAllocator &, NodeId &, std::string &error) override {
		error = "A line holds nothing.";
		return false;
	}
};

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

} // namespace

// The walk, placements, ancestors, names, paths and locators.
static int test_structure() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	TEST_EXPECT(fake.header.kind == kHeader && fake.a1.kind == kItem && fake.x.kind == kLeaf && fake.a1b.kind == kItem);
	Document::Placement at;
	TEST_EXPECT(!document.placement(fake.alpha, at));
	TEST_EXPECT(document.placement(fake.header, at) && at.owner == fake.alpha && at.spec.fixed && at.index == 0);
	TEST_EXPECT(document.placement(fake.y, at) && at.owner == fake.a1 && at.spec.kind == kLeaf && at.index == 1);
	TEST_EXPECT(document.placement(fake.a1b, at) && at.owner == fake.a1 && at.spec.kind == kItem && at.index == 0);
	TEST_EXPECT(document.placement(fake.a2, at) && at.owner == fake.alpha && at.index == 1);
	TEST_EXPECT(document.ancestors(fake.a1b) == std::vector<NodeAddress>({fake.alpha, fake.a1}));
	TEST_EXPECT(document.ancestors(fake.alpha).empty() && document.ancestors({fake.alpha.row, kItem, 99999}).empty());
	TEST_EXPECT(document.address_of(fake.y.child) == fake.y && document.address_of(fake.beta.row) == fake.beta);
	TEST_EXPECT(document.record_name(fake.header) == "Header 1" && document.record_name(fake.y) == "y");
	TEST_EXPECT(document.record_path(fake.a1b) == "alpha/a1/a1b" && document.record_path(fake.b1) == "beta/b1");
	TEST_EXPECT(document.locator(fake.header) == "0/header:0" && document.locator(fake.y) == "0/item:0/leaf:1");
	TEST_EXPECT(document.locator(fake.a1b) == "0/item:0/item:0" && document.locator(fake.b1) == "1/item:0");
	TEST_EXPECT(document.locator(fake.beta) == "1" && document.address_at("1") == fake.beta);
	size_t round_trips = 0, records = 0;
	for (const auto &row : document.rows())
		document.walk_records(*row, [&](const NodeAddress &record, const Document::Placement &) {
			++records;
			if (document.address_at(document.locator(record)) == record) ++round_trips;
			return true;
		});
	TEST_EXPECT(records == 8 && round_trips == records);
	TEST_EXPECT(document.address_at("0/item:5") == NodeAddress() && document.address_at("0/leaf:0") == NodeAddress());
	TEST_EXPECT(document.address_at("2") == NodeAddress() && document.address_at("x/item:0") == NodeAddress());
	// The walk stops when the visitor says so.
	size_t visited = 0;
	document.walk_records(*document.rows()[0], [&](const NodeAddress &, const Document::Placement &) { return ++visited < 3; });
	TEST_EXPECT(visited == 3);
	// A reload assigns the same identities in the same order: the locators find them.
	FakeDocument again;
	Diagnostic error;
	TEST_EXPECT(again.load(fake.dir.file("fake.txt"), "fake.txt", opennova::editor::AssetKind::Unknown, "jo", error));
	TEST_EXPECT(again.address_at(document.locator(fake.a1b)).child == fake.a1b.child);
	// The same bytes with no file (an import's plan reads them so): the same rows, and a Save
	// that says there is no file to write.
	FakeDocument bytes;
	const std::string text = kFile;
	TEST_EXPECT(bytes.load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "fake.txt",
	                             opennova::editor::AssetKind::Unknown, "jo", error));
	TEST_EXPECT(bytes.path() == "fake.txt" && bytes.rows().size() == 2 && bytes.serialize().text == fake.original);
	TEST_EXPECT(bytes.address_at(document.locator(fake.a1b)).child == fake.a1b.child && !bytes.dirty());
	TEST_EXPECT(!bytes.matches_file() && !bytes.save(error) && error.code() == "document.no_file");
	return 0;
}

// Add at depth, reorder, reparent, and the refusals.
static int test_structural_edits() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	// A leaf added into a1b (depth 2), then one between x and y.
	TEST_EXPECT(document.apply(make(EditOperation::Add, {0, kLeaf, 0}, fake.a1b.child), error));
	const NodeAddress deep = document.address_of(document.last_added());
	Document::Placement at;
	TEST_EXPECT(deep.kind == kLeaf && document.placement(deep, at) && at.owner == fake.a1b && at.index == 0);
	TEST_EXPECT(document.ancestors(deep) == std::vector<NodeAddress>({fake.alpha, fake.a1, fake.a1b}));
	TEST_EXPECT(document.apply(make(EditOperation::Add, {fake.alpha.row, kLeaf, 0}, fake.a1.child, 1), error));
	TEST_EXPECT(document.placement(document.address_of(document.last_added()), at) && at.owner == fake.a1 && at.index == 1);
	TEST_EXPECT(document.placement(fake.y, at) && at.index == 2);
	// Reorder inside a1 (an index among a1's leaves), then reparent x into a2.
	TEST_EXPECT(document.apply(make(EditOperation::Move, fake.y, 0, 0), error));
	TEST_EXPECT(document.placement(fake.y, at) && at.owner == fake.a1 && at.index == 0);
	TEST_EXPECT(document.apply(make(EditOperation::Move, fake.x, fake.a2.child, 0), error));
	TEST_EXPECT(document.placement(fake.x, at) && at.owner == fake.a2 && at.index == 0);
	TEST_EXPECT(document.record_path(fake.x) == "alpha/a2/x");
	// An item reparented into its sibling, with its subtree.
	TEST_EXPECT(document.apply(make(EditOperation::Move, fake.a1b, fake.a2.child, SIZE_MAX), error));
	TEST_EXPECT(document.ancestors(deep) == std::vector<NodeAddress>({fake.alpha, fake.a2, fake.a1b}));
	for (int i = 0; i < 5; ++i) document.undo();
	TEST_EXPECT(document.serialize().text == fake.original && !document.dirty());

	// Refused: out of its row (a Move and an Add), into itself or anything inside it, a
	// fixed collection, a kind the owner does not hold, a wrong kind, a stale identity.
	auto refused = [&](const Edit &edit, const char *code) {
		const bool as_expected = !document.apply(edit, error) && error.code() == code;
		if (!as_expected) std::printf("  not refused as %s: %s\n", code, error.message.c_str());
		return as_expected;
	};
	TEST_EXPECT(refused(make(EditOperation::Move, fake.b1, fake.a1.child), "document.collection"));
	TEST_EXPECT(error.message.find("within its own") != std::string::npos);
	TEST_EXPECT(refused(make(EditOperation::Add, {fake.beta.row, kLeaf, 0}, fake.a1.child), "document.selection"));
	TEST_EXPECT(refused(make(EditOperation::Move, fake.a1, fake.a1.child), "document.collection"));
	TEST_EXPECT(error.message.find("inside itself") != std::string::npos);
	TEST_EXPECT(refused(make(EditOperation::Move, fake.a1, fake.a1b.child), "document.collection"));
	TEST_EXPECT(error.message.find("inside itself") != std::string::npos);
	TEST_EXPECT(refused(make(EditOperation::Remove, fake.header), "document.collection"));
	TEST_EXPECT(refused(make(EditOperation::Move, fake.header, 0, 5), "document.collection"));
	TEST_EXPECT(refused(make(EditOperation::Duplicate, fake.header), "document.collection"));
	TEST_EXPECT(refused(make(EditOperation::Add, {fake.alpha.row, kHeader, 0}), "document.collection"));
	TEST_EXPECT(refused(make(EditOperation::Add, {fake.alpha.row, kLeaf, 0}), "document.collection")); // a group holds no leaves
	TEST_EXPECT(refused(make(EditOperation::Move, fake.x, fake.y.child), "document.collection"));      // nor does a leaf
	TEST_EXPECT(refused(set({fake.alpha.row, kLeaf, fake.a1.child}, "name", std::string("n")), "document.selection"));
	TEST_EXPECT(error.message.find("the record is Item, the address says Leaf") != std::string::npos);
	TEST_EXPECT(refused(set({fake.alpha.row, kItem, 99999}, "name", std::string("n")), "document.selection"));
	TEST_EXPECT(refused(make(EditOperation::Remove, {fake.alpha.row, kItem, 99999}), "document.selection"));
	TEST_EXPECT(refused(set({fake.alpha.row, kItem, 0}, "name", std::string("n")), "document.selection"));
	TEST_EXPECT(refused(make(EditOperation::Remove, {fake.alpha.row, kLeaf, 0}), "document.selection"));
	Edit clear = make(EditOperation::Clear, fake.a1);
	clear.field = "name";
	TEST_EXPECT(refused(clear, "document.value") && error.message == "This field is always written.");
	clear.field = "nope";
	TEST_EXPECT(refused(clear, "document.value"));
	Edit write = make(EditOperation::Write, fake.a1);
	write.field = "name";
	TEST_EXPECT(refused(write, "document.value") && error.message == "This field is always written.");
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo());

	// Clear: an optional field left out, its value kept; Write writes that value again.
	clear.field = write.field = "weight";
	TEST_EXPECT(document.apply(write, error) && !document.can_undo()); // already written: no step
	TEST_EXPECT(document.present(fake.a1, "weight") && document.apply(clear, error) && !document.present(fake.a1, "weight"));
	Value weight;
	TEST_EXPECT(document.get(fake.a1, "weight", weight) && std::get<int64_t>(weight) == 5);
	TEST_EXPECT(document.serialize().text.find("I 1 a1\n") != std::string::npos);
	const uint64_t cleared = document.revision();
	TEST_EXPECT(document.apply(clear, error) && document.revision() == cleared); // already left out: no step
	TEST_EXPECT(document.apply(write, error) && document.present(fake.a1, "weight"));
	TEST_EXPECT(document.serialize().text == fake.original && document.dirty());
	document.undo();
	TEST_EXPECT(!document.present(fake.a1, "weight") && document.revision() == cleared);
	// In a batch, each edit reads the row as the earlier ones left it: one step.
	TEST_EXPECT(document.apply(std::vector<Edit>{write, clear, write}, error) && document.present(fake.a1, "weight"));
	document.undo();
	TEST_EXPECT(!document.present(fake.a1, "weight"));
	document.undo();
	TEST_EXPECT(document.serialize().text == fake.original && !document.dirty());

	// B5: a Move that leaves a record where it is records nothing, the document stays clean.
	TEST_EXPECT(document.apply(make(EditOperation::Move, fake.y, 0, 1), error));
	TEST_EXPECT(document.apply(make(EditOperation::Move, fake.y, 0, 7), error)); // Down on the last sibling
	TEST_EXPECT(document.apply(make(EditOperation::Move, fake.y, fake.a1.child, 1), error)); // its own owner, named
	TEST_EXPECT(document.apply(make(EditOperation::Move, fake.alpha, 0, 0), error));        // a row to its own place
	TEST_EXPECT(!document.dirty() && !document.can_undo());
	return 0;
}

// S12 Z2: a Set of the value a field holds records nothing, alone or in a batch whose other
// edits change nothing either; a coalesced group typed back to the value it found leaves no
// step and no redo, and typing on starts a step from there. A Set of an optional field left
// out, of the value it keeps, writes it: its presence changes, a step.
static int test_set_same_value() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	Value name, weight;
	TEST_EXPECT(document.get(fake.a1, "name", name) && document.get(fake.a1, "weight", weight));
	const uint64_t start = document.revision();
	TEST_EXPECT(document.apply(set(fake.a1, "name", name), error));
	TEST_EXPECT(!document.dirty() && !document.can_undo() && document.revision() == start);
	Edit write = make(EditOperation::Write, fake.a1);
	write.field = "weight";
	TEST_EXPECT(document.apply(std::vector<Edit>{set(fake.a1, "weight", weight), write, set(fake.a1, "name", name)}, error));
	TEST_EXPECT(!document.dirty() && !document.can_undo() && document.revision() == start);
	// Typing: the group's value changed, then typed back to what the group found.
	Edit typed = set(fake.a1, "name", std::string("a1 typed"));
	typed.coalesce = true;
	TEST_EXPECT(document.apply(typed, error) && document.dirty() && document.can_undo());
	typed.value = name;
	TEST_EXPECT(document.apply(typed, error) && !document.dirty() && !document.can_undo() && !document.can_redo());
	TEST_EXPECT(document.serialize().text == fake.original && document.revision() == start);
	typed.value = std::string("a1 again");
	TEST_EXPECT(document.apply(typed, error) && document.dirty());
	typed.value = std::string("a1 again, on");
	TEST_EXPECT(document.apply(typed, error) && document.dirty());
	document.undo(); // the typing on: one step
	TEST_EXPECT(!document.dirty() && !document.can_undo() && document.serialize().text == fake.original);
	// Left out, the weight keeps its value: a Set of it writes the field again.
	Edit clear = make(EditOperation::Clear, fake.a1);
	clear.field = "weight";
	TEST_EXPECT(document.apply(clear, error) && !document.present(fake.a1, "weight"));
	const uint64_t cleared = document.revision();
	TEST_EXPECT(document.apply(set(fake.a1, "weight", weight), error) && document.present(fake.a1, "weight") &&
	            document.revision() != cleared);
	document.undo();
	document.undo();
	TEST_EXPECT(!document.dirty() && document.serialize().text == fake.original);
	return 0;
}

// An Add naming a field sets it on the new record in the same step (a menu window added
// with its type, ADR 0046 S9h2): one undo step takes both; a value the field refuses
// refuses the Add and commits nothing; a new row takes one too.
static int test_add_with_a_value() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	Edit add = make(EditOperation::Add, {fake.alpha.row, kItem, 0}, fake.a1.child);
	add.field = "weight";
	add.value = int64_t(9);
	TEST_EXPECT(document.apply(add, error));
	const NodeAddress made = document.address_of(document.last_added());
	Value weight;
	TEST_EXPECT(made.kind == kItem && document.ancestors(made).back() == fake.a1);
	TEST_EXPECT(document.get(made, "weight", weight) && std::get<int64_t>(weight) == 9 && document.present(made, "weight"));
	document.undo();
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo());
	add.value = std::string("heavy"); // the weight takes a number
	TEST_EXPECT(!document.apply(add, error) && error.code() == "document.value" && error.field == "weight");
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo() && !document.dirty());
	Edit row = make(EditOperation::Add, {0, kGroup, 0});
	row.field = "name";
	row.value = std::string("gamma");
	TEST_EXPECT(document.apply(row, error) && document.rows().back()->name() == "gamma");
	document.undo();
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo());
	row.value = int64_t(3);
	TEST_EXPECT(!document.apply(row, error) && error.code() == "document.value");
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo());
	return 0;
}

// The type's veto (accept_change) sees every change before it commits, the rows added,
// removed, moved and duplicated included (which never reach the type otherwise), as the
// row swap it is; a refused one commits nothing and leaves the history and last_added as
// they were.
static int test_veto() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	TEST_EXPECT(document.apply(make(EditOperation::Duplicate, fake.a2, 0, 2), error));
	const NodeId added = document.last_added();
	const uint64_t revision = document.revision();
	const std::string text = document.serialize().text;
	std::vector<Change> seen;
	document.veto = [&](const Change &change) {
		seen.push_back(change);
		return false;
	};
	TEST_EXPECT(!document.apply(make(EditOperation::Add, {0, kGroup, 0}), error) && error.code() == "document.structure" &&
	            error.message == "Vetoed.");
	TEST_EXPECT(!document.apply(make(EditOperation::Remove, fake.beta), error));
	TEST_EXPECT(!document.apply(make(EditOperation::Move, fake.beta, 0, 0), error));
	TEST_EXPECT(!document.apply(make(EditOperation::Duplicate, fake.beta, 0, 2), error));
	TEST_EXPECT(!document.apply(make(EditOperation::Add, {fake.alpha.row, kItem, 0}, fake.a1.child), error));
	TEST_EXPECT(!document.apply(set(fake.y, "name", std::string("z")), error) && error.code() == "document.structure");
	TEST_EXPECT(document.revision() == revision && document.last_added() == added && document.serialize().text == text &&
	            document.rows().size() == 2);
	TEST_EXPECT(seen.size() == 6);
	if (seen.size() == 6) {
		TEST_EXPECT(!seen[0].before && seen[0].after && seen[0].after_position == 2);
		TEST_EXPECT(seen[1].before && seen[1].before->id == fake.beta.row && !seen[1].after);
		TEST_EXPECT(seen[2].before && seen[2].after && seen[2].before_position == 1 && seen[2].after_position == 0);
		TEST_EXPECT(!seen[3].before && seen[3].after && seen[3].after->id != fake.beta.row);
		TEST_EXPECT(seen[4].before && seen[4].after && seen[4].before->id == fake.alpha.row && seen[4].after->id == fake.alpha.row);
		TEST_EXPECT(seen[5].before && seen[5].after && seen[5].before_position == 0);
	}
	// Accepted: the same Remove commits.
	document.veto = [](const Change &change) { return !(change.before && !change.after && change.before_position == 0); };
	TEST_EXPECT(document.apply(make(EditOperation::Remove, fake.beta), error) && document.rows().size() == 1);
	TEST_EXPECT(!document.apply(make(EditOperation::Remove, fake.alpha), error) && document.rows().size() == 1);
	document.veto = nullptr;
	document.undo();
	TEST_EXPECT(document.serialize().text == text);
	return 0;
}

// Batches, undo as a row swap, coalescing and gestures.
static int test_batches_and_gestures() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	// A batch is one step, and undo swaps the identical row back.
	const Node *before = document.rows()[0].get();
	TEST_EXPECT(document.apply(std::vector<Edit>{set(fake.a1, "name", std::string("A")), set(fake.x, "name", std::string("X")),
	                                             make(EditOperation::Remove, fake.y),
	                                             make(EditOperation::Add, {fake.alpha.row, kItem, 0}, 0, 0)},
	                           error));
	TEST_EXPECT(document.rows()[0].get() != before && document.address_of(fake.y.child) == NodeAddress());
	TEST_EXPECT(document.last_added_records().size() == 1 && document.address_of(document.last_added()).kind == kItem);
	document.undo();
	TEST_EXPECT(document.rows()[0].get() == before && !document.can_undo() && document.serialize().text == fake.original);
	document.redo();
	TEST_EXPECT(document.rows()[0].get() != before);
	document.undo();
	// A batch resolves each edit against the row as the edits before it left it: an item
	// removed, then its leaf, is refused, and nothing is committed.
	TEST_EXPECT(!document.apply(std::vector<Edit>{make(EditOperation::Remove, fake.a1), make(EditOperation::Remove, fake.x)}, error) &&
	            error.code() == "document.selection");
	// Refused: two rows, or a row-level edit, in one batch.
	TEST_EXPECT(!document.apply(std::vector<Edit>{set(fake.a1, "name", std::string("A")), set(fake.b1, "name", std::string("B"))}, error) &&
	            error.code() == "document.batch");
	TEST_EXPECT(!document.apply(std::vector<Edit>{set(fake.a1, "name", std::string("A")), make(EditOperation::Remove, fake.beta)}, error) &&
	            error.code() == "document.batch");
	TEST_EXPECT(document.rows()[0].get() == before && !document.can_undo());

	// Coalesced batches of the same fields fold into one step (a drag without a gesture),
	// each applied to the row as the group found it; the group ends at end_edit_group.
	auto drag = [&](int64_t weight, const char *name) {
		Edit a = set(fake.a1, "weight", weight), b = set(fake.a2, "name", std::string(name));
		a.coalesce = b.coalesce = true;
		return document.apply(std::vector<Edit>{a, b}, error);
	};
	TEST_EXPECT(drag(6, "p") && drag(7, "q") && drag(8, "r"));
	Value value;
	TEST_EXPECT(document.get(fake.a1, "weight", value) && std::get<int64_t>(value) == 8);
	document.end_edit_group();
	TEST_EXPECT(drag(9, "s"));
	document.undo();
	TEST_EXPECT(document.get(fake.a1, "weight", value) && std::get<int64_t>(value) == 8);
	document.undo();
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo());

	// A gesture: edits sharing it fold into one step on their row, each building on the
	// last (a leaf added, then renamed, then moved), until the group ends.
	const uint64_t gesture = next_edit_gesture();
	TEST_EXPECT(gesture != 0 && next_edit_gesture() != gesture);
	Edit add = make(EditOperation::Add, {0, kLeaf, 0}, fake.a2.child);
	add.gesture = gesture;
	TEST_EXPECT(document.apply(add, error));
	const NodeAddress leaf = document.address_of(document.last_added());
	Edit rename = set(leaf, "name", std::string("z"));
	rename.gesture = gesture;
	Edit move = make(EditOperation::Move, leaf, fake.a1.child, 0);
	move.gesture = gesture;
	TEST_EXPECT(document.apply(rename, error) && document.apply(move, error));
	Document::Placement at;
	TEST_EXPECT(document.placement(leaf, at) && at.owner == fake.a1 && at.index == 0);
	// The same gesture on another row is a step of its own.
	Edit other = set(fake.b1, "name", std::string("B"));
	other.gesture = gesture;
	TEST_EXPECT(document.apply(other, error));
	document.undo();
	TEST_EXPECT(document.address_of(leaf.child) == leaf);
	document.undo();
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo());
	// A gesture never swallows the saved checkpoint: the step after a save is its own.
	document.redo();
	TEST_EXPECT(document.save(error) && !document.dirty());
	Edit after = set(fake.a1, "name", std::string("saved-then"));
	after.gesture = gesture;
	TEST_EXPECT(document.apply(after, error) && document.dirty());
	document.undo();
	TEST_EXPECT(!document.dirty() && document.can_undo());
	return 0;
}

// A typing burst on one row: one coalesced Set per key of a1's name is one undo step, a value
// typed on the way (the name cleared) leaving nothing behind; a new name is its record's edit
// alone (Rename everywhere rewrites what names it: S13 D5 cut the same-file follow, which planned
// each keystroke's sites in the same step); a keystroke the type vetoes keeps the step as the one
// before left it, the group still open; the saved checkpoint ends the group. S13 D7's batch over
// several rows lands here as its multi-row case; the step over several changes it commits
// through is EditHistory's (test_joined_step).
static int test_typing_burst() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	const auto name = [&](const NodeAddress &address) {
		Value value;
		return document.get(address, "name", value) ? std::get<std::string>(value) : std::string("?");
	};
	const auto type = [&](const char *text) {
		Edit key = set(fake.a1, "name", std::string(text));
		key.coalesce = true;
		return document.apply(std::vector<Edit>{key}, error);
	};
	TEST_EXPECT(type("n") && type("") && name(fake.a1).empty());
	TEST_EXPECT(type("ne") && type("new") && name(fake.a1) == "new");
	TEST_EXPECT(name(fake.x) == "x" && name(fake.b1) == "b1");
	document.veto = [](const Change &) { return false; };
	TEST_EXPECT(!type("newer") && error.code() == "document.structure");
	document.veto = nullptr;
	TEST_EXPECT(name(fake.a1) == "new" && document.can_undo());
	TEST_EXPECT(type("news") && name(fake.a1) == "news");
	document.undo();
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo());
	// The saved checkpoint ends the group: the keystroke after a save is a step of its own.
	TEST_EXPECT(type("s") && document.save(error) && type("sa"));
	document.undo();
	TEST_EXPECT(name(fake.a1) == "s" && !document.dirty());
	document.undo();
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo());
	return 0;
}

// One step over several changes (EditHistory::commit of several, each joined to the one before
// it), which a batch over several rows commits through: undone and redone whole, and nothing
// folds into it, a change under its key after it being a step of its own. With no production
// caller since S13 D5 (S13 D7's multi-row step is the next), its group rules are held here: its
// key's reopen() undoes the whole step, back to the revision before it, and resume() redoes it;
// drop() forgets it and ends the group; the saved checkpoint is never reopened.
static int test_joined_step() {
	std::vector<std::shared_ptr<const Node>> rows;
	std::shared_ptr<const FileState> state;
	EditHistory history(rows, state);
	const auto group = [](NodeId id, const char *title) {
		auto node = std::make_shared<FakeGroup>();
		node->id = id;
		node->title = title;
		return std::shared_ptr<const Node>(node);
	};
	using Row = std::shared_ptr<const Node>;
	const auto change = [](const Row &before, Row after, size_t at) {
		Change out;
		out.before = before;
		out.after = std::move(after);
		out.before_position = out.after_position = at;
		return out;
	};
	const auto titles = [&] {
		std::string out;
		for (const auto &row : rows) out += row->name();
		return out;
	};
	rows = {group(1, "a"), group(2, "b")};
	const std::vector<Change> both{change(rows[0], group(1, "A"), 0),
	                               change(rows[1], group(2, "B"), 1)};
	history.commit(both, "k");
	TEST_EXPECT(titles() == "AB" && history.can_undo() && history.dirty());
	history.commit(change(rows[0], group(1, "Z"), 0), "k");
	TEST_EXPECT(titles() == "ZB");
	history.undo();
	TEST_EXPECT(titles() == "AB" && history.can_undo());
	history.undo();
	TEST_EXPECT(titles() == "ab" && !history.can_undo() && !history.dirty());
	history.redo();
	TEST_EXPECT(titles() == "AB" && history.can_redo());
	history.redo();
	TEST_EXPECT(titles() == "ZB" && !history.can_redo());

	std::vector<std::shared_ptr<const Node>> rows2 = {group(1, "a"), group(2, "b")};
	std::shared_ptr<const FileState> state2;
	EditHistory group_history(rows2, state2);
	const auto titles2 = [&] {
		std::string out;
		for (const auto &row : rows2) out += row->name();
		return out;
	};
	const auto commit_both = [&] {
		group_history.commit(std::vector<Change>{change(rows2[0], group(1, "A"), 0),
		                                         change(rows2[1], group(2, "B"), 1)},
		                     "k");
	};
	commit_both();
	TEST_EXPECT(titles2() == "AB" && group_history.revision() != 0);
	TEST_EXPECT(group_history.reopen("k") && titles2() == "ab" && group_history.revision() == 0);
	group_history.resume();
	TEST_EXPECT(titles2() == "AB" && group_history.can_undo() && !group_history.can_redo());
	TEST_EXPECT(group_history.reopen("k") && titles2() == "ab");
	group_history.drop();
	TEST_EXPECT(titles2() == "ab" && !group_history.can_undo() && !group_history.can_redo() &&
	            !group_history.dirty() && !group_history.reopen("k"));
	commit_both();
	group_history.mark_saved();
	TEST_EXPECT(!group_history.reopen("k") && titles2() == "AB" && !group_history.dirty());
	return 0;
}

// A batch that fills in what it makes (batch_made, S9m: the editor MCP's editor_menu edit):
// an item added into a1 (the row named by its owner), then named, weighed and given a leaf
// through batch_made (the leaf's edit naming no row), a leaf moved into it, a2 duplicated
// and the copy renamed: one undo step, redone whole; the selection the session makes of it
// is what no other made record holds. Refused with nothing committed: a batch_made naming a
// later edit, itself, an edit that made nothing, or another row.
static int test_batch_made() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	const std::vector<Edit> batch{
	        make(EditOperation::Add, {0, kItem, 0}, fake.a1.child),                    // 0: an item in a1
	        set({fake.alpha.row, kItem, batch_made(0)}, "name", std::string("fresh")), // 1
	        make(EditOperation::Add, {0, kLeaf, 0}, batch_made(0)),                    // 2: a leaf in it
	        set({0, kLeaf, batch_made(2)}, "name", std::string("leafy")),              // 3
	        make(EditOperation::Move, fake.y, batch_made(0), 0),                       // 4: y into it, first
	        make(EditOperation::Duplicate, fake.a2, 0, 2),                             // 5: a2 copied
	        set({0, kItem, batch_made(5)}, "name", std::string("a2copy")),             // 6
	        set({0, kItem, batch_made(0)}, "weight", int64_t(4)),                      // 7
	};
	TEST_EXPECT(document.apply(batch, error));
	const std::vector<NodeId> made = document.last_added_records();
	TEST_EXPECT(made.size() == 3);
	if (made.size() != 3) return 1;
	const NodeAddress fresh = document.address_of(made[0]), leafy = document.address_of(made[1]),
	                  copy = document.address_of(made[2]);
	Value value;
	TEST_EXPECT(document.record_path(fresh) == "alpha/a1/fresh" && document.record_path(leafy) == "alpha/a1/fresh/leafy");
	TEST_EXPECT(document.get(fresh, "weight", value) && std::get<int64_t>(value) == 4 && document.present(fresh, "weight"));
	Document::Placement at;
	TEST_EXPECT(document.placement(fake.y, at) && at.owner == fresh && at.index == 0);
	TEST_EXPECT(document.placement(leafy, at) && at.owner == fresh && at.index == 1);
	TEST_EXPECT(document.record_path(copy) == "alpha/a2copy" && document.placement(copy, at) && at.index == 2);
	// The session selects what no other made record holds: the new item and the copy.
	SessionView view;
	view.documents.select_added(document);
	TEST_EXPECT(view.documents.selected == std::vector<NodeAddress>({fresh, copy}) && view.documents.selection == fresh);
	document.undo();
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo());
	document.redo();
	TEST_EXPECT(document.record_path(document.address_of(made[1])) == "alpha/a1/fresh/leafy");
	document.undo();

	// Refused, nothing committed.
	const uint64_t revision = document.revision();
	TEST_EXPECT(!document.apply(std::vector<Edit>{set({fake.alpha.row, kItem, batch_made(1)}, "name", std::string("early")),
	                                              make(EditOperation::Add, {0, kItem, 0}, fake.a1.child)},
	                            error) &&
	            error.code() == "document.batch");
	TEST_EXPECT(!document.apply(set({fake.alpha.row, kItem, batch_made(0)}, "name", std::string("self")), error) &&
	            error.code() == "document.batch");
	TEST_EXPECT(!document.apply(std::vector<Edit>{set(fake.a1, "name", std::string("A")),
	                                              set({fake.alpha.row, kItem, batch_made(0)}, "name", std::string("B"))},
	                            error) &&
	            error.code() == "document.batch");
	TEST_EXPECT(!document.apply(std::vector<Edit>{make(EditOperation::Add, {0, kItem, 0}, fake.a1.child),
	                                              set({fake.beta.row, kItem, batch_made(0)}, "name", std::string("elsewhere"))},
	                            error) &&
	            error.code() == "document.batch");
	TEST_EXPECT(document.revision() == revision && document.serialize().text == fake.original && !document.can_undo());
	return 0;
}

// The clipboard seam: copy() makes the type's payload, a Paste takes it back.
static int test_clipboard() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	const std::string leaves = document.copy({fake.x, fake.y});
	TEST_EXPECT(leaves == "leaves\nL 1 x\nL 1 y\n");
	TEST_EXPECT(document.copy({fake.x, fake.a1}).empty() && document.copy({fake.header}).empty());
	Edit paste = make(EditOperation::Paste, {0, 0, 0}, fake.a2.child, 0);
	paste.value = leaves;
	TEST_EXPECT(document.apply(paste, error));
	const std::vector<NodeId> added = document.last_added_records();
	TEST_EXPECT(added.size() == 2 && document.last_added() == added.front());
	Value name;
	TEST_EXPECT(document.get(document.address_of(added[1]), "name", name) && std::get<std::string>(name) == "y");
	TEST_EXPECT(added[0] != fake.x.child && added[1] != fake.y.child);
	TEST_EXPECT(document.record_path(document.address_of(added[0])) == "alpha/a2/x");
	document.undo(); // one step
	TEST_EXPECT(document.serialize().text == fake.original && !document.can_undo());
	// An item with its subtree, pasted into the other group; leaves never at the top.
	const std::string items = document.copy({fake.a1});
	paste = make(EditOperation::Paste, {fake.beta.row, 0, 0}, 0, SIZE_MAX);
	paste.value = items;
	TEST_EXPECT(document.apply(paste, error));
	const NodeAddress copied = document.address_of(document.last_added());
	TEST_EXPECT(copied.row == fake.beta.row && document.record_path(copied) == "beta/a1");
	std::vector<NodeAddress> inside;
	document.walk_records(*document.row(fake.beta.row), [&](const NodeAddress &record, const Document::Placement &) {
		inside.push_back(record);
		return true;
	});
	TEST_EXPECT(inside.size() == 6); // header, b1, a1, x, y, a1b
	paste = make(EditOperation::Paste, {fake.alpha.row, 0, 0});
	paste.value = leaves;
	TEST_EXPECT(!document.apply(paste, error) && error.code() == "document.paste");
	paste = make(EditOperation::Paste, {0, 0, 0});
	paste.value = items;
	TEST_EXPECT(!document.apply(paste, error) && error.code() == "document.paste"); // at the top: rows paste later
	return 0;
}

// The selection the session keeps: one row, a primary among the selected, repaired after
// edits and undo.
static int test_selection() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	SessionView view;
	view.documents.select(document.path(), fake.x, SelectMode::Replace);
	view.documents.select(document.path(), fake.y, SelectMode::Add);
	TEST_EXPECT(view.documents.selection == fake.y &&
			view.documents.selected == std::vector<NodeAddress>({ fake.x, fake.y }));
	view.documents.select(document.path(), fake.y, SelectMode::Toggle);
	TEST_EXPECT(view.documents.selection == fake.x &&
			view.documents.selected == std::vector<NodeAddress>({ fake.x }));
	view.documents.select(document.path(), fake.a2, SelectMode::Toggle);
	TEST_EXPECT(view.documents.selection == fake.a2 && view.documents.selected.size() == 2);
	// Another row (or another document) starts over, whatever the mode.
	view.documents.select(document.path(), fake.b1, SelectMode::Add);
	TEST_EXPECT(view.documents.selection == fake.b1 &&
			view.documents.selected == std::vector<NodeAddress>({ fake.b1 }));
	view.documents.select("other.txt", fake.x, SelectMode::Add);
	TEST_EXPECT(view.documents.active == "other.txt" &&
			view.documents.selected == std::vector<NodeAddress>({ fake.x }));
	view.documents.select(document.path(), NodeAddress(), SelectMode::Replace);
	TEST_EXPECT(view.documents.selected.empty() && view.documents.selection == NodeAddress());

	// Removing the primary selects its owner.
	Diagnostic error;
	view.documents.select(document.path(), fake.x, SelectMode::Replace);
	view.documents.select(document.path(), fake.a1b, SelectMode::Add);
	TEST_EXPECT(document.apply(make(EditOperation::Remove, fake.a1b), error));
	view.documents.repair_selection(document, fake.a1);
	TEST_EXPECT(view.documents.selection == fake.a1 &&
			view.documents.selected == std::vector<NodeAddress>({ fake.a1 }));
	document.undo();
	// A record removed that was selected but not the primary just drops out.
	view.documents.select(document.path(), fake.x, SelectMode::Replace);
	view.documents.select(document.path(), fake.y, SelectMode::Add);
	view.documents.select(document.path(), fake.a2, SelectMode::Add);
	TEST_EXPECT(document.apply(make(EditOperation::Remove, fake.y), error));
	view.documents.repair_selection(document, fake.alpha);
	TEST_EXPECT(view.documents.selection == fake.a2 && view.documents.selected == std::vector<NodeAddress>({fake.x, fake.a2}));
	document.undo();
	// A record added is the selection; its undo leaves the last one still there, or none.
	TEST_EXPECT(document.apply(make(EditOperation::Duplicate, fake.a2, 0, 2), error));
	view.documents.select_added(document);
	const NodeAddress duplicate = document.address_of(document.last_added());
	TEST_EXPECT(view.documents.selection == duplicate && view.documents.selected == std::vector<NodeAddress>({duplicate}));
	document.undo();
	view.documents.repair_selection(document, NodeAddress());
	TEST_EXPECT(view.documents.selection == NodeAddress() && view.documents.selected.empty());
	return 0;
}

// S11a: what changed since the load or the last save (the saved baseline). A field and
// its record; a child's own field marks the child, not its owner; a child removed or moved
// marks the owner that keeps it; a record the baseline lacks is Added (and its owner
// changed); a row moved among the rows is changed, as is the one it passed; the file-wide
// state; an undo back to the checkpoint is unchanged again; saved_value reads a field as the
// saved file holds it (S11e: the inspector's tooltip) and revert_edits gives it its saved
// value and presence back as one undoable batch; a save moves the baseline; and a clean
// document whose file holds other bytes than it writes needs a rewrite.
static int test_changes_since_save() {
	using Change = Document::RecordChange;
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	TEST_EXPECT(!document.field_changed(fake.a1, "name") && document.record_change(fake.a1) == Change::Unchanged);
	TEST_EXPECT(document.record_change(fake.alpha) == Change::Unchanged && !document.file_state_changed());
	TEST_EXPECT(document.rewrite_need() == Document::RewriteNeed::None);

	// A child's own field.
	TEST_EXPECT(document.apply(set(fake.x, "name", std::string("xx")), error));
	TEST_EXPECT(document.field_changed(fake.x, "name") && document.record_change(fake.x) == Change::Changed);
	TEST_EXPECT(document.record_change(fake.a1) == Change::Unchanged && document.record_change(fake.alpha) == Change::Unchanged);
	TEST_EXPECT(!document.field_changed(fake.y, "name") && document.record_change(fake.y) == Change::Unchanged);
	TEST_EXPECT(!document.field_changed(fake.x, "no_such_field"));
	document.undo();
	TEST_EXPECT(!document.dirty() && !document.field_changed(fake.x, "name") && document.record_change(fake.x) == Change::Unchanged);

	// A child removed, then one moved within its owner: the owner that keeps them.
	TEST_EXPECT(document.apply(make(EditOperation::Remove, fake.y), error));
	TEST_EXPECT(document.record_change(fake.a1) == Change::Changed && document.record_change(fake.x) == Change::Unchanged);
	TEST_EXPECT(document.record_change(fake.alpha) == Change::Unchanged);
	document.undo();
	TEST_EXPECT(document.record_change(fake.a1) == Change::Unchanged);
	TEST_EXPECT(document.apply(make(EditOperation::Move, fake.y, 0, 0), error));
	TEST_EXPECT(document.record_change(fake.a1) == Change::Changed && document.record_change(fake.y) == Change::Unchanged);
	document.undo();

	// A record the baseline lacks: Added, its owner changed; a row too.
	TEST_EXPECT(document.apply(make(EditOperation::Add, {fake.alpha.row, kLeaf, 0}, fake.a2.child), error));
	const NodeAddress fresh = document.address_of(document.last_added());
	TEST_EXPECT(document.record_change(fresh) == Change::Added && document.field_changed(fresh, "name"));
	TEST_EXPECT(document.record_change(fake.a2) == Change::Changed && document.record_change(fake.a1) == Change::Unchanged);
	TEST_EXPECT(document.revert_edits(fresh, "name").empty()); // nothing saved to go back to
	Value nothing;
	TEST_EXPECT(!document.saved_value(fresh, "name", nothing)); // nor to show
	document.undo();
	TEST_EXPECT(document.apply(make(EditOperation::Add, {0, kGroup, 0}), error));
	const NodeAddress gamma{document.last_added(), kGroup, 0};
	TEST_EXPECT(document.record_change(gamma) == Change::Added && document.record_change(fake.alpha) == Change::Unchanged &&
	            document.record_change(fake.beta) == Change::Unchanged);
	document.undo();
	// A row moved among the rows: it and the one it passed.
	TEST_EXPECT(document.apply(make(EditOperation::Move, fake.beta, 0, 0), error));
	TEST_EXPECT(document.record_change(fake.beta) == Change::Changed && document.record_change(fake.alpha) == Change::Changed);
	TEST_EXPECT(document.record_change(fake.a1) == Change::Unchanged);
	document.undo();
	TEST_EXPECT(document.record_change(fake.beta) == Change::Unchanged && !document.dirty());

	// The file-wide state.
	Edit note;
	note.operation = EditOperation::SetFileValue;
	note.value = std::string("hello");
	TEST_EXPECT(document.apply(note, error) && document.file_state_changed());
	document.undo();
	TEST_EXPECT(!document.file_state_changed());

	// A typing burst folds into one step, each keystroke a revision of its own: the cached
	// answers follow every one, and the undo of the step.
	Edit typing = set(fake.x, "name", std::string("xy"));
	typing.coalesce = true;
	TEST_EXPECT(document.apply(typing, error) && document.field_changed(fake.x, "name"));
	typing.value = std::string("x");
	TEST_EXPECT(document.apply(typing, error) && !document.field_changed(fake.x, "name"));
	typing.value = std::string("xz");
	TEST_EXPECT(document.apply(typing, error) && document.field_changed(fake.x, "name"));
	document.undo();
	TEST_EXPECT(!document.dirty() && !document.field_changed(fake.x, "name"));

	// revert_edits. a1's weight left out: a Write gives it back. saved_value says what the
	// saved file holds: the value and that it is written.
	Edit clear = make(EditOperation::Clear, fake.a1);
	clear.field = "weight";
	TEST_EXPECT(document.apply(clear, error) && document.field_changed(fake.a1, "weight"));
	TEST_EXPECT(document.record_change(fake.a1) == Change::Changed);
	Value saved;
	bool kept = false;
	TEST_EXPECT(document.saved_value(fake.a1, "weight", saved, &kept) && kept && !document.present(fake.a1, "weight"));
	std::vector<Edit> back = document.revert_edits(fake.a1, "weight");
	TEST_EXPECT(back.size() == 1 && back[0].operation == EditOperation::Write);
	TEST_EXPECT(document.revert_edits(fake.a1, "name").empty()); // unchanged
	TEST_EXPECT(document.apply(back, error) && !document.field_changed(fake.a1, "weight"));
	TEST_EXPECT(document.serialize().text == fake.original);
	document.undo(); // the revert is one step
	TEST_EXPECT(document.field_changed(fake.a1, "weight") && !document.present(fake.a1, "weight"));
	document.undo();
	// a2's weight, left out in the baseline, set: a Set of its saved value, then a Clear.
	TEST_EXPECT(document.apply(set(fake.a2, "weight", int64_t(7)), error) && document.present(fake.a2, "weight"));
	TEST_EXPECT(document.saved_value(fake.a2, "weight", saved, &kept) && !kept && std::get<int64_t>(saved) == 0);
	back = document.revert_edits(fake.a2, "weight");
	TEST_EXPECT(back.size() == 2 && back[0].operation == EditOperation::Set && std::get<int64_t>(back[0].value) == 0 &&
	            back[1].operation == EditOperation::Clear);
	TEST_EXPECT(document.apply(back, error) && !document.field_changed(fake.a2, "weight") && !document.present(fake.a2, "weight"));
	TEST_EXPECT(document.serialize().text == fake.original);
	document.undo();
	TEST_EXPECT(document.field_changed(fake.a2, "weight"));
	document.undo();
	// A plain field: one Set.
	TEST_EXPECT(document.apply(set(fake.x, "name", std::string("xx")), error));
	back = document.revert_edits(fake.x, "name");
	TEST_EXPECT(back.size() == 1 && back[0].operation == EditOperation::Set && std::get<std::string>(back[0].value) == "x");
	TEST_EXPECT(document.saved_value(fake.x, "name", saved, &kept) && kept && std::get<std::string>(saved) == "x");

	// Saved: the baseline moves; an undo past the save is a change again. The answers are
	// cached per revision, and the save moves the baseline at the same revision: asked just
	// before it, the field answers anew just after.
	TEST_EXPECT(document.field_changed(fake.x, "name") && document.record_change(fake.x) == Change::Changed);
	TEST_EXPECT(document.save(error) && !document.dirty());
	TEST_EXPECT(!document.field_changed(fake.x, "name") && document.record_change(fake.x) == Change::Unchanged);
	document.undo();
	TEST_EXPECT(document.dirty() && document.field_changed(fake.x, "name") && document.record_change(fake.x) == Change::Changed);
	document.redo();
	TEST_EXPECT(!document.field_changed(fake.x, "name"));

	// A clean document whose file holds other bytes than it writes (a blank line the reader
	// skips) needs a rewrite; written, it does not.
	TEST_EXPECT(editor_test::write_text(fake.dir.file("spaced.txt"), std::string(kFile) + "\n"));
	FakeDocument spaced;
	TEST_EXPECT(spaced.load(fake.dir.file("spaced.txt"), "spaced.txt", opennova::editor::AssetKind::Unknown, "jo", error));
	TEST_EXPECT(!spaced.dirty() && spaced.rewrite_need() == Document::RewriteNeed::Rewrite);
	TEST_EXPECT(spaced.save(error) && spaced.rewrite_need() == Document::RewriteNeed::None);
	std::string written, message;
	TEST_EXPECT(read_file_text(fake.dir.file("spaced.txt"), written, message) && written == kFile);
	return 0;
}

// S13 D2: the model's per-call costs over a table of 5,000 lines, counted. An edit of one line
// clones it once. What changed since the save reads no field of a line whose committed row is
// the baseline's own, and each field of the edited line at most once a side, the answers kept
// while the two rows compared stand (an undo gives the saved row back, a redo the same edited
// one: neither reads a field). A record is found by its identity (test_record_index). A
// field's use points at its schema, the same entry of the type's table for every record of every
// document, its choices never copied; a type's refinement never makes a read-only field
// writable; a record's own choices reach the widgets and the find (a value found by its
// choice's name), never the graph's extraction.
static int test_per_call_costs() {
	using Change = Document::RecordChange;
	constexpr size_t kRows = 5000, kEdited = 2500;
	std::string text;
	for (size_t i = 0; i < kRows; ++i) text += "L line" + std::to_string(i) + " " + std::to_string(i) + " TAG1\n";
	FlatDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(bytes_of(text), "lines.txt", AssetKind::Unknown, "jo", error));
	TEST_EXPECT(document.rows().size() == kRows);
	const std::vector<FieldSchema> &table = document.fields(kLine);
	const auto line = [&](size_t i) {
		const Node &row = *document.rows()[i];
		return NodeAddress{row.id, row.kind, 0};
	};
	g_line_reads = 0;
	for (size_t i = 0; i < kRows; ++i) TEST_EXPECT(document.record_change(line(i)) == Change::Unchanged);
	TEST_EXPECT(g_line_reads == 0);

	const NodeAddress edited = line(kEdited);
	g_line_clones = 0;
	TEST_EXPECT(document.apply(set(edited, "weight", int64_t(-1)), error));
	TEST_EXPECT(g_line_clones == 1);
	g_line_reads = 0;
	for (size_t i = 0; i < kRows; ++i) {
		const size_t before = g_line_reads;
		const Change change = document.record_change(line(i));
		if (i == kEdited) TEST_EXPECT(change == Change::Changed && g_line_reads - before <= 2 * table.size());
		else TEST_EXPECT(change == Change::Unchanged && g_line_reads == before);
	}
	for (const FieldSchema &field : table) TEST_EXPECT(!document.field_changed(line(kEdited + 1), field.id));
	const size_t asked = g_line_reads;
	TEST_EXPECT(document.record_change(edited) == Change::Changed && g_line_reads == asked);
	TEST_EXPECT(document.field_changed(edited, "weight") && !document.field_changed(edited, "title"));
	const size_t fields_asked = g_line_reads;
	TEST_EXPECT(document.field_changed(edited, "weight") && g_line_reads == fields_asked);
	document.undo();
	TEST_EXPECT(document.record_change(edited) == Change::Unchanged && !document.field_changed(edited, "weight") &&
	            g_line_reads == fields_asked);
	document.redo();
	TEST_EXPECT(document.record_change(edited) == Change::Changed && g_line_reads == fields_asked);
	TEST_EXPECT(document.address_of(edited.row) == edited && document.address_of(line(kRows - 1).row) == line(kRows - 1) &&
	            document.address_of(NodeId(987654321)) == NodeAddress());

	// The field's use: the table's own entry, the same for every record of every document.
	const FieldSchema &tag = table[2];
	FlatDocument other;
	TEST_EXPECT(other.load_bytes(bytes_of("L one 1 TAG2\n"), "other.txt", AssetKind::Unknown, "jo", error));
	const FieldUse first = document.field_on(line(0), tag), again = document.field_on(line(0), tag);
	const FieldUse there = other.field_on({other.rows()[0]->id, kLine, 0}, other.fields(kLine)[2]);
	TEST_EXPECT(first.schema == &tag && again.schema == &tag && there.schema == &tag);
	TEST_EXPECT(first.schema->choices.size() == kTags && first.schema->choices.data() == tag.choices.data());
	TEST_EXPECT(sizeof(FieldUse) < sizeof(FieldSchema) / 2);
	TEST_EXPECT(table[3].read_only && document.field_on(line(0), table[3]).read_only);
	// A record's own choices: the widgets' and the find's (choices_on), a value found by its
	// choice's name as the Inspector shows it; the graph's extraction never asks.
	TEST_EXPECT(document.apply({set(line(3), "title", std::string("own3")), set(line(3), "tag", std::string("MINE"))},
	                           error));
	const FieldUse own_tag = document.field_on(line(3), tag);
	std::vector<FieldChoice> own;
	TEST_EXPECT(own_tag.own_choices && !first.own_choices);
	TEST_EXPECT(&document.choices_on(line(3), own_tag, own) == &own && own.size() == 2 && own[0].label == "Mine");
	TEST_EXPECT(&document.choices_on(line(0), first, own) == &tag.choices);
	const std::vector<DocumentHit> mine = find_in_document(document, "Mine");
	const std::vector<DocumentHit> named = find_in_document(document, "own3");
	TEST_EXPECT(mine.size() == 1 && mine[0].address == line(3) && mine[0].field == "tag" && mine[0].text == "Mine");
	TEST_EXPECT(named.size() == 1 && named[0].address == line(3));
	return 0;
}

// S13 D2's review: a record is found by its identity through an index of every record's row that
// each revision brings up to date one changed row at a time (its records taken out as its last
// indexed version held them, put in as it holds them now; a row no longer there taken out), made
// again by a save and a load. Over a Set, a nested record removed and duplicated, a row moved,
// removed and duplicated, and every undo and redo of them; several edits asked after the last of
// them only; a record pasted with what it holds; a save, an edit after it and undos past it; and
// the file loaded again in place (its identities start again, so none met before counts as
// gone), then a row moved: each record a walk meets is found where it is, and each one met
// before that is no longer there is found nowhere.
static int test_record_index() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	std::vector<NodeId> met;
	const auto every_found = [&] {
		bool found = true;
		std::vector<NodeId> here;
		for (const auto &row : document.rows()) {
			here.push_back(row->id);
			found = found && document.address_of(row->id) == NodeAddress{row->id, row->kind, 0};
			document.walk_records(*row, [&](const NodeAddress &record, const Document::Placement &) {
				here.push_back(record.child);
				found = found && document.address_of(record.child) == record;
				return true;
			});
		}
		for (const NodeId id : here)
			if (std::find(met.begin(), met.end(), id) == met.end()) met.push_back(id);
		for (const NodeId id : met)
			if (std::find(here.begin(), here.end(), id) == here.end()) found = found && document.address_of(id) == NodeAddress();
		return found;
	};
	TEST_EXPECT(every_found());
	TEST_EXPECT(document.apply(set(fake.x, "name", std::string("xx")), error) && every_found());
	TEST_EXPECT(document.apply(make(EditOperation::Remove, fake.y), error) && every_found());
	TEST_EXPECT(document.apply(make(EditOperation::Duplicate, fake.a1, 0, 2), error) && every_found());
	TEST_EXPECT(document.apply(make(EditOperation::Move, fake.beta, 0, 0), error) && every_found());
	TEST_EXPECT(document.apply(make(EditOperation::Duplicate, fake.beta, 0, 2), error) && every_found());
	TEST_EXPECT(document.apply(make(EditOperation::Remove, fake.beta), error) && every_found());
	TEST_EXPECT(document.rows().size() == 2 && met.size() > 12);
	while (document.can_undo()) {
		document.undo();
		TEST_EXPECT(every_found());
	}
	TEST_EXPECT(document.serialize().text == fake.original);
	while (document.can_redo()) {
		document.redo();
		TEST_EXPECT(every_found());
	}
	// Several edits, then one lookup: the index catches up over all of them at once.
	const auto last_row = [&] { return NodeAddress{document.rows().back()->id, document.rows().back()->kind, 0}; };
	TEST_EXPECT(document.apply(set(fake.a1, "name", std::string("A1")), error));
	TEST_EXPECT(document.apply(make(EditOperation::Remove, fake.x), error));
	TEST_EXPECT(document.apply(make(EditOperation::Duplicate, fake.a2, 0, 2), error));
	TEST_EXPECT(document.apply(make(EditOperation::Move, last_row(), 0, 0), error));
	TEST_EXPECT(every_found());
	// A record pasted with what it holds: every one it makes found in its new place.
	const std::string items = document.copy({fake.a1});
	Edit paste = make(EditOperation::Paste, {fake.alpha.row, 0, 0}, 0, SIZE_MAX);
	paste.value = items;
	TEST_EXPECT(!items.empty() && document.apply(paste, error) && document.last_added_records().size() == 1 &&
	            every_found());
	// Saved: the index is made again; an edit after the save, then undos past it.
	TEST_EXPECT(document.save(error) && every_found());
	TEST_EXPECT(document.apply(make(EditOperation::Remove, fake.a2), error) && every_found());
	document.undo();
	TEST_EXPECT(every_found());
	document.undo();
	TEST_EXPECT(every_found() && document.dirty());
	// Loaded again in place from the file the save wrote: the identities start again.
	TEST_EXPECT(document.load(fake.dir.file("fake.txt"), "fake.txt", AssetKind::Unknown, "jo", error));
	met.clear();
	TEST_EXPECT(every_found() && document.rows().size() == 2);
	TEST_EXPECT(document.apply(make(EditOperation::Move, last_row(), 0, 0), error) && every_found());
	return 0;
}

// S13 D2's review: a row's place among the rows depends on every row, so record_change asks it
// on each call and never keeps it with the row's own answer. Saved a, b, c: a edited and given
// back (its row a clone equal to the saved one); c moved to the top moves every row's place
// among the rows both sides have, and moved back moves none. The other order: c to the top
// first, a edited and given back while moved, c back: a is unchanged again.
static int test_moved_rows() {
	using Change = Document::RecordChange;
	using Changes = std::vector<Change>;
	const Change same = Change::Unchanged, moved = Change::Changed;
	for (const bool move_first : {false, true}) {
		FlatDocument document;
		Diagnostic error;
		TEST_EXPECT(document.load_bytes(bytes_of("L a 1 TAG1\nL b 2 TAG1\nL c 3 TAG1\n"), "abc.txt", AssetKind::Unknown,
		                                "jo", error));
		const NodeAddress a{document.rows()[0]->id, kLine, 0}, b{document.rows()[1]->id, kLine, 0},
		        c{document.rows()[2]->id, kLine, 0};
		const auto changes = [&] { return Changes{document.record_change(a), document.record_change(b), document.record_change(c)}; };
		const auto edit_and_give_back = [&] {
			return document.apply(set(a, "title", std::string("a2")), error) &&
			       document.apply(document.revert_edits(a, "title"), error) && !document.field_changed(a, "title");
		};
		if (!move_first) {
			TEST_EXPECT(edit_and_give_back());
			TEST_EXPECT(changes() == Changes({same, same, same}));
		}
		TEST_EXPECT(document.apply(make(EditOperation::Move, c, 0, 0), error));
		TEST_EXPECT(changes() == Changes({moved, moved, moved}));
		if (move_first) {
			TEST_EXPECT(edit_and_give_back());
			TEST_EXPECT(changes() == Changes({moved, moved, moved}));
		}
		TEST_EXPECT(document.apply(make(EditOperation::Move, c, 0, 2), error));
		TEST_EXPECT(changes() == Changes({same, same, same}));
	}
	return 0;
}

// S13 D2: a snapshot is the document as it stands, for another thread: the same committed
// rows, identity, revision, history and baseline, read only (an edit, an undo, a load and a
// save of it do nothing or are refused, document.snapshot). A thread reads it while the
// document edits on, and it keeps the rows it was made over.
static int test_snapshot() {
	using Change = Document::RecordChange;
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	TEST_EXPECT(document.apply(set(fake.x, "name", std::string("xx")), error));
	const std::string edited = document.serialize().text;
	std::unique_ptr<Document> snapshot = records_of(document.snapshot());
	TEST_EXPECT(snapshot && snapshot->is_snapshot() && !document.is_snapshot());
	TEST_EXPECT(snapshot->identity() == document.identity() && snapshot->revision() == document.revision() &&
	            snapshot->dirty() && snapshot->can_undo() && snapshot->rows() == document.rows());
	TEST_EXPECT(snapshot->serialize().text == edited && snapshot->record_change(fake.x) == Change::Changed &&
	            snapshot->field_changed(fake.x, "name") && snapshot->record_change(fake.y) == Change::Unchanged);
	TEST_EXPECT(!snapshot->apply(set(fake.x, "name", std::string("z")), error) && error.code() == "document.snapshot");
	snapshot->undo();
	TEST_EXPECT(snapshot->revision() == document.revision() && snapshot->serialize().text == edited);
	TEST_EXPECT(!snapshot->save(error) && error.code() == "document.snapshot");
	TEST_EXPECT(!snapshot->load_bytes(bytes_of(kFile), "fake.txt", AssetKind::Unknown, "jo", error) &&
	            error.code() == "document.snapshot");

	std::string read_there, locator_there;
	size_t records_there = 0;
	bool changed_there = false;
	std::thread reader([&] {
		for (int pass = 0; pass < 20; ++pass) {
			read_there = snapshot->serialize().text;
			records_there = 0;
			for (const auto &row : snapshot->rows())
				snapshot->walk_records(*row, [&](const NodeAddress &, const Document::Placement &) {
					++records_there;
					return true;
				});
			changed_there = snapshot->record_change(fake.x) == Change::Changed;
			locator_there = snapshot->locator(snapshot->address_of(fake.y.child));
		}
	});
	bool edits = true;
	for (int i = 0; i < 50; ++i) {
		Diagnostic refused;
		edits = document.apply(set(fake.y, "name", std::string("y") + std::to_string(i)), refused) && edits;
		edits = document.apply(make(EditOperation::Duplicate, fake.a2, 0, 2), refused) && edits;
		document.undo();
	}
	reader.join();
	TEST_EXPECT(edits);
	TEST_EXPECT(read_there == edited && records_there == 8 && changed_there && locator_there == "0/item:0/leaf:1");
	TEST_EXPECT(snapshot->serialize().text == edited && document.serialize().text != edited);
	return 0;
}

// S13 D6: a change the type makes in C++ (Edit Apply, its EditPayload), applied through the type's
// apply_payload to the record, what it holds and the file-wide state: one undo step, undone and
// redone byte for byte (a rename; a rename with the note; a leaf added under a fresh identity);
// one step with the Set of its batch; a gesture's Applies folding into one; none for a payload
// that changes nothing. Naming no row, the file-wide state alone (apply_file_payload): one step,
// a gesture's folding into one, none when it changes nothing. Refused, nothing committed: another
// type's payload or none (document.payload, the type's words or the base's), a batch holding one,
// a file payload naming an item's parts; and every one by a type that takes none (the defaults).
static int test_apply_payload() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	Value name;
	const auto apply = [&](const NodeAddress &address, std::shared_ptr<const EditPayload> payload,
	                       uint64_t gesture = 0) {
		Edit edit = make(EditOperation::Apply, address);
		edit.payload = std::move(payload);
		edit.gesture = gesture;
		return edit;
	};
	const auto name_of_a1 = [&] {
		return document.get(fake.a1, "name", name) ? std::get<std::string>(name) : std::string();
	};
	const auto note = [&] {
		const auto *state = static_cast<const FakeState *>(document.file_state());
		return state ? state->note : std::string();
	};
	const auto text = [&] { return document.serialize().text; };
	TEST_EXPECT(document.apply(apply(fake.a1, fake_change("renamed")), error));
	TEST_EXPECT(name_of_a1() == "renamed" && document.revision() == 1);
	TEST_EXPECT(document.dirty() && document.can_undo());
	const std::string renamed = text();
	TEST_EXPECT(renamed != fake.original);
	document.undo();
	TEST_EXPECT(text() == fake.original && !document.dirty());
	document.redo();
	TEST_EXPECT(text() == renamed && name_of_a1() == "renamed");
	// A payload that changes nothing (the name the item has) is no step.
	TEST_EXPECT(document.apply(apply(fake.a1, fake_change("renamed")), error));
	TEST_EXPECT(document.revision() == 1);
	// The record and the file-wide state in one step: the item renamed and the note written.
	TEST_EXPECT(document.apply(apply(fake.a1, fake_change("noted", "", "a note")), error));
	TEST_EXPECT(name_of_a1() == "noted" && note() == "a note" && document.revision() == 2);
	TEST_EXPECT(document.file_state_changed() && text().rfind("N a note\n", 0) == 0);
	document.undo();
	TEST_EXPECT(text() == renamed && note().empty() && !document.file_state_changed());
	// What the item holds: a leaf added under a fresh identity, found where it is, undone away.
	TEST_EXPECT(document.apply(apply(fake.a1, fake_change("", "sprout")), error));
	NodeId sprout = 0;
	for (const Document::Collection &collection : document.collections_of(fake.a1))
		if (collection.spec.kind == kLeaf && !collection.ids.empty())
			sprout = collection.ids.back();
	const NodeAddress sprouted = document.address_of(sprout);
	TEST_EXPECT(sprout != fake.x.child && sprout != fake.y.child && sprouted.kind == kLeaf &&
	            document.record_name(sprouted) == "sprout");
	document.undo();
	TEST_EXPECT(text() == renamed && !document.address_of(sprout).row);
	// With a Set in its batch: one step.
	const Edit again = apply(fake.a1, fake_change("again"));
	TEST_EXPECT(document.apply({again, set(fake.x, "name", std::string("xx"))}, error));
	TEST_EXPECT(name_of_a1() == "again" && document.revision() == 4);
	document.undo();
	TEST_EXPECT(text() == renamed);
	// A gesture's Applies fold into one step.
	const uint64_t gesture = next_edit_gesture();
	for (const char *new_name : {"g1", "g2", "g3"})
		TEST_EXPECT(document.apply(apply(fake.a1, fake_change(new_name), gesture), error));
	document.end_edit_group();
	TEST_EXPECT(name_of_a1() == "g3");
	document.undo();
	TEST_EXPECT(text() == renamed && name_of_a1() == "renamed");
	// Naming no row: the file-wide state alone, one step; the same note again is no step; a
	// gesture's fold into one.
	const uint64_t before_note = document.revision();
	TEST_EXPECT(document.apply(apply({}, fake_change("", "", "file")), error) && note() == "file");
	TEST_EXPECT(document.revision() != before_note && text() == "N file\n" + renamed);
	const uint64_t noted = document.revision();
	TEST_EXPECT(document.apply(apply({}, fake_change("", "", "file")), error));
	TEST_EXPECT(document.revision() == noted);
	document.undo();
	TEST_EXPECT(text() == renamed && note().empty());
	const uint64_t strokes = next_edit_gesture();
	for (const char *new_note : {"n1", "n2", "n3"})
		TEST_EXPECT(document.apply(apply({}, fake_change("", "", new_note), strokes), error));
	document.end_edit_group();
	TEST_EXPECT(note() == "n3");
	document.undo();
	TEST_EXPECT(text() == renamed && note().empty());
	// Refused, nothing committed: another's payload (the type's words), none at all, a batch
	// holding one, a file payload naming an item's parts, and one alongside other edits.
	struct Other : EditPayload {
		const char *token() const override { return "other"; }
	};
	const uint64_t revision = document.revision();
	const Edit other = apply(fake.a1, std::make_shared<Other>());
	TEST_EXPECT(!document.apply(other, error) && error.code() == "document.payload" &&
	            error.message == "Not a fake change.");
	TEST_EXPECT(!document.apply(make(EditOperation::Apply, fake.a1), error) &&
	            error.code() == "document.payload");
	TEST_EXPECT(!document.apply({set(fake.x, "name", std::string("zz")), other}, error) &&
	            error.code() == "document.payload");
	TEST_EXPECT(!document.apply(apply({}, fake_change("name")), error) &&
	            error.code() == "document.payload" &&
	            error.message == "A name or a leaf needs its item.");
	const Edit file_note = apply({}, fake_change("", "", "x"));
	TEST_EXPECT(!document.apply({file_note, set(fake.x, "name", std::string("zz"))}, error) &&
	            error.code() == "document.batch");
	TEST_EXPECT(document.revision() == revision && text() == renamed);
	// A type that takes none refuses every one (the defaults).
	FlatDocument flat;
	TEST_EXPECT(flat.load_bytes(bytes_of("L a 1 TAG1\n"), "flat.txt", AssetKind::Unknown, "jo",
	                            error));
	const std::string refused = "This document does not take that change.";
	TEST_EXPECT(!flat.apply(apply({flat.rows()[0]->id, kLine, 0}, fake_change("b")), error) &&
	            error.code() == "document.payload" && error.message == refused);
	TEST_EXPECT(!flat.apply(apply({}, fake_change("", "", "n")), error) &&
	            error.code() == "document.payload" && error.message == refused);
	TEST_EXPECT(flat.revision() == 0);
	return 0;
}

// S13 D6: a load that fails leaves the document as it was: its rows, path, kind, game, load
// generation and the file it matches.
static int test_failed_load() {
	Loaded fake;
	TEST_EXPECT(fake.load());
	FakeDocument &document = fake.document;
	Diagnostic error;
	const uint64_t generation = document.load_generation();
	const std::vector<std::shared_ptr<const Node>> rows = document.rows();
	TEST_EXPECT(!document.load_bytes(bytes_of("G broken\nI 3 too_deep\n"), "other.txt",
	                                 AssetKind::Menu, "dfx", error));
	TEST_EXPECT(error.code() == "document.parse" && error.asset == "other.txt");
	TEST_EXPECT(document.path() == "fake.txt" && document.kind() == AssetKind::Unknown &&
	            document.game_name() == "jo");
	TEST_EXPECT(document.load_generation() == generation && document.rows() == rows);
	TEST_EXPECT(document.serialize().text == fake.original && document.matches_file());
	TEST_EXPECT(!document.dirty());
	// A file that does not read leaves it so too.
	TEST_EXPECT(!document.load(fake.dir.file("missing.txt"), "missing.txt", AssetKind::Menu, "dfx",
	                           error) &&
	            error.code() == "document.read");
	TEST_EXPECT(document.path() == "fake.txt" && document.load_generation() == generation);
	return 0;
}

// S13 D6: the lifecycle alone, over a document of another kind than records (BlobDocument, the
// base alone): loaded (its fingerprint and a load generation taken, its ignored line an issue),
// changed by a payload its type made and by nothing else, undone and redone through its own
// history (dirty against its saved checkpoint), saved (the file holding what it serializes, the
// issue gone with the line, nothing more to rewrite), a save refused over a file changed outside
// the editor (document.conflict), a snapshot sharing it that the base refuses an edit, a save and
// a load (document.snapshot) and whose undo it ignores, loaded again in place under a new load
// generation, and a document read from bytes with no file to save to; a blocked one refused its
// edits (document.parse), its undo and redo, and its save (document.unserializable, the first
// blocking finding) by the base, the type testing neither; a load that fails leaving it as it
// was; and no records (as_records, records_of).
static int test_document_base() {
	using editor_test::BlobDocument;
	using editor_test::BlobReplace;
	editor_test::TempProjectDir dir{"opennova_document_base_test"};
	const std::string file = dir.file("blob.txt");
	const auto file_text = [&] {
		std::vector<uint8_t> bytes;
		std::string message;
		if (!read_file_bytes(file, bytes, message)) return std::string("<unread>");
		return std::string(bytes.begin(), bytes.end());
	};
	const std::string original = "one\n# a note\ntwo\n", edited = "one\ntwo\nthree\n";
	TEST_EXPECT(editor_test::write_text(file, original));
	BlobDocument blob;
	Diagnostic error;
	TEST_EXPECT(blob.load_generation() == 0);
	TEST_EXPECT(blob.load(file, "blob.txt", AssetKind::Unknown, "jo", error));
	const uint64_t first_load = blob.load_generation();
	TEST_EXPECT(first_load != 0 && blob.path() == "blob.txt" && blob.revision() == 0);
	TEST_EXPECT(!blob.dirty() && !blob.can_undo() && blob.history_bytes() == 0);
	TEST_EXPECT(blob.matches_file() && !blob.wrote_file());
	TEST_EXPECT(blob.issues().size() == 1 && blob.ignored_lines() == 1 && !blob.blocked());
	// The note it would drop: a rewrite.
	TEST_EXPECT(blob.rewrite_need() == DocumentBase::RewriteNeed::Rewrite);
	TEST_EXPECT(blob.as_records() == nullptr && records_of(blob) == nullptr);
	// A payload its type made changes it; anything else is refused, nothing changed.
	Edit replace;
	replace.operation = EditOperation::Apply;
	replace.payload = std::make_shared<BlobReplace>(edited);
	TEST_EXPECT(blob.apply(replace, error) && blob.blob() == edited && blob.revision() == 1);
	TEST_EXPECT(blob.dirty() && blob.can_undo() && blob.history_bytes() == original.size());
	TEST_EXPECT(!blob.apply(set({1, 0, 0}, "name", std::string("x")), error) &&
	            error.code() == "document.payload");
	TEST_EXPECT(blob.revision() == 1 && blob.blob() == edited);
	// Its own history: undone to the file as loaded (clean again), redone.
	blob.undo();
	TEST_EXPECT(blob.blob() == original && blob.revision() == 0 && !blob.dirty());
	TEST_EXPECT(blob.can_redo());
	blob.redo();
	TEST_EXPECT(blob.revision() == 1 && blob.dirty() && !blob.can_redo());
	// Saved: the file holds what it serializes, the note's issue is gone with the line.
	TEST_EXPECT(blob.save(error) && blob.wrote_file() && !blob.dirty() && blob.matches_file());
	TEST_EXPECT(file_text() == edited && blob.issues().empty() && blob.ignored_lines() == 0);
	TEST_EXPECT(blob.rewrite_need() == DocumentBase::RewriteNeed::None);
	blob.undo();
	TEST_EXPECT(blob.dirty());
	blob.redo();
	TEST_EXPECT(!blob.dirty()); // back at the saved checkpoint
	// A file changed outside the editor: it no longer matches, and a save is refused.
	TEST_EXPECT(editor_test::write_text(file, "changed\n") && !blob.matches_file());
	replace.payload = std::make_shared<BlobReplace>("four\n");
	TEST_EXPECT(blob.apply(replace, error) && !blob.save(error) &&
	            error.code() == "document.conflict");
	TEST_EXPECT(file_text() == "changed\n" && blob.dirty());
	// A snapshot shares it and takes nothing: the base refuses for the type.
	const std::unique_ptr<DocumentBase> snapshot = blob.snapshot();
	TEST_EXPECT(snapshot && snapshot->is_snapshot() && !blob.is_snapshot());
	TEST_EXPECT(!snapshot->as_records() && snapshot->identity() == blob.identity());
	TEST_EXPECT(snapshot->load_generation() == blob.load_generation() &&
	            snapshot->revision() == blob.revision() && snapshot->serialize().text == "four\n");
	TEST_EXPECT(!snapshot->apply(replace, error) && error.code() == "document.snapshot");
	snapshot->undo();
	TEST_EXPECT(snapshot->revision() == blob.revision() &&
	            snapshot->serialize().text == "four\n");
	TEST_EXPECT(!snapshot->save(error) && error.code() == "document.snapshot");
	TEST_EXPECT(!snapshot->load_bytes(bytes_of("x\n"), "blob.txt", AssetKind::Unknown, "jo",
	                                  error) &&
	            error.code() == "document.snapshot");
	// Loaded again in place: a new load generation, the revisions from 0, the file matching.
	TEST_EXPECT(blob.load(file, "blob.txt", AssetKind::Unknown, "jo", error));
	TEST_EXPECT(blob.load_generation() > first_load && blob.revision() == 0);
	TEST_EXPECT(!blob.dirty() && !blob.can_undo() && blob.blob() == "changed\n");
	TEST_EXPECT(blob.matches_file() && !blob.wrote_file());
	// A load that does not read leaves it as it was: path, kind, game, generation, the file.
	const uint64_t loaded = blob.load_generation();
	TEST_EXPECT(!blob.load_bytes(bytes_of("FAIL\n"), "other.txt", AssetKind::Menu, "dfx", error) &&
	            error.code() == "document.parse");
	TEST_EXPECT(blob.path() == "blob.txt" && blob.kind() == AssetKind::Unknown &&
	            blob.game_name() == "jo");
	TEST_EXPECT(blob.load_generation() == loaded && blob.blob() == "changed\n");
	TEST_EXPECT(blob.matches_file());
	// Blocked by what its own save wrote (a line it cannot carry, read back): the base refuses its
	// edit and its save, and does nothing on its undo or redo, though its history holds a step.
	replace.payload = std::make_shared<BlobReplace>("one\n! cannot carry\n");
	TEST_EXPECT(blob.apply(replace, error) && blob.save(error));
	TEST_EXPECT(blob.blocked() && blob.can_undo());
	const uint64_t blocked_at = blob.revision();
	replace.payload = std::make_shared<BlobReplace>("five\n");
	TEST_EXPECT(!blob.apply(replace, error) && error.code() == "document.parse" &&
	            blob.revision() == blocked_at);
	TEST_EXPECT(!blob.save(error) && error.code() == "document.unserializable" &&
	            error.message == "The blob cannot carry this line.");
	TEST_EXPECT(blob.rewrite_need() == DocumentBase::RewriteNeed::Unserializable);
	blob.undo();
	TEST_EXPECT(blob.revision() == blocked_at && blob.blob() == "one\n! cannot carry\n");
	blob.redo();
	TEST_EXPECT(blob.revision() == blocked_at && file_text() == "one\n! cannot carry\n");
	// Read from bytes: no file to save to.
	BlobDocument bytes;
	TEST_EXPECT(bytes.load_bytes(bytes_of("b\n"), "b.txt", AssetKind::Unknown, "jo", error));
	TEST_EXPECT(bytes.load_generation() > blob.load_generation());
	TEST_EXPECT(!bytes.save(error) && error.code() == "document.no_file");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_document_base();
	failures += test_apply_payload();
	failures += test_failed_load();
	failures += test_per_call_costs();
	failures += test_moved_rows();
	failures += test_record_index();
	failures += test_snapshot();
	failures += test_changes_since_save();
	failures += test_structure();
	failures += test_structural_edits();
	failures += test_add_with_a_value();
	failures += test_set_same_value();
	failures += test_veto();
	failures += test_batches_and_gestures();
	failures += test_typing_burst();
	failures += test_joined_step();
	failures += test_batch_made();
	failures += test_clipboard();
	failures += test_selection();
	if (failures == 0) std::printf("editor_document_core: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
