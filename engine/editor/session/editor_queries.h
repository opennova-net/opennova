#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <base/io/json.h>
#include <editor/session/view/view_revisions.h>

namespace opennova::editor {

class SessionCore;

// What is asked of the session without a request (ADR 0046 S13 A5): one row per question, the one
// read seam the editor MCP, the Shell and the tests ask through (ProjectSession::query), as the
// request table (request_kinds.h) is the one write seam. A request changes the session and answers
// what it came to; a query changes nothing and answers what the session holds.
enum class EditorQueryKind : uint8_t {
	State,
	Files,
	Documents,
	Document,
	Record,
	ReferenceChoices,
	ReferenceTargets,
	DocumentSearch,
	Problems,
	References,
	Referrers,
	Usages,
	Missing,
	Symbols,
	ProjectSearch,
	MenuTree,
	MenuFindings,
	MenuRender,
	Viewport,
	ImportPreview,
	Output,
	Operation,
	BuildGate,
	Events,
	MissionLogic,
	MissionUses,
	ScriptAssist,
	Catalog,
	kCount,
};

inline constexpr size_t kEditorQueryKindCount = static_cast<size_t>(EditorQueryKind::kCount);

// A query param's type on the wire.
enum class QueryJson : uint8_t {
	String, // a string (a path, a token, a text)
	Integer, // a whole number, 0 or more (an identity, an offset, a revision)
	Number, // a number a float holds, finite (a point on a picture)
	Boolean, // true or false
	Strings, // an array of strings
};

// A string param that takes one of a list of tokens (S13 V7: the viewport query's op and kind): its
// name, and its index-th token (null past the last). The args check refuses any other, naming them,
// and the catalog writes them as the param's `enum`.
struct QueryChoices {
	const char *param = "";
	const char *(*token)(size_t index) = nullptr;
};

// One param a query takes: its name on the wire, its type, whether the query needs it, its default
// as JSON text ("100", "false", "\"none\""; null for none: left out, the query reads it as absent),
// and what it means.
struct QueryParam {
	const char *name = "";
	QueryJson type = QueryJson::String;
	bool required = false;
	const char *default_value = nullptr;
	const char *doc = "";
};

// The most entries one page of a list holds (the editor MCP's transport caps a list at 200), and
// the page a query answers when its args name no limit.
inline constexpr size_t kQueryPageMax = 200;
inline constexpr size_t kQueryPageDefault = 100;

struct EditorQueryRow;

// A query's args as its row's params read them. run_query checked each once, before the handler
// runs: every member a param of the row, every param the row needs there, each of its type, an
// integer 0 or more, a number one a float holds, a param with choices one of its tokens, and a paged
// row's `limit` from 1 to kQueryPageMax; a param left out reads as its default.
class QueryArgs {
public:
	QueryArgs(const EditorQueryRow &row, const io::JsonValue &args) : row_(row), args_(args) {}

	// Whether the args carry `name` (a default does not count).
	bool has(const char *name) const;
	std::string text(const char *name) const;
	int64_t integer(const char *name) const;
	double number(const char *name) const;
	bool boolean(const char *name) const;
	std::vector<std::string> strings(const char *name) const;
	// A paged row's page: from `offset` (a list in order) or `cursor` (a list by absolute index or
	// sequence number, which moves on while it is read), `limit` entries at most.
	size_t offset() const { return size_t(integer("offset")); }
	size_t limit() const { return size_t(integer("limit")); }
	uint64_t cursor() const { return uint64_t(integer("cursor")); }

private:
	const io::JsonValue *value(const char *name) const;

	const EditorQueryRow &row_;
	const io::JsonValue &args_;
};

// What a handler reads: the session's core, which reaches the view and every part of the session
// (the open documents, the Problems' caches). A handler asks, never changes.
struct QueryContext {
	SessionCore &core;
};

using QueryHandler = io::JsonValue (*)(
		const QueryContext &context, const QueryArgs &args, std::string &error);

// The query table: one row per EditorQueryKind, in the enum's order (static_asserted as the
// request table is): its token on the wire, its handler, the params it takes, the key of the list
// it pages (null for none: its offset or cursor and its limit checked once, the answer's `count`
// the list's whole length), the concerns its answer reads (one at least: every one whose move can
// change what it answers, ActiveDocument too where a pathless read follows the active document;
// the state's are every concern), whose stamps give the answer's `view_revision`, what it answers,
// and the tokens its string params with choices take (none for most).
struct EditorQueryRow {
	EditorQueryKind kind = EditorQueryKind::kCount;
	const char *token = "";
	QueryHandler handler = nullptr;
	const QueryParam *params = nullptr;
	size_t param_count = 0;
	const char *list_key = nullptr;
	ConcernSet reads = 0;
	const char *doc = "";
	const QueryChoices *choices = nullptr;
	size_t choice_count = 0;
};

// A kind's row; the Catalog row for a value past the last kind.
const EditorQueryRow &editor_query_row(EditorQueryKind kind);
// The kind a wire token names ("files"); false for none.
bool editor_query_from_token(std::string_view token, EditorQueryKind &out);
// A param type's word: "string", "integer", "number", "boolean", "string[]".
const char *query_json_token(QueryJson type);

// The query `name` answered over the session's core: its row found, its args checked (an object
// of its params, or null for none), its handler run, the answer stamped with `view_revision`, the
// view's clock value at which the concerns the row reads last moved (ViewRevisions::stamp_of: the
// state's is the clock itself), beside whatever `revision` of its own the answer carries (a
// document's, a menu's). Null with `error` (cleared first) naming the query for a name no row has,
// args it refuses, or a question it cannot answer.
io::JsonValue run_query(
		SessionCore &core, std::string_view name, const io::JsonValue &args, std::string &error);

} // namespace opennova::editor
