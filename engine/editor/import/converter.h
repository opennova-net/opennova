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
// is a Diagnostic carrying its line; an error makes no output.
struct Converter {
	const char *id = "";
	std::vector<std::string> extensions; // lower-case, with the dot
	bool (*run)(const std::string &source_name, const std::vector<uint8_t> &bytes, ImportProduct &out) = nullptr;
};

const std::vector<Converter> &converters();
// The converter for a source file name, by extension; null when the file is native.
const Converter *converter_for(const std::string &source_name);

} // namespace opennova::editor
