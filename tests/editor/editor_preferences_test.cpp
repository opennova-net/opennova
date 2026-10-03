// Pins the editor's preferences over their store (ADR 0046 S13 A2): the memory store gives back
// every preference it was given; the file store writes the settings file, byte for byte, with the
// game install's keys S13 A4 renamed and the recently placed items kept per game (the polish) under
// schema 3, and sets aside a file of another schema, an older editor's among them (no reader for it:
// pre-1.0; S15's schema 2 with its one list of items too), read as the defaults with a warning
// naming everything it held and written again by the next save; EditorPreferences reads a store
// once, writes a change from a copy (a change the store refuses leaves the values in effect), and
// keeps the recent-projects list and each game's recently placed items capped, most recent first;
// and a session over each store shows them, a project's game's items alone.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <editor/session/editor_preferences.h>
#include <editor/session/file_preferences_store.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

// Every preference set away from its default.
Preferences every_preference() {
	Preferences preferences;
	preferences.recent_projects = {"C:/games/Armory", "D:/mods/Harbor"};
	preferences.runtime_executable = "C:/tools/opennova.exe";
	preferences.game_install = "D:/Joint Operations";
	preferences.play_in_install = true;
	preferences.import_dependencies = false;
	preferences.recent_items = {{"jo", {106100, 2044}}, {"dfx", {7}}};
	return preferences;
}

bool same(const Preferences &a, const Preferences &b) {
	return a.recent_projects == b.recent_projects && a.runtime_executable == b.runtime_executable &&
	       a.game_install == b.game_install && a.play_in_install == b.play_in_install &&
	       a.import_dependencies == b.import_dependencies && a.recent_items == b.recent_items;
}

// The settings file every_preference() is: the keys sorted, two spaces an indent, a newline last;
// the game install's keys as S13 A4 named them; the recently placed items (S15) per game, schema 3.
const char *const kSettingsFile = "{\n"
                                  "  \"game_install\": \"D:/Joint Operations\",\n"
                                  "  \"import_dependencies\": false,\n"
                                  "  \"play_in_install\": true,\n"
                                  "  \"recent_items\": {\n"
                                  "    \"dfx\": [\n"
                                  "      7\n"
                                  "    ],\n"
                                  "    \"jo\": [\n"
                                  "      106100,\n"
                                  "      2044\n"
                                  "    ]\n"
                                  "  },\n"
                                  "  \"recent_projects\": [\n"
                                  "    \"C:/games/Armory\",\n"
                                  "    \"D:/mods/Harbor\"\n"
                                  "  ],\n"
                                  "  \"runtime_executable\": \"C:/tools/opennova.exe\",\n"
                                  "  \"schema_version\": 3\n"
                                  "}\n";

// S15's file (schema 2): the recently placed items one list, whatever the game.
const char *const kSchemaTwoFile = "{\n"
                                   "  \"game_install\": \"D:/Joint Operations\",\n"
                                   "  \"recent_items\": [\n"
                                   "    106100,\n"
                                   "    2044\n"
                                   "  ],\n"
                                   "  \"schema_version\": 2\n"
                                   "}\n";

// The same preferences as an editor before S13 A4 wrote them: schema 1, the game install under
// retail_directory and Play in it under play_retail.
const char *const kSchemaOneFile = "{\n"
                                   "  \"import_dependencies\": false,\n"
                                   "  \"play_retail\": true,\n"
                                   "  \"recent_projects\": [\n"
                                   "    \"C:/games/Armory\",\n"
                                   "    \"D:/mods/Harbor\"\n"
                                   "  ],\n"
                                   "  \"retail_directory\": \"D:/Joint Operations\",\n"
                                   "  \"runtime_executable\": \"C:/tools/opennova.exe\",\n"
                                   "  \"schema_version\": 1\n"
                                   "}\n";

// A store whose saves fail, as a settings file that cannot be written does.
struct RefusingStore : PreferencesStore {
	Preferences kept;
	bool load(Preferences &out, Diagnostic &) override {
		out = kept;
		return true;
	}
	bool save(const Preferences &, Diagnostic &error) override {
		error = editor_test::finding_of(DiagnosticSeverity::Error, "editor_settings.write", "refused");
		return false;
	}
};

} // namespace

