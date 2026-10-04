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

} // namespace opennova::editor
