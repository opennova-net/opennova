#pragma once
// opennova-project (pre-1.0, experimental): the OpenNova Editor's project session on the command
// line (ADR 0046 d4, S13 A7). Each run is one headless ProjectSession
// (editor/session/project_session.h) over a process seam with no processes and preferences kept in
// memory, driven only as the editor MCP drives the editor's: the requests its verb's row names
// (the request table, request_kinds.h), each handled in turn with the operation it starts run to
// its end, then its query (the query table, editor_queries.h), whose answer it prints, as text or,
// with --json, as the Shell's query_json gives it.
//
//   opennova-project new <dir> [--title <text>] [--game <code>] [--expansion <name>] [--builds-on <expansion>]
//   opennova-project status <dir>
//   opennova-project validate <dir>
//   opennova-project create-missing <dir> [--role <token>]
//   opennova-project import <dir> [<source>] [--entry <name>]... [--replace] [--with-dependencies]
//                    [--all] [--dry-run [--rows]]
//   opennova-project reimport <dir> [--force] [--source <path>]
//   opennova-project new-terrain <dir> <name> --heightmap <file> --colormap <file> [--detail <file>]
//                    [--tiles <file>] [--top <units>] [--water <units>] [--layout island|tiled]
//   opennova-project build <dir> [--out <dir>]
//   opennova-project request <dir> <json>
//   opennova-project query <dir> <name> [<json>]
//
// Every verb also takes --install <dir>, the project's game install, a folder that is there (taken
// from where the command runs and lexically normal, as the settings keep it, before it is looked
// for): a verb that writes sets it as the editor's project settings set it (apply_project_settings:
// the project's .opennova/local.json, replacing the install it names) before its own requests, and
// a dry run opens the project on it for that run alone, writing nothing; and --json. A run is one
// session: what it holds (an open document, an unsaved edit, a selection) ends with it, so the
// request verb takes an array of requests to handle in turn (an edit, then the save that writes
// it).
//
// Exit 0 on success; 1 when validate found what a build would be refused for (the build_gate
// query: the build follows retail, refused where the game would fail to load or run as retail
// does, or where the editor cannot vouch for what it packs; validate lists every Problems row, and
// an error the build does not gate on, a listed code's or a project check's, fails nothing),
// create-missing left a role it was asked for unmet, an import found a problem in its plan,
// reported a finding or stopped at the plan's cap, a reimport was refused, a build was blocked or
// failed, or a request was refused, is the editor's shell's to serve (nothing done) or its operation
// did not end done; 2 on a usage error (an option or an argument it does not take, an --install
// that names no folder, a request or a query's args that are not JSON, a query or a request kind no
// table has), a project that could not be made or opened, a game install that could not be set, a
// request that did not read or a query that did not answer.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include <editor/session/editor_queries.h>
#include <editor/session/editor_request.h>

namespace opennova::project {

enum class CliVerb : uint8_t {
	New,
	Status,
	Validate,
	CreateMissing,
	Import,
	Reimport,
	NewTerrain,
	Build,
	Export,
	Request,
	Query,
	kCount,
};

inline constexpr size_t kCliVerbCount = static_cast<size_t>(CliVerb::kCount);

// What a verb prints once its requests are handled.
enum class CliAnswer : uint8_t {
	Query, // its row's query, asked with its row's args
	Request, // what the request its arguments carry came to (handle_json's answer: the request verb)
	NamedQuery, // the query its arguments name, with their args (the query verb)
};

// One row per verb (cli_verbs.cpp, static_asserted into place as the request and query tables
// are): its token, its usage and what it does, the requests it sends and the query whose answer
// it prints.
struct CliVerbRow {
	CliVerb verb = CliVerb::kCount;
	const char *token = "";
	const char *synopsis = ""; // its arguments, as the usage writes them after the token
	const char *doc = ""; // what it does, as the usage says it
	// The requests it sends, in order: the one that opens the project (new_project; open_project
	// for every other verb), then apply_project_settings when --install names the game install,
	// then its own, those an option or its arguments choose among them (the request verb's own is
	// the one its arguments carry).
	const editor::EditorRequestKind *requests = nullptr;
	size_t request_count = 0;
	// Whether its open runs the project's import pass first (false where its own request runs it:
	// a build, a reimport); a dry run (import --dry-run) opens without it too, writing nothing.
	bool import_pass = true;
	CliAnswer answer = CliAnswer::Query;
	// The query it answers with (kCount unless its answer is its row's query) and the args it asks
	// it with, as JSON ("" for none: the state's every section, a list's first page).
	editor::EditorQueryKind query = editor::EditorQueryKind::kCount;
	const char *query_args = "";
};

// A verb's row; the Query verb's for a value past the last verb.
const CliVerbRow &cli_verb_row(CliVerb verb);
// The verb a token names ("create-missing"); false for none.
bool cli_verb_from_token(const std::string &token, CliVerb &out);

// `argv[0]` is the verb; `argc` counts it. What the verb answers goes to `out`; its usage errors,
// the findings of its requests and its other notes go to `err`.
int run_project_command(int argc, const char *const *argv, std::FILE *out, std::FILE *err);

} // namespace opennova::project
