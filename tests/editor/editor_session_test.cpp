// Pins the project session (ADR 0046 d10) over a fake process platform: the request
// kinds it serves and the ones it leaves to the shell, the view it rewrites (a new
// project's checklist, create-missing, the stepped build, Play on a good build, the
// child's exit, the log tail), the editor settings it keeps, and the feature toggle
// that changes the checklist.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <editor/session/project_session.h>
#include <editor/project/project_files.h>
#include <formats/pff/pff.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

struct FakePlatform : ProcessPlatform {
	int64_t next_pid = 500;
	int64_t clock = 0;
	bool spawn_fails = false;
	std::vector<int64_t> running;
	LaunchPlan last_plan;
	int spawns = 0;

	int64_t spawn(const LaunchPlan &plan) override {
		last_plan = plan;
		++spawns;
		if (spawn_fails) return -1;
		running.push_back(next_pid);
		return next_pid++;
	}
	bool is_running(int64_t pid) override {
		for (int64_t p : running) if (p == pid) return true;
		return false;
	}
	bool terminate(int64_t pid) override { exit_child(pid); return true; }
	bool kill(int64_t pid) override { exit_child(pid); return true; }
	void release(int64_t) override {}
	int64_t now_ms() override { return clock; }
	void sleep_ms(int64_t ms) override { clock += ms; }
	void exit_child(int64_t pid) {
		for (size_t i = 0; i < running.size(); ++i)
			if (running[i] == pid) running.erase(running.begin() + static_cast<std::ptrdiff_t>(i));
	}
};

static bool output_has(const SessionView &v, const std::string &needle) {
	for (const std::string &line : v.output)
		if (line.find(needle) != std::string::npos) return true;
	return false;
}