// The memory store gives back every preference it was given, and the defaults before a save.
static int test_memory_store_round_trips() {
	MemoryPreferencesStore store;
	Preferences loaded;
	Diagnostic error;
	TEST_EXPECT(store.load(loaded, error) && same(loaded, Preferences()) && loaded.import_dependencies);
	TEST_EXPECT(store.save(every_preference(), error) && store.saves() == 1);
	TEST_EXPECT(store.load(loaded, error) && same(loaded, every_preference()));
	MemoryPreferencesStore seeded(every_preference());
	TEST_EXPECT(seeded.load(loaded, error) && same(loaded, every_preference()) && seeded.saves() == 0);
	// Each preference alone, so none rides on another.
	Preferences one;
	one.recent_projects = {"E:/x"};
	TEST_EXPECT(store.save(one, error) && store.load(loaded, error) && same(loaded, one));
	one = Preferences();
	one.runtime_executable = "F:/runtime.exe";
	TEST_EXPECT(store.save(one, error) && store.load(loaded, error) && same(loaded, one));
	one = Preferences();
	one.game_install = "G:/JO";
	TEST_EXPECT(store.save(one, error) && store.load(loaded, error) && same(loaded, one));
	one = Preferences();
	one.play_in_install = true;
	TEST_EXPECT(store.save(one, error) && store.load(loaded, error) && same(loaded, one));
	one = Preferences();
	one.import_dependencies = false;
	TEST_EXPECT(store.save(one, error) && store.load(loaded, error) && same(loaded, one));
	one = Preferences();
	one.recent_items = {{"jo", {7}}};
	TEST_EXPECT(store.save(one, error) && store.load(loaded, error) && same(loaded, one));
	return 0;
}

// The file store writes the settings file byte for byte and reads it back; a file an older editor
// wrote (schema 1, the game install under its old keys) is set aside (S13 A4: no compat reader):
// read as the defaults, the warning naming the file and everything it held, now gone, and written
// again as a new file by the next save.
static int test_file_store_writes_the_settings_file() {
	editor_test::TempProjectDir dir("opennova_editor_preferences_file");
	const std::string path = dir.file("a/b/editor_settings.json");
	FilePreferencesStore store(path);
	Diagnostic error;
	TEST_EXPECT(store.path() == path);
	TEST_EXPECT(store.save(every_preference(), error));
	std::string written;
	TEST_EXPECT(test_io::read_file_text(path, written) && written == kSettingsFile);
	Preferences loaded;
	TEST_EXPECT(store.load(loaded, error) && same(loaded, every_preference()));

	// A user's file as this editor writes it: read as it is. One an editor before S13 A4 wrote: set
	// aside, read as the defaults, the warning naming the file and each thing it held.
	const std::string users = dir.file("user/editor_settings.json");
	TEST_EXPECT(editor_test::write_text(users, kSettingsFile));
	TEST_EXPECT(FilePreferencesStore(users).load(loaded, error) && same(loaded, every_preference()));
	const std::string older_editor = dir.file("user/older_editor_settings.json");
	TEST_EXPECT(editor_test::write_text(older_editor, kSchemaOneFile));
	loaded = every_preference();
	Diagnostic finding;
	TEST_EXPECT(FilePreferencesStore(older_editor).load(loaded, finding) &&
			same(loaded, Preferences()));
	TEST_EXPECT(finding.severity == DiagnosticSeverity::Warning &&
			finding.code() == "editor_settings.schema_version.unsupported" &&
			finding.message.find(older_editor + " is set aside") == 0);
	for (const char *held : { "(schema 1; this editor reads schema 3)", "import_dependencies false",
				 "play_retail true", "recent_projects (2 entries)",
				 "retail_directory \"D:/Joint Operations\"",
				 "runtime_executable \"C:/tools/opennova.exe\"" })
		TEST_EXPECT(finding.message.find(held) != std::string::npos);
	// Written over by the next save: the file the store writes.
	TEST_EXPECT(FilePreferencesStore(older_editor).save(every_preference(), error));
	TEST_EXPECT(test_io::read_file_text(older_editor, written) && written == kSettingsFile);
	// S15's file, its items one list (the polish keeps them per game): set aside the same, no reader.
	const std::string s15 = dir.file("user/s15_editor_settings.json");
	TEST_EXPECT(editor_test::write_text(s15, kSchemaTwoFile));
	loaded = every_preference();
	Diagnostic two;
	TEST_EXPECT(FilePreferencesStore(s15).load(loaded, two) && same(loaded, Preferences()) &&
	            two.code() == "editor_settings.schema_version.unsupported" &&
	            two.message.find("(schema 2; this editor reads schema 3)") != std::string::npos &&
	            two.message.find("recent_items (2 entries)") != std::string::npos);

	// A file that is not there reads as the defaults; one that does not say whether an import
	// brings the files it needs reads as on; a newer schema is set aside too (it held nothing);
	// a file that is not JSON is an error.
	TEST_EXPECT(FilePreferencesStore(dir.file("missing.json")).load(loaded, error) && same(loaded, Preferences()));
	TEST_EXPECT(editor_test::write_text(dir.file("bare.json"), "{\"schema_version\": 3}"));
	TEST_EXPECT(FilePreferencesStore(dir.file("bare.json")).load(loaded, error) && loaded.import_dependencies &&
	            loaded.game_install.empty() && !loaded.play_in_install);
	TEST_EXPECT(editor_test::write_text(dir.file("newer.json"), "{\"schema_version\": 99}"));
	Diagnostic newer;
	TEST_EXPECT(FilePreferencesStore(dir.file("newer.json")).load(loaded, newer) &&
			same(loaded, Preferences()) &&
			newer.code() == "editor_settings.schema_version.unsupported" &&
			newer.message.find("(schema 99; this editor reads schema 3)") != std::string::npos &&
			newer.message.find("what it held is gone: nothing.") != std::string::npos);
	TEST_EXPECT(editor_test::write_text(dir.file("broken.json"), "not json"));
	TEST_EXPECT(!FilePreferencesStore(dir.file("broken.json")).load(loaded, error) && error.code() == "editor_settings.json");

	// A file that cannot be written (its path a directory) is an error, the file untouched.
	std::error_code ec;
	fs::create_directories(dir.file("taken.json"), ec);
	TEST_EXPECT(!FilePreferencesStore(dir.file("taken.json")).save(every_preference(), error) &&
	            error.code() == "editor_settings.write");
	return 0;
}

