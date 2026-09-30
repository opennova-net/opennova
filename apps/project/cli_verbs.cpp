#include "cli_verbs.h"

#include <initializer_list>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <base/io/json.h>
#include <base/io/strutil.h>
#include <editor/run/null_process_platform.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_json.h>

namespace opennova::project_cli {

namespace {

using editor::EditorQueryKind;
using editor::EditorRequest;
using editor::EditorRequestKind;
using io::json_string;
using io::JsonValue;

using K = EditorRequestKind;
using Q = EditorQueryKind;

// --- the arguments ------------------------------------------------------------------------------

// An option a verb takes: its name, what its value is as the usage and a missing value say it
// ("a text"; null for a switch), and whether it may be given more than once.
struct CliOption {
	const char *name = "";
	const char *value = nullptr;
	bool repeats = false;
};

// An argument a verb takes by its place, as a missing one is said ("a project directory").
struct CliPositional {
	const char *words = "";
	bool required = true;
};

// The options every verb takes: the project's game install, and the answer as JSON.
constexpr CliOption kCommonOptions[] = {
	{ "--install", "the game install folder" },
	{ "--json" },
};

// What a verb was given: its arguments by place (the project's directory first) and its options
// in the order given.
struct CliArgs {
	std::vector<std::string> positional;
	std::vector<std::pair<std::string, std::string>> options; // name, value ("" for a switch)

	bool has(const char *name) const {
		for (const auto &option : options)
			if (option.first == name) return true;
		return false;
	}
	// The value it was last given, "" for none.
	std::string value(const char *name) const {
		std::string out;
		for (const auto &option : options)
			if (option.first == name) out = option.second;
		return out;
	}
	std::vector<std::string> values(const char *name) const {
		std::vector<std::string> out;
		for (const auto &option : options)
			if (option.first == name) out.push_back(option.second);
		return out;
	}
};

// --- the session -------------------------------------------------------------------------------

// One run of the command: the headless session it drives (a process seam with no processes, the
// preferences in memory, both ending with it) and where it prints.
struct Cli {
	Cli(std::FILE *out_to, std::FILE *err_to, bool as_json) : out(out_to), err(err_to), json(as_json) {}

