// Pins the project session (ADR 0046 d10) over a fake process platform: the request
// kinds it serves and the ones it leaves to the shell, the view it rewrites (a new
// project's checklist, create-missing, the stepped build, Play on a good build, the
// child's exit, the log tail), the editor settings it keeps, and the feature toggle
// that changes the checklist, what a validation costs (the closed files it reads,
// the edits a pump holds), the menu screen the preview follows, and (S11a) the save
// contract, the unsaved prompt and the selection each open document keeps.
#include <chrono>
#include <cstdio>
#include <iterator>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <editor/assets/asset_import.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/document_types.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/reference_queries.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <editor/project_build/build_run.h>
#include <editor/run/play_lease.h>
#include <editor/session/file_preferences_store.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/request_kinds.h>
#include <editor/session/session_json.h>
#include <editor/session/view/session_view.h>
#include <editor/session/view_json.h>
#include <editor/project/project_files.h>
#include <formats/mnu/mnu.h>
#include <formats/pff/pff.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/menu_test_support.h"
#include "editor/png_test_support.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

using editor_test::FakePlatform;

static bool output_has(const SessionView &v, const std::string &needle) {
	for (const std::string &line : v.activity.output)
		if (line.find(needle) != std::string::npos) return true;
	return false;
}

static bool has_code(const std::vector<Diagnostic> &diagnostics, const char *code) {
	for (const Diagnostic &d : diagnostics)
		if (d.code() == code) return true;
	return false;
}

static size_t count_code(const std::vector<Diagnostic> &diagnostics, const char *code) {
	size_t n = 0;
	for (const Diagnostic &d : diagnostics) n += d.code() == code ? 1 : 0;
	return n;
}

// The finding of `code` about `target` (a required file, a reference), or null.
static const Diagnostic *finding_about(const std::vector<Diagnostic> &diagnostics, const char *code,
                                       const std::string &target) {
	for (const Diagnostic &d : diagnostics)
		if (d.code() == code && subject_target(d) == target) return &d;
	return nullptr;
}

// A line of the game's boot report naming `name` (the marker, then what follows it).
static std::string boot_line(const std::string &name, const char *rest) {
	return std::string("BootRootMount: ") + opennova::gameprofile::kBootResourceMissingMarker + name + " " + rest + "\r\n";
}

// The finding of `code` on the file `asset` (project-relative), or null.
static const Diagnostic *finding_in(const std::vector<Diagnostic> &diagnostics, const char *code, const std::string &asset) {
	for (const Diagnostic &d : diagnostics)
		if (d.code() == code && d.asset == asset) return &d;
	return nullptr;
}

// A document's first row (a menu's first screen); none for no document or no row.
static NodeAddress first_row(const Document *document) {
	if (!document || document->rows().empty()) return NodeAddress();
	return {document->rows().front()->id, document->rows().front()->kind, 0};
}

// The fix labelled `label` of the first finding of `code` that offers it.
static bool find_fix(const SessionView &v, const char *code, const std::string &label, ProblemFix &out) {
	for (const Diagnostic &d : v.findings.diagnostics) {
		if (d.code() != code) continue;
		for (const ProblemFix &fix : fixes_for(d, v))
			if (fix.label == label) {
				out = fix;
				return true;
			}
	}
	return false;
}

static int test_lifecycle() {
	editor_test::TempProjectDir dir("opennova_editor_session_test");
	FakePlatform platform;
	FilePreferencesStore preferences(dir.file("settings/editor.json"));
	ProjectSession session(platform, preferences);
	TEST_EXPECT(!session.project_open());
	TEST_EXPECT(session.view().project.recent_projects.empty());

	// The shell-only kinds are declined; the portable ones served.
	EditorRequest pick = request::pick_directory(PickPurpose::OpenProject);
	TEST_EXPECT(!session.handle(pick));
	TEST_EXPECT(session.handle(request::quit()));
	TEST_EXPECT(session.handle(request::build())); // no project: nothing happens
	TEST_EXPECT(!session.view().activity.operation.running());

	// New project: open, listed as recent, the checklist all unmet.
	const std::string root = dir.file("My Game");
	TEST_EXPECT(session.handle(request::new_project(root, "My Game")));
	session.run_operations();
	TEST_EXPECT(session.project_open());
	const SessionView &v = session.view();
	TEST_EXPECT(v.project.document->title == "My Game");
	TEST_EXPECT(v.project.root == root);
	TEST_EXPECT(v.project.requirements->required_total > 0);
	TEST_EXPECT(v.project.requirements->required_missing == v.project.requirements->required_total);
	TEST_EXPECT(v.project.recent_projects.size() == 1 && v.project.recent_projects[0] == root);
	TEST_EXPECT(fs::is_regular_file(dir.file("settings/editor.json")));
	TEST_EXPECT(!v.findings.diagnostics.empty()); // one per unmet required row

	// A build on the unmet project is refused, and Play with it.
	const uint64_t before = v.revisions.any();
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(v.revisions.any() > before);
	TEST_EXPECT(v.activity.has_build && !v.activity.last_build->ok);
	TEST_EXPECT(v.activity.play_state == PlayState::Stopped && platform.spawns == 0);
	TEST_EXPECT(output_has(v, "Build failed"));

	// Create all missing: the checklist clears.
	editor_test::create_missing_files(session);
	TEST_EXPECT(v.project.requirements->required_missing == 0 &&
			v.project.requirements->required_wrong_kind == 0);
	TEST_EXPECT(!v.project.scan->entries.empty());
	TEST_EXPECT(output_has(v, "Created menus/main.mnu"));

	// Build is an operation stepped by bytes, one 64 KiB step per poll at this budget; the view
	// shows its progress, which never goes back, until it lands and says what it came to.
	session.set_poll_budget({0, 64 * 1024});
	session.handle(request::build());
	TEST_EXPECT(v.activity.operation.running() && v.activity.operation.kind == OperationKind::Build && v.activity.operation.cancellable);
	TEST_EXPECT(session.outcome().done() && session.outcome().operation == v.activity.operation.id);
	const uint64_t build_id = v.activity.operation.id;
	size_t polls = 0;
	uint64_t done = 0;
	while (v.activity.operation.running()) {
		session.poll();
		++polls;
		TEST_EXPECT(polls < 200);
		if (!v.activity.operation.running()) break;
		TEST_EXPECT(v.activity.operation.done >= done &&
				v.activity.operation.done <= v.activity.operation.total &&
				v.activity.operation.unit == OperationUnit::Bytes &&
				!v.activity.operation.label.empty());
		done = v.activity.operation.done;
	}
	TEST_EXPECT(polls > 2); // the gate, the hash, the staging, the archives, the loose file, the publish
	TEST_EXPECT(
			v.activity.has_build && v.activity.last_build->ok && !v.activity.operation.running());
	TEST_EXPECT(v.activity.last_operation.id == build_id &&
			v.activity.last_operation.kind == OperationKind::Build &&
			v.activity.last_operation.end == OperationEnd::Done);
	session.set_poll_budget(kDefaultPollBudget);
	TEST_EXPECT(fs::is_regular_file(fs::path(v.activity.last_build->build_dir) / "localres.pff"));
	// S11e: Output names the project by its name and its files (the build too) from its
	// folder: no line holds the folder itself. Clear empties it.
	TEST_EXPECT(output_has(v, "Created My Game.") && output_has(v, "Opened My Game.") &&
	            output_has(v, "Built .opennova/build/play/"));
	TEST_EXPECT(!output_has(v, fs::path(root).generic_string()));
	TEST_EXPECT(session.handle(request::clear_output()) && v.activity.output.empty());
	// A path inside the project from its folder; one outside it (a sibling folder whose name
	// starts alike, another drive) as it is.
	TEST_EXPECT(shown_path(root + "/.opennova/build/play", root) == ".opennova/build/play");
	TEST_EXPECT(shown_path(root + "_other/build", root) == root + "_other/build");
	TEST_EXPECT(shown_path("Z:/elsewhere/build", root) == "Z:/elsewhere/build");
	TEST_EXPECT(shown_path("Z:/elsewhere/build", "") == "Z:/elsewhere/build");

	// Play: no runtime set and none beside a fake editor -> a plain problem, no spawn.
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 0);
	TEST_EXPECT(v.findings.diagnostics.back().code() == "play.runtime_missing");

	// With a runtime the launcher names, Play builds (unchanged) and spawns on it.
	const std::string runtime = dir.file("runtime/opennova.exe");
	TEST_EXPECT(editor_test::write_text(runtime, "MZ"));
	PlayLauncher launcher;
	launcher.executable = runtime;
	launcher.mcp_port = 8999;
	launcher.engine_args = {"--headless"};
	session.set_launcher_source(editor_test::fixed_launcher(launcher));
	TEST_EXPECT(v.activity.runtime_executable == runtime);
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 1);
	TEST_EXPECT(v.activity.play_state == PlayState::Running && v.activity.play_pid == 500);
	TEST_EXPECT(platform.last_plan.working_dir == v.activity.last_build->build_dir);
	TEST_EXPECT(platform.last_plan.args[0] == "--headless");
	TEST_EXPECT(platform.last_plan.mcp_port == 8999);
	TEST_EXPECT(v.activity.play_mcp_port == 8999 && view_section_to_json(v, ViewSection::Run).get_int("mcp_port", 0) == 8999);
	TEST_EXPECT(session.running_build_dir() == v.activity.last_build->build_dir);
	TEST_EXPECT(output_has(v, "Running: "));

	// A second Play while running is refused; the game's log is tailed line by line.
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 1);
	TEST_EXPECT(v.findings.diagnostics.back().code() == "play.already_running");
	TEST_EXPECT(editor_test::write_text(platform.last_plan.log_file, "Godot Engine v4.6.1\r\nhalf"));
	session.poll();
	TEST_EXPECT(output_has(v, "game: Godot Engine v4.6.1"));
	TEST_EXPECT(!output_has(v, "game: half"));
	// The game's boot report names a file it could not find: the name reaches the
	// Requirements rows and Problems, once per file, compared as the game compares names.
	// The row is the project's (no file of it is at fault) and names the requirement's role
	// and file, so Problems opens nothing for it and offers its fixes. The first line is the
	// one the game writes (godot/game/boot_root_mount.gd through push_error: the log's
	// prefix, the class's, the marker, the name, an em dash and the manifest's failure text).
	const opennova::gameprofile::RequiredResource *main_menu =
	        opennova::gameprofile::gameprofile_required_resource_find("main.mnu");
	TEST_EXPECT(main_menu && main_menu->failure);
	const std::string written = std::string("USER ERROR: BootRootMount: ") +
	                            opennova::gameprofile::kBootResourceMissingMarker + "MAIN.MNU \xE2\x80\x94 retail: " +
	                            main_menu->failure + "\r\n";
	TEST_EXPECT(editor_test::write_text(platform.last_plan.log_file,
	                                    "Godot Engine v4.6.1\r\nhalf line\r\n" + written +
	                                            "USER ERROR: BootRootMount: " +
	                                            opennova::gameprofile::kBootResourceMissingMarker +
	                                            "main.mnu - retail: again\r\n"));
	session.poll();
	TEST_EXPECT(v.activity.boot_missing.size() == 1 && v.activity.boot_missing[0] == "MAIN.MNU");
	session.run_operations(); // the validation the report left due (S13 A3: the polls step it)
	TEST_EXPECT(count_code(v.findings.diagnostics, "play.boot_missing") == 1);
	const Diagnostic *boot = finding_about(v.findings.diagnostics, "play.boot_missing", "main.mnu");
	TEST_EXPECT(boot && boot->asset.empty() && editor_test::requirement_of(*boot).role == "main_menu" && boot->severity == DiagnosticSeverity::Error);
	TEST_EXPECT(boot && boot->message.find("MAIN.MNU") != std::string::npos && boot->message.find("Without it") != std::string::npos);
	TEST_EXPECT(v.activity.missing_at_boot("main.mnu"));
	{
		bool marked = false;
		const opennova::io::JsonValue json = view_section_to_json(v, ViewSection::Requirements);
		for (const opennova::io::JsonValue &row : json.get("rows")->array)
			if (row.get_string("name", "") == "main.mnu") marked = row.get_bool("boot_missing", false);
		TEST_EXPECT(marked);
	}

	// The child quits on its own: the view says so on the next poll, with the code it quit
	// with; its endpoint is gone.
	platform.codes[500] = 0;
	platform.exit_child(500);
	session.poll();
	TEST_EXPECT(v.activity.play_state == PlayState::Stopped && v.activity.play_exited_on_its_own && v.activity.play_exit_code == 0);
	TEST_EXPECT(v.activity.play_mcp_port == 0 && view_section_to_json(v, ViewSection::Run).get_int("mcp_port", -1) == 0);
	TEST_EXPECT(view_section_to_json(v, ViewSection::Run).get_int("exit_code", -1) == 0);
	TEST_EXPECT(
			output_has(v, "The game exited.") && !has_code(v.findings.diagnostics, "play.crashed"));
	TEST_EXPECT(session.running_build_dir().empty());

	// A new Play clears the previous boot report, its row with it. A game that ends with
	// another code crashed or stopped on an error: Output says the code, and a Problems row
	// stays until the next Play (a validation keeps it).
	TEST_EXPECT(has_code(v.findings.diagnostics, "play.boot_missing"));
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(v.activity.play_state == PlayState::Running && v.activity.play_exit_code == -1);
	TEST_EXPECT(v.activity.boot_missing.empty() &&
			!has_code(v.findings.diagnostics, "play.boot_missing"));
	platform.codes[501] = 0xC0000005u;
	platform.exit_child(501);
	session.poll();
	session.run_operations(); // the validation the exit left due (S13 A3: the polls step it)
	TEST_EXPECT(v.activity.play_state == PlayState::Stopped && v.activity.play_exited_on_its_own && v.activity.play_exit_code == 0xC0000005LL);
	TEST_EXPECT(output_has(v, "The game exited with code 3221225477 (0xC0000005)."));
	const Diagnostic *crashed = finding_about(v.findings.diagnostics, "play.crashed", "");
	TEST_EXPECT(crashed && crashed->severity == DiagnosticSeverity::Error && crashed->asset.empty() &&
	            crashed->message.find("3221225477 (0xC0000005)") != std::string::npos);
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(count_code(v.findings.diagnostics, "play.crashed") == 1);

	// Stop: terminate, then the deadline kill if ignored (the fake exits on terminate). The
	// crash row goes with the new Play; a stopped game reports no code.
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(v.activity.play_state == PlayState::Running &&
			!has_code(v.findings.diagnostics, "play.crashed"));
	platform.codes[502] = 1;
	session.handle(request::stop_play());
	session.poll();
	TEST_EXPECT(v.activity.play_state == PlayState::Stopped && !v.activity.play_exited_on_its_own &&
			v.activity.play_exit_code == -1);
	TEST_EXPECT(output_has(v, "The game was stopped.") &&
			!has_code(v.findings.diagnostics, "play.crashed"));

	// The mission feature widens the checklist and is saved to the project file.
	const int before_rows = static_cast<int>(v.project.requirements->rows.size());
	editor_test::set_missions(session, true);
	TEST_EXPECT(static_cast<int>(v.project.requirements->rows.size()) > before_rows);
	TEST_EXPECT(v.project.document->features.mission);
	ProjectDocument reloaded;
	Diagnostic error;
	TEST_EXPECT(::opennova::editor::open_project(root, reloaded, error) && reloaded.features.mission);
	ProjectSettingsChange renamed;
	renamed.title = "Renamed";
	editor_test::apply_settings(session, renamed);
	TEST_EXPECT(::opennova::editor::open_project(root, reloaded, error) && reloaded.title == "Renamed");

	// Close, forget, reopen from the settings file with a fresh session.
	session.handle(request::close_project());
	TEST_EXPECT(!session.project_open() && v.project.requirements->rows.empty());
	{
		FakePlatform other;
		FilePreferencesStore again_preferences(dir.file("settings/editor.json"));
		ProjectSession again(other, again_preferences);
		TEST_EXPECT(again.view().project.recent_projects.size() == 1);
		TEST_EXPECT(again.handle(request::open_project(root)));
		again.run_operations();
		TEST_EXPECT(again.view().project.document->title == "Renamed");
		TEST_EXPECT(again.view().project.requirements->required_missing > 0); // the mission rows
		again.handle(request::forget_recent(root));
		TEST_EXPECT(again.view().project.recent_projects.empty());
	}
	// A vanished recent project is dropped from the list when opening it fails.
	{
		FakePlatform other;
		FilePreferencesStore again_preferences(dir.file("settings/editor.json"));
		ProjectSession again(other, again_preferences);
		TEST_EXPECT(!again.handle(request::open_project(dir.file("nowhere"))) ||
		            !again.project_open());
		again.run_operations();
	}
	return 0;
}

static int test_import() {
	editor_test::TempProjectDir dir("opennova_editor_import_test");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Imports"));
	session.run_operations();
	const std::string loose = dir.file("loose.txt");
	const std::string packed = dir.file("source.pff");
	TEST_EXPECT(editor_test::write_text(loose, "loose file"));
	const uint8_t data[] = {'p', 'a', 'c', 'k', 'e', 'd'};
	const opennova::pff::PffWriteEntry entries[] = {
		{"note.txt", data, sizeof(data), 0, 0, 0},
		{"unused.txt", data, sizeof(data), 0, 0, 0},
	};
	TEST_EXPECT(opennova::pff::pff_write_archive(packed.c_str(), opennova::pff::PFF_FORMAT_PFF3,
	                                           entries, 2) == opennova::pff::PFF_WRITE_OK);
	EditorRequest preview = request::of(EditorRequestKind::PreviewImport);
	preview.paths = {loose, packed};
	session.handle(preview);
	session.run_operations();
	// The loose file chosen and planned; the archive's members listed to choose from.
	const DialogsView::ImportPreview &shown = session.view().dialogs.import_preview;
	TEST_EXPECT(shown.open && shown.roots.size() == 1 && shown.roots[0].path == loose && shown.choices.size() == 2);
	TEST_EXPECT(shown.plan->rows.size() == 1 && shown.plan->rows[0].name == "loose.txt" && shown.plan->rows[0].selected);
	session.handle(request::cancel_import());
	TEST_EXPECT(!shown.open && shown.roots.empty() && shown.choices.empty() && shown.plan->rows.empty());
	session.handle(preview);
	session.run_operations();
	// A member chosen from the list: planned with the loose file, the list kept.
	EditorRequest choose = request::of(EditorRequestKind::PlanImport);
	choose.imports = {{loose, {}}, {packed, "note.txt"}};
	session.handle(choose);
	session.run_operations();
	TEST_EXPECT(shown.open && shown.choices.size() == 2 && shown.roots.size() == 2 && shown.plan->rows.size() == 2);
	EditorRequest importing = request::of(EditorRequestKind::ImportFiles);
	importing.imports = choose.imports;
	session.handle(importing);
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && !shown.open);
	TEST_EXPECT(session.view().project.scan->find("loose.txt") &&
			session.view().project.scan->find("note.txt"));
	TEST_EXPECT(!session.view().project.scan->find("unused.txt"));
	std::string text, error;
	TEST_EXPECT(read_file_text(dir.file("project/note.txt"), text, error) && text == "packed");
	TEST_EXPECT(read_file_text(loose, text, error) && text == "loose file");

	// Replacing is explicit and uses the existing project's path and spelling.
	fs::create_directory(dir.file("project/custom"));
	fs::rename(dir.file("project/note.txt"), dir.file("project/custom/NOTE.TXT"));
	TEST_EXPECT(editor_test::write_text(dir.file("project/custom/NOTE.TXT"), "authored"));
	importing.imports = {{packed, "note.txt"}};
	session.handle(importing);
	session.run_operations();
	TEST_EXPECT(session.view().findings.diagnostics.back().code() == "import.exists");
	TEST_EXPECT(read_file_text(dir.file("project/custom/NOTE.TXT"), text, error) && text == "authored");
	importing.replace = true;
	session.handle(importing);
	session.run_operations();
	TEST_EXPECT(read_file_text(dir.file("project/custom/NOTE.TXT"), text, error) && text == "packed");
	TEST_EXPECT(!fs::exists(dir.file("project/note.txt")));

	// Import never writes over unsaved edits: replacing a catalog open with them waits on the
	// unsaved prompt, which lists that file alone and offers no discard (its Save writes the
	// edits, then the import replaces them). An import that replaces no edited file goes ahead.
	// A clean open document reloads after replacement.
	const std::string items = dir.file("items.def");
	TEST_EXPECT(editor_test::write_text(items, "begin \"Marker\"\nid 100001\ntype marker\nhp 10\nend\n"));
	importing.imports = {{items, {}}};
	session.handle(importing);
	session.run_operations();
	session.handle(request::open_document("items.def"));
	TEST_EXPECT(session.view().documents.open.size() == 1);
	const Document *held = session.document_for("items.def");
	// The view holds the open document as the base (S13 D6); the session's is its record document.
	TEST_EXPECT(held && records_of(*session.view().documents.open[0]) == held);
	EditorRequest edit = request::edit_record("defs/items.def", Edit());
	edit.edits[0].address = {records_of(*session.view().documents.open[0])->rows()[0]->id,
	                         node_kind(opennova::def::DefRecordKind::Item), 0};
	edit.edits[0].field = "hp"; edit.edits[0].value = int64_t(20);
	session.handle(edit);
	TEST_EXPECT(session.documents_dirty());
	TEST_EXPECT(editor_test::write_text(items, "begin \"Marker\"\nid 100001\ntype marker\nhp 30\nend\n"));
	session.handle(importing);
	session.run_operations();
	const DialogsView::UnsavedPrompt &prompt = session.view().dialogs.unsaved_prompt;
	TEST_EXPECT(session.outcome().unsaved_prompt && prompt.open && prompt.action == EditorRequestKind::ImportFiles);
	TEST_EXPECT(prompt.files == std::vector<std::string>({"defs/items.def"}) && !prompt.can_discard);
	TEST_EXPECT(!has_code(session.view().findings.diagnostics, "import.unsaved"));
	TEST_EXPECT(read_file_text(dir.file("project/defs/items.def"), text, error) && text.find("hp 10") != std::string::npos);
	EditorRequest cancel = request::resolve_unsaved(UnsavedChoice::Cancel);
	session.handle(cancel);
	TEST_EXPECT(!prompt.open && session.document_for("items.def") == held && held->dirty());
	{
		// Another file, no replace: nothing edited is written over, so nothing waits.
		const std::string other = dir.file("other.txt");
		TEST_EXPECT(editor_test::write_text(other, "other"));
		EditorRequest alone = request::of(EditorRequestKind::ImportFiles);
		alone.imports = {{other, {}}};
		session.handle(alone);
		session.run_operations();
		TEST_EXPECT(session.outcome().done() && !prompt.open && session.view().project.scan->find("other.txt"));
		TEST_EXPECT(session.document_for("items.def") == held && held->dirty());
	}
	session.handle(request::undo());
	session.handle(importing);
	session.run_operations();
	TEST_EXPECT(session.outcome().done());
	const auto &reimported = static_cast<const CatalogRow &>(
	        *records_of(*session.view().documents.open[0])->rows()[0]);
	TEST_EXPECT(reimported.native.as<opennova::def::DefItemDef>().hp == 30);

	const ProjectPaths paths = ProjectPaths::for_root(dir.file("project"));
	const auto invalid = import_assets({{packed, "../escape.txt"}, {packed, "absent.txt"}},
	                                  paths, *session.view().project.document, false);
	TEST_EXPECT(invalid.imported.empty() && invalid.diagnostics.size() == 2);
	TEST_EXPECT(invalid.diagnostics[0].code() == "import.name");
	TEST_EXPECT(!fs::exists(dir.file("escape.txt")));
	// The whole selection or none of it: a file refused refuses the others.
	const auto duplicates = import_assets({{loose, {}}, {loose, {}}}, paths, *session.view().project.document, true);
	TEST_EXPECT(duplicates.imported.empty() && duplicates.diagnostics.size() == 1);
	TEST_EXPECT(duplicates.diagnostics[0].code() == "import.duplicate");
	// The archive's 16-byte name limit binds only what the build packs: a video is copied
	// loose under any name, a texture is refused (and with it the video it came with).
	const std::string video = dir.file("intro_cinematic.bik");
	const std::string texture = dir.file("a_long_texture_name.tga");
	TEST_EXPECT(editor_test::write_text(video, "bink"));
	TEST_EXPECT(editor_test::write_text(texture, "tga"));
	const auto long_names = import_assets({{video, {}}, {texture, {}}}, paths, *session.view().project.document, false);
	TEST_EXPECT(long_names.imported.empty() && long_names.diagnostics.size() == 1);
	TEST_EXPECT(!long_names.diagnostics.empty() && long_names.diagnostics[0].code() == "import.name" &&
	            long_names.diagnostics[0].asset == "a_long_texture_name.tga");
	const auto video_alone =
			import_assets({ { video, {} } }, paths, *session.view().project.document, false);
	TEST_EXPECT(video_alone.imported.size() == 1 && fs::path(video_alone.imported[0]).filename() == "intro_cinematic.bik");
	TEST_EXPECT(editor_test::write_text(dir.file("bad.pff"), "not an archive"));
	preview.paths = {dir.file("bad.pff"), dir.file("missing.txt")};
	session.handle(preview);
	session.run_operations();
	TEST_EXPECT(!session.view().dialogs.import_preview.open);
	return 0;
}

