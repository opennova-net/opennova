#pragma once

#include <string>
#include <vector>

#include <editor/import/importer.h>

namespace opennova::editor {

// One-shot conversions on import (ADR 0046 S10): a scene text the Blender add-on
// writes (ADR 0047) becomes the native files the project edits, and the source is not
// kept, unlike an importer's source (importer.h), which stays in the project and is
// imported again when it changes. `.o3d` makes the model: the textures its materials name
// are the model's references, which an import with the files it needs plans and copies
// like any other file's (import_plan.h); `.o3a` makes the animation map and every clip it
// names (one clip with no table row makes that clip alone). A finding of the scene reader
// is a Diagnostic carrying its line; an error makes no output. A Black Hawk Down GP `.3di`
// imported from the disk is migrated to the 3DI3 the game loads (formats/threedi_gp; ADR
// 0027 as amended): the one row that claims a source by its bytes, since a 3DI3 shares
// its extension.
struct Converter {
	const char *id = "";
	std::vector<std::string> extensions; // lower-case, with the dot
	// When set, the row takes only a source whose bytes it claims (the others of its
	// extensions are copied as they are).
	bool (*claims)(const std::vector<uint8_t> &bytes) = nullptr;
	// `source_path`: the file on the disk ("" for an archive's or the install's), whose
	// folder a converter may look in for the files beside it.
	bool (*run)(const std::string &source_name, const std::string &source_path, const std::vector<uint8_t> &bytes,
	            ImportProduct &out) = nullptr;
};

const std::vector<Converter> &converters();
// The converter for a source file name, by extension; null when the file is native. A row
// that claims by bytes takes the source only when `bytes` are given and it claims them: the
// callers give an author's file's (one chosen from the disk, not a file an import brings as the
// game's own), in the import and its plan alike, so a GP model is migrated when imported as a
// file, never from the game's archives or as another file's dependency.
const Converter *converter_for(const std::string &source_name, const std::vector<uint8_t> *bytes = nullptr);

} // namespace opennova::editor