static int test_lifecycle() {
	editor_test::TempProjectDir dir("opennova_editor_session_test");
	FakePlatform platform;
	ProjectSession session(platform, dir.file("settings/editor.json"));
	TEST_EXPECT(!session.project_open());
	TEST_EXPECT(session.view().recent_projects.empty());

	// The shell-only kinds are declined; the portable ones served.
	EditorRequest pick = make_request(EditorRequestKind::PickDirectory);
	pick.purpose = PickPurpose::OpenProject;
	TEST_EXPECT(!session.handle(pick));
	TEST_EXPECT(session.handle(make_request(EditorRequestKind::Quit)));
	TEST_EXPECT(session.handle(make_request(EditorRequestKind::Build))); // no project: nothing happens
	TEST_EXPECT(!session.build_running());

	// New project: open, listed as recent, the checklist all unmet.
	const std::string root = dir.file("My Game");
	TEST_EXPECT(session.handle(make_request(EditorRequestKind::NewProject, root, "My Game")));
	TEST_EXPECT(session.project_open());
	const SessionView &v = session.view();
	TEST_EXPECT(v.document.title == "My Game");
	TEST_EXPECT(v.project_root == root);
	TEST_EXPECT(v.requirements.required_total > 0);
	TEST_EXPECT(v.requirements.required_missing == v.requirements.required_total);
	TEST_EXPECT(v.recent_projects.size() == 1 && v.recent_projects[0] == root);
	TEST_EXPECT(fs::is_regular_file(dir.file("settings/editor.json")));
	TEST_EXPECT(!v.diagnostics.empty()); // one per unmet required row

	// A build on the unmet project is refused, and Play with it.
	const uint64_t before = v.revision;
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(v.revision > before);
	TEST_EXPECT(v.has_build && !v.last_build.ok);
	TEST_EXPECT(v.play_state == PlayState::Stopped && platform.spawns == 0);
	TEST_EXPECT(output_has(v, "Build failed"));

	// Create all missing: the checklist clears.
	session.handle(make_request(EditorRequestKind::CreateMissing));
	TEST_EXPECT(v.requirements.required_missing == 0 && v.requirements.required_wrong_kind == 0);
	TEST_EXPECT(!v.scan.entries.empty());
	TEST_EXPECT(output_has(v, "Created menus/main.mnu"));

	// Build steps once per poll; the view shows progress until it lands.
	session.handle(make_request(EditorRequestKind::Build));
	TEST_EXPECT(session.build_running() && v.build_running);
	size_t polls = 0;
	while (session.build_running()) {
		session.poll();
		++polls;
		TEST_EXPECT(polls < 100);
	}
	TEST_EXPECT(polls > 2); // prepare, three archives, the loose file, publish
	TEST_EXPECT(v.has_build && v.last_build.ok && !v.build_running);
	TEST_EXPECT(fs::is_regular_file(fs::path(v.last_build.build_dir) / "localres.pff"));

	// Play: no runtime set and none beside a fake editor -> a plain problem, no spawn.
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 0);
	TEST_EXPECT(v.diagnostics.back().code == "play.runtime_missing");

	// With a runtime the launcher names, Play builds (unchanged) and spawns on it.
	const std::string runtime = dir.file("runtime/opennova.exe");
	TEST_EXPECT(editor_test::write_text(runtime, "MZ"));
	PlayLauncher launcher;
	launcher.executable = runtime;
	launcher.mcp_port = 8999;
	launcher.engine_args = {"--headless"};
	session.set_launcher(launcher);
	TEST_EXPECT(v.runtime_executable == runtime);
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 1);
	TEST_EXPECT(v.play_state == PlayState::Running && v.play_pid == 500);
	TEST_EXPECT(platform.last_plan.working_dir == v.last_build.build_dir);
	TEST_EXPECT(platform.last_plan.args[0] == "--headless");
	TEST_EXPECT(platform.last_plan.mcp_port == 8999);
	TEST_EXPECT(session.running_build_dir() == v.last_build.build_dir);
	TEST_EXPECT(output_has(v, "Running: "));

	// A second Play while running is refused; the game's log is tailed line by line.
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 1);
	TEST_EXPECT(v.diagnostics.back().code == "play.already_running");
	TEST_EXPECT(editor_test::write_text(platform.last_plan.log_file, "Godot Engine v4.6.1\r\nhalf"));
	session.poll();
	TEST_EXPECT(output_has(v, "game: Godot Engine v4.6.1"));
	TEST_EXPECT(!output_has(v, "game: half"));

	// The child exits on its own: the view says so on the next poll.
	platform.exit_child(500);
	session.poll();
	TEST_EXPECT(v.play_state == PlayState::Stopped && v.play_exited_on_its_own);
	TEST_EXPECT(session.running_build_dir().empty());

	// Stop: terminate, then the deadline kill if ignored (the fake exits on terminate).
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(v.play_state == PlayState::Running);
	session.handle(make_request(EditorRequestKind::StopPlay));
	session.poll();
	TEST_EXPECT(v.play_state == PlayState::Stopped && !v.play_exited_on_its_own);

	// The mission feature widens the checklist and is saved to the project file.
	const int before_rows = static_cast<int>(v.requirements.rows.size());
	EditorRequest feature = make_request(EditorRequestKind::SetFeature, std::string(), "mission");
	feature.flag = true;
	session.handle(feature);
	TEST_EXPECT(static_cast<int>(v.requirements.rows.size()) > before_rows);
	TEST_EXPECT(v.document.features.mission);
	ProjectDocument reloaded;
	Diagnostic error;
	TEST_EXPECT(::opennova::editor::open_project(root, reloaded, error) && reloaded.features.mission);
	session.handle(make_request(EditorRequestKind::SetTitle, std::string(), "Renamed"));
	TEST_EXPECT(::opennova::editor::open_project(root, reloaded, error) && reloaded.title == "Renamed");

	// Close, forget, reopen from the settings file with a fresh session.
	session.handle(make_request(EditorRequestKind::CloseProject));
	TEST_EXPECT(!session.project_open() && v.requirements.rows.empty());
	{
		FakePlatform other;
		ProjectSession again(other, dir.file("settings/editor.json"));
		TEST_EXPECT(again.view().recent_projects.size() == 1);
		TEST_EXPECT(again.handle(make_request(EditorRequestKind::OpenProject, root)));
		TEST_EXPECT(again.view().document.title == "Renamed");
		TEST_EXPECT(again.view().requirements.required_missing > 0); // the mission rows
		again.handle(make_request(EditorRequestKind::ForgetRecent, root));
		TEST_EXPECT(again.view().recent_projects.empty());
	}
	// A vanished recent project is dropped from the list when opening it fails.
	{
		FakePlatform other;
		ProjectSession again(other, dir.file("settings/editor.json"));
		TEST_EXPECT(!again.handle(make_request(EditorRequestKind::OpenProject, dir.file("nowhere"))) ||
		            !again.project_open());
	}
	return 0;
}

