#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <editor/import/import_context.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

// Imports the Godot way (ADR 0046 d6/d10, S8): a source file the game cannot read (a
// PNG) stays in the project tree; an importer turns it into the native outputs the
// build packs, under `.opennova/imported/`, and a committed `<file>.import` sidecar
// beside the source records the importer, its version, the options, the source
// fingerprint and every other file the import read, so the outputs are rebuilt exactly when
// something changed. The registry is a fixed table: one row per importer, matched by the
// source's extension. An import is one source with as many inputs as its importer reads through
// its ImportContext (S13 A8): a sound bank's manifest with its waves, a font's metrics with its
// atlas, a terrain's set with its images, each a feature's importer row, none a shape of its own.

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
	// The import of the context's source: its outputs, each named after the source's stem
	// (renamed_import_output relies on it), from the source and the files it reads through the
	// context.
	bool (*run)(ImportContext &context, ImportProduct &out) = nullptr;
};

const std::vector<Importer> &importers();
// The importer for a source file name, by extension, in `table` (the compiled-in importers(), or a
// test's own); null when the file is native.
const Importer *importer_for(const std::string &source_name);
const Importer *importer_for(const std::string &source_name, const std::vector<Importer> &table);

// What an output of a source is called once the source is renamed (ADR 0046 d6, S9c):
// an importer names its outputs after the source's stem (logo.png makes logo.pcx), so
// the output takes the new stem and keeps its extension; one whose stem is not the
// source's keeps its name.
std::string renamed_import_output(const std::string &output, const std::string &old_source,
                                  const std::string &new_source);

} // namespace opennova::editor
