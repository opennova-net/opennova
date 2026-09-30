// S13 A3: long operations everywhere (ADR 0046). The walks the session steps (the scan,
// ProjectScan; the import pass, ImportPass; a scan's targeted update, AssetScan::update), the
// validation a file at a time (ProjectValidation) and a rename's commit a file at a time
// (RenameTransaction), each stepped at any budget coming to what it comes to in one call; and the
// session's operations over them: D4's four fixture projects opened under a 64 KB budget a step at
// a time, the progress only rising, finished once, and coming to the view an Open run to its end
// makes; an Open cancelled between two steps leaving nothing; an edit refused and a query answered
// while one runs; a Save scanning and validating its one file; an import that finds its plan stale
// refusing without writing, stepped; a Rescan, an import and a rename stepped coming to the view
// they come to run to their end. Retail leg (OPENNOVA_JO_DIR): a project of the JO install's files
// opened a step at a time at the editor's budget (its polls, its steps, its longest poll and its
// wall time), and the base layer a dependency mount of the install would build.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_import.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/assets/project_scan.h>
#include <editor/documents/document_types.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/graph_layer.h>
#include <editor/graph/project_validation.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/import_pass.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/preview/menu_render_check.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/editor_preferences.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_operation.h>
#include <editor/session/view/session_view.h>

#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/fixture_projects.h"
#include "editor/png_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using fixture_projects::Files;
using fixture_projects::row_of;
namespace fs = std::filesystem;
using editor_test::NoProcess;
using editor_test::gradient_png;

namespace {

constexpr uint64_t k64K = 64 * 1024;

// A project of `files` on disk, made as New makes one: its root, "" when it could not be made.
std::string make_project(const std::string &root, const std::string &title, const Files &files) {
	ProjectDocument document;
	Diagnostic error;
	if (!create_project(root, title, "jo", document, error)) return std::string();
	for (const auto &[path, text] : files)
		if (!editor_test::write_text(root + "/" + path, text)) return std::string();
	return root;
}

ProjectDocument document_of(const std::string &root) {
	ProjectDocument document;
	Diagnostic error;
	load_project_document(ProjectPaths::for_root(root).project_file, document, error);
	return document;
}

// A file made an import source: its record, the importer's defaults (import_assets writes one).
bool mark_for_import(const std::string &source) {
	const Importer *importer = importer_for(source);
	if (!importer) return false;
	ImportSidecar sidecar;
	sidecar.importer = importer->id;
	sidecar.version = importer->version;
	sidecar.options = importer->default_options;
	Diagnostic error;
	return save_import_sidecar(source + kImportSidecarSuffix, sidecar, error);
}

// An entry as one line, every member of it (its last write only when the two compared are the same
// files: twin projects write theirs at other times).
std::string entry_line(const AssetEntry &e, bool times) {
	return e.relative_path + "|" + e.logical_name + "|" + e.key + "|" + asset_kind_token(e.kind) + "|" +
	       std::to_string(e.size_bytes) + "|" + (times ? std::to_string(e.modified_ticks) : std::string()) + "|" +
	       e.imported_from;
}

std::vector<std::string> scan_lines(const AssetScan &scan, bool times = true) {
	std::vector<std::string> lines;
	for (const AssetEntry &e : scan.entries) lines.push_back(entry_line(e, times));
	lines.push_back("-- findings");
	for (const Diagnostic &d : scan.diagnostics) lines.push_back(row_of(d));
	return lines;
}

// What a project's session holds of it, member for member: the files the scan lists and their
// findings, the imported sources, the requirements' rows and findings, the graph's counts, the
// Problems rows.
std::vector<std::string> view_lines(const SessionView &v, bool times = true) {
	std::vector<std::string> lines = scan_lines(*v.project.scan, times);
	lines.push_back("-- imports");
	for (const ImportedSource &s : *v.project.imports)
		lines.push_back(s.source + "|" + s.sidecar + "|" + s.importer + "|" + s.output_dir + "|" +
		                std::to_string(s.outputs.size()) + "|" + (s.ok ? "ok" : "failed"));
	lines.push_back("-- requirements");
	for (const RequirementRow &row : v.project.requirements->rows)
		lines.push_back(row.role + "|" + row.name + "|" + std::to_string(int(row.state)) + "|" + row.asset_path + "|" +
		                asset_kind_token(row.found_kind));
	for (const Diagnostic &d : v.project.requirements->diagnostics) lines.push_back(row_of(d));
	const AssetGraph &graph = *v.findings.graph;
	lines.push_back("-- graph " + std::to_string(graph.index().slot_count()) + " files, " +
	                std::to_string(graph.edge_count()) + " edges, " + std::to_string(graph.symbol_count()) +
	                " symbols, " + std::to_string(graph.missing_count()) + " missing");
	lines.push_back("-- problems");
	for (const Diagnostic &d : v.findings.diagnostics) lines.push_back(row_of(d));
	return lines;
}

// Two lists the same, else the first difference printed.
bool same_lines(const char *what, const std::vector<std::string> &a, const std::vector<std::string> &b) {
	if (a == b) return true;
	std::printf("%s: %zu and %zu lines\n", what, a.size(), b.size());
	for (size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
		const std::string x = i < a.size() ? a[i] : std::string("(none)");
		const std::string y = i < b.size() ? b[i] : std::string("(none)");
		if (x != y) {
			std::printf("  %zu: %s\n  %zu: %s\n", i, x.c_str(), i, y.c_str());
			break;
		}
	}
	return false;
}

// Every file under `root` with its bytes: what a step that must write nothing leaves as it was.
std::vector<std::pair<std::string, std::string>> snapshot(const std::string &root) {
	std::vector<std::pair<std::string, std::string>> files;
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (ec) break;
		if (!it->is_regular_file(ec)) continue;
		std::string text, error;
		read_file_text(it->path().generic_string(), text, error);
		files.push_back({fs::relative(it->path(), root, ec).generic_string(), text});
	}
	std::sort(files.begin(), files.end());
	return files;
}

