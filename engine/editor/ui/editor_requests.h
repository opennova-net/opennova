#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document.h>
#include <editor/ui/workspace.h>

// The requests the document windows raise for a record (select it, add / duplicate /
// remove / move it, set one of its fields, leave an optional one out of the file or write
// it again, copy, cut and paste, go to where a reference or a use leads) and the name filter
// their lists share.
namespace opennova::editor::window_requests {

// Go to a target (reference_targets, usage_target): a file the editor edits opened
// at the record (found by its locator, this same document's too) with its field shown; a file
// the editor does not edit selected in Files.
inline void go_to(Workspace &workspace, const ReferenceTarget &target) {
	if (!target.editable) return workspace.request(make_request(EditorRequestKind::ShowInFiles, target.file));
	auto request = make_request(EditorRequestKind::OpenDocument, target.file, target.locator);
	request.edit.field = target.field;
	workspace.request(std::move(request));
}

inline void select(Workspace &workspace, const Document &document, NodeAddress address,
                   SelectMode mode = SelectMode::Replace) {
	auto request = make_request(EditorRequestKind::SelectRecord, document.path());
	request.edit.address = address;
	request.select_mode = mode;
	workspace.request(std::move(request));
}

// `parent`: an Add's owner, or a Move's destination owner (0 = the row / unchanged).
inline void edit(Workspace &workspace, const Document &document, EditOperation operation, NodeAddress address,
                 size_t position = SIZE_MAX, NodeId parent = 0) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.operation = operation;
	request.edit.address = address;
	request.edit.position = position;
	request.edit.parent = parent;
	workspace.request(std::move(request));
}

// One prepared edit (a Move a drop or an Indent computed).
inline void edit(Workspace &workspace, const Document &document, const Edit &change) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit = change;
	workspace.request(std::move(request));
}

// A batch on one row, one undo step (a drag's step: the edges it writes).
inline void edits(Workspace &workspace, const Document &document, std::vector<Edit> batch) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edits = std::move(batch);
	workspace.request(std::move(request));
}

// The coalesced edit group, or the gesture, of the document at `path` ends.
inline void end_edit(Workspace &workspace, const std::string &path) {
	workspace.request(make_request(EditorRequestKind::EndEdit, path));
}

// An Add whose new record has `field` := `value` in the same step (a window of a type).
inline void add_with(Workspace &workspace, const Document &document, NodeAddress address, NodeId parent,
                     const std::string &field, Value value) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.operation = EditOperation::Add;
	request.edit.address = address;
	request.edit.parent = parent;
	request.edit.field = field;
	request.edit.value = std::move(value);
	workspace.request(std::move(request));
}

// Copy / Cut the selected records onto the session's clipboard, Paste it after the
// selection (the session's own target rule), or Duplicate them (each selected record
// copied right after itself, one step).
inline void clipboard(Workspace &workspace, const Document &document, EditorRequestKind kind) {
	workspace.request(make_request(kind, document.path()));
}

// Paste the clipboard into `parent` (0 = the row's own list) of the row `row` at `position`:
// a target the window names instead of the session's rule.
inline void paste(Workspace &workspace, const Document &document, NodeAddress row, NodeId parent, size_t position) {
	auto request = make_request(EditorRequestKind::Paste, document.path());
	request.edit.address = row;
	request.edit.parent = parent;
	request.edit.position = position;
	workspace.request(std::move(request));
}

// A Set; `coalesce` folds a typing burst into one undo step (Edit::coalesce).
inline void set(Workspace &workspace, const Document &document, NodeAddress address, const std::string &field,
                Value value, bool coalesce = true) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.address = address;
	request.edit.field = field;
	request.edit.value = std::move(value);
	request.edit.coalesce = coalesce;
	workspace.request(std::move(request));
}

// Each of `fields` (a field, or the members of a group drawn on one row) of each of `records`
// given back what the saved file holds (Revert to saved): one batch, one undo step.
inline void revert(Workspace &workspace, const Document &document, const std::vector<NodeAddress> &records,
                   const std::vector<std::string> &fields) {
	auto request = make_request(EditorRequestKind::RevertToSaved, document.path());
	for (const std::string &field : fields)
		for (const NodeAddress &record : records) {
			Edit target;
			target.address = record;
			target.field = field;
			request.edits.push_back(std::move(target));
		}
	workspace.request(std::move(request));
}

// An optional field written with the value it reads (Edit Write), or left out of the
// file (Edit Clear).
inline void set_written(Workspace &workspace, const Document &document, NodeAddress address, const std::string &field,
                        bool written) {
	auto request = make_request(EditorRequestKind::EditRecord, document.path());
	request.edit.operation = written ? EditOperation::Write : EditOperation::Clear;
	request.edit.address = address;
	request.edit.field = field;
	workspace.request(std::move(request));
}

// True when `name` contains `filter`, compared as the game compares names.
inline bool matches(const std::string &name, const char *filter) {
	return normalized_logical_name(name).find(normalized_logical_name(filter)) != std::string::npos;
}

} // namespace opennova::editor::window_requests
