#pragma once

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// What ships (ADR 0046 S16): Export copies the last good build, which the build's gate let through,
// into a folder of its own, the project's export folder (project.opennova's `export.output`, or the
// one the request names), as a player puts it in place: a standalone game's archives and loose
// files, which an OpenNova runtime boots with --resource-dir (with the runtime's own folder under
// `runtime/` when the project's `export.include_runtime` asks for it); an expansion's
// `expansion/<b>/` laid out as in an install, which a player copies into the game's folder and plays
// with `/exp <b>`. The build's record stays behind; the export's own, `export.json`, names the
// project, the build, the expansion and the files it wrote, and is what lets a later Export replace
// the folder.
//
// The copy is staged beside the folder (`<folder>.tmp`), and put in place only once whole, the last
// export set aside (`<folder>.old`) until the new one is in and put back when it cannot go in; the
// project's scan passes over both (project_scan). The folder is replaced only when it is missing,
// empty, or an export of this project (its export.json names the project's id): a folder holding
// anything else is the person's, refused (export.folder), nothing written. A staging or set-aside
// folder left by an export of this project cut short is removed first; one of anything else is refused
// alike. Over an earlier export of the project, what the person did there is said (export.replaced):
// the files they added, which the new export keeps (copied into it, unless the build now writes a file
// of the path); the export's files they changed since it was written, which the new export replaces;
// the earlier export's files the new one no longer holds, removed. A set-aside folder that cannot be
// removed once the new export is in (a file of it open elsewhere) is said (export.cleanup), and the next
// export removes it first. The build is only read: every file is copied (an export is the person's to
// change; a link would change the build with it).
//
// ExportRun steps the copy a budget of bytes at a time (S16: progress and cancel, as the build's own
// phases are stepped); cancelled before its folder is replaced, it removes its staging folder and leaves
// the folder as it was (export.cancelled).
inline constexpr const char *kExportRecordFileName = "export.json";
inline constexpr int kExportRecordSchemaVersion = 1;
inline constexpr const char *kExportRuntimeFolder = "runtime";

struct ExportRequest {
	std::string build_dir;   // the published build
	std::string build_id;
	std::string expansion;   // the build's (BuildReport::expansion); "" for the standalone game
	std::string export_dir;  // where it lands (a trailing separator is dropped)
	std::string project_id;  // the project's (project.opennova's project_id)
	std::string runtime_dir; // the runtime's folder to ship beside a standalone game ("" for none)
};

struct ExportReport {
	bool ok = false;
	std::string export_dir;
	std::vector<std::string> files; // the files written, '/'-separated and relative to the folder
	uint64_t bytes = 0;
	// Over an earlier export of the project (export.replaced): the files the person added there, kept;
	// the ones they changed since, replaced; the earlier export's files the new one no longer holds.
	std::vector<std::string> kept, replaced, removed;
	std::vector<Diagnostic> diagnostics;
};

// Removes the folder `dir` and everything in it; false with the reason when something of it is left
// (a file open elsewhere). ExportRun's set-aside folder goes through it (remove_tree, but in a test: one
// failing as an open file would).
using RemoveTree = std::function<bool(const std::string &dir, std::string &reason)>;
bool remove_tree(const std::string &dir, std::string &reason);

class ExportRun {
public:
	explicit ExportRun(ExportRequest request, RemoveTree remove_previous = remove_tree);
	~ExportRun();
	ExportRun(const ExportRun &) = delete;
	ExportRun &operator=(const ExportRun &) = delete;

	// One step: at most `budget` bytes copied (a file opened or the folder swapped counts as some). True
	// once done, whatever came of it (report().ok).
	bool step(uint64_t budget);
	// Stops it: before the folder is replaced, the staging folder is removed and the folder left as it
	// was (export.cancelled); once replaced, nothing to undo.
	void cancel();
	bool done() const { return phase_ == Phase::Done; }
	uint64_t bytes_done() const { return bytes_done_; }
	uint64_t bytes_total() const { return bytes_total_; }
	const std::string &label() const { return label_; }
	const ExportReport &report() const { return report_; }

private:
	enum class Phase { Check, Copy, Swap, Done };
	struct File {
		std::string from;   // a system path
		std::string to;     // relative to the folder, '/'-separated
		uint64_t size = 0;
		bool listed = true; // written by the export (a file the person added is carried, not listed)
	};
	struct Streams;

	void check();
	void copy(uint64_t budget);
	void swap();
	void fail(CoreFinding code, const std::string &message);
	void close_streams();
	void remove_staging();

	ExportRequest request_;
	RemoveTree remove_previous_;
	ExportReport report_;
	Phase phase_ = Phase::Check;
	std::string staging_, previous_;
	bool staged_ = false; // the staging folder is this export's own (the check let it through)
	std::vector<File> files_;
	size_t next_ = 0;
	uint64_t offset_ = 0; // in files_[next_]
	std::unique_ptr<Streams> streams_;
	uint64_t bytes_done_ = 0;
	uint64_t bytes_total_ = 0;
	std::string label_;
};

// An ExportRun run to its end.
ExportReport export_build(const ExportRequest &request, const RemoveTree &remove_previous = remove_tree);

} // namespace opennova::editor
