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
	BaseProject,
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
	Fresh,
	Plan,
	Report,
	Steps,
	Folder,
	Start,
	PlayMode,
	SaveBeforePlay,
	Alone,
	Define,
	PreviewBackground,
	KeepSelection,
	Create,
	Event,
	Variable,
	Value,
	Stroke,
	Gesture,
	Cancel,
	TerrainView,
	kCount,
};

inline constexpr size_t kRequestFieldCount = static_cast<size_t>(RequestFieldId::kCount);

// A field's type on the wire.
enum class RequestJson : uint8_t {
	String, // a string (a token for mode, choice and purpose)
	Boolean, // true or false
	Integer, // a whole number (0 or more, unless its row says otherwise)
	Strings, // an array of strings
	Object, // an object (address, paste_at, settings, viewport, drag, command, drop, workspace, define)
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

// A set of fields, one bit each, in two words (the fields outgrew one, S24).
struct RequestFieldSet {
	uint64_t words[2] = {0, 0};
	constexpr RequestFieldSet() = default;
	// 0, the empty set (`RequestFieldSet set = 0;`).
	constexpr RequestFieldSet(int) {}
	constexpr RequestFieldSet operator|(const RequestFieldSet &o) const {
		RequestFieldSet out;
		out.words[0] = words[0] | o.words[0];
		out.words[1] = words[1] | o.words[1];
		return out;
	}
	constexpr RequestFieldSet operator&(const RequestFieldSet &o) const {
		RequestFieldSet out;
		out.words[0] = words[0] & o.words[0];
		out.words[1] = words[1] & o.words[1];
		return out;
	}
	constexpr RequestFieldSet operator~() const {
		RequestFieldSet out;
		out.words[0] = ~words[0];
		out.words[1] = ~words[1];
		return out;
	}
	constexpr RequestFieldSet &operator|=(const RequestFieldSet &o) {
		words[0] |= o.words[0];
		words[1] |= o.words[1];
		return *this;
	}
	constexpr bool any() const { return (words[0] | words[1]) != 0; }
	constexpr explicit operator bool() const { return any(); }
	constexpr bool operator!() const { return !any(); }
	// Compared with 0: whether the set is empty.
	constexpr bool operator==(int) const { return !any(); }
	constexpr bool operator!=(int) const { return any(); }
};
static_assert(kRequestFieldCount <= 128, "a RequestFieldSet holds every request field");

constexpr RequestFieldSet field_bit(RequestFieldId id) {
	RequestFieldSet out;
	const unsigned bit = static_cast<unsigned>(id);
	out.words[bit / 64] = uint64_t(1) << (bit % 64);
	return out;
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
