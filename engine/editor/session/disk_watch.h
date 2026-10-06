#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <editor/assets/disk_changes.h>

namespace opennova::editor {

class SessionCore;
struct PollBudget;

// How often the Shell sends its check (ADR 0046 DI-01), with or without the focus (an editor an MCP
// client launched never has it); while a file waits to hold still, or the sweep runs, the next comes
// kDiskHoldStillMs after the last instead.
inline constexpr int64_t kDiskCheckMs = 1000;

// What another program saves of the open project's files, brought back without a Rescan (ADR 0046
// DI-01; the session's part RefreshChangedSources is served by). A check looks at the files the editor
// shows (each open document's file, every file a viewport's picture read), the folders the scan's walk
// went into (a file made, deleted or renamed in one moves its last write, so a new file and a gone one
// are found too), and the files a look found moved before (assets/disk_changes.h: each read once it held
// still, never while a program may still be writing it); with `all`, every file of the project besides,
// swept over the polls (the editor gaining the focus). The files S18's round trip watches (an import
// source, a file an import read, a PNG the game reads as it is: import/import_run.h's external_changes)
// are its, under its own rule, and join what a check reads. What is ready is read again alone, as an
// operation (SessionCore::start_changed_refresh: the sources it touches imported again, the scan updated
// for those files, the open documents of them read again, Output naming each file that came back); an
// open document with unsaved edits whose file changed keeps them, its conflict raised at once.
class DiskWatch {
public:
	explicit DiskWatch(SessionCore &core);
	DiskWatch(const DiskWatch &) = delete;
	DiskWatch &operator=(const DiskWatch &) = delete;

	// RefreshChangedSources: a check as above, `all` beginning the sweep over every file. Nothing when no
	// project is open or nothing is ready (no operation started).
	void check(bool all);
	// The poll's: the sweep stepped within the budget's bytes while no operation runs; what it finds
	// waits for the next check, which comes soon (the view's project.outside_waiting).
	void step(const PollBudget &budget);

private:
	// The files a check looks at each time: the open documents' and those the viewports' pictures read,
	// less those the import round trip watches.
	std::vector<std::string> watched_files() const;
	// Whether S18's round trip watches the project file at `relative` (external_changes' files): an import
	// source, a PNG the game reads as it is, a file an import read, a file an import made.
	bool imports_watch(const std::string &relative) const;
	// The open project followed: another project (or none) forgets every look.
	bool follow_project();
	// The view's count of what waits (project.outside_waiting), moved when it changed.
	void publish(size_t unsettled);

	SessionCore &core_;
	DiskChanges changes_;
	std::string root_;      // the project the looks are of
	size_t unsettled_ = 0;  // the import round trip's files that moved too recently, at the last check
};

} // namespace opennova::editor