// The four fixture projects of S13 D4 (fixture_projects.h), each by its name.
struct Fixture {
	const char *name;
	Files (*files)();
};
const Fixture kFixtures[] = {
	{"fixtures", fixture_projects::fixture_files},
	{"styles", fixture_projects::style_files},
	{"items", fixture_projects::item_files},
	{"open", fixture_projects::style_and_item_files},
};

// The PNG of a project made an import source, a second PNG left a texture, and a record whose
// source is gone: the scan's import cases.
Files import_files() {
	const std::vector<uint8_t> png = gradient_png(8, 8);
	return {{"art/logo.png", std::string(png.begin(), png.end())},
	        {"art/plain.png", std::string(png.begin(), png.end())},
	        {"art/gone.png.import", "{}"},
	        {"menus/menu_style.mns", "COL FF00FF00\r\n"}};
}

// A session over its own preferences.
struct Session {
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	const SessionView &view() const { return session.view(); }
};

// A project opened in `session` a poll at a time at `budget` (ms 0: one step of each a poll): the
// polls the Open took, its progress checked as it went (only rising, never past its total), and the
// polls its validation took after it.
struct Stepped {
	size_t open_polls = 0;
	size_t validation_polls = 0;
	bool progress_rose = true;
	bool finished_once = false;
};
Stepped open_stepped(ProjectSession &session, const std::string &root, const PollBudget &budget) {
	Stepped out;
	const SessionView &v = session.view();
	session.set_poll_budget(budget);
	session.handle(request::open_project(root));
	const uint64_t id = session.outcome().operation;
	uint64_t done = 0, total = 0;
	while (v.activity.operation.running() && out.open_polls < 100000) {
		if (v.activity.operation.id != id || v.project.open) out.progress_rose = false; // nothing of it before its finish
		if (v.activity.operation.done < done || v.activity.operation.total < total ||
		    v.activity.operation.done > v.activity.operation.total)
			out.progress_rose = false;
		done = v.activity.operation.done;
		total = v.activity.operation.total;
		session.poll();
		++out.open_polls;
	}
	out.finished_once = v.project.open && v.activity.last_operation.id == id &&
	                    v.activity.last_operation.kind == OperationKind::Open &&
	                    v.activity.last_operation.end == OperationEnd::Done;
	uint64_t validated = 0;
	while (v.activity.validation.running && out.validation_polls < 100000) {
		if (v.activity.validation.done < validated) out.progress_rose = false;
		validated = v.activity.validation.done;
		session.poll();
		++out.validation_polls;
	}
	return out;
}

} // namespace

// The scan a step at a time: at a budget of one byte (a file a step), of 64 KB and whole, over the
// four fixture projects and one with import sources (a PNG with its record and its output, one
// without, a record whose source is gone), the scan equals scan_project_assets' member for member;
// at one byte the steps are at least the files listed and visited; the counts only rise.
static int test_project_scan_steps() {
	editor_test::TempProjectDir dir("opennova_long_ops_scan");
	std::vector<std::string> roots;
	for (const Fixture &fixture : kFixtures)
		roots.push_back(make_project(dir.file(fixture.name), fixture.name, fixture.files()));
	roots.push_back(make_project(dir.file("imports"), "Imports", import_files()));
	TEST_EXPECT(mark_for_import(roots.back() + "/art/logo.png"));
	run_imports(ProjectPaths::for_root(roots.back()), document_of(roots.back()));
	for (const std::string &root : roots) {
		TEST_EXPECT(!root.empty());
		const ProjectPaths paths = ProjectPaths::for_root(root);
		const ProjectDocument document = document_of(root);
		const AssetScan whole = scan_project_assets(paths, document);
		for (const uint64_t budget : {uint64_t(1), k64K, kWholeWalkStep}) {
			ProjectScan walk(paths, document);
			size_t steps = 0, listed = 0, visited = 0;
			bool rose = true;
			while (!walk.step(budget)) {
				++steps;
				rose = rose && walk.files_listed() >= listed && walk.files_visited() >= visited;
				listed = walk.files_listed();
				visited = walk.files_visited();
			}
			++steps;
			const AssetScan stepped = walk.take();
			TEST_EXPECT(rose && walk.files_visited() == walk.files_listed());
			TEST_EXPECT(same_lines(root.c_str(), scan_lines(stepped), scan_lines(whole)));
			if (budget == 1) TEST_EXPECT(steps >= 2 * walk.files_listed());
			if (budget == kWholeWalkStep) TEST_EXPECT(steps == 1);
		}
	}
	const AssetScan imports = scan_project_assets(ProjectPaths::for_root(roots.back()), document_of(roots.back()));
	const AssetEntry *logo = imports.find("logo.png"), *pcx = imports.find("logo.pcx"), *plain = imports.find("plain.png");
	TEST_EXPECT(logo && logo->kind == AssetKind::ImageSource && pcx && pcx->imported_from == "art/logo.png" && plain &&
	            plain->kind == AssetKind::Texture);
	TEST_EXPECT(std::any_of(imports.diagnostics.begin(), imports.diagnostics.end(),
	                        [](const Diagnostic &d) { return d.code == "import.orphan_record"; }));
	return 0;
}

