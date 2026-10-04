#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document.h>
#include <editor/session/request_factories.h>
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
	if (!target.editable) return workspace.request(request::show_in_files(target.file));
	workspace.request(request::open_document(target.file, target.locator, target.field));
}

inline void select(Workspace &workspace, const Document &document, NodeAddress address,
                   SelectMode mode = SelectMode::Replace) {
	workspace.request(request::select_record(document.path(), address, mode));
}

// `parent`: an Add's owner, or a Move's destination owner (0 = the row / unchanged).
inline void edit(Workspace &workspace, const Document &document, EditOperation operation, NodeAddress address,
                 size_t position = SIZE_MAX, NodeId parent = 0) {
	Edit change;
	change.operation = operation;
	change.address = address;
	change.position = position;
	change.parent = parent;
	workspace.request(request::edit_record(document.path(), std::move(change)));
}

// One prepared edit (a Move a drop or an Indent computed).
inline void edit(Workspace &workspace, const Document &document, const Edit &change) {
	workspace.request(request::edit_record(document.path(), change));
}

// A batch over any rows, one undo step (a drag's step: the edges it writes).
inline void edits(Workspace &workspace, const Document &document, std::vector<Edit> batch) {
	workspace.request(request::edit_record(document.path(), std::move(batch)));
}

// The coalesced edit group, or the gesture, of the document at `path` ends.
inline void end_edit(Workspace &workspace, const std::string &path) {
	workspace.request(request::end_edit(path));
}

// An Add whose new record has `field` := `value` in the same step (a window of a type).
inline void add_with(Workspace &workspace, const Document &document, NodeAddress address, NodeId parent,
                     const std::string &field, Value value) {
	Edit add;
	add.operation = EditOperation::Add;
	add.address = address;
	add.parent = parent;
	add.field = field;
	add.value = std::move(value);
	workspace.request(request::edit_record(document.path(), std::move(add)));
}

// Copy / Cut the selected records onto the session's clipboard, Paste it after the
// selection (the session's own target rule), or Duplicate them (each selected record
// copied right after itself, one step): `kind` one of the four, each naming its document.
inline void clipboard(Workspace &workspace, const Document &document, EditorRequestKind kind) {
	EditorRequest request = request::of(kind);
	request.path = document.path();
	workspace.request(std::move(request));
}

// Paste the clipboard into `parent` (0 = the row's own list) of the row `row` at `position`:
// a target the window names instead of the session's rule.
inline void paste(Workspace &workspace, const Document &document, NodeAddress row, NodeId parent, size_t position) {
	workspace.request(request::paste(document.path(), PasteAt{row.row, parent, position}));
}

// A Set; `coalesce` folds a typing burst into one undo step (Edit::coalesce).
inline void set(Workspace &workspace, const Document &document, NodeAddress address, const std::string &field,
                Value value, bool coalesce = true) {
	Edit set;
	set.address = address;
	set.field = field;
	set.value = std::move(value);
	set.coalesce = coalesce;
	workspace.request(request::edit_record(document.path(), std::move(set)));
}

// Each of `fields` (a field, or the members of a group drawn on one row) of each of `records`
// given back what the saved file holds (Revert to saved): one batch, one undo step.
inline void revert(Workspace &workspace, const Document &document, const std::vector<NodeAddress> &records,
                   const std::vector<std::string> &fields) {
	std::vector<Edit> targets;
	for (const std::string &field : fields)
		for (const NodeAddress &record : records) {
			Edit target;
			target.address = record;
			target.field = field;
			targets.push_back(std::move(target));
		}
	workspace.request(request::revert_to_saved(document.path(), std::move(targets)));
}

// An optional field written with the value it reads (Edit Write), or left out of the
// file (Edit Clear).
inline void set_written(Workspace &workspace, const Document &document, NodeAddress address, const std::string &field,
                        bool written) {
	Edit change;
	change.operation = written ? EditOperation::Write : EditOperation::Clear;
	change.address = address;
	change.field = field;
	workspace.request(request::edit_record(document.path(), std::move(change)));
}

// What a window shows of its own changed (set_workspace, the MCP gaps lane): `part`'s `members`, an
// object of the members it names, each left out as it is.
inline void set_workspace(Workspace &workspace, const char *part, io::JsonValue members) {
	io::JsonValue change = io::JsonValue::make_object();
	change.set(part, std::move(members));
	workspace.request(request::set_workspace(io::json_write(change)));
}
// One member of one part.
inline void set_workspace(Workspace &workspace, const char *part, const char *member, io::JsonValue value) {
	io::JsonValue members = io::JsonValue::make_object();
	members.set(member, std::move(value));
	set_workspace(workspace, part, std::move(members));
}
// A window brought forward by its token (files, document, preview, inspector, problems, output).
inline void focus(Workspace &workspace, const char *window) {
	io::JsonValue change = io::JsonValue::make_object();
	change.set("focus", io::JsonValue::make_string(window));
	workspace.request(request::set_workspace(io::json_write(change)));
}

// True when `name` contains `filter`, compared as the game compares names.
inline bool matches(const std::string &name, const char *filter) {
	return normalized_logical_name(name).find(normalized_logical_name(filter)) != std::string::npos;
}

} // namespace opennova::editor::window_requests
