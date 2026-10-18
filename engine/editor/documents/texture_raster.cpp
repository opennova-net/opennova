#include <editor/documents/texture_raster.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include <editor/model/core_finding.h>
#include <formats/png/png_decode.h>
#include <formats/png/png_encode.h>
#include <formats/tga/tga_read.h>

namespace opennova::editor {

namespace {

RasterState whole_state(const std::vector<uint8_t> &bytes) {
	auto side = std::make_shared<TextureSide>();
	side->form = TextureSide::Form::Whole;
	side->bytes = std::make_shared<const std::vector<uint8_t>>(bytes);
	RasterState out;
	out.side = std::move(side);
	return out;
}

// An 8-bit one-plane PCX whose rows are its width: its indices, its palette and its form.
bool read_pcx8(const std::vector<uint8_t> &bytes, RasterState &out) {
	PcxHeader header;
	if (!pcx_read_header(bytes.data(), bytes.size(), header) || header.bits != 8 || header.planes != 1 ||
	    pcx_row_fit(header) != PcxRowFit::Exact)
		return false;
	IndexedImage8 image;
	auto side = std::make_shared<TextureSide>();
	std::string error;
	if (!decode_pcx_indexed(bytes.data(), bytes.size(), image, error, &side->pcx) || image.empty()) return false;
	side->form = TextureSide::Form::Pcx8;
	std::memcpy(side->palette.data(), image.palette, side->palette.size());
	out.planes.push_back(RasterPlane::from_bytes(RasterFormat::U8, image.width, image.height, image.indices.data()));
	out.side = std::move(side);
	return true;
}

// An uncompressed true-colour TGA in a form its writer makes again, its texels as the game's reader decodes them
// (tga::tga_decode_retail: the top row first, the rows taken bottom up whatever the descriptor says), a 24-bit
// one's in its own three channels.
bool read_tga(const std::vector<uint8_t> &bytes, RasterState &out) {
	tga::TgaForm form;
	tga::TgaImage image;
	std::string error;
	if (!tga::tga_read_form(bytes.data(), bytes.size(), form) ||
	    !tga::tga_decode_retail(bytes.data(), bytes.size(), image, error))
		return false;
	if (form.bits == 32) {
		out.planes.push_back(RasterPlane::from_bytes(RasterFormat::Rgba8, image.width, image.height, image.rgba.data()));
	} else {
		// R, G, B alone: the reader's opaque alpha is no channel of the file.
		const size_t count = size_t(image.width) * size_t(image.height);
		std::vector<uint8_t> rgb(count * 3);
		for (size_t i = 0; i < count; ++i) std::copy_n(&image.rgba[i * 4], 3, &rgb[i * 3]);
		out.planes.push_back(RasterPlane::from_bytes(RasterFormat::Rgb8, image.width, image.height, rgb.data()));
	}
	auto side = std::make_shared<TextureSide>();
	side->form = TextureSide::Form::Tga;
	side->tga = form;
	out.side = std::move(side);
	return true;
}

// A PNG of 8-bit samples (an import's source image; its IHDR's bit depth and colour type, png::png_header_form): a
// true-colour one its texels in its own channels (2 RGB, 6 RGBA: written again in that form, never an RGB source
// widened to RGBA); a palette one (3) its indices and its palette; a grey one (0) its levels. A PNG of another depth (a
// heightmap's 16-bit grey, lane A's) is no plane a texture's brush paints.
bool read_png(const std::vector<uint8_t> &bytes, RasterState &out) {
	uint8_t depth = 0, colour = 0;
	if (!png::png_header_form(bytes, depth, colour) || depth != 8) return false;
	auto side = std::make_shared<TextureSide>();
	side->form = TextureSide::Form::Png;
	side->png_colour = colour;
	std::string error;
	if (colour == 2 || colour == 6) {
		RgbaImage image;
		IndexedImage8 indexed;
		if (!png::decode_png(bytes, image, error, &indexed) || image.empty() || !indexed.indices.empty()) return false;
		if (colour == 6) {
			out.planes.push_back(RasterPlane::from_bytes(RasterFormat::Rgba8, image.width, image.height, image.pixels.data()));
		} else {
			// R, G, B alone: the decoder's opaque alpha is no channel of the file.
			const size_t count = size_t(image.width) * size_t(image.height);
			std::vector<uint8_t> rgb(count * 3);
			for (size_t i = 0; i < count; ++i) std::copy_n(&image.pixels[i * 4], 3, &rgb[i * 3]);
			out.planes.push_back(RasterPlane::from_bytes(RasterFormat::Rgb8, image.width, image.height, rgb.data()));
		}
	} else if (colour == 3) {
		RgbaImage image;
		IndexedImage8 indexed;
		png::PngPalette held;
		if (!png::decode_png(bytes, image, error, &indexed, &held) || indexed.empty() || held.entries == 0) return false;
		side->png_entries = held.entries;
		std::memcpy(side->palette.data(), indexed.palette, side->palette.size());
		// Its transparency, written back as it was (no reader of the maps' indices looks at it).
		side->png_alpha.assign(held.alpha.begin(),
		                       held.alpha.begin() + std::ptrdiff_t(std::min<size_t>(held.alpha.size(), held.entries)));
		out.planes.push_back(RasterPlane::from_bytes(RasterFormat::U8, indexed.width, indexed.height, indexed.indices.data()));
	} else if (colour == 0) {
		png::GrayImage grey;
		if (!png::decode_png_gray(bytes, grey, error) || grey.width <= 0 || grey.height <= 0 || grey.max_value != 255) return false;
		// Each level as its byte (max_value 255: an 8-bit grey PNG's samples).
		std::vector<uint8_t> levels(grey.samples.size());
		for (size_t i = 0; i < levels.size(); ++i) levels[i] = static_cast<uint8_t>(grey.samples[i]);
		out.planes.push_back(RasterPlane::from_bytes(RasterFormat::U8, grey.width, grey.height, levels.data()));
	} else {
		return false;
	}
	out.side = std::move(side);
	return true;
}

class TextureCodec final : public RasterCodec {
public:
	explicit TextureCodec(TextureReader reader) : reader_(reader) {}
	bool read(const std::vector<uint8_t> &decoded, RasterState &out, std::vector<SourceIssue> &issues,
	          Diagnostic &error) const override {
		(void)issues;
		(void)error;
		// Every file reads: what the game cannot load is said by its image, never a document that does not open.
		out = RasterState();
		const bool planes = (reader_ == TextureReader::Pcx && read_pcx8(decoded, out)) ||
		                    (reader_ == TextureReader::Tga && read_tga(decoded, out)) ||
		                    (reader_ == TextureReader::Png && read_png(decoded, out));
		if (!planes) out = whole_state(decoded);
		return true;
	}
	SerializeResult write(const RasterState &state) const override {
		SerializeResult out;
		std::vector<uint8_t> bytes;
		std::string error;
		if (!write_texture_state(state, bytes, error)) {
			out.issues.push_back({});
			out.issues.back().message = error;
			return out;
		}
		out.text.assign(bytes.begin(), bytes.end());
		return out;
	}
	std::string save_words(const RasterState &state) const override {
		const TextureSide *side = texture_side(state);
		if (side && side->form == TextureSide::Form::Pcx8 && side->pcx.coding == PcxCoding::Other)
			return "Writes its rows in the coding the shipped maps have (the file's own coding is no tool's)";
		if (side && side->form == TextureSide::Form::Png)
			return side->png_colour == 3 ? "Writes it as an 8-bit palette PNG"
			       : side->png_colour == 0 ? "Writes it as an 8-bit grey PNG"
			       : side->png_colour == 2 ? "Writes it as an 8-bit RGB PNG"
			                               : "Writes it as an 8-bit RGBA PNG";
		return std::string();
	}

private:
	TextureReader reader_;
};

} // namespace

size_t TextureSide::footprint() const { return sizeof(*this) + (bytes ? bytes->size() : 0) + png_alpha.size(); }

const TextureSide *texture_side(const RasterState &state) { return dynamic_cast<const TextureSide *>(state.side.get()); }

bool texture_paintable(const RasterState &state) {
	const TextureSide *side = texture_side(state);
	return side && side->form != TextureSide::Form::Whole && state.planes.size() == 1;
}

std::string texture_unpaintable_words(const std::string &path, const RasterState &state) {
	if (texture_paintable(state)) return std::string();
	switch (texture_reader_for(path)) {
	case TextureReader::Pcx: return "a PCX other than 8-bit indices in one plane with rows of its width";
	case TextureReader::Tga: return "a TGA other than uncompressed true colour (24 or 32 bits, no colour map)";
	case TextureReader::Png: return "a PNG of other than 8-bit samples";
	case TextureReader::Dds: return "a DDS";
	case TextureReader::None: break;
	}
	return "a form no brush paints";
}

bool texture_rgba_rect(const RasterState &state, int x, int y, int w, int h, std::vector<uint8_t> &out) {
	out.clear();
	if (!texture_paintable(state)) return false;
	const RasterPlane &plane = state.planes.front();
	if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > plane.width() || y + h > plane.height()) return false;
	const TextureSide &side = *texture_side(state);
	// An index plane's colours: a PCX's palette and a palette PNG's; a grey PNG's levels.
	const bool grey = side.form == TextureSide::Form::Png && side.png_colour == 0;
	std::vector<uint8_t> texels(size_t(w) * size_t(h) * size_t(plane.texel_bytes()));
	plane.copy_rect(x, y, w, h, texels.data());
	out.resize(size_t(w) * size_t(h) * 4);
	for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
		uint8_t *rgba = &out[i * 4];
		switch (plane.format()) {
		case RasterFormat::Rgb8:
			rgba[0] = texels[i * 3];
			rgba[1] = texels[i * 3 + 1];
			rgba[2] = texels[i * 3 + 2];
			rgba[3] = 255;
			break;
		case RasterFormat::Rgba8: std::copy_n(&texels[i * 4], 4, rgba); break;
		case RasterFormat::U8: {
			const uint8_t index = texels[i];
			for (int c = 0; c < 3; ++c) rgba[c] = grey ? index : side.palette[size_t(index) * 3 + size_t(c)];
			rgba[3] = 255;
			break;
		}
		default:
			out.clear();
			return false;
		}
	}
	return true;
}

