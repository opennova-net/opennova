#include <editor/session/session_json.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <variant>

#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <base/io/cp1252.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/text_document.h>
#include <editor/project/project_files.h>
#include <editor/session/record_batch.h>
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
	{EditOperation::Apply, "apply"},
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

JsonValue boolean(bool value) { return JsonValue::make_bool(value); }

} // namespace

JsonValue address_to_json(const NodeAddress &address) {
	JsonValue out = JsonValue::make_object();
	out.set("row", json_number(double(address.row)));
	out.set("kind", json_number(double(address.kind)));
	out.set("child", json_number(double(address.child)));
	return out;
}

namespace {

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
		for (size_t i = 0; i < collection.ids.size(); ++i) {
			const NodeId id = collection.ids[i];
			const NodeAddress address{owner.row, collection.kind_at(i), id};
			JsonValue record = JsonValue::make_object();
			record.set("id", json_number(double(id)));
			// A list of several kinds names each record's own.
			if (!collection.kinds.empty()) record.set("kind_name", json_string(document.kind_token(address.kind)));
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
} // namespace

JsonValue import_source_to_json(const ImportSource &source) {
	JsonValue out = JsonValue::make_object();
	out.set("path", json_string(source.path));
	if (!source.entry.empty()) out.set("entry", json_string(source.entry));
	if (source.install) out.set("install", boolean(true));
	if (source.native) out.set("native", boolean(true));
	return out;
}

namespace {

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

// What a kind's edits are (S13 A5): revert_to_saved's the fields to give back, every other's
// changes, over a text document its spans (S13 D9).
RecordBatchForm batch_form(EditorRequestKind kind, bool text) {
	if (kind == EditorRequestKind::RevertToSaved) return RecordBatchForm::Fields;
	return text ? RecordBatchForm::Spans : RecordBatchForm::Edits;
}

// Whether the type that opens `path` makes text documents (S13 D9): a request's edits over a
// closed file of it are spans.
bool text_path(const std::string &path) {
	if (path.empty()) return false;
	const DocumentType *type = document_type_for(classify_asset(basename_of(path), nullptr));
	return type && document_content(*type) == DocumentContent::Text;
}

// A blank record document of the type that opens `path` (its kinds' tokens, no records): what a
// request's edits name their kinds in when no document it acts on is open (a fix's edit, whose
// document opens first). The path's kind is the scan's by its name (classify_asset: a menu's too,
// which the runtime's classifier types). Null for a path no document type opens, or one whose
// documents hold no records (S13 D6).
std::unique_ptr<Document> blank_names(const std::string &path) {
	if (path.empty()) return nullptr;
	const DocumentType *type = document_type_for(classify_asset(basename_of(path), nullptr));
	return type && type->make ? records_of(type->make()) : nullptr;
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

// A name a rename gives: a string, or a whole number (an item id sent as one) as its digits.
bool name_of(const JsonValue &json, const char *token, std::string &out, std::string &error) {
	if (json.is_string()) {
		out = json.string;
		return true;
	}
	if (json.is_number() && json.number == std::floor(json.number) &&
			std::fabs(json.number) < 9007199254740992.0) {
		out = std::to_string(static_cast<int64_t>(json.number));
		return true;
	}
	error = std::string("\"") + token + "\" must be a string or a whole number.";
	return false;
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

// A record's address: {row, kind, child}, each left out 0; `what` names it in a refusal.
bool address_from_json(const JsonValue &json, NodeAddress &out, std::string &error,
		const std::string &what = "address") {
	if (!json.is_object()) {
		error = "\"" + what + "\" must be an object {row, kind, child}.";
		return false;
	}
	if (!members_known(json, {"row", "kind", "child"}, what.c_str(), error)) return false;
	// A member's value refused by its place ("address.row", "records[1].child").
	const auto refuse = [&](const char *member, const char *must) {
		error = "\"" + what + "." + member + "\" must be " + must + ".";
		return false;
	};
	NodeAddress address;
	if (const JsonValue *row = json.get("row"); row && !read_id(*row, address.row))
		return refuse("row", "a record identity");
	if (const JsonValue *kind = json.get("kind"); kind && !read_kind(*kind, address.kind))
		return refuse("kind", "a whole number");
	if (const JsonValue *child = json.get("child"); child && !read_id(*child, address.child))
		return refuse("child", "a record identity");
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

// A request's field `id` from its wire form into `request` (its edits named in `names`, their
// records not looked for when `unresolved`, RequestNames', their labels into `labels`); false with
// `error` for a value of another type, an unknown token or a malformed object.
bool field_from_json(RequestFieldId id, const JsonValue &json, EditorRequest &request,
		const Document *names, bool unresolved, bool text, std::vector<std::string> &labels,
		std::string &error) {
	using F = RequestFieldId;
	const char *token = request_field(id).token;
	const std::string shown = json.is_string() ? json.string : std::string("?");
	switch (id) {
	case F::Dir: return text_of(json, token, request.dir, error);
	case F::Title: return text_of(json, token, request.title, error);
	case F::Game: return text_of(json, token, request.game, error);
	case F::GameInstall: return text_of(json, token, request.game_install, error);
	case F::Path: return text_of(json, token, request.path, error);
	case F::Locator: return text_of(json, token, request.locator, error);
	case F::Field: return text_of(json, token, request.field, error);
	case F::NewName: return name_of(json, token, request.new_name, error);
	case F::Role: return text_of(json, token, request.role, error);
	case F::FileKind: return text_of(json, token, request.file_kind, error);
	case F::OutDir: return text_of(json, token, request.out_dir, error);
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
		RecordBatch batch;
		if (!record_batch_from_json(
					json, names, batch_form(request.kind, text), batch, error, !unresolved))
			return false;
		request.edits = std::move(batch.edits);
		labels = std::move(batch.labels);
		return true;
	}
	case F::Address: return address_from_json(json, request.address, error);
	case F::Records: {
		if (!json.is_array()) {
			error = "\"records\" must be an array of addresses {row, kind, child}.";
			return false;
		}
		std::vector<NodeAddress> records;
		for (size_t i = 0; i < json.array.size(); ++i) {
			NodeAddress address;
			const std::string place = "records[" + std::to_string(i) + "]";
			if (!address_from_json(json.array[i], address, error, place)) return false;
			records.push_back(address);
		}
		request.records = std::move(records);
		return true;
	}
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
	case F::Viewport:
		// Held as its text (editor_request.h pulls no JSON reader); preview/viewports.h reads it.
		if (!json.is_object()) {
			error = "\"viewport\" must be an object.";
			return false;
		}
		request.viewport = io::json_write(json);
		return true;
	case F::Purpose:
		if (json.is_string() && pick_purpose_from_token(json.string, request.purpose)) return true;
		error = "Unknown pick purpose \"" + shown + "\".";
		return false;
	case F::WithDependencies: return flag_of(json, token, request.with_dependencies, error);
	case F::Replace: return flag_of(json, token, request.replace, error);
	case F::Force: return flag_of(json, token, request.force, error);
	case F::AskName: return flag_of(json, token, request.ask_name, error);
	case F::OpenFirst: return flag_of(json, token, request.open_first, error);
	case F::ImportPass: return flag_of(json, token, request.import_pass, error);
	case F::kCount: break;
	}
	error = std::string("Unknown request member \"") + token + "\".";
	return false;
}

// A request's field `id` as it goes on the wire into `out` (its edits named in `names`); false when
// it holds its default, which the writer leaves out unless the kind must carry the field.
bool field_to_json(
		RequestFieldId id, const EditorRequest &request, const Document *names, JsonValue &out) {
	using F = RequestFieldId;
	switch (id) {
	case F::Dir: out = json_string(request.dir); return !request.dir.empty();
	case F::Title: out = json_string(request.title); return !request.title.empty();
	case F::Game: out = json_string(request.game); return !request.game.empty();
	case F::GameInstall: out = json_string(request.game_install); return !request.game_install.empty();
	case F::Path: out = json_string(request.path); return !request.path.empty();
	case F::Locator: out = json_string(request.locator); return !request.locator.empty();
	case F::Field: out = json_string(request.field); return !request.field.empty();
	case F::NewName: out = json_string(request.new_name); return !request.new_name.empty();
	case F::Role: out = json_string(request.role); return !request.role.empty();
	case F::FileKind: out = json_string(request.file_kind); return !request.file_kind.empty();
	case F::OutDir: out = json_string(request.out_dir); return !request.out_dir.empty();
	case F::Roles: out = strings_to_json(request.roles); return !request.roles.empty();
	case F::Names: out = strings_to_json(request.names); return !request.names.empty();
	case F::Paths: out = strings_to_json(request.paths); return !request.paths.empty();
	case F::Imports:
		out = JsonValue::make_array();
		for (const ImportSource &source : request.imports) out.push(import_source_to_json(source));
		return !request.imports.empty();
	case F::Edits:
		out = record_batch_to_json(request.edits, names, batch_form(request.kind, false));
		return !request.edits.empty();
	case F::Address:
		out = address_to_json(request.address);
		return request.address != NodeAddress();
	case F::Records:
		out = JsonValue::make_array();
		for (const NodeAddress &address : request.records) out.push(address_to_json(address));
		return !request.records.empty();
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
	case F::Viewport: {
		std::string error;
		if (request.viewport.empty() || !io::json_parse(request.viewport, out, error))
			out = JsonValue::make_object();
		return !request.viewport.empty();
	}
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
	// Its default is true: the writer names it only when it is false.
	case F::ImportPass: out = boolean(request.import_pass); return !request.import_pass;
	case F::kCount: break;
	}
	out = JsonValue::make_null();
	return false;
}

// The fields a kind takes, as a refusal names them: "path, locator", or "nothing".
std::string fields_taken(const RequestParams &params) {
	std::string out;
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<RequestFieldId>(i);
		if (params.has(id)) out += (out.empty() ? "" : ", ") + std::string(request_field(id).token);
	}
	return out.empty() ? std::string("nothing") : out;
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

void set_page(JsonValue &out, const JsonPage &page, size_t total, size_t beside) {
	out.set("count", json_number(double(total)));
	const size_t span = total > beside ? total : beside;
	const size_t first = page.first(span), last = page.last(span);
	out.set("offset", json_number(double(first)));
	out.set("next_offset", last < span ? json_number(double(last)) : JsonValue::make_null());
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

bool editor_request_from_json(
		const JsonValue &json, EditorRequest &out, std::string &error, RequestNames *names) {
	if (!json.is_object()) { error = "A request is a JSON object."; return false; }
	const JsonValue *kind = json.get("kind");
	if (!kind || !kind->is_string()) { error = "\"kind\" names the request (a string)."; return false; }
	EditorRequest request;
	if (!editor_request_kind_from_token(kind->string, request.kind)) {
		error = "Unknown request kind \"" + kind->string + "\".";
		return false;
	}
	// Each member a field the kind's row takes (request_kinds.cpp), each field it must carry there;
	// its edits named in the document it acts on, else in a blank of the type its path opens.
	const RequestParams &params = request_kind_row(request.kind).params;
	RequestFieldSet carried = 0;
	std::unique_ptr<Document> blank;
	const Document *document = names ? names->document : nullptr;
	bool text = names && names->text;
	if (!document && !text && params.has(RequestFieldId::Edits)) {
		const std::string path = json.get_string("path", "");
		blank = blank_names(path);
		document = blank.get();
		text = !document && text_path(path);
	}
	std::vector<std::string> labels;
	for (const io::JsonMember &member : json.object) {
		if (member.key == "kind") continue;
		RequestFieldId id = RequestFieldId::Dir;
		if (!request_field_from_token(member.key, id)) {
			error = "Unknown request member \"" + member.key + "\" (" + kind->string + " takes " +
					fields_taken(params) + ").";
			return false;
		}
		if (!params.has(id)) {
			error = kind->string + " takes no \"" + member.key + "\" (it takes " +
					fields_taken(params) + ").";
			return false;
		}
		if (!field_from_json(id, member.value, request, document, names && names->unresolved,
					text, labels, error))
			return false;
		carried |= field_bit(id);
	}
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<RequestFieldId>(i);
		if (params.needs(id) && !(carried & field_bit(id))) {
			error = kind->string + " needs \"" + request_field(id).token + "\" (it takes " +
					fields_taken(params) + ").";
			return false;
		}
	}
	out = std::move(request);
	if (names) names->labels = std::move(labels);
	return true;
}

JsonValue editor_request_to_json(const EditorRequest &request, const Document *names) {
	JsonValue out = JsonValue::make_object();
	out.set("kind", json_string(editor_request_kind_token(request.kind)));
	// The fields its row takes: each it must carry, and any other it carries (not its default).
	const RequestParams &params = request_kind_row(request.kind).params;
	std::unique_ptr<Document> blank;
	if (!names && !request.edits.empty()) {
		blank = blank_names(request.path);
		names = blank.get();
	}
	for (size_t i = 0; i < kRequestFieldCount; ++i) {
		const auto id = static_cast<RequestFieldId>(i);
		if (!params.has(id)) continue;
		JsonValue value;
		if (field_to_json(id, request, names, value) || params.needs(id))
			out.set(request_field(id).token, std::move(value));
	}
	return out;
}

JsonValue diagnostic_to_json(const Diagnostic &d) {
	JsonValue out = JsonValue::make_object();
	out.set("severity", json_string(diagnostic_severity_label(d.severity)));
	out.set("code", json_string(d.code()));
	out.set("message", json_string(d.message));
	if (!d.asset.empty()) out.set("asset", json_string(d.asset));
	if (!d.field.empty()) out.set("field", json_string(d.field));
	if (!d.record.empty()) out.set("record", json_string(d.record));
	if (d.line) out.set("line", json_number(double(d.line)));
	if (d.column) out.set("column", json_number(double(d.column)));
	if (d.row_id) {
		out.set("row", json_number(double(d.row_id)));
		out.set("child", json_number(double(d.child_id)));
		out.set("kind", json_number(double(d.record_kind)));
	}
	// What it is about: a required file's role and name, or a reference's kind, name, scope and
	// what its loader picks the file by (each key only when it holds something).
	if (const RequirementSubject *requirement = requirement_subject(d)) {
		if (!requirement->role.empty()) out.set("role", json_string(requirement->role));
		if (!requirement->target.empty()) out.set("target", json_string(requirement->target));
	} else if (const ReferenceSubject *reference = reference_subject(d)) {
		if (!reference->target.empty()) out.set("target", json_string(reference->target));
		if (reference->kind != ReferenceKind::None)
			out.set("reference", json_string(reference_row(reference->kind).token));
		if (!reference->scope.empty()) out.set("scope", json_string(reference->scope));
		if (reference->loader_arg >= 0) out.set("loader_arg", json_number(double(reference->loader_arg)));
	}
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

JsonValue problems_to_json(const SessionView &view, const ProblemAnswer &answer,
		const JsonPage &page, ProblemFixCache &fixes) {
	JsonValue out = JsonValue::make_object();
	out.set("total", json_number(double(answer.total())));
	out.set("shown", json_number(double(answer.rows.size())));
	JsonValue counts = JsonValue::make_object();
	counts.set("errors", json_number(double(answer.errors)));
	counts.set("warnings", json_number(double(answer.warnings)));
	counts.set("infos", json_number(double(answer.infos)));
	out.set("counts", std::move(counts));
	const size_t first = page.first(answer.rows.size());
	const size_t last = page.last(answer.rows.size());
	set_page(out, page, answer.rows.size());
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
		JsonValue row = diagnostic_to_json(view.findings.diagnostics[answer.rows[i]]);
		if (answer.grouped) row.set("group", json_string(answer.groups[group_of[i]].key));
		JsonValue listed = JsonValue::make_array();
		for (const ProblemFix &fix : fixes.fixes(view, answer.rows[i])) listed.push(problem_fix_to_json(fix));
		row.set("fixes", std::move(listed));
		problems.push(std::move(row));
	}
	out.set("problems", std::move(problems));
	return out;
}

JsonValue document_to_json(const DocumentBase &base, const JsonPage *page) {
	// The record document's rows and records where it is one; a text document's lines (S13 D9); the
	// lifecycle alone for another kind.
	const Document *records = records_of(base);
	const TextDocument *text = text_of(base);
	JsonValue out = JsonValue::make_object();
	out.set("path", json_string(base.path()));
	out.set("kind", json_string(asset_kind_token(base.kind())));
	out.set("dirty", boolean(base.dirty()));
	if (records) out.set("file_state_changed", boolean(records->file_state_changed()));
	out.set("blocked", boolean(base.blocked()));
	out.set("revision", json_number(double(base.revision())));
	out.set("can_undo", boolean(base.can_undo()));
	out.set("can_redo", boolean(base.can_redo()));
	out.set("ignored_lines", json_number(double(base.ignored_lines())));
	if (records) {
		out.set("row_count", json_number(double(records->rows().size())));
		out.set("last_added", json_number(double(records->last_added())));
	}
	if (text) out.set("line_count", json_number(double(text->line_count())));
	JsonValue issues = JsonValue::make_array();
	for (const SourceIssue &issue : base.issues()) {
		JsonValue entry = JsonValue::make_object();
		entry.set("blocks", boolean(issue.blocks));
		entry.set("line", json_number(double(issue.line)));
		if (!issue.record.empty()) entry.set("record", json_string(issue.record));
		if (!issue.field.empty()) entry.set("field", json_string(issue.field));
		entry.set("message", json_string(issue.message));
		issues.push(std::move(entry));
	}
	out.set("issues", std::move(issues));
	if (text && page) {
		// A page of its lines, each its number and its text (in the game's code page, as UTF-8).
		const size_t total = text->line_count();
		set_page(out, *page, total);
		JsonValue lines = JsonValue::make_array();
		for (size_t i = page->first(total); i < page->last(total); ++i) {
			JsonValue line = JsonValue::make_object();
			line.set("line", json_number(double(i + 1)));
			line.set("text", json_string(cp1252_to_utf8(text->line(i + 1))));
			lines.push(std::move(line));
		}
		out.set("lines", std::move(lines));
	}
	if (!records) return out;
	const Document &document = *records;
	JsonValue kinds = JsonValue::make_array();
	for (const RecordKindRow &row : document.kinds()) {
		if (!*row.add_label) continue;
		JsonValue entry = JsonValue::make_object();
		entry.set("kind", json_number(double(row.kind)));
		entry.set("label", json_string(row.add_label));
		kinds.push(std::move(entry));
	}
	out.set("top_kinds", std::move(kinds));
	if (!page) return out;
	JsonValue rows = JsonValue::make_array();
	const size_t total = document.rows().size();
	set_page(out, *page, total);
	for (size_t i = page->first(total); i < page->last(total); ++i) {
		const auto &row = document.rows()[i];
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
		// The choices it offers here: the schema's, or the record's own (a model's LOD 0 parts).
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
		// any of its own (a part index on a model whose LOD 0 has no parts).
		if (schema.open_choices && (field.own_choices || !schema.choices.empty()))
			entry.set("open_choices", boolean(true));
		if (!field.scope.empty() && (field.reference != ReferenceKind::None || field.defines != ReferenceKind::None))
			entry.set("scope", json_string(field.scope));
		// The field's reference, or the variable a menu text's whole %NAME% names (value_reference).
		const ReferenceKind reference = value_reference(field, value);
		if (reference != ReferenceKind::None) {
			entry.set("reference", json_string(reference_row(reference).token));
			std::string symbol;
			const ReferenceStatus status = view.findings.graph
					? reference_status(*view.findings.graph, field, value, &symbol)
					: ReferenceStatus::Unverified;
			entry.set("reference_status", json_string(reference_status_token(status)));
			if (!symbol.empty()) entry.set("symbol", json_string(symbol));
			const std::string target = view.findings.graph
					? reference_target_file(*view.findings.graph, field, value)
					: std::string();
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
	// A text's reference (S13 D9) is written as its text is, in the game's code page: as UTF-8 here.
	const bool text = edge.span.line != 0;
	const auto written = [text](const std::string &value) { return text ? cp1252_to_utf8(value) : value; };
	out.set("source", json_string(edge.source));
	if (!edge.record.empty()) out.set("record", json_string(edge.record));
	if (!edge.locator.empty()) out.set("locator", json_string(edge.locator));
	if (edge.address.row) out.set("address", address_to_json(edge.address));
	if (text) {
		JsonValue span = JsonValue::make_object();
		span.set("line", json_number(double(edge.span.line)));
		span.set("column", json_number(double(edge.span.column)));
		span.set("length", json_number(double(edge.span.length)));
		out.set("span", std::move(span));
	}
	out.set("field", json_string(edge.field));
	out.set("kind", json_string(reference_row(edge.kind).token));
	out.set("value", json_string(written(edge.value)));
	if (!edge.fallback.empty()) out.set("fallback", json_string(written(edge.fallback)));
	out.set("target", json_string(written(edge.target)));
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

JsonValue reference_choices_to_json(const Document &document, const NodeAddress &address,
		const std::string &id, const SessionView &view, const JsonPage &page) {
	FieldUse field;
	Value value;
	if (!field_of(document, address, id, field, value)) return JsonValue::make_null();
	const std::vector<ReferenceChoice> choices = view.findings.graph
			? reference_choices(*view.findings.graph, field)
			: std::vector<ReferenceChoice>();
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(choices.size()); i < page.last(choices.size()); ++i) {
		const ReferenceChoice &choice = choices[i];
		JsonValue entry = JsonValue::make_object();
		entry.set("name", json_string(choice.name));
		entry.set("kind", json_string(reference_row(choice.kind).token));
		entry.set("file", json_string(choice.file));
		if (!choice.record.empty()) entry.set("record", json_string(choice.record));
		if (!choice.label.empty()) entry.set("label", json_string(choice.label));
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
	set_page(out, page, choices.size());
	out.set("choices", std::move(list));
	return out;
}

JsonValue reference_targets_to_json(const Document &document, const NodeAddress &address,
		const std::string &id, const SessionView &view, const JsonPage &page) {
	FieldUse field;
	Value value;
	if (!field_of(document, address, id, field, value)) return JsonValue::make_null();
	const std::vector<ReferenceTarget> targets = view.findings.graph
			? reference_targets(*view.findings.graph, *view.project.scan, field, value)
			: std::vector<ReferenceTarget>();
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(targets.size()); i < page.last(targets.size()); ++i) {
		const ReferenceTarget &target = targets[i];
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
	set_page(out, page, targets.size());
	out.set("targets", std::move(list));
	return out;
}

JsonValue document_hits_to_json(const std::vector<DocumentHit> &hits, const JsonPage &page) {
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(hits.size()); i < page.last(hits.size()); ++i) {
		const DocumentHit &hit = hits[i];
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
	set_page(out, page, hits.size());
	out.set("hits", std::move(list));
	return out;
}

JsonValue graph_search_to_json(const std::vector<GraphSearchHit> &hits, const JsonPage &page) {
	JsonValue list = JsonValue::make_array();
	for (size_t i = page.first(hits.size()); i < page.last(hits.size()); ++i) {
		const GraphSearchHit &hit = hits[i];
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
	set_page(out, page, hits.size());
	out.set("hits", std::move(list));
	return out;
}

} // namespace opennova::editor
