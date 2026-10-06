// Pins Play's log as Problems rows (ADR 0046 DI-27) over a fake process platform: a line of OpenNova's
// log saying a miss (gameprofile::kResourceMissingMarker) is a row on the file and the record of the
// project that names the missing name, with its Go to and a missing reference's or a required file's
// fixes; the game install's logs, read once its game exited, make rows of what its file log and its
// graphics log show it lacked; each mode's rows stay until the next Play of that mode, and go with the
// project.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <base/gameprofile/resource_missing.h>
#include <base/io/json.h>
#include <editor/project/project_files.h>
#include <editor/run/launch_plan.h>
#include <editor/session/play_log.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
using opennova::gameprofile::ResourceMiss;
namespace resource_kind = opennova::gameprofile::resource_kind;
namespace fs = std::filesystem;

namespace {

using editor_test::FakePlatform;

// A line of OpenNova's log saying a miss, as ResourceRoot::report_missing writes it.
std::string miss_line(const char *kind, const std::string &name, const std::string &by = std::string(),
                      const std::string &words = std::string()) {
	ResourceMiss miss;
	miss.kind = kind;
	miss.name = name;
	miss.by = by;
	miss.words = words;
	return "WARNING: ResourceRoot: " + opennova::gameprofile::resource_missing_text(miss) + "\n   at: push_warning (core/variant/variant_utility.cpp:1034)\n";
}

std::vector<const Diagnostic *> rows_of(const SessionView &v, const char *code) {
	std::vector<const Diagnostic *> out;
	for (const Diagnostic &d : v.findings.diagnostics)
		if (d.code() == code) out.push_back(&d);
	return out;
}

const Diagnostic *row_about(const SessionView &v, const char *code, const std::string &target) {
	for (const Diagnostic &d : v.findings.diagnostics)
		if (d.code() == code && subject_target(d) == target) return &d;
	return nullptr;
}

bool has_fix(const Diagnostic &d, const SessionView &v, const std::string &label) {
	for (const ProblemFix &fix : fixes_for(d, v))
		if (fix.label == label) return true;
	return false;
}

// A project whose files name what the lines below report missing: an item drawing a model the project
// lacks, a model (the synthetic crate) naming a texture it lacks, a sound profile's slot naming a set no
// bank holds.
struct LogProject {
	editor_test::TempProjectDir dir{"opennova_editor_play_log"};
	FakePlatform platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;