RasterState read_texture_state(const std::string &path, const std::vector<uint8_t> &bytes) {
	RasterState out;
	std::vector<SourceIssue> issues;
	Diagnostic error;
	TextureCodec(texture_reader_for(path)).read(bytes, out, issues, error);
	return out;
}

bool write_texture_state(const RasterState &state, std::vector<uint8_t> &out, std::string &error) {
	out.clear();
	const TextureSide *side = texture_side(state);
	if (!side) {
		error = "The texture has no form to write: it was never read.";
		return false;
	}
	if (side->form == TextureSide::Form::Whole) {
		if (side->bytes) out = *side->bytes;
		return true;
	}
	if (state.planes.size() != 1) {
		error = "The texture's plane is missing.";
		return false;
	}
	const RasterPlane &plane = state.planes.front();
	const std::vector<uint8_t> texels = plane.bytes();
	const int width = plane.width(), height = plane.height();
	switch (side->form) {
	case TextureSide::Form::Pcx8: {
		IndexedImage8 image;
		image.width = width;
		image.height = height;
		image.indices = texels;
		std::memcpy(image.palette, side->palette.data(), side->palette.size());
		return encode_pcx_indexed(image, out, error, &side->pcx);
	}
	case TextureSide::Form::Tga: {
		std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4, 255);
		const int stride = plane.texel_bytes();
		for (size_t i = 0; i < size_t(width) * size_t(height); ++i)
			for (int c = 0; c < stride; ++c) rgba[i * 4 + size_t(c)] = texels[i * size_t(stride) + size_t(c)];
		return tga::tga_write(rgba.data(), uint32_t(width), uint32_t(height), side->tga, out, error);
	}
	case TextureSide::Form::Png:
		out = side->png_colour == 3 ? png::encode_png_indexed(texels.data(), uint32_t(width), uint32_t(height),
		                                                      side->palette.data(), side->png_entries,
		                                                      side->png_alpha.data(), uint32_t(side->png_alpha.size()))
		      : side->png_colour == 0 ? png::encode_png_gray(texels.data(), uint32_t(width), uint32_t(height))
		      : side->png_colour == 2 ? png::encode_png_rgb(texels.data(), uint32_t(width), uint32_t(height))
		                              : png::encode_png_rgba(texels.data(), uint32_t(width), uint32_t(height));
		if (out.empty()) error = "The PNG writer made nothing of it.";
		return !out.empty();
	case TextureSide::Form::Whole: break;
	}
	return true;
}

