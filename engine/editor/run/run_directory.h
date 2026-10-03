#pragma once

#include <cstdint>
#include <string>

#include <editor/run/play_lease.h>
#include <editor/run/process_platform.h>

namespace opennova::editor {

// Where Play runs the game (ADR 0046 S13 A8): a directory of its own under the project's cache,
// `.opennova/run/<n>/` (n = 1, 2, ...), the game's working directory and the log Play tails
// (session.log), so the build directory the game runs from stays as the build wrote it: a game
// writes beside itself (the game install's game.cfg and _filelog.txt, its saves), and an Export
// ships that directory. Play in the game install puts there what the install's game needs beside
// it (prepare_retail_launch_plan, run/launch_plan.h): the build's archives and loose files and the
// install's binaries. A run directory records the game it runs (its pid, image and creation time,
// as the platform reports them) from the spawn until the game stops. A Play takes the first
// numbered one that is not there, records no game, or records one the platform says is gone
// (Dead), emptied; it passes one whose game may still run (Alive, or Unknown: a game the platform
// cannot check is kept, as a lease's is); and it removes every other one whose game is gone. Only a
// numbered directory under the run root is ever removed.
inline constexpr const char *kRunLogFileName = "session.log";
inline constexpr const char *kRunRecordFileName = "run.json";
inline constexpr int kRunRecordSchemaVersion = 1;

// The run directory a Play takes under `runs_root`, made empty (`out`, '/'-separated); false with
// `error` when none can be made.
bool take_run_directory(const std::string &runs_root, const LeaseLiveness &liveness, std::string &out,
                        std::string &error);
// The game `pid` runs in `dir`: its record written (`identity`, the platform's report of it); false
// with `error` when it cannot be, the game then unknown to the next Play's choice.
bool claim_run_directory(const std::string &dir, int64_t pid, const ProcessIdentity &identity, std::string &error);
// The game that ran in `dir` is gone: its record removed (the directory and the game's log stay
// until a Play takes it again).
void release_run_directory(const std::string &dir);

} // namespace opennova::editor
