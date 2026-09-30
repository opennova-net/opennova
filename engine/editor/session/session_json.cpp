#include <editor/session/session_json.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <variant>

#include <editor/assets/asset_kind.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/requirements/requirements.h>
#include <editor/run/play_session.h>
#include <editor/session/request_fields.h>
#include <editor/session/request_kinds.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

template <typename Enum>
struct Token {
	Enum value;
	const char *token;
};

constexpr Token<EditOperation> kOperationTokens[] = {
	{EditOperation::Set, "set"},
	{EditOperation::Clear, "clear"},
	{EditOperation::Write, "write"},
	{EditOperation::Add, "add"},
	{EditOperation::Duplicate, "duplicate"},
	{EditOperation::Remove, "remove"},
	{EditOperation::Move, "move"},
	{EditOperation::Paste, "paste"},
	{EditOperation::SetFileValue, "set_file_value"},
};

constexpr Token<SelectMode> kSelectModeTokens[] = {
	{SelectMode::Replace, "replace"},
	{SelectMode::Add, "add"},
	{SelectMode::Toggle, "toggle"},
};

constexpr Token<PickPurpose> kPurposeTokens[] = {
	{PickPurpose::None, "none"},
	{PickPurpose::NewProjectLocation, "new_project_location"},
	{PickPurpose::OpenProject, "open_project"},
	{PickPurpose::RuntimeExecutable, "runtime_executable"},
	{PickPurpose::GameInstall, "game_install"},
	{PickPurpose::ImportFiles, "import_files"},
};

constexpr Token<UnsavedChoice> kChoiceTokens[] = {
	{UnsavedChoice::Save, "save"},
	{UnsavedChoice::Discard, "discard"},
	{UnsavedChoice::Cancel, "cancel"},
};

constexpr Token<ProblemScope> kScopeTokens[] = {
	{ProblemScope::Project, "project"},
	{ProblemScope::ActiveFile, "active_file"},
	{ProblemScope::OpenFiles, "open_files"},
};

constexpr Token<ProblemGrouping> kGroupingTokens[] = {
	{ProblemGrouping::None, "none"},
	{ProblemGrouping::File, "file"},
	{ProblemGrouping::Kind, "kind"},
};

template <typename Enum, size_t N>
const char *token_of(const Token<Enum> (&table)[N], Enum value) {
	for (const Token<Enum> &row : table)
		if (row.value == value) return row.token;
	return "";
}

template <typename Enum, size_t N>
bool value_of(const Token<Enum> (&table)[N], const std::string &token, Enum &out) {
	for (const Token<Enum> &row : table) {
		if (token == row.token) {
			out = row.value;
			return true;
		}
	}
	return false;
}

const char *field_type_token(FieldType type) {
	switch (type) {
	case FieldType::Integer: return "integer";
	case FieldType::Unsigned: return "unsigned";
	case FieldType::Byte: return "byte";
	case FieldType::Count: return "count";
	case FieldType::Real: return "real";
	case FieldType::Text: return "text";
	}
	return "integer";
}

const char *applicability_token(Applicability applies) {
	switch (applies) {
	case Applicability::Reads: return "reads";
	case Applicability::Ignored: return "ignored";
	case Applicability::Unverified: return "unverified";
	}
	return "reads";
}

// How a field holds a colour ("" for none).
const char *color_token(FieldColor color) {
	switch (color) {
	case FieldColor::HexArgb: return "hex_argb";
	case FieldColor::PackedRgb: return "packed_rgb";
	case FieldColor::Channel: return "channel";
	case FieldColor::None: break;
	}
	return "";
}

// How a record stands against the saved baseline (Document::record_change).
const char *record_change_token(Document::RecordChange change) {
	switch (change) {
	case Document::RecordChange::Unchanged: return "unchanged";
	case Document::RecordChange::Changed: return "changed";
	case Document::RecordChange::Added: return "added";
	}
	return "unchanged";
}

const char *reference_status_token(ReferenceStatus status) {
	switch (status) {
	case ReferenceStatus::NotAReference: return "not_a_reference";
	case ReferenceStatus::Present: return "present";
	case ReferenceStatus::Missing: return "missing";
	case ReferenceStatus::Unverified: return "unverified";
	}
	return "not_a_reference";
}

const char *requirement_state_token(RequirementState state) {
	switch (state) {
	case RequirementState::Present: return "present";
	case RequirementState::Missing: return "missing";
	case RequirementState::WrongKind: return "wrong_kind";
	}
	return "missing";
}

JsonValue boolean(bool value) { return JsonValue::make_bool(value); }

JsonValue address_to_json(const NodeAddress &address) {
	JsonValue out = JsonValue::make_object();
	out.set("row", json_number(double(address.row)));
	out.set("kind", json_number(double(address.kind)));
	out.set("child", json_number(double(address.child)));
	return out;
}

// A whole, non-negative JSON number as an identity; false for anything else.
bool read_id(const JsonValue &json, uint64_t &out) {
	if (!json.is_number() || json.number < 0.0 || json.number != std::floor(json.number) ||
	    json.number > 9007199254740992.0) return false;
	out = static_cast<uint64_t>(json.number);
	return true;
}

// A record kind: a whole JSON number that fits a NodeKind; false for anything else.
bool read_kind(const JsonValue &json, NodeKind &out) {
	if (!json.is_number() || json.number != std::floor(json.number) ||
	    json.number < -2147483648.0 || json.number > 2147483647.0) return false;
	out = static_cast<NodeKind>(json.number);
	return true;
}

bool read_string(const JsonValue &object, const char *key, std::string &out, std::string &error) {
	const JsonValue *member = object.get(key);
	if (!member) return true;
	if (!member->is_string()) { error = std::string("\"") + key + "\" must be a string."; return false; }
	out = member->string;
	return true;
}

bool read_bool(const JsonValue &object, const char *key, bool &out, std::string &error) {
	const JsonValue *member = object.get(key);
	if (!member) return true;
	if (!member->is_bool()) { error = std::string("\"") + key + "\" must be true or false."; return false; }
	out = member->boolean;
	return true;
}

JsonValue strings_to_json(const std::vector<std::string> &values) {
	JsonValue out = JsonValue::make_array();
	for (const std::string &value : values) out.push(json_string(value));
	return out;
}

bool members_known(const JsonValue &object, std::initializer_list<const char *> known, const char *what,
                   std::string &error) {
	for (const io::JsonMember &member : object.object) {
		bool found = false;
		for (const char *key : known)
			if (member.key == key) { found = true; break; }
		if (!found) {
			error = std::string("Unknown ") + what + " member \"" + member.key + "\".";
			return false;
		}
	}
	return true;
}

// ApplyProjectSettings' settings: each member optional (one left out stays as it is), its
// type checked.
bool settings_from_json(const JsonValue &json, ProjectSettingsChange &out, std::string &error) {
	if (!json.is_object()) { error = "\"settings\" must be an object."; return false; }
	if (!members_known(json, {"serial", "title", "mission", "multiplayer", "game_install", "runtime_executable",
	                          "play_in_install"},
	                   "settings", error)) return false;
	ProjectSettingsChange change;
	if (const JsonValue *serial = json.get("serial"); serial && !read_id(*serial, change.serial)) {
		error = "\"serial\" must be a whole number, 0 or more.";
		return false;
	}
	const auto text = [&json, &error](const char *key, std::optional<std::string> &member) {
		std::string value;
		if (!json.get(key)) return true;
		if (!read_string(json, key, value, error)) return false;
		member = std::move(value);
		return true;
	};
	const auto flag = [&json, &error](const char *key, std::optional<bool> &member) {
		bool value = false;
		if (!json.get(key)) return true;
		if (!read_bool(json, key, value, error)) return false;
		member = value;
		return true;
	};
	if (!text("title", change.title) || !flag("mission", change.mission) || !flag("multiplayer", change.multiplayer) ||
	    !text("game_install", change.game_install) || !text("runtime_executable", change.runtime_executable) ||
	    !flag("play_in_install", change.play_in_install))
		return false;
	out = std::move(change);
	return true;
}

