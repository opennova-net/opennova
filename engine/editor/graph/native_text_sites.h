#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <editor/assets/asset_kind.h>

namespace opennova::editor {

// The sites of the native text kinds (ADR 0046 S18): a terrain (.trn), an environment (.env), a
// particle file (.ptl), the HUD layout (hudpos.def) and a face animation (.grm) are read by the engine's own parsers
// (graph/extractors.cpp), which keep no places, and have no document type to write them. A name such
// a file writes is rewritten as text: a whole token spelling the site's value (bounded by the start
// or end of the text, a space, a line's end, or one of , ; = " ' / \ ( ) [ ] { }) is replaced, and
// the text is read again through the same parser; the token is the site when the one edge that
// changed is the site's (the same record, field and kind), now naming the new value, and every other
// edge is as it was. Every other byte of the file (its spacing, its comments, a line the game reads
// over, its line ends) stays.
bool native_text_kind(AssetKind kind);

// The names (before, after) are UTF-8, as the graph reads them; the text is the game's code page
// (Windows-1252), each name looked for and written in its bytes there.
struct NativeTextSite {
	std::string record;
	std::string field;
	std::string before;
	std::string after;
};

// Each site rewritten in `text` (the file `file` of `kind`, read as `game` reads it) in turn, each
// found in the text as the sites before it left it. `missed` takes the index of each site no token
// was found for (a name the file spells otherwise, or one that is no longer there). The number
// rewritten.
size_t rewrite_native_text(const std::string &file, AssetKind kind, const std::string &game, std::string &text,
                           const std::vector<NativeTextSite> &sites, std::vector<size_t> &missed);

// Where a text the engine's own parser reads writes a record's name (the deep-integration plan's DI-17: a
// Go to into such a text lands on its line, though the parser keeps no places): the name of the first edge
// the parser reads of `record` and `field` (any field with none), else of the first symbol `record`
// defines (of `field`, where given); the whole token spelling it (as rewrite_native_text bounds one, without
// case) whose change, the text read again, changes that one name and nothing else. Its line and column
// (1-based, in the text's bytes); false when the text names no such record or no token is it. Any kind the
// graph reads through a native extractor (the avatars, a terrain, a face animation, a particle file).
bool native_text_place(const std::string &file, AssetKind kind, const std::string &game, const std::string &text,
                       const std::string &record, const std::string &field, size_t &line, size_t &column);

} // namespace opennova::editor
