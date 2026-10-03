// Pins the Play launch plan and the one managed child (ADR 0046 d8/d10) over a fake
// platform: the argument vector, where a packaged editor finds its runtime, the run directory the
// game works in and what the game install's game finds there (S13 A8), one child at a
// time, the stop request, the deadline kill, exit on its own with the code it exited with,
// and the build directory the session protects while alive.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <editor/project/project_files.h>
#include <editor/run/launch_plan.h>
#include <formats/pff/pff.h>
#include <editor/run/play_session.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

using editor_test::FakePlatform;

// An archive of no entries (formats/pff's writer), as the game opens one.
static bool write_empty_archive(const std::string &path) {
	std::filesystem::create_directories(std::filesystem::path(path).parent_path());
	return opennova::pff::pff_write_archive(path.c_str(), opennova::pff::PFF_FORMAT_PFF3, nullptr, 0) == opennova::pff::PFF_WRITE_OK;
}

static int test_launch_plans() {
	// S13 A8: the game runs on the build directory and works in the run directory, its log there.
	const LaunchPlan play = make_play_launch_plan("C:/tools/opennova.exe", "C:/p/.opennova/build/play/abc",
	                                              "C:/p/.opennova/run/1", "jo", 8975, "m.bms");
	TEST_EXPECT(play.executable == "C:/tools/opennova.exe");
	TEST_EXPECT(play.build_dir == "C:/p/.opennova/build/play/abc" && play.working_dir == "C:/p/.opennova/run/1");
	TEST_EXPECT(play.log_file == "C:/p/.opennova/run/1/session.log");
	const std::vector<std::string> expected = {"--log-file", "C:/p/.opennova/run/1/session.log", "--",
	                                           "--resource-dir", "C:/p/.opennova/build/play/abc", "/game", "jo",
	                                           "--mcp-port", "8975", "--mission", "m.bms"};
	TEST_EXPECT(play.args == expected);
	TEST_EXPECT(launch_plan_command_line(play).find("--resource-dir C:/p/.opennova/build/play/abc") != std::string::npos);

	const LaunchPlan quiet = make_play_launch_plan("opennova", "/b", "/r", "", 0);
	TEST_EXPECT(quiet.args == std::vector<std::string>({"--log-file", "/r/session.log", "--", "--resource-dir", "/b"}));
	const LaunchPlan headless = make_play_launch_plan("opennova", "/b", "/r", "", 0, "", {"--headless", "--quit-after", "3"});
	TEST_EXPECT(headless.args[0] == "--headless" && headless.args[2] == "3" && headless.args[3] == "--log-file");

	// A source run names its run directory too: Godot's --path moves the process to the project,
	// and the game keeps its saves in the directory it was started in (LaunchFlags.working_dir). A
	// packaged runtime is started in it, and takes no such flag.
	const LaunchPlan source = make_source_launch_plan("godot", "C:/repo/godot", "C:/p/build", "C:/p/run/2", "jo", 0);
	TEST_EXPECT(source.args[0] == "--path" && source.args[1] == "C:/repo/godot");
	TEST_EXPECT(source.args[2] == "res://game/game_runtime_root.tscn");
	TEST_EXPECT(source.args[3] == "--log-file" && source.args[4] == "C:/p/run/2/session.log");
	TEST_EXPECT(source.working_dir == "C:/p/run/2" && source.build_dir == "C:/p/build");
	TEST_EXPECT(source.args.size() >= 2 && source.args[source.args.size() - 2] == "--working-dir" &&
	            source.args.back() == "C:/p/run/2");
	TEST_EXPECT(std::find(play.args.begin(), play.args.end(), "--working-dir") == play.args.end());
	TEST_EXPECT(launch_plan_command_line(make_play_launch_plan("C:/a b/opennova.exe", "/x y", "/r", "", 0))
	                    .find("\"C:/a b/opennova.exe\"") == 0);

	// A packaged editor runs the runtime the release layout puts beside it; a source run
	// drives its own binary on the checkout.
	const PlayLauncher packaged =
	        make_play_launcher(false, "C:/OpenNova/editor/opennova_editor.exe", "", 8123, {"--headless"});
	TEST_EXPECT(!packaged.source_run && packaged.executable == "C:/OpenNova/runtime/opennova.exe");
	TEST_EXPECT(packaged.godot_project_dir.empty() && packaged.mcp_port == 8123);
	TEST_EXPECT(packaged.engine_args == std::vector<std::string>({"--headless"}));
	const PlayLauncher from_source = make_play_launcher(true, "C:/tools/godot.exe", "C:/repo/godot/", 0, {});
	TEST_EXPECT(from_source.source_run && from_source.executable == "C:/tools/godot.exe" &&
	            from_source.godot_project_dir == "C:/repo/godot/");
	return 0;
}