JsonValue settings_to_json(const ProjectSettingsChange &change) {
	JsonValue out = JsonValue::make_object();
	if (change.serial) out.set("serial", json_number(double(change.serial)));
	if (change.title) out.set("title", json_string(*change.title));
	if (change.mission) out.set("mission", boolean(*change.mission));
	if (change.multiplayer) out.set("multiplayer", boolean(*change.multiplayer));
	if (change.game_install) out.set("game_install", json_string(*change.game_install));
	if (change.runtime_executable)
		out.set("runtime_executable", json_string(*change.runtime_executable));
	if (change.play_in_install) out.set("play_in_install", boolean(*change.play_in_install));
	return out;
}

bool edit_from_json(const JsonValue &json, Edit &out, std::string &error) {
	if (!json.is_object()) { error = "\"edit\" must be an object."; return false; }
	if (!members_known(json, {"operation", "row", "kind", "child", "parent", "field", "value", "position", "coalesce", "gesture"},
	                   "edit", error)) return false;
	Edit edit;
	std::string operation;
	if (!read_string(json, "operation", operation, error)) return false;
	if (!operation.empty() && !edit_operation_from_token(operation, edit.operation)) {
		error = "Unknown edit operation \"" + operation + "\".";
		return false;
	}
	for (const char *key : {"row", "child", "parent"}) {
		if (const JsonValue *member = json.get(key)) {
			uint64_t id = 0;
			if (!read_id(*member, id)) { error = std::string("\"") + key + "\" must be a record identity."; return false; }
			(key[0] == 'r' ? edit.address.row : key[0] == 'c' ? edit.address.child : edit.parent) = id;
		}
	}
	if (const JsonValue *kind = json.get("kind"); kind && !read_kind(*kind, edit.address.kind)) {
		error = "\"kind\" must be a whole number.";
		return false;
	}
	if (!read_string(json, "field", edit.field, error)) return false;
	if (const JsonValue *value = json.get("value")) {
		if (!value_from_json(*value, edit.value)) { error = "\"value\" must be a number, a string or a bool."; return false; }
	}
	if (const JsonValue *position = json.get("position")) {
		uint64_t index = 0;
		if (!read_id(*position, index)) { error = "\"position\" must be a whole number."; return false; }
		edit.position = static_cast<size_t>(index);
	}
	if (!read_bool(json, "coalesce", edit.coalesce, error)) return false;
	if (const JsonValue *gesture = json.get("gesture")) {
		if (!read_id(*gesture, edit.gesture)) { error = "\"gesture\" must be a whole number."; return false; }
	}
	out = edit;
	return true;
}

JsonValue edit_to_json(const Edit &edit) {
	JsonValue out = JsonValue::make_object();
	out.set("operation", json_string(edit_operation_token(edit.operation)));
	out.set("row", json_number(double(edit.address.row)));
	out.set("kind", json_number(double(edit.address.kind)));
	out.set("child", json_number(double(edit.address.child)));
	if (edit.parent) out.set("parent", json_number(double(edit.parent)));
	if (!edit.field.empty()) out.set("field", json_string(edit.field));
	out.set("value", value_to_json(edit.value));
	if (edit.position != SIZE_MAX) out.set("position", json_number(double(edit.position)));
	if (edit.coalesce) out.set("coalesce", boolean(true));
	if (edit.gesture) out.set("gesture", json_number(double(edit.gesture)));
	return out;
}

// A record's own collections (none for one that holds nothing), each record with the
// collections it holds in turn.
JsonValue collections_to_json(const Document &document, const NodeAddress &owner) {
	JsonValue collections = JsonValue::make_array();
	for (const Document::Collection &collection : document.collections_of(owner)) {
		JsonValue entry = JsonValue::make_object();
		entry.set("kind", json_number(double(collection.spec.kind)));
		entry.set("kind_name", json_string(document.kind_token(collection.spec.kind)));
		entry.set("label", json_string(collection.spec.label));
		if (collection.spec.fixed) entry.set("fixed", boolean(true));
		if (collection.spec.max) entry.set("max", json_number(double(collection.spec.max)));
		if (collection.spec.applies != Applicability::Reads) entry.set("applies", json_string(applicability_token(collection.spec.applies)));
		JsonValue records = JsonValue::make_array();
		for (const NodeId id : collection.ids) {
			const NodeAddress address{owner.row, collection.spec.kind, id};
			JsonValue record = JsonValue::make_object();
			record.set("id", json_number(double(id)));
			record.set("name", json_string(document.record_name(address)));
			record.set("change", json_string(record_change_token(document.record_change(address))));
			JsonValue nested = collections_to_json(document, address);
			if (!nested.array.empty()) record.set("collections", std::move(nested));
			records.push(std::move(record));
		}
		entry.set("records", std::move(records));
		collections.push(std::move(entry));
	}
	return collections;
}

// An import source as a request's `imports` takes it: {path, entry, install, native}, the
// defaults left out, so the view's sources pass back as they are.
JsonValue import_source_to_json(const ImportSource &source) {
	JsonValue out = JsonValue::make_object();
	out.set("path", json_string(source.path));
	if (!source.entry.empty()) out.set("entry", json_string(source.entry));
	if (source.install) out.set("install", boolean(true));
	if (source.native) out.set("native", boolean(true));
	return out;
}

// Whether the document holds the record `address` names, of the kind it says.
bool holds_record(const Document &document, const NodeAddress &address) {
	const Node *row = address.row ? document.row(address.row) : nullptr;
	if (!row) return false;
	Document::Placement at;
	return address.child ? document.placement(address, at) && at.spec.kind == address.kind : row->kind == address.kind;
}

// A field of a record as it applies to it (Document::field_on) and the value it holds; false
// for a record the document does not hold or a field it does not have.
bool field_of(const Document &document, const NodeAddress &address, const std::string &id, FieldUse &field, Value &value) {
	if (!holds_record(document, address)) return false;
	for (const FieldSchema &schema : document.fields(address.kind))
		if (schema.id == id && document.get(address, id, value)) {
			field = document.field_on(address, schema);
			return true;
		}
	return false;
}

// --- a request's fields (request_fields.h): each read and written by its row's JSON type ---------

bool text_of(const JsonValue &json, const char *token, std::string &out, std::string &error) {
	if (!json.is_string()) {
		error = std::string("\"") + token + "\" must be a string.";
		return false;
	}
	out = json.string;
	return true;
}

bool flag_of(const JsonValue &json, const char *token, bool &out, std::string &error) {
	if (!json.is_bool()) {
		error = std::string("\"") + token + "\" must be true or false.";
		return false;
	}
	out = json.boolean;
	return true;
}

bool texts_of(const JsonValue &json, const char *token, std::vector<std::string> &out,
              std::string &error) {
	const auto refuse = [&]() {
		error = std::string("\"") + token + "\" must be an array of strings.";
		return false;
	};
	if (!json.is_array()) return refuse();
	std::vector<std::string> texts;
	for (const JsonValue &item : json.array) {
		if (!item.is_string()) return refuse();
		texts.push_back(item.string);
	}
	out = std::move(texts);
	return true;
}

// A record's address: {row, kind, child}, each left out 0.
bool address_from_json(const JsonValue &json, NodeAddress &out, std::string &error) {
	if (!json.is_object()) {
		error = "\"address\" must be an object {row, kind, child}.";
		return false;
	}
	if (!members_known(json, {"row", "kind", "child"}, "address", error)) return false;
	NodeAddress address;
	if (const JsonValue *row = json.get("row"); row && !read_id(*row, address.row)) {
		error = "\"row\" must be a record identity.";
		return false;
	}
	if (const JsonValue *kind = json.get("kind"); kind && !read_kind(*kind, address.kind)) {
		error = "\"kind\" must be a whole number.";
		return false;
	}
	if (const JsonValue *child = json.get("child"); child && !read_id(*child, address.child)) {
		error = "\"child\" must be a record identity.";
		return false;
	}
	out = address;
	return true;
}

// Where a paste goes: {row, parent, position}, each left out at its default.
JsonValue paste_at_to_json(const PasteAt &at) {
	JsonValue out = JsonValue::make_object();
	if (at.row) out.set("row", json_number(double(at.row)));
	if (at.parent) out.set("parent", json_number(double(at.parent)));
	if (at.position != SIZE_MAX) out.set("position", json_number(double(at.position)));
	return out;
}

