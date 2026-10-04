#pragma once

#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/import/sidecar.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

// One importable source in the project and where its outputs are.
struct ImportedSource {
	std::string source;   // project-relative
	std::string sidecar;  // project-relative
	std::string importer;
	std::string output_dir; // project-relative, under .opennova/imported/
	std::vector<std::string> inputs;  // project-relative: the other files its import read (S13 A8)
	std::vector<std::string> outputs; // project-relative
	bool reimported = false;
	bool ok = true; // false when the last import failed (the findings say why)
};

struct ImportRunResult {
	std::vector<ImportedSource> sources;
	std::vector<Diagnostic> diagnostics; // a finding's asset is the source, or its sidecar
	size_t reimported = 0;
};

// The import pass (ADR 0046 d6/d10; S8, S9c, S9p2a), run to its end (import/import_pass.h's
// ImportPass steps the same pass by bytes; S13 A3). An import source is a file an
// importer converts that has its record (the committed sidecar); importing a file
// writes the record (import_assets), and the pass never makes one: a file with no
// record is not an import source (a PNG without one is a texture the game loads as it
// is). A source is imported again when its record names another importer or version,
// the source's content no longer hashes to the record's, an input the record lists (S13 A8:
// a file the importer read through its ImportContext) is gone or no longer hashes as it did when
// this machine made the outputs, an output is missing, or the record changed since this machine
// made the outputs (an option edited by hand, a sidecar pulled with a new source); `force` imports
// again the sources `only` names (every one when it is empty) even when nothing changed. The
// machine-local import cache (`paths.import_cache_file`) keeps each source's and each input's size
// and last-write time with the content hash they vouch for, so an untouched file is not read again
// (a file whose last write lies too near the pass that read it is read again next time:
// io::file_stamp_settled), and what each source's outputs were made from, its record's
// fingerprint and each input's content hash; a source the cache does not know is imported again
// (a fresh clone has no outputs either). A sidecar is written
// only when one of its fields changes, so a checkout that only touches file times
// rewrites nothing; a sidecar that is there but does not read is a finding, left as it
// is, and its source is not imported until it reads again. Outputs land under
// `<cache>/imported/<hash of the source's path>/`, a stable place the scan lists as
// project files. Whatever the pass writes under the cache comes with its self-ignore
// file (ensure_project_cache_dir); a project with no import sources gets no cache. The sources
// are taken in the order of their paths.
ImportRunResult run_imports(const ProjectPaths &paths, const ProjectDocument &project, bool force = false,
                            const std::string &only = std::string());

// Whether `only` names the source at `source_relative_path`: its project-relative path
// or its bare file name, case-insensitive; "" names every source.
bool import_source_named(const std::string &only, const std::string &source_relative_path);

// Where a source's outputs live, project-relative: a directory named by a hash of the
// source's project-relative path (lower-cased), so it is stable across imports.
std::string import_output_dir(const ProjectPaths &paths, const std::string &source_relative_path);

// The import sources of `scan` whose file's size or last write moved since the scan read it (a program
// saved it: ADR 0046 S18's external round trip), project-relative, in the scan's order; a source gone is
// none (the next scan says so). What refresh_changed_sources asks before it refreshes: a stat a source.
std::vector<std::string> changed_import_sources(const ProjectPaths &paths, const AssetScan &scan);

} // namespace opennova::editor
