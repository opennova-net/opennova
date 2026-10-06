#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/run/play_lease.h>
#include <editor/run/process_platform.h>

namespace opennova::editor {

// Where Play runs the game (ADR 0046 S13 A8): a directory of its own under the project's cache,
// `.opennova/run/<n>/` (n = 1, 2, ...), the game's working directory and the log Play tails
// (session.log), so the build directory the game runs from stays as the build wrote it: a game
// writes beside itself (the game install's game.cfg and _filelog.txt, its saves), and an Export
// ships that directory. Play in the game install puts there what the install's game needs beside
// it (prepare_retail_launch_plan, run/launch_plan.h): the build's archives and loose files and the
// install's binaries (Strict Play's, prepare_strict_install_launch_plan: those and nothing else). A
// run directory records the game it runs (its pid, image and creation time, as the platform reports
// them) from the spawn until the game stops. A Play takes the first numbered one that is not there,
// records no game, or records one the platform says is gone (Dead); it passes one whose game may
// still run (Alive, or Unknown: a game the platform cannot check is kept, as a lease's is); and it
// removes every other one whose game is gone. Only a numbered directory under the run root is ever
// removed.
//
// The directory it takes keeps what the game wrote there in the runs before (its game.cfg, which names
// the display adapter its device dialog chose, its saves, its scores), as a player's install keeps them:
// a game.cfg whose hw3d_name and hw3d_guid are the adapter's is what spares a run the device dialog
// [orig: Config_ParseSettingsLine @ 0x54ff2a hw3d_deviceno, @ 0x54ff55 hw3d_name, @ 0x54ff85 hw3d_guid;
// Game_InitSubsystems @ 0x4a711c the compare (Mission_HasMapOrNameChanged @ 0x53de40, a misnomer) ->
// @ 0x4a7125 Game_RunVideoTestDialog]. Only what a Play staged there (its staging record, RunStaging)
// goes before the next stages again, with the logs a run writes for Play to read (session.log,
// _filelog.txt) and the game's record. It is emptied instead when the Play asks for a fresh run, when
// the Play before was of another mode (Strict Play never inherits what lenient Play copied from the
// install; neither shares the OpenNova runtime's files), or when it holds no staging record.
inline constexpr const char *kRunLogFileName = "session.log";
inline constexpr const char *kRunRecordFileName = "run.json";
inline constexpr int kRunRecordSchemaVersion = 1;
// What the last Play staged in a run directory (RunStaging), kept there until the next Play takes it.
inline constexpr const char *kRunStagingFileName = "staging.json";
inline constexpr int kRunStagingSchemaVersion = 1;

// Which Play stages a run directory: the OpenNova runtime's, lenient Play in the game install's, or
// Strict Play's. A run directory's game state belongs to the mode that made it.
inline constexpr const char *kRunModeRuntime = "runtime";
inline constexpr const char *kRunModeInstall = "install";
inline constexpr const char *kRunModeStrict = "strict";

// A run directory's staging record: the mode of the Play that staged it and every file the staging put
// there ('/'-separated paths under it: the build's files, the install's program and Bink DLL, an
// expansion's base game and folder), which the next Play of that mode removes before it stages again.
// The files the game wrote, and the install's configuration and saves lenient Play seeds where the
// directory has none of its own, are not in it: they are the run's own.
struct RunStaging {
	std::string mode;
	std::vector<std::string> files;
};

// How a Play takes its run directory: the mode it stages (kRunMode*) and whether it starts fresh, the
// directory emptied whatever it kept (a first run: the game's device dialog, its default profile).
struct RunTake {
	std::string mode;
	bool fresh = false;
};

// The run directory a Play takes under `runs_root` (`out`, '/'-separated), readied for `take`: the
// files the Play before staged, its logs and its record removed, the rest kept and listed in `kept`
// ('/'-separated paths under it, sorted); emptied (`kept` empty) for a fresh take, another mode's
// directory or one with no staging record. One whose staged files will not go (a process holding them)
// is passed over. False with `error` when none can be made.
bool take_run_directory(const std::string &runs_root, const LeaseLiveness &liveness, const RunTake &take,
                        std::string &out, std::vector<std::string> &kept, std::string &error);
// What a Play staged in `dir` recorded there (RunStaging), so the next Play of its mode removes those
// files alone; false with `error` when it cannot be (the next Play then empties the directory).
bool record_run_staging(const std::string &dir, const RunStaging &staging, std::string &error);
// The game `pid` runs in `dir`: its record written (`identity`, the platform's report of it); false
// with `error` when it cannot be, the game then unknown to the next Play's choice.
bool claim_run_directory(const std::string &dir, int64_t pid, const ProcessIdentity &identity, std::string &error);
// The game that ran in `dir` is gone: its record removed (the directory and the game's log stay
// until a Play takes it again).
void release_run_directory(const std::string &dir);

} // namespace opennova::editor
