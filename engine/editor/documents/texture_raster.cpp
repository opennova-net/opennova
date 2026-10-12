#include <editor/documents/texture_raster.h>

#include <cstring>
#include <utility>

#include <editor/model/finding_code_row.h>
#include <formats/png/png_decode.h>
#include <formats/png/png_encode.h>

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

// An uncompressed true-colour TGA in a form its writer makes again: its texels the top row first, as the game's
// reader takes the rows bottom up whatever the descriptor says (tga_read.h).
bool read_tga(const std::vector<uint8_t> &bytes, RasterState &out) {
	tga::TgaForm form;
	tga::TgaHeader header;
	if (!tga::tga_read_form(bytes.data(), bytes.size(), form) || !tga::tga_read_header(bytes.data(), bytes.size(), header))
		return false;
	const int width = header.width, height = header.height, stride = form.bits / 8;
	const RasterFormat format = form.bits == 32 ? RasterFormat::Rgba8 : RasterFormat::Rgb8;
	std::vector<uint8_t> texels(size_t(width) * size_t(height) * size_t(stride));
	const uint8_t *rows = bytes.data() + tga::TGA_HEADER_SIZE;
	for (int y = 0; y < height; ++y) {
		const uint8_t *row = rows + size_t(height - 1 - y) * size_t(width) * size_t(stride);
		uint8_t *to = texels.data() + size_t(y) * size_t(width) * size_t(stride);
		for (int x = 0; x < width; ++x) {
			// B, G, R (, A) in the file; R, G, B (, A) in the plane.
			to[x * stride + 0] = row[x * stride + 2];
			to[x * stride + 1] = row[x * stride + 1];
			to[x * stride + 2] = row[x * stride + 0];
			if (stride == 4) to[x * stride + 3] = row[x * stride + 3];
		}
	}
	auto side = std::make_shared<TextureSide>();
	side->form = TextureSide::Form::Tga;
	side->tga = form;
	out.planes.push_back(RasterPlane::from_bytes(format, width, height, texels.data()));
	out.side = std::move(side);
	return true;
}

// A true-colour PNG of 8-bit samples (an import's source image; IHDR's bit depth at byte 24, its colour type, 2 or
// 6, at byte 25): its texels as RGBA. A grey (a heightmap's 16-bit samples) or palette PNG is no plane of colours.
bool read_png(const std::vector<uint8_t> &bytes, RasterState &out) {
	if (bytes.size() < 26 || !png::is_png(bytes) || bytes[24] != 8 || (bytes[25] != 2 && bytes[25] != 6)) return false;
	RgbaImage image;
	IndexedImage8 indexed;
	std::string error;
	if (!png::decode_png(bytes, image, error, &indexed) || image.empty() || !indexed.indices.empty()) return false;
	auto side = std::make_shared<TextureSide>();
	side->form = TextureSide::Form::Png;
	out.planes.push_back(RasterPlane::from_bytes(RasterFormat::Rgba8, image.width, image.height, image.pixels.data()));
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
		if (side && side->form == TextureSide::Form::Png) return "Writes it as an 8-bit RGBA PNG";
		return std::string();
	}

private:
	TextureReader reader_;
};

} // namespace

size_t TextureSide::footprint() const { return sizeof(*this) + (bytes ? bytes->size() : 0); }

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
	case TextureReader::Png: return "a palette or grey PNG";
	case TextureReader::Dds: return "a DDS";
	case TextureReader::None: break;
	}
	return "a form no brush paints";
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
		out = png::encode_png_rgba(texels.data(), uint32_t(width), uint32_t(height));
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
