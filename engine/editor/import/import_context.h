#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

using ImportOptions = std::map<std::string, std::string>; // option -> value, as the sidecar spells them

// A file an import read besides its source (ADR 0046 S13 A8): its path, relative to the source's
// folder and '/'-separated, as the import record lists it, and the 64-bit FNV-1a of what was read.
struct ImportInput {
	std::string path;
	uint64_t hash = 0;

	bool operator==(const ImportInput &other) const { return path == other.path && hash == other.hash; }
	bool operator!=(const ImportInput &other) const { return !(*this == other); }
};

// What a read learned of an input besides its record's line: its project-relative path and the
// size and last write it had, taken before its bytes were read (so a change made while it was read
// reads as a change the next time), which the machine-local import cache keeps.
struct ImportInputStamp {
	std::string relative;
	uint64_t size = 0;
	int64_t modified = 0;
};

// What an importer reads (ADR 0046 S13 A8): its source (the file its import record sits beside),
// the options the record holds, and every other file it declares by reading it here, a sound
// bank's waves beside its manifest, a font's atlas beside its metrics, a terrain's height and
// colour images beside its set. Each file read is recorded (its path and the hash of its bytes):
// the record lists every input the outputs were made from by its path, the machine-local import
// cache by its hash, and the import pass imports again when any of them changes, and when none does
// reads none of them. A path is taken from the source's folder: a file of the folder, of a folder
// under it, or one a `..` reaches elsewhere in the project (`../shared/palette.pal`), never a place
// outside the project, under a dot-folder (the cache) or in the game install (a bank over the
// install's own waves cannot read them: a feature that needs it records an install input by its
// install-relative path, a decision of its own). Single-threaded, like the pass that makes it.
class ImportContext {
public:
	// `folder` and `root`: the source's folder and the project's, on disk.
	ImportContext(std::string source_name, const std::vector<uint8_t> &source, const ImportOptions &options,
	              std::string folder, std::string root);

	// The source's file name: an importer names its outputs after its stem (renamed_import_output
	// relies on it).
	const std::string &source_name() const { return source_name_; }
	const std::vector<uint8_t> &source() const { return source_; }
	const ImportOptions &options() const { return options_; }

	// The file at `path` (relative to the source's folder) read whole into `out` and recorded as an
	// input, once however often it is read. False, with an import.input finding in findings(), when
	// it is not a file of the project the import may read, or cannot be read.
	bool read(const std::string &path, std::vector<uint8_t> &out);
	// What it read, in the order first read, with each one's stamp at the same index; its findings;
	// the bytes read.
	const std::vector<ImportInput> &inputs() const { return inputs_; }
	const std::vector<ImportInputStamp> &stamps() const { return stamps_; }
	const std::vector<Diagnostic> &findings() const { return findings_; }
	uint64_t bytes_read() const { return bytes_read_; }

	// What a recorded input's path names: its file on disk and its project-relative path; false for
	// a path that leaves the project or passes through a dot-folder. The import pass checks a
	// record's inputs by it, as read() reads them.
	static bool resolve(const std::string &folder, const std::string &root, const std::string &path,
	                    std::string &file, std::string &relative);

private:
	std::string source_name_;
	const std::vector<uint8_t> &source_;
	const ImportOptions &options_;
	std::string folder_;
	std::string root_;
	std::vector<ImportInput> inputs_;
	std::vector<ImportInputStamp> stamps_;
	std::vector<Diagnostic> findings_;
	uint64_t bytes_read_ = 0;
};

} // namespace opennova::editor