// The import pass a step at a time: over twin projects whose sources import (one stale, one new,
// one whose record does not read), the pass at a budget of one byte and the pass in one call come
// to the same sources, findings and outputs; at one byte a source a step; the cache written once,
// at the end, and a pass dropped before its end leaves it as it was.
static int test_import_pass_steps() {
	editor_test::TempProjectDir dir("opennova_long_ops_pass");
	std::vector<std::string> roots;
	for (const char *name : {"stepped", "whole"}) {
		const std::vector<uint8_t> png = gradient_png(8, 8);
		const std::vector<uint8_t> other = gradient_png(4, 4, 3);
		Files files{{"art/logo.png", std::string(png.begin(), png.end())},
		            {"art/b/icon.png", std::string(other.begin(), other.end())},
		            {"art/typo.png", std::string(png.begin(), png.end())}};
		roots.push_back(make_project(dir.file(name), name, files));
		TEST_EXPECT(mark_for_import(roots.back() + "/art/logo.png") && mark_for_import(roots.back() + "/art/b/icon.png"));
		TEST_EXPECT(editor_test::write_text(roots.back() + "/art/typo.png.import", "{ not json"));
	}
	const ProjectPaths stepped_paths = ProjectPaths::for_root(roots[0]);
	const ProjectDocument document = document_of(roots[0]);
	// Dropped before its end: the sources it reached imported, the cache as it was (none).
	{
		ImportPass pass(stepped_paths, document);
		while (pass.sources_done() == 0) pass.step(1);
		TEST_EXPECT(!pass.done() && !fs::exists(stepped_paths.import_cache_file));
	}
	ImportPass pass(stepped_paths, document);
	size_t steps = 0;
	while (!pass.step(1)) ++steps;
	const ImportRunResult stepped = pass.take();
	const ImportRunResult whole = run_imports(ProjectPaths::for_root(roots[1]), document_of(roots[1]));
	const auto lines = [](const ImportRunResult &run) {
		std::vector<std::string> out;
		for (const ImportedSource &s : run.sources) {
			std::string outputs;
			for (const std::string &o : s.outputs) outputs += o + ",";
			out.push_back(s.source + "|" + s.importer + "|" + outputs + "|" + (s.ok ? "ok" : "failed"));
		}
		for (const Diagnostic &d : run.diagnostics) out.push_back(row_of(d));
		return out;
	};
	TEST_EXPECT(same_lines("the pass", lines(stepped), lines(whole)));
	TEST_EXPECT(stepped.sources.size() == 3 && steps >= 3 && fs::exists(stepped_paths.import_cache_file));
	// The sources in the order of their paths, whatever the folder lists first.
	TEST_EXPECT(stepped.sources[0].source == "art/b/icon.png" && stepped.sources[1].source == "art/logo.png" &&
	            stepped.sources[2].source == "art/typo.png" && !stepped.sources[2].ok);
	std::string a, b, error;
	TEST_EXPECT(read_file_text(roots[0] + "/" + stepped.sources[1].outputs[0], a, error) &&
	            read_file_text(roots[1] + "/" + whole.sources[1].outputs[0], b, error) && a == b && !a.empty());
	return 0;
}

// A scan's targeted update (AssetScan::update): after each of a script of changes on disk (a file
// edited, added, removed, renamed, its case changed; a PNG given a record and its record taken away;
// a record's source gone; the files a project's cache and the export directory hold, which the walk
// never reaches), the scan updated with the paths changed equals a scan made afresh, member for
// member, and reads those files alone (their count).
static int test_scan_update() {
	editor_test::TempProjectDir dir("opennova_long_ops_update");
	const std::string root = make_project(dir.file("project"), "Update", fixture_projects::style_and_item_files());
	TEST_EXPECT(!root.empty());
	TEST_EXPECT(editor_test::write_bytes(root + "/art/logo.png", gradient_png(8, 8)));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const ProjectDocument document = document_of(root);
	AssetScan scan = scan_project_assets(paths, document);
	scan.set_import_findings({make_diagnostic(DiagnosticSeverity::Error, "import.read", "kept by an update", "art/x.png")});
	struct Change {
		const char *what;
		std::vector<std::string> changed;
		size_t read;
		std::function<bool()> apply;
	};
	const std::vector<Change> script = {
		{"an item table edited", {"defs/items.def"}, 1,
		 [&] { return editor_test::write_text(root + "/defs/items.def", fixture_projects::item("Zulu", 900)); }},
		{"a file added", {"menus/new.mnu"}, 1,
		 [&] { return editor_test::write_text(root + "/menus/new.mnu", "<SCREEN>\r\n<NAME>NEW</NAME>\r\n</SCREEN>\r\n"); }},
		{"a second file of a name", {"other/new.mnu"}, 1,
		 [&] { return editor_test::write_text(root + "/other/new.mnu", "<SCREEN>\r\n<NAME>TWO</NAME>\r\n</SCREEN>\r\n"); }},
		{"a file removed", {"other/new.mnu"}, 0, [&] { return fs::remove(root + "/other/new.mnu"); }},
		{"a file renamed", {"menus/new.mnu", "menus/renamed.mnu"}, 1,
		 [&] {
			 std::error_code ec;
			 fs::rename(root + "/menus/new.mnu", root + "/menus/renamed.mnu", ec);
			 return !ec;
		 }},
		{"its case changed", {"menus/renamed.mnu", "menus/Renamed.mnu"}, 1,
		 [&] {
			 std::error_code ec;
			 fs::rename(root + "/menus/renamed.mnu", root + "/menus/Renamed.mnu", ec);
			 return !ec;
		 }},
		{"a PNG given a record", {"art/logo.png.import"}, 2, [&] { return mark_for_import(root + "/art/logo.png"); }},
		{"its output made", {"art/logo.png.import"}, 2,
		 [&] {
			 run_imports(paths, document);
			 return true;
		 }},
		{"the PNG gone, its record left", {"art/logo.png"}, 1, [&] { return fs::remove(root + "/art/logo.png"); }},
		{"the record gone", {"art/logo.png.import"}, 0, [&] { return fs::remove(root + "/art/logo.png.import"); }},
		{"a file under the cache", {".opennova/stray.def"}, 0,
		 [&] { return editor_test::write_text(root + "/.opennova/stray.def", "x"); }},
		{"the project file", {"project.opennova"}, 0, [&] { return true; }},
		{"a name too long for an archive", {"textures/averyveryverylongname.pcx"}, 1,
		 [&] { return editor_test::write_text(root + "/textures/averyveryverylongname.pcx", "pcx"); }},
		{"a kind the game does not use", {"notes/readme.docx"}, 1,
		 [&] { return editor_test::write_text(root + "/notes/readme.docx", "docx"); }},
	};
	for (const Change &change : script) {
		TEST_EXPECT(change.apply());
		const size_t read = scan.update(paths, document, change.changed);
		AssetScan fresh = scan_project_assets(paths, document);
		fresh.set_import_findings(scan.import_findings());
		const bool same = same_lines(change.what, scan_lines(scan), scan_lines(fresh));
		if (!same || read != change.read) std::printf("%s: %zu files read, %zu expected\n", change.what, read, change.read);
		TEST_EXPECT(same && read == change.read);
	}
	TEST_EXPECT(std::any_of(scan.diagnostics.begin(), scan.diagnostics.end(),
	                        [](const Diagnostic &d) { return d.code == "import.read" && d.message == "kept by an update"; }));
	return 0;
}