// EditorPreferences: the recent list capped at kRecentProjectsMax, most recent first, a project
// named again moved to the front; a change written from a copy, in effect once the store keeps
// it; a store that cannot be read gives the defaults.
static int test_editor_preferences() {
	MemoryPreferencesStore store;
	EditorPreferences preferences(store);
	Diagnostic error;
	TEST_EXPECT(preferences.load(error) && same(preferences.values(), Preferences()));
	for (int i = 0; i < 12; ++i) preferences.remember_recent_project("p" + std::to_string(i));
	TEST_EXPECT(preferences.values().recent_projects.size() == kRecentProjectsMax);
	TEST_EXPECT(preferences.values().recent_projects.front() == "p11");
	preferences.remember_recent_project("p5"); // moves to the front, no duplicate
	TEST_EXPECT(preferences.values().recent_projects.front() == "p5" &&
	            preferences.values().recent_projects.size() == kRecentProjectsMax);
	preferences.forget_recent_project("p5");
	TEST_EXPECT(preferences.values().recent_projects.front() == "p11" &&
	            preferences.values().recent_projects.size() == kRecentProjectsMax - 1);
	TEST_EXPECT(store.saves() == 0 && preferences.save(error) && store.saves() == 1 &&
	            store.preferences().recent_projects == preferences.values().recent_projects);
	// The recently placed items (S15), per game (the polish): capped at kRecentItemsMax, most recent
	// first, an item placed again moved to the front; another game's its own, none before any.
	for (int64_t item = 1; item <= 14; ++item) preferences.remember_recent_item("jo", item);
	const std::vector<int64_t> &jo = preferences.recent_items("jo");
	TEST_EXPECT(jo.size() == kRecentItemsMax && jo.front() == 14 && jo.back() == 3);
	TEST_EXPECT(preferences.remember_recent_item("jo", 5) && !preferences.remember_recent_item("jo", 5));
	TEST_EXPECT(preferences.recent_items("jo").front() == 5 && preferences.recent_items("jo").size() == kRecentItemsMax &&
	            std::count(preferences.recent_items("jo").begin(), preferences.recent_items("jo").end(), int64_t(5)) == 1);
	TEST_EXPECT(preferences.recent_items("dfx").empty() && preferences.remember_recent_item("dfx", 106100));
	TEST_EXPECT(preferences.recent_items("dfx") == std::vector<int64_t>({106100}) && preferences.recent_items("jo").front() == 5);
	TEST_EXPECT(preferences.write(every_preference(), error) && same(preferences.values(), every_preference()) &&
	            same(store.preferences(), every_preference()));

	// A write the store refuses leaves the values in effect.
	RefusingStore refusing;
	refusing.kept = every_preference();
	EditorPreferences held(refusing);
	TEST_EXPECT(held.load(error) && same(held.values(), every_preference()));
	TEST_EXPECT(!held.write(Preferences(), error) && error.code() == "editor_settings.write" &&
	            same(held.values(), every_preference()));

	// A store that cannot be read: the defaults, and why. One that set aside what it kept: the
	// defaults, and the warning. A load with nothing to say clears what an earlier one said.
	editor_test::TempProjectDir dir("opennova_editor_preferences_unreadable");
	TEST_EXPECT(editor_test::write_text(dir.file("bad.json"), "not json"));
	FilePreferencesStore bad(dir.file("bad.json"));
	EditorPreferences unread(bad);
	TEST_EXPECT(!unread.load(error) && error.code() == "editor_settings.json" &&
			same(unread.values(), Preferences()));
	TEST_EXPECT(editor_test::write_text(dir.file("aside.json"), "{\"schema_version\": 1}"));
	FilePreferencesStore aside(dir.file("aside.json"));
	EditorPreferences set_aside(aside);
	TEST_EXPECT(set_aside.load(error) && error.severity == DiagnosticSeverity::Warning &&
			error.code() == "editor_settings.schema_version.unsupported" &&
			same(set_aside.values(), Preferences()));
	TEST_EXPECT(preferences.load(error) && error.code().empty());
	return 0;
}

