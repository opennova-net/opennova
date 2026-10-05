#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>

namespace opennova::editor {

// The fields of a request (EditorRequest, editor_request.h), one enumerator per member in the
// struct's order after its kind (ADR 0046 S13 A4). Each means one thing whatever the kind that
// takes it; the kind's row (request_kinds.h) lists the fields it takes and those it must carry.
enum class RequestFieldId : uint8_t {
	Dir,
	Title,
	Game,
	Expansion,
	BuildsOn,
	GameInstall,
	Path,
	Locator,
	Field,
	NewName,
	Role,
	FileKind,
	OutDir,
	ExportDir,
	Mission,
	Operation,
	Values,
	Roles,
	Names,
	Paths,
	Imports,
	Edits,
	Address,
	Records,
	PasteAt,
	Mode,
	Choice,
	Settings,
	Viewport,
	Drag,
	Command,
	Drop,
	Workspace,
	Purpose,
	WithDependencies,
	Replace,
	Force,
	AskName,
	OpenFirst,
	ImportPass,
	Rehash,
	All,
	Planned,
	Behind,
	Plan,
	Report,
	kCount,
};

inline constexpr size_t kRequestFieldCount = static_cast<size_t>(RequestFieldId::kCount);

// A field's type on the wire.
enum class RequestJson : uint8_t {
	String, // a string (a token for mode, choice and purpose)
	Boolean, // true or false
	Integer, // a whole number, 0 or more
	Strings, // an array of strings
	Object, // an object (address, paste_at, settings, viewport, drag, command, drop, workspace)
	Objects, // an array of objects (imports, edits, records)
};

// One row per field (request_fields.cpp, static_asserted into place as the reference-kind table
// is): its token on the wire (session_json, the editor MCP), its JSON type, and what it means.
struct RequestField {
	RequestFieldId id;
	const char *token;
	RequestJson json;
	const char *doc;
};

// A field's row; the Dir row for a value past the last field.
const RequestField &request_field(RequestFieldId id);
// The field `token` names on the wire; false for none ("kind" names none: it is the request's).
bool request_field_from_token(const std::string &token, RequestFieldId &out);
// A JSON type's word: "string", "boolean", "string[]", "object", "object[]".
const char *request_json_token(RequestJson json);

// A set of fields, one bit each.
using RequestFieldSet = uint64_t;
static_assert(kRequestFieldCount <= 64, "a RequestFieldSet holds every request field");

constexpr RequestFieldSet field_bit(RequestFieldId id) {
	return RequestFieldSet(1) << static_cast<unsigned>(id);
}

// The fields a request kind takes (its row's params column): those a request of the kind may
// carry, and among them those it must. The wire reader refuses a field outside `takes` and a
// request missing one of `required`.
struct RequestParams {
	RequestFieldSet takes = 0;
	RequestFieldSet required = 0;
	constexpr bool has(RequestFieldId id) const { return (takes & field_bit(id)) != 0; }
	constexpr bool needs(RequestFieldId id) const { return (required & field_bit(id)) != 0; }
};

// The params of a kind that must carry `required` and may carry `optional` too.
constexpr RequestParams request_params(std::initializer_list<RequestFieldId> required,
		std::initializer_list<RequestFieldId> optional = {}) {
	RequestParams params;
	for (const RequestFieldId id : required) {
		params.takes |= field_bit(id);
		params.required |= field_bit(id);
	}
	for (const RequestFieldId id : optional)
		params.takes |= field_bit(id);
	return params;
}

} // namespace opennova::editor
