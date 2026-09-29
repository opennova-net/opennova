#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>

namespace opennova::editor {

// Imports the Godot way (ADR 0046 d6/d10, S8): a source file the game cannot read (a
// PNG) stays in the project tree; an importer turns it into the native outputs the
// build packs, under `.opennova/imported/`, and a committed `<file>.import` sidecar
// beside the source records the importer, its version, the options and the source
// fingerprint, so the outputs are rebuilt exactly when something changed. The
// registry is a fixed table: one row per importer, matched by the source's extension.

using ImportOptions = std::map<std::string, std::string>; // option -> value, as the sidecar spells them

struct ImportOutput {
	std::string name; // the logical name the game will see
	std::vector<uint8_t> bytes;
};

struct ImportProduct {
	std::vector<ImportOutput> outputs;
	std::vector<Diagnostic> diagnostics; // an error means no outputs
};

struct Importer {
	const char *id = "";
	int version = 0; // bump it and every source imports again
	std::vector<std::string> extensions; // lower-case, with the dot
	ImportOptions default_options;
	// `source_name` is the source's file name: every output takes its stem
	// (renamed_import_output relies on it).
	bool (*run)(const std::string &source_name, const std::vector<uint8_t> &bytes, const ImportOptions &options,
	            ImportProduct &out) = nullptr;
};

const std::vector<Importer> &importers();
// The importer for a source file name, by extension; null when the file is native.
const Importer *importer_for(const std::string &source_name);

// What an output of a source is called once the source is renamed (ADR 0046 d6, S9c):
// an importer names its outputs after the source's stem (logo.png makes logo.pcx), so
// the output takes the new stem and keeps its extension; one whose stem is not the
// source's keeps its name.
std::string renamed_import_output(const std::string &output, const std::string &old_source,
                                  const std::string &new_source);

} // namespace opennova::editor