static int test_retail_play() {
	editor_test::TempProjectDir dir("opennova_editor_retail_play_test");
	FakePlatform platform;
	FilePreferencesStore preferences(dir.file("settings.json"));
	ProjectSession session(platform, preferences);
	TEST_EXPECT(
			!session.view().project.play_retail && session.view().project.retail_directory.empty());
	session.handle(request::new_project(dir.file("project"), "Retail test"));
	session.run_operations();
	editor_test::create_missing_files(session);
	ProjectSettingsChange retail;
	retail.play_in_install = true;
	editor_test::apply_settings(session, retail);
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 0 && session.view().findings.diagnostics.back().code() == "play.install_missing");

	const std::string install = dir.file("retail install");
	TEST_EXPECT(editor_test::write_text(install + "/Jointops.exe", "retail executable"));
	TEST_EXPECT(editor_test::write_text(install + "/binkw32.dll", "ordinary Bink"));
	editor_test::set_game_install(session, install);
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 0);
	TEST_EXPECT(session.view().findings.diagnostics.back().message.find("game.cfg") !=
			std::string::npos);
	const std::string built = session.view().activity.last_build->build_dir;
	TEST_EXPECT(!fs::exists(fs::path(built) / "Jointops.exe")); // missing source: no partial stage
	std::vector<uint8_t> archive_before, archive_after;
	std::string io_error;
	TEST_EXPECT(read_file_bytes(built + "/localres.pff", archive_before, io_error));

	TEST_EXPECT(editor_test::write_text(install + "/game.cfg", "video settings"));
	TEST_EXPECT(editor_test::write_text(install + "/binkw32_.dll", "real JOTAC Bink"));
	// Retail is selected even in a source checkout with OpenNova arguments and an MCP port.
	PlayLauncher launcher;
	launcher.source_run = true;
	launcher.executable = dir.file("godot.exe");
	TEST_EXPECT(editor_test::write_text(launcher.executable, "test runtime"));
	launcher.godot_project_dir = dir.file("godot");
	launcher.engine_args = {"--headless"};
	launcher.mcp_port = 8999;
	session.set_launcher_source(editor_test::fixed_launcher(launcher));
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 1 && session.view().activity.play_state == PlayState::Running);
	TEST_EXPECT(platform.last_plan.executable == built + "/Jointops.exe");
	TEST_EXPECT(platform.last_plan.working_dir == built && session.running_build_dir() == built);
	TEST_EXPECT(platform.last_plan.args == std::vector<std::string>({"/w", "/d", "/FRISK"}));
	TEST_EXPECT(platform.last_plan.mcp_port == 0 && platform.last_plan.log_file == built + "/_filelog.txt");
	std::string copied;
	TEST_EXPECT(read_file_text(built + "/binkw32.dll", copied, io_error) && copied == "real JOTAC Bink");
	TEST_EXPECT(read_file_text(built + "/game.cfg", copied, io_error) && copied == "video settings");
	{
		FakePlatform other;
		FilePreferencesStore reopened_preferences(dir.file("settings.json"));
		ProjectSession reopened(other, reopened_preferences);
		TEST_EXPECT(reopened.view().project.play_retail &&
				reopened.view().project.retail_directory == install);
	}
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 1); // Build stays a build with the retail checkbox checked
	session.handle(request::stop_play());
	session.poll();
	TEST_EXPECT(session.view().activity.play_state == PlayState::Stopped);

	// Ordinary installs use the plain Bink DLL. A missing source cannot launch the
	// staged executable left from the successful run.
	fs::remove(fs::path(install) / "binkw32_.dll");
	TEST_EXPECT(editor_test::write_text(built + "/game.cfg", "project video settings"));
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 2);
	TEST_EXPECT(read_file_text(built + "/binkw32.dll", copied, io_error) && copied == "ordinary Bink");
	TEST_EXPECT(read_file_text(built + "/game.cfg", copied, io_error) && copied == "project video settings");
	session.handle(request::stop_play());
	session.poll();
	fs::remove(fs::path(install) / "Jointops.exe");
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 2 && session.view().activity.play_state == PlayState::Stopped);

	// A copy failure is also reported before any child starts.
	TEST_EXPECT(editor_test::write_text(install + "/Jointops.exe", "retail executable"));
	fs::remove(fs::path(built) / "binkw32.dll");
	fs::create_directory(fs::path(built) / "binkw32.dll");
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 2 && session.view().findings.diagnostics.back().code() == "play.install_copy");

	retail.play_in_install = false;
	editor_test::apply_settings(session, retail);
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 3 && platform.last_plan.executable == launcher.executable);
	TEST_EXPECT(platform.last_plan.args[0] == "--path" && platform.last_plan.mcp_port == 8999);
	TEST_EXPECT(read_file_bytes(built + "/localres.pff", archive_after, io_error) && archive_after == archive_before);
	TEST_EXPECT(read_file_text(install + "/game.cfg", copied, io_error) && copied == "video settings");
	return 0;
}

// The import dialog's "Include the files these need" (S11g): SetImportDependencies writes
// the editor's setting, which a session started later reads, and plans an open preview
// again with it (its serial moving, the files the chosen one needs gone or back).
static int test_import_dependencies_setting() {
	editor_test::TempProjectDir dir("opennova_editor_import_setting");
	const std::string settings = dir.file("settings/editor.json");
	const std::string art = dir.file("art");
	TEST_EXPECT(editor_test::write_text(art + "/a.mnu", "<SCREEN>\r\n<NAME>A</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"GO\">\r\n"
	                                                    "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>9</RIGHT><BOTTOM>9</BOTTOM>"
	                                                    "</POSITION>\r\n<FONT><NAME>arial99</NAME></FONT>\r\n</WINDOW>\r\n"
	                                                    "</SCREEN>\r\n") &&
	            editor_test::write_text(art + "/arial99.fnt", "fnt"));
	{
		FakePlatform platform;
		FilePreferencesStore preferences(settings);
		ProjectSession session(platform, preferences);
		const SessionView &v = session.view();
		TEST_EXPECT(v.project.import_dependencies);
		session.handle(request::new_project(dir.file("project"), "Setting"));
		session.run_operations();
		EditorRequest preview = request::of(EditorRequestKind::PreviewImport);
		preview.paths = {art + "/a.mnu"};
		preview.with_dependencies = v.project.import_dependencies;
		session.handle(preview);
		session.run_operations();
		TEST_EXPECT(v.dialogs.import_preview.open && v.dialogs.import_preview.with_dependencies && v.dialogs.import_preview.plan->rows.size() == 2);
		const uint64_t before = v.events.next_seq() - 1;
		EditorRequest off = request::of(EditorRequestKind::SetImportDependencies);
		session.handle(off);
		session.run_operations();
		TEST_EXPECT(session.outcome().done() && !v.project.import_dependencies &&
				!v.dialogs.import_preview.with_dependencies);
		// Planned again: one ImportPlanned event, the dialog's cue to take the new plan's checks.
		TEST_EXPECT(
				editor_test::events_after(v, before, ViewEventKind::ImportPlanned).size() == 1 &&
				v.dialogs.import_preview.plan->rows.size() == 1);
		Preferences stored;
		Diagnostic error;
		TEST_EXPECT(FilePreferencesStore(settings).load(stored, error) && !stored.import_dependencies);
	}
	FakePlatform platform;
	FilePreferencesStore later_preferences(settings);
	ProjectSession later(platform, later_preferences);
	TEST_EXPECT(!later.view().project.import_dependencies);
	EditorRequest on = request::of(EditorRequestKind::SetImportDependencies);
	on.with_dependencies = true;
	later.handle(on);
	later.run_operations();
	TEST_EXPECT(
			later.view().project.import_dependencies && !later.view().dialogs.import_preview.open);
	Preferences stored;
	Diagnostic error;
	TEST_EXPECT(FilePreferencesStore(settings).load(stored, error) && stored.import_dependencies);
	return 0;
}

// What a request came to (the outcome the editor MCP reads) and the refusals that
// leave the project as it was: a rename refused when it commits keeps its Problems
// row past the refresh, a file open with unsaved changes waits on the prompt, a new
// document with a bad name writes nothing, and a new menu of its own gets one screen
// named after the file instead of a copy of STARTUP.
static int test_outcomes_and_refusals() {
	editor_test::TempProjectDir dir("opennova_editor_session_outcomes");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Outcomes"));
	session.run_operations();
	editor_test::create_missing_files(session);
	TEST_EXPECT(session.outcome().done() && session.outcome().findings.empty());
	const SessionView &v = session.view();
	const std::string root = v.project.root;

	// The new name taken on disk after the last scan: the plan passes, the commit
	// refuses, and the finding outlives the refresh that follows.
	TEST_EXPECT(editor_test::write_text(root + "/logo.tga", "tga"));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(editor_test::write_text(root + "/logo2.tga", "late"));
	// The request is done (its commit started), and what the commit came to is its operation's.
	const ActionOutcome renamed = editor_test::handle_to_end(session, request::rename_asset("logo.tga", "logo2.tga"));
	TEST_EXPECT(!renamed.done() && !renamed.unsaved_prompt);
	TEST_EXPECT(has_code(renamed.findings, "rename.exists"));
	TEST_EXPECT(has_code(v.findings.diagnostics, "rename.exists"));
	TEST_EXPECT(fs::exists(root + "/logo.tga"));
	std::string text, error;
	TEST_EXPECT(read_file_text(root + "/logo2.tga", text, error) && text == "late");

	// A file of no kind the game knows is packed all the same (route_asset): a new name past
	// the archives' 16 characters is refused as for any packed kind, nothing moved, where the
	// rename once passed and the scan then held asset.name.too_long against the build.
	TEST_EXPECT(editor_test::write_text(root + "/notes/readme.docx", "notes"));
	session.handle(request::rescan());
	session.run_operations();
	const ActionOutcome long_name =
			editor_test::handle_to_end(session, request::rename_asset("notes/readme.docx", "readme_notes.docx"));
	TEST_EXPECT(!long_name.done() && has_code(long_name.findings, "rename.name"));
	TEST_EXPECT(fs::exists(root + "/notes/readme.docx") &&
			!fs::exists(root + "/notes/readme_notes.docx"));
	// The name the rename refuses is the one the scan refuses.
	TEST_EXPECT(editor_test::write_text(root + "/notes/readme_notes.docx", "notes"));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(has_code(v.findings.diagnostics, "asset.name.too_long"));
	fs::remove_all(root + "/notes");
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(!has_code(v.findings.diagnostics, "asset.name.too_long"));

	// A table open with unsaved changes: the rename waits on the unsaved prompt (its edits
	// would stay behind on the old name), which lists it and offers no discard; cancelled,
	// the file and the edit are untouched.
	session.handle(request::open_document("gametext.bin"));
	Document *table = session.document_for("gametext.bin");
	TEST_EXPECT(table != nullptr);
	if (!table) return 1;
	const std::string table_path = root + "/" + table->path();
	std::vector<uint8_t> before, after;
	TEST_EXPECT(read_file_bytes(table_path, before, error));
	EditorRequest add = request::edit_record(table->path(), Edit());
	add.edits[0].operation = EditOperation::Add;
	add.edits[0].address = {0, table->kind_from_name("section"), 0};
	session.handle(add);
	TEST_EXPECT(table->dirty());
	session.handle(request::rename_asset("gametext.bin", "gametxt2.bin"));
	session.run_operations();
	TEST_EXPECT(!session.outcome().done() && session.outcome().unsaved_prompt && session.outcome().findings.empty());
	TEST_EXPECT(v.dialogs.unsaved_prompt.open && v.dialogs.unsaved_prompt.action == EditorRequestKind::RenameAsset &&
	            v.dialogs.unsaved_prompt.files == std::vector<std::string>({table->path()}) && !v.dialogs.unsaved_prompt.can_discard);
	EditorRequest cancel = request::resolve_unsaved(UnsavedChoice::Cancel);
	session.handle(cancel);
	TEST_EXPECT(
			!v.dialogs.unsaved_prompt.open && !has_code(v.findings.diagnostics, "rename.unsaved"));
	TEST_EXPECT(session.document_for("gametext.bin") == table && table->dirty());
	TEST_EXPECT(read_file_bytes(table_path, after, error) && after == before);
	TEST_EXPECT(!v.project.scan->find("gametxt2.bin") && !fs::exists(fs::path(table_path).parent_path() / "gametxt2.bin"));
	// Closing it waits on the unsaved-changes prompt; cancel keeps it open.
	session.handle(request::close_document(table->path()));
	TEST_EXPECT(session.outcome().unsaved_prompt && !session.outcome().done() && v.dialogs.unsaved_prompt.open);
	session.handle(cancel);
	TEST_EXPECT(session.outcome().done() && !v.dialogs.unsaved_prompt.open && session.document_for("gametext.bin") == table);
	session.handle(request::undo(table->path()));
	TEST_EXPECT(!table->dirty());

	// Bad names for a new document: refused before anything is written.
	struct BadName { const char *path; const char *kind; const char *code; };
	const BadName bad[] = {
		{"../x.mnu", "", "document.name"},
		{"abcdefghijklm.mnu", "", "document.name"}, // 17 bytes: past the archive's 16
		{"foo.mnu", "strings", "document.kind"},
		{"foo.bin", "menu", "document.kind"},
	};
	for (const BadName &name : bad) {
		session.handle(request::create_file(name.path, name.kind));
		TEST_EXPECT(!session.outcome().done() && !session.outcome().findings.empty() &&
		            session.outcome().findings.back().code() == name.code);
		TEST_EXPECT(has_code(v.findings.diagnostics, name.code));
	}
	// "../x.mnu" placed under menus/ would have landed at the project root.
	TEST_EXPECT(!fs::exists(root + "/x.mnu") && !fs::exists(root + "/menus/x.mnu") && !fs::exists(dir.file("x.mnu")));
	TEST_EXPECT(!fs::exists(root + "/menus/abcdefghijklm.mnu"));
	TEST_EXPECT(!fs::exists(root + "/menus/foo.mnu") && !fs::exists(root + "/strings/foo.mnu") &&
	            !fs::exists(root + "/menus/foo.bin") && !fs::exists(root + "/strings/foo.bin"));
	TEST_EXPECT(!v.project.scan->find("foo.mnu") && !v.project.scan->find("foo.bin"));

	// A menu of its own: one screen named after the file, a bare MAIN over the design
	// frame, no Exit button and no second STARTUP.
	session.handle(request::create_file("extra.mnu", "menu"));
	TEST_EXPECT(session.outcome().done());
	const Document *extra = session.document_for("extra.mnu");
	TEST_EXPECT(extra != nullptr && v.documents.active == "menus/extra.mnu");
	opennova::mnu::Document menu;
	TEST_EXPECT(opennova::mnu::parse_file(root + "/menus/extra.mnu", menu, error));
	TEST_EXPECT(menu.screens.size() == 1 && menu.screens[0].name == "EXTRA");
	if (menu.screens.size() == 1 && menu.screens[0].roots.size() == 1) {
		const opennova::mnu::Window &main = menu.screens[0].roots.front();
		TEST_EXPECT(main.name == "MAIN" && main.children.empty());
		TEST_EXPECT(main.position.left == 0 && main.position.top == 0 && main.position.right == 800 &&
		            main.position.bottom == 600);
	}
	NodeAddress found;
	TEST_EXPECT(extra && !find_definition(AssetGraph(), *extra, "EXIT", found) &&
			!find_definition(AssetGraph(), *extra, "STARTUP", found));
	TEST_EXPECT(!has_code(v.findings.diagnostics, "reference.missing"));
	// The required name still gets its requirement's blank.
	const AssetEntry *main_menu = v.project.scan->find("main.mnu");
	TEST_EXPECT(main_menu != nullptr);
	if (main_menu) {
		fs::remove(root + "/" + main_menu->relative_path);
		session.handle(request::rescan());
		session.run_operations();
		session.handle(request::create_file("main.mnu"));
		TEST_EXPECT(session.outcome().done());
		const Document *startup = session.document_for("main.mnu");
		TEST_EXPECT(startup && find_definition(AssetGraph(), *startup, "STARTUP", found) &&
				find_definition(AssetGraph(), *startup, "EXIT", found));
	}
	return 0;
}

// A request that cannot run now says so (ADR 0046 S9f): a rename, a reimport, a Create,
// a Save, a create-missing or an import (S11b) while a build packs the project's files (S13
// A1: the busy gate's operation.busy), and an edit, an undo or a redo on a file that is not
// open, each leave a warning and an outcome that is not done, and change nothing. A blocked
// build lists each blocker once. Play where the platform has no spawn says it is Windows-only
// before building.
static int test_requests_that_cannot_run() {
	editor_test::TempProjectDir dir("opennova_editor_session_cannot_run");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Cannot run"));
	session.run_operations();
	const SessionView &v = session.view();
	const std::string root = v.project.root;

	// Blocked: every unmet requirement is one Problems row, before and after the build,
	// and the build adds its own refusal once.
	const size_t missing = count_code(v.findings.diagnostics, "requirement.missing");
	TEST_EXPECT(missing > 0);
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && !v.activity.last_build->ok);
	TEST_EXPECT(count_code(v.activity.last_build->diagnostics, "requirement.missing") == missing);
	TEST_EXPECT(count_code(v.findings.diagnostics, "requirement.missing") == missing);
	TEST_EXPECT(count_code(v.findings.diagnostics, "build.blocked") == 1);

	editor_test::create_missing_files(session);
	TEST_EXPECT(v.project.requirements->required_missing == 0);
	TEST_EXPECT(editor_test::write_text(root + "/logo.tga", "tga"));
	session.handle(request::rescan());
	session.run_operations();

	// While a build packs: refused with a warning, nothing moved or written.
	session.handle(request::build());
	TEST_EXPECT(session.view().activity.operation.running());
	struct Case { EditorRequest request; const char *code; };
	EditorRequest create = request::create_file("extra.mnu", "menu");
	EditorRequest reimport = request::reimport();
	reimport.force = true;
	EditorRequest brand = request::create_missing({"brand_style"});
	TEST_EXPECT(editor_test::write_text(dir.file("loose.txt"), "loose"));
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = {{dir.file("loose.txt"), {}}};
	const Case during_build[] = {
		{request::rename_asset("logo.tga", "logo2.tga"), "operation.busy"},
		{reimport, "operation.busy"},
		{create, "operation.busy"},
		{request::save_all(), "operation.busy"},
		{brand, "operation.busy"},
		{import, "operation.busy"},
	};
	for (const Case &c : during_build) {
		session.handle(c.request);
		TEST_EXPECT(!session.outcome().done() && !session.outcome().unsaved_prompt);
		TEST_EXPECT(session.outcome().findings.size() == 1 && session.outcome().findings[0].code() == c.code &&
		            session.outcome().findings[0].severity == DiagnosticSeverity::Warning);
		TEST_EXPECT(has_code(v.findings.diagnostics, c.code));
	}
	TEST_EXPECT(fs::exists(root + "/logo.tga") && !fs::exists(root + "/logo2.tga"));
	TEST_EXPECT(!fs::exists(root + "/menus/extra.mnu") && !session.document_for("extra.mnu"));
	TEST_EXPECT(!fs::exists(root + "/menus/brand.mns") && !fs::exists(root + "/loose.txt"));
	session.run_operations();
	TEST_EXPECT(v.activity.last_build->ok);
	// Once it is done the same requests run.
	session.handle(request::rename_asset("logo.tga", "logo2.tga"));
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && fs::exists(root + "/logo2.tga"));

	// An edit, an undo or a redo on a file that is not open.
	TEST_EXPECT(!session.document_for("gametext.bin"));
	EditorRequest edit = request::edit_record("gametext.bin", Edit());
	edit.edits[0].operation = EditOperation::Add;
	edit.edits[0].address = {0, 1, 0};
	for (const EditorRequest &request : {edit, request::undo("gametext.bin"),
	                                     request::redo("gametext.bin")}) {
		session.handle(request);
		TEST_EXPECT(!session.outcome().done());
		TEST_EXPECT(session.outcome().findings.size() == 1 && session.outcome().findings[0].code() == "document.not_open" &&
		            session.outcome().findings[0].severity == DiagnosticSeverity::Warning);
	}
	TEST_EXPECT(!session.document_for("gametext.bin"));

	// Play where nothing can be spawned: said before any build starts.
	platform.spawn_supported = false;
	const size_t spawns = platform.spawns;
	session.handle(request::play());
	TEST_EXPECT(!session.view().activity.operation.running() && platform.spawns == spawns);
	TEST_EXPECT(
			!session.outcome().done() && v.findings.diagnostics.back().code() == "play.unsupported");
	TEST_EXPECT(v.findings.diagnostics.back().message.find("Windows-only") != std::string::npos);
	return 0;
}

