// opennova-project as the editor's session, headless (ADR 0046 S13 A7): the verb table (each verb's
// requests and query named in the request and query tables, its args ones the query takes); each
// verb's --json the very answer a session gives its query after the same requests, for all nine
// verbs; the request and query verbs answering as the Shell's request_json and query_json do, their
// refusals included; --install writing the project's local.json as the editor reads it; and, with
// the game install, a project imported from it whose Problems the query verb reads as the session
// does (the round's end-to-end check).
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#include <base/io/json.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project_build/build_run.h>
#include <editor/run/null_process_platform.h>
#include <editor/session/editor_queries.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/view/session_view.h>
#include <formats/pff/pff.h>

#include "cli_verbs.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/png_test_support.h"

namespace fs = std::filesystem;

using opennova::io::json_number;
using opennova::io::json_string;
using opennova::io::JsonValue;
using opennova::project_cli::CliAnswer;
using opennova::project_cli::CliVerb;
using opennova::project_cli::CliVerbRow;
using opennova::project_cli::cli_verb_from_token;
using opennova::project_cli::cli_verb_row;
using opennova::project_cli::kCliVerbCount;
using opennova::project_cli::run_project_command;

namespace editor = opennova::editor;
namespace request = opennova::editor::request;
using K = editor::EditorRequestKind;
using Q = editor::EditorQueryKind;

namespace {

// A run of the command: its exit code, what it printed and what it said on its error stream.
struct Ran {
	int code = -1;
	std::string out, err;
};

std::string read_back(std::FILE *file) {
	std::string text;
	std::rewind(file);
	char buffer[4096];
	for (size_t n; (n = std::fread(buffer, 1, sizeof(buffer), file)) > 0;) text.append(buffer, n);
	std::fclose(file);
	return text;
}

Ran run(const std::string &scratch, const std::vector<std::string> &args) {
	std::vector<const char *> argv;
	for (const std::string &a : args) argv.push_back(a.c_str());
	Ran ran;
	std::FILE *out = std::fopen((scratch + "/out.txt").c_str(), "w+b");
	std::FILE *err = std::fopen((scratch + "/err.txt").c_str(), "w+b");
	if (!out || !err) return ran;
	ran.code = run_project_command(static_cast<int>(argv.size()), argv.data(), out, err);
	ran.out = read_back(out);
	ran.err = read_back(err);
	if (!ran.err.empty()) std::fputs(ran.err.c_str(), stderr);
	return ran;
}

JsonValue parsed(const std::string &text) {
	JsonValue value;
	std::string error;
	if (!opennova::io::json_parse(text, value, error)) std::fprintf(stderr, "not JSON (%s): %s\n", error.c_str(), text.c_str());
	return value;
}

// Two answers the same, as the wire writes them; each said when not.
bool same(const JsonValue &a, const JsonValue &b) {
	const std::string left = opennova::io::json_write(a), right = opennova::io::json_write(b);
	if (left == right) return true;
	std::fprintf(stderr, "the command line answered:\n%.4000s\nthe session answered:\n%.4000s\n", left.c_str(), right.c_str());
	return false;
}

// The session a verb drives, as the command line makes it: no processes, its preferences in
// memory.
struct Headless {
	editor::NullProcessPlatform platform;
	editor::MemoryPreferencesStore preferences;
	editor::ProjectSession session{ platform, preferences };

