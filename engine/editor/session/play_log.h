#pragma once

#include <string>
#include <vector>

#include <base/gameprofile/graphics_log.h>
#include <base/gameprofile/resource_missing.h>
#include <editor/model/diagnostic.h>
#include <editor/run/launch_plan.h>
#include <formats/filelog/file_access_log.h>

namespace opennova::editor {

struct SessionView;

// What a Play's log says the game looked for and did not find, as Problems rows (ADR 0046 DI-27), each
// on the file of the project that names the missing name and the record and field that name it, so a
// row's Go to lands there and its fixes are a missing reference's or a required file's
// (session/problem_fixes.h). The rows are the Play's: PlayController keeps those of each mode (OpenNova,
// the game install, strict Play in the game install) until the next Play of that mode, or until the
// project closes.

// Which game a row's words name: "OpenNova", "the game install", "the game install (strict Play)".
struct PlayGame {
	std::string name;
};

// The rows a line of OpenNova's log saying a miss makes (gameprofile::kResourceMissingMarker, read by
// gameprofile::parse_resource_missing): of a name the asset graph resolves (its kind a reference kind's
// token), one row on each edge of that kind whose name the miss is (for a file kind, one of the names its
// loader opens; for a symbol, the name as the kind compares it), only those of the file that named it
// where the line says which (play.reference_missing, the edge's subject, its place); with no such edge, a
// file the game opens by its own name (the line's kind "file", or a name the boot manifest has a row of)
// is a row about that file (play.file_missing, the manifest row's role: its fixes Create and Import), and
// any other reference a row about the name alone (play.reference_missing: Import, Create, or Open the file
// where a symbol belongs), each on the file that named it where the line says which and the project has
// it. A Warning, an Info for a file the game goes on without (an optional manifest row, a name it has no
// row of). None with no project open.
std::vector<Diagnostic> resource_miss_findings(const gameprofile::ResourceMiss &miss, const PlayGame &game,
                                               const SessionView &view);

// The game install's own logs, read once its game exited (never while it runs:
// formats/filelog/file_access_log.h's kInstallFileLogName): its file log (null when it left none), the
// graphics log this run wrote (gameprofile::kGraphicsLogName, ghw.txt, "" when the run wrote none; its
// missions read by gameprofile::graphics_log_missions), and whether the game exited on its own.
struct InstallLogs {
	const filelog::FileAccessLog *file_log = nullptr;
	std::string graphics_log;
	bool exited_on_its_own = false;
};

// The rows the game install's logs make, the file log naming only what the game opened (each open that
// succeeded, never a file it did not find):
// - the boot's text tables, which Game_InitSubsystems opens in this order right after the archives
//   [orig: gameerr.bin @ 0x4a6fc8, gametext.bin @ 0x4a6fed, vmacros.bin @ 0x4a702f, keyhelp.bin @
//   0x4a7072]: with the archives opened, the first the log lacks of a game that exited on its own was not
//   found, its row a required file's (play.file_missing); gameerr.bin's lack shows earlyerr.txt's line 4
//   and the boot goes on, so the next is looked at; any other's is the game's refusal to go on, the last;
// - no file log at all from a game that exited on its own: it opened none of its archives and showed
//   earlyerr.txt's line 3 [orig: the check @ 0x4a6f44; Game_ShowEarlyError @ 0x4a68a0], a row on the
//   project's earlyerr.txt at that line where it has one;
// - every reference the asset graph finds missing in a file the game read (its file log names the file):
//   a row on that reference (play.reference_missing), the game having read what names it;
// - a mission the graphics log says the game began loading and never finished (play.mission.unfinished,
//   on the mission's file).
std::vector<Diagnostic> install_log_findings(const InstallLogs &logs, const PlayGame &game, const SessionView &view);

} // namespace opennova::editor