// After a rename the document the modder was in stays active: a document the rename
// reloaded does not take over, an untouched one keeps its selection, and the renamed
// file's own document follows it to the new name.
static int test_rename_keeps_the_active_document() {
	editor_test::TempProjectDir dir("opennova_editor_session_rename_active");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Rename active"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	TEST_EXPECT(editor_test::write_text(root + "/logo.tga", "tga"));
	session.handle(request::rescan());
	session.run_operations();

	// The startup menu names logo.tga, saved.
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	NodeAddress exit;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "EXIT", exit));
	if (!menu) return 1;
	EditorRequest image =
			request::edit_record(menu->path(), menu_test::image_edits(*menu, exit, "logo.tga"));
	session.handle(image);
	session.handle(request::save_all());
	TEST_EXPECT(!menu->dirty());
	// A menu of its own, open behind the catalog the modder works in.
	session.handle(request::create_file("extra.mnu", "menu"));
	TEST_EXPECT(session.document_for("extra.mnu") != nullptr);
	session.handle(request::open_document("items.def"));
	const Document *items = session.document_for("items.def");
	TEST_EXPECT(items && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
	const std::string items_path = items->path();
	EditorRequest select = request::select_record(items_path, {});
	const NodeAddress record{items->rows()[0]->id, items->rows()[0]->kind, 0};
	select.address = record;
	session.handle(select);
	TEST_EXPECT(v.documents.active == items_path && v.documents.selection.primary == record);

	// The rename reloads main.mnu: the catalog stays active with its selection.
	session.handle(request::rename_asset("logo.tga", "logo2.tga"));
	session.run_operations();
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(v.documents.active == items_path && v.documents.selection.primary == record);
	// An open file that is not active, renamed: the catalog still is.
	session.handle(request::rename_asset("extra.mnu", "extra2.mnu"));
	session.run_operations();
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(v.documents.active == items_path && v.documents.selection.primary == record);
	const Document *renamed = session.document_for("extra2.mnu");
	TEST_EXPECT(renamed != nullptr && !session.document_for("extra.mnu"));
	// The active file renamed: its document follows it, with no stale selection (a menu read
	// again shows its first screen).
	if (!renamed) return 1;
	session.handle(request::open_document(renamed->path()));
	session.handle(request::rename_asset("extra2.mnu", "extra3.mnu"));
	session.run_operations();
	TEST_EXPECT(session.outcome().done());
	const Document *moved = session.document_for("extra3.mnu");
	TEST_EXPECT(moved && v.documents.active == moved->path() && v.documents.selection.primary.row && v.documents.selection.primary == first_row(moved));
	// The active file is one the rename reloads: it stays active, and its selection
	// (an id in the old records) is dropped for its first screen.
	const Document *startup = session.document_for("main.mnu");
	TEST_EXPECT(startup && find_definition(AssetGraph(), *startup, "EXIT", exit));
	if (!startup) return 1;
	const std::string menu_path = startup->path();
	EditorRequest select_exit = request::select_record(menu_path, exit);
	session.handle(select_exit);
	TEST_EXPECT(v.documents.active == menu_path && v.documents.selection.primary == exit);
	session.handle(request::rename_asset("logo2.tga", "logo3.tga"));
	session.run_operations();
	TEST_EXPECT(session.outcome().done());
	const Document *reread = session.document_for("main.mnu");
	TEST_EXPECT(reread && v.documents.active == menu_path && v.documents.selection.primary.row && v.documents.selection.primary == first_row(reread) &&
	            v.documents.selection.records == std::vector<NodeAddress>{first_row(reread)});
	return 0;
}

// The preview's target (S9i): the screen of the last menu selection, kept while another
// document (the stylesheet the screen draws with) is active, cleared when the screen is
// removed or the menu closes.
static int test_preview_target() {
	editor_test::TempProjectDir dir("opennova_editor_session_preview");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Preview"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	TEST_EXPECT(v.documents.previews.menu.path.empty() && v.documents.previews.menu.screen == 0);
	session.handle(request::open_document("main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	NodeAddress exit;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "EXIT", exit));
	if (!menu) return 1;
	const std::string menu_path = menu->path();
	EditorRequest select = request::select_record(menu_path, exit);
	session.handle(select);
	TEST_EXPECT(v.documents.previews.menu.path == menu_path &&
			v.documents.previews.menu.screen == exit.row);
	// The stylesheet active: the preview stays on the screen.
	const AssetEntry *style = v.project.scan->find("menu_style.mns");
	TEST_EXPECT(style != nullptr);
	if (!style) return 1;
	session.handle(request::open_document(style->relative_path));
	TEST_EXPECT(v.documents.active == style->relative_path);
	TEST_EXPECT(v.documents.previews.menu.path == menu_path &&
			v.documents.previews.menu.screen == exit.row);
	// A second screen selected, then removed: the preview clears.
	const NodeKind screen_kind = menu->kind_from_name("screen");
	EditorRequest add = request::edit_record(menu_path, Edit());
	add.edits[0].operation = EditOperation::Add;
	add.edits[0].address = {0, screen_kind, 0};
	session.handle(add);
	const NodeId added = menu->last_added();
	TEST_EXPECT(added != 0 && v.documents.previews.menu.path == menu_path &&
			v.documents.previews.menu.screen == added);
	EditorRequest remove = request::edit_record(menu_path, Edit());
	remove.edits[0].operation = EditOperation::Remove;
	remove.edits[0].address = {added, screen_kind, 0};
	session.handle(remove);
	TEST_EXPECT(v.documents.previews.menu.path.empty() && v.documents.previews.menu.screen == 0);
	// The first screen again, then the menu closed: the preview clears.
	session.handle(select);
	TEST_EXPECT(v.documents.previews.menu.path == menu_path &&
			v.documents.previews.menu.screen == exit.row);
	session.handle(request::open_document(style->relative_path));
	session.handle(request::save_all()); // the screen added and removed: dirty
	TEST_EXPECT(v.documents.previews.menu.path == menu_path &&
			v.documents.previews.menu.screen == exit.row);
	session.handle(request::close_document(menu_path));
	TEST_EXPECT(session.document_for("main.mnu") == nullptr);
	TEST_EXPECT(v.documents.previews.menu.path.empty() && v.documents.previews.menu.screen == 0);
	return 0;
}

static const Diagnostic *finding_on(const std::vector<Diagnostic> &diagnostics, const std::string &asset) {
	for (const Diagnostic &d : diagnostics)
		if (d.asset == asset && d.severity == DiagnosticSeverity::Error) return &d;
	return nullptr;
}

// What a validation costs (ADR 0046 S9e, S13 D4): each file's own findings are made once and
// kept while what they were made from holds (a closed file's size and modified time as the
// scan says them, an open document's instance and revision), so an edit to an open document
// validates that document alone, extracts it alone and reads no file; no request runs the
// validation (S13 A3): the edits between two polls validate once, at the poll, and a finding
// reported between them survives it; the build gates on the session's own findings, so an error
// an unsaved edit made still blocks it; a closed file changed on disk is read again at the next
// refresh, alone; an open one changed on disk is read again (or kept as it was, its error
// blocking) before the build's gate; a rename asked while a validation is due lists every
// document with unsaved edits on its prompt (it plans again once the validation it joins ends).
static int test_validation_cost() {
	editor_test::TempProjectDir dir("opennova_editor_session_validation");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Validation"));
	session.run_operations();
	editor_test::create_missing_files(session);
	// A poll steps the validation within its budget (S13 A3): a few seconds, which no validation
	// of this project outlasts however loaded the machine, so each poll below runs the validation
	// left due to its end, once (and one that started again every poll would show).
	session.set_poll_budget({5000, uint64_t(1) << 20});
	const SessionView &v = session.view();
	const ValidationStats &stats = session.validation_stats();
	const std::string root = v.project.root;
	size_t editable = 0;
	for (const AssetEntry &asset : v.project.scan->entries)
		editable += is_editable_kind(asset.kind) ? 1 : 0;
	TEST_EXPECT(editable >= 3); // the item and weapon tables, the string tables, the startup menu
	// Create-missing's refresh read every new file once.
	TEST_EXPECT(stats.files_validated == editable && stats.files_loaded == editable &&
			stats.files_reused == 0 && stats.files_failed == 0);

	// Opening a document leaves the validation due; the poll validates it from the document,
	// reading nothing: the other files' findings are kept.
	session.handle(request::open_document("items.def"));
	session.poll();
	Document *items = session.document_for("items.def");
	TEST_EXPECT(items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
	TEST_EXPECT(stats.files_validated == 1 && stats.files_loaded == 0 &&
			stats.files_reused == editable - 1);
	const NodeAddress marker{items->rows()[0]->id, items->rows()[0]->kind, 0};
	auto set = [&](const char *field, Value value) {
		EditorRequest request = request::edit_record(items->path(), Edit());
		request.edits[0].address = marker;
		request.edits[0].field = field;
		request.edits[0].value = std::move(value);
		session.handle(request);
	};

	// One Set on the open document, then the poll: one validation of that document alone, no file
	// read, the graph extracting that document alone, the edit's finding listed.
	size_t passes = stats.passes;
	set("type", int64_t(0));
	TEST_EXPECT(stats.passes == passes && !has_code(v.findings.diagnostics, "catalog.item_type"));
	session.poll();
	TEST_EXPECT(stats.passes == passes + 1 && stats.files_validated == 1 &&
			stats.files_loaded == 0 && stats.files_reused == editable - 1);
	TEST_EXPECT(v.findings.graph->stats().files_extracted == 1);
	TEST_EXPECT(has_code(v.findings.diagnostics, "catalog.item_type"));
	set("type", int64_t(4));
	session.poll();
	TEST_EXPECT(!has_code(v.findings.diagnostics, "catalog.item_type"));

	// The edits between two polls (a frame's burst: typing, a drag) validate once, at the poll, on
	// the last value.
	passes = stats.passes;
	set("type", int64_t(0));
	set("type", int64_t(4));
	set("type", int64_t(0));
	TEST_EXPECT(stats.passes == passes && !has_code(v.findings.diagnostics, "catalog.item_type"));
	session.poll();
	TEST_EXPECT(stats.passes == passes + 1 && stats.files_loaded == 0);
	TEST_EXPECT(has_code(v.findings.diagnostics, "catalog.item_type"));
	// A finding reported between two polls lands after the validation the edits left due:
	// reporting it validates nothing (S13 A2), the poll runs that validation once and keeps the
	// finding after the rows it composes, as often as it was reported (the same edit refused
	// twice, two rows).
	passes = stats.passes;
	set("type", int64_t(4));
	set("no_such_field", int64_t(1));
	set("no_such_field", int64_t(1));
	TEST_EXPECT(stats.passes == passes);
	TEST_EXPECT(!v.findings.diagnostics.empty() &&
			v.findings.diagnostics.back().code() == "document.value");
	TEST_EXPECT(count_code(v.findings.diagnostics, "document.value") == 2);
	session.poll();
	TEST_EXPECT(stats.passes == passes + 1);
	TEST_EXPECT(!v.findings.diagnostics.empty() &&
			v.findings.diagnostics.back().code() == "document.value");
	TEST_EXPECT(count_code(v.findings.diagnostics, "document.value") == 2);
	TEST_EXPECT(!has_code(v.findings.diagnostics, "catalog.item_type"));
	// A Move to where the record already is changes nothing: nothing to validate.
	passes = stats.passes;
	EditorRequest stay = request::edit_record(items->path(), Edit());
	stay.edits[0].operation = EditOperation::Move;
	stay.edits[0].address = marker;
	stay.edits[0].position = 0;
	session.handle(stay);
	TEST_EXPECT(stats.passes == passes);
	// So does a Set of the value the field holds (S12 Z2): no step, the document's revision as
	// it was, nothing to validate.
	Value type_now;
	TEST_EXPECT(items->get(marker, "type", type_now));
	const uint64_t revision_now = items->revision();
	const bool dirty_now = items->dirty();
	set("type", type_now);
	TEST_EXPECT(session.last_edit_ok() && stats.passes == passes && items->revision() == revision_now &&
	            items->dirty() == dirty_now);

	// An unsaved edit that errs: Build waits on the unsaved prompt (the edit's finding listed at
	// the poll); the prompt's Save writes the file and builds, and the plan gates on the session's
	// own findings.
	set("type", int64_t(0));
	session.handle(request::build());
	TEST_EXPECT(!session.view().activity.operation.running() && v.dialogs.unsaved_prompt.open && v.dialogs.unsaved_prompt.action == EditorRequestKind::Build);
	session.poll();
	TEST_EXPECT(
			has_code(v.findings.diagnostics, "catalog.item_type") && v.dialogs.unsaved_prompt.open);
	// The finding reported in the earlier burst is gone: a validation for a later change drops it.
	TEST_EXPECT(!has_code(v.findings.diagnostics, "document.value"));
	EditorRequest save_and_build = request::resolve_unsaved(UnsavedChoice::Save);
	session.handle(save_and_build);
	TEST_EXPECT(!items->dirty() && !v.dialogs.unsaved_prompt.open &&
			session.view().activity.operation.running());
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && !v.activity.last_build->ok && has_code(v.activity.last_build->diagnostics, "catalog.item_type"));
	set("type", int64_t(4));
	session.handle(request::save());
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && v.activity.last_build->ok);

	// A closed file changed on disk (another size) is read again at the next refresh,
	// alone; a file that does not load keeps its finding while it stays as it is.
	const AssetEntry *menu_entry = v.project.scan->find("main.mnu");
	TEST_EXPECT(menu_entry != nullptr);
	if (!menu_entry) return 1;
	const std::string menu = menu_entry->relative_path;
	std::vector<uint8_t> original;
	std::string error;
	TEST_EXPECT(read_file_bytes(root + "/" + menu, original, error));
	// A UTF-16 byte-order mark and half a code unit: a menu that cannot be decoded.
	TEST_EXPECT(editor_test::write_bytes(root + "/" + menu, {0xFF, 0xFE, 0x41}));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(stats.files_validated == 1 && stats.files_loaded == 1 && stats.files_failed == 1 &&
			stats.files_reused == editable - 1);
	TEST_EXPECT(session.document_for("items.def") == items); // open and unchanged: kept as it is
	TEST_EXPECT(finding_on(v.findings.diagnostics, menu) != nullptr);
	// A rescan that finds nothing changed: the validation moves nothing the rows are made of, so
	// they stand, composed none (no row copied or compared).
	const size_t compositions = session.problems_compositions();
	const uint64_t findings = v.revisions.of(ViewConcern::Findings);
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(stats.files_validated == 0 && stats.files_loaded == 0 &&
			stats.files_reused == editable);
	TEST_EXPECT(session.problems_compositions() == compositions &&
			v.revisions.of(ViewConcern::Findings) == findings);
	TEST_EXPECT(finding_on(v.findings.diagnostics, menu) != nullptr);
	TEST_EXPECT(editor_test::write_bytes(root + "/" + menu, original));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(stats.files_validated == 1 && stats.files_loaded == 1 && stats.files_failed == 0 &&
			stats.files_reused == editable - 1);
	TEST_EXPECT(finding_on(v.findings.diagnostics, menu) == nullptr);
	// The same size with a new modified time: read again too.
	const AssetEntry *weapons = v.project.scan->find("weapon.def");
	TEST_EXPECT(weapons != nullptr);
	if (!weapons) return 1;
	const fs::path weapon_path = fs::path(root) / weapons->relative_path;
	fs::last_write_time(weapon_path, fs::last_write_time(weapon_path) + std::chrono::hours(1));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(stats.files_validated == 1 && stats.files_loaded == 1 &&
			stats.files_reused == editable - 1);

	// Build packs the files on disk: a clean open document whose file changed outside the
	// editor is read again before the gate, so the gate sees what the build packs.
	Document *held = session.document_for("items.def");
	TEST_EXPECT(held != nullptr && !held->dirty() && !held->rows().empty());
	if (!held || held->rows().empty()) return 1;
	const std::string items_file = root + "/" + held->path();
	std::vector<uint8_t> items_bytes;
	TEST_EXPECT(read_file_bytes(items_file, items_bytes, error));
	{
		DefCatalogDocument outside;
		Diagnostic outside_error;
		TEST_EXPECT(outside.load(items_file, held->path(), held->kind(), v.project.document->target_game, outside_error));
		if (outside.rows().empty()) return 1;
		Edit broken;
		broken.address = {outside.rows()[0]->id, outside.rows()[0]->kind, 0};
		broken.field = "type";
		broken.value = int64_t(0);
		TEST_EXPECT(outside.apply(broken, outside_error) && outside.save(outside_error));
	}
	TEST_EXPECT(!held->matches_file() && !has_code(v.findings.diagnostics, "catalog.item_type"));
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && !v.activity.last_build->ok && has_code(v.activity.last_build->diagnostics, "catalog.item_type"));
	TEST_EXPECT(has_code(v.findings.diagnostics, "catalog.item_type"));
	held = session.document_for("items.def"); // read again: a new document
	TEST_EXPECT(held != nullptr && !held->dirty() && held->matches_file());
	TEST_EXPECT(editor_test::write_bytes(items_file, items_bytes));
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && v.activity.last_build->ok &&
			!has_code(v.findings.diagnostics, "catalog.item_type"));
	// An open document whose file no longer loads stays open as it was, and why is an error
	// (document.stale) that blocks the build; once the file reads again it is gone.
	session.handle(request::open_document(menu));
	const Document *kept = session.document_for(menu);
	TEST_EXPECT(kept != nullptr);
	TEST_EXPECT(editor_test::write_bytes(root + "/" + menu, {0xFF, 0xFE, 0x41}));
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && !v.activity.last_build->ok && finding_on(v.activity.last_build->diagnostics, menu) != nullptr);
	TEST_EXPECT(session.document_for(menu) == kept && has_code(v.activity.last_build->diagnostics, "document.stale"));
	TEST_EXPECT(finding_on(v.findings.diagnostics, menu) != nullptr && finding_on(v.findings.diagnostics, menu)->code() == "document.stale");
	TEST_EXPECT(editor_test::write_bytes(root + "/" + menu, original));
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && v.activity.last_build->ok && session.document_for(menu) == kept && !has_code(v.findings.diagnostics, "document.stale"));

	// A rename asked while the validation an unsaved edit left is due cannot read that edit in the
	// graph yet: its prompt lists every document with unsaved edits, the menu whose new reference
	// the rename would rewrite among them (S13 A3: the rename plans again once the validation it
	// joins has ended).
	TEST_EXPECT(editor_test::write_text(root + "/logo.tga", "tga"));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document(menu));
	Document *menu_document = session.document_for(menu);
	NodeAddress exit;
	TEST_EXPECT(menu_document != nullptr &&
			find_definition(AssetGraph(), *menu_document, "EXIT", exit));
	if (!menu_document) return 1;
	EditorRequest image = request::edit_record(
			menu_document->path(), menu_test::image_edits(*menu_document, exit, "logo.tga"));
	session.handle(image);
	session.handle(request::rename_asset("logo.tga", "logo2.tga"));
	session.run_operations();
	TEST_EXPECT(v.dialogs.unsaved_prompt.open && v.dialogs.unsaved_prompt.files == std::vector<std::string>({menu}));
	TEST_EXPECT(fs::exists(root + "/logo.tga") && !fs::exists(root + "/logo2.tga"));
	session.poll();
	TEST_EXPECT(v.dialogs.unsaved_prompt.open);
	EditorRequest cancel = request::resolve_unsaved(UnsavedChoice::Cancel);
	session.handle(cancel);
	session.handle(request::undo(menu_document->path()));
	TEST_EXPECT(!menu_document->dirty());
	// Closing the project forgets what was read.
	session.handle(request::close_project());
	TEST_EXPECT(stats.files_validated == 0 && stats.files_loaded == 0 && stats.files_reused == 0 &&
			stats.passes == 0);
	return 0;
}

// The project the save tests edit: a new project with its required files, its item table
// holding a line the game ignores (a catalog.ignored_input finding), the item table and
// the string table open, the item table active.
struct SaveProject {
	editor_test::TempProjectDir dir;
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session;
	std::string root, items_path, strings_path;
	Document *items = nullptr;
	Document *strings = nullptr;

