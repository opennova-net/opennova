#pragma once

// The project walk's one rule for the folders it passes over (ADR 0046 S13 A3, S16): a
// dot-directory (`.opennova/`, `.git/`), the export folder, and the staging and set-aside folders
// an export keeps beside it. The project scan, its folder listing and the content store's scan
// (content_store/content_scan.h, ADR 0069) walk a project by it, so a file one of them meets is a
// file of the project to each.

#include <filesystem>

namespace opennova::editor {

// Whether `dir` (a system path) is the export folder `export_dir`, or the staging or set-aside
// folder an export keeps beside it (kExportStagingSuffix, kExportPreviousSuffix): no file of the
// project's, whatever an export cut short or a removal refused left in it.
bool export_folder(const std::filesystem::path &dir, const std::filesystem::path &export_dir);

// Whether a project's walk passes over the folder `dir`: a dot-directory or an export folder.
bool walk_skips_folder(const std::filesystem::path &dir, const std::filesystem::path &export_dir);

} // namespace opennova::editor