static int test_lifecycle() {
	// A child that runs on after a stop request until the test ends it (or the deadline kills it).
	FakePlatform platform;
	platform.next_pid = 100;
	platform.clock = 1000;
	platform.terminate_exits = false;
	PlaySession session(platform);
	Diagnostic error;
	TEST_EXPECT(session.state() == PlayState::Stopped);
	TEST_EXPECT(session.running_build_dir().empty());
	const LaunchPlan plan = make_play_launch_plan("opennova.exe", "/build/1", "/run/1", "jo", 9000);

	TEST_EXPECT(session.start(plan, error));
	TEST_EXPECT(session.state() == PlayState::Running && session.pid() == 100);
	TEST_EXPECT(session.running_build_dir() == "/build/1");
	TEST_EXPECT(platform.last_plan.working_dir == "/run/1");
	TEST_EXPECT(!session.start(plan, error)); // one child at a time
	TEST_EXPECT(error.code() == "play.already_running");
	TEST_EXPECT(session.poll() == PlayState::Running);

	// The child exits on its own: the session notices on the next poll, and reads the code
	// it exited with before it lets the handle go.
	platform.codes[100] = 0xC0000005u;
	platform.exit_child(100);
	TEST_EXPECT(session.poll() == PlayState::Stopped);
	TEST_EXPECT(session.exited_on_its_own() && session.exit_code() == 0xC0000005LL);
	TEST_EXPECT(session.pid() == -1 && session.running_build_dir().empty());
	TEST_EXPECT(platform.log.back() == "release 100");

	// A stop request that the child honours.
	TEST_EXPECT(session.start(plan, error));
	session.stop();
	TEST_EXPECT(session.state() == PlayState::Stopping);
	TEST_EXPECT(platform.log.back() == "terminate 101");
	TEST_EXPECT(session.poll() == PlayState::Stopping);
	platform.codes[101] = 1;
	platform.exit_child(101);
	TEST_EXPECT(session.poll() == PlayState::Stopped);
	TEST_EXPECT(!session.exited_on_its_own() && session.exit_code() == -1); // stopped: no code of its own

	// A stop request the child ignores: killed once the deadline passes.
	TEST_EXPECT(session.start(plan, error));
	session.stop();
	platform.clock += kPlayStopDeadlineMs - 1;
	TEST_EXPECT(session.poll() == PlayState::Stopping);
	platform.clock += 1;
	TEST_EXPECT(session.poll() == PlayState::Stopped);
	TEST_EXPECT(platform.log[platform.log.size() - 2] == "kill 102");

	// wait() polls on the clock, pausing between observations, and gives up honestly.
	TEST_EXPECT(session.start(plan, error));
	const int64_t before_wait = platform.clock;
	TEST_EXPECT(!session.wait(35));
	TEST_EXPECT(platform.clock - before_wait >= 35 && platform.clock - before_wait < 35 + 2 * kPlayWaitStepMs);
	TEST_EXPECT(session.state() == PlayState::Running); // a timed-out wait changes nothing
	platform.exit_child(103);
	TEST_EXPECT(session.wait(35));
	TEST_EXPECT(session.exited_on_its_own() && session.exit_code() == -1); // a code the platform could not read

	platform.spawn_fails = true;
	TEST_EXPECT(!session.start(plan, error));
	TEST_EXPECT(error.code() == "play.spawn");
	TEST_EXPECT(session.state() == PlayState::Stopped);
	TEST_EXPECT(std::string(play_state_label(PlayState::Stopping)) == "stopping");
	return 0;
}