	explicit SaveProject(const char *name) : dir(name), session(platform, preferences) {}
	bool open() {
		session.handle(request::new_project(dir.file("project"), "Saves"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const SessionView &v = session.view();
		root = v.project.root;
		const AssetEntry *items_entry = v.project.scan->find("items.def");
		const AssetEntry *strings_entry = v.project.scan->find("gametext.bin");
		if (!items_entry || !strings_entry) return false;
		items_path = items_entry->relative_path;
		strings_path = strings_entry->relative_path;
		if (!editor_test::write_text(root + "/" + items_path,
		                             "begin \"Marker\"\nid 100001\ntype marker\nsubtype Ruins\nhp 10\nend\n"))
			return false;
		session.handle(request::rescan());
		session.run_operations();
		session.handle(request::open_document(items_path));
		session.handle(request::open_document(strings_path));
		session.handle(request::open_document(items_path));
		items = session.document_for(items_path);
		strings = session.document_for(strings_path);
		return items && strings && v.documents.active == items_path && !items->rows().empty();
	}
	NodeAddress marker() const { return {items->rows()[0]->id, items->rows()[0]->kind, 0}; }
	void set_hp(int64_t hp) {
		EditorRequest request = request::edit_record(items_path, Edit());
		request.edits[0].address = marker();
		request.edits[0].field = "hp";
		request.edits[0].value = hp;
		session.handle(request);
	}
	int64_t hp() const { return static_cast<const CatalogRow &>(*items->rows()[0]).native.as<opennova::def::DefItemDef>().hp; }
	// A section added to the string table.
	void add_section() {
		EditorRequest request = request::edit_record(strings_path, Edit());
		request.edits[0].operation = EditOperation::Add;
		request.edits[0].address = {0, strings->kind_from_name("section"), 0};
		session.handle(request);
	}
	// The item table's file cannot be written while a directory stands where the atomic
	// write puts its temporary file (on any system, whoever runs the test).
	bool block_items(bool blocked) {
		std::error_code ec;
		const fs::path temporary = fs::path(root) / (items_path + ".tmp");
		return blocked ? fs::create_directory(temporary, ec) : fs::remove(temporary, ec);
	}
};

// S11a, the save contract. Save writes the file it names (the active one when it names
// none), with no unsaved edits too when the file holds other bytes than it writes: the item
// table's ignored line dropped, its finding gone from the Problems rows, its history kept.
// A clean file with nothing to rewrite writes nothing and says so. Save All goes past a
// file it cannot write, reports it, writes the rest, and says how many of each.
static int test_save_contract() {
	SaveProject project("opennova_editor_session_save");
	TEST_EXPECT(project.open());
	if (!project.items) return 1;
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	TEST_EXPECT(has_code(v.findings.diagnostics, "catalog.ignored_input"));
	// An edit undone: clean, with the step to redo.
	project.set_hp(20);
	session.handle(request::undo(project.items_path));
	TEST_EXPECT(!project.items->dirty() && project.items->can_redo());
	// Save names the string table: clean, its bytes the ones it writes, so nothing is written.
	session.handle(request::save(project.strings_path));
	TEST_EXPECT(session.outcome().done() && v.activity.status == project.strings_path + " has no changes to save.");
	TEST_EXPECT(!output_has(v, "Saved " + project.strings_path));
	// Save naming nothing: the active item table, rewritten without the ignored line.
	session.handle(request::save());
	TEST_EXPECT(session.outcome().done() && output_has(v, "Saved " + project.items_path) && v.activity.status == "Saved 1 file(s).");
	session.run_operations(); // the validation the save left due
	TEST_EXPECT(!has_code(v.findings.diagnostics, "catalog.ignored_input") && project.items->issues().empty());
	std::string text, error;
	TEST_EXPECT(read_file_text(project.root + "/" + project.items_path, text, error));
	TEST_EXPECT(text.find("subtype") == std::string::npos && text.find("hp 10") != std::string::npos);
	session.handle(request::redo(project.items_path));
	TEST_EXPECT(project.items->dirty() && project.hp() == 20);
	session.handle(request::undo(project.items_path));
	TEST_EXPECT(!project.items->dirty() && project.hp() == 10);

	// Save All past a failure: the item table (first) cannot be written, the string table is.
	project.set_hp(30);
	project.add_section();
	TEST_EXPECT(project.items->dirty() && project.strings->dirty() && project.block_items(true));
	session.handle(request::save_all());
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "document.write"));
	TEST_EXPECT(project.items->dirty() && !project.strings->dirty() && output_has(v, "Saved " + project.strings_path));
	TEST_EXPECT(has_code(v.findings.diagnostics, "document.write") && v.activity.status == "Saved 1 file(s); 1 could not be saved: see Problems.");
	TEST_EXPECT(project.block_items(false));
	session.handle(request::save_all());
	TEST_EXPECT(session.outcome().done() && !project.items->dirty() && v.activity.status == "Saved 1 file(s).");
	session.handle(request::save_all());
	TEST_EXPECT(session.outcome().done() && v.activity.status == "No file has unsaved changes.");
	// A file that is not open is read and left closed (S11b; test_rewrite_closed_file); a
	// Save that names nothing with no document active is refused.
	session.handle(request::save("weapon.def"));
	TEST_EXPECT(session.outcome().done() && !session.document_for("weapon.def"));
	session.handle(request::close_document(project.items_path));
	session.handle(request::close_document(project.strings_path));
	session.handle(request::save());
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "document.not_open"));
	return 0;
}

// S11b: Save of a file that is not open (a Rewrite fix names one): read, written when it
// would write other bytes (the item table's ignored line dropped), left closed, its finding
// gone with the refresh; asked again, nothing to write; a file the project lacks is refused,
// and so is one that does not serialize, closed or open, with the reason.
static int test_rewrite_closed_file() {
	editor_test::TempProjectDir dir("opennova_editor_session_rewrite");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Rewrite"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const AssetEntry *entry = v.project.scan->find("items.def");
	TEST_EXPECT(entry != nullptr);
	if (!entry) return 1;
	const std::string items = entry->relative_path;
	const std::string file = v.project.root + "/" + items;
	TEST_EXPECT(editor_test::write_text(file, "begin \"Marker\"\nid 100001\ntype marker\nsubtype Ruins\nhp 10\nend\n"));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(has_code(v.findings.diagnostics, "catalog.ignored_input") &&
			!session.document_for(items));
	session.handle(request::save(items));
	TEST_EXPECT(session.outcome().done() && output_has(v, "Saved " + items) && v.activity.status == "Saved 1 file(s).");
	session.run_operations(); // the validation the save left due
	std::string text, error;
	TEST_EXPECT(read_file_text(file, text, error) && text.find("subtype") == std::string::npos &&
	            text.find("hp 10") != std::string::npos);
	TEST_EXPECT(!has_code(v.findings.diagnostics, "catalog.ignored_input") &&
			!session.document_for(items));
	session.handle(request::save("items.def"));
	TEST_EXPECT(
			session.outcome().done() && v.activity.status == items + " has no changes to save.");
	session.handle(request::save("nowhere.def"));
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "document.missing"));

	// A menu that does not serialize (an empty ACTION TYPE, which retail crashes on) and holds
	// input the game ignores (SCREENX): its own finding says it does not serialize, so the
	// ignored input offers no Rewrite; a Save of it is refused with the reason, the file
	// untouched, closed and open alike: never "no changes".
	const std::string crash_text = "<SCREEN><NAME>C</NAME><WINDOW type=\"button\" name=\"B\" SCREENX=\"1\">"
	                               "<POSITION><LEFT>0</LEFT></POSITION><ACTION type=\"\">X</ACTION></WINDOW></SCREEN>";
	const std::string crash = v.project.root + "/menus/crash.mnu";
	TEST_EXPECT(editor_test::write_text(crash, crash_text));
	session.handle(request::rescan());
	session.run_operations();
	ProblemFix rewrite;
	TEST_EXPECT(has_code(v.findings.diagnostics, "menu.ignored_input") && !find_fix(v, "menu.ignored_input", "Rewrite crash.mnu", rewrite));
	session.handle(request::save("menus/crash.mnu"));
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "document.unserializable"));
	TEST_EXPECT(v.activity.status.find("no changes") == std::string::npos && !session.document_for("crash.mnu"));
	TEST_EXPECT(read_file_text(crash, text, error) && text == crash_text);
	session.handle(request::open_document("crash.mnu"));
	const Document *blocked = session.document_for("crash.mnu");
	TEST_EXPECT(blocked && !blocked->dirty());
	session.handle(request::save("crash.mnu"));
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "document.unserializable"));
	TEST_EXPECT(v.activity.status.find("no changes") == std::string::npos);
	TEST_EXPECT(read_file_text(crash, text, error) && text == crash_text);
	return 0;
}

// S11a, the unsaved prompt names what waits and the files: a Close lists its one file and
// offers Discard; Quit lists every edited file; Build and Play list every one and offer no
// Discard (one is refused, the prompt kept). A Save that cannot write a file keeps the
// prompt open over the build (the other file written); saved, the build runs.
static int test_unsaved_prompt() {
	SaveProject project("opennova_editor_session_prompt");
	TEST_EXPECT(project.open());
	if (!project.items) return 1;
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	const DialogsView::UnsavedPrompt &prompt = v.dialogs.unsaved_prompt;
	const auto answer = [&](UnsavedChoice choice) {
		EditorRequest request = request::resolve_unsaved(choice);
		session.handle(request);
	};
	project.set_hp(20);
	project.add_section();
	const std::vector<std::string> both{project.items_path, project.strings_path};

	session.handle(request::close_document("items.def"));
	TEST_EXPECT(session.outcome().unsaved_prompt && prompt.open && prompt.action == EditorRequestKind::CloseDocument);
	TEST_EXPECT(prompt.target == project.items_path && prompt.files == std::vector<std::string>{project.items_path} &&
	            prompt.can_discard);
	answer(UnsavedChoice::Cancel);
	TEST_EXPECT(!prompt.open && session.document_for(project.items_path) == project.items);

	session.handle(request::quit());
	TEST_EXPECT(prompt.open && prompt.action == EditorRequestKind::Quit && prompt.files == both && prompt.can_discard &&
	            !v.dialogs.quit_requested);
	answer(UnsavedChoice::Cancel);

	session.handle(request::play());
	TEST_EXPECT(prompt.open && prompt.action == EditorRequestKind::Play && prompt.files == both && !prompt.can_discard);
	answer(UnsavedChoice::Cancel);
	TEST_EXPECT(!prompt.open && !session.view().activity.operation.running() && project.platform.spawns == 0);

	session.handle(request::build());
	TEST_EXPECT(prompt.open && prompt.action == EditorRequestKind::Build && prompt.files == both && !prompt.can_discard);
	TEST_EXPECT(!session.view().activity.operation.running() && !session.outcome().done());
	answer(UnsavedChoice::Discard);
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "unsaved.discard"));
	TEST_EXPECT(prompt.open && project.items->dirty() && project.strings->dirty());
	// Save, the item table unwritable: the string table is written, the prompt stays.
	TEST_EXPECT(project.block_items(true));
	answer(UnsavedChoice::Save);
	TEST_EXPECT(!session.outcome().done() && session.outcome().unsaved_prompt && has_code(session.outcome().findings, "document.write"));
	TEST_EXPECT(prompt.open && prompt.action == EditorRequestKind::Build && !session.view().activity.operation.running());
	TEST_EXPECT(project.items->dirty() && !project.strings->dirty());
	TEST_EXPECT(project.block_items(false));
	answer(UnsavedChoice::Save);
	TEST_EXPECT(session.outcome().done() && !prompt.open && !project.items->dirty() && session.view().activity.operation.running());
	session.run_operations();
	TEST_EXPECT(v.activity.has_build);
	return 0;
}

// S11a: a Close or a Reload's prompt lists its one document, and its Save writes that
// document alone: another with unsaved edits stays as it is.
static int test_prompt_saves_what_it_lists() {
	SaveProject project("opennova_editor_session_listed");
	TEST_EXPECT(project.open());
	if (!project.items) return 1;
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	EditorRequest save = request::resolve_unsaved(UnsavedChoice::Save);
	project.set_hp(20);
	project.add_section();
	session.handle(request::close_document(project.items_path));
	TEST_EXPECT(v.dialogs.unsaved_prompt.files == std::vector<std::string>{project.items_path});
	session.handle(save);
	TEST_EXPECT(session.outcome().done() && !session.document_for(project.items_path) && project.strings->dirty());
	TEST_EXPECT(output_has(v, "Saved " + project.items_path) && !output_has(v, "Saved " + project.strings_path));
	std::string text, error;
	TEST_EXPECT(read_file_text(project.root + "/" + project.items_path, text, error) && text.find("hp 20") != std::string::npos);
	// Reopened and edited again: the string table's Reload writes the table alone.
	session.handle(request::open_document(project.items_path));
	project.items = session.document_for(project.items_path);
	TEST_EXPECT(project.items != nullptr);
	if (!project.items) return 1;
	project.set_hp(30);
	session.handle(request::reload_document(project.strings_path));
	TEST_EXPECT(v.dialogs.unsaved_prompt.action == EditorRequestKind::ReloadDocument &&
	            v.dialogs.unsaved_prompt.files == std::vector<std::string>{project.strings_path});
	session.handle(save);
	project.strings = session.document_for(project.strings_path);
	TEST_EXPECT(session.outcome().done() && project.strings && !project.strings->dirty() && project.items->dirty());
	TEST_EXPECT(project.hp() == 30 && output_has(v, "Saved " + project.strings_path));
	return 0;
}

// S11a: what the prompt lists is taken again before its Save or its Discard acts. A file
// an edit (here an undo of the string table's saved section) made unsaved after the prompt
// opened renews the prompt with it: nothing is saved or dropped unlisted, and the answer
// waits again. Answered over the renewed list, it acts.
static int test_prompt_renews() {
	SaveProject project("opennova_editor_session_renew");
	TEST_EXPECT(project.open());
	if (!project.items) return 1;
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	const DialogsView::UnsavedPrompt &prompt = v.dialogs.unsaved_prompt;
	const auto answer = [&](UnsavedChoice choice) {
		EditorRequest request = request::resolve_unsaved(choice);
		session.handle(request);
	};
	const auto undo_strings = [&](EditorRequestKind kind) {
		EditorRequest request = request::of(kind);
		request.path = project.strings_path;
		session.handle(request);
	};
	// The string table: a section added and saved (clean, with the step to undo).
	project.add_section();
	session.handle(request::save(project.strings_path));
	TEST_EXPECT(!project.strings->dirty() && project.strings->can_undo());
	project.set_hp(20);
	const std::vector<std::string> items_only{project.items_path}, both{project.items_path, project.strings_path};

	// Build lists the item table; the table is made unsaved; Save renews and saves nothing.
	session.handle(request::build());
	TEST_EXPECT(prompt.open && prompt.files == items_only);
	undo_strings(EditorRequestKind::Undo);
	TEST_EXPECT(project.strings->dirty());
	answer(UnsavedChoice::Save);
	TEST_EXPECT(!session.outcome().done() && session.outcome().unsaved_prompt && prompt.open && prompt.files == both);
	TEST_EXPECT(project.items->dirty() && project.strings->dirty() && !session.view().activity.operation.running());
	answer(UnsavedChoice::Cancel);
	undo_strings(EditorRequestKind::Redo);
	TEST_EXPECT(!project.strings->dirty());

	// Quit lists the item table; the table is made unsaved; Discard renews and drops nothing.
	session.handle(request::quit());
	TEST_EXPECT(prompt.open && prompt.files == items_only);
	undo_strings(EditorRequestKind::Undo);
	answer(UnsavedChoice::Discard);
	TEST_EXPECT(!session.outcome().done() && prompt.open && prompt.files == both && !v.dialogs.quit_requested);
	TEST_EXPECT(session.document_for(project.items_path) == project.items && project.items->dirty() &&
	            session.document_for(project.strings_path) == project.strings && project.strings->dirty());
	answer(UnsavedChoice::Discard);
	TEST_EXPECT(session.outcome().done() && !prompt.open && v.dialogs.quit_requested && v.documents.open.empty());
	return 0;
}

// S11a: a prompt belongs to its project. Build waits on it in project A and the file is
// saved another way: a later Build finds nothing unsaved, goes ahead and drops the stale
// prompt. Waiting again, with the file saved another way, project B is opened: the prompt
// goes with A, and an answer to it is refused (nothing waits), so nothing builds B.
static int test_prompt_belongs_to_its_project() {
	SaveProject project("opennova_editor_session_prompt_project");
	TEST_EXPECT(project.open());
	if (!project.items) return 1;
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	project.set_hp(20);
	session.handle(request::build());
	TEST_EXPECT(v.dialogs.unsaved_prompt.open);
	session.handle(request::save_all());
	TEST_EXPECT(!project.items->dirty() && v.dialogs.unsaved_prompt.open);
	session.handle(request::build());
	TEST_EXPECT(!v.dialogs.unsaved_prompt.open && session.view().activity.operation.running());
	session.run_operations();

	project.set_hp(25);
	session.handle(request::build());
	session.handle(request::save_all());
	TEST_EXPECT(v.dialogs.unsaved_prompt.open &&
			v.dialogs.unsaved_prompt.action == EditorRequestKind::Build);
	const std::string other = project.dir.file("Other");
	ProjectDocument created;
	Diagnostic error;
	TEST_EXPECT(::opennova::editor::create_project(other, "Other", kDefaultTargetGame, created, error));
	session.handle(request::open_project(other));
	session.run_operations();
	TEST_EXPECT(v.project.open && v.project.root == other && !v.dialogs.unsaved_prompt.open);
	EditorRequest save = request::resolve_unsaved(UnsavedChoice::Save);
	session.handle(save);
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "unsaved.none"));
	TEST_EXPECT(!session.view().activity.operation.running() && !v.activity.has_build);
	return 0;
}

// S11a: each open document keeps its selection. The document made active again by
// OpenDocument with no record takes back the selection it had (one it names still wins);
// one read again or closed forgets it.
static int test_selection_memory() {
	SaveProject project("opennova_editor_session_selection");
	TEST_EXPECT(project.open());
	if (!project.items) return 1;
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	const auto select = [&](const std::string &path, const NodeAddress &address) {
		EditorRequest request = request::select_record(path, address);
		session.handle(request);
	};
	const auto section = [&](size_t index) -> NodeAddress {
		const auto &row = project.strings->rows()[index];
		return {row->id, row->kind, 0};
	};
	const NodeAddress marker = project.marker();
	select(project.items_path, marker);
	TEST_EXPECT(v.documents.active == project.items_path && v.documents.selection.primary == marker);
	select(project.strings_path, section(1));
	select(project.strings_path, section(2));
	TEST_EXPECT(v.documents.active == project.strings_path && v.documents.selection.primary == section(2));
	session.handle(request::open_document(project.items_path));
	TEST_EXPECT(v.documents.active == project.items_path && v.documents.selection.primary == marker &&
	            v.documents.selection.records == std::vector<NodeAddress>{marker});
	session.handle(request::open_document("gametext.bin"));
	TEST_EXPECT(v.documents.active == project.strings_path && v.documents.selection.primary == section(2) &&
	            v.documents.selection.records == std::vector<NodeAddress>{section(2)});
	// A record named (a Problems row, a Go to) wins over the one kept.
	session.handle(request::open_document(project.items_path));
	session.handle(request::open_record(project.strings_path, section(4)));
	TEST_EXPECT(v.documents.active == project.strings_path && v.documents.selection.primary == section(4));
	// Read again: forgotten (its records have new identities).
	session.handle(request::reload_document(project.items_path));
	session.handle(request::open_document(project.strings_path));
	session.handle(request::open_document(project.items_path));
	TEST_EXPECT(v.documents.active == project.items_path && v.documents.selection.primary == NodeAddress() && v.documents.selection.records.empty());
	// Closed: forgotten; the document that becomes active takes back its own.
	session.handle(request::open_document(project.strings_path));
	TEST_EXPECT(v.documents.selection.primary == section(4));
	session.handle(request::close_document(project.strings_path));
	TEST_EXPECT(v.documents.active == project.items_path && v.documents.selection.primary == NodeAddress());
	session.handle(request::open_document("gametext.bin"));
	TEST_EXPECT(v.documents.selection.primary == NodeAddress() && v.documents.selection.records.empty());
	return 0;
}