bool paste_at_from_json(const JsonValue &json, PasteAt &out, std::string &error) {
	if (!json.is_object()) {
		error = "\"paste_at\" must be an object {row, parent, position}.";
		return false;
	}
	if (!members_known(json, {"row", "parent", "position"}, "paste_at", error)) return false;
	PasteAt at;
	if (const JsonValue *row = json.get("row"); row && !read_id(*row, at.row)) {
		error = "\"row\" must be a record identity.";
		return false;
	}
	if (const JsonValue *parent = json.get("parent"); parent && !read_id(*parent, at.parent)) {
		error = "\"parent\" must be a record identity.";
		return false;
	}
	if (const JsonValue *position = json.get("position")) {
		uint64_t index = 0;
		if (!read_id(*position, index)) {
			error = "\"position\" must be a whole number.";
			return false;
		}
		at.position = static_cast<size_t>(index);
	}
	out = at;
	return true;
}

// An import source: {path, entry?, install?, native?}.
constexpr const char *kImportsShape =
        "\"imports\" must be an array of {path, entry, install, native}.";

bool import_source_from_json(const JsonValue &json, ImportSource &out, std::string &error) {
	if (!json.is_object()) {
		error = kImportsShape;
		return false;
	}
	if (!members_known(json, {"path", "entry", "install", "native"}, "import", error)) return false;
	ImportSource import;
	if (!read_string(json, "path", import.path, error) ||
	    !read_string(json, "entry", import.entry, error) ||
	    !read_bool(json, "install", import.install, error) ||
	    !read_bool(json, "native", import.native, error))
		return false;
	if (import.path.empty()) { error = "An import names its path."; return false; }
	out = std::move(import);
	return true;
}

// A request's field `id` from its wire form into `request`; false with `error` for a value of
// another type, an unknown token or a malformed object.
bool field_from_json(RequestFieldId id, const JsonValue &json, EditorRequest &request,
                     std::string &error) {
	using F = RequestFieldId;
	const char *token = request_field(id).token;
	const std::string shown = json.is_string() ? json.string : std::string("?");
	switch (id) {
	case F::Dir: return text_of(json, token, request.dir, error);
	case F::Title: return text_of(json, token, request.title, error);
	case F::Path: return text_of(json, token, request.path, error);
	case F::Locator: return text_of(json, token, request.locator, error);
	case F::Field: return text_of(json, token, request.field, error);
	case F::NewName: return text_of(json, token, request.new_name, error);
	case F::Role: return text_of(json, token, request.role, error);
	case F::FileKind: return text_of(json, token, request.file_kind, error);
	case F::Roles: return texts_of(json, token, request.roles, error);
	case F::Names: return texts_of(json, token, request.names, error);
	case F::Paths: return texts_of(json, token, request.paths, error);
	case F::Imports: {
		if (!json.is_array()) {
			error = kImportsShape;
			return false;
		}
		std::vector<ImportSource> imports;
		for (const JsonValue &source : json.array) {
			ImportSource import;
			if (!import_source_from_json(source, import, error)) return false;
			imports.push_back(std::move(import));
		}
		request.imports = std::move(imports);
		return true;
	}
	case F::Edits: {
		if (!json.is_array()) {
			error = "\"edits\" must be an array of edits.";
			return false;
		}
		std::vector<Edit> edits;
		for (const JsonValue &member : json.array) {
			Edit edit;
			if (!edit_from_json(member, edit, error)) return false;
			edits.push_back(std::move(edit));
		}
		request.edits = std::move(edits);
		return true;
	}
	case F::Address: return address_from_json(json, request.address, error);
	case F::PasteAt: return paste_at_from_json(json, request.paste_at, error);
	case F::Mode:
		if (json.is_string() && select_mode_from_token(json.string, request.mode)) return true;
		error = "Unknown selection mode \"" + shown + "\".";
		return false;
	case F::Choice:
		if (json.is_string() && unsaved_choice_from_token(json.string, request.choice)) return true;
		error = "Unknown unsaved choice \"" + shown + "\".";
		return false;
	case F::Settings: return settings_from_json(json, request.settings, error);
	case F::Purpose:
		if (json.is_string() && pick_purpose_from_token(json.string, request.purpose)) return true;
		error = "Unknown pick purpose \"" + shown + "\".";
		return false;
	case F::WithDependencies: return flag_of(json, token, request.with_dependencies, error);
	case F::Replace: return flag_of(json, token, request.replace, error);
	case F::Force: return flag_of(json, token, request.force, error);
	case F::AskName: return flag_of(json, token, request.ask_name, error);
	case F::OpenFirst: return flag_of(json, token, request.open_first, error);
	case F::kCount: break;
	}
	error = std::string("Unknown request member \"") + token + "\".";
	return false;
}

// A request's field `id` as it goes on the wire into `out`; false when it holds its default, which
// the writer leaves out unless the kind must carry the field.
bool field_to_json(RequestFieldId id, const EditorRequest &request, JsonValue &out) {
	using F = RequestFieldId;
	switch (id) {
	case F::Dir: out = json_string(request.dir); return !request.dir.empty();
	case F::Title: out = json_string(request.title); return !request.title.empty();
	case F::Path: out = json_string(request.path); return !request.path.empty();
	case F::Locator: out = json_string(request.locator); return !request.locator.empty();
	case F::Field: out = json_string(request.field); return !request.field.empty();
	case F::NewName: out = json_string(request.new_name); return !request.new_name.empty();
	case F::Role: out = json_string(request.role); return !request.role.empty();
	case F::FileKind: out = json_string(request.file_kind); return !request.file_kind.empty();
	case F::Roles: out = strings_to_json(request.roles); return !request.roles.empty();
	case F::Names: out = strings_to_json(request.names); return !request.names.empty();
	case F::Paths: out = strings_to_json(request.paths); return !request.paths.empty();
	case F::Imports:
		out = JsonValue::make_array();
		for (const ImportSource &source : request.imports) out.push(import_source_to_json(source));
		return !request.imports.empty();
	case F::Edits:
		out = JsonValue::make_array();
		for (const Edit &edit : request.edits) out.push(edit_to_json(edit));
		return !request.edits.empty();
	case F::Address:
		out = address_to_json(request.address);
		return request.address != NodeAddress();
	case F::PasteAt:
		out = paste_at_to_json(request.paste_at);
		return !(request.paste_at == PasteAt());
	case F::Mode:
		out = json_string(select_mode_token(request.mode));
		return request.mode != SelectMode::Replace;
	case F::Choice:
		out = json_string(unsaved_choice_token(request.choice));
		return request.choice != UnsavedChoice::Cancel;
	case F::Settings:
		out = settings_to_json(request.settings);
		return !(request.settings == ProjectSettingsChange());
	case F::Purpose:
		out = json_string(pick_purpose_token(request.purpose));
		return request.purpose != PickPurpose::None;
	case F::WithDependencies:
		out = boolean(request.with_dependencies);
		return request.with_dependencies;
	case F::Replace: out = boolean(request.replace); return request.replace;
	case F::Force: out = boolean(request.force); return request.force;
	case F::AskName: out = boolean(request.ask_name); return request.ask_name;
	case F::OpenFirst: out = boolean(request.open_first); return request.open_first;
	case F::kCount: break;
	}
	out = JsonValue::make_null();
	return false;
}

// The fields a kind takes, as a refusal names them: "it takes path, locator" or "it takes nothing".
std::string fields_taken(const RequestParams &params) {
	std::string out;
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<RequestFieldId>(i);
		if (params.has(id)) out += (out.empty() ? "" : ", ") + std::string(request_field(id).token);
	}
	return out.empty() ? std::string("it takes nothing") : "it takes " + out;
}

} // namespace