	// A request handled and the operation it starts run to its end, as the command line sends one.
	void send(const editor::EditorRequest &r) {
		session.handle(r);
		session.run_operations();
	}
	JsonValue send_json(const JsonValue &r) {
		JsonValue answer = session.handle_json(r);
		session.run_operations();
		return answer;
	}
	// A query's answer, or {error} as the Shell's query_json answers a refusal.
	JsonValue ask(const std::string &name, const JsonValue &args) {
		std::string error;
		JsonValue answer = session.query(name, args, error);
		if (error.empty()) return answer;
		JsonValue refused = JsonValue::make_object();
		refused.set("error", json_string(error));
		return refused;
	}
	// A verb's row's query, with its row's args.
	JsonValue ask(const CliVerbRow &row) {
		return ask(editor::editor_query_row(row.query).token, row.query_args[0] ? parsed(row.query_args) : JsonValue());
	}
};

// The requests a parity case sends, as the verb's row names them (--install's aside).
bool sends(const CliVerbRow &row, std::initializer_list<K> kinds) {
	std::vector<K> named;
	for (size_t i = 0; i < row.request_count; ++i)
		if (row.requests[i] != K::ApplyProjectSettings) named.push_back(row.requests[i]);
	for (const K kind : kinds) {
		bool found = false;
		for (const K other : named) found = found || other == kind;
		if (!found) {
			std::fprintf(stderr, "%s's row does not name %s\n", row.token, editor::request_kind_row(kind).token);
			return false;
		}
	}
	return true;
}

// A project's folder as it is, put back before each run a parity case compares, so every run
// starts from the same files.
struct Saved {
	fs::path dir, copy;
	Saved(const std::string &from, const std::string &to) : dir(from), copy(to) {
		std::error_code ec;
		fs::remove_all(copy, ec);
		fs::copy(dir, copy, fs::copy_options::recursive, ec);
	}
	bool restore() const {
		std::error_code ec;
		fs::remove_all(dir, ec);
		fs::copy(copy, dir, fs::copy_options::recursive, ec);
		return !ec;
	}
};

JsonValue strings(std::initializer_list<const char *> values) {
	JsonValue out = JsonValue::make_array();
	for (const char *value : values) out.push(json_string(value));
	return out;
}

JsonValue import_request(const std::string &path, bool with_dependencies) {
	JsonValue source = JsonValue::make_object();
	source.set("path", json_string(path));
	JsonValue imports = JsonValue::make_array();
	imports.push(std::move(source));
	JsonValue out = JsonValue::make_object();
	out.set("kind", json_string("plan_import"));
	out.set("imports", std::move(imports));
	out.set("with_dependencies", JsonValue::make_bool(with_dependencies));
	return out;
}

const char *const kTitleMenu =
        "<SCREEN>\r\n<NAME>EXTRA</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"TITLE\">\r\n"
        "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>90</RIGHT><BOTTOM>90</BOTTOM></POSITION>\r\n"
        "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"COLOR\">FF0000</APPEARANCE>\r\n"
        "</WINDOW>\r\n</SCREEN>\r\n";

const char *const kFontMenu =
        "<SCREEN>\r\n<NAME>A</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"GO\">\r\n"
        "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>9</RIGHT><BOTTOM>9</BOTTOM></POSITION>\r\n"
        "<FONT><NAME>arial99</NAME></FONT>\r\n</WINDOW>\r\n</SCREEN>\r\n";

} // namespace

// The verb table against the request and query tables (the static_asserts' runtime half): every
// verb's token its own and found again, every request it sends a kind the session serves whose
// token names it again, its query a row of the query table whose token names it again and whose
// args the query takes, and the usage listing every verb.
static int test_verb_table() {
	editor_test::TempProjectDir dir("opennova_project_cli_table");
	const std::string root = dir.file("Table");
	TEST_EXPECT(run(dir.root(), { "new", root }).code == 0);
	Headless headless;
	headless.send(request::open_project(root));
	TEST_EXPECT(headless.session.project_open());
	size_t queries = 0, requests = 0;
	for (size_t v = 0; v < kCliVerbCount; ++v) {
		const CliVerbRow &row = cli_verb_row(static_cast<CliVerb>(v));
		CliVerb back = CliVerb::kCount;
		TEST_EXPECT(row.verb == static_cast<CliVerb>(v) && cli_verb_from_token(row.token, back) && back == row.verb);
		for (size_t i = 0; i < row.request_count; ++i) {
			const editor::RequestKindRow &kind = editor::request_kind_row(row.requests[i]);
			K token_kind = K::kCount;
			TEST_EXPECT(kind.kind == row.requests[i] && kind.served_by == editor::ServedBy::Session);
			TEST_EXPECT(editor::request_kind_from_token(kind.token, token_kind) && token_kind == row.requests[i]);
			++requests;
		}
		if (row.answer != CliAnswer::Query) continue;
		const editor::EditorQueryRow &query = editor::editor_query_row(row.query);
		Q token_query = Q::kCount;
		TEST_EXPECT(query.kind == row.query && editor::editor_query_from_token(query.token, token_query) &&
		            token_query == row.query);
		const JsonValue args = row.query_args[0] ? parsed(row.query_args) : JsonValue();
		std::string error;
		headless.session.query(query.token, args, error);
		if (!error.empty()) std::fprintf(stderr, "%s: %s\n", row.token, error.c_str());
		TEST_EXPECT(error.empty() && (args.is_null() || args.is_object()));
		++queries;
	}
	TEST_EXPECT(kCliVerbCount == 9 && queries == 7 && requests >= 2 * kCliVerbCount);
	TEST_EXPECT(cli_verb_row(CliVerb::Request).answer == CliAnswer::Request &&
	            cli_verb_row(CliVerb::Query).answer == CliAnswer::NamedQuery);
	const Ran usage = run(dir.root(), { "--help" });
	TEST_EXPECT(usage.code == 2);
	for (size_t v = 0; v < kCliVerbCount; ++v)
		TEST_EXPECT(usage.err.find(std::string("opennova-project ") + cli_verb_row(static_cast<CliVerb>(v)).token + " ") !=
		            std::string::npos);
	TEST_EXPECT(usage.err.find("pre-1.0") != std::string::npos && usage.err.find("--install") != std::string::npos);
	std::printf("verb table: %zu verbs, %zu requests named, %zu answer with a query\n", kCliVerbCount, requests, queries);
	return 0;
}

