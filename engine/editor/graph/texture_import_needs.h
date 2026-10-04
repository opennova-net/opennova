#pragma once

#include <string>
#include <vector>

#include <editor/graph/texture_uses.h>
#include <editor/import/import_context.h>

namespace opennova::editor {

// What a texture's uses ask of the file an import makes for them (ADR 0046 S18): the image importer's
// options each use's role and the name it writes call for (a model row naming body.tga reads body.dds
// first, the form of the game's own model textures: dds; a terrain's colour map: a 24-bit TGA of 1024 x
// 1024; a loading screen: an 800 x 600 PCX; a sky cloud named .pcx: a PCX; any other .tga: a TGA), each
// with why, in the use's words. Where two uses ask two things of one option the strictest that serves
// both wins (a TGA serves a model row as well as the HUD, which reads no .dds), else it is a conflict,
// said and left out (a loading screen and a particle graphic of one name: no one file serves both). A
// foliage or char map's indices are data the game reads: from an 8-bit PCX source a PCX keeping them
// (palette indices), from any other a conflict (an import from colours cannot keep them). A model's
// normal row naming a .tga asks the height in the alpha (normal height). No use asks nothing.
// `source_name` is the import's source (its stem the output's name unless a use names another; its
// extension whether it holds indices).
struct TextureImportNeeds {
	ImportOptions options;
	std::vector<std::string> reasons;   // one line an option asked: "format dds: the model diffuse of ..."
	std::vector<std::string> conflicts; // what no one file serves
	size_t uses = 0;
};
TextureImportNeeds texture_import_needs(const std::vector<TextureUse> &uses, const std::string &source_name);

} // namespace opennova::editor