const char *editor_request_kind_token(EditorRequestKind kind) {
	return request_kind_row(kind).token;
}
bool editor_request_kind_from_token(const std::string &token, EditorRequestKind &out) {
	return request_kind_from_token(token, out);
}
const char *edit_operation_token(EditOperation operation) { return token_of(kOperationTokens, operation); }
bool edit_operation_from_token(const std::string &token, EditOperation &out) {
	return value_of(kOperationTokens, token, out);
}
const char *pick_purpose_token(PickPurpose purpose) { return token_of(kPurposeTokens, purpose); }
bool pick_purpose_from_token(const std::string &token, PickPurpose &out) { return value_of(kPurposeTokens, token, out); }
const char *unsaved_choice_token(UnsavedChoice choice) { return token_of(kChoiceTokens, choice); }
const char *select_mode_token(SelectMode mode) { return token_of(kSelectModeTokens, mode); }
bool select_mode_from_token(const std::string &token, SelectMode &out) { return value_of(kSelectModeTokens, token, out); }
bool unsaved_choice_from_token(const std::string &token, UnsavedChoice &out) {
	return value_of(kChoiceTokens, token, out);
}
bool diagnostic_severity_from_token(const std::string &token, DiagnosticSeverity &out) {
	for (const DiagnosticSeverity severity :
	     {DiagnosticSeverity::Info, DiagnosticSeverity::Warning, DiagnosticSeverity::Error}) {
		if (token == diagnostic_severity_label(severity)) {
			out = severity;
			return true;
		}
	}
	return false;
}
const char *problem_scope_token(ProblemScope scope) { return token_of(kScopeTokens, scope); }
bool problem_scope_from_token(const std::string &token, ProblemScope &out) { return value_of(kScopeTokens, token, out); }
const char *problem_grouping_token(ProblemGrouping grouping) { return token_of(kGroupingTokens, grouping); }
bool problem_grouping_from_token(const std::string &token, ProblemGrouping &out) {
	return value_of(kGroupingTokens, token, out);
}

std::vector<std::string> editor_request_kind_tokens() {
	std::vector<std::string> tokens;
	for (size_t i = 0; i < kEditorRequestKindCount; ++i)
		tokens.emplace_back(request_kind_row(static_cast<EditorRequestKind>(i)).token);
	return tokens;
}

JsonValue value_to_json(const Value &value) {
	if (const auto *number = std::get_if<int64_t>(&value)) return json_number(double(*number));
	if (const auto *real = std::get_if<double>(&value)) return json_number(*real);
	return json_string(std::get<std::string>(value));
}

bool value_from_json(const JsonValue &json, Value &out) {
	switch (json.type) {
	case JsonValue::Type::Number:
		if (json.number == std::floor(json.number) && std::fabs(json.number) < 9007199254740992.0)
			out = static_cast<int64_t>(json.number);
		else
			out = json.number;
		return true;
	case JsonValue::Type::String: out = json.string; return true;
	case JsonValue::Type::Bool: out = int64_t(json.boolean ? 1 : 0); return true;
	default: return false;
	}
}

bool editor_request_from_json(const JsonValue &json, EditorRequest &out, std::string &error) {
	if (!json.is_object()) { error = "A request is a JSON object."; return false; }
	const JsonValue *kind = json.get("kind");
	if (!kind || !kind->is_string()) { error = "\"kind\" names the request (a string)."; return false; }
	EditorRequest request;
	if (!editor_request_kind_from_token(kind->string, request.kind)) {
		error = "Unknown request kind \"" + kind->string + "\".";
		return false;
	}
	// Each member a field the kind's row takes (request_kinds.cpp), each field it must carry there.
	const RequestParams &params = request_kind_row(request.kind).params;
	RequestFieldSet carried = 0;
	for (const io::JsonMember &member : json.object) {
		if (member.key == "kind") continue;
		RequestFieldId id = RequestFieldId::Dir;
		if (!request_field_from_token(member.key, id)) {
			error = "Unknown request member \"" + member.key + "\".";
			return false;
		}
		if (!params.has(id)) {
			error = kind->string + " takes no \"" + member.key + "\" (" + fields_taken(params) +
			        ").";
			return false;
		}
		if (!field_from_json(id, member.value, request, error)) return false;
		carried |= field_bit(id);
	}
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<RequestFieldId>(i);
		if (params.needs(id) && !(carried & field_bit(id))) {
			error = kind->string + " needs \"" + request_field(id).token + "\" (" +
			        fields_taken(params) + ").";
			return false;
		}
	}
	out = std::move(request);
	return true;
}

JsonValue editor_request_to_json(const EditorRequest &request) {
	JsonValue out = JsonValue::make_object();
	out.set("kind", json_string(editor_request_kind_token(request.kind)));
	// The fields its row takes: each it must carry, and any other it carries (not its default).
	const RequestParams &params = request_kind_row(request.kind).params;
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<RequestFieldId>(i);
		if (!params.has(id)) continue;
		JsonValue value;
		if (field_to_json(id, request, value) || params.needs(id))
			out.set(request_field(id).token, std::move(value));
	}
	return out;
}

JsonValue diagnostic_to_json(const Diagnostic &d) {
	JsonValue out = JsonValue::make_object();
	out.set("severity", json_string(diagnostic_severity_label(d.severity)));
	out.set("code", json_string(d.code));
	out.set("message", json_string(d.message));
	if (!d.asset.empty()) out.set("asset", json_string(d.asset));
	if (!d.field.empty()) out.set("field", json_string(d.field));
	if (!d.record.empty()) out.set("record", json_string(d.record));
	if (d.line) out.set("line", json_number(double(d.line)));
	if (d.row_id) {
		out.set("row", json_number(double(d.row_id)));
		out.set("child", json_number(double(d.child_id)));
		out.set("kind", json_number(double(d.record_kind)));
	}
	if (!d.role.empty()) out.set("role", json_string(d.role));
	if (!d.target.empty()) out.set("target", json_string(d.target));
	if (d.reference != ReferenceKind::None) out.set("reference", json_string(reference_row(d.reference).token));
	if (!d.scope.empty()) out.set("scope", json_string(d.scope));
	if (d.loader_arg >= 0) out.set("loader_arg", json_number(double(d.loader_arg)));
	return out;
}

JsonValue diagnostics_to_json(const std::vector<Diagnostic> &diagnostics) {
	JsonValue out = JsonValue::make_array();
	for (const Diagnostic &d : diagnostics) out.push(diagnostic_to_json(d));
	return out;
}

JsonValue problem_fix_to_json(const ProblemFix &fix) {
	JsonValue out = JsonValue::make_object();
	out.set("label", json_string(fix.label));
	out.set("detail", json_string(fix.detail));
	out.set("bulk", boolean(fix.bulk));
	out.set("request", editor_request_to_json(fix.request));
	return out;
}

bool problem_query_from_json(const JsonValue &json, ProblemQuery &query, size_t &offset, size_t &limit,
                             std::string &error) {
	if (!json.is_object()) { error = "A Problems query is a JSON object."; return false; }
	if (!members_known(json, {"severities", "text", "scope", "fixable", "group", "offset", "limit"}, "query", error))
		return false;
	ProblemQuery parsed;
	if (const JsonValue *severities = json.get("severities")) {
		if (!severities->is_array()) { error = "\"severities\" must be an array of error, warning and info."; return false; }
		parsed.errors = parsed.warnings = parsed.infos = false;
		for (const JsonValue &token : severities->array) {
			DiagnosticSeverity severity = DiagnosticSeverity::Error;
			if (!token.is_string() || !diagnostic_severity_from_token(token.string, severity)) {
				error = "\"severities\" takes error, warning and info.";
				return false;
			}
			switch (severity) {
			case DiagnosticSeverity::Error: parsed.errors = true; break;
			case DiagnosticSeverity::Warning: parsed.warnings = true; break;
			case DiagnosticSeverity::Info: parsed.infos = true; break;
			}
		}
	}
	if (!read_string(json, "text", parsed.text, error) || !read_bool(json, "fixable", parsed.fixable, error)) return false;
	std::string scope, group;
	if (!read_string(json, "scope", scope, error) || !read_string(json, "group", group, error)) return false;
	if (!scope.empty() && !problem_scope_from_token(scope, parsed.scope)) {
		error = "Unknown scope \"" + scope + "\".";
		return false;
	}
	if (!group.empty() && !problem_grouping_from_token(group, parsed.grouping)) {
		error = "Unknown grouping \"" + group + "\".";
		return false;
	}
	uint64_t first = 0, count = UINT64_MAX;
	if (const JsonValue *member = json.get("offset"); member && !read_id(*member, first)) {
		error = "\"offset\" must be a whole number.";
		return false;
	}
	if (const JsonValue *member = json.get("limit"); member && !read_id(*member, count)) {
		error = "\"limit\" must be a whole number.";
		return false;
	}
	query = parsed;
	offset = static_cast<size_t>(first);
	limit = count == UINT64_MAX ? SIZE_MAX : static_cast<size_t>(count);
	return true;
}