// Each verb's --json is the answer a session driven by the same requests gives its query (the
// command line prints the session's JSON and makes none of its own), for all nine verbs: every run
// starts from the same files.
static int test_verbs_answer_as_the_session() {
	editor_test::TempProjectDir dir("opennova_project_cli_parity");
	const std::string scratch = dir.root(), root = dir.file("Parity"), out = dir.file("out");
	TEST_EXPECT(run(scratch, { "new", root, "--title", "Parity" }).code == 0);
	TEST_EXPECT(run(scratch, { "create-missing", root }).code == 0);
	// A menu the render check notes, an import source, and a menu naming a font beside it outside.
	TEST_EXPECT(editor_test::write_text(root + "/menus/extra.mnu", kTitleMenu));
	TEST_EXPECT(editor_test::write_bytes(dir.file("logo.png"), editor_test::gradient_png(8, 8)));
	TEST_EXPECT(run(scratch, { "import", root, dir.file("logo.png") }).code == 0);
	TEST_EXPECT(editor_test::write_text(dir.file("art/a.mnu"), kFontMenu));
	TEST_EXPECT(editor_test::write_text(dir.file("art/arial99.fnt"), "fnt"));
	TEST_EXPECT(run(scratch, { "status", root }).code == 0); // the import pass has run: the files settle
	const Saved saved(root, dir.file("Parity.saved"));
	const auto fresh = [&]() {
		std::error_code ec;
		fs::remove_all(out, ec);
		return saved.restore();
	};

	// status and validate: the project opened, then the state (every section) and the problems.
	for (const CliVerb verb : { CliVerb::Status, CliVerb::Validate }) {
		const CliVerbRow &row = cli_verb_row(verb);
		TEST_EXPECT(sends(row, { K::OpenProject }) && row.import_pass);
		TEST_EXPECT(fresh());
		Headless headless;
		headless.send(request::open_project(root));
		const JsonValue expected = headless.ask(row);
		// validate exits 1 exactly when the build's gate would refuse a build.
		const bool blocked = verb == CliVerb::Validate &&
		                     headless.ask("build_gate", JsonValue::make_object()).get_bool("blocked", true);
		TEST_EXPECT(fresh());
		const Ran ran = run(scratch, { row.token, root, "--json" });
		TEST_EXPECT(ran.code == (blocked ? 1 : 0) && same(parsed(ran.out), expected));
	}

	// create-missing: the requirements asked, the roles unmet made, the requirements again.
	{
		const CliVerbRow &row = cli_verb_row(CliVerb::CreateMissing);
		TEST_EXPECT(sends(row, { K::OpenProject, K::CreateMissing }));
		const std::string bare = dir.file("Bare");
		TEST_EXPECT(run(scratch, { "new", bare }).code == 0);
		const Saved bare_saved(bare, dir.file("Bare.saved"));
		Headless headless;
		headless.send(request::open_project(bare));
		std::vector<std::string> roles;
		const JsonValue before = headless.ask(row);
		for (const JsonValue &requirement : before.get("requirements")->get("rows")->array)
			if (requirement.get_bool("required", false) && requirement.get_string("state", "") != "present")
				roles.push_back(requirement.get_string("role", ""));
		TEST_EXPECT(!roles.empty());
		headless.send(request::create_missing(roles));
		const JsonValue expected = headless.ask(row);
		TEST_EXPECT(bare_saved.restore());
		const Ran ran = run(scratch, { "create-missing", bare, "--json" });
		TEST_EXPECT(ran.code == 0 && same(parsed(ran.out), expected));
	}

	// import: the plan of a menu with the files it needs, a dry run (opened without the import
	// pass) and an import (the plan it carried out).
	{
		const CliVerbRow &row = cli_verb_row(CliVerb::Import);
		TEST_EXPECT(sends(row, { K::OpenProject, K::PlanImport, K::ImportFiles }) && row.import_pass);
		for (const bool dry_run : { true, false }) {
			TEST_EXPECT(fresh());
			Headless headless;
			headless.send(request::open_project(root, !dry_run));
			const JsonValue planned = headless.send_json(import_request(dir.file("art/a.mnu"), true));
			TEST_EXPECT(planned.get_bool("ok", false));
			const JsonValue expected = headless.ask(row);
			TEST_EXPECT(expected.get_number("count", 0) == 2); // a.mnu and the font it needs
			TEST_EXPECT(fresh());
			std::vector<std::string> args = { "import", root, dir.file("art/a.mnu"), "--with-dependencies", "--json" };
			if (dry_run) args.push_back("--dry-run");
			const Ran ran = run(scratch, args);
			TEST_EXPECT(ran.code == 0 && same(parsed(ran.out), expected));
			TEST_EXPECT(fs::exists(root + "/menus/a.mnu") != dry_run && fs::exists(root + "/fonts/arial99.fnt") != dry_run);
		}
	}

	// reimport: opened without the import pass, the pass forced, the import section.
	{
		const CliVerbRow &row = cli_verb_row(CliVerb::Reimport);
		TEST_EXPECT(sends(row, { K::OpenProject, K::Reimport }) && !row.import_pass);
		TEST_EXPECT(fresh());
		Headless headless;
		headless.send(request::open_project(root, false));
		headless.send(request::reimport(std::string(), true));
		const JsonValue expected = headless.ask(row);
		TEST_EXPECT(expected.get("import")->get("imported")->array.size() == 1);
		TEST_EXPECT(fresh());
		const Ran ran = run(scratch, { "reimport", root, "--force", "--json" });
		TEST_EXPECT(ran.code == 0 && same(parsed(ran.out), expected));
	}

	// build: opened without the import pass, the build run to its end, the import and operation
	// sections (the same content, the same build id).
	{
		const CliVerbRow &row = cli_verb_row(CliVerb::Build);
		TEST_EXPECT(sends(row, { K::OpenProject, K::Build }) && !row.import_pass);
		TEST_EXPECT(fresh());
		Headless headless;
		headless.send(request::open_project(root, false));
		headless.send(request::build(out));
		const JsonValue expected = headless.ask(row);
		TEST_EXPECT(expected.get("operation")->get("build")->get_bool("ok", false));
		TEST_EXPECT(fresh());
		const Ran ran = run(scratch, { "build", root, "--out", out, "--json" });
		TEST_EXPECT(ran.code == 0 && same(parsed(ran.out), expected));
		TEST_EXPECT(!editor::last_good_build_dir(out).empty());
	}

	// new: the project made and opened, its project section (its id is made anew each time).
	{
		const CliVerbRow &row = cli_verb_row(CliVerb::New);
		TEST_EXPECT(sends(row, { K::NewProject }));
		const std::string made = dir.file("Made");
		Headless headless;
		headless.send(request::new_project(made, "Made", "jo"));
		JsonValue expected = headless.ask(row);
		std::error_code ec;
		fs::remove_all(made, ec);
		const Ran ran = run(scratch, { "new", made, "--title", "Made", "--game", "jo", "--json" });
		JsonValue answer = parsed(ran.out);
		TEST_EXPECT(ran.code == 0 && !answer.get("project")->get_string("id", "").empty() &&
		            !expected.get("project")->get_string("id", "").empty());
		answer.get("project")->set("id", json_string("fresh"));
		expected.get("project")->set("id", json_string("fresh"));
		TEST_EXPECT(same(answer, expected));
	}

	// request: a request as the editor MCP sends it, answered as handle_json answers it.
	{
		JsonValue create = JsonValue::make_object();
		create.set("kind", json_string("create_file"));
		create.set("path", json_string("fresh.mnu"));
		TEST_EXPECT(fresh());
		Headless headless;
		headless.send(request::open_project(root));
		const JsonValue expected = headless.send_json(create);
		TEST_EXPECT(expected.get("outcome")->get_bool("done", false));
		TEST_EXPECT(fresh());
		const Ran ran = run(scratch, { "request", root, opennova::io::json_write(create) });
		TEST_EXPECT(ran.code == 0 && same(parsed(ran.out), expected) && fs::exists(root + "/menus/fresh.mnu"));
	}

	// query: the plan's end-to-end check, `query <project> problems --json` answering as the
	// session's problems query (and as the editor MCP's editor_query, which is it), and a page of
	// the files.
	{
		TEST_EXPECT(fresh());
		Headless headless;
		headless.send(request::open_project(root));
		const JsonValue problems = headless.ask("problems", JsonValue::make_object());
		JsonValue page = JsonValue::make_object();
		page.set("limit", json_number(3));
		const JsonValue files = headless.ask("files", page);
		TEST_EXPECT(problems.get_number("total", 0) > 0 && files.get("files")->array.size() == 3);
		TEST_EXPECT(fresh());
		const Ran asked = run(scratch, { "query", root, "problems", "--json" });
		TEST_EXPECT(asked.code == 0 && same(parsed(asked.out), problems));
		TEST_EXPECT(fresh());
		const Ran paged = run(scratch, { "query", root, "files", "{\"limit\": 3}" });
		TEST_EXPECT(paged.code == 0 && same(parsed(paged.out), files));
	}
	return 0;
}

