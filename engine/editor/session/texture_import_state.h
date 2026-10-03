#pragma once

#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/graph/texture_import_needs.h>
#include <editor/import/importer.h>
#include <editor/import/sidecar.h>

namespace opennova::editor {

struct SessionView;

// An import as the texture's tab and the import_options query show it (ADR 0046 S18): the source (a
// file of the project with its import record) and the record, the importer and its option rows, the
// files it makes, and what the texture uses of those files, and of every name of the source's stem the
// project writes, ask of it (texture_import_needs).
struct TextureImportState {
	std::string source; // project-relative
	std::string record; // its .import, project-relative
	const Importer *importer = nullptr;
	ImportSidecar sidecar;
	std::vector<std::string> outputs; // project-relative, as the scan lists them
	TextureImportNeeds needs;
};

// The import `path` is (a source holding its record) or comes from (an output: the scan's
// imported_from), by its project-relative path or its logical name; false, with `error`, for neither, a
// record that does not read or an importer the editor does not have.
bool texture_import_state(const SessionView &view, const std::string &path, TextureImportState &out,
                          std::string &error);

// Its wire form: {source, record, importer, version, options (the record's), effective (every row's
// value, the record's or its fallback), rows [{key, label, words, values [{token, words}], forms,
// fallback, applies {option, values}, applies_now}], outputs, needs {options, reasons, conflicts,
// uses}}.
io::JsonValue texture_import_state_json(const TextureImportState &state);

// The value an option has: the record's, else its row's fallback.
std::string import_option_value(const TextureImportState &state, const ImportOptionRow &row);
// Whether the option applies with the options the record holds (a DDS's compression only to the dds
// format).
bool import_option_applies_now(const TextureImportState &state, const ImportOptionRow &row);

} // namespace opennova::editor