JsonValue problems_to_json(const SessionView &view, const ProblemAnswer &answer, size_t offset, size_t limit,
                           ProblemFixCache &fixes) {
	JsonValue out = JsonValue::make_object();
	out.set("total", json_number(double(answer.total())));
	out.set("shown", json_number(double(answer.rows.size())));
	JsonValue counts = JsonValue::make_object();
	counts.set("errors", json_number(double(answer.errors)));
	counts.set("warnings", json_number(double(answer.warnings)));
	counts.set("infos", json_number(double(answer.infos)));
	out.set("counts", std::move(counts));
	const size_t first = std::min(offset, answer.rows.size());
	const size_t last = first + std::min(limit, answer.rows.size() - first);
	// Grouped, the rows run group after group: the group of each shown row and where each
	// group starts. Only the page's groups are written (a project can have a group per file).
	std::vector<size_t> group_of;
	std::vector<size_t> starts;
	if (answer.grouped) {
		for (size_t g = 0; g < answer.groups.size(); ++g) {
			starts.push_back(group_of.size());
			group_of.insert(group_of.end(), answer.groups[g].rows.size(), g);
		}
		out.set("group_count", json_number(double(answer.groups.size())));
		JsonValue groups = JsonValue::make_array();
		for (size_t i = first; i < last; ++i) {
			if (i > first && group_of[i] == group_of[i - 1]) continue;
			const ProblemGroup &group = answer.groups[group_of[i]];
			JsonValue entry = JsonValue::make_object();
			entry.set("key", json_string(group.key));
			entry.set("title", json_string(group.title));
			entry.set("first", json_number(double(starts[group_of[i]])));
			entry.set("count", json_number(double(group.rows.size())));
			entry.set("errors", json_number(double(group.errors)));
			entry.set("warnings", json_number(double(group.warnings)));
			entry.set("infos", json_number(double(group.infos)));
			groups.push(std::move(entry));
		}
		out.set("groups", std::move(groups));
	}
	JsonValue problems = JsonValue::make_array();
	for (size_t i = first; i < last; ++i) {
		JsonValue row = diagnostic_to_json(view.diagnostics[answer.rows[i]]);
		if (answer.grouped) row.set("group", json_string(answer.groups[group_of[i]].key));
		JsonValue listed = JsonValue::make_array();
		for (const ProblemFix &fix : fixes.fixes(view, answer.rows[i])) listed.push(problem_fix_to_json(fix));
		row.set("fixes", std::move(listed));
		problems.push(std::move(row));
	}
	out.set("problems", std::move(problems));
	return out;
}

JsonValue action_outcome_to_json(const ActionOutcome &outcome) {
	JsonValue out = JsonValue::make_object();
	out.set("done", boolean(outcome.done()));
	out.set("unsaved_prompt", boolean(outcome.unsaved_prompt));
	out.set("operation", json_number(double(outcome.operation)));
	out.set("findings", diagnostics_to_json(outcome.findings));
	return out;
}

JsonValue operation_status_to_json(const OperationStatus &status) {
	JsonValue out = JsonValue::make_object();
	out.set("running", boolean(status.running()));
	if (!status.running()) return out;
	out.set("id", json_number(double(status.id)));
	out.set("kind", json_string(operation_kind_row(status.kind).token));
	out.set("label", json_string(status.label));
	out.set("done", json_number(double(status.done)));
	out.set("total", json_number(double(status.total)));
	out.set("unit", json_string(operation_unit_token(status.unit)));
	out.set("cancellable", boolean(status.cancellable));
	// What it reads and writes: a request that writes either, or reads what it writes, waits.
	JsonValue reads = JsonValue::make_array();
	for (const char *token : holds_tokens(status.reads)) reads.push(json_string(token));
	out.set("reads", std::move(reads));
	JsonValue writes = JsonValue::make_array();
	for (const char *token : holds_tokens(status.writes)) writes.push(json_string(token));
	out.set("writes", std::move(writes));
	return out;
}

JsonValue operation_outcome_to_json(const OperationOutcome &outcome) {
	if (outcome.id == 0) return JsonValue::make_null();
	JsonValue out = JsonValue::make_object();
	out.set("id", json_number(double(outcome.id)));
	out.set("kind", json_string(operation_kind_row(outcome.kind).token));
	out.set("end", json_string(operation_end_token(outcome.end)));
	out.set("findings", diagnostics_to_json(outcome.findings));
	return out;
}

