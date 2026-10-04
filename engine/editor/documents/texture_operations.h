#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace opennova::editor {

// The whole-image edits of a texture document (ADR 0046 S18, the texture_operation request): each makes
// the file anew from the texels as the game reads them, written through the editor's own writers in the
// form the file is stored in, and is one undo step of the document. No paint program: no brush, no crop,
// no filter.
// - resize: `size` as the image importer takes it (pow2_down, pow2_up, <W>x<H>, fit:<W>x<H>): halving by
//   2 x 2 boxes as the game halves, any other size by the average of what each texel covers;
// - alpha: `alpha` (opaque, luminance, threshold:<n>, key:#RRGGBB as the importer takes them, or invert);
//   refused for a form that holds none (a 24-bit TGA, a PCX);
// - format: the stored form within the name's extension (a .tga's `tga` 32-bit or `tga24`; a .dds's `dds`
//   dxt5, dxt1 or argb and its `mips`; a .pcx's `palette` median_cut or exact); another extension is a
//   rename or the import's format, never this file;
// - reorder_rows: a TGA whose rows are stored top first (its origin bit, which every game reader ignores:
//   texture.tga_upside_down) written bottom first, the way up its header meant it [orig: the rows always
//   taken bottom up @ 0x56E995..0x56E9EA], true colour of its depth;
// - remap_palette: an 8-bit PCX's indices moved, each param `<from>` = `<to>` (0..255), the palette as it
//   is: a foliage or char map's codes, which are data the game reads.
enum class TextureOperationKind : uint8_t { Resize, Alpha, Format, ReorderRows, RemapPalette, kCount };

const char *texture_operation_token(TextureOperationKind kind);
// The kind a token names; false for none.
bool texture_operation_kind(const std::string &token, TextureOperationKind &out);

struct TextureOperation {
	TextureOperationKind kind = TextureOperationKind::Resize;
	std::vector<std::pair<std::string, std::string>> params;
};

// The file `name` holding `bytes` made anew by `operation`: its bytes in `out` and the step's words
// ("Resized to 256 x 128"); false with `why` in a modder's words (the game cannot read it, a form that
// holds no alpha, a param the operation does not take, rows already bottom first).
bool apply_texture_operation(const std::string &name, const std::vector<uint8_t> &bytes, const TextureOperation &operation,
                             std::vector<uint8_t> &out, std::string &words, std::string &why);

} // namespace opennova::editor
