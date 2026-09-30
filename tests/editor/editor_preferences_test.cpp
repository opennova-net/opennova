// Pins the editor's preferences over their store (ADR 0046 S13 A2): the memory store gives back
// every preference it was given; the file store writes the settings file the editor has always
// written, byte for byte, and reads one a user already has; EditorPreferences reads a store once,
// writes a change from a copy (a change the store refuses leaves the values in effect), and keeps
// the recent-projects list capped, most recent first; and a session over each store shows them.
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
#include <editor/session/session_view.h>

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
	preferences.retail_directory = "D:/Joint Operations";
	preferences.play_retail = true;
	preferences.import_dependencies = false;
	return preferences;
}

bool same(const Preferences &a, const Preferences &b) {
	return a.recent_projects == b.recent_projects && a.runtime_executable == b.runtime_executable &&
	       a.retail_directory == b.retail_directory && a.play_retail == b.play_retail &&
	       a.import_dependencies == b.import_dependencies;
}

// The settings file every_preference() is, as the editor's settings had always written it
// (session/editor_settings before S13 A2): the keys sorted, two spaces an indent, a newline last.
const char *const kSettingsFile = "{\n"
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
		error = make_diagnostic(DiagnosticSeverity::Error, "editor_settings.write", "refused");
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
	one.retail_directory = "G:/JO";
	TEST_EXPECT(store.save(one, error) && store.load(loaded, error) && same(loaded, one));
	one = Preferences();
	one.play_retail = true;
	TEST_EXPECT(store.save(one, error) && store.load(loaded, error) && same(loaded, one));
	one = Preferences();
	one.import_dependencies = false;
	TEST_EXPECT(store.save(one, error) && store.load(loaded, error) && same(loaded, one));
	return 0;
}

// The file store writes the file the editor always wrote, byte for byte, reads it back, and reads
// a file a user already has (written before the store existed) as it was.
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

	// A user's file, as the editor wrote it before: read as it was.
	const std::string users = dir.file("user/editor_settings.json");
	TEST_EXPECT(editor_test::write_text(users, kSettingsFile));
	TEST_EXPECT(FilePreferencesStore(users).load(loaded, error) && same(loaded, every_preference()));

	// A file that is not there reads as the defaults; one that does not say whether an import
	// brings the files it needs reads as on; another schema version, and a file that is not
	// JSON, are errors.
	TEST_EXPECT(FilePreferencesStore(dir.file("missing.json")).load(loaded, error) && same(loaded, Preferences()));
	TEST_EXPECT(editor_test::write_text(dir.file("older.json"), "{\"schema_version\": 1}"));
	TEST_EXPECT(FilePreferencesStore(dir.file("older.json")).load(loaded, error) && loaded.import_dependencies);
	TEST_EXPECT(editor_test::write_text(dir.file("bad.json"), "{\"schema_version\": 99}"));
	TEST_EXPECT(!FilePreferencesStore(dir.file("bad.json")).load(loaded, error) &&
	            error.code == "editor_settings.schema_version.unsupported");
	TEST_EXPECT(editor_test::write_text(dir.file("broken.json"), "not json"));
	TEST_EXPECT(!FilePreferencesStore(dir.file("broken.json")).load(loaded, error) && error.code == "editor_settings.json");

	// A file that cannot be written (its path a directory) is an error, the file untouched.
	std::error_code ec;
	fs::create_directories(dir.file("taken.json"), ec);
	TEST_EXPECT(!FilePreferencesStore(dir.file("taken.json")).save(every_preference(), error) &&
	            error.code == "editor_settings.write");
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
	TEST_EXPECT(preferences.write(every_preference(), error) && same(preferences.values(), every_preference()) &&
	            same(store.preferences(), every_preference()));

	// A write the store refuses leaves the values in effect.
	RefusingStore refusing;
	refusing.kept = every_preference();
	EditorPreferences held(refusing);
	TEST_EXPECT(held.load(error) && same(held.values(), every_preference()));
	TEST_EXPECT(!held.write(Preferences(), error) && error.code == "editor_settings.write" &&
	            same(held.values(), every_preference()));

	// A store that cannot be read: the defaults, and why.
	editor_test::TempProjectDir dir("opennova_editor_preferences_unreadable");
	TEST_EXPECT(editor_test::write_text(dir.file("bad.json"), "{\"schema_version\": 99}"));
	FilePreferencesStore bad(dir.file("bad.json"));
	EditorPreferences unread(bad);
	TEST_EXPECT(!unread.load(error) && same(unread.values(), Preferences()));
	return 0;
}

// A session reads its preferences from the store its embedder hands it and writes them back
// there: a memory store's session starts from what the store holds and keeps what it changes; a
// file store's session writes the settings file; one that cannot be read is a finding, the
// defaults in effect.
static int test_session_over_a_store() {
	editor_test::TempProjectDir dir("opennova_editor_preferences_session");
	editor_test::NoProcess platform;
	{
		MemoryPreferencesStore store(every_preference());
		ProjectSession session(platform, store);
		const SessionView &v = session.view();
		TEST_EXPECT(v.recent_projects == every_preference().recent_projects && v.retail_directory == "D:/Joint Operations" &&
		            v.play_retail && v.runtime_setting == "C:/tools/opennova.exe" && !v.import_dependencies);
		EditorRequest on = make_request(EditorRequestKind::SetImportDependencies);
		on.flag = true;
		session.handle(on);
		TEST_EXPECT(v.import_dependencies && store.preferences().import_dependencies);
		session.handle(make_request(EditorRequestKind::ForgetRecent, "C:/games/Armory"));
		TEST_EXPECT(v.recent_projects == std::vector<std::string>({"D:/mods/Harbor"}) &&
		            store.preferences().recent_projects == v.recent_projects);
	}
	{
		const std::string path = dir.file("settings/editor_settings.json");
		FilePreferencesStore store(path);
		ProjectSession session(platform, store);
		session.handle(make_request(EditorRequestKind::NewProject, dir.file("project"), "Stored"));
		TEST_EXPECT(session.project_open());
		Preferences stored;
		Diagnostic error;
		TEST_EXPECT(store.load(stored, error) && stored.recent_projects.size() == 1 &&
		            stored.recent_projects.front() == session.view().project_root);
	}
	{
		TEST_EXPECT(editor_test::write_text(dir.file("bad.json"), "{\"schema_version\": 99}"));
		FilePreferencesStore store(dir.file("bad.json"));
		ProjectSession session(platform, store);
		const SessionView &v = session.view();
		bool said = false;
		for (const Diagnostic &d : v.diagnostics) said = said || d.code == "editor_settings.schema_version.unsupported";
		TEST_EXPECT(said && v.recent_projects.empty() && v.import_dependencies);
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