JsonValue session_view_to_json(const SessionView &view, const SessionJsonOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("revision", json_number(double(view.revisions.any())));
	JsonValue revisions = JsonValue::make_object();
	for (size_t i = 0; i < kViewConcernCount; ++i) {
		const ViewConcern concern = static_cast<ViewConcern>(i);
		revisions.set(view_concern_token(concern), json_number(double(view.revisions.of(concern))));
	}
	out.set("revisions", std::move(revisions));
	out.set("status", json_string(view.status));
	// What waits on the unsaved-changes prompt, and the files its Save writes.
	JsonValue prompt = JsonValue::make_object();
	prompt.set("open", boolean(view.unsaved_prompt.open));
	if (view.unsaved_prompt.open) {
		prompt.set("action", json_string(editor_request_kind_token(view.unsaved_prompt.action)));
		if (!view.unsaved_prompt.target.empty()) prompt.set("target", json_string(view.unsaved_prompt.target));
		JsonValue files = JsonValue::make_array();
		for (const std::string &file : view.unsaved_prompt.files) files.push(json_string(file));
		prompt.set("files", std::move(files));
		prompt.set("can_discard", boolean(view.unsaved_prompt.can_discard));
	}
	out.set("unsaved_prompt", std::move(prompt));
	// What the last preview_rename planned: every site, before and after, and the refusals.
	if (view.rename_preview.serial) {
		const SessionView::RenamePreview &rename = view.rename_preview;
		JsonValue preview = JsonValue::make_object();
		preview.set("serial", json_number(double(rename.serial)));
		preview.set("symbol", boolean(rename.symbol));
		if (rename.symbol) preview.set("kind", json_string(reference_row(rename.kind).token));
		preview.set("path", json_string(rename.path));
		if (!rename.locator.empty()) preview.set("locator", json_string(rename.locator));
		if (!rename.field.empty()) preview.set("field", json_string(rename.field));
		preview.set("old_name", json_string(rename.old_name));
		preview.set("new_name", json_string(rename.new_name));
		JsonValue sites = JsonValue::make_array();
		for (const RenameSite &site : rename.sites) {
			JsonValue entry = JsonValue::make_object();
			entry.set("file", json_string(site.file));
			if (!site.record.empty()) entry.set("record", json_string(site.record));
			if (!site.locator.empty()) entry.set("locator", json_string(site.locator));
			entry.set("field", json_string(site.field));
			entry.set("before", json_string(site.before));
			entry.set("after", json_string(site.after));
			sites.push(std::move(entry));
		}
		preview.set("sites", std::move(sites));
		preview.set("refusals", diagnostics_to_json(rename.refusals));
		preview.set("ok", boolean(rename.refusals.empty()));
		out.set("rename_preview", std::move(preview));
	}
	// What the last apply_project_settings came to, under its serial.
	JsonValue settings_result = JsonValue::make_object();
	settings_result.set("serial", json_number(double(view.settings_result.serial)));
	settings_result.set("failures", diagnostics_to_json(view.settings_result.failures));
	out.set("settings_result", std::move(settings_result));
	out.set("quit_requested", boolean(view.quit_requested));

	JsonValue project = JsonValue::make_object();
	project.set("open", boolean(view.project_open));
	if (view.project_open) {
		project.set("root", json_string(view.project_root));
		project.set("title", json_string(view.document.title));
		project.set("id", json_string(view.document.project_id));
		project.set("target_game", json_string(view.document.target_game));
		JsonValue features = JsonValue::make_object();
		features.set("menu", boolean(view.document.features.menu));
		features.set("mission", boolean(view.document.features.mission));
		features.set("multiplayer", boolean(view.document.features.multiplayer));
		project.set("features", std::move(features));
		project.set("assets", json_number(double(view.scan.entries.size())));
		// Every file the scan lists, as Files lists them, each with its kind and whether the
		// editor opens it, so a client need not guess at kinds.
		JsonValue files = JsonValue::make_array();
		for (const AssetEntry &entry : view.scan.entries) {
			JsonValue file = JsonValue::make_object();
			file.set("path", json_string(entry.relative_path));
			file.set("name", json_string(entry.logical_name));
			file.set("kind", json_string(asset_kind_token(entry.kind)));
			file.set("editable", boolean(is_editable_kind(entry.kind)));
			files.push(std::move(file));
		}
		project.set("files", std::move(files));
	}
	out.set("project", std::move(project));

	JsonValue requirements = JsonValue::make_object();
	requirements.set("total", json_number(double(view.requirements.required_total)));
	requirements.set("missing", json_number(double(view.requirements.required_missing)));
	requirements.set("wrong_kind", json_number(double(view.requirements.required_wrong_kind)));
	JsonValue rows = JsonValue::make_array();
	for (const RequirementRow &row : view.requirements.rows) {
		JsonValue entry = JsonValue::make_object();
		entry.set("role", json_string(row.role));
		entry.set("name", json_string(row.name));
		entry.set("phase", json_string(requirement_phase_label(row.phase)));
		entry.set("severity", json_number(double(row.severity)));
		entry.set("required", boolean(row.required));
		entry.set("state", json_string(requirement_state_token(row.state)));
		entry.set("expected_kind", json_string(asset_kind_token(row.expected_kind)));
		if (!row.asset_path.empty()) entry.set("asset", json_string(row.asset_path));
		if (row.found_kind != AssetKind::Unknown) entry.set("found_kind", json_string(asset_kind_token(row.found_kind)));
		if (view.missing_at_boot(row.name)) entry.set("boot_missing", boolean(true));
		rows.push(std::move(entry));
	}
	requirements.set("rows", std::move(rows));
	out.set("requirements", std::move(requirements));

	JsonValue documents = JsonValue::make_array();
	for (const auto &document : view.documents) {
		if (document) documents.push(document_to_json(*document, false));
	}
	out.set("documents", std::move(documents));
	out.set("active_document", json_string(view.active_document));
	out.set("selection", address_to_json(view.selection));
	JsonValue selected = JsonValue::make_array();
	for (const NodeAddress &address : view.selected) selected.push(address_to_json(address));
	out.set("selected", std::move(selected));
	if (!view.reveal_field.empty()) {
		out.set("reveal_field", json_string(view.reveal_field));
		out.set("reveal_serial", json_number(double(view.reveal_serial)));
	}
	if (!view.reveal_file.empty()) {
		JsonValue reveal = JsonValue::make_object();
		reveal.set("path", json_string(view.reveal_file));
		reveal.set("serial", json_number(double(view.reveal_file_serial)));
		if (view.reveal_file_rename) reveal.set("rename", boolean(true));
		out.set("reveal_file", std::move(reveal));
	}
	out.set("clipboard_bytes", json_number(double(view.clipboard.size())));

	// The operation that runs (a build stepping) and what the last one came to; the build block
	// is the last build's.
	out.set("operation", operation_status_to_json(view.operation));
	out.set("last_operation", operation_outcome_to_json(view.last_operation));
	JsonValue build = JsonValue::make_object();
	build.set("has_build", boolean(view.has_build));
	if (view.has_build) {
		build.set("ok", boolean(view.last_build.ok));
		build.set("id", json_string(view.last_build.build_id));
		build.set("dir", json_string(view.last_build.build_dir));
		build.set("reused_existing", boolean(view.last_build.reused_existing));
		build.set("archives_written", json_number(double(view.last_build.archives_written.size())));
		build.set("archives_reused", json_number(double(view.last_build.archives_reused.size())));
		build.set("loose_written", json_number(double(view.last_build.loose_written.size())));
		build.set("diagnostics", diagnostics_to_json(view.last_build.diagnostics));
	}
	out.set("build", std::move(build));

	JsonValue play = JsonValue::make_object();
	play.set("state", json_string(play_state_label(view.play_state)));
	play.set("pid", json_number(double(view.play_pid)));
	play.set("mcp_port", json_number(double(view.play_mcp_port)));
	play.set("command_line", json_string(view.play_command_line));
	play.set("exited_on_its_own", boolean(view.play_exited_on_its_own));
	play.set("exit_code", view.play_exit_code >= 0 ? json_number(double(view.play_exit_code)) : JsonValue::make_null());
	play.set("in_install", boolean(view.play_retail));
	play.set("game_install", json_string(view.retail_directory));
	play.set("source_run", boolean(view.source_run));
	play.set("runtime_executable", json_string(view.runtime_executable));
	play.set("runtime_setting", json_string(view.runtime_setting));
	JsonValue boot_missing = JsonValue::make_array();
	for (const std::string &name : view.boot_missing) boot_missing.push(json_string(name));
	play.set("boot_missing", std::move(boot_missing));
	out.set("play", std::move(play));

	// The import dialog: what it lists to choose from, the files chosen, and their plan, each
	// list a page (options.import_offset / import_limit) with its count.
	const SessionView::ImportPreview &preview = view.import_preview;
	JsonValue import = JsonValue::make_object();
	import.set("open", boolean(preview.open));
	import.set("import_dependencies", boolean(view.import_dependencies));
	import.set("with_dependencies", boolean(preview.with_dependencies));
	if (preview.changed) import.set("changed", boolean(true));
	import.set("serial", json_number(double(preview.serial)));
	import.set("offset", json_number(double(options.import_offset)));
	const auto on_page = [&options](size_t index) {
		return index >= options.import_offset && index - options.import_offset < options.import_limit;
	};
	const auto sources_to_json = [&on_page](const std::vector<ImportSource> &sources) {
		JsonValue out = JsonValue::make_array();
		for (size_t i = 0; i < sources.size(); ++i)
			if (on_page(i)) out.push(import_source_to_json(sources[i]));
		return out;
	};
	import.set("choice_count", json_number(double(preview.choices.size())));
	import.set("choices", sources_to_json(preview.choices));
	import.set("root_count", json_number(double(preview.roots.size())));
	import.set("roots", sources_to_json(preview.roots));
	JsonValue planned = JsonValue::make_array();
	JsonValue not_found = JsonValue::make_array();
	size_t row_count = 0, not_found_count = 0;
	for (const ImportPlanRow &row : preview.plan.rows) {
		const bool found = row.state != ImportPlanRow::State::NotFound;
		if (!on_page(found ? row_count++ : not_found_count++)) continue;
		JsonValue entry = JsonValue::make_object();
		entry.set("state", json_string(row.state == ImportPlanRow::State::Selected ? "selected" : found ? "found" : "not_found"));
		entry.set("name", json_string(row.name));
		entry.set("kind", json_string(asset_kind_token(row.kind)));
		if (!row.needed_by.file.empty()) {
			JsonValue need = JsonValue::make_object();
			need.set("file", json_string(row.needed_by.file));
			need.set("record", json_string(row.needed_by.record));
			need.set("field", json_string(row.needed_by.field));
			need.set("reference", json_string(reference_row(row.needed_by.reference).token));
			need.set("name", json_string(row.needed_by.name));
			if (row.needed_by.loader_arg >= 0) need.set("loader_arg", json_number(double(row.needed_by.loader_arg)));
			entry.set("needed_by", std::move(need));
		}
		if (found) {
			entry.set("source", import_source_to_json(row.source));
			entry.set("destination", json_string(row.destination));
			if (!row.made_from.empty()) entry.set("made_from", json_string(row.made_from));
			entry.set("found_in", json_string(row.found_in));
			entry.set("selected", boolean(row.selected));
			if (!row.problem.empty()) entry.set("problem", json_string(row.problem));
			JsonValue rivals = JsonValue::make_array();
			for (const ImportRival &rival : row.rivals) {
				JsonValue other = JsonValue::make_object();
				other.set("name", json_string(rival.name));
				other.set("found_in", json_string(rival.found_in));
				other.set("differs", boolean(rival.differs));
				other.set("source", import_source_to_json(rival.source));
				rivals.push(std::move(other));
			}
			if (!row.rivals.empty()) entry.set("rivals", std::move(rivals));
		}
		(found ? planned : not_found).push(std::move(entry));
	}
	import.set("row_count", json_number(double(row_count)));
	import.set("rows", std::move(planned));
	import.set("not_found_count", json_number(double(not_found_count)));
	import.set("not_found", std::move(not_found));
	JsonValue not_followed = JsonValue::make_array();
	for (const ImportNotFollowed &kind : preview.plan.not_followed) {
		JsonValue entry = JsonValue::make_object();
		if (kind.reference != ReferenceKind::None) entry.set("reference", json_string(reference_row(kind.reference).token));
		else entry.set("kind", json_string(asset_kind_token(kind.kind)));
		entry.set("count", json_number(double(kind.count)));
		entry.set("first", json_string(kind.first));
		not_followed.push(std::move(entry));
	}
	import.set("not_followed", std::move(not_followed));
	import.set("truncated", boolean(preview.plan.truncated));
	import.set("diagnostics", diagnostics_to_json(preview.plan.diagnostics));
	// The importable sources in the project and what their importers made.
	JsonValue imported = JsonValue::make_array();
	for (const ImportedSource &source : view.imports) {
		JsonValue entry = JsonValue::make_object();
		entry.set("source", json_string(source.source));
		entry.set("sidecar", json_string(source.sidecar));
		entry.set("importer", json_string(source.importer));
		entry.set("ok", boolean(source.ok));
		entry.set("reimported", boolean(source.reimported));
		JsonValue outputs = JsonValue::make_array();
		for (const std::string &output : source.outputs) outputs.push(json_string(output));
		entry.set("outputs", std::move(outputs));
		imported.push(std::move(entry));
	}
	import.set("imported", std::move(imported));
	import.set("install_files", json_number(double(view.retail_files.size())));
	out.set("import", std::move(import));

	size_t errors = 0, warnings = 0, infos = 0;
	for (const Diagnostic &d : view.diagnostics) {
		if (d.severity == DiagnosticSeverity::Error) ++errors;
		else if (d.severity == DiagnosticSeverity::Warning) ++warnings;
		else ++infos;
	}
	JsonValue problems = JsonValue::make_object();
	problems.set("count", json_number(double(view.diagnostics.size())));
	problems.set("errors", json_number(double(errors)));
	problems.set("warnings", json_number(double(warnings)));
	problems.set("infos", json_number(double(infos)));
	out.set("problems", std::move(problems));

	JsonValue recent = JsonValue::make_array();
	for (const std::string &root : view.recent_projects) recent.push(json_string(root));
	out.set("recent_projects", std::move(recent));

	JsonValue graph = JsonValue::make_object();
	if (view.graph) {
		graph.set("edges", json_number(double(view.graph->edges().size())));
		graph.set("symbols", json_number(double(view.graph->symbols().size())));
		graph.set("missing", json_number(double(view.graph->missing().size())));
		graph.set("files_extracted", json_number(double(view.graph->stats().files_extracted)));
		graph.set("files_reused", json_number(double(view.graph->stats().files_reused)));
		graph.set("files_failed", json_number(double(view.graph->stats().files_failed)));
	}
	out.set("graph", std::move(graph));

	// A page of the output lines by absolute index (OutputLog): `first` is the oldest line held,
	// `next` one past the newest; a cursor below `first` missed the lines dropped since, one past
	// `next` waits for lines to come.
	JsonValue output = JsonValue::make_object();
	const uint64_t first = view.output.first_index();
	const uint64_t next = view.output.next_index();
	const uint64_t cursor = std::min<uint64_t>(std::max<uint64_t>(options.output_cursor, first), next);
	const uint64_t last = std::min<uint64_t>(cursor + options.output_limit, next);
	output.set("first", json_number(double(first)));
	output.set("next", json_number(double(next)));
	output.set("cursor", json_number(double(cursor)));
	output.set("next_cursor", json_number(double(last)));
	JsonValue lines = JsonValue::make_array();
	for (uint64_t i = cursor; i < last; ++i) lines.push(json_string(view.output.at(i)));
	output.set("lines", std::move(lines));
	out.set("output", std::move(output));
	return out;
}