// S11b: the game's boot report is the project's. Each file it names is a row every
// validation makes again (an edit, its undo and a rescan keep it, once), never a build's
// gate (the next Play must run to clear it); closing the project drops the rows, and a line
// its game writes later is ignored, with no project open, in another, and in this one
// opened again; the next Play's game reports anew, and the Play after that clears it.
static int test_boot_findings() {
	editor_test::TempProjectDir dir("opennova_editor_session_boot");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	const std::string first = dir.file("first");
	session.handle(request::new_project(first, "First"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const std::string runtime = dir.file("runtime/opennova.exe");
	TEST_EXPECT(editor_test::write_text(runtime, "MZ"));
	PlayLauncher launcher;
	launcher.executable = runtime;
	session.set_launcher_source(editor_test::fixed_launcher(launcher));
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(v.activity.play_state == PlayState::Running);
	const std::string log = platform.last_plan.log_file;
	const int64_t first_game = v.activity.play_pid;
	std::string lines = boot_line("gametext.bin", "- retail: exits") + boot_line("mystery.dat", "- unknown");
	TEST_EXPECT(editor_test::write_text(log, lines));
	session.poll();
	session.run_operations(); // the validation the report left due (S13 A3: the polls step it)
	TEST_EXPECT(v.activity.boot_missing.size() == 2 &&
			count_code(v.findings.diagnostics, "play.boot_missing") == 2);
	const Diagnostic *table =
			finding_about(v.findings.diagnostics, "play.boot_missing", "gametext.bin");
	const Diagnostic *mystery =
			finding_about(v.findings.diagnostics, "play.boot_missing", "mystery.dat");
	TEST_EXPECT(table && editor_test::requirement_of(*table).role == "gametext" && table->asset.empty());
	TEST_EXPECT(mystery && editor_test::requirement_of(*mystery).role.empty() && mystery->message.find("Without it") == std::string::npos);
	// An edit and its undo validate again, and so does a rescan: the rows stay, once each.
	session.handle(request::open_document("items.def"));
	Document *items = session.document_for("items.def");
	TEST_EXPECT(items && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
	EditorRequest edit = request::edit_record(items->path(), Edit());
	edit.edits[0].address = {items->rows()[0]->id, items->rows()[0]->kind, 0};
	edit.edits[0].field = "hp";
	edit.edits[0].value = int64_t(42);
	session.handle(edit);
	TEST_EXPECT(items->dirty() && count_code(v.findings.diagnostics, "play.boot_missing") == 2);
	session.handle(request::undo(items->path()));
	TEST_EXPECT(!items->dirty() && count_code(v.findings.diagnostics, "play.boot_missing") == 2);
	session.handle(request::rescan());
	session.run_operations(); // reads the clean table again: `items` is gone
	TEST_EXPECT(count_code(v.findings.diagnostics, "play.boot_missing") == 2);
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(v.activity.last_build->ok &&
			count_code(v.findings.diagnostics, "play.boot_missing") == 2);
	// The project closes, its game still running: the rows go, and a later line of the game's
	// log is ignored with no project open, in another project, and in this one opened again.
	session.handle(request::close_project());
	TEST_EXPECT(v.activity.boot_missing.empty() && !has_code(v.findings.diagnostics, "play.boot_missing") && v.activity.play_state == PlayState::Running);
	lines += boot_line("keyhelp.bin", "- late");
	TEST_EXPECT(editor_test::write_text(log, lines));
	session.poll();
	TEST_EXPECT(v.activity.boot_missing.empty());
	session.handle(request::new_project(dir.file("second"), "Second"));
	session.run_operations();
	lines += boot_line("vmacros.bin", "- late");
	TEST_EXPECT(editor_test::write_text(log, lines));
	session.poll();
	TEST_EXPECT(v.activity.boot_missing.empty() &&
			!has_code(v.findings.diagnostics, "play.boot_missing"));
	session.handle(request::open_project(first));
	session.run_operations();
	lines += boot_line("gameerr.bin", "- late");
	TEST_EXPECT(editor_test::write_text(log, lines));
	session.poll();
	TEST_EXPECT(v.activity.boot_missing.empty() &&
			!has_code(v.findings.diagnostics, "play.boot_missing"));
	// That game ends; the next Play's game reports on this project, the Play after it clears it.
	platform.exit_child(first_game);
	session.poll();
	TEST_EXPECT(v.activity.play_state == PlayState::Stopped);
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(v.activity.play_state == PlayState::Running);
	TEST_EXPECT(editor_test::write_text(platform.last_plan.log_file,
	                                    boot_line("menutxt.bin", "- optional")));
	session.poll();
	session.run_operations(); // the validation the report left due
	TEST_EXPECT(v.activity.boot_missing.size() == 1 && finding_about(v.findings.diagnostics, "play.boot_missing", "menutxt.bin") != nullptr);
	session.handle(request::stop_play());
	session.poll();
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(v.activity.boot_missing.empty() &&
			!has_code(v.findings.diagnostics, "play.boot_missing"));
	return 0;
}

// S11b: an optional file the project lacks is a note naming its row, counted apart from the
// required ones and never a build's gate; made by name (its fix), its note goes.
static int test_optional_rows() {
	editor_test::TempProjectDir dir("opennova_editor_session_optional");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Optional"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	TEST_EXPECT(v.project.requirements->required_missing == 0 &&
			v.project.requirements->required_wrong_kind == 0);
	size_t lacking = 0;
	for (const RequirementRow &row : v.project.requirements->rows)
		lacking += !row.required && row.state == RequirementState::Missing ? 1 : 0;
	TEST_EXPECT(lacking > 0 && count_code(v.findings.diagnostics, "requirement.optional_missing") == lacking);
	for (const Diagnostic &d : v.findings.diagnostics)
		if (d.code() == "requirement.optional_missing")
			TEST_EXPECT(d.severity == DiagnosticSeverity::Info && !editor_test::requirement_of(d).role.empty() && !subject_target(d).empty() && d.asset.empty());
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(v.activity.last_build->ok && !has_code(v.activity.last_build->diagnostics, "requirement.optional_missing"));
	EditorRequest brand = request::create_missing({"brand_style"});
	session.handle(brand);
	TEST_EXPECT(session.outcome().done() && v.project.scan->find("brand.mns") != nullptr);
	session.run_operations(); // the validation the files made left due
	TEST_EXPECT(count_code(v.findings.diagnostics, "requirement.optional_missing") == lacking - 1 &&
			!finding_about(v.findings.diagnostics, "requirement.optional_missing", "brand.mns"));
	return 0;
}

// S11b: create missing makes the files its roles name and nothing else: none named makes
// nothing; a file put in place since the last refresh (the Problems row still says it is
// missing) is found first, refused and left as it is, of the right kind or the wrong one;
// a role no requirement of the project has is refused.
static int test_create_missing_roles() {
	editor_test::TempProjectDir dir("opennova_editor_session_create_roles");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Roles"));
	session.run_operations();
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	const int total = v.project.requirements->required_total;
	session.handle(request::of(EditorRequestKind::CreateMissing));
	TEST_EXPECT(session.outcome().done() && v.activity.status == "Nothing to create." && v.project.requirements->required_missing == total);
	EditorRequest two = request::create_missing({"main_menu", "gametext"});
	session.handle(two);
	TEST_EXPECT(session.outcome().done() && v.project.requirements->required_missing == total - 2);
	TEST_EXPECT(v.project.scan->find("main.mnu") && v.project.scan->find("gametext.bin") && output_has(v, "Created menus/main.mnu"));
	// A table put where the factory's would go, behind the session's back: refused, untouched.
	std::vector<uint8_t> table, after;
	Diagnostic error;
	BlankRequest blank;
	blank.logical_name = "keyhelp.bin";
	blank.role = "menutxt"; // another table than keyhelp's own blank, to tell them apart
	TEST_EXPECT(make_blank(blank, AssetKind::Strings, table, error));
	TEST_EXPECT(editor_test::write_bytes(root + "/strings/keyhelp.bin", table));
	TEST_EXPECT(
			finding_about(v.findings.diagnostics, "requirement.missing", "keyhelp.bin") != nullptr);
	EditorRequest keyhelp = request::create_missing({"keyhelp"});
	session.handle(keyhelp);
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "create_missing.exists"));
	session.run_operations(); // the validation the scan it read left due
	std::string io_error, text;
	TEST_EXPECT(read_file_bytes(root + "/strings/keyhelp.bin", after, io_error) && after == table);
	TEST_EXPECT(!finding_about(v.findings.diagnostics, "requirement.missing", "keyhelp.bin")); // the refresh after it sees the file
	// A file of the wrong kind put in place since: refused, never overwritten.
	TEST_EXPECT(editor_test::write_text(root + "/vmacros.bin", "raw bytes"));
	EditorRequest vmacros = request::create_missing({"vmacros"});
	session.handle(vmacros);
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "create_missing.wrong_kind"));
	TEST_EXPECT(read_file_text(root + "/vmacros.bin", text, io_error) && text == "raw bytes");
	TEST_EXPECT(!fs::exists(root + "/strings/vmacros.bin"));
	// A mission row with missions off: no such requirement of this project.
	EditorRequest ammo = request::create_missing({"ammo_def"});
	session.handle(ammo);
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "create_missing.unknown"));
	TEST_EXPECT(!fs::exists(root + "/defs/ammo.def"));
	return 0;
}

// S11g: a required menu's Import fix opens the import dialog planned with the files it
// needs (the editor's setting, on): the menu chosen, the font and the texture it names
// found in the game install. The import waits while a build packs, the preview kept open
// and nothing written; then it writes the three, a file with unsaved edits it does not
// write over left as it is.
static int test_import_fix_plans_dependencies() {
	editor_test::TempProjectDir dir("opennova_editor_session_import_fix");
	const std::string install = dir.file("install");
	const std::string menu = "<SCREEN>\r\n<NAME>STARTUP</NAME>\r\n<WINDOW TYPE=\"STATIC\" NAME=\"TITLE\">\r\n"
	                         "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>9</RIGHT><BOTTOM>9</BOTTOM></POSITION>\r\n"
	                         "<FONT><NAME>retail</NAME></FONT>\r\n"
	                         "<APPEARANCE STATE=\"DEFAULT\" TYPE=\"IMAGE\">retail.tga</APPEARANCE>\r\n</WINDOW>\r\n</SCREEN>\r\n";
	const std::string fnt = "fnt", tga = "tga";
	const opennova::pff::PffWriteEntry entries[] = {
		{"main.mnu", reinterpret_cast<const uint8_t *>(menu.data()), uint32_t(menu.size()), 0, 0, 0},
		{"retail.fnt", reinterpret_cast<const uint8_t *>(fnt.data()), uint32_t(fnt.size()), 0, 0, 0},
		{"retail.tga", reinterpret_cast<const uint8_t *>(tga.data()), uint32_t(tga.size()), 0, 0, 0}};
	std::error_code ec;
	fs::create_directories(install, ec);
	TEST_EXPECT(opennova::pff::pff_write_archive((install + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 3) ==
	            opennova::pff::PFF_WRITE_OK);
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Import fix"));
	session.run_operations();
	editor_test::set_game_install(session, install);
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	ProblemFix fix;
	TEST_EXPECT(find_fix(v, "requirement.missing", "Import main.mnu from the game data...", fix));
	TEST_EXPECT(fix.request.kind == EditorRequestKind::PreviewInstallImport && fix.request.names == std::vector<std::string>({"main.mnu"}) &&
	            fix.request.with_dependencies && fix.detail.find("with the files it needs") != std::string::npos);
	session.handle(fix.request);
	session.run_operations();
	const DialogsView::ImportPreview &shown = v.dialogs.import_preview;
	TEST_EXPECT(session.outcome().done() && shown.open && shown.with_dependencies && shown.roots.size() == 1);
	TEST_EXPECT(shown.plan->rows.size() == 3 && shown.plan->rows[0].name == "main.mnu" &&
	            shown.plan->rows[0].state == ImportPlanRow::State::Selected);
	for (const char *name : {"retail.fnt", "retail.tga"}) {
		bool found = false;
		for (const ImportPlanRow &row : shown.plan->rows)
			found = found || (row.name == name && row.state == ImportPlanRow::State::Found && row.selected && row.source.install &&
			                  row.found_in == "the game install" && row.needed_by.file == "main.mnu");
		TEST_EXPECT(found);
	}
	std::vector<ImportSource> kept;
	for (const ImportPlanRow &row : shown.plan->rows) kept.push_back(row.source);
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = kept;

	// A build packing: refused with a warning, the preview kept, nothing written.
	session.handle(request::build());
	TEST_EXPECT(session.view().activity.operation.running());
	session.handle(import);
	session.run_operations();
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "operation.busy") && shown.open);
	TEST_EXPECT(!v.project.scan->find("main.mnu") && !fs::exists(root + "/menus/main.mnu"));
	session.run_operations();
	// A file with unsaved edits the import does not write over holds nothing: the import goes
	// ahead, and the edits stay.
	TEST_EXPECT(editor_test::write_text(root + "/defs/items.def", "begin \"Marker\"\nid 100001\ntype marker\nhp 10\nend\n"));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("items.def"));
	const Document *items = session.document_for("items.def");
	TEST_EXPECT(items != nullptr);
	if (!items) return 1;
	EditorRequest edit = request::edit_record(items->path(), Edit());
	edit.edits[0].address = {items->rows()[0]->id, node_kind(opennova::def::DefRecordKind::Item), 0};
	edit.edits[0].field = "hp";
	edit.edits[0].value = int64_t(20);
	session.handle(edit);
	TEST_EXPECT(session.documents_dirty());
	session.handle(import);
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && !shown.open && !v.dialogs.unsaved_prompt.open);
	TEST_EXPECT(session.document_for("items.def") == items && items->dirty());
	for (const char *name : {"main.mnu", "retail.fnt", "retail.tga"}) TEST_EXPECT(v.project.scan->find(name));
	TEST_EXPECT(!finding_about(v.findings.diagnostics, "requirement.missing", "main.mnu") && !has_code(v.findings.diagnostics, "reference.missing"));
	return 0;
}

// S11b: a fix's request, raised through handle, does what its label says: Create makes the
// required file; Import opens the import dialog on that one file of the game data, and
// importing it meets the row; Use renames a file of the project to the required name;
// Rewrite drops the input the game ignores from a closed file; a missing font's Create
// makes the font (not opened: the editor has no font document) and the reference resolves.
static int test_fixes_apply() {
	editor_test::TempProjectDir dir("opennova_editor_session_fixes");
	const std::string install = dir.file("install");
	std::vector<uint8_t> table;
	Diagnostic error;
	BlankRequest blank;
	blank.logical_name = "gametext.bin";
	TEST_EXPECT(make_blank(blank, AssetKind::Strings, table, error));
	const std::vector<uint8_t> splash = editor_test::gradient_png(4, 4, 7);
	const opennova::pff::PffWriteEntry entries[] = {{"gametext.bin", table.data(), uint32_t(table.size()), 0, 0, 0},
	                                                {"splash.png", splash.data(), uint32_t(splash.size()), 0, 0, 0}};
	TEST_EXPECT(editor_test::write_text(install + "/readme.txt", "an install"));
	TEST_EXPECT(opennova::pff::pff_write_archive((install + "/resource.pff").c_str(), opennova::pff::PFF_FORMAT_PFF3, entries, 2) ==
	            opennova::pff::PFF_WRITE_OK);
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Fixes"));
	session.run_operations();
	editor_test::set_game_install(session, install);
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	TEST_EXPECT(editor_test::write_bytes(root + "/strings/spare.bin", table));
	session.handle(request::rescan());
	session.run_operations();
	ProblemFix fix;
	TEST_EXPECT(find_fix(v, "requirement.missing", "Create main.mnu", fix));
	session.handle(fix.request);
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && v.project.scan->find("main.mnu") && !finding_about(v.findings.diagnostics, "requirement.missing", "main.mnu"));
	TEST_EXPECT(find_fix(v, "requirement.missing", "Import gametext.bin from the game data...", fix));
	session.handle(fix.request);
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && v.dialogs.import_preview.open &&
			v.dialogs.import_preview.roots.size() == 1);
	TEST_EXPECT(v.dialogs.import_preview.choices.empty() &&
			v.dialogs.import_preview.plan->rows.size() == 1);
	TEST_EXPECT(!v.dialogs.import_preview.roots.empty() &&
			v.dialogs.import_preview.roots[0].entry == "gametext.bin" &&
			v.dialogs.import_preview.roots[0].install);
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = v.dialogs.import_preview.roots;
	session.handle(import);
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && !v.dialogs.import_preview.open);
	TEST_EXPECT(v.project.scan->find("gametext.bin") && !finding_about(v.findings.diagnostics, "requirement.missing", "gametext.bin"));
	TEST_EXPECT(find_fix(v, "requirement.missing", "Use spare.bin as keyhelp.bin", fix));
	TEST_EXPECT(fix.detail == "Renames spare.bin to keyhelp.bin; nothing refers to it. It cannot be undone with Undo.");
	session.handle(fix.request);
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && v.project.scan->find("keyhelp.bin") && !v.project.scan->find("spare.bin"));
	TEST_EXPECT(!finding_about(v.findings.diagnostics, "requirement.missing", "keyhelp.bin"));
	// The rest made; then the item table with a line the game ignores, closed: Rewrite drops it.
	editor_test::create_missing_files(session);
	TEST_EXPECT(v.project.requirements->required_missing == 0);
	const AssetEntry *items = v.project.scan->find("items.def");
	TEST_EXPECT(items != nullptr);
	if (!items) return 1;
	const std::string items_file = root + "/" + items->relative_path;
	TEST_EXPECT(editor_test::write_text(items_file, "begin \"Marker\"\nid 100001\ntype marker\nsubtype Ruins\nhp 10\nend\n"));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(find_fix(v, "catalog.ignored_input", "Rewrite items.def", fix));
	session.handle(fix.request);
	session.run_operations();
	std::string text, io_error;
	TEST_EXPECT(
			session.outcome().done() && !has_code(v.findings.diagnostics, "catalog.ignored_input"));
	TEST_EXPECT(read_file_text(items_file, text, io_error) && text.find("subtype") == std::string::npos);
	// A font no file of the project is: Create makes it, and the startup menu's name resolves.
	session.handle(request::open_document("main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	NodeAddress main_window;
	TEST_EXPECT(menu && find_definition(AssetGraph(), *menu, "MAIN", main_window));
	if (!menu) return 1;
	EditorRequest font = request::edit_record(menu->path(), Edit());
	font.edits[0].address = main_window;
	font.edits[0].field = "font.name";
	font.edits[0].value = std::string("Custom.fnt");
	session.handle(font);
	session.run_operations(); // the validation the edit left due
	TEST_EXPECT(has_code(v.findings.diagnostics, "reference.missing"));
	TEST_EXPECT(find_fix(v, "reference.missing", "Create Custom.fnt", fix));
	session.handle(fix.request);
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && v.project.scan->find("Custom.fnt") && v.project.scan->find("Custom.fnt")->relative_path == "fonts/Custom.fnt");
	TEST_EXPECT(!session.document_for("Custom.fnt") && v.documents.active == menu->path());
	TEST_EXPECT(!has_code(v.findings.diagnostics, "reference.missing"));

	// A menu image the project lacks, a .png the game data has: its Import copies the game's own
	// file with no import record, so the image resolves as the texture it is.
	NodeAddress exit;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "EXIT", exit));
	EditorRequest image =
			request::edit_record(menu->path(), menu_test::image_edits(*menu, exit, "splash.png"));
	session.handle(image);
	session.handle(request::save_all()); // an import waits for the edits to be saved
	session.run_operations();            // the validation they left due
	TEST_EXPECT(find_fix(v, "reference.missing", "Import splash.png from the game data...", fix));
	session.handle(fix.request);
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && v.dialogs.import_preview.open && v.dialogs.import_preview.roots.size() == 1);
	import.imports = v.dialogs.import_preview.roots;
	session.handle(import);
	session.run_operations();
	const AssetEntry *copied = v.project.scan->find("splash.png");
	TEST_EXPECT(session.outcome().done() && copied && copied->kind == AssetKind::Texture);
	TEST_EXPECT(copied && !fs::exists(root + "/" + copied->relative_path + kImportSidecarSuffix));
	TEST_EXPECT(
			!has_code(v.findings.diagnostics, "reference.missing") && v.project.imports->empty());

	// An author's PNG imported from the disk becomes an import source. With its output gone and
	// the source no longer decoding, the pass cannot make it again and the scan says so; the
	// source mended, the finding's fix imports it again and the output is back.
	const std::string authored = dir.file("badge.png");
	TEST_EXPECT(editor_test::write_bytes(authored, editor_test::gradient_png(8, 8)));
	EditorRequest loose = request::of(EditorRequestKind::ImportFiles);
	loose.imports = {{authored, {}}};
	session.handle(loose);
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && v.project.imports->size() == 1 && (*v.project.imports)[0].outputs.size() == 1);
	if (v.project.imports->size() != 1 || (*v.project.imports)[0].outputs.size() != 1) return 1;
	const std::string source = root + "/" + (*v.project.imports)[0].source;
	const std::string output = root + "/" + (*v.project.imports)[0].outputs[0];
	std::vector<uint8_t> good;
	TEST_EXPECT(fs::exists(source + kImportSidecarSuffix) && fs::is_regular_file(output) && read_file_bytes(source, good, io_error));
	TEST_EXPECT(editor_test::write_text(source, "no longer a png"));
	fs::remove(output);
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(has_code(v.findings.diagnostics, "import.output_missing") && !fs::exists(output));
	TEST_EXPECT(editor_test::write_bytes(source, good));
	TEST_EXPECT(find_fix(v, "import.output_missing", "Import badge.png again", fix));
	session.handle(fix.request);
	session.run_operations();
	TEST_EXPECT(session.outcome().done() && fs::is_regular_file(output) && !has_code(v.findings.diagnostics, "import.output_missing"));
	return 0;
}

// The project settings (S11d): one ApplyProjectSettings names any of them; the session
// writes what differs from the settings in effect, the project's (its name and features)
// to project.opennova and the others to the editor's settings, each file from a copy, so a
// setting whose file could not be written stays the one in effect and a retry writes it
// again, while one written is what the next is compared with. Each Apply posts one
// SettingsApplied event carrying its serial, flagged when the view's settings_result lists a
// setting that could not be written; a failure is a finding and the request's refusal too.
static int test_project_settings() {
	editor_test::TempProjectDir dir("opennova_editor_session_settings");
	FakePlatform platform;
	const std::string settings_file = dir.file("settings/editor.json");
	FilePreferencesStore preferences(settings_file);
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	// The one SettingsApplied event the last Apply posted: its serial, and its flag the failures.
	uint64_t seen = 0;
	const auto applied = [&](uint64_t serial) {
		const std::vector<ViewEvent> events =
				editor_test::events_after(v, seen, ViewEventKind::SettingsApplied);
		seen = v.events.next_seq() - 1;
		return events.size() == 1 && events[0].tag == serial &&
				events[0].flag == !v.project.settings_result.failures.empty() &&
				events[0].path.empty() && !events[0].address.row && events[0].field.empty();
	};
	const std::string install = dir.file("install");
	// No project: the editor's settings apply, a project's is refused.
	ProjectSettingsChange change;
	change.serial = 1;
	change.title = "Nothing open";
	change.game_install = install;
	editor_test::apply_settings(session, change);
	TEST_EXPECT(applied(1) && v.project.settings_result.failures.size() == 1 &&
	            v.project.settings_result.failures[0].code() == "project.none" && v.project.retail_directory == install);
	TEST_EXPECT(!session.outcome().done());
	session.handle(request::new_project(dir.file("project"), "Armory"));
	session.run_operations();
	const std::string root = v.project.root;
	ProjectDocument on_disk;
	Diagnostic error;
	// Nothing differs: nothing written.
	change = ProjectSettingsChange();
	change.serial = 2;
	change.title = "Armory";
	change.mission = false;
	change.game_install = install;
	editor_test::apply_settings(session, change);
	TEST_EXPECT(applied(2) && v.project.settings_result.failures.empty() && v.activity.status == "No setting changed." &&
	            session.outcome().done());
	// A name the project cannot take.
	change = ProjectSettingsChange();
	change.serial = 3;
	change.title = "";
	editor_test::apply_settings(session, change);
	TEST_EXPECT(applied(3) && v.project.settings_result.failures.size() == 1 &&
	            v.project.settings_result.failures[0].code() == "project.title_empty" && v.project.document->title == "Armory");

	// The project file cannot be written (a folder stands where it is written first): the
	// missions feature stays off with its requirements, on every retry; writable again, on.
	std::error_code ec;
	const std::string project_blocker = ProjectPaths::for_root(root).project_file + ".tmp";
	fs::create_directories(project_blocker, ec);
	const size_t rows = v.project.requirements->rows.size();
	change = ProjectSettingsChange();
	change.mission = true;
	for (const uint64_t serial : {uint64_t(4), uint64_t(5)}) {
		change.serial = serial;
		editor_test::apply_settings(session, change);
		TEST_EXPECT(applied(serial) && v.project.settings_result.failures.size() == 1 &&
		            v.project.settings_result.failures[0].code() == "project.write" && !v.project.document->features.mission &&
		            v.project.requirements->rows.size() == rows && has_code(v.findings.diagnostics, "project.write") && !session.outcome().done());
	}
	fs::remove_all(project_blocker, ec);
	change.serial = 6;
	editor_test::apply_settings(session, change);
	TEST_EXPECT(applied(6) && v.project.settings_result.failures.empty() && v.project.document->features.mission &&
	            v.project.requirements->rows.size() > rows);
	TEST_EXPECT(::opennova::editor::open_project(root, on_disk, error) && on_disk.features.mission);

	// The editor's settings cannot be written: a partial success, the name written and the
	// runtime not. The name changed back is written back (Harbor is the name in effect), the
	// runtime still not; the settings writable again, the runtime is written.
	fs::create_directories(settings_file + ".tmp", ec);
	change = ProjectSettingsChange();
	change.serial = 7;
	change.title = "Harbor";
	change.runtime_executable = "C:/tools/opennova.exe";
	editor_test::apply_settings(session, change);
	TEST_EXPECT(applied(7) && v.project.settings_result.failures.size() == 1 &&
	            v.project.settings_result.failures[0].code() == "editor_settings.write");
	TEST_EXPECT(v.project.document->title == "Harbor" && v.project.runtime_setting.empty());
	TEST_EXPECT(::opennova::editor::open_project(root, on_disk, error) && on_disk.title == "Harbor");
	change.serial = 8;
	change.title = "Armory";
	editor_test::apply_settings(session, change);
	TEST_EXPECT(applied(8) && v.project.settings_result.failures.size() == 1 && v.project.document->title == "Armory" &&
	            v.project.runtime_setting.empty());
	TEST_EXPECT(::opennova::editor::open_project(root, on_disk, error) && on_disk.title == "Armory");
	fs::remove_all(settings_file + ".tmp", ec);
	change.serial = 9;
	editor_test::apply_settings(session, change);
	TEST_EXPECT(applied(9) && v.project.settings_result.failures.empty() && v.project.document->title == "Armory" &&
	            v.project.runtime_setting == "C:/tools/opennova.exe" && v.activity.runtime_executable == "C:/tools/opennova.exe" &&
	            v.activity.status == "Saved the settings.");
	Preferences saved;
	TEST_EXPECT(FilePreferencesStore(settings_file).load(saved, error) && saved.runtime_executable == "C:/tools/opennova.exe" &&
	            saved.game_install == install);
	// "" names the runtime packaged beside the editor.
	change = ProjectSettingsChange();
	change.serial = 10;
	change.runtime_executable = "";
	editor_test::apply_settings(session, change);
	TEST_EXPECT(v.project.runtime_setting.empty() &&
			v.activity.runtime_executable == PlayLauncher().executable);
	return 0;
}

