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

namespace opennova::editor {

namespace {

using io::JsonValue;

template <typename Enum>
struct Token {
	Enum value;
	const char *token;
};

// One row per enumerator, in enumerator order: the ctest walks the enum and expects a
// token for each, so a kind added without a row fails there, not on the wire.
constexpr Token<EditorRequestKind> kKindTokens[] = {
	{EditorRequestKind::NewProject, "new_project"},
	{EditorRequestKind::OpenProject, "open_project"},
	{EditorRequestKind::CloseProject, "close_project"},
	{EditorRequestKind::ForgetRecent, "forget_recent"},
	{EditorRequestKind::Rescan, "rescan"},
	{EditorRequestKind::ApplyProjectSettings, "apply_project_settings"},
	{EditorRequestKind::PreviewImport, "preview_import"},
	{EditorRequestKind::PlanImport, "plan_import"},
	{EditorRequestKind::SetImportDependencies, "set_import_dependencies"},
	{EditorRequestKind::ImportFiles, "import_files"},
	{EditorRequestKind::CancelImport, "cancel_import"},
	{EditorRequestKind::CreateMissing, "create_missing"},
	{EditorRequestKind::Build, "build"},
	{EditorRequestKind::Play, "play"},
	{EditorRequestKind::StopPlay, "stop_play"},
	{EditorRequestKind::CreateFile, "create_file"},
	{EditorRequestKind::OpenDocument, "open_document"},
	{EditorRequestKind::ShowInFiles, "show_in_files"},
	{EditorRequestKind::ReloadDocument, "reload_document"},
	{EditorRequestKind::CloseDocument, "close_document"},
	{EditorRequestKind::SelectRecord, "select_record"},
	{EditorRequestKind::EditRecord, "edit_record"},
	{EditorRequestKind::RevertToSaved, "revert_to_saved"},
	{EditorRequestKind::EndEdit, "end_edit"},
	{EditorRequestKind::Copy, "copy"},
	{EditorRequestKind::Cut, "cut"},
	{EditorRequestKind::Paste, "paste"},
	{EditorRequestKind::Duplicate, "duplicate"},
	{EditorRequestKind::Save, "save"},
	{EditorRequestKind::SaveAll, "save_all"},
	{EditorRequestKind::Undo, "undo"},
	{EditorRequestKind::Redo, "redo"},
	{EditorRequestKind::ResolveUnsaved, "resolve_unsaved"},
	{EditorRequestKind::RenameAsset, "rename_asset"},
	{EditorRequestKind::AssignRequirement, "assign_requirement"},
	{EditorRequestKind::PreviewRename, "preview_rename"},
	{EditorRequestKind::RenameSymbol, "rename_symbol"},
	{EditorRequestKind::Reimport, "reimport"},
	{EditorRequestKind::PreviewRetailImport, "preview_retail_import"},
	{EditorRequestKind::ClearOutput, "clear_output"},
	{EditorRequestKind::Quit, "quit"},
	{EditorRequestKind::PickDirectory, "pick_directory"},
	{EditorRequestKind::PickFile, "pick_file"},
	{EditorRequestKind::RevealPath, "reveal_path"},
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
	{PickPurpose::RetailDirectory, "retail_directory"},
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

JsonValue str(const std::string &value) { return JsonValue::make_string(value); }
JsonValue num(double value) { return JsonValue::make_number(value); }
JsonValue boolean(bool value) { return JsonValue::make_bool(value); }

JsonValue address_to_json(const NodeAddress &address) {
	JsonValue out = JsonValue::make_object();
	out.set("row", num(double(address.row)));
	out.set("kind", num(double(address.kind)));
	out.set("child", num(double(address.child)));
	return out;
}

// A whole, non-negative JSON number as an identity; false for anything else.
bool read_id(const JsonValue &json, uint64_t &out) {
	if (!json.is_number() || json.number < 0.0 || json.number != std::floor(json.number) ||
	    json.number > 9007199254740992.0) return false;
	out = static_cast<uint64_t>(json.number);
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

bool read_strings(const JsonValue &object, const char *key, std::vector<std::string> &out, std::string &error) {
	const JsonValue *member = object.get(key);
	if (!member) return true;
	const auto refuse = [&]() {
		error = std::string("\"") + key + "\" must be an array of strings.";
		return false;
	};
	if (!member->is_array()) return refuse();
	for (const JsonValue &item : member->array) {
		if (!item.is_string()) return refuse();
		out.push_back(item.string);
	}
	return true;
}

JsonValue strings_to_json(const std::vector<std::string> &values) {
	JsonValue out = JsonValue::make_array();
	for (const std::string &value : values) out.push(str(value));
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
	if (!members_known(json, {"serial", "title", "mission", "multiplayer", "retail_directory", "runtime_executable",
	                          "play_retail"},
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
	    !text("retail_directory", change.retail_directory) || !text("runtime_executable", change.runtime_executable) ||
	    !flag("play_retail", change.play_retail))
		return false;
	out = std::move(change);
	return true;
}

JsonValue settings_to_json(const ProjectSettingsChange &change) {
	JsonValue out = JsonValue::make_object();
	if (change.serial) out.set("serial", num(double(change.serial)));
	if (change.title) out.set("title", str(*change.title));
	if (change.mission) out.set("mission", boolean(*change.mission));
	if (change.multiplayer) out.set("multiplayer", boolean(*change.multiplayer));
	if (change.retail_directory) out.set("retail_directory", str(*change.retail_directory));
	if (change.runtime_executable) out.set("runtime_executable", str(*change.runtime_executable));
	if (change.play_retail) out.set("play_retail", boolean(*change.play_retail));
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
	if (const JsonValue *kind = json.get("kind")) {
		if (!kind->is_number() || kind->number != std::floor(kind->number) || kind->number < -2147483648.0 ||
		    kind->number > 2147483647.0) { error = "\"kind\" must be a whole number."; return false; }
		edit.address.kind = static_cast<NodeKind>(kind->number);
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
	out.set("operation", str(edit_operation_token(edit.operation)));
	out.set("row", num(double(edit.address.row)));
	out.set("kind", num(double(edit.address.kind)));
	out.set("child", num(double(edit.address.child)));
	if (edit.parent) out.set("parent", num(double(edit.parent)));
	if (!edit.field.empty()) out.set("field", str(edit.field));
	out.set("value", value_to_json(edit.value));
	if (edit.position != SIZE_MAX) out.set("position", num(double(edit.position)));
	if (edit.coalesce) out.set("coalesce", boolean(true));
	if (edit.gesture) out.set("gesture", num(double(edit.gesture)));
	return out;
}

// A record's own collections (none for one that holds nothing), each record with the
// collections it holds in turn.
JsonValue collections_to_json(const Document &document, const NodeAddress &owner) {
	JsonValue collections = JsonValue::make_array();
	for (const Document::Collection &collection : document.collections_of(owner)) {
		JsonValue entry = JsonValue::make_object();
		entry.set("kind", num(double(collection.spec.kind)));
		entry.set("kind_name", str(collection.spec.kind_name));
		entry.set("label", str(collection.spec.label));
		if (collection.spec.fixed) entry.set("fixed", boolean(true));
		if (collection.spec.max) entry.set("max", num(double(collection.spec.max)));
		if (collection.spec.applies != Applicability::Reads) entry.set("applies", str(applicability_token(collection.spec.applies)));
		JsonValue records = JsonValue::make_array();
		for (const NodeId id : collection.ids) {
			const NodeAddress address{owner.row, collection.spec.kind, id};
			JsonValue record = JsonValue::make_object();
			record.set("id", num(double(id)));
			record.set("name", str(document.record_name(address)));
			record.set("change", str(record_change_token(document.record_change(address))));
			JsonValue nested = collections_to_json(document, address);
			if (!nested.array.empty()) record.set("collections", std::move(nested));
			records.push(std::move(record));
		}
		entry.set("records", std::move(records));
		collections.push(std::move(entry));
	}
	return collections;
}

// An import source as a request's `imports` takes it: {path, entry, retail, native}, the
// defaults left out, so the view's sources pass back as they are.
JsonValue import_source_to_json(const ImportSource &source) {
	JsonValue out = JsonValue::make_object();
	out.set("path", str(source.path));
	if (!source.entry.empty()) out.set("entry", str(source.entry));
	if (source.retail) out.set("retail", boolean(true));
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
bool field_of(const Document &document, const NodeAddress &address, const std::string &id, FieldSchema &field, Value &value) {
	if (!holds_record(document, address)) return false;
	for (const FieldSchema &schema : document.fields(address.kind))
		if (schema.id == id && document.get(address, id, value)) {
			field = document.field_on(address, schema);
			return true;
		}
	return false;
}

} // namespace

const char *editor_request_kind_token(EditorRequestKind kind) { return token_of(kKindTokens, kind); }
bool editor_request_kind_from_token(const std::string &token, EditorRequestKind &out) {
	return value_of(kKindTokens, token, out);
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
	for (const Token<EditorRequestKind> &row : kKindTokens) tokens.emplace_back(row.token);
	return tokens;
}

JsonValue value_to_json(const Value &value) {
	if (const auto *number = std::get_if<int64_t>(&value)) return num(double(*number));
	if (const auto *real = std::get_if<double>(&value)) return num(*real);
	return str(std::get<std::string>(value));
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
	if (!members_known(json, {"kind", "path", "text", "flag", "purpose", "paths", "names", "imports", "edit", "edits",
	                          "mode", "unsaved_choice", "settings"},
	                   "request", error)) return false;
	const JsonValue *kind = json.get("kind");
	if (!kind || !kind->is_string()) { error = "\"kind\" names the request (a string)."; return false; }
	EditorRequest request;
	if (!editor_request_kind_from_token(kind->string, request.kind)) {
		error = "Unknown request kind \"" + kind->string + "\".";
		return false;
	}
	if (!read_string(json, "path", request.path, error)) return false;
	if (!read_string(json, "text", request.text, error)) return false;
	if (!read_bool(json, "flag", request.flag, error)) return false;
	std::string purpose;
	if (!read_string(json, "purpose", purpose, error)) return false;
	if (!purpose.empty() && !pick_purpose_from_token(purpose, request.purpose)) {
		error = "Unknown pick purpose \"" + purpose + "\".";
		return false;
	}
	if (!read_strings(json, "paths", request.paths, error) || !read_strings(json, "names", request.names, error)) return false;
	if (const JsonValue *imports = json.get("imports")) {
		if (!imports->is_array()) { error = "\"imports\" must be an array of {path, entry}."; return false; }
		for (const JsonValue &source : imports->array) {
			if (!source.is_object()) { error = "\"imports\" must be an array of {path, entry}."; return false; }
			if (!members_known(source, {"path", "entry", "retail", "native"}, "import", error)) return false;
			ImportSource import;
			if (!read_string(source, "path", import.path, error) || !read_string(source, "entry", import.entry, error) ||
			    !read_bool(source, "retail", import.retail, error) || !read_bool(source, "native", import.native, error))
				return false;
			if (import.path.empty()) { error = "An import names its path."; return false; }
			request.imports.push_back(import);
		}
	}
	if (const JsonValue *edit = json.get("edit")) {
		if (!edit_from_json(*edit, request.edit, error)) return false;
	}
	if (const JsonValue *edits = json.get("edits")) {
		if (!edits->is_array()) { error = "\"edits\" must be an array of edits."; return false; }
		for (const JsonValue &member : edits->array) {
			Edit edit;
			if (!edit_from_json(member, edit, error)) return false;
			request.edits.push_back(std::move(edit));
		}
	}
	std::string mode;
	if (!read_string(json, "mode", mode, error)) return false;
	if (!mode.empty() && !select_mode_from_token(mode, request.select_mode)) {
		error = "Unknown selection mode \"" + mode + "\".";
		return false;
	}
	std::string choice;
	if (!read_string(json, "unsaved_choice", choice, error)) return false;
	if (!choice.empty() && !unsaved_choice_from_token(choice, request.unsaved_choice)) {
		error = "Unknown unsaved choice \"" + choice + "\".";
		return false;
	}
	if (const JsonValue *settings = json.get("settings"); settings && !settings_from_json(*settings, request.settings, error))
		return false;
	out = std::move(request);
	return true;
}

JsonValue editor_request_to_json(const EditorRequest &request) {
	JsonValue out = JsonValue::make_object();
	out.set("kind", str(editor_request_kind_token(request.kind)));
	if (!request.path.empty()) out.set("path", str(request.path));
	if (!request.text.empty()) out.set("text", str(request.text));
	if (request.flag) out.set("flag", boolean(true));
	if (request.purpose != PickPurpose::None) out.set("purpose", str(pick_purpose_token(request.purpose)));
	if (!request.paths.empty()) out.set("paths", strings_to_json(request.paths));
	if (!request.names.empty()) out.set("names", strings_to_json(request.names));
	if (!request.imports.empty()) {
		JsonValue imports = JsonValue::make_array();
		for (const ImportSource &source : request.imports) imports.push(import_source_to_json(source));
		out.set("imports", std::move(imports));
	}
	const bool carries_edit = request.kind == EditorRequestKind::EditRecord ||
	                          request.kind == EditorRequestKind::SelectRecord ||
	                          (request.kind == EditorRequestKind::RevertToSaved && request.edits.empty()) ||
	                          request.edit.address != NodeAddress{} || !request.edit.field.empty();
	if (carries_edit) out.set("edit", edit_to_json(request.edit));
	if (!request.edits.empty()) {
		JsonValue edits = JsonValue::make_array();
		for (const Edit &edit : request.edits) edits.push(edit_to_json(edit));
		out.set("edits", std::move(edits));
	}
	if (request.select_mode != SelectMode::Replace) out.set("mode", str(select_mode_token(request.select_mode)));
	if (request.kind == EditorRequestKind::ResolveUnsaved)
		out.set("unsaved_choice", str(unsaved_choice_token(request.unsaved_choice)));
	if (request.kind == EditorRequestKind::ApplyProjectSettings) out.set("settings", settings_to_json(request.settings));
	return out;
}

JsonValue diagnostic_to_json(const Diagnostic &d) {
	JsonValue out = JsonValue::make_object();
	out.set("severity", str(diagnostic_severity_label(d.severity)));
	out.set("code", str(d.code));
	out.set("message", str(d.message));
	if (!d.asset.empty()) out.set("asset", str(d.asset));
	if (!d.field.empty()) out.set("field", str(d.field));
	if (!d.record.empty()) out.set("record", str(d.record));
	if (d.line) out.set("line", num(double(d.line)));
	if (d.row_id) {
		out.set("row", num(double(d.row_id)));
		out.set("child", num(double(d.child_id)));
		out.set("kind", num(double(d.record_kind)));
	}
	if (!d.role.empty()) out.set("role", str(d.role));
	if (!d.target.empty()) out.set("target", str(d.target));
	if (d.reference != ReferenceKind::None) out.set("reference", str(reference_row(d.reference).token));
	if (!d.scope.empty()) out.set("scope", str(d.scope));
	if (d.material_type >= 0) out.set("material_type", num(double(d.material_type)));
	return out;
}

JsonValue diagnostics_to_json(const std::vector<Diagnostic> &diagnostics) {
	JsonValue out = JsonValue::make_array();
	for (const Diagnostic &d : diagnostics) out.push(diagnostic_to_json(d));
	return out;
}

JsonValue problem_fix_to_json(const ProblemFix &fix) {
	JsonValue out = JsonValue::make_object();
	out.set("label", str(fix.label));
	out.set("detail", str(fix.detail));
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
	out.set("total", num(double(answer.total())));
	out.set("shown", num(double(answer.rows.size())));
	JsonValue counts = JsonValue::make_object();
	counts.set("errors", num(double(answer.errors)));
	counts.set("warnings", num(double(answer.warnings)));
	counts.set("infos", num(double(answer.infos)));
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
		out.set("group_count", num(double(answer.groups.size())));
		JsonValue groups = JsonValue::make_array();
		for (size_t i = first; i < last; ++i) {
			if (i > first && group_of[i] == group_of[i - 1]) continue;
			const ProblemGroup &group = answer.groups[group_of[i]];
			JsonValue entry = JsonValue::make_object();
			entry.set("key", str(group.key));
			entry.set("title", str(group.title));
			entry.set("first", num(double(starts[group_of[i]])));
			entry.set("count", num(double(group.rows.size())));
			entry.set("errors", num(double(group.errors)));
			entry.set("warnings", num(double(group.warnings)));
			entry.set("infos", num(double(group.infos)));
			groups.push(std::move(entry));
		}
		out.set("groups", std::move(groups));
	}
	JsonValue problems = JsonValue::make_array();
	for (size_t i = first; i < last; ++i) {
		JsonValue row = diagnostic_to_json(view.diagnostics[answer.rows[i]]);
		if (answer.grouped) row.set("group", str(answer.groups[group_of[i]].key));
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
	out.set("findings", diagnostics_to_json(outcome.findings));
	return out;
}

JsonValue session_view_to_json(const SessionView &view, const SessionJsonOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("revision", num(double(view.revisions.any())));
	JsonValue revisions = JsonValue::make_object();
	for (size_t i = 0; i < kViewConcernCount; ++i) {
		const ViewConcern concern = static_cast<ViewConcern>(i);
		revisions.set(view_concern_token(concern), num(double(view.revisions.of(concern))));
	}
	out.set("revisions", std::move(revisions));
	out.set("status", str(view.status));
	// What waits on the unsaved-changes prompt, and the files its Save writes.
	JsonValue prompt = JsonValue::make_object();
	prompt.set("open", boolean(view.unsaved_prompt.open));
	if (view.unsaved_prompt.open) {
		prompt.set("action", str(editor_request_kind_token(view.unsaved_prompt.action)));
		if (!view.unsaved_prompt.target.empty()) prompt.set("target", str(view.unsaved_prompt.target));
		JsonValue files = JsonValue::make_array();
		for (const std::string &file : view.unsaved_prompt.files) files.push(str(file));
		prompt.set("files", std::move(files));
		prompt.set("can_discard", boolean(view.unsaved_prompt.can_discard));
	}
	out.set("unsaved_prompt", std::move(prompt));
	// What the last preview_rename planned: every site, before and after, and the refusals.
	if (view.rename_preview.serial) {
		const SessionView::RenamePreview &rename = view.rename_preview;
		JsonValue preview = JsonValue::make_object();
		preview.set("serial", num(double(rename.serial)));
		preview.set("symbol", boolean(rename.symbol));
		if (rename.symbol) preview.set("kind", str(reference_row(rename.kind).token));
		preview.set("path", str(rename.path));
		if (!rename.locator.empty()) preview.set("locator", str(rename.locator));
		if (!rename.field.empty()) preview.set("field", str(rename.field));
		preview.set("old_name", str(rename.old_name));
		preview.set("new_name", str(rename.new_name));
		JsonValue sites = JsonValue::make_array();
		for (const RenameSite &site : rename.sites) {
			JsonValue entry = JsonValue::make_object();
			entry.set("file", str(site.file));
			if (!site.record.empty()) entry.set("record", str(site.record));
			if (!site.locator.empty()) entry.set("locator", str(site.locator));
			entry.set("field", str(site.field));
			entry.set("before", str(site.before));
			entry.set("after", str(site.after));
			sites.push(std::move(entry));
		}
		preview.set("sites", std::move(sites));
		preview.set("refusals", diagnostics_to_json(rename.refusals));
		preview.set("ok", boolean(rename.refusals.empty()));
		out.set("rename_preview", std::move(preview));
	}
	// What the last apply_project_settings came to, under its serial.
	JsonValue settings_result = JsonValue::make_object();
	settings_result.set("serial", num(double(view.settings_result.serial)));
	settings_result.set("failures", diagnostics_to_json(view.settings_result.failures));
	out.set("settings_result", std::move(settings_result));
	out.set("quit_requested", boolean(view.quit_requested));

	JsonValue project = JsonValue::make_object();
	project.set("open", boolean(view.project_open));
	if (view.project_open) {
		project.set("root", str(view.project_root));
		project.set("title", str(view.document.title));
		project.set("id", str(view.document.project_id));
		project.set("target_game", str(view.document.target_game));
		JsonValue features = JsonValue::make_object();
		features.set("menu", boolean(view.document.features.menu));
		features.set("mission", boolean(view.document.features.mission));
		features.set("multiplayer", boolean(view.document.features.multiplayer));
		project.set("features", std::move(features));
		project.set("assets", num(double(view.scan.entries.size())));
		// Every file the scan lists, as Files lists them, each with its kind and whether the
		// editor opens it, so a client need not guess at kinds.
		JsonValue files = JsonValue::make_array();
		for (const AssetEntry &entry : view.scan.entries) {
			JsonValue file = JsonValue::make_object();
			file.set("path", str(entry.relative_path));
			file.set("name", str(entry.logical_name));
			file.set("kind", str(asset_kind_token(entry.kind)));
			file.set("editable", boolean(is_editable_kind(entry.kind)));
			files.push(std::move(file));
		}
		project.set("files", std::move(files));
	}
	out.set("project", std::move(project));

	JsonValue requirements = JsonValue::make_object();
	requirements.set("total", num(double(view.requirements.required_total)));
	requirements.set("missing", num(double(view.requirements.required_missing)));
	requirements.set("wrong_kind", num(double(view.requirements.required_wrong_kind)));
	JsonValue rows = JsonValue::make_array();
	for (const RequirementRow &row : view.requirements.rows) {
		JsonValue entry = JsonValue::make_object();
		entry.set("role", str(row.role));
		entry.set("name", str(row.name));
		entry.set("phase", str(requirement_phase_label(row.phase)));
		entry.set("severity", num(double(row.severity)));
		entry.set("required", boolean(row.required));
		entry.set("state", str(requirement_state_token(row.state)));
		entry.set("expected_kind", str(asset_kind_token(row.expected_kind)));
		if (!row.asset_path.empty()) entry.set("asset", str(row.asset_path));
		if (row.found_kind != AssetKind::Unknown) entry.set("found_kind", str(asset_kind_token(row.found_kind)));
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
	out.set("active_document", str(view.active_document));
	out.set("selection", address_to_json(view.selection));
	JsonValue selected = JsonValue::make_array();
	for (const NodeAddress &address : view.selected) selected.push(address_to_json(address));
	out.set("selected", std::move(selected));
	if (!view.reveal_field.empty()) out.set("reveal_field", str(view.reveal_field));
	if (!view.reveal_file.empty()) {
		JsonValue reveal = JsonValue::make_object();
		reveal.set("path", str(view.reveal_file));
		reveal.set("serial", num(double(view.reveal_file_serial)));
		if (view.reveal_file_rename) reveal.set("rename", boolean(true));
		out.set("reveal_file", std::move(reveal));
	}
	out.set("clipboard_bytes", num(double(view.clipboard.size())));

	JsonValue build = JsonValue::make_object();
	build.set("running", boolean(view.build_running));
	build.set("done", num(double(view.build_done)));
	build.set("total", num(double(view.build_total)));
	build.set("step", str(view.build_step));
	build.set("has_build", boolean(view.has_build));
	if (view.has_build) {
		build.set("ok", boolean(view.last_build.ok));
		build.set("id", str(view.last_build.build_id));
		build.set("dir", str(view.last_build.build_dir));
		build.set("reused_existing", boolean(view.last_build.reused_existing));
		build.set("archives_written", num(double(view.last_build.archives_written.size())));
		build.set("archives_reused", num(double(view.last_build.archives_reused.size())));
		build.set("loose_written", num(double(view.last_build.loose_written.size())));
		build.set("diagnostics", diagnostics_to_json(view.last_build.diagnostics));
	}
	out.set("build", std::move(build));

	JsonValue play = JsonValue::make_object();
	play.set("state", str(play_state_label(view.play_state)));
	play.set("pid", num(double(view.play_pid)));
	play.set("mcp_port", num(double(view.play_mcp_port)));
	play.set("command_line", str(view.play_command_line));
	play.set("exited_on_its_own", boolean(view.play_exited_on_its_own));
	play.set("exit_code", view.play_exit_code >= 0 ? num(double(view.play_exit_code)) : JsonValue::make_null());
	play.set("retail", boolean(view.play_retail));
	play.set("retail_directory", str(view.retail_directory));
	play.set("source_run", boolean(view.source_run));
	play.set("runtime_executable", str(view.runtime_executable));
	play.set("runtime_setting", str(view.runtime_setting));
	JsonValue boot_missing = JsonValue::make_array();
	for (const std::string &name : view.boot_missing) boot_missing.push(str(name));
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
	import.set("serial", num(double(preview.serial)));
	import.set("offset", num(double(options.import_offset)));
	const auto on_page = [&options](size_t index) {
		return index >= options.import_offset && index - options.import_offset < options.import_limit;
	};
	const auto sources_to_json = [&on_page](const std::vector<ImportSource> &sources) {
		JsonValue out = JsonValue::make_array();
		for (size_t i = 0; i < sources.size(); ++i)
			if (on_page(i)) out.push(import_source_to_json(sources[i]));
		return out;
	};
	import.set("choice_count", num(double(preview.choices.size())));
	import.set("choices", sources_to_json(preview.choices));
	import.set("root_count", num(double(preview.roots.size())));
	import.set("roots", sources_to_json(preview.roots));
	JsonValue planned = JsonValue::make_array();
	JsonValue not_found = JsonValue::make_array();
	size_t row_count = 0, not_found_count = 0;
	for (const ImportPlanRow &row : preview.plan.rows) {
		const bool found = row.state != ImportPlanRow::State::NotFound;
		if (!on_page(found ? row_count++ : not_found_count++)) continue;
		JsonValue entry = JsonValue::make_object();
		entry.set("state", str(row.state == ImportPlanRow::State::Selected ? "selected" : found ? "found" : "not_found"));
		entry.set("name", str(row.name));
		entry.set("kind", str(asset_kind_token(row.kind)));
		if (!row.needed_by.file.empty()) {
			JsonValue need = JsonValue::make_object();
			need.set("file", str(row.needed_by.file));
			need.set("record", str(row.needed_by.record));
			need.set("field", str(row.needed_by.field));
			need.set("reference", str(reference_row(row.needed_by.reference).token));
			need.set("name", str(row.needed_by.name));
			if (row.needed_by.material_type >= 0) need.set("material_type", num(double(row.needed_by.material_type)));
			entry.set("needed_by", std::move(need));
		}
		if (found) {
			entry.set("source", import_source_to_json(row.source));
			entry.set("destination", str(row.destination));
			if (!row.made_from.empty()) entry.set("made_from", str(row.made_from));
			entry.set("found_in", str(row.found_in));
			entry.set("selected", boolean(row.selected));
			if (!row.problem.empty()) entry.set("problem", str(row.problem));
			JsonValue rivals = JsonValue::make_array();
			for (const ImportRival &rival : row.rivals) {
				JsonValue other = JsonValue::make_object();
				other.set("name", str(rival.name));
				other.set("found_in", str(rival.found_in));
				other.set("differs", boolean(rival.differs));
				other.set("source", import_source_to_json(rival.source));
				rivals.push(std::move(other));
			}
			if (!row.rivals.empty()) entry.set("rivals", std::move(rivals));
		}
		(found ? planned : not_found).push(std::move(entry));
	}
	import.set("row_count", num(double(row_count)));
	import.set("rows", std::move(planned));
	import.set("not_found_count", num(double(not_found_count)));
	import.set("not_found", std::move(not_found));
	JsonValue not_followed = JsonValue::make_array();
	for (const ImportNotFollowed &kind : preview.plan.not_followed) {
		JsonValue entry = JsonValue::make_object();
		if (kind.reference != ReferenceKind::None) entry.set("reference", str(reference_row(kind.reference).token));
		else entry.set("kind", str(asset_kind_token(kind.kind)));
		entry.set("count", num(double(kind.count)));
		entry.set("first", str(kind.first));
		not_followed.push(std::move(entry));
	}
	import.set("not_followed", std::move(not_followed));
	import.set("truncated", boolean(preview.plan.truncated));
	import.set("diagnostics", diagnostics_to_json(preview.plan.diagnostics));
	// The importable sources in the project and what their importers made.
	JsonValue imported = JsonValue::make_array();
	for (const ImportedSource &source : view.imports) {
		JsonValue entry = JsonValue::make_object();
		entry.set("source", str(source.source));
		entry.set("sidecar", str(source.sidecar));
		entry.set("importer", str(source.importer));
		entry.set("ok", boolean(source.ok));
		entry.set("reimported", boolean(source.reimported));
		JsonValue outputs = JsonValue::make_array();
		for (const std::string &output : source.outputs) outputs.push(str(output));
		entry.set("outputs", std::move(outputs));
		imported.push(std::move(entry));
	}
	import.set("imported", std::move(imported));
	import.set("retail_files", num(double(view.retail_files.size())));
	out.set("import", std::move(import));

	size_t errors = 0, warnings = 0, infos = 0;
	for (const Diagnostic &d : view.diagnostics) {
		if (d.severity == DiagnosticSeverity::Error) ++errors;
		else if (d.severity == DiagnosticSeverity::Warning) ++warnings;
		else ++infos;
	}
	JsonValue problems = JsonValue::make_object();
	problems.set("count", num(double(view.diagnostics.size())));
	problems.set("errors", num(double(errors)));
	problems.set("warnings", num(double(warnings)));
	problems.set("infos", num(double(infos)));
	out.set("problems", std::move(problems));

	JsonValue recent = JsonValue::make_array();
	for (const std::string &root : view.recent_projects) recent.push(str(root));
	out.set("recent_projects", std::move(recent));

	JsonValue graph = JsonValue::make_object();
	if (view.graph) {
		graph.set("edges", num(double(view.graph->edges().size())));
		graph.set("symbols", num(double(view.graph->symbols().size())));
		graph.set("missing", num(double(view.graph->missing().size())));
		graph.set("files_extracted", num(double(view.graph->stats().files_extracted)));
		graph.set("files_reused", num(double(view.graph->stats().files_reused)));
		graph.set("files_failed", num(double(view.graph->stats().files_failed)));
	}
	out.set("graph", std::move(graph));

	JsonValue output = JsonValue::make_object();
	const size_t total = view.output.size();
	const size_t first = std::min(options.output_cursor, total);
	const size_t last = std::min(first + options.output_limit, total);
	output.set("total", num(double(total)));
	output.set("cursor", num(double(first)));
	output.set("next_cursor", num(double(last)));
	JsonValue lines = JsonValue::make_array();
	for (size_t i = first; i < last; ++i) lines.push(str(view.output[i]));
	output.set("lines", std::move(lines));
	out.set("output", std::move(output));
	return out;
}

JsonValue document_to_json(const Document &document, bool with_rows) {
	JsonValue out = JsonValue::make_object();
	out.set("path", str(document.path()));
	out.set("kind", str(asset_kind_token(document.kind())));
	out.set("dirty", boolean(document.dirty()));
	out.set("file_state_changed", boolean(document.file_state_changed()));
	out.set("blocked", boolean(document.blocked()));
	out.set("revision", num(double(document.revision())));
	out.set("can_undo", boolean(document.can_undo()));
	out.set("can_redo", boolean(document.can_redo()));
	out.set("ignored_lines", num(double(document.ignored_lines())));
	out.set("row_count", num(double(document.rows().size())));
	out.set("last_added", num(double(document.last_added())));
	JsonValue issues = JsonValue::make_array();
	for (const SourceIssue &issue : document.issues()) {
		JsonValue entry = JsonValue::make_object();
		entry.set("blocks", boolean(issue.blocks));
		entry.set("line", num(double(issue.line)));
		if (!issue.record.empty()) entry.set("record", str(issue.record));
		if (!issue.field.empty()) entry.set("field", str(issue.field));
		entry.set("message", str(issue.message));
		issues.push(std::move(entry));
	}
	out.set("issues", std::move(issues));
	JsonValue kinds = JsonValue::make_array();
	for (const Document::KindSpec &spec : document.top_kinds()) {
		JsonValue entry = JsonValue::make_object();
		entry.set("kind", num(double(spec.kind)));
		entry.set("label", str(spec.label));
		kinds.push(std::move(entry));
	}
	out.set("top_kinds", std::move(kinds));
	if (!with_rows) return out;
	JsonValue rows = JsonValue::make_array();
	for (const auto &row : document.rows()) {
		if (!row) continue;
		JsonValue entry = JsonValue::make_object();
		entry.set("id", num(double(row->id)));
		entry.set("kind", num(double(row->kind)));
		entry.set("kind_label", str(document.kind_label(row->kind)));
		entry.set("name", str(row->name()));
		entry.set("change", str(record_change_token(document.record_change({row->id, row->kind, 0}))));
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
	out.set("row", num(double(address.row)));
	out.set("kind", num(double(address.kind)));
	out.set("child", num(double(address.child)));
	out.set("kind_label", str(document.kind_label(address.kind)));
	out.set("name", str(document.record_name(address)));
	out.set("path", str(document.record_path(address)));
	out.set("locator", str(document.locator(address)));
	out.set("change", str(record_change_token(document.record_change(address))));
	if (address.child) {
		out.set("owner", address_to_json(at.owner));
		out.set("index", num(double(at.index)));
	}
	JsonValue fields = JsonValue::make_array();
	for (const FieldSchema &schema : document.fields(address.kind)) {
		Value value;
		if (!document.get(address, schema.id, value)) continue;
		const FieldSchema field = document.field_on(address, schema);
		JsonValue entry = JsonValue::make_object();
		entry.set("id", str(field.id));
		if (!field.label.empty()) entry.set("label", str(field.label));
		if (!field.section.empty()) entry.set("section", str(field.section));
		if (!field.group.empty()) entry.set("group", str(field.group));
		entry.set("type", str(field_type_token(field.type)));
		entry.set("value", value_to_json(value));
		// What the format table says of the field: its unit, its note, the key the file writes,
		// the range it keeps to, how it holds a colour.
		if (!field.unit.empty()) entry.set("unit", str(field.unit));
		if (!field.description.empty()) entry.set("description", str(field.description));
		if (!field.token.empty()) entry.set("token", str(field.token));
		if (field.ranged) {
			entry.set("min", num(field.min));
			entry.set("max", num(field.max));
			if (field.step > 0.0) entry.set("step", num(field.step));
		}
		if (field.color != FieldColor::None) entry.set("color", str(color_token(field.color)));
		if (field.width) entry.set("width", num(double(field.width)));
		if (field.read_only) entry.set("read_only", boolean(true));
		if (field.flags) entry.set("flags", boolean(true));
		if (field.optional) {
			entry.set("optional", boolean(true));
			entry.set("present", boolean(document.present(address, field.id)));
		}
		if (field.applies != Applicability::Reads) entry.set("applies", str(applicability_token(field.applies)));
		// Changed since the saved baseline: what the saved file holds (null when it lacks the
		// record), as the Inspector's mark and its tooltip show it.
		if (document.field_changed(address, field.id)) {
			entry.set("changed", boolean(true));
			Value saved;
			bool written = true;
			if (document.saved_value(address, field.id, saved, &written)) {
				entry.set("saved", value_to_json(saved));
				if (field.optional) entry.set("saved_present", boolean(written));
			} else {
				entry.set("saved", JsonValue::make_null());
			}
		}
		if (!field.choices.empty()) {
			JsonValue choices = JsonValue::make_array();
			for (const FieldChoice &choice : field.choices) {
				JsonValue option = JsonValue::make_object();
				option.set("name", str(choice.name));
				option.set("value", num(double(choice.value)));
				if (!choice.label.empty()) option.set("label", str(choice.label));
				choices.push(std::move(option));
			}
			entry.set("choices", std::move(choices));
		}
		// Open: any value typed, whether or not the record knows any (a model with no registers).
		if (field.open_choices) entry.set("open_choices", boolean(true));
		if (!field.scope.empty() && (field.reference != ReferenceKind::None || field.defines != ReferenceKind::None))
			entry.set("scope", str(field.scope));
		if (field.reference != ReferenceKind::None) {
			entry.set("reference", str(reference_row(field.reference).token));
			std::string symbol;
			const ReferenceStatus status = document.reference_status(field, value, view, &symbol);
			entry.set("reference_status", str(reference_status_token(status)));
			if (!symbol.empty()) entry.set("symbol", str(symbol));
			const std::string target = document.reference_target_file(field, value, view);
			if (!target.empty()) entry.set("reference_file", str(target));
		}
		if (field.defines != ReferenceKind::None) entry.set("defines", str(reference_row(field.defines).token));
		fields.push(std::move(entry));
	}
	out.set("fields", std::move(fields));
	out.set("collections", collections_to_json(document, address));
	return out;
}

JsonValue graph_edge_to_json(const AssetGraph &graph, const GraphEdge &edge) {
	JsonValue out = JsonValue::make_object();
	out.set("source", str(edge.source));
	if (!edge.record.empty()) out.set("record", str(edge.record));
	if (!edge.locator.empty()) out.set("locator", str(edge.locator));
	if (edge.address.row) out.set("address", address_to_json(edge.address));
	out.set("field", str(edge.field));
	out.set("kind", str(reference_row(edge.kind).token));
	out.set("value", str(edge.value));
	out.set("target", str(edge.target));
	if (!edge.scope.empty()) out.set("scope", str(edge.scope));
	out.set("rewritable", boolean(edge.rewritable));
	if (edge.through != ReferenceKind::None) out.set("through", str(reference_row(edge.through).token));
	if (edge.material_type >= 0) out.set("material_type", num(double(edge.material_type)));
	std::string file;
	const ReferenceStatus status = edge.target.empty() ? ReferenceStatus::NotAReference : graph.resolve(edge, &file);
	out.set("status", str(reference_status_token(status)));
	if (!file.empty()) out.set("file", str(file));
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
	out.set("kind", str(reference_row(symbol.kind).token));
	out.set("name", str(symbol.display));
	out.set("file", str(symbol.file));
	if (!symbol.record.empty()) out.set("record", str(symbol.record));
	if (!symbol.locator.empty()) out.set("locator", str(symbol.locator));
	if (symbol.address.row) out.set("address", address_to_json(symbol.address));
	if (!symbol.field.empty()) out.set("field", str(symbol.field));
	if (!symbol.scope.empty()) out.set("scope", str(symbol.scope));
	if (!symbol.value.empty()) out.set("value", str(symbol.value));
	if (symbol.inert) out.set("inert", boolean(true));
	if (!symbol.inert_reason.empty()) out.set("inert_reason", str(symbol.inert_reason));
	return out;
}

JsonValue reference_choices_to_json(const Document &document, const NodeAddress &address, const std::string &id,
                                    const SessionView &view) {
	FieldSchema field;
	Value value;
	if (!field_of(document, address, id, field, value)) return JsonValue::make_null();
	const std::vector<ReferenceChoice> choices = document.reference_choices(field, view);
	JsonValue list = JsonValue::make_array();
	for (const ReferenceChoice &choice : choices) {
		JsonValue entry = JsonValue::make_object();
		entry.set("name", str(choice.name));
		entry.set("kind", str(reference_row(choice.kind).token));
		entry.set("file", str(choice.file));
		if (!choice.record.empty()) entry.set("record", str(choice.record));
		entry.set("status", str(reference_status_token(choice.status)));
		if (choice.inert) {
			entry.set("inert", boolean(true));
			entry.set("reason", str(choice.reason));
		}
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	out.set("field", str(field.id));
	out.set("reference", str(reference_row(field.reference).token));
	if (!field.scope.empty()) out.set("scope", str(field.scope));
	out.set("count", num(double(choices.size())));
	out.set("choices", std::move(list));
	return out;
}

JsonValue reference_targets_to_json(const Document &document, const NodeAddress &address, const std::string &id,
                                    const SessionView &view) {
	FieldSchema field;
	Value value;
	if (!field_of(document, address, id, field, value)) return JsonValue::make_null();
	const std::vector<ReferenceTarget> targets = document.reference_targets(field, value, view);
	JsonValue list = JsonValue::make_array();
	for (const ReferenceTarget &target : targets) {
		JsonValue entry = JsonValue::make_object();
		entry.set("label", str(target.label));
		entry.set("file", str(target.file));
		if (!target.locator.empty()) entry.set("locator", str(target.locator));
		if (!target.field.empty()) entry.set("field", str(target.field));
		entry.set("editable", boolean(target.editable));
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	out.set("field", str(field.id));
	out.set("reference", str(reference_row(field.reference).token));
	out.set("value", value_to_json(value));
	out.set("count", num(double(targets.size())));
	out.set("targets", std::move(list));
	return out;
}

JsonValue document_hits_to_json(const std::vector<DocumentHit> &hits) {
	JsonValue list = JsonValue::make_array();
	for (const DocumentHit &hit : hits) {
		JsonValue entry = JsonValue::make_object();
		entry.set("id", num(double(hit.address.child ? hit.address.child : hit.address.row)));
		entry.set("address", address_to_json(hit.address));
		entry.set("record", str(hit.record));
		entry.set("locator", str(hit.locator));
		entry.set("field", str(hit.field));
		entry.set("label", str(hit.label));
		entry.set("text", str(hit.text));
		entry.set("at", num(double(hit.at)));
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	out.set("count", num(double(hits.size())));
	out.set("hits", std::move(list));
	return out;
}

JsonValue graph_search_to_json(const std::vector<GraphSearchHit> &hits) {
	JsonValue list = JsonValue::make_array();
	for (const GraphSearchHit &hit : hits) {
		JsonValue entry = hit.symbol ? graph_symbol_to_json(*hit.symbol) : JsonValue::make_object();
		if (!hit.symbol) {
			entry.set("kind", str("file"));
			entry.set("name", str(hit.name));
			entry.set("file", str(hit.file));
		}
		entry.set("usages", num(double(hit.usages)));
		list.push(std::move(entry));
	}
	JsonValue out = JsonValue::make_object();
	out.set("count", num(double(hits.size())));
	out.set("hits", std::move(list));
	return out;
}

} // namespace opennova::editor
