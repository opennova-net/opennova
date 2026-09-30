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
	{ F::Title, "title", J::String, "A new project's title (New Game when left out)." },
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
	{ F::Roles, "roles", J::Strings, "Requirements' roles." },
	{ F::Names, "names", J::Strings, "Files of the game install, by logical name." },
	{ F::Paths, "paths", J::Strings,
			"Files on disk to import: a loose file is chosen, an archive's members are listed to "
			"choose "
			"from." },
	{ F::Imports, "imports", J::Objects,
			"Import sources, {path, entry?, install?, native?}, each as the view's import rows "
			"carry it "
			"(install: the path is the game install; native: a loose file copied as the game's "
			"own)." },
	{ F::Edits, "edits", J::Objects,
			"Edits over any rows of one document in the batch form, one undo step: [{op, id, parent, "
			"kind, field, value, position, as, coalesce, gesture}], op one of set, clear, write, "
			"add, duplicate, remove, move, set_file_value or replace_list ({op, id, list, records}: "
			"the list of that kind the record holds replaced by records, each {field: value}); a "
			"record or a row by its identity or by the label (as) an earlier add or duplicate of the "
			"batch gave it, an add's kind by its token. revert_to_saved's: [{id, field}]." },
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