JsonValue document_to_json(const Document &document, bool with_rows) {
	JsonValue out = JsonValue::make_object();
	out.set("path", json_string(document.path()));
	out.set("kind", json_string(asset_kind_token(document.kind())));
	out.set("dirty", boolean(document.dirty()));
	out.set("file_state_changed", boolean(document.file_state_changed()));
	out.set("blocked", boolean(document.blocked()));
	out.set("revision", json_number(double(document.revision())));
	out.set("can_undo", boolean(document.can_undo()));
	out.set("can_redo", boolean(document.can_redo()));
	out.set("ignored_lines", json_number(double(document.ignored_lines())));
	out.set("row_count", json_number(double(document.rows().size())));
	out.set("last_added", json_number(double(document.last_added())));
	JsonValue issues = JsonValue::make_array();
	for (const SourceIssue &issue : document.issues()) {
		JsonValue entry = JsonValue::make_object();
		entry.set("blocks", boolean(issue.blocks));
		entry.set("line", json_number(double(issue.line)));
		if (!issue.record.empty()) entry.set("record", json_string(issue.record));
		if (!issue.field.empty()) entry.set("field", json_string(issue.field));
		entry.set("message", json_string(issue.message));
		issues.push(std::move(entry));
	}
	out.set("issues", std::move(issues));
	JsonValue kinds = JsonValue::make_array();
	for (const RecordKindRow &row : document.kinds()) {
		if (!*row.add_label) continue;
		JsonValue entry = JsonValue::make_object();
		entry.set("kind", json_number(double(row.kind)));
		entry.set("label", json_string(row.add_label));
		kinds.push(std::move(entry));
	}
	out.set("top_kinds", std::move(kinds));
	if (!with_rows) return out;
	JsonValue rows = JsonValue::make_array();
	for (const auto &row : document.rows()) {
		if (!row) continue;
		JsonValue entry = JsonValue::make_object();
		entry.set("id", json_number(double(row->id)));
		entry.set("kind", json_number(double(row->kind)));
		entry.set("kind_label", json_string(document.kind_label(row->kind)));
		entry.set("name", json_string(row->name()));
		entry.set("change", json_string(record_change_token(document.record_change({row->id, row->kind, 0}))));
		JsonValue collections = collections_to_json(document, {row->id, row->kind, 0});
		entry.set("collections", std::move(collections));
		rows.push(std::move(entry));
	}
	out.set("rows", std::move(rows));
	return out;
}

JsonValue record_to_json(const Document &document, const NodeAddress &address, const SessionView &view) {
	if (!holds_record(document, address)) return JsonValue::make_null();
	Document::Placement at;
	if (address.child) document.placement(address, at);
	JsonValue out = JsonValue::make_object();
	out.set("row", json_number(double(address.row)));
	out.set("kind", json_number(double(address.kind)));
	out.set("child", json_number(double(address.child)));
	out.set("kind_label", json_string(document.kind_label(address.kind)));
	out.set("name", json_string(document.record_name(address)));
	out.set("path", json_string(document.record_path(address)));
	out.set("locator", json_string(document.locator(address)));
	out.set("change", json_string(record_change_token(document.record_change(address))));
	if (address.child) {
		out.set("owner", address_to_json(at.owner));
		out.set("index", json_number(double(at.index)));
	}
	JsonValue fields = JsonValue::make_array();
	std::vector<FieldChoice> own;
	for (const FieldSchema &schema : document.fields(address.kind)) {
		Value value;
		if (!document.get(address, schema.id, value)) continue;
		const FieldUse field = document.field_on(address, schema);
		JsonValue entry = JsonValue::make_object();
		entry.set("id", json_string(schema.id));
		if (!schema.label.empty()) entry.set("label", json_string(schema.label));
		if (!schema.section.empty()) entry.set("section", json_string(schema.section));
		if (!schema.group.empty()) entry.set("group", json_string(schema.group));
		entry.set("type", json_string(field_type_token(schema.type)));
		entry.set("value", value_to_json(value));
		// What the format table says of the field: its unit, its note, the key the file writes,
		// the range it keeps to, how it holds a colour.
		if (!schema.unit.empty()) entry.set("unit", json_string(schema.unit));
		if (!schema.description.empty()) entry.set("description", json_string(schema.description));
		if (!schema.token.empty()) entry.set("token", json_string(schema.token));
		if (schema.ranged) {
			entry.set("min", json_number(schema.min));
			entry.set("max", json_number(schema.max));
			if (schema.step > 0.0) entry.set("step", json_number(schema.step));
		}
		if (field.color != FieldColor::None)
			entry.set("color", json_string(color_token(field.color)));
		if (schema.width) entry.set("width", json_number(double(schema.width)));
		if (field.read_only) entry.set("read_only", boolean(true));
		if (schema.flags) entry.set("flags", boolean(true));
		if (schema.optional) {
			entry.set("optional", boolean(true));
			entry.set("present", boolean(document.present(address, schema.id)));
		}
		if (field.applies != Applicability::Reads) entry.set("applies", json_string(applicability_token(field.applies)));
		// Changed since the saved baseline: what the saved file holds (null when it lacks the
		// record), as the Inspector's mark and its tooltip show it.
		if (document.field_changed(address, schema.id)) {
			entry.set("changed", boolean(true));
			Value saved;
			bool written = true;
			if (document.saved_value(address, schema.id, saved, &written)) {
				entry.set("saved", value_to_json(saved));
				if (schema.optional) entry.set("saved_present", boolean(written));
			} else {
				entry.set("saved", JsonValue::make_null());
			}
		}
		// The choices it offers here: the schema's, or the record's own (a model's registers).
		const std::vector<FieldChoice> &offered = document.choices_on(address, field, own);
		if (!offered.empty()) {
			JsonValue choices = JsonValue::make_array();
			for (const FieldChoice &choice : offered) {
				JsonValue option = JsonValue::make_object();
				option.set("name", json_string(choice.name));
				option.set("value", json_number(double(choice.value)));
				if (!choice.label.empty()) option.set("label", json_string(choice.label));
				choices.push(std::move(option));
			}
			entry.set("choices", std::move(choices));
		}
		// Open: any value typed, where the field offers choices, whether or not the record knows
		// any of its own (a model with no registers).
		if (schema.open_choices && (field.own_choices || !schema.choices.empty()))
			entry.set("open_choices", boolean(true));
		if (!field.scope.empty() && (field.reference != ReferenceKind::None || field.defines != ReferenceKind::None))
			entry.set("scope", json_string(field.scope));
		if (field.reference != ReferenceKind::None) {
			entry.set("reference", json_string(reference_row(field.reference).token));
			std::string symbol;
			const ReferenceStatus status = document.reference_status(field, value, view, &symbol);
			entry.set("reference_status", json_string(reference_status_token(status)));
			if (!symbol.empty()) entry.set("symbol", json_string(symbol));
			const std::string target = document.reference_target_file(field, value, view);
			if (!target.empty()) entry.set("reference_file", json_string(target));
		}
		if (field.defines != ReferenceKind::None) entry.set("defines", json_string(reference_row(field.defines).token));
		fields.push(std::move(entry));
	}
	out.set("fields", std::move(fields));
	out.set("collections", collections_to_json(document, address));
	return out;
}

