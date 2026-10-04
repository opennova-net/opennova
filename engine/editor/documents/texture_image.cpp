#include <editor/documents/texture_image.h>

#include <algorithm>
#include <array>
#include <cstdio>

#include <base/io/strutil.h>
#include <editor/import/png_decode.h>
#include <formats/bfc1/bfc1.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>
#include <formats/tga/tga_read.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/texture_dxt.h>

namespace opennova::editor {

namespace {

bool power_of_two(uint32_t side) { return side != 0 && (side & (side - 1)) == 0; }

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

// The game builds the mip chain of a texture made from texels (renderer::pixel_texture_mip_levels).
void built_chain(TextureImage &image) {
	const uint32_t levels = renderer::pixel_texture_mip_levels(image.width(), image.height());
	image.game_levels = levels;
	uint32_t w = image.width(), h = image.height();
	if (levels == 0) {
		fact(image, "mips", "Mip levels", "none in the file; the game builds its whole chain, down to 1 x 1");
		return;
	}
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
	     sides(w, h) + (power_of_two(w) && power_of_two(h) ? " (sides are powers of two)" : " (a side is no power of two)"));
}

void read_tga(TextureImage &image, const std::vector<uint8_t> &stored) {
	fact(image, "format", "File format", "TGA image");
	fact(image, "reader", "Read by", "the game's TGA reader");
	std::vector<uint8_t> bytes = stored;
	// The models' archive reader unpacks a BFC1 file first [orig: CTerrainTileData_LoadTGAFromArchive
	// @ 0x56E570, AudioFile_DecompressBFC_Aligned @ 0x75AFB0].
	if (bfc1::bfc1_is_bfc1(stored.data(), stored.size())) {
		uint32_t unpacked = 0;
		if (bfc1::bfc1_uncompressed_size(stored.data(), stored.size(), &unpacked) == 0) {
			std::vector<uint8_t> out(unpacked);
			size_t size = out.size();
			if (bfc1::bfc1_decompress(stored.data(), stored.size(), out.data(), &size) == 0) {
				out.resize(size);
				bytes = std::move(out);
				image.bfc1 = true;
			}
		}
		if (!image.bfc1) {
			image.refusal = "Its BFC1 packing does not unpack.";
			return;
		}
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
	// [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E570, the switch @ 0x56E6C2].
	const uint8_t type = h.image_type;
	const bool known = type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11;
	const bool unset = !known || (type == 3 && h.bits != 8);
	const bool zeroed = !unset && (type == 9 || type == 11 || ((type == 2 || type == 10) && h.bits != 24 && h.bits != 32) ||
	                               (type == 1 && h.map_entry_bits != 24));
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
	PcxGameImage pcx;
	RgbaImage rgba;
	std::string error;
	if (!decode_pcx_game(bytes.data(), bytes.size(), pcx, error) ||
	    !decode_pcx_menu_rgba(bytes.data(), bytes.size(), rgba, error)) {
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
	if (!decode_png(bytes, rgba, error)) {
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

void read_dds(TextureImage &image, const std::vector<uint8_t> &bytes) {
	constexpr const char *kD3dx = "D3DX, which reads the bytes by their content";
	// D3DX takes the first of its formats whose test the bytes pass (renderer::dds_reader_format).
	const renderer::DdsReaderFormat content = renderer::dds_reader_format(bytes.data(), bytes.size());
	switch (content) {
	case renderer::DdsReaderFormat::Dds: break;
	case renderer::DdsReaderFormat::Png:
		read_png(image, bytes, "PNG image under a .dds name (D3DX reads it as a PNG)", kD3dx);
		return;
	case renderer::DdsReaderFormat::Tga:
	case renderer::DdsReaderFormat::Bmp:
	case renderer::DdsReaderFormat::Jpeg: {
		const char *name = content == renderer::DdsReaderFormat::Tga ? "TGA" : content == renderer::DdsReaderFormat::Bmp ? "BMP" : "JPEG";
		fact(image, "format", "File format", std::string(name) + " image under a .dds name (D3DX reads it as one)");
		fact(image, "reader", "Read by", kD3dx);
		image.loads = true;
		image.undecoded = std::string("D3DX's ") + name + " reader, which the editor does not decode yet";
		fact(image, "loads", "In the game", "loads it");
		return;
	}
	case renderer::DdsReaderFormat::None:
		fact(image, "format", "File format", "unknown");
		fact(image, "reader", "Read by", kD3dx);
		image.refusal = "D3DX reads none of its formats from these bytes.";
		return;
	}
	fact(image, "format", "File format", "DDS (DirectDraw surface)");
	fact(image, "reader", "Read by", kD3dx);
	dds::DdsImage dds;
	std::string error;
	if (!dds::dds_read(bytes.data(), bytes.size(), dds, error)) {
		image.refusal = error;
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
	// A DXT1, DXT4 or DXT5 level's blocks through the port of the D3DX codec the game links (its DXT4 is
	// DXT5's blocks over colour premultiplied by alpha); the other forms dds_read decoded.
	const bool dxt1 = format.d3d == dds::dds_fourcc('D', 'X', 'T', '1');
	const bool dxt5 = format.d3d == dds::dds_fourcc('D', 'X', 'T', '4') || format.d3d == dds::dds_fourcc('D', 'X', 'T', '5');
	for (dds::DdsLevel &level : dds.levels) {
		if (dxt1 || dxt5) {
			renderer::DxtSurface surface;
			surface.format = dxt1 ? renderer::TextureDxtFormat::Dxt1 : renderer::TextureDxtFormat::Dxt5;
			surface.width = level.width;
			surface.height = level.height;
			surface.blocks.assign(bytes.begin() + std::ptrdiff_t(level.offset),
			                      bytes.begin() + std::ptrdiff_t(level.offset + level.bytes));
			level.rgba = renderer::encode_rgba8(renderer::decode_dxt_surface(surface));
		}
		image.levels.push_back({level.width, level.height, std::move(level.rgba)});
	}
	image.game_levels = uint32_t(dds.levels.size());
	image.decoded = format.decoded || dxt1 || dxt5;
	if (!image.decoded) image.undecoded = std::string("its ") + format.name + " texels, which the editor does not decode yet";
	size_fact(image);
	fact(image, "loads", "In the game", "loads it");
	const uint32_t count = uint32_t(image.levels.size());
	const TextureLevel &smallest = image.levels.back();
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

std::shared_ptr<const TextureImage> decode_texture(const std::string &name, const std::vector<uint8_t> &bytes) {
	auto image = std::make_shared<TextureImage>();
	image->reader = texture_reader_for(name);
	switch (image->reader) {
	case TextureReader::Tga: read_tga(*image, bytes); break;
	case TextureReader::Pcx: read_pcx(*image, bytes); break;
	case TextureReader::Dds: read_dds(*image, bytes); break;
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
			uint32_t size = 0;
			if (bfc1::bfc1_uncompressed_size(bytes.data(), bytes.size(), &size) == 0) {
				unpacked.resize(size);
				size_t made = unpacked.size();
				if (bfc1::bfc1_decompress(bytes.data(), bytes.size(), unpacked.data(), &made) == 0) {
					unpacked.resize(made);
					data = &unpacked;
					out.bfc1 = true;
				}
			}
			if (!out.bfc1) {
				out.refusal = "Its BFC1 packing does not unpack.";
				return out;
			}
		}
		tga::TgaHeader header;
		if (!tga::tga_read_header(data->data(), data->size(), header)) {
			out.refusal = "The TGA header is cut short.";
			return out;
		}
		out.read = true;
		out.width = uint32_t(std::max<int>(0, header.width));
		out.height = uint32_t(std::max<int>(0, header.height));
		out.tga_type = header.image_type;
		out.tga_bits = header.bits;
		out.tga_descriptor = header.descriptor;
		out.tga_map_type = header.colour_map_type;
		out.tga_map_length = header.map_length;
		out.tga_map_entry_bits = header.map_entry_bits;
		out.alpha = (header.image_type == 2 || header.image_type == 10) && header.bits == 32;
		return out;
	}
	case TextureReader::Pcx:
		// The header's fields (the 128 bytes before the texels): bits a plane @ 3, the window @ 4..11,
		// the planes @ 65, the bytes a line @ 66.
		if (bytes.size() < 128 || bytes[0] != 0x0A) {
			out.refusal = "The PCX header is cut short or is not one.";
			return out;
		}
		out.read = true;
		out.pcx_bits = bytes[3];
		out.pcx_planes = bytes[65];
		out.pcx_bytes_per_line = uint16_t(bytes[66] | bytes[67] << 8);
		out.width = uint32_t((bytes[8] | bytes[9] << 8) - (bytes[4] | bytes[5] << 8) + 1);
		out.height = uint32_t((bytes[10] | bytes[11] << 8) - (bytes[6] | bytes[7] << 8) + 1);
		return out;
	case TextureReader::Dds: {
		dds::DdsImage image;
		std::string error;
		if (is_png(bytes)) {
			// D3DX reads a PNG under a .dds name as the PNG (read_dds).
			out.read = png_header_size(bytes, out.width, out.height);
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
		out.dds_levels = uint32_t(image.levels.size());
		const std::string format = image.format.name;
		// A DXT1 block may hold a transparent texel (its three-colour mode), so it counts as holding one.
		out.alpha = format.rfind("DXT", 0) == 0 || format.find('A') != std::string::npos;
		return out;
	}
	case TextureReader::Png:
		out.read = png_header_size(bytes, out.width, out.height);
		if (!out.read) out.refusal = "The PNG header is cut short or is not one.";
		out.alpha = bytes.size() > 25 && (bytes[25] == 4 || bytes[25] == 6);
		return out;
	case TextureReader::None: out.refusal = "No texture reader of the game takes a file of this name."; break;
	}
	return out;
}

} // namespace opennova::editor
