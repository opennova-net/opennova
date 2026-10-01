#include <editor/session/request_fields.h>

#include <iterator>

namespace opennova::editor {

namespace {

using F = RequestFieldId;
using J = RequestJson;

// What each field of a request means, whatever the kind that takes it: the kind's own row says
// what it does with it (request_kinds.cpp).
constexpr RequestField kFields[] = {
	{ F::Dir, "dir", J::String, "A project's directory on disk." },
	{ F::Title, "title", J::String, "A new project's title (its folder's name when left out)." },
	{ F::Game, "game", J::String,
			"A new project's game, a gameprofile code (jo, jodemo, dfx, dfx2, bhd; jo when left "
			"out)." },
	{ F::GameInstall, "game_install", J::String,
			"A game install the project opens with for the session alone, in place of the one its "
			".opennova/local.json names, which stays as it is (a dry run's install); left out, its "
			"own." },
	{ F::Path, "path", J::String,
			"A file: a project file or an open document by its project-relative path or its "
			"logical "
			"name (left out, the active document, where the kind acts on one); a source to import "
			"again; "
			"for reveal_path, a file or folder on disk." },
	{ F::Locator, "locator", J::String,
			"A record by its locator, the place a reload finds it again by (as the record and the "
			"reference queries give it)." },
	{ F::Field, "field", J::String, "A field of that record, by its id." },
	{ F::NewName, "new_name", J::String,
			"The name a rename gives: a file's new logical name, or a defined name's new name." },
	{ F::Role, "role", J::String, "A requirement's role, as the requirements' rows name it." },
	{ F::FileKind, "file_kind", J::String,
			"An asset kind's token, for a file whose name cannot say its kind (a .bin)." },
	{ F::OutDir, "out_dir", J::String,
			"Where a build lands: a directory on disk, each build a directory under it named by "
			"its id (left out, the project's .opennova/build/play; a relative one from the "
			"project's folder; one inside the project refused but its cache or export folder)." },
	{ F::Roles, "roles", J::Strings, "Requirements' roles." },
	{ F::Names, "names", J::Strings, "Files of the game install, by logical name." },
	{ F::Paths, "paths", J::Strings,
			"Files on disk to import: a loose file is chosen, an archive's members are listed to "
			"choose "
			"from." },
	{ F::Imports, "imports", J::Objects,
			"Files chosen to import, {path, entry?, install?, native?}, each as the view's import rows "
			"carry it "
			"(install: the path is the game install; native: a loose file copied as the game's "
			"own)." },
	{ F::Edits, "edits", J::Objects,
			"Edits over any rows of one document in the batch form, one undo step: [{op, id, "
			"parent, kind, field, value, position, as, coalesce, gesture}], op one of set, clear, "
			"write, add, duplicate, remove, move, set_file_value or replace_list ({op, id, list, "
			"records}: the list of that kind the record holds replaced by records, each {field: "
			"value}); a record or a row by its identity or by the label (as) an earlier add or "
			"duplicate of the batch gave it, an add's kind by its token. revert_to_saved's: [{id, "
			"field}]. Over a text document its spans replaced: [{op: apply, payload: text.span, "
			"line, column, length, text}]. Every op and member: editor_query catalog's batch." },
	{ F::Address, "address", J::Object, "A record by its address, {row, kind, child}." },
	{ F::Records, "records", J::Objects,
			"Records by their addresses, [{row, kind, child}]: those a selection takes with the "
			"address (of any rows of the document)." },
	{ F::PasteAt, "paste_at", J::Object,
			"Where a paste goes, {row, parent, position}: into the owner parent (0 = the row) at "
			"position; left out, after the selection." },
	{ F::Mode, "mode", J::String, "How a record joins the selection: replace, add or toggle." },
	{ F::Choice, "choice", J::String,
			"The unsaved-changes prompt's answer: save, discard or cancel." },
	{ F::Settings, "settings", J::Object,
			"The settings to set, {serial?, title?, mission?, multiplayer?, game_install?, "
			"runtime_executable?, play_in_install?}, each left out as it is." },
	{ F::Viewport, "viewport", J::Object,
			"A viewport's change, {kind?, device?, clock?, options?, camera?}: kind its kind's token "
			"(menu, model; left out, the kind the document shows in: the Preview's kind that shows "
			"it, else its Main view), device {width, height} the size its device draws at (1 to "
			"8192), clock {playing, rate, time_ms, ticks} the preview clock, options the kind's (a "
			"menu's show_hidden, force_id, force_state, checked, popup_open, focus; a model's lod, "
			"ctrl, overlays, rig_model), camera a model's {yaw, pitch, distance, target, frame}, each "
			"member optional." },
	{ F::Drag, "drag", J::Object,
			"A drag in a viewport, {id, handle, by | to, snap?, gesture?, end?}: the record id's "
			"handle (a menu window's move, left, right, top, bottom, top_left, top_right, "
			"bottom_left or bottom_right; a model marker's place or axis) dragged by [dx, dy] from "
			"where the picture shows it now, or to the point [x, y] of the picture, in the "
			"viewport's units (a menu's 800x600 design units, a model's picture pixels); snap a "
			"menu's grid of 8 when not 0, a model's grid in metres (0, free, when left out); gesture "
			"the gesture it goes on with (the token an earlier drag's answer gave; left out, a new "
			"one); end false keeps the gesture open for the next drag (true when left out)." },
	{ F::Command, "command", J::Object,
			"A command in a viewport, {name, ids?}: a menu's arrange of the windows ids, the first "
			"the one the others follow (align_left, align_right, align_top, align_bottom, "
			"align_horizontal_centers, align_vertical_centers; distribute_horizontally and "
			"distribute_vertically, three or more; bring_to_front, bring_forward, send_backward, "
			"send_to_back), or a model's frame (its camera on the marker of the first id, else on "
			"the whole model)." },
	{ F::Purpose, "purpose", J::String,
			"What a picked path is for: new_project_location, open_project, runtime_executable, "
			"game_install or import_files." },
	{ F::WithDependencies, "with_dependencies", J::Boolean,
			"An import brings the files the chosen ones need." },
	{ F::Replace, "replace", J::Boolean,
			"An import replaces the project's files of the same names." },
	{ F::Force, "force", J::Boolean, "A source imports again even when it did not change." },
	{ F::AskName, "ask_name", J::Boolean,
			"And asks the new name (Files' Rename..., the Rename everywhere dialog)." },
	{ F::OpenFirst, "open_first", J::Boolean,
			"The document opens first when it is not open (a fix's edit)." },
	{ F::ImportPass, "import_pass", J::Boolean,
			"The project opens with its import pass, the sources that changed imported first "
			"(true when left out); false: it opens on its files as they are, scanned and checked, "
			"no source imported (a dry run's read, a project made in a folder that holds sources)." },
	{ F::Rehash, "rehash", J::Boolean,
			"A build reads every file again, the build cache's hashes set aside (and kept afresh)." },
};

static_assert(std::size(kFields) == kRequestFieldCount, "every request field has exactly one row");

constexpr bool fields_in_order() {
	for (size_t i = 0; i < kRequestFieldCount; ++i)
		if (kFields[i].id != static_cast<RequestFieldId>(i))
			return false;
	return true;
}
static_assert(fields_in_order(), "the request field rows follow the enum's order");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// Every token its own, never the request's "kind", and every field says what it means.
constexpr bool fields_named() {
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		if (!kFields[i].token[0] || same_text(kFields[i].token, "kind") || !kFields[i].doc[0])
			return false;
		for (size_t j = i + 1; j < kRequestFieldCount; ++j)
			if (same_text(kFields[i].token, kFields[j].token))
				return false;
	}
	return true;
}
static_assert(fields_named(), "each request field has a token of its own and a doc");

} // namespace

const RequestField &request_field(RequestFieldId id) {
	const size_t index = static_cast<size_t>(id);
	return kFields[index < kRequestFieldCount ? index : 0];
}

bool request_field_from_token(const std::string &token, RequestFieldId &out) {
	for (const RequestField &row : kFields) {
		if (token == row.token) {
			out = row.id;
			return true;
		}
	}
	return false;
}

const char *request_json_token(RequestJson json) {
	switch (json) {
		case RequestJson::String:
			return "string";
		case RequestJson::Boolean:
			return "boolean";
		case RequestJson::Strings:
			return "string[]";
		case RequestJson::Object:
			return "object";
		case RequestJson::Objects:
			return "object[]";
	}
	return "string";
}

} // namespace opennova::editor