static int test_import() {
	editor_test::TempProjectDir dir("opennova_editor_import_test");
	FakePlatform platform;
	ProjectSession session(platform, dir.file("settings.json"));
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Imports"));
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
	EditorRequest preview = make_request(EditorRequestKind::PreviewImport);
	preview.paths = {loose, packed};
	session.handle(preview);
	TEST_EXPECT(session.view().import_open && session.view().import_sources.size() == 3);
	session.handle(make_request(EditorRequestKind::CancelImport));
	TEST_EXPECT(!session.view().import_open && session.view().import_sources.empty());
	session.handle(preview);
	EditorRequest importing = make_request(EditorRequestKind::ImportFiles);
	importing.imports = {{loose, {}}, {packed, "note.txt"}};
	session.handle(importing);
	TEST_EXPECT(!session.view().import_open);
	TEST_EXPECT(session.view().scan.find("loose.txt") && session.view().scan.find("note.txt"));
	TEST_EXPECT(!session.view().scan.find("unused.txt"));
	std::string text, error;
	TEST_EXPECT(read_file_text(dir.file("project/note.txt"), text, error) && text == "packed");
	TEST_EXPECT(read_file_text(loose, text, error) && text == "loose file");

	// Replacing is explicit and uses the existing project's path and spelling.
	fs::create_directory(dir.file("project/custom"));
	fs::rename(dir.file("project/note.txt"), dir.file("project/custom/NOTE.TXT"));
	TEST_EXPECT(editor_test::write_text(dir.file("project/custom/NOTE.TXT"), "authored"));
	importing.imports = {{packed, "note.txt"}};
	session.handle(importing);
	TEST_EXPECT(session.view().diagnostics.back().code == "import.exists");
	TEST_EXPECT(read_file_text(dir.file("project/custom/NOTE.TXT"), text, error) && text == "authored");
	importing.flag = true;
	session.handle(importing);
	TEST_EXPECT(read_file_text(dir.file("project/custom/NOTE.TXT"), text, error) && text == "packed");
	TEST_EXPECT(!fs::exists(dir.file("project/note.txt")));

	// Import never discards an unsaved catalog. A clean open document reloads after replacement.
	const std::string items = dir.file("items.def");
	TEST_EXPECT(editor_test::write_text(items, "begin \"Marker\"\nid 100001\ntype marker\nhp 10\nend\n"));
	importing.imports = {{items, {}}};
	session.handle(importing);
	session.handle(make_request(EditorRequestKind::OpenDocument, "items.def"));
	TEST_EXPECT(session.view().documents.size() == 1);
	EditorRequest edit = make_request(EditorRequestKind::EditRecord, "defs/items.def");
	edit.catalog_edit.address = {session.view().documents[0]->rows()[0]->id, opennova::def::DefRecordKind::Item, 0};
	edit.catalog_edit.field = "hp"; edit.catalog_edit.value = int64_t(20);
	session.handle(edit);
	TEST_EXPECT(session.documents_dirty());
	TEST_EXPECT(editor_test::write_text(items, "begin \"Marker\"\nid 100001\ntype marker\nhp 30\nend\n"));
	session.handle(importing);
	TEST_EXPECT(session.view().diagnostics.back().code == "import.unsaved");
	TEST_EXPECT(read_file_text(dir.file("project/defs/items.def"), text, error) && text.find("hp 10") != std::string::npos);
	session.handle(make_request(EditorRequestKind::Undo));
	session.handle(importing);
	TEST_EXPECT(std::get<opennova::def::DefItemDef>(session.view().documents[0]->rows()[0]->data).hp == 30);

	const ProjectPaths paths = ProjectPaths::for_root(dir.file("project"));
	const auto invalid = import_assets({{packed, "../escape.txt"}, {packed, "absent.txt"}},
	                                  paths, session.view().document, false);
	TEST_EXPECT(invalid.imported.empty() && invalid.diagnostics.size() == 2);
	TEST_EXPECT(invalid.diagnostics[0].code == "import.name");
	TEST_EXPECT(!fs::exists(dir.file("escape.txt")));
	const auto duplicates = import_assets({{loose, {}}, {loose, {}}}, paths, session.view().document, true);
	TEST_EXPECT(duplicates.imported.size() == 1 && duplicates.diagnostics.size() == 1);
	TEST_EXPECT(duplicates.diagnostics[0].code == "import.duplicate");
	TEST_EXPECT(editor_test::write_text(dir.file("bad.pff"), "not an archive"));
	preview.paths = {dir.file("bad.pff"), dir.file("missing.txt")};
	session.handle(preview);
	TEST_EXPECT(!session.view().import_open);
	return 0;
}