// The validation a file at a time (ProjectValidation): over the four fixture projects, stepped at a
// budget of one byte (a file a step), it makes the rows refresh_project and project_rows make in one
// call, member for member, and the same stats; at one byte a step for the graph's update and one
// for each file the document types open, and the last one ends it.
static int test_validation_steps() {
	editor_test::TempProjectDir dir("opennova_long_ops_validation");
	for (const Fixture &fixture : kFixtures) {
		const std::string root = make_project(dir.file(fixture.name), fixture.name, fixture.files());
		TEST_EXPECT(!root.empty());
		const ProjectPaths paths = ProjectPaths::for_root(root);
		const ProjectDocument document = document_of(root);
		const AssetScan scan = scan_project_assets(paths, document);
		const std::vector<std::shared_ptr<const DocumentBase>> open;
		const ValidationInput input{paths, document, scan, open};
		AssetGraph whole_graph, stepped_graph;
		ValidationCache whole_cache, stepped_cache;
		TEST_EXPECT(refresh_project(input, whole_graph, whole_cache));
		ProjectValidation validation(stepped_graph, stepped_cache);
		size_t steps = 1;
		while (!validation.step(input, 1)) ++steps;
		TEST_EXPECT(validation.moved() && validation.files_done() == validation_files(scan).size());
		TEST_EXPECT(steps == validation_files(scan).size() + 2); // the graph, each file, the end
		std::vector<std::string> a, b;
		for (const Diagnostic &d : project_rows(input, whole_graph, whole_cache)) a.push_back(row_of(d));
		for (const Diagnostic &d : project_rows(input, stepped_graph, stepped_cache)) b.push_back(row_of(d));
		TEST_EXPECT(same_lines(fixture.name, b, a) && !a.empty());
		TEST_EXPECT(stepped_cache.stats().files_validated == whole_cache.stats().files_validated &&
		            stepped_cache.stats().files_loaded == whole_cache.stats().files_loaded);
		// Again with nothing changed: nothing made again, the graph unmoved.
		ProjectValidation again(stepped_graph, stepped_cache);
		while (!again.step(input, k64K)) {
		}
		TEST_EXPECT(!again.moved() && !again.graph_moved() && stepped_cache.stats().files_validated == 0);
	}
	return 0;
}

// A rename's commit a file at a time (RenameTransaction): a weapon's name renamed where it is
// defined and where an item names it stages one file a step, nothing written until the commit, the
// one step that writes; dropped before it, every file is as it was; run to its end it writes what
// apply_symbol_rename writes on a twin project.
static int test_rename_transaction_steps() {
	editor_test::TempProjectDir dir("opennova_long_ops_rename");
	const Files files{{"defs/weapon.def", "weapon \"GUN_A\"\nend\nweapon \"GUN_C\"\nend\n"},
	                  {"defs/items.def", "begin \"Carrier\"\nid 100300\ntype vehicle\nprimary_weapon GUN_A\nend\n"}};
	std::vector<std::string> roots;
	for (const char *name : {"stepped", "whole"}) roots.push_back(make_project(dir.file(name), name, files));
	const auto plan_of = [](const std::string &root, AssetScan &scan, AssetGraph &graph) {
		const ProjectPaths paths = ProjectPaths::for_root(root);
		const ProjectDocument document = document_of(root);
		scan = scan_project_assets(paths, document);
		graph.update(paths, document, scan, {});
		const std::vector<const GraphSymbol *> gun = graph.symbols_named(ReferenceKind::Weapon, "GUN_A");
		return gun.empty() ? SymbolRenamePlan() : plan_symbol_rename_project(scan, graph, *gun.front(), "GUN_B");
	};
	AssetScan scan, twin_scan;
	AssetGraph graph, twin_graph;
	const SymbolRenamePlan plan = plan_of(roots[0], scan, graph);
	TEST_EXPECT(plan.ok() && plan.sites.size() == 2);
	const ProjectPaths paths = ProjectPaths::for_root(roots[0]);
	const ProjectDocument document = document_of(roots[0]);
	const auto before = snapshot(roots[0]);
	{
		RenameTransaction dropped(paths, document, scan, graph, plan);
		TEST_EXPECT(!dropped.step() && dropped.files_staged() == 1 && !dropped.committed());
	}
	TEST_EXPECT(snapshot(roots[0]) == before);
	RenameTransaction transaction(paths, document, scan, graph, plan);
	size_t staged = 0;
	while (!transaction.step()) {
		++staged;
		TEST_EXPECT(snapshot(roots[0]) == before && !transaction.committed()); // staging writes nothing
	}
	TEST_EXPECT(staged == transaction.files_total() && staged == 2 && transaction.committed() && transaction.ok() &&
	            transaction.findings().empty());
	TEST_EXPECT(transaction.touched() == std::vector<std::string>({"defs/items.def", "defs/weapon.def"}));
	std::vector<Diagnostic> findings;
	const SymbolRenamePlan twin = plan_of(roots[1], twin_scan, twin_graph);
	TEST_EXPECT(apply_symbol_rename(ProjectPaths::for_root(roots[1]), document_of(roots[1]), twin_scan, twin_graph, twin,
	                                findings));
	std::vector<std::pair<std::string, std::string>> stepped = snapshot(roots[0]), whole = snapshot(roots[1]);
	const auto drop_project_file = [](std::vector<std::pair<std::string, std::string>> &files) {
		files.erase(std::remove_if(files.begin(), files.end(), [](const auto &f) { return f.first == "project.opennova"; }),
		            files.end());
	};
	drop_project_file(stepped);
	drop_project_file(whole);
	TEST_EXPECT(stepped == whole && stepped != before);
	for (const auto &[path, text] : stepped)
		if (path == "defs/weapon.def" || path == "defs/items.def")
			TEST_EXPECT(text.find("GUN_B") != std::string::npos && text.find("GUN_A") == std::string::npos);
	return 0;
}

