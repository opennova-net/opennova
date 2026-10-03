#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

// What ships (ADR 0046 S16): Export copies the last good build, which the build's gate let through,
// into a folder of its own, the project's export folder (project.opennova's `export.output`, or the
// one the request names), as a player puts it in place: a standalone game's archives and loose
// files, which an OpenNova runtime boots with --resource-dir (with the runtime's own folder under
// `runtime/` when the project's `export.include_runtime` asks for it); an expansion's
// `expansion/<b>/` laid out as in an install, which a player copies into the game's folder and plays
// with `/exp <b>`. The build's record stays behind; the export's own, `export.json`, names the
// project, the build and the expansion, and is what lets a later Export replace the folder.
//
// The copy is staged beside the folder (`<folder>.tmp`), and put in place only once whole, the last
// export set aside (`<folder>.old`) until the new one is in and put back when it cannot go in. The
// folder is replaced only when it is missing, empty, or an export of this project (its export.json
// names the project's id): a folder holding anything else is the person's, refused
// (export.folder), nothing written. A staging or set-aside folder left by an export of this project
// cut short is removed first; one of anything else is refused alike. The build is only read: every
// file is copied (an export is the person's to change; a link would change the build with it).
inline constexpr const char *kExportRecordFileName = "export.json";
inline constexpr int kExportRecordSchemaVersion = 1;
inline constexpr const char *kExportStagingSuffix = ".tmp";
inline constexpr const char *kExportPreviousSuffix = ".old"; // the last export, set aside while the new goes in
inline constexpr const char *kExportRuntimeFolder = "runtime";

struct ExportRequest {
	std::string build_dir;   // the published build
	std::string build_id;
	std::string expansion;   // the build's (BuildReport::expansion); "" for the standalone game
	std::string export_dir;  // where it lands
	std::string project_id;  // the project's (project.opennova's project_id)
	std::string runtime_dir; // the runtime's folder to ship beside a standalone game ("" for none)
};

struct ExportReport {
	bool ok = false;
	std::string export_dir;
	std::vector<std::string> files; // the files written, '/'-separated and relative to the folder
	uint64_t bytes = 0;
	std::vector<Diagnostic> diagnostics;
};

ExportReport export_build(const ExportRequest &request);

} // namespace opennova::editor