// A menu made the active document with nothing of it selected shows its first screen (the
// menu view lists the selected screen's windows, the preview draws it): opened, taken back
// with no selection kept, read again, and after a rescan; a selection it kept, or a record
// named, wins.
static int test_menu_first_screen() {
	SaveProject project("opennova_editor_session_first_screen");
	TEST_EXPECT(project.open());
	if (!project.items) return 1;
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu && !menu->rows().empty());
	if (!menu) return 1;
	const std::string menu_path = menu->path();
	TEST_EXPECT(v.documents.active == menu_path && v.documents.selection.primary == first_row(menu) &&
	            v.documents.selection.records == std::vector<NodeAddress>{first_row(menu)});
	TEST_EXPECT(v.documents.previews.menu.path == menu_path &&
			v.documents.previews.menu.screen == first_row(menu).row);
	const auto select = [&](const NodeAddress &address) {
		EditorRequest request = request::select_record(menu_path, address);
		session.handle(request);
	};
	// A window selected, another document, back: the window, kept.
	NodeAddress exit;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "EXIT", exit));
	select(exit);
	session.handle(request::open_document(project.items_path));
	TEST_EXPECT(v.documents.active == project.items_path);
	session.handle(request::open_document(menu_path));
	TEST_EXPECT(v.documents.active == menu_path && v.documents.selection.primary == exit);
	// Nothing selected, another document, back: the first screen again.
	select(NodeAddress());
	TEST_EXPECT(v.documents.selection.primary == NodeAddress());
	session.handle(request::open_document(project.items_path));
	session.handle(request::open_document(menu_path));
	TEST_EXPECT(v.documents.selection.primary == first_row(menu));
	// A record named wins.
	NodeAddress title;
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "TITLE", title));
	session.handle(request::open_document(project.items_path));
	session.handle(request::open_record(menu_path, title));
	TEST_EXPECT(v.documents.selection.primary == title);
	// Read again (new records): its first screen, whatever was selected.
	session.handle(request::reload_document(menu_path));
	menu = session.document_for(menu_path);
	TEST_EXPECT(menu && v.documents.active == menu_path && v.documents.selection.primary.row && v.documents.selection.primary == first_row(menu));
	if (!menu) return 1;
	// A rescan keeps it while its file is as it was read (the selection with it), and reads it
	// again once the file changed outside the editor.
	TEST_EXPECT(find_definition(AssetGraph(), *menu, "TITLE", title));
	select(title);
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(session.document_for(menu_path) == menu && v.documents.selection.primary == title);
	std::string text, io_error;
	TEST_EXPECT(read_file_text(project.root + "/" + menu_path, text, io_error) &&
	            editor_test::write_text(project.root + "/" + menu_path, text + "\r\n"));
	session.handle(request::rescan());
	session.run_operations();
	menu = session.document_for(menu_path);
	TEST_EXPECT(menu && v.documents.active == menu_path && v.documents.selection.primary.row && v.documents.selection.primary == first_row(menu));
	return 0;
}

// S12 C1: a Rescan reads again only what changed outside the editor. A clean document whose
// file is as it was keeps its identity, its history and its selection; one whose file changed
// is read again; one whose file no longer reads stays open as it was, its reason a Problems
// error (document.stale) until the file reads again; one with unsaved edits whose file changed
// keeps them, with a document.conflict warning whose Reload fix asks about the edits first
// (Don't save reads the file). Save of such a document is refused as a conflict, which the
// warning also follows.
static int test_rescan_keeps_what_did_not_change() {
	SaveProject project("opennova_editor_session_rescan");
	TEST_EXPECT(project.open());
	if (!project.items || !project.strings) return 1;
	ProjectSession &session = project.session;
	const SessionView &v = session.view();
	Document *items = project.items;
	session.handle(request::open_document("main.mnu"));
	Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu != nullptr);
	if (!menu) return 1;
	const std::string menu_path = menu->path();
	// The items table edited and saved: clean, with a step to undo, the marker selected.
	EditorRequest edit = request::edit_record(project.items_path, Edit());
	edit.edits[0].address = project.marker();
	edit.edits[0].field = "hp";
	edit.edits[0].value = int64_t(20);
	session.handle(edit);
	session.handle(request::save(project.items_path));
	EditorRequest select = request::select_record(project.items_path, project.marker());
	session.handle(select);
	TEST_EXPECT(!items->dirty() && items->can_undo() && v.documents.selection.primary == project.marker());
	// The menu changed on disk (a line end added by hand): read again, alone.
	std::string text, error;
	TEST_EXPECT(read_file_text(project.root + "/" + menu_path, text, error) &&
	            editor_test::write_text(project.root + "/" + menu_path, text + "\r\n"));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(session.outcome().done());
	TEST_EXPECT(session.document_for(project.items_path) == items && items->can_undo() && !items->dirty());
	TEST_EXPECT(session.document_for(project.strings_path) == project.strings);
	TEST_EXPECT(
			v.documents.active == project.items_path && v.documents.selection.primary == project.marker());
	const Document *reread = session.document_for(menu_path);
	TEST_EXPECT(reread != nullptr && reread != menu && reread->matches_file());
	TEST_EXPECT(output_has(v, "Reloaded " + menu_path));
	TEST_EXPECT(!has_code(v.findings.diagnostics, "document.stale") && !has_code(v.findings.diagnostics, "document.conflict"));

	// A file that no longer reads (a UTF-16 byte-order mark and half a code unit: a menu that
	// cannot be decoded): the document stays as it was, and why is an error on it, until the
	// file reads again.
	std::vector<uint8_t> menu_bytes;
	TEST_EXPECT(read_file_bytes(project.root + "/" + menu_path, menu_bytes, error));
	TEST_EXPECT(editor_test::write_bytes(project.root + "/" + menu_path, {0xFF, 0xFE, 0x41}));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(session.document_for(menu_path) == reread);
	const Diagnostic *stale = finding_in(v.findings.diagnostics, "document.stale", menu_path);
	TEST_EXPECT(stale && stale->severity == DiagnosticSeverity::Error);
	TEST_EXPECT(output_has(v, "Kept " + menu_path));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(count_code(v.findings.diagnostics, "document.stale") == 1 && session.document_for(menu_path) == reread);
	TEST_EXPECT(editor_test::write_bytes(project.root + "/" + menu_path, menu_bytes));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(session.document_for(menu_path) == reread && !has_code(v.findings.diagnostics, "document.stale"));

	// Unsaved edits over a file that changed: kept, with a warning and its Reload fix.
	edit.edits[0].value = int64_t(30);
	session.handle(edit);
	TEST_EXPECT(items->dirty());
	TEST_EXPECT(editor_test::write_text(project.root + "/" + project.items_path,
	                                    "begin \"Marker\"\nid 100001\ntype marker\nsubtype Ruins\nhp 50\nend\n"));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(session.document_for(project.items_path) == items && items->dirty());
	const Diagnostic *conflict =
			finding_in(v.findings.diagnostics, "document.conflict", project.items_path);
	TEST_EXPECT(conflict && conflict->severity == DiagnosticSeverity::Warning);
	ProblemFix reload;
	TEST_EXPECT(find_fix(v, "document.conflict", "Reload items.def", reload));
	TEST_EXPECT(reload.request.kind == EditorRequestKind::ReloadDocument && reload.request.path == project.items_path &&
	            !reload.bulk);
	// Its Save is refused as a conflict, and the warning stays.
	session.handle(request::save(project.items_path));
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "document.conflict"));
	TEST_EXPECT(
			finding_in(v.findings.diagnostics, "document.conflict", project.items_path) != nullptr);
	// The fix asks about the edits first; Don't save reads the file.
	session.handle(reload.request);
	TEST_EXPECT(v.dialogs.unsaved_prompt.open && v.dialogs.unsaved_prompt.action == EditorRequestKind::ReloadDocument &&
	            v.dialogs.unsaved_prompt.can_discard);
	EditorRequest discard = request::resolve_unsaved(UnsavedChoice::Discard);
	// The discarded document is freed before its file is read again: known by its identity,
	// not its address, which the new one may take.
	const uint64_t discarded = items->identity();
	session.handle(discard);
	session.run_operations(); // the validation the reload left due
	const Document *fresh = session.document_for(project.items_path);
	TEST_EXPECT(fresh != nullptr && fresh->identity() != discarded && !fresh->dirty() && fresh->matches_file());
	TEST_EXPECT(!has_code(v.findings.diagnostics, "document.conflict"));
	if (fresh && !fresh->rows().empty())
		TEST_EXPECT(static_cast<const CatalogRow &>(*fresh->rows()[0]).native.as<opennova::def::DefItemDef>().hp == 50);
	return 0;
}

// S12 C2: the last build's own findings (the plan's build.blocked, those the rows it was gated
// on lack) stay Problems rows through every validation, once each, until the next build: the
// menu bar's "Build failed" always has its rows to show. The findings that blocked it are
// not listed twice.
static int test_build_findings_stay() {
	editor_test::TempProjectDir dir("opennova_editor_session_build_rows");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Build rows"));
	session.run_operations();
	const SessionView &v = session.view();
	const size_t missing = count_code(v.findings.diagnostics, "requirement.missing");
	TEST_EXPECT(missing > 0);
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && !v.activity.last_build->ok &&
			count_code(v.findings.diagnostics, "build.blocked") == 1);
	TEST_EXPECT(count_code(v.findings.diagnostics, "requirement.missing") == missing);
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(count_code(v.findings.diagnostics, "build.blocked") == 1 && count_code(v.findings.diagnostics, "requirement.missing") == missing);
	editor_test::create_missing_files(session);
	TEST_EXPECT(count_code(v.findings.diagnostics, "build.blocked") == 1 && !has_code(v.findings.diagnostics, "requirement.missing"));
	session.handle(request::build());
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && v.activity.last_build->ok &&
			!has_code(v.findings.diagnostics, "build.blocked"));

	// The build's own findings are those its report adds to the rows it was gated on, not to
	// the rows when it ends: an item type of zero, saved, blocks the build; corrected while it
	// packs, the old blocker is not kept (and not listed twice once it is back).
	const AssetEntry *items_entry = v.project.scan->find("items.def");
	TEST_EXPECT(items_entry != nullptr);
	if (!items_entry) return 1;
	const std::string items_path = items_entry->relative_path;
	TEST_EXPECT(editor_test::write_text(v.project.root + "/" + items_path, "begin \"Marker\"\nid 100001\ntype marker\nhp 10\nend\n"));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document(items_path));
	const Document *items = session.document_for(items_path);
	TEST_EXPECT(items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
	const auto set_type = [&](int64_t type) {
		EditorRequest edit = request::edit_record(items->path(), Edit());
		edit.edits[0].address = {items->rows()[0]->id, items->rows()[0]->kind, 0};
		edit.edits[0].field = "type";
		edit.edits[0].value = type;
		session.handle(edit);
	};
	set_type(0);
	session.handle(request::save(items->path()));
	session.run_operations(); // the validation the save left due
	TEST_EXPECT(!items->dirty() && count_code(v.findings.diagnostics, "catalog.item_type") == 1);
	session.handle(request::build());
	TEST_EXPECT(session.view().activity.operation.running());
	set_type(4);
	// The polls validate the edit while the build packs (S13 A3: no request runs it).
	while (v.activity.validation.running) session.poll();
	TEST_EXPECT(!has_code(v.findings.diagnostics, "catalog.item_type"));
	session.run_operations();
	TEST_EXPECT(v.activity.has_build && !v.activity.last_build->ok && has_code(v.activity.last_build->diagnostics, "catalog.item_type"));
	TEST_EXPECT(count_code(v.findings.diagnostics, "build.blocked") == 1 && !has_code(v.findings.diagnostics, "catalog.item_type"));
	set_type(0);
	while (v.activity.validation.running) session.poll();
	TEST_EXPECT(count_code(v.findings.diagnostics, "catalog.item_type") == 1 && count_code(v.findings.diagnostics, "build.blocked") == 1);
	session.handle(request::undo(items->path()));
	session.handle(request::undo(items->path()));

	// A build still packing when another project opens is cancelled (S13 A1: a project switch
	// cancels the running operation), and nothing of it reaches the new project: no build, none
	// of its rows.
	session.handle(request::new_project(dir.file("other"), "Other"));
	session.run_operations();
	session.handle(request::build());
	TEST_EXPECT(session.view().activity.operation.running());
	session.handle(request::new_project(dir.file("third"), "Third"));
	session.run_operations();
	TEST_EXPECT(v.project.open && v.project.document->title == "Third" && !session.view().activity.operation.running() && !v.activity.has_build);
	TEST_EXPECT(!has_code(v.findings.diagnostics, "build.blocked"));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(!has_code(v.findings.diagnostics, "build.blocked"));
	return 0;
}

// The concerns of the view that moved since `before` (view_revisions.h), in the enum's order;
// kCount at the end when `any` did not move with them (it moves with every one, never alone).
static std::vector<ViewConcern> moved_since(const SessionView &v, const ViewRevisions &before) {
	std::vector<ViewConcern> out;
	for (size_t i = 0; i < kViewConcernCount; ++i) {
		const ViewConcern concern = static_cast<ViewConcern>(i);
		if (v.revisions.of(concern) != before.of(concern)) out.push_back(concern);
	}
	if ((v.revisions.any() != before.any()) != !out.empty()) out.push_back(ViewConcern::kCount);
	return out;
}