// Opening D4's four fixture projects under a 64 KB budget (ms 0: one step a poll): the Open takes at
// least as many polls as its walks need at 16 files a poll (a file 4 KB of the budget), its progress
// only rises and never passes its total, nothing of the project is in the view until it finishes,
// and it finishes once; then the validation the polls step, a file at a time. What the view holds
// after is what an Open run to its end makes (another session over the same files), member for
// member.
static int test_open_stepped() {
	editor_test::TempProjectDir dir("opennova_long_ops_open");
	for (const Fixture &fixture : kFixtures) {
		const std::string root = make_project(dir.file(fixture.name), fixture.name, fixture.files());
		TEST_EXPECT(!root.empty());
		const size_t files = scan_project_assets(ProjectPaths::for_root(root), document_of(root)).entries.size();
		Session stepped;
		const Stepped steps = open_stepped(stepped.session, root, {0, k64K});
		std::printf("%s: %zu files, opened in %zu polls, validated in %zu more\n", fixture.name, files, steps.open_polls,
		            steps.validation_polls);
		TEST_EXPECT(steps.progress_rose && steps.finished_once);
		// The install's names, then the import pass's listing and the scan's listing and visits, 16
		// entries a poll at most (every file is listed twice and visited once).
		TEST_EXPECT(steps.open_polls >= 1 + (3 * files) / 16);
		TEST_EXPECT(steps.validation_polls >= 1);
		Session whole;
		whole.session.handle(request::open_project(root));
		whole.session.run_operations();
		TEST_EXPECT(same_lines(fixture.name, view_lines(stepped.view()), view_lines(whole.view())));
	}
	return 0;
}

// An Open cancelled at a step boundary: the session is left with no project open and nothing of it
// in the view (no files, no requirements, no Problems rows, no recent project), the operation's
// outcome cancelled; while it runs an edit and a save are refused (operation.busy), a selection and a
// query go on, and a Close cancels it as a cancel does.
static int test_open_cancelled() {
	editor_test::TempProjectDir dir("opennova_long_ops_cancel");
	const std::string root = make_project(dir.file("project"), "Cancel", fixture_projects::fixture_files());
	TEST_EXPECT(!root.empty());
	Session s;
	const SessionView &v = s.view();
	s.session.set_poll_budget({0, k64K});
	s.session.handle(request::open_project(root));
	const uint64_t id = s.session.outcome().operation;
	TEST_EXPECT(s.session.outcome().done() && id != 0 && v.activity.operation.kind == OperationKind::Open &&
	            v.activity.operation.cancellable && v.activity.operation.writes == (kHoldsAll | HoldsSlot));
	// Polled into its scan: a step boundary with files visited.
	for (int i = 0; i < 1000 && v.activity.operation.running() && v.activity.operation.done == 0; ++i) s.session.poll();
	s.session.poll();
	TEST_EXPECT(v.activity.operation.running() && v.activity.operation.done > 0 &&
	            v.activity.operation.done < v.activity.operation.total && !v.project.open);
	// While it runs: an edit and a save wait, a selection and a query go on.
	s.session.handle(request::edit_record("defs/items.def", Edit()));
	TEST_EXPECT(!s.session.outcome().done() && s.session.outcome().findings.size() == 1 &&
	            s.session.outcome().findings[0].code == "operation.busy");
	s.session.handle(request::save_all());
	TEST_EXPECT(!s.session.outcome().done() && s.session.outcome().findings[0].code == "operation.busy");
	s.session.handle(request::select_record(std::string(), NodeAddress()));
	TEST_EXPECT(s.session.outcome().done());
	std::string error;
	const opennova::io::JsonValue state = s.session.query("operation", opennova::io::JsonValue::make_null(), error);
	TEST_EXPECT(error.empty() && state.get("operation")->get_string("kind", "") == "open" &&
	            state.get("operation")->get_bool("running", false));
	s.session.handle(request::of(EditorRequestKind::CancelOperation));
	TEST_EXPECT(s.session.outcome().done() && !v.activity.operation.running() && v.activity.last_operation.id == id &&
	            v.activity.last_operation.end == OperationEnd::Cancelled);
	TEST_EXPECT(!v.project.open && v.project.root.empty() && v.project.scan->entries.empty() &&
	            v.project.requirements->rows.empty() && v.documents.open.empty() && !v.activity.validation.running);
	for (const Diagnostic &d : v.findings.diagnostics) TEST_EXPECT(d.code == "operation.busy");
	TEST_EXPECT(v.project.recent_projects.empty());
	for (int i = 0; i < 3; ++i) s.session.poll(); // nothing finishes later
	TEST_EXPECT(!v.project.open && !v.activity.operation.running() && v.activity.last_operation.id == id);
	// A Close during an Open cancels it as its commit does: the session stays closed.
	s.session.handle(request::open_project(root));
	s.session.poll();
	s.session.handle(request::close_project());
	TEST_EXPECT(s.session.outcome().done() && !v.project.open && !v.activity.operation.running() &&
	            v.activity.last_operation.end == OperationEnd::Cancelled);
	// Opened to its end, it is the open project and a recent one.
	s.session.handle(request::open_project(root));
	s.session.run_operations();
	TEST_EXPECT(v.project.open && v.project.root == ProjectPaths::for_root(root).root && !v.project.scan->entries.empty() &&
	            v.project.recent_projects.size() == 1);
	return 0;
}

