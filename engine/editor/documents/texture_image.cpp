#include <editor/documents/texture_image.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>

#include <base/io/bits.h>
#include <base/io/strutil.h>
#include <formats/bfc1/bfc1.h>
#include <formats/configfile/config_file.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_decode.h>
#include <formats/tga/tga.h>
#include <formats/tga/tga_read.h>
#include <runtime/renderer/d3dx_image_codecs.h>
#include <runtime/renderer/device_texture.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/texture_dxt.h>
#include <runtime/renderer/texture_load_rules.h>

namespace opennova::editor {

namespace {

std::string sides(uint32_t width, uint32_t height) { return std::to_string(width) + " x " + std::to_string(height); }

std::string bytes_words(size_t bytes) {
	if (bytes < 1024) return std::to_string(bytes) + (bytes == 1 ? " byte" : " bytes");
	char text[32];
	if (bytes < 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f KB", double(bytes) / 1024.0);
	else std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
	return text;
}

void fact(TextureImage &image, const char *key, const char *label, std::string words) {
	image.facts.push_back({key, label, std::move(words)});
}

// The alpha the first level's texels hold, and in words.
void classify_alpha(TextureImage &image) {
	if (image.levels.empty() || image.levels.front().rgba.empty()) return;
	std::array<bool, 256> seen{};
	const std::vector<uint8_t> &rgba = image.levels.front().rgba;
	for (size_t i = 3; i < rgba.size(); i += 4) seen[rgba[i]] = true;
	size_t values = 0;
	int low = 255, high = 0;
	for (int v = 0; v < 256; ++v)
		if (seen[size_t(v)]) {
			++values;
			low = std::min(low, v);
			high = std::max(high, v);
		}
	if (values == 1 && seen[255]) image.alpha = TextureAlpha::None;
	else if (values <= 2 && (values == 1 ? (seen[0] || seen[255]) : (seen[0] && seen[255]))) image.alpha = TextureAlpha::Mask;
	else image.alpha = TextureAlpha::Graded;
	switch (image.alpha) {
	case TextureAlpha::None: fact(image, "alpha", "Alpha", "none: every texel is opaque"); break;
	case TextureAlpha::Mask:
		fact(image, "alpha", "Alpha",
		     values == 1 ? "on or off: every texel is transparent" : "on or off: each texel 0 or 255 (a cut-out)");
		break;
	case TextureAlpha::Graded:
		fact(image, "alpha", "Alpha",
		     "graded: " + std::to_string(values) + " values from " + std::to_string(low) + " to " + std::to_string(high));
		break;
	}
}

// The game builds the mip chain of a texture made from texels with no creation flags
// (renderer::texture_level_count).
void built_chain(TextureImage &image) {
	const uint32_t levels = renderer::texture_level_count(image.width(), image.height(), 0);
	image.game_levels = levels;
	uint32_t w = image.width(), h = image.height();
	if (levels == 0) return;
	for (uint32_t i = 1; i < levels; ++i) {
		w = std::max(1u, w / 2);
		h = std::max(1u, h / 2);
	}
	fact(image, "mips", "Mip levels",
	     "none in the file; the game builds " + std::to_string(levels) + (levels == 1 ? " level" : " levels") +
	             (levels > 1 ? ", down to " + sides(w, h) : std::string()));
}

void size_fact(TextureImage &image) {
	const uint32_t w = image.width(), h = image.height();
	if (!w || !h) return;
	fact(image, "size", "Size",
	     sides(w, h) + (io::is_power_of_two(w) && io::is_power_of_two(h) ? " (sides are powers of two)" : " (a side is no power of two)"));
}

void read_tga(TextureImage &image, const std::vector<uint8_t> &stored) {
	fact(image, "format", "File format", "TGA image");
	fact(image, "reader", "Read by", "the game's TGA reader");
	std::vector<uint8_t> bytes = stored;
	// The models' archive reader unpacks a BFC1 file first (bfc1::bfc1_unpack, its witnesses).
	if (bfc1::bfc1_is_bfc1(stored.data(), stored.size())) {
		if (!bfc1::bfc1_unpack(bytes)) {
			image.refusal = "Its BFC1 packing does not unpack.";
			return;
		}
		image.bfc1 = true;
	}
	// The texels as the game's TGA reader decodes them (formats/tga/tga_read.h, its witnesses); the
	// header's fields say what form the file is and what the reader makes of it.
	tga::TgaHeader h;
	tga::TgaImage tga;
	std::string error;
	if (!tga::tga_read_header(bytes.data(), bytes.size(), h) || !tga::tga_decode_retail(bytes.data(), bytes.size(), tga, error)) {
		image.refusal = error.empty() ? std::string("The TGA header is cut short.") : error;
		return;
	}
	// The forms the reader zeroes, and those it leaves unwritten (which the port reads as zeros too)
	// (tga::tga_retail_form, its witnesses).
	const tga::TgaRetailForm form = tga::tga_retail_form(h);
	const bool unset = form == tga::TgaRetailForm::Unset;
	const bool zeroed = form == tga::TgaRetailForm::Zeroed;
	image.loads = true;
	image.decoded = true;
	image.blank = unset || zeroed;
	image.levels.push_back({uint32_t(tga.width), uint32_t(tga.height), std::move(tga.rgba)});
	size_fact(image);
	std::string texels;
	switch (h.image_type) {
	case 1:
	case 9:
		texels = "8-bit indices into a colour map of " + std::to_string(h.map_length) + " entries of " +
		         std::to_string(h.map_entry_bits) + " bits";
		break;
	case 2:
	case 10:
		texels = h.bits == 32 ? "32 bits: colour and 8 bits of alpha"
		         : h.bits == 24 ? "24 bits: colour, no alpha"
		                        : std::to_string(h.bits) + " bits of colour";
		break;
	case 3:
	case 11: texels = std::to_string(h.bits) + "-bit grey"; break;
	default: texels = "image type " + std::to_string(h.image_type) + ", which the TGA format does not define"; break;
	}
	fact(image, "texels", "Texels", texels);
	std::string compression = h.run_length() ? "run-length" : "none";
	if (image.bfc1) compression = "BFC1-packed (the models' reader unpacks it), then " + compression;
	fact(image, "compression", "Compression", compression);
	if (zeroed)
		fact(image, "loads", "In the game", "loads blank: its reader zeroes this form (every texel transparent black)");
	else if (unset)
		fact(image, "loads", "In the game",
		     "loads no texels of the file: its reader leaves this form's texels unwritten (shown blank)");
	else
		fact(image, "loads", "In the game", "loads it");
	image.upside_down = h.top_first();
	fact(image, "rows", "Rows",
	     h.top_first() ? "top row first: the game reads every TGA bottom row first, so it shows upside down"
	                   : "bottom row first, as the game reads it");
	classify_alpha(image);
	if (h.alpha_bits() && h.bits == 32)
		fact(image, "alpha_bits", "Alpha bits", std::to_string(h.alpha_bits()) + " (the header's; the game copies the byte)");
	built_chain(image);
}

void read_pcx(TextureImage &image, const std::vector<uint8_t> &bytes) {
	fact(image, "format", "File format", "PCX image");
	fact(image, "reader", "Read by", "the game's PCX reader");
	PcxIndexed pcx;
	RgbaImage rgba;
	std::string error;
	if (!decode_pcx_menu_rgba(bytes.data(), bytes.size(), rgba, error, &pcx)) {
		image.refusal = error == "PCX is not 8 bits per pixel"
		                        ? "It is not of 8 bits a plane, the one depth the game's PCX reader takes."
		                        : error;
		return;
	}
	image.loads = true;
	image.decoded = true;
	image.levels.push_back({uint32_t(rgba.width), uint32_t(rgba.height), std::move(rgba.pixels)});
	size_fact(image);
	if (pcx.indexed) {
		fact(image, "texels", "Texels", "8-bit indices into a 256-colour palette");
		image.indices = std::move(pcx.indices);
		image.palette.resize(256 * 3);
		for (size_t i = 0; i < 256; ++i)
			for (size_t c = 0; c < 3; ++c) image.palette[i * 3 + c] = pcx.palette[i][c];
		// The archive loader's alpha of it: its 8-bit read, each entry's luminance.
		RgbaImage lit;
		if (decode_pcx_luminance_alpha(bytes.data(), bytes.size(), lit, error) && lit.width == rgba.width &&
		    lit.height == rgba.height) {
			image.luminance.resize(lit.pixels.size() / 4);
			for (size_t i = 0; i < image.luminance.size(); ++i) image.luminance[i] = lit.pixels[i * 4 + 3];
		}
	} else {
		fact(image, "texels", "Texels", "24 bits: three planes of colour, no alpha");
	}
	fact(image, "compression", "Compression", "run-length");
	fact(image, "loads", "In the game", "loads it");
	if (pcx.indexed) fact(image, "palette", "Palette", "256 colours, every one opaque");
	classify_alpha(image);
	built_chain(image);
}

void read_png(TextureImage &image, const std::vector<uint8_t> &bytes, const char *format = "PNG image",
              const char *reader = "the menus' PNG reader") {
	fact(image, "format", "File format", format);
	fact(image, "reader", "Read by", reader);
	RgbaImage rgba;
	std::string error;
	if (!png::decode_png(bytes, rgba, error)) {
		image.refusal = error;
		return;
	}
	image.loads = true;
	image.decoded = true;
	image.levels.push_back({uint32_t(rgba.width), uint32_t(rgba.height), std::move(rgba.pixels)});
	size_fact(image);
	fact(image, "texels", "Texels", "8 bits a channel, as the menus' reader expands them");
	fact(image, "compression", "Compression", "deflate");
	fact(image, "loads", "In the game", "loads it");
	classify_alpha(image);
	built_chain(image);
}

// A file under a .dds name that D3DX decodes as another image, decoded here as D3DX's codec decodes
// it.
void read_decoded(TextureImage &image, const char *name, RgbaImage &rgba, const char *texels) {
	fact(image, "format", "File format",
			std::string(name) + " image under a .dds name (D3DX reads it as one)");
	fact(image, "reader", "Read by", "D3DX, which reads the bytes by their content");
	image.loads = true;
	image.decoded = true;
	image.levels.push_back({ uint32_t(rgba.width), uint32_t(rgba.height), std::move(rgba.pixels) });
	size_fact(image);
	fact(image, "texels", "Texels", texels);
	fact(image, "loads", "In the game", "loads it");
	classify_alpha(image);
	built_chain(image);
}

void read_dds(TextureImage &image, const std::vector<uint8_t> &bytes, bool first_level_only) {
	constexpr const char *kD3dx = "D3DX, which reads the bytes by their content";
	// D3DX's walk over its codecs (renderer::dds_read_by_content), the game's as the editor's: each
	// codec whose header test passes is tried in turn, the first that decodes the file's. The DDS,
	// PNG, PPM, PFM and HDR codecs decode here as the game decodes them; the BMP and DIB codecs are
	// taken at the BMP core's header rules (renderer::d3dx_bmp_rehead, d3dx_dib_to_bmp), the JPEG
	// and TGA codecs at their header tests, their pixels not decoded here.
	dds::DdsImage dds;
	std::string dds_error, error;
	RgbaImage rgba;
	std::vector<uint8_t> bmp;
	const std::optional<renderer::DdsCodec> codec =
			renderer::dds_read_by_content(bytes.data(), bytes.size(), [&](renderer::DdsCodec each) {
				switch (each) {
					case renderer::DdsCodec::Dds:
						return dds::dds_read(bytes.data(), bytes.size(), dds, dds_error) &&
								dds.loads;
					case renderer::DdsCodec::Png:
						return png::decode_png(bytes, rgba, error);
					case renderer::DdsCodec::Ppm:
						return renderer::decode_d3dx_ppm(bytes.data(), bytes.size(), rgba, error);
					case renderer::DdsCodec::Pfm:
						return renderer::decode_d3dx_pfm(bytes.data(), bytes.size(), rgba, error);
					case renderer::DdsCodec::Hdr: {
						int width = 0, height = 0;
						if (!renderer::d3dx_hdr_size(
									bytes.data(), bytes.size(), width, height, error))
							return false;
						rgba.width = width;
						rgba.height = height;
						rgba.pixels.assign(size_t(width) * size_t(height) * 4, 0);
						return renderer::decode_d3dx_hdr_into(
								bytes.data(), bytes.size(), rgba.pixels.data(), error);
					}
					case renderer::DdsCodec::Bmp:
						return renderer::d3dx_bmp_rehead(bytes.data(), bytes.size(), bmp, error);
					case renderer::DdsCodec::Dib:
						return renderer::d3dx_dib_to_bmp(bytes.data(), bytes.size(), bmp, error);
					case renderer::DdsCodec::Jpeg:
					case renderer::DdsCodec::Tga:
						return true;
				}
				return false;
			});
	bool parsed = true;
	if (!codec) {
		// A DDS header D3DX's DDS codec refuses, and no other codec decodes the bytes: its facts
		// and the refusal.
		if (!renderer::dds_codec_header_accepts(
					renderer::DdsCodec::Dds, bytes.data(), bytes.size())) {
			fact(image, "format", "File format", "unknown");
			fact(image, "reader", "Read by", kD3dx);
			image.refusal = "D3DX reads none of its formats from these bytes.";
			return;
		}
		dds = dds::DdsImage();
		dds_error.clear();
		parsed = dds::dds_read(bytes.data(), bytes.size(), dds, dds_error);
	}
	switch (codec ? *codec : renderer::DdsCodec::Dds) {
		case renderer::DdsCodec::Dds:
			break;
		case renderer::DdsCodec::Png:
			read_decoded(image, "PNG", rgba, "8 bits a channel, as D3DX's PNG reader expands them");
			return;
		case renderer::DdsCodec::Ppm:
			read_decoded(image, "PPM", rgba, "8 bits a channel, opaque");
			return;
		case renderer::DdsCodec::Pfm:
			read_decoded(image, "PFM", rgba,
					"32-bit floats a channel, shown through D3DX's 8-bit encode");
			return;
		case renderer::DdsCodec::Hdr:
			read_decoded(image, "Radiance HDR", rgba, "RGBE, shown through D3DX's 8-bit encode");
			return;
		case renderer::DdsCodec::Bmp:
		case renderer::DdsCodec::Dib:
		case renderer::DdsCodec::Jpeg:
		case renderer::DdsCodec::Tga: {
			const char *name = *codec == renderer::DdsCodec::Tga ? "TGA"
					: *codec == renderer::DdsCodec::Bmp			 ? "BMP"
					: *codec == renderer::DdsCodec::Dib			 ? "DIB"
																 : "JPEG";
			fact(image, "format", "File format",
					std::string(name) + " image under a .dds name (D3DX reads it as one)");
			fact(image, "reader", "Read by", kD3dx);
			image.loads = true;
			image.undecoded =
					std::string("D3DX's ") + name + " reader, which the editor does not decode yet";
			fact(image, "loads", "In the game", "loads it");
			return;
		}
	}
	fact(image, "format", "File format", "DDS (DirectDraw surface)");
	fact(image, "reader", "Read by", kD3dx);
	if (!parsed) {
		image.refusal = dds_error;
		return;
	}
	image.loads = dds.loads;
	image.refusal = dds.refusal;
	const dds::DdsFormat &format = dds.format;
	if (format.d3d) {
		std::string texels = format.name;
		if (format.compressed)
			texels += format.block_bytes == 8 ? ": 4 bits a texel, in 4 x 4 blocks" : ": 8 bits a texel, in 4 x 4 blocks";
		else
			texels += ": " + std::to_string(format.bits) + " bits a texel";
		if (format.d3d == dds::dds_fourcc('D', 'X', 'T', '2') || format.d3d == dds::dds_fourcc('D', 'X', 'T', '4'))
			texels += " (colour premultiplied by alpha)";
		fact(image, "texels", "Texels", texels);
		fact(image, "compression", "Compression",
		     format.compressed ? std::string(format.name) + " block compression (" + std::to_string(format.block_bytes) +
		                                 " bytes a block)"
		                       : "none");
	}
	if (!dds.loads) {
		fact(image, "loads", "In the game", "cannot load it: " + dds.refusal);
		return;
	}
	// The levels as D3DX reads them (renderer::decode_dds_levels: a DXT1, DXT4 or DXT5 level's blocks through
	// the port of the D3DX codec the game links; the other forms dds_read decoded). A thumbnail's: the chain's
	// first level alone, the others left coded.
	image.decoded = renderer::decode_dds_levels(bytes.data(), bytes.size(), dds, first_level_only ? 1 : 0);
	for (dds::DdsLevel &level : dds.levels) {
		if (first_level_only && !image.levels.empty()) break;
		image.levels.push_back({level.width, level.height, std::move(level.rgba)});
	}
	image.game_levels = uint32_t(dds.levels.size());
	if (!image.decoded) image.undecoded = std::string("its ") + format.name + " texels, which the editor does not decode yet";
	size_fact(image);
	fact(image, "loads", "In the game", "loads it");
	const uint32_t count = uint32_t(dds.levels.size());
	const dds::DdsLevel &smallest = dds.levels.back();
	fact(image, "mips", "Mip levels",
	     count == 1 ? "1 in the file: no level past the texture"
	                : std::to_string(count) + " in the file, down to " + sides(smallest.width, smallest.height));
	if (dds.faces == 6) fact(image, "faces", "Faces", "6: a cube map (the first face shown)");
	if (dds.depth > 1) fact(image, "depth", "Depth", std::to_string(dds.depth) + " slices: a volume (the first slice shown)");
	if (!dds.palette.empty()) {
		image.palette.resize(256 * 3);
		for (size_t i = 0; i < 256; ++i)
			for (size_t c = 0; c < 3; ++c) image.palette[i * 3 + c] = dds.palette[i * 4 + c];
		fact(image, "palette", "Palette", "256 colours");
	}
	if (dds.extra_bytes) fact(image, "extra", "Unread bytes", bytes_words(dds.extra_bytes) + " after the last level, which nothing reads");
	if (image.decoded) classify_alpha(image);
}

} // namespace

const char *texture_reader_token(TextureReader reader) {
	switch (reader) {
	case TextureReader::Tga: return "tga";
	case TextureReader::Dds: return "dds";
	case TextureReader::Pcx: return "pcx";
	case TextureReader::Png: return "png";
	case TextureReader::None: break;
	}
	return "none";
}

const char *texture_alpha_token(TextureAlpha alpha) {
	switch (alpha) {
	case TextureAlpha::None: return "none";
	case TextureAlpha::Mask: return "mask";
	case TextureAlpha::Graded: return "graded";
	}
	return "none";
}

TextureReader texture_reader_for(const std::string &name) {
	const size_t dot = name.find_last_of('.');
	if (dot == std::string::npos) return TextureReader::None;
	const std::string extension = strutil::to_lower(name.substr(dot));
	if (extension == ".tga" || extension == ".mdt") return TextureReader::Tga;
	if (extension == ".pcx") return TextureReader::Pcx;
	if (extension == ".dds") return TextureReader::Dds;
	if (extension == ".png") return TextureReader::Png;
	return TextureReader::None;
}

const TextureFact *TextureImage::fact(const std::string &key) const {
	for (const TextureFact &each : facts)
		if (each.key == key) return &each;
	return nullptr;
}

std::shared_ptr<const TextureImage> retexel_texture(const TextureImage &as_read, std::vector<uint8_t> rgba,
                                                    std::vector<uint8_t> indices, std::vector<uint8_t> luminance) {
	if (!as_read.decoded || as_read.levels.size() != 1) return nullptr;
	const TextureLevel &level = as_read.levels.front();
	if (rgba.size() != size_t(level.width) * size_t(level.height) * 4) return nullptr;
	auto image = std::make_shared<TextureImage>(as_read);
	image->levels.front().rgba = std::move(rgba);
	if (!as_read.indices.empty()) image->indices = std::move(indices);
	if (!as_read.luminance.empty()) image->luminance = std::move(luminance);
	// The alpha classed again, its fact where it was.
	size_t at = image->facts.size();
	for (size_t i = 0; i < image->facts.size(); ++i)
		if (image->facts[i].key == "alpha") at = i;
	if (at < image->facts.size()) image->facts.erase(image->facts.begin() + std::ptrdiff_t(at));
	const size_t before = image->facts.size();
	classify_alpha(*image);
	if (image->facts.size() == before + 1 && at < before) {
		TextureFact made = std::move(image->facts.back());
		image->facts.pop_back();
		image->facts.insert(image->facts.begin() + std::ptrdiff_t(at), std::move(made));
	}
	// The file's size is the bytes read: a save writes the texels again (a compressed form's size moves with them).
	for (TextureFact &fact : image->facts)
		if (fact.key == "file_size") fact.words += " as read; a save writes it again";
	return image;
}

std::shared_ptr<const TextureImage> decode_texture(const std::string &name, const std::vector<uint8_t> &bytes,
                                                   bool first_level_only) {
	auto image = std::make_shared<TextureImage>();
	image->reader = texture_reader_for(name);
	switch (image->reader) {
	case TextureReader::Tga: read_tga(*image, bytes); break;
	case TextureReader::Pcx: read_pcx(*image, bytes); break;
	case TextureReader::Dds: read_dds(*image, bytes, first_level_only); break;
	case TextureReader::Png: read_png(*image, bytes); break;
	case TextureReader::None: image->refusal = "No texture reader of the game takes a file of this name."; break;
	}
	if (!image->loads && !image->refusal.empty() && !image->fact("loads"))
		image->facts.push_back({"loads", "In the game", "cannot load it: " + image->refusal});
	image->facts.push_back({"file_size", "File size", bytes_words(bytes.size())});
	return image;
}

bool texture_texel(const TextureImage &image, size_t level, uint32_t x, uint32_t y, uint8_t rgba[4]) {
	if (level >= image.levels.size()) return false;
	const TextureLevel &at = image.levels[level];
	if (x >= at.width || y >= at.height || at.rgba.size() < size_t(at.width) * at.height * 4) return false;
	const uint8_t *texel = at.rgba.data() + (size_t(y) * at.width + x) * 4;
	for (int i = 0; i < 4; ++i) rgba[i] = texel[i];
	return true;
}

TextureHeader texture_header(const std::string &name, const std::vector<uint8_t> &bytes) {
	return texture_header_as(texture_reader_for(name), bytes);
}

TextureHeader texture_header_as(TextureReader reader, const std::vector<uint8_t> &bytes) {
	TextureHeader out;
	out.reader = reader;
	switch (out.reader) {
	case TextureReader::Tga: {
		// Unpacked from BFC1 first, as the models' reader does (read_tga).
		std::vector<uint8_t> unpacked;
		const std::vector<uint8_t> *data = &bytes;
		if (bfc1::bfc1_is_bfc1(bytes.data(), bytes.size())) {
			unpacked = bytes;
			if (!bfc1::bfc1_unpack(unpacked)) {
				out.refusal = "Its BFC1 packing does not unpack.";
				return out;
			}
			data = &unpacked;
			out.bfc1 = true;
		}
		tga::TgaHeader header;
		if (!tga::tga_read_header(data->data(), data->size(), header)) {
			out.refusal = "The TGA header is cut short.";
			return out;
		}
		out.read = true;
		out.tga_held = uint32_t(std::min<size_t>(data->size(), UINT32_MAX));
		out.width = uint32_t(std::max<int>(0, header.width));
		out.height = uint32_t(std::max<int>(0, header.height));
		out.tga_type = header.image_type;
		out.tga_bits = header.bits;
		out.tga_descriptor = header.descriptor;
		out.tga_map_type = header.colour_map_type;
		out.tga_map_length = header.map_length;
		out.tga_map_entry_bits = header.map_entry_bits;
		out.alpha = (header.image_type == 2 || header.image_type == 10) && header.bits == 32;
		out.tga_short = tga::tga_reads_past_end(header, data->data(), data->size());
		return out;
	}
	case TextureReader::Pcx: {
		// The header's fields (the 128 bytes before the texels: pcx_read_header).
		PcxHeader header;
		if (!pcx_read_header(bytes.data(), bytes.size(), header)) {
			out.refusal = "The PCX header is cut short or is not one.";
			return out;
		}
		out.read = true;
		out.pcx_bits = header.bits;
		out.pcx_planes = header.planes;
		out.pcx_bytes_per_line = header.bytes_per_line;
		out.width = uint32_t(header.width);
		out.height = uint32_t(header.height);
		return out;
	}
	case TextureReader::Dds: {
		dds::DdsImage image;
		std::string error;
		if (png::is_png(bytes)) {
			// D3DX reads a PNG under a .dds name as the PNG (read_dds).
			out.read = png::png_header_size(bytes, out.width, out.height);
			out.alpha = bytes.size() > 25 && (bytes[25] == 4 || bytes[25] == 6);
			return out;
		}
		if (!dds::dds_read(bytes.data(), bytes.size(), image, error) || !image.loads) {
			out.refusal = error.empty() ? image.refusal : error;
			return out;
		}
		out.read = true;
		out.width = image.header.width;
		out.height = image.header.height;
		out.dds_format = image.format.name;
		out.dds_bits = image.format.bits;
		out.dds_block_bytes = image.format.compressed ? image.format.block_bytes : 0;
		out.dds_levels = uint32_t(image.levels.size());
		if (out.dds_format == "DXT5" && !image.levels.empty() && image.levels[0].offset <= bytes.size()) {
			const dds::DdsLevel &first = image.levels[0];
			out.dds_dxt5_opaque = renderer::dxt5_first_level_opaque(bytes.data() + first.offset,
			                                                        std::min(first.bytes, bytes.size() - first.offset),
			                                                        first.width, first.height);
		}
		out.dds_faces = std::max<uint32_t>(1, uint32_t(image.faces));
		out.dds_depth = std::max<uint32_t>(1, uint32_t(image.depth));
		const std::string format = image.format.name;
		// A DXT1 block may hold a transparent texel (its three-colour mode), so it counts as holding one.
		out.alpha = format.rfind("DXT", 0) == 0 || format.find('A') != std::string::npos;
		return out;
	}
	case TextureReader::Png:
		out.read = png::png_header_size(bytes, out.width, out.height);
		if (!out.read) out.refusal = "The PNG header is cut short or is not one.";
		out.alpha = bytes.size() > 25 && (bytes[25] == 4 || bytes[25] == 6);
		return out;
	case TextureReader::None: out.refusal = "No texture reader of the game takes a file of this name."; break;
	}
	return out;
}

namespace {

// FastMem_Alloc's block for a request, in the int it computes: a size under 1 taken as 1, rounded up to 64,
// wrapping as its add does [orig: FastMem_Alloc @ 0x7697C1..0x7697D7].
int32_t fastmem_block(int32_t size) {
	const uint32_t asked = size < 1 ? 1u : uint32_t(size);
	return int32_t(asked + ((0u - asked) & (configfile::kFastMemStep - 1)));
}

} // namespace

TgaReaderRead tga_reader_read(const TextureHeader &header) {
	TgaReaderRead out;
	out.width = int16_t(uint16_t(header.width));
	out.height = int16_t(uint16_t(header.height));
	const int32_t count = out.width * out.height; // @ 0x56E677: at most 2^30
	out.texel_bytes = int64_t(count) * 4;
	// The pool's one block, the held bytes taken from it first; the texels' request fails where the block left
	// after them is smaller, however empty the pool: first fit over a signed compare [orig: FastMem_Alloc @
	// 0x7697FC..0x7697FF], the block split where more than a header is left (@ 0x769812..0x769819).
	const int32_t header_bytes = int32_t(configfile::kFastMemHeaderBytes);
	const int32_t payload = kGameMemoryPoolBytes - header_bytes;
	const int64_t held = header.tga_held > uint32_t(INT32_MAX) ? int64_t(INT32_MAX) : int64_t(header.tga_held);
	const int64_t held_block = held < 1 ? 64 : (held + 63) / 64 * 64;
	const int64_t left = held_block > payload ? -1 : payload - held_block - header_bytes;
	const int32_t request = int32_t(uint32_t(count) * 4u); // @ 0x56E67F..0x56E685
	const int32_t block = fastmem_block(request);
	if (left <= 0 || block > left) {
		out.outcome = TgaReaderOutcome::Refused;
		return out;
	}
	// The arm of the image type (@ 0x56E6D2): its texels decoded, cleared, copied, or none written.
	enum class Arm { Decode, Clear, Copy, None };
	Arm arm = Arm::None;
	switch (header.tga_type) {
	case 1: arm = header.tga_map_entry_bits == 24 ? Arm::Decode : Arm::Clear; break; // @ 0x56E6DD
	case 2: arm = header.tga_bits == 24 ? Arm::Decode : header.tga_bits == 32 ? Arm::Copy : Arm::Clear; break;
	case 3: arm = header.tga_bits == 8 ? Arm::Decode : Arm::None; break; // @ 0x56E7B3
	case 9:
	case 11: arm = Arm::Clear; break;
	case 10: arm = header.tga_bits == 24 || header.tga_bits == 32 ? Arm::Decode : Arm::Clear; break; // @ 0x56E7FE, @ 0x56E8B9
	default: arm = Arm::None; break; // @ 0x56E6CC: the switch's default writes nothing
	}
	// The block holds the 4 x count bytes where the request neither wrapped nor fell under 1. A decoding arm
	// writes count texels where count is above 0 (@ 0x56E6EE, @ 0x56E756, @ 0x56E7BD, @ 0x56E808, @ 0x56E8C3);
	// the clear and the copy take 4 x count as an unsigned length (@ 0x56E971, @ 0x56E7A5); the flip swaps rows
	// where the height is 2 or more and the width above 0 (@ 0x56E997, @ 0x56E9C2).
	const bool holds = count >= 0 && count < (1 << 29) && block > 0;
	const bool writes = arm == Arm::Decode ? count > 0 : arm == Arm::Clear || arm == Arm::Copy ? count != 0 : false;
	const bool flips = out.height >= 2 && out.width > 0;
	if ((writes || flips) && !holds) out.outcome = TgaReaderOutcome::Faults;
	else if (count <= 0) out.outcome = TgaReaderOutcome::Empty;
	else if (arm == Arm::Clear) out.outcome = TgaReaderOutcome::Black;
	else if (arm == Arm::None) out.outcome = TgaReaderOutcome::Stale;
	else out.outcome = TgaReaderOutcome::Decoded;
	return out;
}

} // namespace opennova::editor