// S13 D1: each concern of the view moves with what it covers alone, `any` with every one. A
// build's step moves Operation and a line of the game's log Output, neither validating; the
// game's boot report Run (and the row it makes); the next Play drops that row; a Rescan that
// finds the files as they were moves Files alone; opening a document moves DocumentSet and
// ActiveDocument; a selection Selection alone; an edit Documents (and Output, whose status line
// says so), DocumentSet only when the document goes from saved to unsaved, Findings only when
// the Problems rows differ and Graph only when what the graph holds does; a finding reported
// Findings; a close every concern. The view JSON's revision is `any`, its revisions each
// concern's counter by its token.
static int test_view_revisions() {
	using Concerns = std::vector<ViewConcern>;
	const auto has = [](const Concerns &concerns, ViewConcern concern) {
		return std::find(concerns.begin(), concerns.end(), concern) != concerns.end();
	};
	editor_test::TempProjectDir dir("opennova_editor_session_revisions");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Revisions"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const ValidationStats &stats = session.validation_stats();

	// The build: each step moves Operation alone and validates nothing; the last lands it.
	session.set_poll_budget({0, 64 * 1024});
	session.handle(request::build());
	size_t steps = 0;
	while (session.view().activity.operation.running()) {
		const ViewRevisions before = v.revisions;
		const size_t passes = stats.passes;
		session.poll();
		TEST_EXPECT(stats.passes == passes);
		if (!session.view().activity.operation.running()) break;
		++steps;
		TEST_EXPECT(moved_since(v, before) == Concerns({ViewConcern::Operation}));
	}
	TEST_EXPECT(steps > 1 && v.activity.has_build);

	// Play, then a line of the game's log: Output alone, nothing validated; a poll with nothing
	// new moves nothing.
	const std::string runtime = dir.file("runtime/opennova.exe");
	TEST_EXPECT(editor_test::write_text(runtime, "MZ"));
	PlayLauncher launcher;
	launcher.executable = runtime;
	session.set_launcher_source(editor_test::fixed_launcher(launcher));
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(v.activity.play_state == PlayState::Running);
	{
		ViewRevisions before = v.revisions;
		session.poll();
		TEST_EXPECT(moved_since(v, before).empty());
		before = v.revisions;
		const size_t passes = stats.passes;
		const std::string log = platform.last_plan.log_file;
		TEST_EXPECT(editor_test::write_text(log, "Godot Engine v4.6.1\r\n"));
		session.poll();
		TEST_EXPECT(output_has(v, "game: Godot Engine v4.6.1"));
		TEST_EXPECT(moved_since(v, before) == Concerns({ViewConcern::Output}));
		TEST_EXPECT(stats.passes == passes);
		// The game's boot report names a file it did not find: Run and its line, and the validation
		// it leaves due (Operation: the view shows it due, S13 A3); the next poll makes its row.
		before = v.revisions;
		const std::string boot = "Godot Engine v4.6.1\r\n" + boot_line("main.mnu", "- gone");
		TEST_EXPECT(editor_test::write_text(log, boot));
		session.poll();
		TEST_EXPECT(v.activity.boot_missing.size() == 1 && v.activity.validation.running &&
				!has_code(v.findings.diagnostics, "play.boot_missing"));
		TEST_EXPECT(moved_since(v, before) ==
				Concerns({ViewConcern::Output, ViewConcern::Operation, ViewConcern::Run}));
		before = v.revisions;
		while (v.activity.validation.running) session.poll();
		TEST_EXPECT(has_code(v.findings.diagnostics, "play.boot_missing"));
		TEST_EXPECT(moved_since(v, before) == Concerns({ViewConcern::Findings, ViewConcern::Operation}));
	}
	// The game quits, and the next Play drops its boot report: the row goes (Findings) as the
	// game starts, no validation making it go.
	platform.codes[v.activity.play_pid] = 0;
	platform.exit_child(v.activity.play_pid);
	session.poll();
	{
		const ViewRevisions before = v.revisions;
		session.handle(request::play());
		session.run_operations();
		TEST_EXPECT(v.activity.play_state == PlayState::Running && v.activity.boot_missing.empty());
		TEST_EXPECT(!has_code(v.findings.diagnostics, "play.boot_missing"));
		TEST_EXPECT(has(moved_since(v, before), ViewConcern::Findings));
	}

	// An item naming a model the project lacks, read by a Rescan.
	const std::string crate_def =
			"begin \"Crate\"\nid 100300\ntype building\nhp 10\ngraphic crate\nend\n";
	TEST_EXPECT(editor_test::write_text(v.project.root + "/defs/items.def", crate_def));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(finding_about(v.findings.diagnostics, "reference.missing", "crate") != nullptr);

	// A Rescan that finds the files as they were: Files, and the Operation it ran as (S13 A3).
	{
		const ViewRevisions before = v.revisions;
		session.handle(request::rescan());
		session.run_operations();
		TEST_EXPECT(moved_since(v, before) == Concerns({ViewConcern::Files, ViewConcern::Operation}));
	}

	// Opening a document: which are open, and which is active, both move.
	{
		const ViewRevisions before = v.revisions;
		session.handle(request::open_document("items.def"));
		const Concerns moved = moved_since(v, before);
		TEST_EXPECT(has(moved, ViewConcern::DocumentSet));
		TEST_EXPECT(has(moved, ViewConcern::ActiveDocument));
	}
	Document *items = session.document_for("items.def");
	TEST_EXPECT(items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
	const NodeAddress crate{items->rows()[0]->id, items->rows()[0]->kind, 0};
	// A record picked: Selection alone (not which document is active, nor which are open).
	{
		const ViewRevisions before = v.revisions;
		EditorRequest select = request::select_record(items->path(), crate);
		session.handle(select);
		TEST_EXPECT(v.documents.selection.primary == crate);
		TEST_EXPECT(moved_since(v, before) == Concerns({ViewConcern::Selection}));
	}

	// Edits: Documents and the status line, and the validation they leave due, which the polls
	// run (Operation: the view shows it due, then ended, S13 A3); Findings and Graph only when the
	// validation changed them.
	const auto set = [&](const char *field, Value value) {
		EditorRequest request = request::edit_record(items->path(), Edit());
		request.edits[0].address = crate;
		request.edits[0].field = field;
		request.edits[0].value = std::move(value);
		const ViewRevisions before = v.revisions;
		session.handle(request);
		while (v.activity.validation.running) session.poll();
		return moved_since(v, before);
	};
	const uint64_t graph = v.findings.graph->generation();
	// A number no finding and no reference reads: the rows and the graph as they were; the
	// document, saved until now, has unsaved edits (DocumentSet).
	TEST_EXPECT(set("hp", int64_t(20)) == Concerns({ViewConcern::Documents, ViewConcern::Output,
			ViewConcern::Operation, ViewConcern::DocumentSet}));
	TEST_EXPECT(v.findings.graph->generation() == graph);
	// The name the item defines: the graph moves, the rows stay, the document as unsaved as it
	// was (no DocumentSet, no ActiveDocument).
	TEST_EXPECT(set("id", int64_t(100302)) == Concerns({ViewConcern::Graph, ViewConcern::Documents,
			ViewConcern::Output, ViewConcern::Operation}));
	TEST_EXPECT(v.findings.graph->generation() != graph);
	// The model it names, missing either way: the graph and the row both move.
	TEST_EXPECT(set("graphic", std::string("barrel")) == Concerns({ViewConcern::Findings,
			ViewConcern::Graph, ViewConcern::Documents, ViewConcern::Output, ViewConcern::Operation}));
	TEST_EXPECT(finding_about(v.findings.diagnostics, "reference.missing", "barrel") != nullptr);
	{
		const ViewRevisions before = v.revisions;
		session.handle(request::undo(items->path()));
		while (v.activity.validation.running) session.poll();
		TEST_EXPECT(moved_since(v, before) == Concerns({ViewConcern::Findings, ViewConcern::Graph,
				ViewConcern::Documents, ViewConcern::Operation}));
		TEST_EXPECT(finding_about(v.findings.diagnostics, "reference.missing", "crate") != nullptr);
	}

	// A finding reported (a request refused: no prompt is open to answer): Findings, and its line.
	{
		const ViewRevisions before = v.revisions;
		session.handle(request::of(EditorRequestKind::ResolveUnsaved));
		TEST_EXPECT(has_code(v.findings.diagnostics, "unsaved.none"));
		TEST_EXPECT(moved_since(v, before) ==
				Concerns({ViewConcern::Findings, ViewConcern::Output}));
	}
	// One clock on the wire (S13 A5): the state query's view_revision is the view's clock, `any`,
	// and its revisions each concern's stamp, the clock value at which it last moved.
	std::string error;
	const opennova::io::JsonValue json =
			session.query("state", opennova::io::JsonValue::make_null(), error);
	TEST_EXPECT(error.empty() &&
			json.get_number("view_revision", -1.0) == double(v.revisions.any()) &&
			json.get("revision") == nullptr);
	const opennova::io::JsonValue *revisions = json.get("revisions");
	TEST_EXPECT(revisions != nullptr);
	if (revisions)
		for (const ViewConcernRow &row : kViewConcernRows) {
			const double stamp = double(v.revisions.stamp(row.concern));
			TEST_EXPECT(revisions->get_number(row.token, -1.0) == stamp &&
					stamp <= double(v.revisions.any()));
		}

	// Closed (saved first, so nothing waits on the prompt): every concern moves, and the graph
	// is emptied under a generation it never had.
	session.handle(request::save_all());
	{
		const ViewRevisions before = v.revisions;
		const uint64_t open_generation = v.findings.graph->generation();
		session.handle(request::close_project());
		TEST_EXPECT(!session.project_open() && v.findings.graph->edge_count() == 0);
		Concerns every;
		for (size_t i = 0; i < kViewConcernCount; ++i) every.push_back(static_cast<ViewConcern>(i));
		TEST_EXPECT(moved_since(v, before) == every);
		TEST_EXPECT(v.findings.graph->generation() != open_generation);
	}
	return 0;
}

// S12: the import guard looks at every file the import writes, past the import plan's cap: a
// replacement of more files than the cap, the last an edited catalog's, waits on the unsaved
// prompt for that catalog, nothing written.
static int test_import_guard_past_the_cap() {
	editor_test::TempProjectDir dir("opennova_editor_session_import_cap");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Cap"));
	session.run_operations();
	const SessionView &v = session.view();
	const std::string root = v.project.root;
	TEST_EXPECT(editor_test::write_text(root + "/defs/items.def", "begin \"Marker\"\nid 100001\ntype marker\nhp 10\nend\n"));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::open_document("items.def"));
	const Document *items = session.document_for("items.def");
	TEST_EXPECT(items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
	EditorRequest edit = request::edit_record(items->path(), Edit());
	edit.edits[0].address = {items->rows()[0]->id, items->rows()[0]->kind, 0};
	edit.edits[0].field = "hp";
	edit.edits[0].value = int64_t(20);
	session.handle(edit);
	TEST_EXPECT(items->dirty());
	std::vector<std::string> names;
	for (size_t i = 0; i < kImportPlanFileCap; ++i) {
		char name[16];
		std::snprintf(name, sizeof(name), "t%04zu.txt", i);
		names.push_back(name);
	}
	names.push_back("items.def");
	const std::string replacement = "begin \"Marker\"\nid 100001\ntype marker\nhp 30\nend\n";
	const uint8_t filler[] = {'x'};
	std::vector<opennova::pff::PffWriteEntry> entries;
	for (const std::string &name : names) {
		const bool catalog = name == "items.def";
		entries.push_back({name.c_str(), catalog ? reinterpret_cast<const uint8_t *>(replacement.data()) : filler,
		                   catalog ? uint32_t(replacement.size()) : uint32_t(sizeof(filler)), 0, 0, 0});
	}
	const std::string archive = dir.file("many.pff");
	TEST_EXPECT(opennova::pff::pff_write_archive(archive.c_str(), opennova::pff::PFF_FORMAT_PFF3, entries.data(),
	                                             uint32_t(entries.size())) == opennova::pff::PFF_WRITE_OK);
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.replace = true;
	for (const std::string &name : names) import.imports.push_back({archive, name});
	session.handle(import);
	session.run_operations();
	TEST_EXPECT(session.outcome().unsaved_prompt && v.dialogs.unsaved_prompt.action == EditorRequestKind::ImportFiles &&
	            v.dialogs.unsaved_prompt.files == std::vector<std::string>({items->path()}));
	std::string text, error;
	TEST_EXPECT(read_file_text(root + "/defs/items.def", text, error) && text.find("hp 10") != std::string::npos);
	TEST_EXPECT(!v.project.scan->find("t0000.txt") && items->dirty());
	return 0;
}

// The staging directories under a build's output root (`<id>.tmp`).
static size_t staging_dirs(const std::string &output_root) {
	size_t count = 0;
	std::error_code ec;
	for (const fs::directory_entry &entry : fs::directory_iterator(output_root, ec)) {
		const std::string name = entry.path().filename().string();
		if (entry.is_directory(ec) && name.size() > 4 && name.compare(name.size() - 4, 4, kBuildStagingSuffix) == 0) ++count;
	}
	return count;
}

// S13 A1: Play, then Close before its build lands. The project's close cancels the build between
// two steps (a project switch cancels the running operation): the staging directory goes, nothing
// is published, and the Play that waited on it starts nothing, then or on any later poll (the
// play-after-close bug: the waiting Play outlived the project it was asked in).
static int test_play_then_close() {
	editor_test::TempProjectDir dir("opennova_editor_session_play_close");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Play close"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string runtime = dir.file("runtime/opennova.exe");
	TEST_EXPECT(editor_test::write_text(runtime, "MZ"));
	PlayLauncher launcher;
	launcher.executable = runtime;
	session.set_launcher_source(editor_test::fixed_launcher(launcher));
	const std::string output_root = v.project.root + "/.opennova/build/play";

	// The hash pass in 1 MiB steps, then 64 bytes a poll until an archive packs.
	session.set_poll_budget({0, uint64_t(1) << 20});
	session.handle(request::play());
	TEST_EXPECT(session.outcome().done() && v.activity.operation.running() && session.outcome().operation == v.activity.operation.id);
	const uint64_t id = v.activity.operation.id;
	for (size_t polls = 0; polls < 10000 && v.activity.operation.running() && v.activity.operation.done < v.activity.operation.total / 2; ++polls)
		session.poll();
	session.set_poll_budget({0, 64});
	for (size_t polls = 0; polls < 10000 && v.activity.operation.label.rfind("Packing", 0) != 0;
			++polls)
		session.poll();
	TEST_EXPECT(
			v.activity.operation.running() && v.activity.operation.label.rfind("Packing", 0) == 0);
	TEST_EXPECT(staging_dirs(output_root) == 1 && platform.spawns == 0);

	session.handle(request::close_project());
	TEST_EXPECT(!v.project.open && !v.activity.operation.running());
	TEST_EXPECT(v.activity.last_operation.id == id &&
			v.activity.last_operation.end == OperationEnd::Cancelled);
	TEST_EXPECT(staging_dirs(output_root) == 0 && last_good_build_dir(output_root).empty());
	TEST_EXPECT(output_has(v, "Cancelled the build."));
	for (int i = 0; i < 5; ++i) session.poll();
	TEST_EXPECT(platform.spawns == 0 && v.activity.play_state == PlayState::Stopped &&
			!v.activity.has_build);

	// CancelOperation stops a build the same way; with nothing running it is refused.
	session.handle(request::open_project(dir.file("project")));
	session.run_operations();
	session.handle(request::build());
	TEST_EXPECT(v.activity.operation.running());
	session.poll();
	session.handle(request::cancel_operation());
	TEST_EXPECT(session.outcome().done() && !v.activity.operation.running() && v.activity.last_operation.end == OperationEnd::Cancelled);
	TEST_EXPECT(staging_dirs(output_root) == 0 && v.activity.status == "Cancelled the build.");
	session.handle(request::cancel_operation());
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "operation.none"));
	return 0;
}

// S13 A1: a game left running across an editor restart keeps its build. Play writes the game's
// lease beside the build directory (<build-id>.<pid>.lease, the directory itself immutable),
// recording the image and the creation time the platform reports for the game, and removes it
// when the game stops, its own and no other: two games may run from one build. A build asks when
// it publishes which directories a lease protects, naming each lease's pid and creation time to
// the platform: one whose process runs (Alive) or cannot be told (Unknown: a game the platform
// may not question) is kept, and a lease whose process is gone (Dead) is deleted, after which its
// directory is pruned like any other. A file that only looks like a lease (an older record among
// them) is never touched.
static int test_play_leases() {
	editor_test::TempProjectDir dir("opennova_editor_session_leases");
	const std::string project = dir.file("project");
	const std::string runtime = dir.file("runtime/opennova.exe");
	TEST_EXPECT(editor_test::write_text(runtime, "MZ"));
	PlayLauncher launcher;
	launcher.executable = runtime;
	const auto lease_of = [](const std::string &build_dir, int64_t pid) {
		return build_dir + "." + std::to_string(pid) + kPlayLeaseSuffix;
	};
	std::string played, output_root, executable;
	{
		FakePlatform platform;
		MemoryPreferencesStore preferences;
		ProjectSession session(platform, preferences);
		session.handle(request::new_project(project, "Leases"));
		session.run_operations();
		editor_test::create_missing_files(session);
		session.set_launcher_source(editor_test::fixed_launcher(launcher));
		session.handle(request::play());
		session.run_operations();
		const SessionView &v = session.view();
		TEST_EXPECT(v.activity.play_state == PlayState::Running && v.activity.play_pid == 500);
		played = v.activity.last_build->build_dir;
		output_root = fs::path(played).parent_path().generic_string();
		executable = platform.last_plan.executable;
		std::string text, error;
		opennova::io::JsonValue record;
		TEST_EXPECT(read_file_text(lease_of(played, 500), text, error) && opennova::io::json_parse(text, record, error));
		TEST_EXPECT(record.get_int("schema_version", -1) == kPlayLeaseSchemaVersion && record.get_int("pid", -1) == 500 &&
		            record.get_string("image", "") == executable && record.get_string("created", "") == "created 500" &&
		            record.get_string("build_id", "") == fs::path(played).filename().string());
		// The editor quits; the game runs on (its handle released, never the process).
	}
	// A second game runs from the same build (another editor's), and files that only look like
	// leases lie beside them: an older name, a lease's name over a record that is not one, a
	// record naming another pid than its name, an older record's schema.
	std::string error;
	TEST_EXPECT(write_play_lease({played, 600, executable, "created 600"}, error));
	const std::string build_id = fs::path(played).filename().string();
	const std::vector<std::string> decoys = {output_root + "/" + build_id + kPlayLeaseSuffix,
	                                         output_root + "/" + build_id + ".77" + kPlayLeaseSuffix,
	                                         output_root + "/" + build_id + ".78" + kPlayLeaseSuffix,
	                                         output_root + "/" + build_id + ".79" + kPlayLeaseSuffix};
	const auto record = [&build_id](int schema, int pid) {
		return "{\"schema_version\":" + std::to_string(schema) + ",\"build_id\":\"" + build_id + "\",\"pid\":" +
		       std::to_string(pid) + ",\"created\":\"created " + std::to_string(pid) + "\"}";
	};
	TEST_EXPECT(editor_test::write_text(decoys[0], record(kPlayLeaseSchemaVersion, 78)));
	TEST_EXPECT(editor_test::write_text(decoys[1], "not a lease record"));
	TEST_EXPECT(editor_test::write_text(decoys[2], record(kPlayLeaseSchemaVersion, 79)));
	TEST_EXPECT(editor_test::write_text(decoys[3], record(1, 79)));

	FakePlatform platform;
	platform.next_pid = 900;
	platform.elsewhere = {{500, ProcessLiveness::Alive}, {600, ProcessLiveness::Unknown}};
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::open_project(project));
	session.run_operations();
	session.set_launcher_source(editor_test::fixed_launcher(launcher));
	const SessionView &v = session.view();
	TEST_EXPECT(v.project.open && v.activity.play_state == PlayState::Stopped);
	const auto rebuild = [&](const char *text) {
		editor_test::write_text(v.project.root + "/defs/items.def",
		                        std::string("begin \"Marker\"\nid 100001\ntype marker\nhp ") + text + "\nend\n");
		session.handle(request::build());
		session.run_operations();
		return v.activity.last_build->ok ? v.activity.last_build->build_dir : std::string();
	};
	// Both games may still run: their directory survives the build's prune, both leases kept. The
	// platform was asked about each by the creation time its lease records.
	const std::string second = rebuild("20");
	TEST_EXPECT(!second.empty() && second != played);
	TEST_EXPECT(fs::is_directory(played) && fs::exists(lease_of(played, 500)) && fs::exists(lease_of(played, 600)));
	TEST_EXPECT(platform.asked_created[500] == "created 500" && platform.asked_created[600] == "created 600" &&
	            platform.asked_created.count(78) == 0 && platform.asked_created.count(79) == 0);

	// This editor's game runs from the new build beside another game's lease on it: stopped, its
	// own lease goes and the other stays.
	TEST_EXPECT(write_play_lease({second, 901, executable, "created 901"}, error));
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(v.activity.play_state == PlayState::Running && v.activity.play_pid == 900 && v.activity.last_build->build_dir == second);
	TEST_EXPECT(fs::exists(lease_of(second, 900)) && fs::exists(lease_of(second, 901)));
	session.handle(request::stop_play());
	session.poll();
	TEST_EXPECT(v.activity.play_state == PlayState::Stopped);
	TEST_EXPECT(!fs::exists(lease_of(second, 900)) && fs::exists(lease_of(second, 901)));
	TEST_EXPECT(fs::exists(lease_of(played, 500)) && fs::exists(lease_of(played, 600)));

	// The first game is gone, the second cannot be told: its lease alone keeps the directory. The
	// game on the second build is gone too: its lease is deleted and the build pruned.
	platform.elsewhere = {{600, ProcessLiveness::Unknown}};
	const std::string third = rebuild("30");
	TEST_EXPECT(!third.empty() && fs::is_directory(played));
	TEST_EXPECT(!fs::exists(lease_of(played, 500)) && fs::exists(lease_of(played, 600)));
	TEST_EXPECT(!fs::exists(second) && !fs::exists(lease_of(second, 901)));
	// And once it is gone as well, the next build deletes its lease and prunes its directory.
	platform.elsewhere.clear();
	const std::string fourth = rebuild("40");
	TEST_EXPECT(!fourth.empty() && !fs::exists(played) && !fs::exists(lease_of(played, 600)) && !fs::exists(third));
	for (const std::string &decoy : decoys) TEST_EXPECT(fs::is_regular_file(decoy));
	return 0;
}

// S13 A1: the Output lines keep an absolute index (OutputLog), so a client paging with the last
// page's next_cursor neither skips nor repeats a line while the log drops its oldest past 2,000.
// 2,100 lines of the game's log, read in pages as they come: every one once, in order; a cursor
// below the oldest line held starts at it.
static int test_output_cursor() {
	editor_test::TempProjectDir dir("opennova_editor_session_output");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Output"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string runtime = dir.file("runtime/opennova.exe");
	TEST_EXPECT(editor_test::write_text(runtime, "MZ"));
	PlayLauncher launcher;
	launcher.executable = runtime;
	session.set_launcher_source(editor_test::fixed_launcher(launcher));
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(v.activity.play_state == PlayState::Running);

	uint64_t cursor = v.activity.output.next_index();
	std::vector<std::string> seen;
	const auto read_pages = [&] {
		for (;;) {
			const opennova::io::JsonValue page = output_page_to_json(v.activity.output, cursor, 200);
			const opennova::io::JsonValue *output = &page;
			if (!output->get("lines")) return;
			const auto &lines = output->get("lines")->array;
			for (const opennova::io::JsonValue &line : lines) seen.push_back(line.string);
			cursor = uint64_t(output->get_number("next_cursor", 0.0));
			if (lines.empty()) return;
		}
	};
	std::string log;
	const auto game_says = [&](int from, int to) {
		for (int i = from; i < to; ++i) log += "line " + std::to_string(i) + "\r\n";
		const bool written = editor_test::write_text(platform.last_plan.log_file, log);
		session.poll();
		return written;
	};
	TEST_EXPECT(game_says(0, 1500));
	read_pages();
	TEST_EXPECT(game_says(1500, 2100));
	TEST_EXPECT(v.activity.output.size() == OutputLog::kMaxLines &&
			v.activity.output.first_index() > 0);
	read_pages();
	TEST_EXPECT(seen.size() == 2100);
	bool in_order = seen.size() == 2100;
	for (size_t i = 0; in_order && i < seen.size(); ++i) in_order = seen[i] == "game: line " + std::to_string(i);
	TEST_EXPECT(in_order);
	// A cursor the log dropped past starts at the oldest line held.
	const opennova::io::JsonValue from_zero = output_page_to_json(v.activity.output, 0, 1);
	TEST_EXPECT(from_zero.get_number("first", 0.0) == double(v.activity.output.first_index()) &&
			from_zero.get_number("cursor", 0.0) == double(v.activity.output.first_index()) &&
			from_zero.get_number("next", 0.0) == double(v.activity.output.next_index()) &&
			from_zero.get("lines")->array.front().string == v.activity.output[0]);
	// Clear empties it; the indices go on, so a held cursor still reads what comes next.
	const uint64_t next = v.activity.output.next_index();
	session.handle(request::clear_output());
	TEST_EXPECT(v.activity.output.empty() && v.activity.output.first_index() == next && v.activity.output.next_index() == next);
	return 0;
}

// S13 A1: of two files of one logical name, a Save of the name rewrites the one every other
// request picks (project_file: the path named, else the first of the name, as Files shows it),
// never another; a Save naming the other's path rewrites that one.
static int test_save_picks_like_the_rest() {
	editor_test::TempProjectDir dir("opennova_editor_session_duplicate_save");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Duplicates"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const SessionView &v = session.view();
	const std::string ignored = "begin \"Marker\"\nid 100001\ntype marker\nsubtype Ruins\nhp 10\nend\n";
	TEST_EXPECT(editor_test::write_text(v.project.root + "/defs/items.def", ignored));
	TEST_EXPECT(editor_test::write_text(v.project.root + "/extra/items.def", ignored));
	session.handle(request::rescan());
	session.run_operations();
	TEST_EXPECT(has_code(v.findings.diagnostics, "asset.name.duplicate"));
	const uint64_t before = v.events.next_seq() - 1;
	session.handle(request::show_in_files("items.def"));
	const std::vector<ViewEvent> shown =
			editor_test::events_after(v, before, ViewEventKind::RevealFile);
	TEST_EXPECT(shown.size() == 1);
	const std::string picked = shown.empty() ? std::string() : shown[0].path;
	const std::string other = picked == "defs/items.def" ? "extra/items.def" : "defs/items.def";
	TEST_EXPECT(picked == "defs/items.def" || picked == "extra/items.def");
	session.handle(request::save("items.def"));
	TEST_EXPECT(session.outcome().done() && output_has(v, "Saved " + picked));
	std::string text, error;
	TEST_EXPECT(read_file_text(v.project.root + "/" + picked, text, error) && text.find("subtype") == std::string::npos);
	TEST_EXPECT(read_file_text(v.project.root + "/" + other, text, error) && text == ignored);
	session.handle(request::save(other));
	TEST_EXPECT(session.outcome().done() && read_file_text(v.project.root + "/" + other, text, error) &&
	            text.find("subtype") == std::string::npos);
	return 0;
}