// A Save of one file scans that file alone (AssetScan::update: one file read) and validates it
// alone (one file's own findings made); SaveAll of two, two; a file created, one; and after each the
// scan equals one made afresh.
static int test_save_scans_one_file() {
	editor_test::TempProjectDir dir("opennova_long_ops_save");
	const std::string root = make_project(dir.file("project"), "Save", fixture_projects::fixture_files());
	TEST_EXPECT(!root.empty());
	Session s;
	const SessionView &v = s.view();
	s.session.handle(request::open_project(root));
	s.session.run_operations();
	TEST_EXPECT(v.project.open && s.session.files_scanned() > 10);
	const auto fresh_equal = [&](const char *what) {
		AssetScan fresh = scan_project_assets(ProjectPaths::for_root(root), *v.project.document);
		fresh.set_import_findings(v.project.scan->import_findings());
		return same_lines(what, scan_lines(*v.project.scan), scan_lines(fresh));
	};
	const auto edit_row = [&](const std::string &path, size_t row, const std::string &field, const Value &value) {
		s.session.handle(request::open_document(path));
		Document *document = s.session.document_for(path);
		if (!document || document->rows().size() <= row) return false;
		EditorRequest edit = request::edit_record(path, Edit());
		edit.edits[0].address = {document->rows()[row]->id, document->rows()[row]->kind, 0};
		edit.edits[0].field = field;
		edit.edits[0].value = value;
		s.session.handle(edit);
		return s.session.outcome().done() && document->dirty();
	};
	TEST_EXPECT(edit_row("defs/items.def", 0, "hp", int64_t(77)));
	s.session.handle(request::save("defs/items.def"));
	TEST_EXPECT(s.session.outcome().done() && s.session.files_scanned() == 1);
	TEST_EXPECT(s.session.validation_stats().files_validated == 1 && s.session.validation_stats().files_loaded == 0);
	TEST_EXPECT(fresh_equal("a Save"));
	// Two saved together: two read, two validated.
	TEST_EXPECT(edit_row("defs/items.def", 0, "hp", int64_t(78)));
	// The stylesheet's DEF_TEXT_FG (its fourth row: two comment lines come first).
	TEST_EXPECT(edit_row("menus/menu_style.mns", 3, "value", std::string("FF00FF00")));
	s.session.handle(request::save_all());
	TEST_EXPECT(s.session.outcome().done() && s.session.files_scanned() == 2 &&
	            s.session.validation_stats().files_validated == 2);
	TEST_EXPECT(fresh_equal("a SaveAll"));
	// A file created: that one read.
	s.session.handle(request::create_file("fresh.mns", std::string()));
	TEST_EXPECT(s.session.outcome().done() && s.session.files_scanned() == 1 && v.project.scan->find("fresh.mns"));
	TEST_EXPECT(fresh_equal("a create"));
	return 0;
}

// An import whose plan went stale (a file it needs changed after the preview): stepped a poll at a
// time at a small budget, it plans again, finds the plan not the one shown (same_import) and ends
// refused (import.changed, what the operation came to), nothing written at any step, the dialog
// showing the new plan, one ImportPlanned event flagged.
static int test_stale_import_refused() {
	editor_test::TempProjectDir dir("opennova_long_ops_stale");
	const std::string root = make_project(dir.file("project"), "Stale", {});
	const std::string art = dir.file("art");
	const auto menu = [](const std::string &texture) {
		return "<SCREEN>\r\n<NAME>A</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"W\">\r\n<APPEARANCE STATE=\"DEFAULT\" "
		       "TYPE=\"IMAGE\">" + texture + "</APPEARANCE>\r\n</WINDOW>\r\n</SCREEN>\r\n";
	};
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", menu("one.tga")) && editor_test::write_text(art + "/one.tga", "tga") &&
	            editor_test::write_text(art + "/two.tga", "tga"));
	Session s;
	const SessionView &v = s.view();
	s.session.handle(request::open_project(root));
	s.session.run_operations();
	EditorRequest preview = request::of(EditorRequestKind::PreviewImport);
	preview.paths = {art + "/a.mnu"};
	preview.with_dependencies = true;
	s.session.handle(preview);
	TEST_EXPECT(s.session.outcome().operation != 0 && v.activity.operation.kind == OperationKind::ImportPlan &&
	            v.dialogs.import_preview.open && v.dialogs.import_preview.plan->rows.empty());
	s.session.run_operations();
	TEST_EXPECT(v.dialogs.import_preview.plan->rows.size() == 2);
	std::vector<ImportSource> shown;
	for (const ImportPlanRow &row : v.dialogs.import_preview.plan->rows)
		if (row.selected) shown.push_back(row.source);
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", menu("two.tga")));
	const auto before = snapshot(root);
	const uint64_t seen = v.events.next_seq() - 1;
	s.session.set_poll_budget({0, 4096});
	s.session.handle(request::import_files(shown, false));
	const uint64_t id = s.session.outcome().operation;
	TEST_EXPECT(s.session.outcome().done() && id != 0 && v.activity.operation.kind == OperationKind::ImportApply);
	size_t polls = 0;
	while (v.activity.operation.running() && polls < 10000) {
		TEST_EXPECT(snapshot(root) == before && v.activity.operation.cancellable);
		s.session.poll();
		++polls;
	}
	TEST_EXPECT(polls >= 4 && snapshot(root) == before);
	TEST_EXPECT(v.activity.last_operation.id == id && v.activity.last_operation.end == OperationEnd::Failed &&
	            v.activity.last_operation.findings.size() == 1 &&
	            v.activity.last_operation.findings[0].code == "import.changed");
	TEST_EXPECT(v.dialogs.import_preview.open && v.dialogs.import_preview.changed);
	const std::vector<ViewEvent> planned = editor_test::events_after(v, seen, ViewEventKind::ImportPlanned);
	TEST_EXPECT(planned.size() == 1 && planned[0].flag);
	return 0;
}