static int test_retail_play() {
	editor_test::TempProjectDir dir("opennova_editor_retail_play_test");
	FakePlatform platform;
	ProjectSession session(platform, dir.file("settings.json"));
	TEST_EXPECT(!session.view().play_retail && session.view().retail_directory.empty());
	session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Retail test"));
	session.handle(make_request(EditorRequestKind::CreateMissing));
	EditorRequest retail = make_request(EditorRequestKind::SetPlayRetail);
	retail.flag = true;
	session.handle(retail);
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 0 && session.view().diagnostics.back().code == "play.retail_missing");

	const std::string install = dir.file("retail install");
	TEST_EXPECT(editor_test::write_text(install + "/Jointops.exe", "retail executable"));
	TEST_EXPECT(editor_test::write_text(install + "/binkw32.dll", "ordinary Bink"));
	session.handle(make_request(EditorRequestKind::SetRetailDirectory, install));
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 0);
	TEST_EXPECT(session.view().diagnostics.back().message.find("game.cfg") != std::string::npos);
	const std::string built = session.view().last_build.build_dir;
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
	session.set_launcher(launcher);
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 1 && session.view().play_state == PlayState::Running);
	TEST_EXPECT(platform.last_plan.executable == built + "/Jointops.exe");
	TEST_EXPECT(platform.last_plan.working_dir == built && session.running_build_dir() == built);
	TEST_EXPECT(platform.last_plan.args == std::vector<std::string>({"/w", "/d", "/FRISK"}));
	TEST_EXPECT(platform.last_plan.mcp_port == 0 && platform.last_plan.log_file == built + "/_filelog.txt");
	std::string copied;
	TEST_EXPECT(read_file_text(built + "/binkw32.dll", copied, io_error) && copied == "real JOTAC Bink");
	TEST_EXPECT(read_file_text(built + "/game.cfg", copied, io_error) && copied == "video settings");
	{
		FakePlatform other;
		ProjectSession reopened(other, dir.file("settings.json"));
		TEST_EXPECT(reopened.view().play_retail && reopened.view().retail_directory == install);
	}
	session.handle(make_request(EditorRequestKind::Build));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 1); // Build stays a build with the retail checkbox checked
	session.handle(make_request(EditorRequestKind::StopPlay));
	session.poll();
	TEST_EXPECT(session.view().play_state == PlayState::Stopped);

	// Ordinary installs use the plain Bink DLL. A missing source cannot launch the
	// staged executable left from the successful run.
	fs::remove(fs::path(install) / "binkw32_.dll");
	TEST_EXPECT(editor_test::write_text(built + "/game.cfg", "project video settings"));
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 2);
	TEST_EXPECT(read_file_text(built + "/binkw32.dll", copied, io_error) && copied == "ordinary Bink");
	TEST_EXPECT(read_file_text(built + "/game.cfg", copied, io_error) && copied == "project video settings");
	session.handle(make_request(EditorRequestKind::StopPlay));
	session.poll();
	fs::remove(fs::path(install) / "Jointops.exe");
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 2 && session.view().play_state == PlayState::Stopped);

	// A copy failure is also reported before any child starts.
	TEST_EXPECT(editor_test::write_text(install + "/Jointops.exe", "retail executable"));
	fs::remove(fs::path(built) / "binkw32.dll");
	fs::create_directory(fs::path(built) / "binkw32.dll");
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 2 && session.view().diagnostics.back().code == "play.retail_copy");

	retail.flag = false;
	session.handle(retail);
	session.handle(make_request(EditorRequestKind::Play));
	session.finish_build();
	TEST_EXPECT(platform.spawns == 3 && platform.last_plan.executable == launcher.executable);
	TEST_EXPECT(platform.last_plan.args[0] == "--path" && platform.last_plan.mcp_port == 8999);
	TEST_EXPECT(read_file_bytes(built + "/localres.pff", archive_after, io_error) && archive_after == archive_before);
	TEST_EXPECT(read_file_text(install + "/game.cfg", copied, io_error) && copied == "video settings");
	return 0;
}

static int test_editor_settings() {
	editor_test::TempProjectDir dir("opennova_editor_settings_test");
	EditorSettings settings;
	for (int i = 0; i < 12; ++i) remember_recent_project(settings, "p" + std::to_string(i));
	TEST_EXPECT(settings.recent_projects.size() == kRecentProjectsMax);
	TEST_EXPECT(settings.recent_projects.front() == "p11");
	remember_recent_project(settings, "p5"); // moves to the front, no duplicate
	TEST_EXPECT(settings.recent_projects.front() == "p5" && settings.recent_projects.size() == kRecentProjectsMax);
	settings.runtime_executable = "C:/tools/opennova.exe";
	Diagnostic error;
	TEST_EXPECT(save_editor_settings(dir.file("a/b/editor.json"), settings, error));
	EditorSettings loaded;
	TEST_EXPECT(load_editor_settings(dir.file("a/b/editor.json"), loaded, error));
	TEST_EXPECT(loaded.recent_projects == settings.recent_projects);
	TEST_EXPECT(loaded.runtime_executable == settings.runtime_executable);
	TEST_EXPECT(load_editor_settings(dir.file("missing.json"), loaded, error) && loaded.recent_projects.empty());
	TEST_EXPECT(editor_test::write_text(dir.file("bad.json"), "{\"schema_version\": 99}"));
	TEST_EXPECT(!load_editor_settings(dir.file("bad.json"), loaded, error));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_editor_settings();
	failures += test_retail_play();
	failures += test_import();
	failures += test_lifecycle();
	if (failures == 0) std::printf("editor_session: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