// A session reads its preferences from the store its embedder hands it and writes them back
// there: a memory store's session starts from what the store holds and keeps what it changes; a
// file store's session writes the settings file; one that cannot be read is a finding, the
// defaults in effect, and one set aside a warning, the next save writing a new file.
static int test_session_over_a_store() {
	editor_test::TempProjectDir dir("opennova_editor_preferences_session");
	editor_test::NoProcess platform;
	{
		MemoryPreferencesStore store(every_preference());
		ProjectSession session(platform, store);
		const SessionView &v = session.view();
		TEST_EXPECT(v.project.recent_projects == every_preference().recent_projects && v.project.retail_directory == "D:/Joint Operations" &&
		            v.project.play_retail && v.project.runtime_setting == "C:/tools/opennova.exe" && !v.project.import_dependencies);
		session.handle(request::set_import_dependencies(true));
		session.run_operations();
		TEST_EXPECT(v.project.import_dependencies && store.preferences().import_dependencies);
		session.handle(request::forget_recent("C:/games/Armory"));
		TEST_EXPECT(v.project.recent_projects == std::vector<std::string>({"D:/mods/Harbor"}) &&
		            store.preferences().recent_projects == v.project.recent_projects);
		// The recently placed items shown are the open project's game's (the polish): none with no
		// project open, a JO project's its own, none again once it closes; the other game's kept.
		TEST_EXPECT(v.project.recent_items.empty());
		session.handle(request::new_project(dir.file("jo_project"), "Placed"));
		session.run_operations();
		TEST_EXPECT(session.project_open() && v.project.recent_items == std::vector<int64_t>({106100, 2044}));
		session.handle(request::close_project());
		session.run_operations();
		TEST_EXPECT(!session.project_open() && v.project.recent_items.empty() &&
		            store.preferences().recent_items.at("dfx") == std::vector<int64_t>({7}));
	}
	{
		const std::string path = dir.file("settings/editor_settings.json");
		FilePreferencesStore store(path);
		ProjectSession session(platform, store);
		session.handle(request::new_project(dir.file("project"), "Stored"));
		session.run_operations();
		TEST_EXPECT(session.project_open());
		Preferences stored;
		Diagnostic error;
		TEST_EXPECT(store.load(stored, error) && stored.recent_projects.size() == 1 &&
		            stored.recent_projects.front() == session.view().project.root);
	}
	{
		TEST_EXPECT(editor_test::write_text(dir.file("bad.json"), "not json"));
		FilePreferencesStore store(dir.file("bad.json"));
		ProjectSession session(platform, store);
		const SessionView &v = session.view();
		bool said = false;
		for (const Diagnostic &d : v.findings.diagnostics)
			said = said || d.code() == "editor_settings.json";
		TEST_EXPECT(said && v.project.recent_projects.empty() && v.project.import_dependencies);
	}
	{
		// An older editor's settings file (S13 A4): set aside, one warning naming what it held,
		// nothing of it in effect (no game install, no recent project); a project then opens, done,
		// and its save of the recent projects writes this editor's schema over it.
		const std::string path = dir.file("older/editor_settings.json");
		TEST_EXPECT(editor_test::write_text(path, kSchemaOneFile));
		FilePreferencesStore store(path);
		ProjectSession session(platform, store);
		const SessionView &v = session.view();
		size_t said = 0;
		for (const Diagnostic &d : v.findings.diagnostics)
			said += d.code() == "editor_settings.schema_version.unsupported" &&
					d.severity == DiagnosticSeverity::Warning &&
					d.message.find("retail_directory \"D:/Joint Operations\"") != std::string::npos;
		TEST_EXPECT(said == 1 && v.project.recent_projects.empty() &&
				v.project.retail_directory.empty() && !v.project.play_retail);
		session.handle(request::new_project(dir.file("after"), "After"));
		session.run_operations();
		TEST_EXPECT(session.outcome().done());
		std::string written;
		TEST_EXPECT(session.project_open() && test_io::read_file_text(path, written) &&
		            written.find("\"schema_version\": 3") != std::string::npos &&
		            written.find("retail") == std::string::npos && written.find("\"game_install\": \"\"") != std::string::npos);
	}
	return 0;
}

int main() {
	int failures = 0;
	failures += test_memory_store_round_trips();
	failures += test_file_store_writes_the_settings_file();
	failures += test_editor_preferences();
	failures += test_session_over_a_store();
	if (failures == 0) std::printf("editor_preferences: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