// A Rescan, an import and a rename stepped a poll at a time at a small budget come to the view
// they come to run to their end, member for member, over twin projects: the Rescan after files
// changed on disk (an import source among them), an import of a menu with what it needs, a file's
// rename and a name's rename everywhere.
static int test_stepped_equals_whole() {
	editor_test::TempProjectDir dir("opennova_long_ops_twins");
	Session sessions[2];
	std::vector<std::string> roots;
	const std::string art = dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/b.mnu", "<SCREEN>\r\n<NAME>B</NAME>\r\n<WINDOW TYPE=\"STATIC\" "
	                                                   "NAME=\"W\">\r\n<APPEARANCE STATE=\"DEFAULT\" "
	                                                   "TYPE=\"IMAGE\">b.tga</APPEARANCE>\r\n</WINDOW>\r\n</SCREEN>\r\n") &&
	            editor_test::write_text(art + "/b.tga", "tga"));
	for (size_t i = 0; i < 2; ++i) {
		roots.push_back(make_project(dir.file(i == 0 ? "stepped" : "whole"), "Twin", fixture_projects::style_and_item_files()));
		TEST_EXPECT(!roots.back().empty());
		sessions[i].session.handle(request::open_project(roots.back()));
		sessions[i].session.run_operations();
	}
	// Each request raised in both, stepped in the first (a poll at a time, 4 KB a step), run to its
	// end in the second.
	const auto both = [&](const char *what, const std::function<EditorRequest(size_t)> &make) {
		for (size_t i = 0; i < 2; ++i) {
			ProjectSession &session = sessions[i].session;
			session.set_poll_budget({0, 4096});
			session.handle(make(i));
			if (i == 0) {
				size_t polls = 0;
				while ((sessions[0].view().activity.operation.running() || sessions[0].view().activity.validation.running) &&
				       polls < 100000) {
					session.poll();
					++polls;
				}
			} else {
				session.run_operations();
			}
		}
		return same_lines(what, view_lines(sessions[0].view(), false), view_lines(sessions[1].view(), false));
	};
	for (const std::string &root : roots) {
		TEST_EXPECT(editor_test::write_text(root + "/defs/more.def", fixture_projects::item("Mike", 500)));
		TEST_EXPECT(editor_test::write_bytes(root + "/art/logo.png", gradient_png(8, 8)) && mark_for_import(root + "/art/logo.png"));
		fs::remove(root + "/extra/items.def");
	}
	TEST_EXPECT(both("a Rescan", [](size_t) { return request::rescan(); }));
	TEST_EXPECT(sessions[0].view().project.scan->find("logo.pcx") && sessions[0].view().project.scan->find("more.def") &&
	            !sessions[0].view().project.scan->at_path("extra/items.def"));
	TEST_EXPECT(both("an import", [&](size_t) { return request::import_files({{art + "/b.mnu", {}}, {art + "/b.tga", {}}}, false); }));
	TEST_EXPECT(sessions[0].view().project.scan->find("b.mnu") && sessions[0].view().project.scan->find("b.tga"));
	TEST_EXPECT(both("a file's rename", [](size_t) { return request::rename_asset("b.tga", "c.tga"); }));
	TEST_EXPECT(sessions[0].view().project.scan->find("c.tga") && !sessions[0].view().project.scan->find("b.tga"));
	TEST_EXPECT(both("an import source's rename", [](size_t) { return request::rename_asset("logo.png", "badge.png"); }));
	TEST_EXPECT(sessions[0].view().project.scan->find("badge.pcx") && !sessions[0].view().project.scan->find("logo.pcx"));
	const GraphSymbol *variable = nullptr;
	for (const GraphSymbol *symbol : sessions[0].view().findings.graph->symbols_named(ReferenceKind::StyleVar, "COL_OK"))
		if (symbol->file == "menus/menu_style.mns") variable = symbol;
	TEST_EXPECT(variable != nullptr);
	if (!variable) return 1;
	const std::string locator = variable->locator, field = variable->field;
	TEST_EXPECT(both("a name's rename everywhere", [&](size_t) {
		return request::rename_symbol("menus/menu_style.mns", locator, field, "COL_FINE");
	}));
	TEST_EXPECT(!sessions[0].view().findings.graph->symbols_named(ReferenceKind::StyleVar, "COL_FINE").empty());
	return 0;
}

