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

// One value an import option takes: its token as the record spells it, and what it makes, in words.
struct ImportOptionValue {
	std::string token;
	std::string words;
};

// One option an importer reads from its import record (ADR 0046 S18): its key, its label and what it
// does, the tokens it takes, the free forms it takes besides (`accepts` tests one: "threshold:<0..255>",
// a file name), the value meant when the record leaves it out ("" none: the importer's own), and the
// option whose values it applies under ("" always: a DDS's compression only under the dds format). The
// import_options query answers them; set_import_options takes a value a row takes, and a record holding
// one no row takes fails its import (import.option). A free value keeps its case where `keeps_case`
// says (a file name); every other value is a token, read in lower case.
struct ImportOptionRow {
	std::string key;
	std::string label;
	std::string words;
	std::vector<ImportOptionValue> values;
	std::vector<std::string> forms;
	bool (*accepts)(const std::string &value) = nullptr;
	bool keeps_case = false;
	std::string fallback;
	std::string applies_to;
	std::vector<std::string> applies_values;
};

// The row of `key` among `rows`; null for none.
const ImportOptionRow *import_option_row(const std::vector<ImportOptionRow> &rows, const std::string &key);
// Whether the row takes `value`: a token of its values, or a free form its accepts takes.
bool import_option_accepts(const ImportOptionRow &row, const std::string &value);
// What it takes, in words: "source, pow2_down, pow2_up, <W>x<H> or fit:<W>x<H>".
std::string import_option_takes(const ImportOptionRow &row);

struct Importer {
	const char *id = "";
	int version = 0; // bump it and every source imports again
	std::vector<std::string> extensions; // lower-case, with the dot
	// What a new record holds (import_assets writes it); an option it leaves out means its row's
	// fallback.
	ImportOptions default_options;
	// The options its record takes, a row each (none: it reads none).
	std::vector<ImportOptionRow> options;
	// The import of the context's source: its outputs, each named after the source's stem, or
	// the stem, an underscore and a suffix of its own where it makes several (renamed_import_output
	// relies on it), from the source and the files it reads through the context.
	bool (*run)(ImportContext &context, ImportProduct &out) = nullptr;
};

const std::vector<Importer> &importers();
// The importer for a source file name, by extension, in `table` (the compiled-in importers(), or a
// test's own); null when the file is native.
const Importer *importer_for(const std::string &source_name);
const Importer *importer_for(const std::string &source_name, const std::vector<Importer> &table);

// What an output of a source is called once the source is renamed (ADR 0046 d6, S9c):
// an importer names its outputs after the source's stem (logo.png makes logo.pcx), and an importer
// of several outputs each `<stem>_<suffix>` (S13 A8: a particle layout `smoke` making smoke_01.tga
// and smoke_02.tga, a terrain set its colour tiles), so the output takes the new stem, keeping its
// suffix and its extension; one named otherwise keeps its name.
std::string renamed_import_output(const std::string &output, const std::string &old_source,
                                  const std::string &new_source);

} // namespace opennova::editor