// S13 A8: the game install's game in a run directory: every required source checked before
// anything is copied, then the build's files (a file the game may write, a .cfg, .sav, .coo or
// .txt, copied; every other, its archives and a video among them, linked, or copied where the file
// system cannot link it; its record left behind), the install's executable and Bink DLL, a game.cfg
// (the build's own over the install's) and the install's saves where the project has none of its
// own put there, the build directory and the install read alone; a run directory that is no folder
// fails with play.install_copy, nothing launched.
static int test_install_staging() {
	namespace fs = std::filesystem;
	editor_test::TempProjectDir dir("opennova_editor_install_staging");
	const std::string install = dir.file("install"), build = dir.file("build/0123456789abcdef"),
	                  run = dir.file("run/1");
	TEST_EXPECT(editor_test::write_text(install + "/Jointops.exe", "exe") && editor_test::write_text(install + "/binkw32.dll", "bink") &&
	            editor_test::write_text(install + "/game.cfg", "install settings") &&
	            editor_test::write_text(install + "/player.sav", "install player") &&
	            editor_test::write_text(install + "/weapon.sav", "install weapon"));
	for (const char *name : {"language.pff", "localres.pff", "resource.pff"})
		TEST_EXPECT(editor_test::write_text(build + "/" + name, std::string("PFF3 ") + name));
	TEST_EXPECT(editor_test::write_text(build + "/intro.bik", "video") && editor_test::write_text(build + "/build.json", "{}") &&
	            editor_test::write_text(build + "/GAME.CFG", "project settings") &&
	            editor_test::write_text(build + "/weapon.sav", "project weapon") &&
	            editor_test::write_text(build + "/filter.txt", "filter") &&
	            editor_test::write_text(build + "/menumus.sbf", "music"));
	fs::create_directories(run);
	const std::string tree = editor_test::tree_digest(build);
	LaunchPlan plan;
	Diagnostic error;
	TEST_EXPECT(prepare_retail_launch_plan(install, build, run, plan, error));
	TEST_EXPECT(plan.executable == run + "/Jointops.exe" && plan.working_dir == run && plan.build_dir == build &&
	            plan.log_file == run + "/_filelog.txt");
	std::string text, io_error;
	std::error_code ec;
	for (const char *name : {"language.pff", "localres.pff", "resource.pff", "intro.bik", "menumus.sbf"}) {
		TEST_EXPECT(read_file_text(run + "/" + name, text, io_error) && read_file_text(build + "/" + name, io_error, io_error));
		TEST_EXPECT(fs::equivalent(run + "/" + name, build + "/" + name, ec)); // linked: the game only reads it
	}
	TEST_EXPECT(read_file_text(run + "/intro.bik", text, io_error) && text == "video");
	for (const char *name : {"filter.txt", "weapon.sav"})
		TEST_EXPECT(!fs::equivalent(run + "/" + name, build + "/" + name, ec)); // copied: the game may write it
	TEST_EXPECT(read_file_text(run + "/game.cfg", text, io_error) && text == "project settings");
	TEST_EXPECT(read_file_text(run + "/weapon.sav", text, io_error) && text == "project weapon"); // the project's own
	TEST_EXPECT(read_file_text(run + "/player.sav", text, io_error) && text == "install player"); // the install's
	TEST_EXPECT(read_file_text(run + "/binkw32.dll", text, io_error) && text == "bink");
	TEST_EXPECT(!fs::exists(run + "/build.json") && editor_test::tree_digest(build) == tree);
	TEST_EXPECT(read_file_text(install + "/game.cfg", text, io_error) && text == "install settings");
	// The files the game writes in its run directory leave the build's and the install's as they were.
	TEST_EXPECT(editor_test::write_text(run + "/game.cfg", "rewritten") && editor_test::write_text(run + "/weapon.sav", "saved") &&
	            editor_test::write_text(run + "/player.sav", "saved") && editor_test::write_text(run + "/filter.txt", "edited") &&
	            editor_test::tree_digest(build) == tree);
	TEST_EXPECT(read_file_text(install + "/player.sav", text, io_error) && text == "install player");

	const std::string squat = dir.file("not a folder");
	TEST_EXPECT(editor_test::write_text(squat, "a file"));
	TEST_EXPECT(!prepare_retail_launch_plan(install, build, squat, plan, error) && error.code() == "play.install_copy");
	std::filesystem::remove(install + "/Jointops.exe");
	TEST_EXPECT(!prepare_retail_launch_plan(install, build, dir.file("run/2"), plan, error) &&
	            error.code() == "play.install_missing" && !std::filesystem::exists(dir.file("run/2/game.cfg")));
	return 0;
}

// ADR 0046 S16: an expansion's build runs from its run directory with /exp: the runtime's
// --resource-dir names the run directory (the install's base game and the build's expansion folder
// prepare_expansion_run put there), not the build; a source run alike.
static int test_expansion_launch_plans() {
	const LaunchPlan play = make_play_launch_plan("opennova.exe", "C:/p/build/abc", "C:/p/run/1", "jo", 8975, "m.bms", {},
	                                              "jxm");
	TEST_EXPECT(play.build_dir == "C:/p/build/abc" && play.resource_dir == "C:/p/run/1" && play.expansion == "jxm");
	const std::vector<std::string> expected = {"--log-file", "C:/p/run/1/session.log", "--", "--resource-dir", "C:/p/run/1",
	                                           "/exp", "jxm", "/game", "jo", "--mcp-port", "8975", "--mission", "m.bms"};
	TEST_EXPECT(play.args == expected);
	const LaunchPlan source = make_source_launch_plan("godot", "C:/repo/godot", "C:/p/build", "C:/p/run/2", "jo", 0, "", {},
	                                                  "jxm");
	const auto at = std::find(source.args.begin(), source.args.end(), "--resource-dir");
	TEST_EXPECT(at != source.args.end() && *(at + 1) == "C:/p/run/2" && *(at + 2) == "/exp" && *(at + 3) == "jxm");
	const LaunchPlan standalone = make_play_launch_plan("opennova.exe", "C:/p/build/abc", "C:/p/run/1", "jo", 0);
	TEST_EXPECT(standalone.resource_dir == "C:/p/build/abc" && standalone.expansion.empty());
	TEST_EXPECT(std::find(standalone.args.begin(), standalone.args.end(), "/exp") == standalone.args.end());
	return 0;
}