	editor::NullProcessPlatform platform;
	editor::MemoryPreferencesStore preferences;
	editor::ProjectSession session{ platform, preferences };
	std::FILE *out;
	std::FILE *err;
	bool json; // --json: the answer as JSON, no text
};

// A member of a JSON object, a null value when it has none.
const JsonValue &at(const JsonValue &object, const char *key) {
	static const JsonValue none;
	const JsonValue *found = object.get(key);
	return found ? *found : none;
}

// An array member's items (none when it is absent).
const std::vector<JsonValue> &items(const JsonValue &object, const char *key) {
	return at(object, key).array;
}

size_t count_at(const JsonValue &object, const char *key) {
	const double number = object.get_number(key, 0.0);
	return number > 0.0 ? static_cast<size_t>(number) : 0;
}

JsonValue boolean(bool value) {
	return JsonValue::make_bool(value);
}

// The state query's args for the sections named.
JsonValue sections(std::initializer_list<const char *> names) {
	JsonValue list = JsonValue::make_array();
	for (const char *name : names) list.push(json_string(name));
	JsonValue args = JsonValue::make_object();
	args.set("sections", std::move(list));
	return args;
}

void print_json(std::FILE *to, const JsonValue &answer) {
	std::fputs(io::json_write(answer).c_str(), to);
}

// A finding as the command line prints it: its severity, code and message, then the file, the
// line, the record and the field it names.
void print_finding(std::FILE *to, const JsonValue &finding) {
	std::string line = finding.get_string("severity", "") + " " + finding.get_string("code", "") + ": " +
	                   finding.get_string("message", "");
	const std::string asset = finding.get_string("asset", "");
	if (!asset.empty()) line += " [" + asset + "]";
	if (const JsonValue *number = finding.get("line"); number && number->is_number())
		line += " line " + std::to_string(static_cast<long long>(number->number));
	const std::string record = finding.get_string("record", ""), field = finding.get_string("field", "");
	if (!record.empty()) line += " record " + record;
	if (!field.empty()) line += " field " + field;
	std::fprintf(to, "%s\n", line.c_str());
}

// The findings a request's outcome carries, each on the error stream.
void print_findings(std::FILE *to, const JsonValue &outcome) {
	for (const JsonValue &finding : items(outcome, "findings")) print_finding(to, finding);
}

bool done(const JsonValue &outcome) {
	return outcome.get_bool("done", false);
}

bool has_error(const std::vector<JsonValue> &findings) {
	for (const JsonValue &finding : findings)
		if (finding.get_string("severity", "") == "error") return true;
	return false;
}

// A request handled and the operation it starts run to its end (a build packs): what it came to
// (action_outcome_to_json's: done, the operation, the findings).
JsonValue handled(Cli &cli, const EditorRequest &request) {
	cli.session.handle(request);
	JsonValue outcome = editor::action_outcome_to_json(cli.session.outcome());
	cli.session.run_operations();
	return outcome;
}

// A request the verb sends: handled, its findings printed on the error stream.
JsonValue send(Cli &cli, const EditorRequest &request) {
	JsonValue outcome = handled(cli, request);
	print_findings(cli.err, outcome);
	return outcome;
}

// A request in its wire form, as the editor MCP sends it (ProjectSession::handle_json), the
// operation it starts run to its end: its answer ({ok, served, error?, outcome, status,
// view_revision}).
JsonValue send_json(Cli &cli, const JsonValue &request) {
	JsonValue answer = cli.session.handle_json(request);
	cli.session.run_operations();
	return answer;
}

// A request the verb makes from what the session answered (an import plan's sources, in the wire
// form the plan gives them), sent: its outcome, its findings printed; false, said why, when it
// did not read.
bool send_wire(Cli &cli, const JsonValue &request, JsonValue &outcome) {
	const JsonValue answer = send_json(cli, request);
	if (!answer.get_bool("ok", false)) {
		std::fprintf(cli.err, "opennova-project: %s\n", answer.get_string("error", "").c_str());
		return false;
	}
	outcome = at(answer, "outcome");
	print_findings(cli.err, outcome);
	return true;
}

// The query `kind` answered with `args` (null for none); null with `error` when it did not answer.
JsonValue ask(Cli &cli, Q kind, const JsonValue &args, std::string &error) {
	return cli.session.query(editor::editor_query_row(kind).token, args, error);
}

// A row's query args (null for none).
JsonValue args_of(const CliVerbRow &row) {
	JsonValue args;
	std::string error;
	if (row.query_args[0] != 0) io::json_parse(row.query_args, args, error);
	return args;
}

// The row's query, asked with its args: false, said why, when it does not answer.
bool answer_of(Cli &cli, const CliVerbRow &row, JsonValue &answer) {
	std::string error;
	answer = ask(cli, row.query, args_of(row), error);
	if (error.empty()) return true;
	std::fprintf(cli.err, "opennova-project: %s\n", error.c_str());
	return false;
}

// A paged query's pages: its first asked with `args`, then, when `whole`, each from the one
// before's next_offset until the list ends. False, said why, when it does not answer.
bool ask_pages(Cli &cli, Q kind, JsonValue args, bool whole, std::vector<JsonValue> &pages) {
	for (;;) {
		std::string error;
		JsonValue page = ask(cli, kind, args, error);
		if (!error.empty()) {
			std::fprintf(cli.err, "opennova-project: %s\n", error.c_str());
			return false;
		}
		const JsonValue next = at(page, "next_offset");
		pages.push_back(std::move(page));
		if (!whole || !next.is_number()) return true;
		if (!args.is_object()) args = JsonValue::make_object();
		args.set("offset", next);
	}
}

// The row's paged query, asked with the row's args.
bool pages_of(Cli &cli, const CliVerbRow &row, bool whole, std::vector<JsonValue> &pages) {
	return ask_pages(cli, row.query, args_of(row), whole, pages);
}

bool project_open(Cli &cli) {
	std::string error;
	const JsonValue state = ask(cli, Q::State, sections({ "project" }), error);
	return error.empty() && at(state, "project").get_bool("open", false);
}

// A request that sets the project up failed: its errors said as the command's own, its other
// findings as findings.
void print_setup_failure(Cli &cli, const JsonValue &outcome) {
	for (const JsonValue &finding : items(outcome, "findings")) {
		if (finding.get_string("severity", "") == "error")
			std::fprintf(cli.err, "opennova-project: %s\n", finding.get_string("message", "").c_str());
		else
			print_finding(cli.err, finding);
	}
}

// The verb's first requests: the project opened (made first, for new; with its import pass unless
// the verb's open skips it or the verb makes a dry run, which writes nothing), then, with
// --install, its game install set as the editor's project settings set it. False, said why, when
// the project does not open or the install is not set (exit 2).
bool set_up(Cli &cli, const CliVerbRow &row, const CliArgs &args) {
	const std::string &dir = args.positional.front();
	const JsonValue opened = handled(
	        cli, row.requests[0] == K::NewProject
	                     ? editor::request::new_project(dir, args.value("--title"), args.value("--game"))
	                     : editor::request::open_project(dir, row.import_pass && !args.has("--dry-run")));
	if (!done(opened) || !project_open(cli)) {
		print_setup_failure(cli, opened);
		if (!has_error(items(opened, "findings")))
			std::fprintf(cli.err, "opennova-project: the project at %s did not open\n", dir.c_str());
		return false;
	}
	print_findings(cli.err, opened);
	if (!args.has("--install")) return true;
	editor::ProjectSettingsChange change;
	change.game_install = args.value("--install");
	const JsonValue applied = handled(cli, editor::request::apply_project_settings(change));
	if (done(applied)) {
		print_findings(cli.err, applied);
		return true;
	}
	print_setup_failure(cli, applied);
	std::fprintf(cli.err, "opennova-project: the game install %s was not set\n", change.game_install->c_str());
	return false;
}

// --- the verbs ----------------------------------------------------------------------------------

int run_new(Cli &cli, const CliVerbRow &row, const CliArgs &) {
	JsonValue answer;
	if (!answer_of(cli, row, answer)) return 2;
	if (cli.json) {
		print_json(cli.out, answer);
		return 0;
	}
	const JsonValue &project = at(answer, "project");
	std::fprintf(cli.out, "created %s (%s) at %s\n", project.get_string("title", "").c_str(),
	             project.get_string("target_game", "").c_str(), project.get_string("root", "").c_str());
	return 0;
}

int run_status(Cli &cli, const CliVerbRow &row, const CliArgs &) {
	JsonValue answer;
	if (!answer_of(cli, row, answer)) return 2;
	if (cli.json) {
		print_json(cli.out, answer);
		return 0;
	}
	const JsonValue &project = at(answer, "project"), &run = at(answer, "run");
	std::fprintf(cli.out, "project: %s (%s) at %s\n", project.get_string("title", "").c_str(),
	             project.get_string("target_game", "").c_str(), project.get_string("root", "").c_str());
	const std::string runtime = run.get_string("runtime_executable", "");
	std::fprintf(cli.out, "runtime: %s\n", runtime.empty() ? "(beside the editor)" : runtime.c_str());
	const std::string install = run.get_string("game_install", "");
	if (!install.empty()) std::fprintf(cli.out, "game install: %s\n", install.c_str());
	std::fprintf(cli.out, "assets: %zu file(s)\n", count_at(project, "file_count"));
	const std::vector<JsonValue> &imported = items(at(answer, "import"), "imported");
	if (!imported.empty()) {
		size_t now = 0, failed = 0;
		for (const JsonValue &source : imported) {
			now += source.get_bool("reimported", false) ? 1 : 0;
			failed += source.get_bool("ok", true) ? 0 : 1;
		}
		std::fprintf(cli.out, "imports: %zu source(s), %zu imported now, %zu failed\n", imported.size(), now, failed);
	}
	const JsonValue &requirements = at(answer, "requirements");
	std::fprintf(cli.out, "requirements: %zu required, %zu missing, %zu wrong kind (missions %s)\n",
	             count_at(requirements, "total"), count_at(requirements, "missing"), count_at(requirements, "wrong_kind"),
	             at(project, "features").get_bool("mission", false) ? "on" : "off");
	const JsonValue &problems = at(answer, "problem_counts");
	std::fprintf(cli.out, "problems: %zu error(s), %zu warning(s), %zu info\n", count_at(problems, "errors"),
	             count_at(problems, "warnings"), count_at(problems, "infos"));
	return 0;
}

// A finding as validate tells two apart: its severity, code, message and file.
std::string finding_key(const JsonValue &finding) {
	return finding.get_string("severity", "") + "\n" + finding.get_string("code", "") + "\n" +
	       finding.get_string("message", "") + "\n" + finding.get_string("asset", "");
}

// Every Problems row, as the editor's Problems window lists them (errors, then warnings, then
// notes), then the verdict of the build's gate (the build_gate query): validate fails exactly when
// a build would be refused. A finding that blocks the build and no row shows (the build's own check
// of the files: an archive in the project) is said before the verdict; an error row the build does
// not gate on (the render check's) is listed and fails nothing.
int run_validate(Cli &cli, const CliVerbRow &row, const CliArgs &) {
	std::vector<JsonValue> pages, gate;
	if (!pages_of(cli, row, !cli.json, pages) || !ask_pages(cli, Q::BuildGate, JsonValue(), !cli.json, gate))
		return 2;
	const bool blocked = gate.front().get_bool("blocked", false);
	if (cli.json) {
		print_json(cli.out, pages.front());
		return blocked ? 1 : 0;
	}
	std::set<std::string> shown;
	for (const JsonValue &page : pages) {
		for (const JsonValue &problem : items(page, "problems")) {
			print_finding(cli.out, problem);
			shown.insert(finding_key(problem));
		}
	}
	for (const JsonValue &page : gate) {
		for (const JsonValue &finding : items(page, "blocking")) {
			if (shown.count(finding_key(finding))) continue;
			std::fputs("blocks a build: ", cli.out);
			print_finding(cli.out, finding);
		}
	}
	const size_t errors = count_at(at(pages.front(), "counts"), "errors");
	if (blocked)
		std::fprintf(cli.out, "not ok: %zu finding(s) block a build\n", count_at(gate.front(), "count"));
	else if (errors > 0)
		std::fprintf(cli.out, "ok: nothing blocks a build (%zu error(s) the build does not gate on)\n", errors);
	else
		std::fprintf(cli.out, "ok: 0 error(s)\n");
	return blocked ? 1 : 0;
}

// A requirement row of the requirements section by its role; null for none.
const JsonValue *requirement_of(const JsonValue &answer, const std::string &role) {
	for (const JsonValue &row : items(at(answer, "requirements"), "rows"))
		if (row.get_string("role", "") == role) return &row;
	return nullptr;
}

bool present(const JsonValue *requirement) {
	return requirement && requirement->get_string("state", "") == "present";
}

// The role --role names, else every required row the project does not meet (the requirements as
// the project opened), made from scratch; then the requirements again.
int run_create_missing(Cli &cli, const CliVerbRow &row, const CliArgs &args) {
	JsonValue before, after;
	if (!answer_of(cli, row, before)) return 2;
	std::vector<std::string> roles;
	if (args.has("--role")) {
		roles.push_back(args.value("--role"));
	} else {
		for (const JsonValue &requirement : items(at(before, "requirements"), "rows"))
			if (requirement.get_bool("required", false) && !present(&requirement))
				roles.push_back(requirement.get_string("role", ""));
	}
	const JsonValue outcome = send(cli, editor::request::create_missing(roles));
	if (!answer_of(cli, row, after)) return 2;
	// Complete when the request was done and every role asked for is met now; a role no row has is
	// refused (its finding says so).
	bool complete = done(outcome);
	std::vector<std::string> lines;
	size_t created = 0;
	for (const std::string &role : roles) {
		const JsonValue *now = requirement_of(after, role);
		if (!now) {
			complete = false;
			continue;
		}
		if (present(now)) {
			if (!present(requirement_of(before, role))) {
				++created;
				lines.push_back("created " + now->get_string("asset", ""));
			}
			continue;
		}
		complete = false;
		lines.push_back(now->get_string("name", "") +
		                (now->get_string("state", "") == "wrong_kind" ? " is still of the wrong kind" : " is still missing"));
	}
	if (cli.json) {
		print_json(cli.out, after);
	} else {
		for (const std::string &line : lines) std::fprintf(cli.out, "%s\n", line.c_str());
		std::fprintf(cli.out, "%zu file(s) created%s\n", created, complete ? "" : ", some requirements remain");
	}
	return complete ? 0 : 1;
}

// What a reference that wants a file is: the file naming it, the record and the field.
std::string need_words(const JsonValue &need) {
	std::string out = need.get_string("file", "");
	const std::string record = need.get_string("record", ""), field = need.get_string("field", "");
	if (!record.empty()) out += ": " + record;
	if (!field.empty()) out += (record.empty() ? ": " : " ") + field;
	return out;
}

void print_not_found(std::FILE *to, const JsonValue &row) {
	std::fprintf(to, "not found %s (%s), needed by %s\n", row.get_string("name", "").c_str(),
	             row.get_string("kind", "").c_str(), need_words(at(row, "needed_by")).c_str());
}

// An import's plan, one line per file (the import_preview query's pages): what it takes, where it
// puts it, what wanted it, where it comes from and the other places that have it; what it cannot
// take and why; what is not found; the kinds not followed; whether the cap stopped it.
void print_plan(std::FILE *to, const std::vector<JsonValue> &pages) {
	size_t take = 0;
	for (const JsonValue &page : pages) {
		for (const JsonValue &row : items(page, "rows")) {
			const bool selected = row.get_bool("selected", false);
			take += selected ? 1 : 0;
			std::string line = std::string(selected ? "take " : "skip ") + row.get_string("name", "") + " (" +
			                   row.get_string("kind", "") + ") -> " + row.get_string("destination", "");
			line += row.get_string("state", "") == "found" ? ", needed by " + need_words(at(row, "needed_by"))
			                                                : std::string(", chosen");
			const std::string made_from = row.get_string("made_from", ""), found_in = row.get_string("found_in", "");
			line += made_from.empty() ? ", from " + found_in : ", made from " + made_from + ", " + found_in;
			const std::string problem = row.get_string("problem", "");
			if (!problem.empty()) line += ": " + problem;
			std::fprintf(to, "%s\n", line.c_str());
			for (const JsonValue &rival : items(row, "rivals"))
				std::fprintf(to, "  also in %s as %s (%s)\n", rival.get_string("found_in", "").c_str(),
				             rival.get_string("name", "").c_str(),
				             rival.get_bool("differs", false) ? "the files differ" : "the same file");
		}
	}
	for (const JsonValue &page : pages)
		for (const JsonValue &row : items(page, "not_found")) print_not_found(to, row);
	const JsonValue &plan = pages.front();
	for (const JsonValue &entry : items(plan, "not_followed")) {
		const std::string reference = entry.get_string("reference", "");
		if (reference.empty())
			std::fprintf(to, "not followed: what %s files name (%zu, the first %s)\n", entry.get_string("kind", "").c_str(),
			             count_at(entry, "count"), entry.get_string("first", "").c_str());
		else
			std::fprintf(to, "not followed: %s references, which name no file (%zu, the first in %s)\n",
			             reference.c_str(), count_at(entry, "count"), entry.get_string("first", "").c_str());
	}
	if (plan.get_bool("truncated", false))
		std::fprintf(to, "the plan stopped at %zu files: the files past them are not in it\n", count_at(plan, "count"));
	std::fprintf(to, "plan: %zu file(s) to import, %zu not found\n", take, count_at(plan, "not_found_count"));
}

// An import source in its wire form, as a request's imports take it.
JsonValue import_source(const std::string &path, const std::string &entry) {
	JsonValue source = JsonValue::make_object();
	source.set("path", json_string(path));
	if (!entry.empty()) source.set("entry", json_string(entry));
	return source;
}

// A source (on disk, or an archive's members --entry names), or the game install's files --entry
// names, planned as the editor's import dialog plans them (with the files they need, found beside
// them or in the game install, with --with-dependencies); then, unless a dry run, what the plan
// takes imported, the whole selection or none of it, and the project read again as the editor
// reads it after an import.
int run_import(Cli &cli, const CliVerbRow &row, const CliArgs &args) {
	const std::string source = args.positional.size() > 1 ? args.positional[1] : std::string();
	const std::vector<std::string> entries = args.values("--entry");
	const bool with_dependencies = args.has("--with-dependencies"), dry_run = args.has("--dry-run");
	JsonValue planned;
	if (!source.empty()) {
		JsonValue imports = JsonValue::make_array();
		if (entries.empty()) imports.push(import_source(source, std::string()));
		for (const std::string &entry : entries) imports.push(import_source(source, entry));
		JsonValue request = JsonValue::make_object();
		request.set("kind", json_string(editor::editor_request_kind_token(K::PlanImport)));
		request.set("imports", std::move(imports));
		request.set("with_dependencies", boolean(with_dependencies));
		if (!send_wire(cli, request, planned)) return 2;
	} else {
		planned = send(cli, editor::request::preview_install_import(entries, with_dependencies));
	}
	std::vector<JsonValue> pages;
	if (!pages_of(cli, row, true, pages)) return 2;
	const JsonValue &plan = pages.front();
	for (const JsonValue &finding : items(plan, "diagnostics")) print_finding(cli.err, finding);
	// A plan refused (a name the game install does not have) or with an error (a source that could
	// not be read or converted): nothing is imported, as the import refuses the selection.
	const bool plan_errors = !done(planned) || has_error(items(plan, "diagnostics"));
	const bool truncated = plan.get_bool("truncated", false);
	if (dry_run) {
		if (cli.json)
			print_json(cli.out, plan);
		else
			print_plan(cli.out, pages);
		return plan_errors || truncated ? 1 : 0;
	}
	if (plan_errors) {
		if (cli.json) print_json(cli.out, plan);
		return 1;
	}
	// What the plan takes and nothing else: the sources it holds (the files a converter makes from
	// one, whole) and each dependency found that the project can take; the files past its cap are
	// not imported.
	JsonValue taken = JsonValue::make_array();
	std::vector<std::string> taken_sources, destinations;
	for (const JsonValue &page : pages) {
		for (const JsonValue &file : items(page, "rows")) {
			if (!file.get_bool("selected", false)) {
				std::fprintf(cli.err, "not importing %s: %s\n", file.get_string("name", "").c_str(),
				             file.get_string("problem", "").c_str());
				continue;
			}
			destinations.push_back(file.get_string("destination", ""));
			const std::string key = io::json_write(at(file, "source"));
			bool listed = false;
			for (const std::string &other : taken_sources) listed = listed || other == key;
			if (listed) continue;
			taken_sources.push_back(key);
			taken.push(at(file, "source"));
		}
	}
	if (!cli.json)
		for (const JsonValue &page : pages)
			for (const JsonValue &missing : items(page, "not_found")) print_not_found(cli.out, missing);
	if (truncated)
		std::fprintf(cli.err, "the plan stopped at %zu files: the files past them are not imported (import fewer at once)\n",
		             count_at(plan, "count"));
	JsonValue request = JsonValue::make_object();
	request.set("kind", json_string(editor::editor_request_kind_token(K::ImportFiles)));
	request.set("imports", std::move(taken));
	request.set("replace", boolean(args.has("--replace")));
	JsonValue outcome;
	if (!send_wire(cli, request, outcome)) return 2;
	if (!cli.json && done(outcome))
		for (const std::string &destination : destinations) std::fprintf(cli.out, "imported %s\n", destination.c_str());
	// As the editor reads the project again after an import: a copied-in source is imported now,
	// and a failure on a file this command brought in fails the command.
	std::string error;
	const JsonValue state = ask(cli, Q::State, sections({ "import" }), error);
	bool import_errors = false;
	for (const JsonValue &imported : items(at(state, "import"), "imported")) {
		if (!imported.get_bool("reimported", false)) continue;
		const std::string name = imported.get_string("source", "");
		if (!cli.json)
			std::fprintf(cli.out, "imported %s -> %zu output(s)\n", name.c_str(), items(imported, "outputs").size());
		if (!imported.get_bool("ok", true))
			for (const std::string &destination : destinations) import_errors = import_errors || destination == name;
	}
	if (cli.json) print_json(cli.out, plan);
	return items(outcome, "findings").empty() && !import_errors && !truncated ? 0 : 1;
}

// The import pass run now, --force importing again every source (or the --source one) even when
// nothing changed; the project opens without its own pass, so this one is the only one.
int run_reimport(Cli &cli, const CliVerbRow &row, const CliArgs &args) {
	const JsonValue outcome = send(cli, editor::request::reimport(args.value("--source"), args.has("--force")));
	JsonValue answer;
	if (!answer_of(cli, row, answer)) return 2;
	if (cli.json) {
		print_json(cli.out, answer);
	} else {
		const std::vector<JsonValue> &sources = items(at(answer, "import"), "imported");
		size_t reimported = 0;
		for (const JsonValue &source : sources) {
			const bool now = source.get_bool("reimported", false);
			reimported += now ? 1 : 0;
			std::fprintf(cli.out, "%s %s -> %zu output(s)%s\n", now ? "imported" : "kept",
			             source.get_string("source", "").c_str(), items(source, "outputs").size(),
			             source.get_bool("ok", true) ? "" : " (failed)");
		}
		std::fprintf(cli.out, "%zu source(s), %zu imported\n", sources.size(), reimported);
	}
	return done(outcome) ? 0 : 1;
}

// The project packed into a directory the runtime boots, as the editor's Build packs it: the
// import pass first (the project opens without its own, so the build's is the only one), then the
// build run to its end.
int run_build(Cli &cli, const CliVerbRow &row, const CliArgs &args) {
	const JsonValue outcome = send(cli, editor::request::build(args.value("--out")));
	JsonValue answer;
	if (!answer_of(cli, row, answer)) return 2;
	const JsonValue &operation = at(answer, "operation");
	const JsonValue &build = at(operation, "build"), &last = at(operation, "last_operation");
	// The operation this request started, landed.
	const bool landed = done(outcome) && last.get_number("id", 0.0) == outcome.get_number("operation", -1.0) &&
	                    last.get_string("end", "") == "done" && build.get_bool("ok", false);
	if (cli.json) {
		print_json(cli.out, answer);
		return landed ? 0 : 1;
	}
	for (const JsonValue &imported : items(at(answer, "import"), "imported"))
		if (imported.get_bool("reimported", false))
			std::fprintf(cli.out, "imported %s -> %zu output(s)\n", imported.get_string("source", "").c_str(),
			             items(imported, "outputs").size());
	if (!done(outcome)) return 1; // refused: its findings said why
	const std::vector<JsonValue> &findings = items(build, "diagnostics");
	for (const JsonValue &finding : findings) print_finding(cli.out, finding);
	if (!landed) {
		bool blocked = false;
		for (const JsonValue &finding : findings) blocked = blocked || finding.get_string("code", "") == "build.blocked";
		std::fprintf(cli.out, "%s\n", blocked ? "not ok: the project cannot be built until these are fixed"
		                                      : "not ok: build failed");
		return 1;
	}
	const std::string dir = build.get_string("dir", "");
	if (build.get_bool("reused_existing", false))
		std::fprintf(cli.out, "unchanged: %s\n", dir.c_str());
	else
		std::fprintf(cli.out, "built %s (%zu archive(s) written, %zu reused, %zu loose file(s))\n", dir.c_str(),
		             count_at(build, "archives_written"), count_at(build, "archives_reused"),
		             count_at(build, "loose_written"));
	std::fprintf(cli.out, "run: opennova.exe -- --resource-dir \"%s\"\n", dir.c_str());
	return 0;
}

// What a request's answer (handle_json's) comes to for the exit code: 2 when it did not read, 1
// when it was not done or the operation it started did not end done (its findings said), else 0.
int answer_code(Cli &cli, const JsonValue &answer) {
	if (!answer.get_bool("ok", false)) return 2;
	const JsonValue &outcome = at(answer, "outcome");
	if (!done(outcome)) return 1;
	const double operation = outcome.get_number("operation", 0.0);
	if (operation == 0.0) return 0;
	std::string error;
	const JsonValue ended = at(ask(cli, Q::Operation, JsonValue(), error), "last_operation");
	if (ended.get_number("id", 0.0) == operation && ended.get_string("end", "") == "done") return 0;
	print_findings(cli.err, ended);
	return 1;
}

// The documents with unsaved edits when the run ends: their edits end with it, said on the error
// stream (the save belongs in the same run).
void warn_unsaved(Cli &cli) {
	std::string error;
	JsonValue args = JsonValue::make_object();
	args.set("limit", io::json_number(double(editor::kQueryPageMax)));
	const JsonValue open = ask(cli, Q::Documents, args, error);
	for (const JsonValue &document : items(open, "documents"))
		if (document.get_bool("dirty", false))
			std::fprintf(cli.err, "opennova-project: %s's unsaved edits end with this run (save it in the same run)\n",
			             document.get_string("path", "").c_str());
}

// A request as the editor MCP would send it, handled as the Shell's request_json handles one, its
// answer printed as JSON; or an array of them handled in turn in the run's one session (an edit and
// the save that writes it), the array of the answers of those sent, the first that did not read or
// was not done the last. An operation a request starts (a build) runs to its end before the next.
int run_request(Cli &cli, const CliVerbRow &, const CliArgs &args) {
	JsonValue sent;
	std::string error;
	if (!io::json_parse(args.positional[1], sent, error)) {
		JsonValue answer = cli.session.handle_json(JsonValue::make_null());
		answer.set("error", json_string("The request is not JSON: " + error));
		print_json(cli.out, answer);
		return 2;
	}
	const bool many = sent.is_array();
	const std::vector<JsonValue> requests = many ? sent.array : std::vector<JsonValue>{ sent };
	JsonValue answers = JsonValue::make_array();
	int code = 0;
	for (const JsonValue &request : requests) {
		JsonValue answer = send_json(cli, request);
		code = answer_code(cli, answer);
		answers.push(std::move(answer));
		if (code != 0) break;
	}
	warn_unsaved(cli);
	print_json(cli.out, many ? answers : answers.array.front());
	return code;
}

// A query by name with its args (an object of its params; none, {}), answered as the Shell's
// query_json answers it: the answer, or {error} when it did not answer.
int run_query(Cli &cli, const CliVerbRow &, const CliArgs &args) {
	JsonValue query_args = JsonValue::make_object(), answer;
	std::string error;
	if (args.positional.size() > 2 && !io::json_parse(args.positional[2], query_args, error))
		error = "The query's args are not JSON: " + error;
	else
		answer = cli.session.query(args.positional[1], query_args, error);
	if (!error.empty()) {
		answer = JsonValue::make_object();
		answer.set("error", json_string(error));
	}
	print_json(cli.out, answer);
	return error.empty() ? 0 : 2;
}

// An import names what it takes: a source file, or the game install's files by --entry; an
// archive's members are chosen by --entry too.
bool check_import(const CliArgs &args, std::string &why) {
	const bool source = args.positional.size() > 1;
	if (!source && !args.has("--entry")) {
		why = args.has("--install") ? "choose the game's files with --entry <name> (repeat for more files)"
		                             : "import needs a source file, or the game install's files by --entry <name>";
		return false;
	}
	if (source && !args.has("--entry") && strutil::ends_with_icase(args.positional[1], ".pff")) {
		why = "choose PFF members with --entry <name> (repeat for more files)";
		return false;
	}
	return true;
}

// --- the table ----------------------------------------------------------------------------------

using CliRun = int (*)(Cli &cli, const CliVerbRow &row, const CliArgs &args);
using CliCheck = bool (*)(const CliArgs &args, std::string &why);

// A verb's row with what only the command line reads: the arguments it takes by place, its
// options, what checks its arguments beyond their shape before the project is touched, and what
// runs it once the project is open.
struct VerbRow {
	CliVerbRow cli;
	const CliPositional *positionals = nullptr;
	size_t positional_count = 0;
	const CliOption *options = nullptr;
	size_t option_count = 0;
	CliCheck check = nullptr;
	CliRun run = nullptr;
};

// A row built up column by column, as the request and query tables' are.
struct Verb {
	VerbRow row;
	template <size_t R, size_t P>
	constexpr Verb(CliVerb verb, const char *token, const char *synopsis, const K (&requests)[R],
	               const CliPositional (&positionals)[P], CliRun run, const char *doc) :
	        row() {
		row.cli.verb = verb;
		row.cli.token = token;
		row.cli.synopsis = synopsis;
		row.cli.doc = doc;
		row.cli.requests = requests;
		row.cli.request_count = R;
		row.positionals = positionals;
		row.positional_count = P;
		row.run = run;
	}
	template <size_t N>
	constexpr Verb takes(const CliOption (&options)[N]) const {
		Verb out = *this;
		out.row.options = options;
		out.row.option_count = N;
		return out;
	}
	constexpr Verb checked_by(CliCheck check) const {
		Verb out = *this;
		out.row.check = check;
		return out;
	}
	// Its open skips the import pass: its own request runs it.
	constexpr Verb opens_without_import_pass() const {
		Verb out = *this;
		out.row.cli.import_pass = false;
		return out;
	}
	constexpr Verb answers(Q query, const char *args = "") const {
		Verb out = *this;
		out.row.cli.answer = CliAnswer::Query;
		out.row.cli.query = query;
		out.row.cli.query_args = args;
		return out;
	}
	constexpr Verb answers_with(CliAnswer answer) const {
		Verb out = *this;
		out.row.cli.answer = answer;
		return out;
	}
};

constexpr K kNewRequests[] = { K::NewProject, K::ApplyProjectSettings };
constexpr K kReadRequests[] = { K::OpenProject, K::ApplyProjectSettings };
constexpr K kCreateMissingRequests[] = { K::OpenProject, K::ApplyProjectSettings, K::CreateMissing };
// A source's plan, or the game install's files', then the import (none in a dry run).
constexpr K kImportRequests[] = { K::OpenProject, K::ApplyProjectSettings, K::PlanImport, K::PreviewInstallImport,
	                              K::ImportFiles };
constexpr K kReimportRequests[] = { K::OpenProject, K::ApplyProjectSettings, K::Reimport };
constexpr K kBuildRequests[] = { K::OpenProject, K::ApplyProjectSettings, K::Build };

constexpr CliPositional kDir[] = { { "a project directory" } };
constexpr CliPositional kImportArgs[] = { { "a project directory" }, { "a source file", false } };
constexpr CliPositional kRequestArgs[] = { { "a project directory" }, { "a request as JSON" } };
constexpr CliPositional kQueryArgs[] = { { "a project directory" }, { "a query's name" }, { "its args as JSON", false } };

constexpr CliOption kNewOptions[] = { { "--title", "a text" }, { "--game", "a code" } };
constexpr CliOption kCreateMissingOptions[] = { { "--role", "a token" } };
constexpr CliOption kImportOptions[] = { { "--entry", "a file name", true },
	                                     { "--replace" },
	                                     { "--with-dependencies" },
	                                     { "--dry-run" } };
constexpr CliOption kReimportOptions[] = { { "--force" }, { "--source", "a source" } };
constexpr CliOption kBuildOptions[] = { { "--out", "a directory" } };

using V = CliVerb;

constexpr VerbRow kRows[] = {
	Verb(V::New, "new", "<dir> [--title <text>] [--game <code>]", kNewRequests, kDir, run_new,
	     "create an empty project (project.opennova + .opennova/) in <dir> and open it; --title\n"
	     "names it (else New Game), --game is its game's code (else jo)")
	        .takes(kNewOptions)
	        .answers(Q::State, "{\"sections\": [\"project\"]}")
	        .row,
	Verb(V::Status, "status", "<dir>", kReadRequests, kDir, run_status,
	     "the project's title, game, runtime, game install, files, imports, requirements and\n"
	     "problems (--json: the state query, every section)")
	        .answers(Q::State)
	        .row,
	Verb(V::Validate, "validate", "<dir>", kReadRequests, kDir, run_validate,
	     "list every finding the editor's Problems lists; exit 1 exactly when a build would be\n"
	     "refused (the build_gate query: a required file missing or wrong, an error the build\n"
	     "gates on; an error it does not gate on, the render check's, is listed and fails\n"
	     "nothing) (--json: the problems query)")
	        .answers(Q::Problems)
	        .row,
	Verb(V::CreateMissing, "create-missing", "<dir> [--role <token>]", kCreateMissingRequests, kDir,
	     run_create_missing,
	     "create every missing required file from scratch (or one, by role); exit 1 when one is\n"
	     "still missing (--json: the state query's requirements)")
	        .takes(kCreateMissingOptions)
	        .answers(Q::State, "{\"sections\": [\"requirements\"]}")
	        .row,
	Verb(V::Import, "import",
	     "<dir> [<source>] [--entry <name>]... [--replace] [--with-dependencies]\n"
	     "                               [--dry-run]",
	     kImportRequests, kImportArgs, run_import,
	     "copy files in (a loose file, PFF members, or the game install's files by --entry\n"
	     "names), the whole selection or none of it; an .o3d (a model) or an .o3a (a clip\n"
	     "set) the Blender add-on wrote converts to the .3di or the .adm and .bad;\n"
	     "--with-dependencies also copies the files they need, found beside them or in the\n"
	     "game install, 1000 files at most; an .o3d's textures come only with\n"
	     "--with-dependencies; --dry-run prints the plan and writes nothing (no import pass\n"
	     "either; --install still sets the install) (--json: the import_preview query, the\n"
	     "plan)")
	        .takes(kImportOptions)
	        .checked_by(check_import)
	        .answers(Q::ImportPreview)
	        .row,
	Verb(V::Reimport, "reimport", "<dir> [--force] [--source <path>]", kReimportRequests, kDir, run_reimport,
	     "run the import pass now; --force imports again every source (or the --source one)\n"
	     "even when nothing changed (--json: the state query's import)")
	        .takes(kReimportOptions)
	        .opens_without_import_pass()
	        .answers(Q::State, "{\"sections\": [\"import\"]}")
	        .row,
	Verb(V::Build, "build", "<dir> [--out <dir>]", kBuildRequests, kDir, run_build,
	     "pack the project into a game directory the runtime boots (default:\n"
	     "<dir>/.opennova/build/play/<build-id>) (--json: the state query's import and\n"
	     "operation)")
	        .takes(kBuildOptions)
	        .opens_without_import_pass()
	        .answers(Q::State, "{\"sections\": [\"import\", \"operation\"]}")
	        .row,
	Verb(V::Request, "request", "<dir> <json>", kReadRequests, kRequestArgs, run_request,
	     "one request as the editor MCP sends it ({\"kind\": ..., its fields}), its answer as\n"
	     "JSON (ok, served, outcome, status, view_revision), or an array of them handled in\n"
	     "turn in the one run (an edit, then the save that writes it), their answers; an\n"
	     "operation one starts runs to its end first")
	        .answers_with(CliAnswer::Request)
	        .row,
	Verb(V::Query, "query", "<dir> <name> [<json>]", kReadRequests, kQueryArgs, run_query,
	     "one query by name with its args as the editor MCP asks it (query catalog lists\n"
	     "them), its answer as JSON")
	        .answers_with(CliAnswer::NamedQuery)
	        .row,
};

static_assert(std::size(kRows) == kCliVerbCount, "every verb has exactly one row");

constexpr bool rows_in_order() {
	for (size_t i = 0; i < kCliVerbCount; ++i)
		if (kRows[i].cli.verb != static_cast<CliVerb>(i)) return false;
	return true;
}
static_assert(rows_in_order(), "the verb rows follow the enum's order");

constexpr bool same_text(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// Every verb a token of its own, its usage, a doc and what runs it; the project's directory its
// first argument, its arguments by place required before optional.
constexpr bool verbs_named() {
	for (size_t i = 0; i < kCliVerbCount; ++i) {
		const VerbRow &row = kRows[i];
		if (!row.cli.token[0] || !row.cli.synopsis[0] || !row.cli.doc[0] || !row.run || row.positional_count == 0)
			return false;
		for (size_t j = i + 1; j < kCliVerbCount; ++j)
			if (same_text(row.cli.token, kRows[j].cli.token)) return false;
		if (!same_text(row.positionals[0].words, "a project directory") || !row.positionals[0].required) return false;
		for (size_t p = 1; p < row.positional_count; ++p)
			if (row.positionals[p].required && !row.positionals[p - 1].required) return false;
	}
	return true;
}
static_assert(verbs_named(), "each verb has a token of its own, its usage, a doc and its run, <dir> first");

// Every request a verb sends is a kind of the request table (never its end), the first the one
// that opens the project (new_project for new alone), then --install's apply_project_settings.
constexpr bool requests_known() {
	for (const VerbRow &row : kRows) {
		const CliVerbRow &verb = row.cli;
		if (verb.request_count < 2 || verb.requests[1] != K::ApplyProjectSettings) return false;
		if (verb.requests[0] != (verb.verb == V::New ? K::NewProject : K::OpenProject)) return false;
		for (size_t i = 0; i < verb.request_count; ++i)
			if (verb.requests[i] >= K::kCount) return false;
		// Only new's open makes the project, which has nothing to import yet.
		if (verb.verb == V::New && !verb.import_pass) return false;
	}
	return true;
}
static_assert(requests_known(), "each verb opens the project, sets --install's game install, and names known requests");

// A verb that answers with its row's query names a query of the query table (never its end); one
// that answers otherwise names none and no args.
constexpr bool answers_known() {
	for (const VerbRow &row : kRows) {
		const CliVerbRow &verb = row.cli;
		if ((verb.answer == CliAnswer::Query) != (verb.query < Q::kCount)) return false;
		if (verb.answer != CliAnswer::Query && (verb.query != Q::kCount || verb.query_args[0])) return false;
	}
	return true;
}
static_assert(answers_known(), "each verb answers with a known query, its arguments' query or its request's answer");

// Every option a name of its own among the verb's and the common ones.
constexpr bool options_named() {
	for (const VerbRow &row : kRows) {
		for (size_t i = 0; i < row.option_count; ++i) {
			for (size_t j = i + 1; j < row.option_count; ++j)
				if (same_text(row.options[i].name, row.options[j].name)) return false;
			for (const CliOption &common : kCommonOptions)
				if (same_text(row.options[i].name, common.name)) return false;
		}
	}
	return true;
}
static_assert(options_named(), "each option of a verb has a name of its own");

const VerbRow &verb_row(CliVerb verb) {
	const size_t index = static_cast<size_t>(verb);
	return kRows[index < kCliVerbCount ? index : static_cast<size_t>(V::Query)];
}

const CliOption *option_of(const VerbRow &row, const std::string &name) {
	for (size_t i = 0; i < row.option_count; ++i)
		if (name == row.options[i].name) return &row.options[i];
	for (const CliOption &common : kCommonOptions)
		if (name == common.name) return &common;
	return nullptr;
}

// The verb's arguments read against its row: its options (a value never another option), each once
// but --entry, and its arguments by place, as many as it takes and those it needs. False with
// `why` for anything else.
bool parse_args(const VerbRow &row, int argc, const char *const *argv, CliArgs &out, std::string &why) {
	const std::string verb = row.cli.token;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg.rfind("--", 0) != 0) {
			if (out.positional.size() >= row.positional_count) {
				why = verb + " takes no more arguments: " + arg;
				return false;
			}
			out.positional.push_back(arg);
			continue;
		}
		const CliOption *option = option_of(row, arg);
		if (!option) {
			why = "unknown option " + arg + " for " + verb;
			return false;
		}
		if (out.has(option->name) && !option->repeats) {
			why = arg + " is given twice";
			return false;
		}
		if (!option->value) {
			out.options.emplace_back(arg, std::string());
			continue;
		}
		if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0) {
			why = arg + " needs " + option->value;
			return false;
		}
		out.options.emplace_back(arg, argv[++i]);
	}
	for (size_t p = out.positional.size(); p < row.positional_count; ++p) {
		if (row.positionals[p].required) {
			why = verb + " needs " + row.positionals[p].words;
			return false;
		}
	}
	return !row.check || row.check(out, why);
}