// The request and query verbs refuse as the Shell's request_json and query_json refuse: a request
// that does not read (a field its kind does not take, text that is not JSON) exits 2 with the
// answer naming why and nothing asked of the session; one refused exits 1; a query no row has, or
// args it refuses, exits 2 with {error}; and a build requested runs to its end before the command
// exits. An array of requests is handled in turn in the run's one session: an edit and the save
// that writes it; an edit alone is said to end with the run, and the file keeps its bytes.
static int test_request_and_query_verbs() {
	editor_test::TempProjectDir dir("opennova_project_cli_wire");
	const std::string scratch = dir.root(), root = dir.file("Wire");
	TEST_EXPECT(run(scratch, { "new", root, "--title", "Wire" }).code == 0);
	TEST_EXPECT(run(scratch, { "create-missing", root }).code == 0);

	Ran ran = run(scratch, { "request", root, "{\"kind\": \"build\", \"path\": \"x\"}" });
	JsonValue answer = parsed(ran.out);
	TEST_EXPECT(ran.code == 2 && !answer.get_bool("ok", true) &&
	            answer.get_string("error", "") == "build takes no \"path\" (it takes out_dir).");
	ran = run(scratch, { "request", root, "{\"kind\": " });
	answer = parsed(ran.out);
	TEST_EXPECT(ran.code == 2 && answer.get_string("error", "").rfind("The request is not JSON: ", 0) == 0);
	// Refused as it is served: a required file there already is never made again.
	ran = run(scratch, { "request", root, "{\"kind\": \"create_missing\", \"roles\": [\"main_menu\"]}" });
	answer = parsed(ran.out);
	TEST_EXPECT(ran.code == 1 && answer.get_bool("ok", false) && !answer.get("outcome")->get_bool("done", true));
	// A build requested: its operation runs to its end before the command exits.
	const std::string out = dir.file("out");
	JsonValue build = JsonValue::make_object();
	build.set("kind", json_string("build"));
	build.set("out_dir", json_string(out));
	ran = run(scratch, { "request", root, opennova::io::json_write(build) });
	answer = parsed(ran.out);
	TEST_EXPECT(ran.code == 0 && answer.get("outcome")->get_number("operation", 0) == 1);
	TEST_EXPECT(!editor::last_good_build_dir(out).empty());

	// An edit, then the save that writes it, in one run; the edit alone writes nothing.
	const std::string menu = root + "/menus/main.mnu";
	std::string before, after, io_error;
	TEST_EXPECT(editor::read_file_text(menu, before, io_error) && before.find("STARTUP2") == std::string::npos);
	const std::string edit =
	        "{\"kind\": \"edit_record\", \"path\": \"menus/main.mnu\", \"open_first\": true, \"edits\": "
	        "[{\"op\": \"set\", \"id\": 1, \"field\": \"name\", \"value\": \"STARTUP2\"}]}";
	ran = run(scratch, { "request", root, edit });
	TEST_EXPECT(ran.code == 0 && parsed(ran.out).get("outcome")->get_bool("done", false));
	TEST_EXPECT(ran.err.find("menus/main.mnu's unsaved edits end with this run") != std::string::npos);
	TEST_EXPECT(editor::read_file_text(menu, after, io_error) && after == before);
	ran = run(scratch, { "request", root, "[" + edit + ", {\"kind\": \"save\", \"path\": \"menus/main.mnu\"}]" });
	answer = parsed(ran.out);
	TEST_EXPECT(ran.code == 0 && answer.is_array() && answer.array.size() == 2 && ran.err.empty());
	for (const JsonValue &one : answer.array) TEST_EXPECT(one.get("outcome")->get_bool("done", false));
	TEST_EXPECT(editor::read_file_text(menu, after, io_error) && after.find("STARTUP2") != std::string::npos);
	// The first that is not done ends the array: the save after a refused request is never sent.
	ran = run(scratch, { "request", root,
	                     "[{\"kind\": \"create_missing\", \"roles\": [\"main_menu\"]}, {\"kind\": \"save_all\"}]" });
	answer = parsed(ran.out);
	TEST_EXPECT(ran.code == 1 && answer.is_array() && answer.array.size() == 1);

	Headless headless;
	headless.send(request::open_project(root));
	for (const std::vector<std::string> &query : std::vector<std::vector<std::string>>{
	             { "nope" }, { "files", "{\"limit\": 0}" }, { "references" }, { "files", "[1]" }, { "files", "{" } }) {
		std::vector<std::string> args = { "query", root };
		args.insert(args.end(), query.begin(), query.end());
		ran = run(scratch, args);
		answer = parsed(ran.out);
		TEST_EXPECT(ran.code == 2 && !answer.get_string("error", "").empty() && answer.object.size() == 1);
		if (query.size() == 1 || query[1] != "{") {
			JsonValue query_args = JsonValue::make_object();
			if (query.size() > 1) query_args = parsed(query[1]);
			TEST_EXPECT(same(answer, headless.ask(query[0], query_args)));
		} else {
			TEST_EXPECT(answer.get_string("error", "").rfind("The query's args are not JSON: ", 0) == 0);
		}
	}
	return 0;
}

