// The `.o3d` model scene text (docs/threedi/o3d-scene-format.md) read into
// its parse, ThreediO3dModel (threedi_o3d_model.h). The text carries geometry
// in MISSION axes (x forward, y left, z up) and none of a native file's
// limits: a mesh of any size, influences of any number, names of any length.
// The lowering (threedi_o3d_lower.h) turns the parse into a model a target
// holds, and threedi_o3d_build there reads, lowers and mints in one call
// (opennova-3di `build` and the editor's `.o3d` import).
//
// The reader checks the text alone, strictly: a field that does not parse, a
// value outside its word (the codes, flag words and quantized words the text
// spells as the file stores them: a style byte, an int16 track, a 32-bit flag
// word), a token past the record's fields, a record outside its container, an
// index outside what it names (a triangle's vertex, a part's parent, a
// declared register) or weights that do not blend to one is an error naming
// its line. It looks nothing up and knows no target. Authoring text, not a
// port.
#pragma once

#include <istream>
#include <vector>

#include <formats/threedi/scene_text.h>
#include <formats/threedi/threedi_o3d_model.h>

namespace opennova::threedi {

// The header the reader takes: `o3d 2`. A text of an earlier version fails on
// its first line (version 1 carried strips, bone tables and four bone slots).
inline constexpr int kThreediO3dVersion = 2;

// Read `.o3d` text into `model`. Every error lands in `findings` (the reader
// notes nothing: what a target makes of the model is the lowering's); true
// when there is none, and `model` is then the whole text.
bool threedi_o3d_read(std::istream &text, ThreediO3dModel &model, std::vector<SceneFinding> &findings);

} // namespace opennova::threedi