// A doc as the usage writes it: its lines after the verb's column.
void print_doc(std::FILE *to, const char *token, const char *doc) {
	std::fprintf(to, "  %-15s ", token);
	for (const char *c = doc; *c; ++c) {
		std::fputc(*c, to);
		if (*c == '\n') std::fputs("                  ", to);
	}
	std::fputc('\n', to);
}

int usage(std::FILE *err, const char *why) {
	if (why != nullptr) std::fprintf(err, "opennova-project: %s\n", why);
	std::fputs("opennova-project (pre-1.0, experimental): the OpenNova Editor's project session on the command line\n",
	           err);
	for (size_t i = 0; i < kCliVerbCount; ++i)
		std::fprintf(err, "%sopennova-project %s %s\n", i == 0 ? "usage: " : "       ", kRows[i].cli.token,
		             kRows[i].cli.synopsis);
	for (const VerbRow &row : kRows) print_doc(err, row.cli.token, row.cli.doc);
	std::fputs("  every verb also takes --install <dir>, the project's game install, set as the editor's\n"
	           "  project settings set it (in its .opennova/local.json) before the verb's own requests,\n"
	           "  and --json, its answer as JSON (as the editor MCP's editor_query gives it)\n"
	           "  every verb opens the project as the editor does: the sources that changed are imported\n"
	           "  first (a build's and a reimport's own pass is the one they run; a dry run runs none)\n",
	           err);
	return 2;
}

} // namespace

const CliVerbRow &cli_verb_row(CliVerb verb) {
	return verb_row(verb).cli;
}

bool cli_verb_from_token(const std::string &token, CliVerb &out) {
	for (const VerbRow &row : kRows) {
		if (token == row.cli.token) {
			out = row.cli.verb;
			return true;
		}
	}
	return false;
}

int run_project_command(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	if (argc < 1) return usage(err, nullptr);
	const std::string token = argv[0];
	if (token == "-h" || token == "--help" || token == "help") return usage(err, nullptr);
	CliVerb verb = CliVerb::kCount;
	if (!cli_verb_from_token(token, verb)) return usage(err, ("unknown verb " + token).c_str());
	const VerbRow &row = verb_row(verb);
	CliArgs args;
	std::string why;
	if (!parse_args(row, argc, argv, args, why)) return usage(err, why.c_str());
	Cli cli(out, err, args.has("--json"));
	if (!set_up(cli, row.cli, args)) return 2;
	return row.run(cli, row.cli, args);
}

} // namespace opennova::project_cli