// Retail leg (OPENNOVA_JO_DIR): the JO install's files a document type opens, and the textures,
// fonts and sounds they name, exported into a project, opened a poll at a time at the editor's own
// budget (kDefaultPollBudget: 10 ms of 1 MiB steps): the polls the Open takes and its wall time,
// then the validation's, and the longest poll of each (the frame the editor would stall for); and
// the base layer a read-only dependency mount of the install builds (GraphLayer::build), timed.
static int test_retail_open() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (a project of the JO install's files opened a step at a time)");
		return 0;
	}
	editor_test::TempProjectDir dir("opennova_long_ops_retail");
	const std::string root = make_project(dir.file("project"), "Retail", {});
	TEST_EXPECT(!root.empty());
	const ProjectDocument document = document_of(root);
	ImportOrigin origin;
	std::string error;
	TEST_EXPECT(origin.open(ImportOrigin::Kind::GameInstall, install, document, error));
	opennova::Vfs mount;
	TEST_EXPECT(mount_retail(mount, install, document));
	size_t exported = 0;
	uint64_t bytes_exported = 0;
	std::vector<LayerFile> layer_files;
	for (const opennova::VfsFileLocation &location : mount.list_files()) {
		const std::string &name = location.logical_name;
		if (opennova::strutil::ends_with_icase(name, ".pff")) continue;
		const AssetKind kind = origin.file_kind(name);
		LayerFile layer;
		layer.name = name;
		layer.kind = kind;
		layer.read = [&origin, name](std::vector<uint8_t> &out, std::string &) { return origin.read(name, out); };
		layer_files.push_back(std::move(layer));
		// The files a document type opens and those they name (textures, fonts): the project a modder
		// imports whole; the videos and the sound banks are left in the install.
		if (!document_type_for(kind) && kind != AssetKind::Texture && kind != AssetKind::Font) continue;
		std::vector<uint8_t> file;
		if (!origin.read(name, file)) continue;
		TEST_EXPECT(editor_test::write_bytes(root + "/" + asset_kind_token(kind) + "/" + name, file));
		++exported;
		bytes_exported += file.size();
	}
	TEST_EXPECT(exported > 1000);
	using clock = std::chrono::steady_clock;
	const auto ms_since = [](clock::time_point start) {
		return std::chrono::duration<double, std::milli>(clock::now() - start).count();
	};
	Session s;
	const SessionView &v = s.view();
	s.session.set_poll_budget(kDefaultPollBudget);
	const auto started = clock::now();
	s.session.handle(request::open_project(root));
	const double request_ms = ms_since(started);
	size_t open_polls = 0, validation_polls = 0;
	double longest_open = 0, longest_validation = 0;
	while (v.activity.operation.running()) {
		const auto poll = clock::now();
		s.session.poll();
		longest_open = std::max(longest_open, ms_since(poll));
		++open_polls;
	}
	const double open_ms = ms_since(started);
	const auto validating = clock::now();
	while (v.activity.validation.running) {
		const auto poll = clock::now();
		s.session.poll();
		longest_validation = std::max(longest_validation, ms_since(poll));
		++validation_polls;
	}
	const double validation_ms = ms_since(validating);
	TEST_EXPECT(v.project.open && v.activity.last_operation.end == OperationEnd::Done && !v.findings.diagnostics.empty());
	std::printf("retail: %zu files exported (%.1f MB); opened in %zu polls, %.0f ms (the request %.1f ms, the longest "
	            "poll %.1f ms); validated in %zu polls, %.0f ms (the longest poll %.1f ms); %zu files scanned, %zu "
	            "Problems rows, %zu edges\n",
	            exported, double(bytes_exported) / (1024.0 * 1024.0), open_polls, open_ms, request_ms, longest_open,
	            validation_polls, validation_ms, longest_validation, s.session.files_scanned(),
	            v.findings.diagnostics.size(), v.findings.graph->edge_count());
	TEST_EXPECT(open_polls > 1 && validation_polls > 1);
	// The validation's parts, each timed over the same files afresh: the graph's update (its first
	// step, one whatever it reads), the files' own findings (a file a step), the render check (the
	// step that ends it, with the rows).
	{
		const ProjectPaths paths = ProjectPaths::for_root(root);
		const std::vector<std::shared_ptr<const DocumentBase>> open;
		const ValidationInput input{paths, *v.project.document, *v.project.scan, open};
		AssetGraph graph;
		ValidationCache cache;
		auto part = clock::now();
		graph.update(paths, *v.project.document, *v.project.scan, open);
		const double graph_ms = ms_since(part);
		part = clock::now();
		cache.begin();
		double longest_file = 0;
		std::string longest_name;
		for (const AssetEntry *asset : validation_files(*v.project.scan)) {
			const auto file = clock::now();
			cache.file_findings(input, *asset);
			if (ms_since(file) > longest_file) {
				longest_file = ms_since(file);
				longest_name = asset->relative_path;
			}
		}
		cache.end();
		const double files_ms = ms_since(part);
		ProjectAssetSource assets;
		assets.set_scan(paths.root, *v.project.scan, v.project.document->target_game);
		MenuRenderCheck check;
		part = clock::now();
		check.update(input, assets);
		const double render_ms = ms_since(part);
		std::printf("retail: the validation's parts afresh: the graph's update %.0f ms (%zu files read), the files' own "
		            "findings %.0f ms (%zu files, the longest %.1f ms, %s), the render check %.0f ms (%zu menus rendered)\n",
		            graph_ms, graph.stats().files_extracted, files_ms, validation_files(*v.project.scan).size(),
		            longest_file, longest_name.c_str(), render_ms, check.rendered());
	}
	// The dependency mount's base layer over the whole install, which an expansion project's Open
	// would build (ADR 0046 d12's later list): one call today, timed.
	const auto layered = clock::now();
	GraphStats built;
	const std::shared_ptr<const GraphLayer> layer = GraphLayer::build(layer_files, document.target_game, &built);
	std::printf("retail: a base layer over the install (%zu files, %zu read) in %.0f ms\n", layer->file_count(),
	            built.files_extracted, ms_since(layered));
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += test_project_scan_steps();
	failures += test_import_pass_steps();
	failures += test_scan_update();
	failures += test_validation_steps();
	failures += test_rename_transaction_steps();
	failures += test_open_stepped();
	failures += test_open_cancelled();
	failures += test_save_scans_one_file();
	failures += test_stale_import_refused();
	failures += test_stepped_equals_whole();
	failures += test_retail_open();
	return failures == 0 ? 0 : 1;
}