// ADR 0046 S16: an expansion's run directory. The install's boot archives and the loose files it
// ships beside them linked at its root (nothing else of the install: not its notes, not its own
// expansions); expansion/jxm a directory of the run's own, the build's archives and video linked
// into it and its version.txt copied (the game may write a .txt); the run directory mounts with
// /exp jxm as an install does; the install and the build read alone. The stock game staged the same
// way, its executable, Bink DLL, configuration and saves the install's, launched /w /d /exp jxm
// /FRISK. Where an install's file cannot be linked (another volume), its copy in the copy cache is,
// copied once and kept while the install's file keeps its size and last write. Refused: no install,
// an install with none of the game's archives (play.install_missing), a build with no expansion
// folder (play.install_copy).
static int test_expansion_staging() {
	namespace fs = std::filesystem;
	editor_test::TempProjectDir dir("opennova_editor_expansion_staging");
	const std::string install = dir.file("install"), build = dir.file("build/0123456789abcdef"), cache = dir.file("copies");
	for (const char *name : {"language.pff", "localres.pff", "resource.pff"})
		TEST_EXPECT(write_empty_archive(install + "/" + name));
	TEST_EXPECT(editor_test::write_text(install + "/intro.bik", "video") && editor_test::write_text(install + "/menumus.sbf", "music") &&
	            editor_test::write_text(install + "/cc.bin", "us") && editor_test::write_text(install + "/Readme.txt", "notes") &&
	            editor_test::write_text(install + "/expansion/jox01/jox01.pff", "theirs") &&
	            editor_test::write_text(install + "/Jointops.exe", "exe") && editor_test::write_text(install + "/binkw32.dll", "bink") &&
	            editor_test::write_text(install + "/game.cfg", "install settings") &&
	            editor_test::write_text(install + "/player.sav", "install player"));
	TEST_EXPECT(write_empty_archive(build + "/expansion/jxm/jxm.pff") && write_empty_archive(build + "/expansion/jxm/jxmL.pff"));
	TEST_EXPECT(editor_test::write_text(build + "/expansion/jxm/header.bik", "their video") &&
	            editor_test::write_text(build + "/expansion/jxm/version.txt", "1.0") &&
	            editor_test::write_text(build + "/build.json", "{}"));
	const std::string build_tree = editor_test::tree_digest(build), install_tree = editor_test::tree_digest(install);
	const std::string run = dir.file("run/1");
	fs::create_directories(run);
	Diagnostic error;
	TEST_EXPECT(prepare_expansion_run(install, build, "jxm", run, cache, error));
	std::error_code ec;
	for (const char *name : {"language.pff", "localres.pff", "resource.pff", "intro.bik", "menumus.sbf", "cc.bin"})
		TEST_EXPECT(fs::equivalent(run + "/" + name, install + "/" + name, ec));
	TEST_EXPECT(!fs::exists(run + "/Readme.txt") && !fs::exists(run + "/expansion/jox01") && !fs::exists(run + "/build.json"));
	TEST_EXPECT(fs::is_directory(run + "/expansion/jxm") && !fs::is_symlink(run + "/expansion/jxm"));
	for (const char *name : {"jxm.pff", "jxmL.pff", "header.bik"})
		TEST_EXPECT(fs::equivalent(run + "/expansion/jxm/" + name, build + "/expansion/jxm/" + name, ec));
	TEST_EXPECT(!fs::equivalent(run + "/expansion/jxm/version.txt", build + "/expansion/jxm/version.txt", ec));
	TEST_EXPECT(!fs::exists(cache)); // every file linked: no copy made
	{
		opennova::Vfs vfs;
		opennova::LaunchFlags flags;
		flags.expansion = "jxm";
		TEST_EXPECT(opennova::mount_install(vfs, run, flags) && vfs.mounted_expansion() == "jxm");
	}
	// The game writes its expansion's weapon.sav beside the expansion's files: the build keeps its own.
	TEST_EXPECT(editor_test::write_text(run + "/expansion/jxm/weapon.sav", "saved") &&
	            editor_test::write_text(run + "/expansion/jxm/version.txt", "edited"));
	TEST_EXPECT(editor_test::tree_digest(build) == build_tree && editor_test::tree_digest(install) == install_tree);

	// The stock game: the same staging, the install's own binaries, configuration and saves beside it.
	LaunchPlan plan;
	const std::string run2 = dir.file("run/2");
	fs::create_directories(run2);
	TEST_EXPECT(prepare_retail_launch_plan(install, build, run2, plan, error, "jxm", cache));
	TEST_EXPECT(plan.args == std::vector<std::string>({"/w", "/d", "/exp", "jxm", "/FRISK"}));
	TEST_EXPECT(plan.executable == run2 + "/Jointops.exe" && plan.resource_dir == run2 && plan.expansion == "jxm");
	std::string text, io_error;
	TEST_EXPECT(read_file_text(run2 + "/game.cfg", text, io_error) && text == "install settings");
	TEST_EXPECT(read_file_text(run2 + "/player.sav", text, io_error) && text == "install player");
	TEST_EXPECT(fs::equivalent(run2 + "/expansion/jxm/jxm.pff", build + "/expansion/jxm/jxm.pff", ec));

	// Another volume: the install's files are copied once into the copy cache and linked from there.
	const FileLink across = [&install](const std::string &from, const std::string &to, std::string &reason) {
		if (from.rfind(install, 0) == 0) {
			reason = "another volume";
			return false;
		}
		return link_file(from, to, reason);
	};
	const std::string run3 = dir.file("run/3"), run4 = dir.file("run/4");
	fs::create_directories(run3);
	fs::create_directories(run4);
	TEST_EXPECT(prepare_expansion_run(install, build, "jxm", run3, cache, error, across));
	TEST_EXPECT(!fs::equivalent(run3 + "/intro.bik", install + "/intro.bik", ec));
	TEST_EXPECT(read_file_text(run3 + "/intro.bik", text, io_error) && text == "video");
	std::vector<std::string> cached;
	for (const fs::directory_entry &key : fs::directory_iterator(cache, ec))
		for (const fs::directory_entry &file : fs::directory_iterator(key.path(), ec)) cached.push_back(file.path().filename().string());
	std::sort(cached.begin(), cached.end());
	TEST_EXPECT(cached == std::vector<std::string>({"cc.bin", "install_copy.json", "intro.bik", "language.pff", "localres.pff",
	                                               "menumus.sbf", "resource.pff"}));
	const std::string copy = cache + "/" + fs::directory_iterator(cache)->path().filename().string() + "/intro.bik";
	TEST_EXPECT(fs::equivalent(run3 + "/intro.bik", copy, ec)); // linked from the cache
	// The next run takes the same copy; an install file that changed is copied again.
	TEST_EXPECT(prepare_expansion_run(install, build, "jxm", run4, cache, error, across));
	TEST_EXPECT(fs::equivalent(run4 + "/intro.bik", copy, ec));
	TEST_EXPECT(editor_test::write_text(install + "/intro.bik", "a new video"));
	const std::string run5 = dir.file("run/5");
	fs::create_directories(run5);
	TEST_EXPECT(prepare_expansion_run(install, build, "jxm", run5, cache, error, across));
	TEST_EXPECT(read_file_text(run5 + "/intro.bik", text, io_error) && text == "a new video");
	TEST_EXPECT(read_file_text(run4 + "/intro.bik", text, io_error) && text == "video"); // the earlier run keeps its own

	// Refused: no install, one with none of the game's archives, a build without the folder.
	TEST_EXPECT(!prepare_expansion_run("", build, "jxm", dir.file("run/6"), cache, error) &&
	            error.code() == "play.install_missing");
	TEST_EXPECT(editor_test::write_text(dir.file("empty/readme.txt"), "x"));
	TEST_EXPECT(!prepare_expansion_run(dir.file("empty"), build, "jxm", dir.file("run/6"), cache, error) &&
	            error.code() == "play.install_missing");
	TEST_EXPECT(!prepare_expansion_run(install, build, "jxn", dir.file("run/6"), cache, error) &&
	            error.code() == "play.install_copy");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_launch_plans();
	failures += test_expansion_launch_plans();
	failures += test_install_staging();
	failures += test_expansion_staging();
	failures += test_lifecycle();
	if (failures == 0) std::printf("editor_play_session: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
