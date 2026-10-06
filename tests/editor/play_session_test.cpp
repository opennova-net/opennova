// Pins the Play launch plan and the one managed child (ADR 0046 d8/d10) over a fake
// platform: the argument vector, where a packaged editor finds its runtime, the run directory the
// game works in and what the game install's game finds there (S13 A8; Strict Play's: the build and the
// program alone, no /d), the game install's file log and Strict Play's first-run start again, one child at a
// time, the stop request, the deadline kill, exit on its own with the code it exited with,
// and the build directory the session protects while alive.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <base/io/os_path.h>
#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <editor/project/project_files.h>
#include <editor/run/behind_start.h>
#include <editor/run/launch_plan.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/pff/pff.h>
#include <editor/run/play_session.h>
#include <editor/run/play_start.h>
#include <editor/run/run_directory.h>

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

// A game started behind (review X12: BehindStarts, what the Shell's process platform does each pump): the
// foreground lock serves only while it starts, until its first window went back once or kLockMs passed; its shown
// windows go back each pump until one is the foreground window (the person brought it forward: let go, never
// pushed back again), it exits, or kTendMs passed.
static int test_behind_starts() {
	using Step = BehindStarts::Step;
	BehindStarts behind;
	Step step = Step::Leave;
	TEST_EXPECT(!behind.tending() && !behind.lock_wanted(0));
	behind.spawned(40, 1000);
	TEST_EXPECT(behind.tending() && behind.lock_wanted(1000));
	// No window yet: left, the lock kept.
	TEST_EXPECT(behind.tend(40, 1100, false, false, false, step) && step == Step::Leave && behind.lock_wanted(1100));
	// Its first window shown: sent back, and the lock no longer serves (the person's desktop is theirs again).
	TEST_EXPECT(behind.tend(40, 1300, false, true, false, step) && step == Step::SendBack && !behind.lock_wanted(1300));
	TEST_EXPECT(behind.tend(40, 5000, false, true, false, step) && step == Step::SendBack);
	// Brought forward: let go, never pushed back again.
	TEST_EXPECT(!behind.tend(40, 6000, false, true, true, step) && step == Step::Leave && !behind.tending());
	TEST_EXPECT(!behind.tend(40, 6100, false, true, false, step));
	// A game that shows no window: the lock goes once kLockMs passed; tended until kTendMs, or until it exits.
	behind.spawned(41, 0);
	TEST_EXPECT(behind.lock_wanted(BehindStarts::kLockMs - 1) && !behind.lock_wanted(BehindStarts::kLockMs));
	TEST_EXPECT(behind.tend(41, BehindStarts::kTendMs - 1, false, false, false, step) && step == Step::Leave);
	TEST_EXPECT(!behind.tend(41, BehindStarts::kTendMs, false, true, false, step) && !behind.tending());
	behind.spawned(42, 0);
	TEST_EXPECT(!behind.tend(42, 10, true, false, false, step) && !behind.tending() && !behind.lock_wanted(10));
	return 0;
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
// anything is copied, then the build's files (one the game only reads, its archives and a video among
// them, linked, or copied where the file system cannot link it; every other, which the game may write,
// copied; its record left behind), the install's executable and Bink DLL, a game.cfg (the build's own
// over the install's) and the install's saves and the files it reads by name (score.ini, earlyerr.txt)
// where the project has none of its own put there, the build directory and the install read alone; a run directory that is no folder
// fails with play.install_copy, nothing launched.
static int test_install_staging() {
	namespace fs = std::filesystem;
	editor_test::TempProjectDir dir("opennova_editor_install_staging");
	const std::string install = dir.file("install"), build = dir.file("build/0123456789abcdef"),
	                  run = dir.file("run/1");
	TEST_EXPECT(editor_test::write_text(install + "/Jointops.exe", "exe") && editor_test::write_text(install + "/binkw32.dll", "bink") &&
	            editor_test::write_text(install + "/game.cfg", "install settings") &&
	            editor_test::write_text(install + "/player.sav", "install player") &&
	            editor_test::write_text(install + "/weapon.sav", "install weapon") &&
	            editor_test::write_text(install + "/score.ini", "install scores") &&
	            editor_test::write_text(install + "/EARLYERR.TXT", "install early"));
	for (const char *name : {"language.pff", "localres.pff", "resource.pff"})
		TEST_EXPECT(editor_test::write_text(build + "/" + name, std::string("PFF3 ") + name));
	TEST_EXPECT(editor_test::write_text(build + "/intro.bik", "video") && editor_test::write_text(build + "/build.json", "{}") &&
	            editor_test::write_text(build + "/GAME.CFG", "project settings") &&
	            editor_test::write_text(build + "/weapon.sav", "project weapon") &&
	            editor_test::write_text(build + "/filter.txt", "filter") &&
	            editor_test::write_text(build + "/menumus.sbf", "music") &&
	            editor_test::write_text(build + "/Score.ini", "project scores"));
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
	// The files the game reads from its folder by name: the project's own, else the install's, copied.
	TEST_EXPECT(read_file_text(run + "/Score.ini", text, io_error) && text == "project scores" &&
	            !fs::equivalent(run + "/Score.ini", build + "/Score.ini", ec));
	TEST_EXPECT(read_file_text(run + "/EARLYERR.TXT", text, io_error) && text == "install early" &&
	            !fs::equivalent(run + "/EARLYERR.TXT", install + "/EARLYERR.TXT", ec));
	TEST_EXPECT(!fs::exists(run + "/build.json") && editor_test::tree_digest(build) == tree);
	TEST_EXPECT(read_file_text(install + "/game.cfg", text, io_error) && text == "install settings");
	// The files the game writes in its run directory leave the build's and the install's as they were.
	TEST_EXPECT(editor_test::write_text(run + "/game.cfg", "rewritten") && editor_test::write_text(run + "/weapon.sav", "saved") &&
	            editor_test::write_text(run + "/player.sav", "saved") && editor_test::write_text(run + "/filter.txt", "edited") &&
	            editor_test::tree_digest(build) == tree);
	TEST_EXPECT(read_file_text(install + "/player.sav", text, io_error) && text == "install player");
	TEST_EXPECT(editor_test::write_text(run + "/EARLYERR.TXT", "edited") && editor_test::write_text(run + "/Score.ini", "edited") &&
	            read_file_text(install + "/EARLYERR.TXT", text, io_error) && text == "install early" &&
	            editor_test::tree_digest(build) == tree);

	const std::string squat = dir.file("not a folder");
	TEST_EXPECT(editor_test::write_text(squat, "a file"));
	TEST_EXPECT(!prepare_retail_launch_plan(install, build, squat, plan, error) && error.code() == "play.install_copy");
	std::filesystem::remove(install + "/Jointops.exe");
	TEST_EXPECT(!prepare_retail_launch_plan(install, build, dir.file("run/2"), plan, error) &&
	            error.code() == "play.install_missing" && !std::filesystem::exists(dir.file("run/2/game.cfg")));
	return 0;
}

static std::vector<std::string> files_in(const std::string &dir);

// Strict Play in the game install: the run directory holds the build's files and the install's
// executable and Bink DLL, nothing else of the install (no game.cfg, save, score table, early error text
// or admin configuration), the build's own game.cfg among the build's files; one the game only reads
// linked (copied where the file system will not link it: `link` injected), every other copied, the
// build's record left out; launched /w /FRISK, no /d. The game writing every file it may write leaves
// the build and the install as they were. Refused, nothing staged: an expansion (play.strict_expansion),
// no install, an install without its program (play.install_missing).
static int test_strict_install_staging() {
	namespace fs = std::filesystem;
	editor_test::TempProjectDir dir("opennova_editor_strict_staging");
	const std::string install = dir.file("install"), build = dir.file("build/0123456789abcdef");
	TEST_EXPECT(editor_test::write_text(install + "/Jointops.exe", "exe") && editor_test::write_text(install + "/binkw32.dll", "shim") &&
	            editor_test::write_text(install + "/binkw32_.dll", "bink") &&
	            editor_test::write_text(install + "/game.cfg", "install settings") &&
	            editor_test::write_text(install + "/player.sav", "install player") &&
	            editor_test::write_text(install + "/weapon.sav", "install weapon") &&
	            editor_test::write_text(install + "/score.ini", "install scores") &&
	            editor_test::write_text(install + "/earlyerr.txt", "install early") &&
	            editor_test::write_text(install + "/admin.cfg", "install admin") &&
	            editor_test::write_text(install + "/menumus.sbf", "install music"));
	for (const char *name : {"language.pff", "localres.pff", "resource.pff"})
		TEST_EXPECT(write_empty_archive(build + "/" + name));
	TEST_EXPECT(editor_test::write_text(build + "/nw_cdata.coo", "cookies") && editor_test::write_text(build + "/build.json", "{}") &&
	            editor_test::write_text(build + "/intro.bik", "video"));
	const std::string build_tree = editor_test::tree_digest(build), install_tree = editor_test::tree_digest(install);
	const std::string run = dir.file("run/1");
	fs::create_directories(run);
	LaunchPlan plan;
	Diagnostic error;
	TEST_EXPECT(prepare_strict_install_launch_plan(install, build, run, "", plan, error));
	TEST_EXPECT(plan.args == std::vector<std::string>({"/w", "/FRISK"}));
	TEST_EXPECT(std::find(plan.args.begin(), plan.args.end(), "/d") == plan.args.end());
	TEST_EXPECT(plan.executable == run + "/Jointops.exe" && plan.working_dir == run && plan.build_dir == build &&
	            plan.resource_dir == run && plan.expansion.empty() && plan.log_file == run + "/_filelog.txt" &&
	            plan.mcp_port == 0);
	std::vector<std::string> held = files_in(run);
	TEST_EXPECT(held == std::vector<std::string>({"Jointops.exe", "binkw32.dll", "intro.bik", "language.pff", "localres.pff",
	                                              "nw_cdata.coo", "resource.pff"}));
	std::string text, io_error;
	std::error_code ec;
	TEST_EXPECT(read_file_text(run + "/binkw32.dll", text, io_error) && text == "bink"); // JOTAC's real Bink over the shim
	for (const char *name : {"language.pff", "localres.pff", "resource.pff", "intro.bik"})
		TEST_EXPECT(fs::equivalent(run + "/" + name, build + "/" + name, ec)); // linked: the game only reads it
	TEST_EXPECT(!fs::equivalent(run + "/nw_cdata.coo", build + "/nw_cdata.coo", ec)); // copied: the game rewrites it
	TEST_EXPECT(!fs::equivalent(run + "/Jointops.exe", install + "/Jointops.exe", ec));
	for (const char *name : {"nw_cdata.coo", "Jointops.exe", "binkw32.dll"})
		TEST_EXPECT(editor_test::write_text(run + "/" + name, "written by the game"));
	for (const char *name : {"game.cfg", "player.sav", "weapon.sav", "score.ini", "_filelog.txt"})
		TEST_EXPECT(editor_test::write_text(run + "/" + name, "written by the game"));
	TEST_EXPECT(editor_test::tree_digest(build) == build_tree && editor_test::tree_digest(install) == install_tree);

	// A game.cfg the project holds is the build's own: staged with the rest, copied (the game rewrites it).
	// Another volume: the files the game only reads are copied too.
	TEST_EXPECT(editor_test::write_text(build + "/game.cfg", "project settings"));
	const FileLink across = [](const std::string &, const std::string &, std::string &reason) {
		reason = "another volume";
		return false;
	};
	const std::string run2 = dir.file("run/2");
	fs::create_directories(run2);
	TEST_EXPECT(prepare_strict_install_launch_plan(install, build, run2, "", plan, error, across));
	TEST_EXPECT(read_file_text(run2 + "/game.cfg", text, io_error) && text == "project settings" &&
	            !fs::equivalent(run2 + "/game.cfg", build + "/game.cfg", ec));
	TEST_EXPECT(!fs::equivalent(run2 + "/language.pff", build + "/language.pff", ec) && fs::is_regular_file(run2 + "/language.pff"));
	TEST_EXPECT(!fs::exists(run2 + "/player.sav") && !fs::exists(run2 + "/score.ini") && !fs::exists(run2 + "/build.json"));

	// Refused, nothing staged.
	const std::string run3 = dir.file("run/3");
	fs::create_directories(run3);
	TEST_EXPECT(!prepare_strict_install_launch_plan(install, build, run3, "jxm", plan, error) &&
	            error.code() == "play.strict_expansion" &&
	            error.message.find("Strict Play of an expansion needs its base game's build; not yet supported") == 0 &&
	            files_in(run3).empty());
	TEST_EXPECT(!prepare_strict_install_launch_plan("", build, run3, "", plan, error) && error.code() == "play.install_missing");
	fs::remove(install + "/Jointops.exe");
	TEST_EXPECT(!prepare_strict_install_launch_plan(install, build, run3, "", plan, error) &&
	            error.code() == "play.install_missing" && files_in(run3).empty());
	return 0;
}

// The game install's file log, as /FRISK writes it [orig: File_LogFileAccess @ 0x75a480]: an archive's
// entry "PFF LOADED FILE: <name>", a file from disk "LOADED FILE: <path>", an archive itself among those,
// lines ended "\n" (the game's) or "\r\n"; each name once (compared without case, the first spelling
// kept), in first-open order; a line of neither form counted alone.
static int test_file_access_log() {
	const FileAccessLog log = parse_file_access_log("LOADED FILE: language.pff\n"
	                                                "LOADED FILE: localres.pff\r\n"
	                                                "LOADED FILE: RESOURCE.PFF\n"
	                                                "PFF LOADED FILE: gameerr.bin\n"
	                                                "PFF LOADED FILE: weapon.def\n"
	                                                "PFF LOADED FILE: WEAPON.DEF\n"
	                                                "LOADED FILE: player.sav\n"
	                                                "LOADED FILE: expansion\\jxm\\jxm.bin\n"
	                                                "something else\n"
	                                                "PFF LOADED FILE: main.mnu");
	TEST_EXPECT(log.lines == 10);
	TEST_EXPECT(log.archives == std::vector<std::string>({"language.pff", "localres.pff", "RESOURCE.PFF"}));
	TEST_EXPECT(log.from_archives == std::vector<std::string>({"gameerr.bin", "weapon.def", "main.mnu"}));
	TEST_EXPECT(log.from_disk == std::vector<std::string>({"player.sav", "expansion\\jxm\\jxm.bin"}));
	TEST_EXPECT(parse_file_access_log("") == FileAccessLog());
	FileAccessLog one;
	add_file_access_line(one, "PFF LOADED FILE: keyhelp.bin");
	TEST_EXPECT(one.lines == 1 && one.from_archives == std::vector<std::string>({"keyhelp.bin"}) && one.archives.empty());
	return 0;
}

// Strict Play's first run is started once more only when the run directory held no game.cfg before it,
// holds one now, and the game exited on its own with code 0 within kStrictFirstRunWindowMs of its start;
// never a second time, never for a stop, an error exit or an exit the platform could not read, never for
// a game that ran past the window.
static int test_strict_first_run_decision() {
	const int64_t soon = 4000;
	TEST_EXPECT(strict_first_run_starts_again(false, true, true, 0, soon, false));
	TEST_EXPECT(strict_first_run_starts_again(false, true, true, 0, kStrictFirstRunWindowMs, false));
	TEST_EXPECT(!strict_first_run_starts_again(false, true, true, 0, kStrictFirstRunWindowMs + 1, false));
	TEST_EXPECT(!strict_first_run_starts_again(true, true, true, 0, soon, false));   // it had one (the build's)
	TEST_EXPECT(!strict_first_run_starts_again(false, false, true, 0, soon, false)); // it wrote none
	TEST_EXPECT(!strict_first_run_starts_again(false, true, false, -1, soon, false)); // stopped
	TEST_EXPECT(!strict_first_run_starts_again(false, true, true, 1, soon, false));  // an error exit
	TEST_EXPECT(!strict_first_run_starts_again(false, true, true, -1, soon, false)); // no code read
	TEST_EXPECT(!strict_first_run_starts_again(false, true, true, 0, soon, true));   // once only
	TEST_EXPECT(!strict_first_run_starts_again(false, true, true, 0, -5, false));
	return 0;
}

// `names` sorted (a staging names its files in the build's directory order).
static std::vector<std::string> sorted(std::vector<std::string> names) {
	std::sort(names.begin(), names.end());
	return names;
}

// The run directory keeps the game's state between Plays (run/run_directory.h). A Play of the same mode
// takes it with what the game wrote there (its game.cfg, which names the adapter its device dialog chose,
// its device log, its saves, its scores), what the Play before staged, the logs a run writes and the game's
// record gone, and stages again: a file the build no longer holds is gone, the build's own cookie jar
// staged again over the one the game rewrote, a build file staged by the name of a file the run kept in
// place of it (removed by its name, never written through). Strict Play's rule holds in a kept directory:
// nothing of the install but its program and Bink DLL. A fresh take, or one with no staging record, is
// emptied; a staging record naming a path outside the directory removes nothing there. Lenient Play runs
// in a directory of its own mode, strict's left as its game wrote it. Lenient Play seeds the install's game.cfg and saves only where the run directory holds none of its
// own (an install copy written since included), names no seed among what it staged, and needs no install
// game.cfg once the run holds its own.
static int test_run_directory_keeps_game_state() {
	namespace fs = std::filesystem;
	editor_test::TempProjectDir dir("opennova_editor_run_keeps_state");
	const std::string install = dir.file("install"), build = dir.file("build/0123456789abcdef"), runs = dir.file("runs");
	TEST_EXPECT(editor_test::write_text(install + "/Jointops.exe", "exe") && editor_test::write_text(install + "/binkw32.dll", "bink") &&
	            editor_test::write_text(install + "/game.cfg", "install settings") &&
	            editor_test::write_text(install + "/player.sav", "install player") &&
	            editor_test::write_text(install + "/score.ini", "install scores"));
	for (const char *name : {"language.pff", "localres.pff", "resource.pff"})
		TEST_EXPECT(write_empty_archive(build + "/" + name));
	TEST_EXPECT(editor_test::write_text(build + "/nw_cdata.coo", "cookies") && editor_test::write_text(build + "/intro.bik", "video") &&
	            editor_test::write_text(build + "/build.json", "{}"));
	const LeaseLiveness gone = [](int64_t, const std::string &) { return ProcessLiveness::Dead; };
	std::string run, run_error, text, io_error;
	std::vector<std::string> kept;
	LaunchPlan plan;
	Diagnostic error;
	std::error_code ec;
	const RunTake strict{kRunModeStrict, false}, strict_fresh{kRunModeStrict, true}, lenient{kRunModeInstall, false};

	// The first strict Play: strict/1, empty; what it staged named and recorded.
	TEST_EXPECT(take_run_directory(runs, gone, strict, run, kept, run_error) && run == runs + "/strict/1" && kept.empty());
	TEST_EXPECT(prepare_strict_install_launch_plan(install, build, run, "", plan, error));
	TEST_EXPECT(sorted(plan.staged) == std::vector<std::string>({"Jointops.exe", "binkw32.dll", "intro.bik", "language.pff",
	                                                             "localres.pff", "nw_cdata.coo", "resource.pff"}));
	TEST_EXPECT(record_run_staging(run, RunStaging{kRunModeStrict, plan.staged}, run_error));
	// The game writes its own files, rewrites the cookie jar, logs its file opens; a record of it is left.
	TEST_EXPECT(editor_test::write_text(run + "/game.cfg", "names the adapter") && editor_test::write_text(run + "/ghw.txt", "device log") &&
	            editor_test::write_text(run + "/player.sav", "the game's player") &&
	            editor_test::write_text(run + "/weapon.sav", "the game's weapons") &&
	            editor_test::write_text(run + "/hiscore.txt", "scores") &&
	            editor_test::write_text(run + "/_filelog.txt", "LOADED FILE: language.pff\n") &&
	            editor_test::write_text(run + "/nw_cdata.coo", "rewritten by the game") &&
	            editor_test::write_text(run + "/run.json", "{}") && editor_test::write_text(run + "/session.log", "a log"));
	// The build changes: its video gone, a file added.
	fs::remove(build + "/intro.bik");
	TEST_EXPECT(editor_test::write_text(build + "/filter.txt", "filter"));
	const std::string install_tree = editor_test::tree_digest(install);

	// The next strict Play keeps what the game wrote, and nothing else.
	TEST_EXPECT(take_run_directory(runs, gone, strict, run, kept, run_error) && run == runs + "/strict/1");
	const std::vector<std::string> game_state = {"game.cfg", "ghw.txt", "hiscore.txt", "player.sav", "weapon.sav"};
	TEST_EXPECT(kept == game_state && files_in(run) == game_state);
	TEST_EXPECT(prepare_strict_install_launch_plan(install, build, run, "", plan, error));
	TEST_EXPECT(record_run_staging(run, RunStaging{kRunModeStrict, plan.staged}, run_error));
	TEST_EXPECT(files_in(run) == std::vector<std::string>({"Jointops.exe", "binkw32.dll", "filter.txt", "game.cfg", "ghw.txt",
	                                                       "hiscore.txt", "language.pff", "localres.pff", "nw_cdata.coo",
	                                                       "player.sav", "resource.pff", "staging.json", "weapon.sav"}));
	TEST_EXPECT(read_file_text(run + "/game.cfg", text, io_error) && text == "names the adapter");
	TEST_EXPECT(read_file_text(run + "/player.sav", text, io_error) && text == "the game's player"); // never the install's
	TEST_EXPECT(read_file_text(run + "/nw_cdata.coo", text, io_error) && text == "cookies" &&
	            !fs::equivalent(run + "/nw_cdata.coo", build + "/nw_cdata.coo", ec));
	TEST_EXPECT(fs::equivalent(run + "/language.pff", build + "/language.pff", ec));
	TEST_EXPECT(!fs::exists(run + "/intro.bik") && !fs::exists(run + "/score.ini") && !fs::exists(run + "/_filelog.txt"));
	// A file the build now holds under the name of one the run kept: the project's own staged in its place,
	// the build's file as it was.
	TEST_EXPECT(editor_test::write_text(build + "/player.sav", "project player"));
	TEST_EXPECT(take_run_directory(runs, gone, strict, run, kept, run_error) && kept == game_state);
	TEST_EXPECT(prepare_strict_install_launch_plan(install, build, run, "", plan, error));
	TEST_EXPECT(record_run_staging(run, RunStaging{kRunModeStrict, plan.staged}, run_error));
	TEST_EXPECT(read_file_text(run + "/player.sav", text, io_error) && text == "project player" &&
	            !fs::equivalent(run + "/player.sav", build + "/player.sav", ec));
	TEST_EXPECT(editor_test::write_text(run + "/player.sav", "rewritten") && read_file_text(build + "/player.sav", text, io_error) &&
	            text == "project player");
	fs::remove(build + "/player.sav");
	TEST_EXPECT(editor_test::tree_digest(install) == install_tree);

	// Fresh: emptied, whatever the game wrote.
	TEST_EXPECT(take_run_directory(runs, gone, strict_fresh, run, kept, run_error) && run == runs + "/strict/1" && kept.empty() &&
	            files_in(run).empty());
	TEST_EXPECT(prepare_strict_install_launch_plan(install, build, run, "", plan, error));
	TEST_EXPECT(record_run_staging(run, RunStaging{kRunModeStrict, plan.staged}, run_error));
	TEST_EXPECT(editor_test::write_text(run + "/game.cfg", "names the adapter"));

	// Lenient Play takes a directory of its own mode (strict's state is not lenient's), strict's left as its
	// game wrote it; it seeds the install's game.cfg and saves there, and names none of them as staged.
	TEST_EXPECT(take_run_directory(runs, gone, lenient, run, kept, run_error) && run == runs + "/install/1" &&
	            kept.empty() && files_in(run).empty());
	TEST_EXPECT(read_file_text(runs + "/strict/1/game.cfg", text, io_error) && text == "names the adapter");
	TEST_EXPECT(prepare_retail_launch_plan(install, build, run, plan, error));
	TEST_EXPECT(record_run_staging(run, RunStaging{kRunModeInstall, plan.staged}, run_error));
	for (const char *seed : {"game.cfg", "player.sav", "score.ini"})
		TEST_EXPECT(std::find(plan.staged.begin(), plan.staged.end(), seed) == plan.staged.end());
	TEST_EXPECT(read_file_text(run + "/game.cfg", text, io_error) && text == "install settings");
	// The game rewrites its game.cfg, and the install's is played since: the run's own is kept, newer or not.
	TEST_EXPECT(editor_test::write_text(run + "/game.cfg", "adjusted in the run") &&
	            editor_test::write_text(install + "/game.cfg", "install settings, played since"));
	TEST_EXPECT(take_run_directory(runs, gone, lenient, run, kept, run_error) &&
	            kept == std::vector<std::string>({"game.cfg", "player.sav", "score.ini"}));
	TEST_EXPECT(prepare_retail_launch_plan(install, build, run, plan, error));
	TEST_EXPECT(record_run_staging(run, RunStaging{kRunModeInstall, plan.staged}, run_error));
	TEST_EXPECT(read_file_text(run + "/game.cfg", text, io_error) && text == "adjusted in the run");
	// An install with no game.cfg: a run that holds its own plays all the same.
	fs::remove(install + "/game.cfg");
	TEST_EXPECT(take_run_directory(runs, gone, lenient, run, kept, run_error) &&
	            kept == std::vector<std::string>({"game.cfg", "player.sav", "score.ini"}));
	TEST_EXPECT(prepare_retail_launch_plan(install, build, run, plan, error));
	TEST_EXPECT(read_file_text(run + "/game.cfg", text, io_error) && text == "adjusted in the run");

	// A directory with no staging record (an older editor's, one whose record could not be written: none is
	// written above) is emptied; a record naming a path outside the directory removes nothing there.
	TEST_EXPECT(!fs::exists(run + "/" + kRunStagingFileName) && fs::is_regular_file(run + "/Jointops.exe"));
	TEST_EXPECT(take_run_directory(runs, gone, lenient, run, kept, run_error) && kept.empty() && files_in(run).empty());
	const std::string outside = dir.file("outside.txt");
	TEST_EXPECT(editor_test::write_text(outside, "mine") && editor_test::write_text(run + "/game.cfg", "kept"));
	TEST_EXPECT(record_run_staging(run, RunStaging{kRunModeInstall, {"../../outside.txt", outside, "sub/../../../outside.txt", ""}},
	                               run_error));
	TEST_EXPECT(take_run_directory(runs, gone, lenient, run, kept, run_error) &&
	            kept == std::vector<std::string>({"game.cfg"}) && fs::is_regular_file(outside));
	return 0;
}

// Each Play mode keeps its own run directories, `<runs>/<mode>/<n>` (run/run_directory.h): a Play of one
// mode never takes, empties or removes another's. An OpenNova runtime Play between two strict Plays leaves
// the game.cfg the first strict run wrote where the next strict Play keeps it (so no device dialog); the
// modes' runs live side by side; a fresh strict take empties strict's alone; a take removes its own mode's
// runs whose game is gone, never another mode's, nor a flat `<runs>/<n>` an older editor left (left
// alone). A mode's directory holding another mode's staging record (one edited by hand) is emptied; a
// mode none of kRunMode* takes nothing.
static int test_run_directories_per_mode() {
	namespace fs = std::filesystem;
	editor_test::TempProjectDir dir("opennova_editor_run_per_mode");
	const std::string runs = dir.file("runs");
	std::map<int64_t, ProcessLiveness> games;
	const LeaseLiveness liveness = [&games](int64_t pid, const std::string &) {
		const auto found = games.find(pid);
		return found == games.end() ? ProcessLiveness::Dead : found->second;
	};
	std::string run, run_error, text, io_error;
	std::vector<std::string> kept;
	const RunTake strict{kRunModeStrict, false}, strict_fresh{kRunModeStrict, true}, runtime{kRunModeRuntime, false},
	        lenient{kRunModeInstall, false};
	// What a Play of `mode` stages in `at` (one file), recorded as its staging.
	const auto stage = [&](const std::string &at, const char *mode) {
		return editor_test::write_text(at + "/staged.bin", mode) && record_run_staging(at, RunStaging{mode, {"staged.bin"}}, run_error);
	};
	TEST_EXPECT(editor_test::write_text(runs + "/1/game.cfg", "an older editor's"));

	// The first strict Play: its game's device dialog answered, the game writes its game.cfg.
	TEST_EXPECT(take_run_directory(runs, liveness, strict, run, kept, run_error) && run == runs + "/strict/1" && kept.empty());
	TEST_EXPECT(stage(run, kRunModeStrict) && editor_test::write_text(run + "/game.cfg", "names the adapter"));
	// An OpenNova runtime Play: a directory of its own, strict's as its game left it.
	TEST_EXPECT(take_run_directory(runs, liveness, runtime, run, kept, run_error) && run == runs + "/runtime/1" &&
	            kept.empty() && files_in(run).empty());
	TEST_EXPECT(stage(run, kRunModeRuntime) && editor_test::write_text(run + "/session.log", "a log") &&
	            editor_test::write_text(run + "/weapon.sav", "the runtime's weapons"));
	TEST_EXPECT(files_in(runs + "/strict/1") == std::vector<std::string>({"game.cfg", "staged.bin", "staging.json"}));
	// The next strict Play keeps the game.cfg the first wrote.
	TEST_EXPECT(take_run_directory(runs, liveness, strict, run, kept, run_error) && run == runs + "/strict/1" &&
	            kept == std::vector<std::string>({"game.cfg"}));
	TEST_EXPECT(read_file_text(run + "/game.cfg", text, io_error) && text == "names the adapter");
	TEST_EXPECT(stage(run, kRunModeStrict));
	// Lenient Play's beside both: three modes' runs side by side, each its own.
	TEST_EXPECT(take_run_directory(runs, liveness, lenient, run, kept, run_error) && run == runs + "/install/1" &&
	            kept.empty() && files_in(run).empty());
	TEST_EXPECT(stage(run, kRunModeInstall) && editor_test::write_text(run + "/game.cfg", "lenient's own"));
	TEST_EXPECT(files_in(runs) == std::vector<std::string>({"1", "install", "runtime", "strict"}));
	TEST_EXPECT(read_file_text(runs + "/strict/1/game.cfg", text, io_error) && text == "names the adapter");
	TEST_EXPECT(read_file_text(runs + "/runtime/1/weapon.sav", text, io_error) && text == "the runtime's weapons");

	// A runtime game may still run in runtime/1: the next runtime Play takes runtime/2. Every game gone, a
	// strict Play leaves both runtime runs alone; the next runtime Play takes runtime/1 again and removes
	// runtime/2, strict's and lenient's untouched.
	TEST_EXPECT(claim_run_directory(runs + "/runtime/1", 700, ProcessIdentity{"opennova.exe", "created 700"}, run_error));
	games[700] = ProcessLiveness::Alive;
	TEST_EXPECT(take_run_directory(runs, liveness, runtime, run, kept, run_error) && run == runs + "/runtime/2");
	TEST_EXPECT(stage(run, kRunModeRuntime));
	games.clear();
	TEST_EXPECT(take_run_directory(runs, liveness, strict, run, kept, run_error) && run == runs + "/strict/1" &&
	            kept == std::vector<std::string>({"game.cfg"}));
	TEST_EXPECT(stage(run, kRunModeStrict));
	TEST_EXPECT(files_in(runs + "/runtime") == std::vector<std::string>({"1", "2"}));
	const std::string strict_tree = editor_test::tree_digest(runs + "/strict"),
	                  install_tree = editor_test::tree_digest(runs + "/install");
	TEST_EXPECT(take_run_directory(runs, liveness, runtime, run, kept, run_error) && run == runs + "/runtime/1" &&
	            kept == std::vector<std::string>({"weapon.sav"}));
	TEST_EXPECT(stage(run, kRunModeRuntime));
	TEST_EXPECT(files_in(runs + "/runtime") == std::vector<std::string>({"1"}));
	TEST_EXPECT(editor_test::tree_digest(runs + "/strict") == strict_tree &&
	            editor_test::tree_digest(runs + "/install") == install_tree);

	// A fresh strict take empties strict's alone.
	const std::string runtime_tree = editor_test::tree_digest(runs + "/runtime");
	TEST_EXPECT(take_run_directory(runs, liveness, strict_fresh, run, kept, run_error) && run == runs + "/strict/1" &&
	            kept.empty() && files_in(run).empty());
	TEST_EXPECT(editor_test::tree_digest(runs + "/runtime") == runtime_tree &&
	            editor_test::tree_digest(runs + "/install") == install_tree);
	TEST_EXPECT(read_file_text(runs + "/install/1/game.cfg", text, io_error) && text == "lenient's own");

	// Another mode's record in strict's directory (edited by hand): emptied all the same.
	TEST_EXPECT(stage(run, kRunModeInstall) && editor_test::write_text(run + "/game.cfg", "names the adapter"));
	TEST_EXPECT(take_run_directory(runs, liveness, strict, run, kept, run_error) && run == runs + "/strict/1" &&
	            kept.empty() && files_in(run).empty());

	// A mode none of kRunMode* takes nothing, makes nothing.
	run_error.clear();
	TEST_EXPECT(!take_run_directory(runs, liveness, RunTake{"../elsewhere", false}, run, kept, run_error) &&
	            !run_error.empty() && !fs::exists(dir.file("elsewhere")));
	TEST_EXPECT(!take_run_directory(runs, liveness, RunTake{"", false}, run, kept, run_error));
	// The flat run an older editor left is left alone.
	TEST_EXPECT(read_file_text(runs + "/1/game.cfg", text, io_error) && text == "an older editor's");
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

// An archive of `files` (formats/pff's writer).
static bool write_archive(const std::string &path, const std::vector<std::pair<std::string, std::string>> &files) {
	std::filesystem::create_directories(std::filesystem::path(path).parent_path());
	std::vector<opennova::pff::PffWriteEntry> entries;
	for (const auto &file : files)
		entries.push_back({file.first.c_str(), reinterpret_cast<const uint8_t *>(file.second.data()),
		                   uint32_t(file.second.size()), 0, 1, 0});
	return opennova::pff::pff_write_archive(path.c_str(), opennova::pff::PFF_FORMAT_PFF3, entries.data(),
	                                        uint32_t(entries.size())) == opennova::pff::PFF_WRITE_OK;
}

// A file's last write set an hour back, so a copy made of it is trusted by its size and last write
// (io::file_stamp_settled); a file written just now is not.
static void settle(const std::string &path) {
	std::error_code ec;
	std::filesystem::last_write_time(opennova::io::os_path(path),
	                                 std::filesystem::file_time_type::clock::now() - std::chrono::hours(1), ec);
}

// The files a folder of the copy cache holds, sorted.
static std::vector<std::string> files_in(const std::string &dir) {
	std::vector<std::string> names;
	std::error_code ec;
	for (const std::filesystem::directory_entry &file : std::filesystem::directory_iterator(opennova::io::os_path(dir), ec))
		names.push_back(file.path().filename().string());
	std::sort(names.begin(), names.end());
	return names;
}

// ADR 0046 S16: an expansion's run directory. The install's boot archives and the loose files it
// ships beside them at its root (nothing else of the install: not its notes, not its own expansions),
// those the game only reads linked (archives, videos, music, NovaWorld screens), every other copied
// fresh (the country code, the NovaWorld cookie jar the game rewrites), and the files the game reads
// from its folder by name (score.ini, earlyerr.txt) copied; but an install's root copy of a file the
// expansion packs, which /d would read over the packed edit, left out. expansion/jxm a directory of
// the run's own, the build's archives and video linked into it and its version.txt copied; the run
// directory mounts with /exp jxm as an install does. The game writing through every file it may
// write leaves the install, the build and the copy cache as they were. The stock game staged the same
// way, its executable, Bink DLL, configuration and saves the install's, launched /w /d /exp jxm
// /FRISK. Where an install's file cannot be linked (another volume), its copy in the copy cache is,
// copied once and kept while the install's file keeps its size and last write, a last write too
// recent to trust (git's racy rule) copied again each run; the cache keeps the current install's
// files a run used and nothing else. Refused: no install, an install with none of the game's archives
// (play.install_missing), a build with no expansion folder (play.install_copy).
static int test_expansion_staging() {
	namespace fs = std::filesystem;
	editor_test::TempProjectDir dir("opennova_editor_expansion_staging");
	const std::string install = dir.file("install"), build = dir.file("build/0123456789abcdef"), cache = dir.file("copies");
	for (const char *name : {"language.pff", "localres.pff", "resource.pff"})
		TEST_EXPECT(write_empty_archive(install + "/" + name));
	TEST_EXPECT(editor_test::write_text(install + "/intro.bik", "video") && editor_test::write_text(install + "/menumus.sbf", "music") &&
	            editor_test::write_text(install + "/cc.bin", "us") && editor_test::write_text(install + "/nw_cdata.coo", "cookies") &&
	            editor_test::write_text(install + "/nwmain.mnx", "their screen") &&
	            editor_test::write_text(install + "/nwother.mnx", "another screen") &&
	            editor_test::write_text(install + "/SCORE.INI", "scores") &&
	            editor_test::write_text(install + "/earlyerr.txt", "early") &&
	            editor_test::write_text(install + "/Readme.txt", "notes") &&
	            editor_test::write_text(install + "/expansion/jox01/jox01.pff", "theirs") &&
	            editor_test::write_text(install + "/Jointops.exe", "exe") && editor_test::write_text(install + "/binkw32.dll", "bink") &&
	            editor_test::write_text(install + "/game.cfg", "install settings") &&
	            editor_test::write_text(install + "/player.sav", "install player"));
	TEST_EXPECT(write_archive(build + "/expansion/jxm/jxm.pff", {{"nwmain.mnx", "our screen"}}) &&
	            write_empty_archive(build + "/expansion/jxm/jxmL.pff"));
	TEST_EXPECT(editor_test::write_text(build + "/expansion/jxm/header.bik", "their video") &&
	            editor_test::write_text(build + "/expansion/jxm/version.txt", "1.0") &&
	            editor_test::write_text(build + "/expansion/jxm/jxm.bin", "the expansion's table") &&
	            editor_test::write_text(build + "/build.json", "{}"));
	const std::string build_tree = editor_test::tree_digest(build), install_tree = editor_test::tree_digest(install);
	const std::string run = dir.file("run/1");
	fs::create_directories(run);
	Diagnostic error;
	TEST_EXPECT(prepare_expansion_run(install, build, "jxm", run, cache, error));
	std::error_code ec;
	std::string text, io_error;
	for (const char *name : {"language.pff", "localres.pff", "resource.pff", "intro.bik", "menumus.sbf", "nwother.mnx"})
		TEST_EXPECT(fs::equivalent(run + "/" + name, install + "/" + name, ec)); // linked: the game only reads it
	const std::pair<const char *, const char *> copied[] = {
	        {"cc.bin", "us"}, {"nw_cdata.coo", "cookies"}, {"SCORE.INI", "scores"}, {"earlyerr.txt", "early"}};
	for (const auto &[name, held] : copied)
		TEST_EXPECT(fs::exists(run + "/" + name) && !fs::equivalent(run + "/" + name, install + "/" + name, ec) &&
		            read_file_text(run + "/" + name, text, io_error) && text == held); // copied: the game may write it
	TEST_EXPECT(!fs::exists(run + "/nwmain.mnx")); // the expansion packs its own: /d would read this one over it
	TEST_EXPECT(!fs::exists(run + "/Readme.txt") && !fs::exists(run + "/expansion/jox01") && !fs::exists(run + "/build.json"));
	TEST_EXPECT(fs::is_directory(run + "/expansion/jxm") && !fs::is_symlink(run + "/expansion/jxm"));
	for (const char *name : {"jxm.pff", "jxmL.pff", "header.bik"})
		TEST_EXPECT(fs::equivalent(run + "/expansion/jxm/" + name, build + "/expansion/jxm/" + name, ec));
	TEST_EXPECT(!fs::equivalent(run + "/expansion/jxm/version.txt", build + "/expansion/jxm/version.txt", ec));
	TEST_EXPECT(!fs::exists(cache)); // every file linked or copied: no copy kept
	{
		opennova::Vfs vfs;
		opennova::LaunchFlags flags;
		flags.expansion = "jxm";
		TEST_EXPECT(opennova::mount_install(vfs, run, flags) && vfs.mounted_expansion() == "jxm");
		// OpenNova's runtime takes the override table from the run directory as retail takes it from an
		// install, the loose expansion/jxm/jxm.bin the build placed (vfs_expansion_override_table, the rule
		// ResourceRoot's boot mount reads it by [orig: TextResource_LoadOverrideTable @ 0x4a49de]).
		std::vector<uint8_t> table;
		TEST_EXPECT(opennova::vfs_expansion_override_table(run, "jxm", opennova::ExpansionLoadPoint::ArchivesClosed, false,
		                                                   table) &&
		            std::string(table.begin(), table.end()) == "the expansion's table");
	}
	// The game writes through every staged file it may write (in place, as it opens them "w"), and its
	// expansion's weapon.sav beside the expansion's files: the build and the install keep their own.
	for (const auto &[name, held] : copied) TEST_EXPECT(editor_test::write_text(run + "/" + name, "written by the game"));
	TEST_EXPECT(editor_test::write_text(run + "/expansion/jxm/weapon.sav", "saved") &&
	            editor_test::write_text(run + "/expansion/jxm/version.txt", "edited"));
	TEST_EXPECT(editor_test::tree_digest(build) == build_tree && editor_test::tree_digest(install) == install_tree);
	// Staged again into the same run directory (run/run_directory.h: the run keeps the game's files): each
	// staged file put back by its name (the cookie jar the install's, the expansion's version.txt the
	// build's), the files the game reads by name kept as the game rewrote them (seeded where the run had
	// none, never staged), and its expansion's weapon.sav kept; what was staged named, each path under the
	// run directory.
	{
		std::vector<std::string> staged;
		TEST_EXPECT(prepare_expansion_run(install, build, "jxm", run, cache, error, link_file, &staged));
		const auto named = [&staged](const char *path) { return std::find(staged.begin(), staged.end(), path) != staged.end(); };
		TEST_EXPECT(named("language.pff") && named("intro.bik") && named("nw_cdata.coo") && named("cc.bin") &&
		            named("expansion/jxm/jxm.pff") && named("expansion/jxm/version.txt"));
		TEST_EXPECT(!named("SCORE.INI") && !named("earlyerr.txt") && !named("expansion/jxm/weapon.sav") && !named("nwmain.mnx"));
		TEST_EXPECT(read_file_text(run + "/nw_cdata.coo", text, io_error) && text == "cookies");
		TEST_EXPECT(read_file_text(run + "/expansion/jxm/version.txt", text, io_error) && text == "1.0");
		TEST_EXPECT(read_file_text(run + "/SCORE.INI", text, io_error) && text == "written by the game");
		TEST_EXPECT(read_file_text(run + "/earlyerr.txt", text, io_error) && text == "written by the game");
		TEST_EXPECT(read_file_text(run + "/expansion/jxm/weapon.sav", text, io_error) && text == "saved");
		TEST_EXPECT(fs::equivalent(run + "/expansion/jxm/jxm.pff", build + "/expansion/jxm/jxm.pff", ec));
		TEST_EXPECT(editor_test::tree_digest(build) == build_tree && editor_test::tree_digest(install) == install_tree);
	}

	// The stock game: the same staging, the install's own binaries, configuration and saves beside it.
	LaunchPlan plan;
	const std::string run2 = dir.file("run/2");
	fs::create_directories(run2);
	TEST_EXPECT(prepare_retail_launch_plan(install, build, run2, plan, error, "jxm", cache));
	TEST_EXPECT(plan.args == std::vector<std::string>({"/w", "/d", "/exp", "jxm", "/FRISK"}));
	TEST_EXPECT(plan.executable == run2 + "/Jointops.exe" && plan.resource_dir == run2 && plan.expansion == "jxm");
	TEST_EXPECT(read_file_text(run2 + "/game.cfg", text, io_error) && text == "install settings");
	TEST_EXPECT(read_file_text(run2 + "/player.sav", text, io_error) && text == "install player");
	TEST_EXPECT(read_file_text(run2 + "/nw_cdata.coo", text, io_error) && text == "cookies"); // a fresh copy each run
	TEST_EXPECT(fs::equivalent(run2 + "/expansion/jxm/jxm.pff", build + "/expansion/jxm/jxm.pff", ec));
	TEST_EXPECT(!fs::exists(run2 + "/nwmain.mnx"));
	for (const char *name : {"game.cfg", "player.sav", "nw_cdata.coo", "cc.bin", "SCORE.INI"})
		TEST_EXPECT(editor_test::write_text(run2 + "/" + name, "written by the game"));
	TEST_EXPECT(editor_test::tree_digest(install) == install_tree);

	// Another volume: the install's files the game only reads are copied once into the copy cache and
	// linked from there; the ones it may write are copied fresh, never cached.
	const FileLink across = [&install](const std::string &from, const std::string &to, std::string &reason) {
		if (from.rfind(install, 0) == 0) {
			reason = "another volume";
			return false;
		}
		return link_file(from, to, reason);
	};
	std::vector<std::string> install_files;
	for (const fs::directory_entry &file : fs::directory_iterator(install, ec))
		if (file.is_regular_file()) install_files.push_back(file.path().generic_string());
	for (const std::string &file : install_files) settle(file);
	const std::string settled_tree = editor_test::tree_digest(install);
	const std::string run3 = dir.file("run/3"), run4 = dir.file("run/4");
	fs::create_directories(run3);
	fs::create_directories(run4);
	TEST_EXPECT(prepare_expansion_run(install, build, "jxm", run3, cache, error, across));
	TEST_EXPECT(!fs::equivalent(run3 + "/intro.bik", install + "/intro.bik", ec));
	TEST_EXPECT(read_file_text(run3 + "/intro.bik", text, io_error) && text == "video");
	TEST_EXPECT(files_in(cache).size() == 1);
	const std::string key = cache + "/" + files_in(cache).front();
	TEST_EXPECT(files_in(key) == std::vector<std::string>({"install_copy.json", "intro.bik", "language.pff", "localres.pff",
	                                                         "menumus.sbf", "nwother.mnx", "resource.pff"}));
	const std::string copy = key + "/intro.bik";
	TEST_EXPECT(fs::equivalent(run3 + "/intro.bik", copy, ec)); // linked from the cache
	for (const auto &[name, held] : copied)
		TEST_EXPECT(!fs::equivalent(run3 + "/" + name, install + "/" + name, ec) &&
		            editor_test::write_text(run3 + "/" + name, "written by the game"));
	const std::string cache_tree = editor_test::tree_digest(key);
	TEST_EXPECT(editor_test::tree_digest(install) == settled_tree);
	// The next run takes the same copy (a settled last write is trusted); an install file that changed
	// is copied again, and a change too recent to trust is copied again by every run after it.
	TEST_EXPECT(prepare_expansion_run(install, build, "jxm", run4, cache, error, across));
	TEST_EXPECT(fs::equivalent(run4 + "/intro.bik", run3 + "/intro.bik", ec) &&
	            fs::equivalent(run4 + "/language.pff", run3 + "/language.pff", ec));
	TEST_EXPECT(editor_test::tree_digest(key) == cache_tree);
	TEST_EXPECT(editor_test::write_text(install + "/intro.bik", "a new video"));
	// Its last write a minute ahead: too recent to trust however long the runs below take (io_test pins the
	// settle window's edge itself).
	fs::last_write_time(opennova::io::os_path(install + "/intro.bik"), fs::file_time_type::clock::now() + std::chrono::minutes(1), ec);
	const std::string run5 = dir.file("run/5"), run6 = dir.file("run/6");
	fs::create_directories(run5);
	fs::create_directories(run6);
	TEST_EXPECT(prepare_expansion_run(install, build, "jxm", run5, cache, error, across));
	TEST_EXPECT(read_file_text(run5 + "/intro.bik", text, io_error) && text == "a new video");
	TEST_EXPECT(read_file_text(run4 + "/intro.bik", text, io_error) && text == "video"); // the earlier run keeps its own
	// The cache keeps the current install's files a run used: another install's folder and a file the
	// install no longer has go.
	TEST_EXPECT(editor_test::write_text(cache + "/0000000000000000/resource.pff", "another install's"));
	fs::remove(install + "/nwother.mnx", ec);
	TEST_EXPECT(prepare_expansion_run(install, build, "jxm", run6, cache, error, across));
	TEST_EXPECT(!fs::equivalent(run6 + "/intro.bik", run5 + "/intro.bik", ec) && // racy: copied again
	            read_file_text(run6 + "/intro.bik", text, io_error) && text == "a new video");
	TEST_EXPECT(fs::equivalent(run6 + "/language.pff", run3 + "/language.pff", ec));
	TEST_EXPECT(files_in(cache).size() == 1 && !fs::exists(cache + "/0000000000000000") &&
	            !fs::exists(key + "/nwother.mnx") && !fs::exists(run6 + "/nwother.mnx"));
	TEST_EXPECT(read_file_text(key + "/install_copy.json", text, io_error) && text.find("nwother") == std::string::npos &&
	            text.find("language.pff") != std::string::npos);

	// Refused: no install, one with none of the game's archives, a build without the folder.
	TEST_EXPECT(!prepare_expansion_run("", build, "jxm", dir.file("run/7"), cache, error) &&
	            error.code() == "play.install_missing");
	TEST_EXPECT(editor_test::write_text(dir.file("empty/readme.txt"), "x"));
	TEST_EXPECT(!prepare_expansion_run(dir.file("empty"), build, "jxm", dir.file("run/7"), cache, error) &&
	            error.code() == "play.install_missing");
	TEST_EXPECT(!prepare_expansion_run(install, build, "jxn", dir.file("run/7"), cache, error) &&
	            error.code() == "play.install_copy");
	return 0;
}

// DI-26: Play from here's start in a mission. The single player deploys at the first start marker type
// its mode's chain holds (world::start_marker_types, team 1): every marker of that type moves to the point,
// facing the heading, a team-2 marker made team 1 (no queued mount), every other marker as it was; none of
// either type, one of the primary is added; a point past the 16.16 positions is refused.
static int test_place_player_start() {
	using namespace opennova;
	const auto markers_at = [](bms::File &file, int item_id, int count) {
		mission::EntityTransform at;
		at.x = 10.0f;
		at.y = 20.0f;
		at.z = 3.0f;
		at.yaw = 45;
		for (int i = 0; i < count; ++i) mission::add_entity(file, mission::EntityKind::Marker, item_id, at);
	};
	PlayStart start;
	start.set = true;
	start.at[0] = 412.5;
	start.at[1] = -88.25;
	start.at[2] = 36.0;
	start.yaw = -90.0;
	const auto at_start = [&start](const bms::Entity &marker) {
		return marker.x == bms::to_fixed_16_16(start.at[0]) && marker.y == bms::to_fixed_16_16(start.at[1]) &&
		       marker.z == bms::to_fixed_16_16(start.at[2]) && marker.yaw == 270 && marker.pitch == 0 && marker.roll == 0;
	};
	PlayStartPlaced placed;
	std::string error;

	// Co-op (no mode bit, as single player): the insertion points 6094, both; the fallback and a waypoint stay.
	bms::File coop;
	mission::make_default(coop);
	markers_at(coop, 106094, 2);
	markers_at(coop, 106001, 1);
	markers_at(coop, 106000, 1);
	coop.markers[1].team = 2;
	TEST_EXPECT(place_player_start(coop, start, placed, error));
	TEST_EXPECT(placed.type == 6094 && placed.moved == 2 && !placed.added && coop.markers.size() == 4);
	TEST_EXPECT(at_start(coop.markers[0]) && at_start(coop.markers[1]) && coop.markers[1].team == 1);
	TEST_EXPECT(!at_start(coop.markers[2]) && coop.markers[2].x == bms::to_fixed_16_16(10.0) && !at_start(coop.markers[3]));
	TEST_EXPECT(play_start_words(start) == "(412.5, -88.2, 36.0) facing 270");

	// Only the fallback: the 6001s.
	bms::File fallback;
	mission::make_default(fallback);
	markers_at(fallback, 106001, 2);
	TEST_EXPECT(place_player_start(fallback, start, placed, error));
	TEST_EXPECT(placed.type == 6001 && placed.moved == 2 && !placed.added && at_start(fallback.markers[1]));

	// None: an insertion point added at the point, its SSN the next.
	bms::File none;
	mission::make_default(none);
	const int ssn = mission::next_entity_ssn(none);
	TEST_EXPECT(place_player_start(none, start, placed, error));
	TEST_EXPECT(placed.type == 6094 && placed.moved == 1 && placed.added && none.markers.size() == 1 &&
	            none.markers[0].type_id == 6094 && none.markers[0].id == ssn && at_start(none.markers[0]));

	// Deathmatch: its chain's 6095, the Co-op starts left.
	bms::File deathmatch;
	mission::make_default(deathmatch);
	deathmatch.header.attrib_flags = bms::AttribFlags::Deathmatch;
	markers_at(deathmatch, 106094, 1);
	markers_at(deathmatch, 106095, 1);
	TEST_EXPECT(place_player_start(deathmatch, start, placed, error));
	TEST_EXPECT(placed.type == 6095 && placed.moved == 1 && !at_start(deathmatch.markers[0]) && at_start(deathmatch.markers[1]));

	// Past what a position holds.
	PlayStart far = start;
	far.at[0] = 40000.0;
	TEST_EXPECT(!place_player_start(coop, far, placed, error) && error.find("32,768") != std::string::npos);
	return 0;
}

// DI-26: the start staged in a run directory. The archive that serves the mission (the boot table's slot
// order, an expansion's pair first) is written again there with the mission's start placed, every other
// entry as it held it (an encrypted one's stored bytes and flags too); its name, a link to the build's
// archive, is replaced, so the build's archive keeps its bytes. A run directory whose archives hold no such
// mission is refused (play.start). The runtime mounts such a run directory (on_run_dir), its build staged
// there by prepare_runtime_run, the build's record left out.
static int test_stage_play_start() {
	using namespace opennova;
	namespace fs = std::filesystem;
	editor_test::TempProjectDir dir("opennova_editor_play_start");
	const std::string build = dir.file("build/b1");
	const std::string run = dir.file("run/runtime/1");
	std::vector<uint8_t> mission_bytes;
	{
		bms::File mission;
		mission::make_default(mission);
		mission::EntityTransform at;
		mission::add_entity(mission, mission::EntityKind::Marker, 106094, at);
		std::string error;
		TEST_EXPECT(bms::write(mission, mission_bytes, error));
	}
	const std::string text = "Hello, briefing";
	std::vector<uint8_t> secret = {1, 2, 3, 4, 5, 6, 7, 8};
	pff::pff_container_xor(secret.data(), secret.size(), 0x0312A4CEu);
	const pff::PffWriteEntry entries[] = {
		{"First.bms", mission_bytes.data(), uint32_t(mission_bytes.size()), 0, pff::PFF_NEW_ENTRY_TIMESTAMP, 0},
		{"First.bin", reinterpret_cast<const uint8_t *>(text.data()), uint32_t(text.size()), 0, 1111u, 7u},
		{"secret.dat", secret.data(), uint32_t(secret.size()), pff::PFF_FLAG_ENCRYPTED, 2222u, 9u},
	};
	fs::create_directories(build);
	TEST_EXPECT(pff::pff_write_archive((build + "/localres.pff").c_str(), pff::PFF_FORMAT_PFF3, entries, 3) == pff::PFF_WRITE_OK);
	TEST_EXPECT(write_empty_archive(build + "/resource.pff") && editor_test::write_text(build + "/game.cfg", "cfg") &&
	            editor_test::write_text(build + "/build.json", "{}"));
	std::vector<uint8_t> built_before;
	std::string io_error;
	TEST_EXPECT(read_file_bytes(build + "/localres.pff", built_before, io_error));

	// The runtime's run directory (Play takes it, made): the build staged (linked; the build's record left out),
	// mounted there.
	fs::create_directories(run);
	Diagnostic error;
	std::vector<std::string> staged;
	TEST_EXPECT(prepare_runtime_run(build, run, error, &staged));
	std::sort(staged.begin(), staged.end());
	TEST_EXPECT(staged == std::vector<std::string>({"game.cfg", "localres.pff", "resource.pff"}));
	TEST_EXPECT(fs::exists(run + "/localres.pff") && !fs::exists(run + "/build.json"));
	const LaunchPlan mounted = make_play_launch_plan("opennova", build, run, "jo", 0, "First.bms", {}, "", true);
	TEST_EXPECT(mounted.resource_dir == mounted.working_dir &&
	            std::find(mounted.args.begin(), mounted.args.end(), mounted.working_dir) != mounted.args.end());
	TEST_EXPECT(make_play_launch_plan("opennova", build, run, "jo", 0).resource_dir == utf8_of(path_of(build)));

	PlayStart start;
	start.set = true;
	start.at[0] = 100.0;
	start.at[1] = 200.0;
	start.at[2] = 12.5;
	start.yaw = 180.0;
	PlayStartPlaced placed;
	TEST_EXPECT(stage_play_start(run, "", "first.BMS", start, placed, error));
	TEST_EXPECT(placed.archive == "localres.pff" && placed.type == 6094 && placed.moved == 1 && !placed.added);
	pff::PffArchive archive{};
	TEST_EXPECT(pff::pff_open(&archive, (run + "/localres.pff").c_str()) == 0);
	if (archive.entry_count == 3) {
		const pff::PffEntry *bms_entry = pff::pff_find(&archive, "First.bms");
		const pff::PffEntry *bin_entry = pff::pff_find(&archive, "First.bin");
		const pff::PffEntry *secret_entry = pff::pff_find(&archive, "secret.dat");
		TEST_EXPECT(bms_entry && bin_entry && secret_entry);
		if (bms_entry && bin_entry && secret_entry) {
			std::vector<uint8_t> bytes(bms_entry->size);
			bms::File staged_mission;
			std::string parse_error;
			TEST_EXPECT(pff::pff_extract(&archive, bms_entry, bytes.data(), bytes.size()) == 0 &&
			            bms::parse(bytes.data(), bytes.size(), staged_mission, parse_error));
			TEST_EXPECT(staged_mission.markers.size() == 1 && staged_mission.markers[0].x == bms::to_fixed_16_16(100.0) &&
			            staged_mission.markers[0].z == bms::to_fixed_16_16(12.5) && staged_mission.markers[0].yaw == 180);
			std::vector<uint8_t> held(bin_entry->size);
			TEST_EXPECT(pff::pff_extract(&archive, bin_entry, held.data(), held.size()) == 0 &&
			            std::string(held.begin(), held.end()) == text && bin_entry->timestamp == 1111u &&
			            bin_entry->checksum == 7u);
			std::vector<uint8_t> raw(secret_entry->size);
			TEST_EXPECT(pff::pff_extract_raw(&archive, secret_entry, raw.data(), raw.size()) == 0 && raw == secret &&
			            secret_entry->flags == pff::PFF_FLAG_ENCRYPTED);
		}
	} else {
		TEST_EXPECT(archive.entry_count == 3);
	}
	pff::pff_close(&archive);
	std::vector<uint8_t> built_after;
	TEST_EXPECT(read_file_bytes(build + "/localres.pff", built_after, io_error) && built_after == built_before);
	TEST_EXPECT(!fs::exists(run + "/localres.pff.start"));

	// An expansion's archive serves before the boot table's.
	const std::string expansion_run = dir.file("run/runtime/2");
	fs::create_directories(expansion_run + "/expansion/jxm");
	TEST_EXPECT(pff::pff_write_archive((expansion_run + "/expansion/jxm/jxmL.pff").c_str(), pff::PFF_FORMAT_PFF3, entries, 1) ==
	                    pff::PFF_WRITE_OK &&
	            pff::pff_write_archive((expansion_run + "/localres.pff").c_str(), pff::PFF_FORMAT_PFF3, entries, 1) ==
	                    pff::PFF_WRITE_OK);
	TEST_EXPECT(stage_play_start(expansion_run, "jxm", "First.bms", start, placed, error) &&
	            placed.archive == "expansion/jxm/jxmL.pff");

	// No archive there holds it.
	TEST_EXPECT(!stage_play_start(run, "", "Second.bms", start, placed, error) && error.code() == "play.start");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_place_player_start();
	failures += test_stage_play_start();
	failures += test_behind_starts();
	failures += test_launch_plans();
	failures += test_expansion_launch_plans();
	failures += test_install_staging();
	failures += test_strict_install_staging();
	failures += test_file_access_log();
	failures += test_strict_first_run_decision();
	failures += test_run_directory_keeps_game_state();
	failures += test_run_directories_per_mode();
	failures += test_expansion_staging();
	failures += test_lifecycle();
	if (failures == 0) std::printf("editor_play_session: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