std::shared_ptr<const RasterCodec> pick_texture_codec(const std::string &path, const std::vector<uint8_t> &decoded) {
	(void)decoded;
	static const auto none = std::make_shared<const TextureCodec>(TextureReader::None);
	static const auto tga = std::make_shared<const TextureCodec>(TextureReader::Tga);
	static const auto dds = std::make_shared<const TextureCodec>(TextureReader::Dds);
	static const auto pcx = std::make_shared<const TextureCodec>(TextureReader::Pcx);
	static const auto png = std::make_shared<const TextureCodec>(TextureReader::Png);
	switch (texture_reader_for(path)) {
	case TextureReader::Tga: return tga;
	case TextureReader::Dds: return dds;
	case TextureReader::Pcx: return pcx;
	case TextureReader::Png: return png;
	case TextureReader::None: break;
	}
	return none;
}

bool TextureWholeEdit::apply(RasterCanvas &canvas, Diagnostic &error) const {
	(void)error;
	canvas.replace_whole(state);
	return true;
}

bool TexturePaintEdit::apply(RasterCanvas &canvas, Diagnostic &error) const {
	const auto refuse = [&](const std::string &why) {
		error = make_finding(CoreFinding::DocumentStale, DiagnosticSeverity::Error, why, std::string());
		return false;
	};
	if (canvas.plane_count() != 1) return refuse("The texture holds no plane a brush paints: it changed since the stroke began.");
	const RasterPlane &plane = canvas.plane(0);
	if (plane.format() != format || plane.width() != width || plane.height() != height)
		return refuse("The texture's form or size changed since the stroke began: paint it again.");
	const size_t bytes = size_t(plane.texel_bytes());
	if (values.size() != at.size() * bytes) return refuse("The paint's texels and values do not match.");
	RasterWriter &writer = canvas.write(0);
	for (size_t i = 0; i < at.size(); ++i) {
		const uint32_t index = at[i];
		if (index >= uint32_t(width) * uint32_t(height)) return refuse("A painted texel lies outside the texture.");
		const int x = int(index % uint32_t(width)), y = int(index / uint32_t(width));
		const uint8_t *value = values.data() + i * bytes;
		if (std::memcmp(plane.texel(x, y), value, bytes) == 0) continue;
		std::memcpy(writer.texel(x, y), value, bytes);
	}
	return true;
}

} // namespace opennova::editor