// --install sets the project's game install as the editor's project settings set it: written to
// its .opennova/local.json, which the editor then opens the project with; kept absolute from the
// command line's working directory; on new, the new project's; and on import, the install whose
// files --entry names.
static int test_install() {
	struct WorkingDirectory {
		fs::path saved = fs::current_path();
		~WorkingDirectory() {
			std::error_code ignored;
			fs::current_path(saved, ignored);
		}
	} const restore;
	editor_test::TempProjectDir dir("opennova_project_cli_install");
	const std::string scratch = dir.root(), root = dir.file("Install"), install = dir.file("Joint Ops");
	std::error_code ec;
	fs::create_directories(install, ec);
	const std::string font = "fnt";
	const opennova::pff::PffWriteEntry entries[] = {
	        { "arial99.fnt", reinterpret_cast<const uint8_t *>(font.data()), uint32_t(font.size()), 0, 0, 0 } };
	TEST_EXPECT(opennova::pff::pff_write_archive((install + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3,
	                                             entries, 1) == opennova::pff::PFF_WRITE_OK);
	TEST_EXPECT(run(scratch, { "new", root, "--title", "Install" }).code == 0);
	fs::current_path(dir.path, ec);
	TEST_EXPECT(!ec);
	Ran ran = run(scratch, { "status", root, "--install", "art/../Joint Ops" });
	TEST_EXPECT(ran.code == 0 && ran.out.find("game install: " + install + "\n") != std::string::npos);
	editor::LocalSettings local;
	editor::Diagnostic finding;
	TEST_EXPECT(editor::load_local_settings(editor::ProjectPaths::for_root(root), local, finding) &&
	            local.game_install == install && finding.code.empty());
	{
		// As the editor reads it: a session with no install of its own opens the project on it.
		Headless headless;
		headless.send(request::open_project(root));
		TEST_EXPECT(headless.session.project_open() && headless.session.view().project.retail_directory == install);
	}
	// A new project's, and an import's: the install --entry names the files of.
	const std::string other = dir.file("Other");
	TEST_EXPECT(run(scratch, { "new", other, "--install", install }).code == 0);
	TEST_EXPECT(editor::load_local_settings(editor::ProjectPaths::for_root(other), local, finding) &&
	            local.game_install == install);
	const std::string third = dir.file("Third");
	TEST_EXPECT(run(scratch, { "new", third }).code == 0);
	ran = run(scratch, { "import", third, "--install", install, "--entry", "arial99.fnt" });
	TEST_EXPECT(ran.code == 0 && ran.out.find("imported fonts/arial99.fnt") != std::string::npos);
	TEST_EXPECT(fs::is_regular_file(third + "/fonts/arial99.fnt"));
	TEST_EXPECT(editor::load_local_settings(editor::ProjectPaths::for_root(third), local, finding) &&
	            local.game_install == install);
	// An --entry the install lacks: refused, nothing imported.
	ran = run(scratch, { "import", third, "--entry", "nothing.fnt" });
	TEST_EXPECT(ran.code == 1 && ran.err.find("nothing.fnt") != std::string::npos);
	return 0;
}

// Retail: a project imported from the game install (the plan's end-to-end check): the main menu
// and the stylesheet with the files they need, then its Problems read through the query and
// validate verbs, each the answer the session gives the same project.
static int test_retail_install_import() {
	const std::string install = retail::install();
	if (install.empty())
		return retail::skip_leg("OPENNOVA_JO_DIR (a project imported from the game install, its Problems through query)");
	editor_test::TempProjectDir dir("opennova_project_cli_retail");
	const std::string scratch = dir.root(), root = dir.file("Retail");
	TEST_EXPECT(run(scratch, { "new", root, "--title", "Retail" }).code == 0);
	Ran ran = run(scratch, { "import", root, "--install", install, "--entry", "main.mnu", "--entry", "menu_style.mns",
	                         "--with-dependencies" });
	TEST_EXPECT(ran.code == 0 && ran.out.find("imported menus/main.mnu") != std::string::npos);
	TEST_EXPECT(fs::is_regular_file(root + "/menus/main.mnu"));
	Headless headless;
	headless.send(request::open_project(root));
	const JsonValue problems = headless.ask("problems", JsonValue::make_object());
	ran = run(scratch, { "query", root, "problems", "--json" });
	TEST_EXPECT(ran.code == 0 && same(parsed(ran.out), problems));
	ran = run(scratch, { "validate", root, "--json" });
	TEST_EXPECT(same(parsed(ran.out), problems));
	std::printf("retail: %d problems after importing main.mnu and menu_style.mns with the files they need\n",
	            int(problems.get_number("total", 0)));
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_verb_table();
	failures += test_verbs_answer_as_the_session();
	failures += test_request_and_query_verbs();
	failures += test_install();
	failures += test_retail_install_import();
	if (failures == 0) std::printf("project_cli_session: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