// A request from outside enters handle() once (S13 A2): the session's parts compose by calling one
// another, so a rename everywhere that reads its open documents again, a file's rename that closes
// the renamed document and opens it under its new name, a create that opens its file, a fix's edit
// that opens its document first, an import that rescans and the unsaved prompt's answer that runs
// what waited on it are each one entry, and one outcome.
static int test_handle_entered_once() {
	editor_test::TempProjectDir dir("opennova_editor_session_entries");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	session.handle(request::new_project(dir.file("project"), "Entries"));
	session.run_operations();
	editor_test::create_missing_files(session);
	session.handle(request::open_document("menu_style.mns"));
	session.handle(request::open_document("main.mnu"));
	TEST_EXPECT(session.document_for("menu_style.mns") && session.document_for("main.mnu"));
	uint64_t before = session.handle_entries();

	// A style variable menu_style.mns defines and main.mnu uses, renamed everywhere: both open
	// documents are read again, in the one request.
	const GraphSymbol *variable = nullptr;
	const std::string menu_text = session.document_for("main.mnu")->serialize().text;
	for (const GraphSymbol *symbol : v.findings.graph->symbols_of_kind(ReferenceKind::StyleVar))
		if (!variable && !symbol->inert && symbol->file == session.document_for("menu_style.mns")->path() &&
		    menu_text.find("%" + symbol->display + "%") != std::string::npos)
			variable = symbol;
	TEST_EXPECT(variable);
	session.handle(request::rename_symbol(variable->file, variable->locator, variable->field, "ENTRIES_RENAMED"));
	session.run_operations();
	TEST_EXPECT(session.handle_entries() == before + 1 && session.outcome().done());
	TEST_EXPECT(session.document_for("main.mnu")->serialize().text.find("%ENTRIES_RENAMED%") != std::string::npos);
	before = session.handle_entries();

	// A create that opens its file; then its file renamed while it is the active document: closed
	// and opened again under the new name, still active.
	session.handle(request::create_file("extra.mnu", asset_kind_token(AssetKind::Menu)));
	TEST_EXPECT(session.handle_entries() == before + 1 && session.document_for("extra.mnu"));
	session.handle(request::rename_asset(session.document_for("extra.mnu")->path(), "renamed.mnu"));
	session.run_operations();
	TEST_EXPECT(session.handle_entries() == before + 2 && session.outcome().done());
	TEST_EXPECT(!session.document_for("extra.mnu") && session.document_for("renamed.mnu") &&
	            v.documents.active == session.document_for("renamed.mnu")->path());

	// A fix's edit on a document that is not open opens it first.
	const std::string main = session.document_for("main.mnu")->path();
	session.handle(request::close_document(main));
	TEST_EXPECT(!session.document_for(main));
	before = session.handle_entries();
	EditorRequest fix = request::edit_record(main, Edit());
	fix.open_first = true;
	fix.edits[0].field = "position.left";
	fix.edits[0].value = int64_t(8);
	{
		// The record the fix names, as a load of the file gives it (two loads give a record the
		// same identity: the contract's).
		Diagnostic error;
		const std::shared_ptr<Document> probe =
		        records_of(document_type_for(AssetKind::Menu)->make());
		TEST_EXPECT(probe->load(v.project.root + "/" + main, main, AssetKind::Menu, v.project.document->target_game, error));
		fix.edits[0].address = probe->address_at("0/window:0");
	}
	session.handle(fix);
	TEST_EXPECT(session.handle_entries() == before + 1 && session.document_for(main));

	// The unsaved prompt's answer: the Close it held runs inside the one answer.
	EditorRequest edit = request::edit_record(main, Edit());
	edit.edits[0].address = session.document_for(main)->address_at("0/window:0");
	edit.edits[0].field = "position.left";
	edit.edits[0].value = int64_t(16);
	session.handle(edit);
	TEST_EXPECT(session.document_for(main)->dirty());
	session.handle(request::close_document(main));
	TEST_EXPECT(v.dialogs.unsaved_prompt.open);
	before = session.handle_entries();
	EditorRequest save = request::resolve_unsaved(UnsavedChoice::Save);
	session.handle(save);
	TEST_EXPECT(session.handle_entries() == before + 1 && session.outcome().done() && !v.dialogs.unsaved_prompt.open &&
	            !session.document_for(main));

	// An import that rescans after it writes.
	const std::string loose = dir.file("loose/notes.txt");
	TEST_EXPECT(editor_test::write_text(loose, "notes"));
	std::vector<Diagnostic> diagnostics;
	EditorRequest import = request::of(EditorRequestKind::ImportFiles);
	import.imports = list_import_sources({loose}, diagnostics);
	before = session.handle_entries();
	session.handle(import);
	session.run_operations();
	TEST_EXPECT(session.handle_entries() == before + 1 && session.outcome().done() && v.project.scan->find("notes.txt"));
	return 0;
}

// The port of the game's MCP endpoint is asked of the launcher source when the game is spawned,
// its build landed (S13 A2), never when Play is asked for: a Play whose build a Cancel stops asks
// for none, and each game started gets a port asked then.
static int test_mcp_port_allocated_at_spawn() {
	editor_test::TempProjectDir dir("opennova_editor_session_mcp_port");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	session.handle(request::new_project(dir.file("project"), "Ports"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const std::string runtime = dir.file("runtime/opennova.exe");
	TEST_EXPECT(editor_test::write_text(runtime, "MZ"));
	int asked = 0, ports = 0;
	session.set_launcher_source([&](bool with_mcp_port) {
		++asked;
		PlayLauncher launcher;
		launcher.executable = runtime;
		if (with_mcp_port) launcher.mcp_port = 9100 + ++ports;
		return launcher;
	});
	// Asked once, without a port, for what the view shows.
	TEST_EXPECT(asked == 1 && ports == 0 && v.activity.runtime_executable == runtime);
	session.set_poll_budget({0, 256});

	// Play: the build starts and steps; no port while it packs.
	session.handle(request::play());
	TEST_EXPECT(session.outcome().done() && session.outcome().operation != 0 && v.activity.operation.running());
	TEST_EXPECT(ports == 0);
	session.poll();
	TEST_EXPECT(v.activity.operation.running() && ports == 0 && platform.spawns == 0);
	// It lands: the port is asked for now, and the game started on it.
	session.run_operations();
	TEST_EXPECT(platform.spawns == 1 && ports == 1 && platform.last_plan.mcp_port == 9101 && v.activity.play_mcp_port == 9101);
	session.handle(request::stop_play());
	session.poll();
	TEST_EXPECT(v.activity.play_state == PlayState::Stopped && v.activity.play_mcp_port == 0);

	// A Play whose build is cancelled asks for no port.
	TEST_EXPECT(editor_test::write_text(v.project.root + "/notes.txt", "a new file: the next build packs again"));
	session.handle(request::rescan());
	session.run_operations();
	session.handle(request::play());
	TEST_EXPECT(v.activity.operation.running());
	session.handle(request::cancel_operation());
	TEST_EXPECT(!v.activity.operation.running());
	session.run_operations();
	TEST_EXPECT(ports == 1 && platform.spawns == 1);

	// The next Play: a port of its own, asked when its game starts.
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 2 && ports == 2 && platform.last_plan.mcp_port == 9102 && v.activity.play_mcp_port == 9102);
	return 0;
}

// Play launches from one answer of the launcher source, asked when the game is spawned (S13 A2):
// the executable, whether the run drives the source checkout, its Godot options and the port all
// come from it, and the view's runtime follows it, whatever the source answered when it was set.
static int test_play_launches_one_answer() {
	editor_test::TempProjectDir dir("opennova_editor_session_one_launcher");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	session.handle(request::new_project(dir.file("project"), "Launcher"));
	session.run_operations();
	editor_test::create_missing_files(session);
	const std::string first = dir.file("runtime/first.exe"), second = dir.file("runtime/second.exe");
	TEST_EXPECT(editor_test::write_text(first, "MZ") && editor_test::write_text(second, "MZ"));
	std::string executable = first;
	std::vector<std::string> args = {"--first"};
	session.set_launcher_source([&](bool with_mcp_port) {
		PlayLauncher launcher;
		launcher.executable = executable;
		launcher.engine_args = args;
		if (with_mcp_port) launcher.mcp_port = 9200;
		return launcher;
	});
	TEST_EXPECT(v.activity.runtime_executable == first);
	// What the source answers changes before the game starts: the game is launched from the
	// answer it gives then, all of it, and the view shows that runtime.
	executable = second;
	args = {"--second"};
	const uint64_t preferences_before = v.revisions.of(ViewConcern::Preferences);
	session.handle(request::play());
	session.run_operations();
	TEST_EXPECT(platform.spawns == 1 && platform.last_plan.executable == second && !platform.last_plan.args.empty() &&
	            platform.last_plan.args[0] == "--second" && platform.last_plan.mcp_port == 9200);
	TEST_EXPECT(v.activity.runtime_executable == second && v.revisions.of(ViewConcern::Preferences) > preferences_before);
	return 0;
}

#ifdef NDEBUG
// A request that reaches the session while another is served (a device's callback: here the process
// seam's can_spawn, asked by a Play) is served inside it (S13 A2; a debug build asserts instead): it
// neither empties nor ends the outer request's outcome, whose later findings are still its own.
static int test_reentry_keeps_the_outer_outcome() {
	struct Reentrant : editor_test::NoProcess {
		ProjectSession *session = nullptr;
		bool can_spawn() const override {
			if (session) session->handle(request::clear_output());
			return false;
		}
	};
	editor_test::TempProjectDir dir("opennova_editor_session_reentry");
	Reentrant platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Reentry"));
	session.run_operations();
	platform.session = &session;
	const uint64_t before = session.handle_entries();
	session.handle(request::play());
	platform.session = nullptr;
	TEST_EXPECT(session.handle_entries() == before + 2);
	TEST_EXPECT(!session.outcome().done() && has_code(session.outcome().findings, "play.unsupported"));
	return 0;
}
#endif

// Every one-shot ask a request makes of a window is one view event (S13 V4), with its kind and
// the fields the window needs, where the view kept a serial: a Problems row's OpenDocument naming
// a record's field posts a RevealRecord (the document, the record, the field), each ask again
// another and one naming no field none; a Go to by locator too; a ShowInFiles with its rename a
// RevealFile (the file its name finds, the flag); a PreviewRename that asks the new name an
// AskRename (the defining file, the field, the preview's serial), one that does not none; an
// ApplyProjectSettings a SettingsApplied (the request's serial, flagged when a setting could not
// be written); a PlanImport an ImportPlanned. Opening, selecting and cancelling post none.
static int test_view_events() {
	editor_test::TempProjectDir dir("opennova_editor_session_view_events");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	const SessionView &v = session.view();
	session.handle(request::new_project(dir.file("project"), "Events"));
	session.run_operations();
	editor_test::create_missing_files(session);
	// The events the last requests posted: every one after `seen`, which moves past them.
	uint64_t seen = v.events.next_seq() - 1;
	const auto posted = [&]() {
		std::vector<ViewEvent> out;
		for (const ViewEvent &event : v.events.held())
			if (event.seq > seen)
				out.push_back(event);
		seen = v.events.next_seq() - 1;
		return out;
	};
	session.handle(request::open_document("main.mnu"));
	const Document *menu = session.document_for("main.mnu");
	TEST_EXPECT(menu && posted().empty());
	if (!menu)
		return 1;
	const NodeAddress window = menu->address_at("0/window:0");
	TEST_EXPECT(window.child != 0);

	// A Problems row's OpenDocument: its record selected, its field shown.
	EditorRequest row = request::open_record(menu->path(), window, "position.left");
	session.handle(row);
	std::vector<ViewEvent> events = posted();
	TEST_EXPECT(events.size() == 1 && events[0].kind == ViewEventKind::RevealRecord &&
			events[0].path == menu->path() && events[0].address == window &&
			events[0].field == "position.left" && !events[0].flag && events[0].tag == 0 &&
			v.documents.selection.primary == window);
	// The same row clicked again: another event, the next seq.
	const uint64_t first = events.empty() ? 0 : events[0].seq;
	session.handle(row);
	events = posted();
	TEST_EXPECT(events.size() == 1 && events[0].kind == ViewEventKind::RevealRecord &&
			events[0].field == "position.left" && events[0].seq == first + 1);
	// A record named with no field: selected, nothing to show.
	row.field.clear();
	session.handle(row);
	TEST_EXPECT(posted().empty() && v.documents.selection.primary == window);
	// A Go to by locator, its defining field shown.
	session.handle(request::open_document(menu->path(), "0/window:0", "name"));
	events = posted();
	TEST_EXPECT(events.size() == 1 && events[0].address == window && events[0].field == "name");

	// ShowInFiles with its rename: the file its name finds, by its path.
	session.handle(request::show_in_files("main.mnu", true));
	events = posted();
	TEST_EXPECT(events.size() == 1 && events[0].kind == ViewEventKind::RevealFile &&
			events[0].path == menu->path() && events[0].flag && events[0].field.empty() &&
			!events[0].address.row);

	// PreviewRename that asks the new name: the preview it asks from, by its serial.
	const GraphSymbol *screen = nullptr;
	for (const GraphSymbol *symbol : v.findings.graph->symbols_of_kind(ReferenceKind::MenuScreen))
		if (!screen && symbol->file == menu->path())
			screen = symbol;
	TEST_EXPECT(screen != nullptr);
	if (!screen)
		return 1;
	EditorRequest ask =
			request::preview_rename(screen->file, screen->locator, screen->field, "OPENING", true);
	session.handle(ask);
	events = posted();
	TEST_EXPECT(events.size() == 1 && events[0].kind == ViewEventKind::AskRename &&
			events[0].path == menu->path() && events[0].field == screen->field &&
			events[0].tag == v.dialogs.rename_preview.serial && v.dialogs.rename_preview.symbol &&
			v.dialogs.rename_preview.requested == "OPENING");
	// The dialog's own previews as the name is typed ask nothing.
	ask.ask_name = false;
	ask.new_name = "OPENED";
	session.handle(ask);
	TEST_EXPECT(posted().empty() && v.dialogs.rename_preview.requested == "OPENED");

	// ApplyProjectSettings: its serial back, flagged when a setting could not be written.
	ProjectSettingsChange settings;
	settings.serial = 41;
	settings.title = std::string("Events Renamed");
	EditorRequest apply = request::apply_project_settings(settings);
	session.handle(apply);
	events = posted();
	TEST_EXPECT(events.size() == 1 && events[0].kind == ViewEventKind::SettingsApplied &&
			events[0].tag == 41 && !events[0].flag && events[0].path.empty() &&
			v.project.settings_result.failures.empty());
	apply.settings.serial = 42;
	apply.settings.title = std::string("");
	session.handle(apply);
	events = posted();
	TEST_EXPECT(events.size() == 1 && events[0].tag == 42 && events[0].flag &&
			v.project.settings_result.failures.size() == 1);

	// PlanImport: the import dialog's plan made, its checks to take again; a cancel posts none.
	const std::string loose = dir.file("loose/notes.txt");
	TEST_EXPECT(editor_test::write_text(loose, "notes"));
	std::vector<Diagnostic> diagnostics;
	session.handle(request::plan_import(list_import_sources({ loose }, diagnostics), false));
	session.run_operations();
	events = posted();
	TEST_EXPECT(events.size() == 1 && events[0].kind == ViewEventKind::ImportPlanned &&
			!events[0].flag && v.dialogs.import_preview.open);
	session.handle(request::cancel_import());
	TEST_EXPECT(posted().empty() && !v.dialogs.import_preview.open);
	return 0;
}

// The unsaved prompt's words are the request table's (S13 A4: waiting_action and the Save
// labels in editor_windows.cpp were switches): for every kind the prompt guards, what waits (the
// file the request names for the rows whose words name one) and the Save button say what they
// said; a kind it does not guard has none.
static int test_prompt_words_from_the_table() {
	struct Words {
		EditorRequestKind kind;
		const char *waiting;
		const char *save;
	};
	const Words kOld[] = {
		{EditorRequestKind::NewProject, "Create a new project", "Save all"},
		{EditorRequestKind::OpenProject, "Open another project", "Save all"},
		{EditorRequestKind::CloseProject, "Close the project", "Save all"},
		{EditorRequestKind::ImportFiles, "Import", "Save all and import"},
		{EditorRequestKind::Build, "Build", "Save all and build"},
		{EditorRequestKind::Play, "Play", "Save all and play"},
		{EditorRequestKind::ReloadDocument, "Reload main.mnu", "Save"},
		{EditorRequestKind::CloseDocument, "Close main.mnu", "Save"},
		{EditorRequestKind::RenameAsset, "Rename main.mnu", "Save all and rename"},
		{EditorRequestKind::AssignRequirement, "Rename main.mnu", "Save all and rename"},
		{EditorRequestKind::RenameSymbol, "Rename everywhere (defined in main.mnu)", "Save all and rename"},
		{EditorRequestKind::Quit, "Quit", "Save all"},
	};
	size_t guarded = 0;
	for (size_t i = 0; i < kEditorRequestKindCount; ++i) {
		const auto kind = static_cast<EditorRequestKind>(i);
		const RequestKindRow &row = request_kind_row(kind);
		const Words *old = nullptr;
		for (const Words &words : kOld)
			if (words.kind == kind) old = &words;
		TEST_EXPECT((row.guard != GuardScope::None) == (old != nullptr));
		if (!old) {
			TEST_EXPECT(!row.waiting && !row.save_label && waiting_words(kind, "menus/main.mnu").empty());
			continue;
		}
		++guarded;
		TEST_EXPECT(waiting_words(kind, "menus/main.mnu") == old->waiting && row.save_label &&
		            std::string(row.save_label) == old->save);
	}
	TEST_EXPECT(guarded == std::size(kOld));
	return 0;
}

// No request runs the validation (S13 A3): an edit leaves it due, and a request after it leaves it
// due too, whatever it is: a selection, a file rename's preview, set_import_dependencies with no
// preview open and with one, import_files with none, assign_requirement refused before its
// rename, an import's preview; the polls run it (an operation that reads the graph joins it: the
// import's plan steps it first), once. Every request that starts an operation ends the edit
// groups first, so no gesture holds the validation it joins; set_import_dependencies with a
// preview open plans it again whatever the setting was.
static int test_no_request_validates() {
	editor_test::TempProjectDir dir("opennova_editor_session_validates");
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Validates"));
	session.run_operations();
	editor_test::create_missing_files(session);
	session.handle(request::open_document("items.def"));
	session.run_operations();
	Document *items = session.document_for("items.def");
	TEST_EXPECT(items != nullptr && !items->rows().empty());
	if (!items || items->rows().empty()) return 1;
	const ValidationStats &stats = session.validation_stats();
	const SessionView &v = session.view();
	const NodeAddress marker{items->rows()[0]->id, items->rows()[0]->kind, 0};
	// An edit leaves the validation due (each one sets the type the last did not); the request
	// leaves it due; the operations and the polls run it, once.
	int64_t type = 4;
	const auto after_an_edit = [&](const EditorRequest &request) {
		type = type == 4 ? 0 : 4;
		Edit set;
		set.address = marker;
		set.field = "type";
		set.value = type;
		session.handle(request::edit_record(items->path(), set));
		const size_t passes = stats.passes;
		session.handle(request);
		const bool left = stats.passes == passes && v.activity.validation.running; // shown due at once
		session.run_operations();
		return left && stats.passes == passes + 1 && !v.activity.validation.running;
	};
	TEST_EXPECT(after_an_edit(request::select_record(items->path(), marker)));
	TEST_EXPECT(after_an_edit(request::preview_file_rename(items->path(), "things.def")));
	TEST_EXPECT(v.dialogs.rename_preview.serial != 0);
	// No preview open: the setting written, nothing planned.
	TEST_EXPECT(!v.dialogs.import_preview.open);
	TEST_EXPECT(after_an_edit(request::set_import_dependencies(false)));
	TEST_EXPECT(!v.project.import_dependencies && !v.dialogs.import_preview.open);
	TEST_EXPECT(after_an_edit(request::import_files({})));
	TEST_EXPECT(v.activity.status == "Nothing to import." && !v.dialogs.import_preview.open);
	// Refused before its rename (no requirement has the role).
	TEST_EXPECT(after_an_edit(request::assign_requirement("no_such_role", items->path())));
	TEST_EXPECT(has_code(session.outcome().findings, "requirement.unknown"));
	// A preview open, then its setting set to what it was: planned again all the same.
	const std::string loose = dir.file("loose.txt");
	TEST_EXPECT(editor_test::write_text(loose, "loose"));
	TEST_EXPECT(after_an_edit(request::preview_import({ loose }, false)));
	TEST_EXPECT(v.dialogs.import_preview.open && !v.dialogs.import_preview.with_dependencies);
	const uint64_t planned = v.activity.last_operation.id;
	TEST_EXPECT(after_an_edit(request::set_import_dependencies(false)));
	TEST_EXPECT(v.dialogs.import_preview.open && !v.dialogs.import_preview.with_dependencies &&
	            v.activity.last_operation.id > planned &&
	            v.activity.last_operation.kind == OperationKind::ImportPlan && !v.dialogs.import_preview.plan->rows.empty());
	// Every request that starts an operation ends the edit groups first.
	for (const EditorRequestKind kind :
	     {EditorRequestKind::NewProject, EditorRequestKind::OpenProject, EditorRequestKind::Rescan,
	      EditorRequestKind::Reimport, EditorRequestKind::PreviewImport, EditorRequestKind::PlanImport,
	      EditorRequestKind::PreviewInstallImport, EditorRequestKind::ImportFiles, EditorRequestKind::RenameAsset,
	      EditorRequestKind::AssignRequirement, EditorRequestKind::RenameSymbol, EditorRequestKind::Build,
	      EditorRequestKind::Play})
		TEST_EXPECT(request_kind_row(kind).ends_edit_groups);
	return 0;
}

int main() {
	int failures = 0;
	failures += test_prompt_words_from_the_table();
	failures += test_no_request_validates();
	failures += test_play_launches_one_answer();
#ifdef NDEBUG
	failures += test_reentry_keeps_the_outer_outcome();
#endif
	failures += test_handle_entered_once();
	failures += test_view_events();
	failures += test_mcp_port_allocated_at_spawn();
	failures += test_play_then_close();
	failures += test_play_leases();
	failures += test_output_cursor();
	failures += test_save_picks_like_the_rest();
	failures += test_view_revisions();
	failures += test_import_guard_past_the_cap();
	failures += test_build_findings_stay();
	failures += test_boot_findings();
	failures += test_optional_rows();
	failures += test_create_missing_roles();
	failures += test_rewrite_closed_file();
	failures += test_fixes_apply();
	failures += test_import_fix_plans_dependencies();
	failures += test_save_contract();
	failures += test_unsaved_prompt();
	failures += test_prompt_saves_what_it_lists();
	failures += test_prompt_renews();
	failures += test_prompt_belongs_to_its_project();
	failures += test_selection_memory();
	failures += test_import_dependencies_setting();
	failures += test_retail_play();
	failures += test_import();
	failures += test_lifecycle();
	failures += test_outcomes_and_refusals();
	failures += test_validation_cost();
	failures += test_requests_that_cannot_run();
	failures += test_rename_keeps_the_active_document();
	failures += test_preview_target();
	failures += test_project_settings();
	failures += test_menu_first_screen();
	failures += test_rescan_keeps_what_did_not_change();
	if (failures == 0) std::printf("editor_session: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