	bool open() {
		root = dir.file("project");
		session.handle(request::new_project(root, "Play log"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const std::vector<uint8_t> crate =
		        test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/crate.3di");
		if (crate.empty() || !editor_test::write_bytes(root + "/crate.3di", crate)) return false;
		if (!editor_test::write_text(root + "/defs/items.def", "begin \"Null\"\n\tid 100000\n\ttype marker\nend\n"
		                                                  "begin \"Wooden barrel\"\n\tgraphic onbarrel\n\tid 100011\n\ttype decoration\nend\n"
		                                                  "begin \"Crate\"\n\tgraphic crate\n\tid 100012\n\ttype decoration\nend\n"))
			return false;
		if (!editor_test::write_text(root + "/defs/SndProf.def", "begin \"default\"\nend\nbegin \"on_soldier\"\n"
		                                                    "\tSSRFootGND NO_SUCH_SET 0 0 0\nend\n"))
			return false;
		session.handle(request::rescan());
		session.run_operations();
		const std::string runtime = dir.file("runtime/opennova.exe");
		if (!editor_test::write_text(runtime, "MZ")) return false;
		PlayLauncher launcher;
		launcher.executable = runtime;
		session.set_launcher_source(editor_test::fixed_launcher(launcher));
		return true;
	}
	// A Play in the mode the settings say, run to its spawn.
	bool play() {
		session.handle(request::play());
		session.run_operations();
		const bool running = session.view().activity.play_state == PlayState::Running;
		if (!running)
			for (const Diagnostic &d : session.view().findings.diagnostics)
				std::fprintf(stderr, "  %s %s: %s\n", diagnostic_severity_label(d.severity), d.code().c_str(), d.message.c_str());
		return running;
	}
	// The running game's log gains `text`, read on a poll, the validation it left due run to its end.
	void log(const std::string &text) {
		std::string held;
		std::string error;
		read_file_text(platform.last_plan.log_file, held, error);
		editor_test::write_text(platform.last_plan.log_file, held + text);
		session.poll();
		session.run_operations();
	}
	// The running game ends on its own with `code`, its exit read on the polls.
	void exit(uint32_t code) {
		const int64_t pid = session.view().activity.play_pid;
		platform.clock += 5000;
		platform.codes[pid] = code;
		platform.exit_child(pid);
		session.poll();
		session.poll();
		session.run_operations();
	}
};

} // namespace

// A miss line's words read back, every kind of row it makes, each on the file and record naming it.
static int test_runtime_misses() {
	LogProject project;
	TEST_EXPECT(project.open());
	const SessionView &v = project.session.view();
	TEST_EXPECT(project.play());
	project.log("Godot Engine v4.6.1\n" + miss_line(resource_kind::kModel, "onbarrel.3di", "", "what draws it is not drawn") +
	            miss_line(resource_kind::kTexture, "crate.tga", "crate.3di", "its material draws the missing-texture checkerboard") +
	            miss_line(resource_kind::kSound, "NO_SUCH_SET", "", "no sound bank the game loaded holds it, so it plays nothing") +
	            miss_line(resource_kind::kFile, "hudpos.def") +
	            miss_line(resource_kind::kFile, "overcast.def", "", "the overcast weather has no table of its own to blend toward") +
	            miss_line(resource_kind::kSound, "NOBODY_NAMES_IT"));
	// The model: on the item that draws it, its graphic field, a missing model's subject; its Go to opens
	// items.def at the record.
	const Diagnostic *model = row_about(v, "play.reference_missing", "onbarrel");
	TEST_EXPECT(model && model->asset == "defs/items.def" && model->record == "Wooden barrel" && model->field == "graphic" &&
	            model->row_id != 0 && model->severity == DiagnosticSeverity::Warning &&
	            model->message == "'Wooden barrel' in defs/items.def names the model 'onbarrel', which OpenNova could not find in "
	                              "its last Play: what draws it is not drawn.");
	if (!model) return 1;
	const ProblemLocation at = problem_location(*model, v);
	TEST_EXPECT(at.path == "defs/items.def" && at.record.row == model->row_id && !at.in_files &&
	            at.request().kind == EditorRequestKind::OpenDocument);
	// The texture: on the crate's material row, its fix the placeholder a missing texture's is.
	const Diagnostic *texture = row_about(v, "play.reference_missing", "crate.tga");
	TEST_EXPECT(texture && texture->asset == "crate.3di" && !texture->field.empty() &&
	            has_fix(*texture, v, "Create a placeholder crate.tga"));
	// The set: on the profile's slot naming it.
	const Diagnostic *sound = row_about(v, "play.reference_missing", "NO_SUCH_SET");
	TEST_EXPECT(sound && sound->asset == "defs/SndProf.def" && sound->row_id != 0 &&
	            reference_subject(*sound)->kind == ReferenceKind::Sound);
	// A set no file names: the name alone.
	const Diagnostic *nobody = row_about(v, "play.reference_missing", "NOBODY_NAMES_IT");
	TEST_EXPECT(nobody && nobody->asset.empty() &&
	            nobody->message == "OpenNova looked for the sound set 'NOBODY_NAMES_IT' in its last Play and did not find it.");
	// Files the game opens by name: hudpos.def by its manifest row (the requirement's role, what the game does
	// without it in the requirement's words), overcast.def by none, an Info.
	const Diagnostic *hudpos = row_about(v, "play.file_missing", "hudpos.def");
	TEST_EXPECT(hudpos && editor_test::requirement_of(*hudpos).role == "hudpos_def" &&
	            hudpos->message == "OpenNova looked for hudpos.def in its last Play and did not find it. The HUD uses its "
	                               "default positions." &&
	            hudpos->severity == DiagnosticSeverity::Info);
	const Diagnostic *overcast = row_about(v, "play.file_missing", "overcast.def");
	TEST_EXPECT(overcast && editor_test::requirement_of(*overcast).role.empty() && overcast->severity == DiagnosticSeverity::Info);
	TEST_EXPECT(rows_of(v, "play.reference_missing").size() == 4 && rows_of(v, "play.file_missing").size() == 2);
	// The same line again, a line of no marker, a marker line with no name: nothing more.
	project.log(miss_line(resource_kind::kModel, "onbarrel.3di", "", "what draws it is not drawn") +
	            "WARNING: resource missing: model\n");
	TEST_EXPECT(rows_of(v, "play.reference_missing").size() == 4);
	// The rows stay through a validation an edit leaves due, and on the wire each carries its Go to.
	project.session.handle(request::rescan());
	project.session.run_operations();
	TEST_EXPECT(rows_of(v, "play.reference_missing").size() == 4);
	{
		std::string error;
		const opennova::io::JsonValue answer =
		        project.session.query("problems", opennova::io::JsonValue::make_object(), error);
		bool go_to = false;
		if (const opennova::io::JsonValue *rows = answer.get("problems"))
			for (const opennova::io::JsonValue &row : rows->array)
				if (row.get_string("code", "") == "play.reference_missing" && row.get_string("asset", "") == "defs/items.def") {
					const opennova::io::JsonValue *request = row.get("go_to");
					go_to = request && request->get_string("kind", "") == "open_document" &&
					        request->get_string("path", "") == "defs/items.def";
				}
		TEST_EXPECT(go_to);
	}
	return 0;
}

// Each mode's rows stay until the next Play of that mode: OpenNova's through a Play in the game install, the
// game install's through one of OpenNova; a line read with another project open is ignored; the project
// closing drops them all.
static int test_rows_by_mode() {
	LogProject project;
	TEST_EXPECT(project.open());
	const SessionView &v = project.session.view();
	const std::string install = project.dir.file("install");
	TEST_EXPECT(editor_test::write_text(install + "/Jointops.exe", "retail executable") &&
	            editor_test::write_text(install + "/binkw32.dll", "Bink"));
	editor_test::set_game_install(project.session, install);
	TEST_EXPECT(project.play());
	project.log(miss_line(resource_kind::kModel, "onbarrel.3di"));
	project.exit(0);
	TEST_EXPECT(rows_of(v, "play.reference_missing").size() == 1);
	// Strict Play in the game install: its file log read once it exited (the crate read, its texture missing
	// from the build; gameerr.bin not opened, so earlyerr.txt's line 4 and on; vmacros.bin not opened, the
	// boot stopped there), and its graphics log (the mission begun, never finished).
	ProjectSettingsChange strict;
	strict.play_in_install = true;
	strict.play_in_install_strict = true;
	editor_test::apply_settings(project.session, strict);
	TEST_EXPECT(project.play());
	const std::string run = project.platform.last_plan.working_dir;
	TEST_EXPECT(editor_test::write_text(run + "/_filelog.txt",
	                                    "LOADED FILE: language.pff\nLOADED FILE: localres.pff\nLOADED FILE: resource.pff\n"
	                                    "PFF LOADED FILE: gametext.bin\nPFF LOADED FILE: crate.3di\n") &&
	            editor_test::write_text(run + "/ghw.txt", "GHW.TXT - LOG FILE\nMission:\"ONJO_M1.BMS\" - \"!Untitled\" - "
	                                                      "\"ONJO_M1.BMS\" - \"\"\n"));
	project.exit(0);
	const Diagnostic *texture = nullptr;
	for (const Diagnostic *d : rows_of(v, "play.reference_missing"))
		if (subject_target(*d) == "crate.tga") texture = d;
	TEST_EXPECT(texture && texture->asset == "crate.3di" &&
	            texture->message.find("the game install (strict Play) read crate.3di in its last Play, and the build holds "
	                                  "no texture of that name.") != std::string::npos);
	const Diagnostic *gameerr = row_about(v, "play.file_missing", "gameerr.bin");
	const Diagnostic *vmacros = row_about(v, "play.file_missing", "vmacros.bin");
	TEST_EXPECT(gameerr && gameerr->message.find("earlyerr.txt's line 4") != std::string::npos &&
	            editor_test::requirement_of(*gameerr).role == "gameerr" && vmacros &&
	            !row_about(v, "play.file_missing", "keyhelp.bin") && !row_about(v, "play.file_missing", "gametext.bin"));
	const std::vector<const Diagnostic *> unfinished = rows_of(v, "play.mission.unfinished");
	TEST_EXPECT(unfinished.size() == 1 && unfinished[0]->message.find("began loading ONJO_M1.BMS") != std::string::npos);
	// OpenNova's row stays beside the strict Play's.
	TEST_EXPECT(row_about(v, "play.reference_missing", "onbarrel") != nullptr);
	// A ghw.txt the next run leaves as it was is a run before's: not read again.
	TEST_EXPECT(project.play());
	TEST_EXPECT(!row_about(v, "play.file_missing", "gameerr.bin") && rows_of(v, "play.mission.unfinished").empty() &&
	            row_about(v, "play.reference_missing", "onbarrel") != nullptr);
	project.exit(0); // no file log: it opened none of its archives
	TEST_EXPECT(rows_of(v, "play.mission.unfinished").empty());
	const Diagnostic *none = nullptr;
	for (const Diagnostic *d : rows_of(v, "play.file_missing"))
		if (d->message.find("opened none of its archives") != std::string::npos) none = d;
	TEST_EXPECT(none != nullptr);
	// OpenNova again: its own rows go, the strict Play's stay.
	ProjectSettingsChange on_runtime;
	on_runtime.play_in_install = false;
	editor_test::apply_settings(project.session, on_runtime);
	TEST_EXPECT(project.play());
	TEST_EXPECT(!row_about(v, "play.reference_missing", "onbarrel") && none && rows_of(v, "play.file_missing").size() == 1);
	// A line read while another project is open is ignored; the first project's rows went with it.
	project.session.handle(request::new_project(project.dir.file("other"), "Other"));
	project.session.run_operations();
	project.log(miss_line(resource_kind::kModel, "onbarrel.3di"));
	TEST_EXPECT(rows_of(v, "play.reference_missing").empty() && rows_of(v, "play.file_missing").empty());
	return 0;
}

// The graphics log's missions: each `Mission:"<file>"` line, finished by the "Mission loading complete"
// after it.
static int test_graphics_log() {
	const std::vector<GraphicsLogMission> missions = graphics_log_missions(
	        "GHW.TXT - LOG FILE - 10/6/2026\r\nMission:\"A.BMS\" - \"x\" - \"A.BMS\" - \"\"\r\nSniper_Start()\r\n"
	        "Mission loading complete\r\nMission:\"B.BMS\" - \"y\" - \"B.BMS\" - \"\"\nTaking Snapshot\n");
	TEST_EXPECT(missions.size() == 2 && missions[0].file == "A.BMS" && missions[0].complete && missions[1].file == "B.BMS" &&
	            !missions[1].complete);
	TEST_EXPECT(graphics_log_missions("Mission loading complete\n").empty());
	return 0;
}

int main() {
	int failures = 0;
	failures += test_runtime_misses();
	failures += test_rows_by_mode();
	failures += test_graphics_log();
	return failures == 0 ? 0 : 1;
}