JsonValue graph_edge_to_json(const AssetGraph &graph, const GraphEdge &edge) {
	JsonValue out = JsonValue::make_object();
	out.set("source", json_string(edge.source));
	if (!edge.record.empty()) out.set("record", json_string(edge.record));
	if (!edge.locator.empty()) out.set("locator", json_string(edge.locator));
	if (edge.address.row) out.set("address", address_to_json(edge.address));
	out.set("field", json_string(edge.field));
	out.set("kind", json_string(reference_row(edge.kind).token));
	out.set("value", json_string(edge.value));
	out.set("target", json_string(edge.target));
	if (!edge.scope.empty()) out.set("scope", json_string(edge.scope));
	out.set("rewritable", boolean(edge.rewritable));
	if (edge.through != ReferenceKind::None) out.set("through", json_string(reference_row(edge.through).token));
	if (edge.loader_arg >= 0) out.set("loader_arg", json_number(double(edge.loader_arg)));
	std::string file;
	const ReferenceStatus status = edge.target.empty() ? ReferenceStatus::NotAReference : graph.resolve(edge, &file);
	out.set("status", json_string(reference_status_token(status)));
	if (!file.empty()) out.set("file", json_string(file));
	return out;
}

JsonValue graph_edges_to_json(const AssetGraph &graph, const std::vector<const GraphEdge *> &edges) {
	JsonValue out = JsonValue::make_array();
	for (const GraphEdge *edge : edges)
		if (edge) out.push(graph_edge_to_json(graph, *edge));
	return out;
}

JsonValue graph_symbol_to_json(const GraphSymbol &symbol) {
	JsonValue out = JsonValue::make_object();
	out.set("kind", json_string(reference_row(symbol.kind).token));
	out.set("name", json_string(symbol.display));
	out.set("file", json_string(symbol.file));
	if (!symbol.record.empty()) out.set("record", json_string(symbol.record));
	if (!symbol.locator.empty()) out.set("locator", json_string(symbol.locator));
	if (symbol.address.row) out.set("address", address_to_json(symbol.address));
	if (!symbol.field.empty()) out.set("field", json_string(symbol.field));
	if (!symbol.scope.empty()) out.set("scope", json_string(symbol.scope));
	if (!symbol.value.empty()) out.set("value", json_string(symbol.value));
	if (symbol.inert) out.set("inert", boolean(true));
	if (!symbol.inert_reason.empty()) out.set("inert_reason", json_string(symbol.inert_reason));
	if (symbol.line) out.set("line", json_number(double(symbol.line)));
	return out;
}

JsonValue reference_choices_to_json(const Document &document, const NodeAddress &address, const std::string &id,
                                    const SessionView &view) {
	FieldUse field;
	Value value;
	if (!field_of(document, address, id, field, value)) return JsonValue::make_null();
	const std::vector<ReferenceChoice> choices = document.reference_choices(field, view);
	JsonValue list = JsonValue::make_array();
	for (const ReferenceChoice &choice : choices) {
		JsonValue entry = JsonValue::make_object();
		entry.set("name", json_string(choice.name));
		entry.set("kind", json_string(reference_row(choice.kind).token));
		entry.set("file", json_string(choice.file));
		if (!choice.record.empty()) entry.set("record", json_string(choice.record));
		entry.set("status", json_string(reference_status_token(choice.status)));
		if (choice.inert) {
			entry.set("inert", boolean(true));
			entry.set("reason", json_string(choice.reason));
		}
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	out.set("field", json_string(field.schema->id));
	out.set("reference", json_string(reference_row(field.reference).token));
	if (!field.scope.empty()) out.set("scope", json_string(field.scope));
	out.set("count", json_number(double(choices.size())));
	out.set("choices", std::move(list));
	return out;
}

JsonValue reference_targets_to_json(const Document &document, const NodeAddress &address, const std::string &id,
                                    const SessionView &view) {
	FieldUse field;
	Value value;
	if (!field_of(document, address, id, field, value)) return JsonValue::make_null();
	const std::vector<ReferenceTarget> targets = document.reference_targets(field, value, view);
	JsonValue list = JsonValue::make_array();
	for (const ReferenceTarget &target : targets) {
		JsonValue entry = JsonValue::make_object();
		entry.set("label", json_string(target.label));
		entry.set("file", json_string(target.file));
		if (!target.locator.empty()) entry.set("locator", json_string(target.locator));
		if (!target.field.empty()) entry.set("field", json_string(target.field));
		entry.set("editable", boolean(target.editable));
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	out.set("field", json_string(field.schema->id));
	out.set("reference", json_string(reference_row(field.reference).token));
	out.set("value", value_to_json(value));
	out.set("count", json_number(double(targets.size())));
	out.set("targets", std::move(list));
	return out;
}

JsonValue document_hits_to_json(const std::vector<DocumentHit> &hits) {
	JsonValue list = JsonValue::make_array();
	for (const DocumentHit &hit : hits) {
		JsonValue entry = JsonValue::make_object();
		entry.set("id",
		          json_number(double(hit.address.child ? hit.address.child : hit.address.row)));
		entry.set("address", address_to_json(hit.address));
		entry.set("record", json_string(hit.record));
		entry.set("locator", json_string(hit.locator));
		entry.set("field", json_string(hit.field));
		entry.set("label", json_string(hit.label));
		entry.set("text", json_string(hit.text));
		entry.set("at", json_number(double(hit.at)));
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	out.set("count", json_number(double(hits.size())));
	out.set("hits", std::move(list));
	return out;
}

JsonValue graph_search_to_json(const std::vector<GraphSearchHit> &hits) {
	JsonValue list = JsonValue::make_array();
	for (const GraphSearchHit &hit : hits) {
		JsonValue entry = hit.symbol ? graph_symbol_to_json(*hit.symbol) : JsonValue::make_object();
		if (!hit.symbol) {
			entry.set("kind", json_string("file"));
			entry.set("name", json_string(hit.name));
			entry.set("file", json_string(hit.file));
		}
		entry.set("usages", json_number(double(hit.usages)));
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	out.set("count", json_number(double(hits.size())));
	out.set("hits", std::move(list));
	return out;
}

} // namespace opennova::editor
