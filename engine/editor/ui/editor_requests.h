#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/model/document.h>
#include <editor/ui/editor_host.h>

// The requests the document windows raise for a record (select it, add / duplicate /
// remove / move it, set one of its fields, leave an optional one out of the file or write
// it again, copy, cut and paste, go to where a reference or a use leads) and the name filter
// their lists share.
namespace opennova::editor::window_requests {

// Go to a target (Document::reference_targets, usage_target): a file the editor edits opened
// at the record (found by its locator, this same document's too) with its field shown; a file
// the editor does not edit selected in Files.
inline void go_to(EditorHost &host, const ReferenceTarget &target) {
	if (!target.editable) return host.request(make_request(EditorRequestKind::ShowInFiles, target.file));
	auto request = make_request(EditorRequestKind::OpenDocument, target.file, target.locator);
	request.edit.field = target.field;
	host.request(std::move(request));
}

inline void select(EditorHost &host, const Document &document, NodeAddress address,
                   SelectMode mode = SelectMode::Replace) {
	auto request = make_request(EditorRequestKind::SelectRecord, document.path());
	request.edit.address = address;
	request.select_mode = mode;
	host.request(std::move(request));
}

// `parent`: an Add's owner, or a Move's destination owner (0 = the row / unchanged).
inline void edit(EditorHost &host, const Document &document, EditOperation operation, NodeAddress address,
                 size_t position = SIZE_MAX, NodeId parent = 0) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.operation = operation;
	request.edit.address = address;
	request.edit.position = position;
	request.edit.parent = parent;
	host.request(std::move(request));
}

// One prepared edit (a Move a drop or an Indent computed).
inline void edit(EditorHost &host, const Document &document, const Edit &change) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit = change;
	host.request(std::move(request));
}

// A batch on one row, one undo step (a drag's step: the edges it writes).
inline void edits(EditorHost &host, const Document &document, std::vector<Edit> batch) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edits = std::move(batch);
	host.request(std::move(request));
}

// The coalesced edit group, or the gesture, of the document at `path` ends.
inline void end_edit(EditorHost &host, const std::string &path) {
	host.request(make_request(EditorRequestKind::EndEdit, path));
}

// An Add whose new record has `field` := `value` in the same step (a window of a type).
inline void add_with(EditorHost &host, const Document &document, NodeAddress address, NodeId parent,
                     const std::string &field, Value value) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.operation = EditOperation::Add;
	request.edit.address = address;
	request.edit.parent = parent;
	request.edit.field = field;
	request.edit.value = std::move(value);
	host.request(std::move(request));
}

// Copy / Cut the selected records onto the session's clipboard, Paste it after the
// selection (the session's own target rule), or Duplicate them (each selected record
// copied right after itself, one step).
inline void clipboard(EditorHost &host, const Document &document, EditorRequestKind kind) {
	host.request(make_request(kind, document.path()));
}

// Paste the clipboard into `parent` (0 = the row's own list) of the row `row` at `position`:
// a target the window names instead of the session's rule.
inline void paste(EditorHost &host, const Document &document, NodeAddress row, NodeId parent, size_t position) {
	auto request = make_request(EditorRequestKind::Paste, document.path());
	request.edit.address = row;
	request.edit.parent = parent;
	request.edit.position = position;
	host.request(std::move(request));
}

// A Set; `coalesce` folds a typing burst into one undo step (Edit::coalesce).
inline void set(EditorHost &host, const Document &document, NodeAddress address, const std::string &field,
                Value value, bool coalesce = true) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.address = address;
	request.edit.field = field;
	request.edit.value = std::move(value);
	request.edit.coalesce = coalesce;
	host.request(std::move(request));
}

// Each of `fields` (a field, or the members of a group drawn on one row) of each of `records`
// given back what the saved file holds (Revert to saved): one batch, one undo step.
inline void revert(EditorHost &host, const Document &document, const std::vector<NodeAddress> &records,
                   const std::vector<std::string> &fields) {
	auto request = make_request(EditorRequestKind::RevertToSaved, document.path());
	for (const std::string &field : fields)
		for (const NodeAddress &record : records) {
			Edit target;
			target.address = record;
			target.field = field;
			request.edits.push_back(std::move(target));
		}
	host.request(std::move(request));
}

// An optional field written with the value it reads (Edit Write), or left out of the
// file (Edit Clear).
inline void set_written(EditorHost &host, const Document &document, NodeAddress address, const std::string &field,
                        bool written) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.operation = written ? EditOperation::Write : EditOperation::Clear;
	request.edit.address = address;
	request.edit.field = field;
	host.request(std::move(request));
}

// True when `name` contains `filter`, compared as the game compares names.
inline bool matches(const std::string &name, const char *filter) {
	return normalized_logical_name(name).find(normalized_logical_name(filter)) != std::string::npos;
}

} // namespace opennova::editor::window_requests
