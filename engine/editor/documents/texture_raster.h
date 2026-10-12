#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/texture_image.h>
#include <editor/model/raster_document.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>

namespace opennova::editor {

// A texture as the raster core holds it (ADR 0046 S25 B3): one plane of its texels where a brush can paint its
// form, beside the form its file is written in again (RasterSideState); the file whole, no plane, where it cannot.
// The paintable forms, each read and written by the writers its format's library has, so a file read and written
// again is its bytes:
//   Pcx8  an 8-bit, one-plane PCX whose rows are its width (every char and foliage map JO ships): a U8 plane of
//         its indices, its palette and its header and row coding (PcxIndexedForm: the shipped coder's or JO's
//         mission editor's, formats/pcx);
//   Tga   an uncompressed true-colour TGA with no ID or colour map (every colour and blend map JO ships): an Rgb8
//         or Rgba8 plane, the top row first as the game's reader takes it, and its depth, descriptor and TGA 2.0
//         footer (TgaForm, formats/tga);
//   Png   an 8-bit PNG (an import's source image): a true-colour one an Rgba8 plane, written again as an RGBA PNG;
//         a palette one (a terrain set's surface or foliage map, its indices the classes or codes) a U8 plane of its
//         indices and its palette, written again as a palette PNG; a grey one (a foliage map's levels) a U8 plane,
//         written again as a grey PNG (formats/png encode_png_indexed, encode_png_gray).
// Every other form (a DDS, an MDT, a run-length or colour-mapped TGA, a 24-bit or misfit PCX, a PNG of another depth,
// a file the game cannot load) is held whole: the texture operations still change it, no brush paints it.
struct TextureSide final : RasterSideState {
	enum class Form : uint8_t { Whole, Pcx8, Tga, Png };
	Form form = Form::Whole;
	std::shared_ptr<const std::vector<uint8_t>> bytes; // Whole: the file
	PcxIndexedForm pcx;                                 // Pcx8
	std::array<uint8_t, 768> palette{};                 // Pcx8: the 256 entries; Png palette: its entries; R, G, B
	tga::TgaForm tga;                                   // Tga
	uint8_t png_colour = 6;                             // Png: 6 true colour (2 read as it), 3 palette, 0 grey
	uint32_t png_entries = 0;                           // Png palette: its PLTE's entries
	std::vector<uint8_t> png_alpha;                     // Png palette: its tRNS's entries (none: opaque), kept
	size_t footprint() const override;
};

// The texture's side state in `state` (null for none).
const TextureSide *texture_side(const RasterState &state);
// Whether a brush paints the texture's form: one plane of its texels.
bool texture_paintable(const RasterState &state);
// What the form is in a modder's words where no brush paints it ("" where one does): why the brush is refused.
std::string texture_unpaintable_words(const std::string &path, const RasterState &state);

// A rectangle of a paintable texture's plane as 8-bit RGBA texels, row-major, the top row first (S25: what a device
// patches into the map it loaded): true colour as it stands (alpha 255 where the plane holds none), an index its
// palette's colour (a PCX's, a palette PNG's), a grey level that grey. False, `out` empty, where no brush paints the
// form or the rectangle leaves the plane.
bool texture_rgba_rect(const RasterState &state, int x, int y, int w, int h, std::vector<uint8_t> &out);

// The texture's file read into a state (`path` picks the reader the game's loaders pick by name) and written back.
// Every file reads (Whole where no plane holds it); a write of a Whole state is its bytes.
RasterState read_texture_state(const std::string &path, const std::vector<uint8_t> &bytes);
bool write_texture_state(const RasterState &state, std::vector<uint8_t> &out, std::string &error);

// The codec its document reads and writes through (RasterDocument's picker).
std::shared_ptr<const RasterCodec> pick_texture_codec(const std::string &path, const std::vector<uint8_t> &decoded);

// A whole new state (a texture operation's bytes, ADR 0046 S18, read by the texture's reader): a whole change.
struct TextureWholeEdit final : RasterEditPayload {
	RasterState state;
	const char *token() const override { return "texture.whole"; }
	bool apply(RasterCanvas &canvas, Diagnostic &error) const override;
};

// Texels painted on the texture's plane (a brush's dab, S25 B4): each texel's index into the plane (y x width + x)
// and its value (the plane's texel bytes: an index, R G B, R G B A). Refused where the plane is not the one the
// texels were made for (another form or size: the document changed under the stroke).
struct TexturePaintEdit final : RasterEditPayload {
	RasterFormat format = RasterFormat::U8;
	int width = 0, height = 0;
	std::vector<uint32_t> at;
	std::vector<uint8_t> values;
	const char *token() const override { return "texture.paint"; }
	bool apply(RasterCanvas &canvas, Diagnostic &error) const override;
};

} // namespace opennova::editor
