#pragma once

// The texture forms JO:CA ships, one file of each, minted here from integer data (our writers where one makes the
// form, the header and texels byte by byte where none does), no retail byte carried. The forms are the census of
// the 2,579 textures of the JO:CA install (the retail-free program, 2026-10-19), each read as the texture document
// reads it (SCR and BFC1 undone, base/vfs vfs_decode_payload, then the reader its name picks): every one loads,
// decodes, holds texels and stands the right way up, and raises no finding of its own (validate_texture_file).
// The texture checks (editor_texture_checks) and the texture document (editor_texture_document) read them.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <formats/dds/dds.h>
#include <formats/pcx/pcx.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_encode.h>
#include <formats/tga/tga.h>

namespace texture_forms {

struct TextureForm {
	std::string name;           // its name in the install's manner, the extension its reader's
	std::vector<uint8_t> bytes; // as the install stores it
	bool packed = false;        // BFC1-stored: the game and the document read it unpacked
	const char *what = "";      // the form in words
};

// A w x h picture of distinct texels, R, G, B, A each, the top row first; opaque, or of graded alpha.
inline std::vector<uint8_t> pattern_rgba(uint32_t w, uint32_t h, bool opaque) {
	std::vector<uint8_t> out;
	out.reserve(size_t(w) * h * 4);
	for (uint32_t i = 0; i < w * h; ++i) {
		out.push_back(uint8_t(i * 7 + 3));
		out.push_back(uint8_t(255 - i * 5));
		out.push_back(uint8_t(i * 3 + 40));
		out.push_back(opaque ? 255 : uint8_t(64 + (i * 11) % 192));
	}
	return out;
}

inline void append_u16(std::vector<uint8_t> &out, uint32_t v) {
	out.push_back(uint8_t(v));
	out.push_back(uint8_t(v >> 8));
}
inline void append_u32(std::vector<uint8_t> &out, uint32_t v) {
	append_u16(out, v & 0xFFFF);
	append_u16(out, v >> 16);
}
inline void append_u32_be(std::vector<uint8_t> &out, uint32_t v) {
	for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(v >> shift));
}

// BFC1 as the install stores its packed textures (formats/bfc1: the "BFC1" mark, the unpacked size, a zlib
// stream), the stream of stored blocks (RFC 1950, RFC 1951 3.2.4), which inflate reads as any other: a packing
// minted with no compressor.
inline std::vector<uint8_t> bfc1_stored(const std::vector<uint8_t> &plain) {
	std::vector<uint8_t> out = {'B', 'F', 'C', '1'};
	append_u32(out, uint32_t(plain.size()));
	out.push_back(0x78);
	out.push_back(0x01);
	size_t at = 0;
	do {
		const size_t n = std::min<size_t>(65535, plain.size() - at);
		out.push_back(at + n == plain.size() ? 1 : 0);
		append_u16(out, uint32_t(n));
		append_u16(out, uint32_t(~n & 0xFFFF));
		out.insert(out.end(), plain.begin() + long(at), plain.begin() + long(at + n));
		at += n;
	} while (at < plain.size());
	uint32_t a = 1, b = 0;
	for (const uint8_t byte : plain) {
		a = (a + byte) % 65521;
		b = (b + a) % 65521;
	}
	append_u32_be(out, (b << 16) | a);
	return out;
}

// The TGA 2.0 footer (formats/tga TGA_FOOTER_SIZE): the extension and developer areas' offsets, the signature.
inline void append_tga_footer(std::vector<uint8_t> &out, uint32_t extension = 0) {
	append_u32(out, extension);
	append_u32(out, 0);
	const char signature[] = "TRUEVISION-XFILE.";
	out.insert(out.end(), signature, signature + sizeof signature); // its NUL too
}

// An 18-byte TGA header of the fields the shipped files set.
inline std::vector<uint8_t> tga_header(uint8_t map_type, uint8_t type, uint16_t map_length, uint8_t map_bits, uint16_t w,
                                       uint16_t h, uint8_t bits, uint8_t descriptor) {
	std::vector<uint8_t> out = {0, map_type, type, 0, 0};
	append_u16(out, map_length);
	out.push_back(map_bits);
	append_u16(out, 0);
	append_u16(out, 0);
	append_u16(out, w);
	append_u16(out, h);
	out.push_back(bits);
	out.push_back(descriptor);
	return out;
}

// An uncompressed true-colour TGA through the writer (formats/tga tga_write): its depth, its descriptor, its footer.
inline std::vector<uint8_t> tga_true_colour(uint32_t w, uint32_t h, uint8_t bits, uint8_t descriptor, bool footer) {
	opennova::tga::TgaForm form;
	form.bits = bits;
	form.descriptor = descriptor;
	form.footer = footer;
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write(pattern_rgba(w, h, bits == 24).data(), w, h, form, out, error);
	return out;
}

inline opennova::IndexedImage8 indexed_pattern(int w, int h) {
	opennova::IndexedImage8 image;
	image.width = w;
	image.height = h;
	for (int i = 0; i < w * h; ++i) image.indices.push_back(uint8_t((i * 7) % 251));
	for (int i = 0; i < 256; ++i) {
		image.palette[i][0] = uint8_t(i);
		image.palette[i][1] = uint8_t(255 - i);
		image.palette[i][2] = uint8_t(i * 3);
	}
	return image;
}

// A DXT5 of `side` with its whole chain to 1 x 1, every block's colour endpoints apart, its alpha endpoints
// `alpha0` and `alpha1` (both 255: opaque, which the game stores as DXT1, renderer::dxt5_first_level_opaque).
inline std::vector<uint8_t> dxt5_chain(uint32_t side, uint8_t alpha0, uint8_t alpha1) {
	std::vector<std::vector<uint8_t>> levels;
	for (uint32_t s = side;; s /= 2) {
		const size_t blocks = size_t(std::max(1u, s / 4)) * std::max(1u, s / 4);
		std::vector<uint8_t> level(blocks * 16, 0);
		for (size_t b = 0; b < blocks; ++b) {
			uint8_t *block = &level[b * 16];
			block[0] = alpha0;
			block[1] = alpha1;
			block[8] = 0xFF; // colour 0 white, colour 1 a dark blue, the indices mixed
			block[9] = 0xFF;
			block[10] = uint8_t(b * 13);
			block[11] = 0x10;
			for (int i = 12; i < 16; ++i) block[i] = uint8_t(0x1B + b + i);
		}
		levels.push_back(std::move(level));
		if (s == 1) break;
	}
	std::vector<uint8_t> out;
	std::string error;
	opennova::dds::dds_write_dxt(opennova::dds::dds_fourcc('D', 'X', 'T', '5'), side, side, levels, out, error);
	return out;
}

inline uint32_t crc32(const uint8_t *data, size_t size, uint32_t crc = 0xFFFFFFFFu) {
	for (size_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return crc;
}

// A PNG chunk of `type` over `data`, its length and CRC (the PNG specification's chunk layout).
inline std::vector<uint8_t> png_chunk(const char type[4], const std::vector<uint8_t> &data) {
	std::vector<uint8_t> out;
	append_u32_be(out, uint32_t(data.size()));
	std::vector<uint8_t> body(type, type + 4);
	body.insert(body.end(), data.begin(), data.end());
	out.insert(out.end(), body.begin(), body.end());
	append_u32_be(out, crc32(body.data(), body.size()) ^ 0xFFFFFFFFu);
	return out;
}

// A 256-colour palette PNG (8-bit indices, colour type 3) through the writer (formats/png encode_png_indexed),
// with the ancillary chunks the shipped ones carry before their palette (pHYs, gAMA, cHRM), and a tRNS where
// `transparent`.
inline std::vector<uint8_t> png_palette(uint32_t w, uint32_t h, bool transparent) {
	std::vector<uint8_t> indices, palette, alpha;
	for (uint32_t i = 0; i < w * h; ++i) indices.push_back(uint8_t((i * 5) % 256));
	for (int i = 0; i < 256; ++i) palette.insert(palette.end(), {uint8_t(i), uint8_t(255 - i), uint8_t(i / 2)});
	for (int i = 0; i < 16; ++i) alpha.push_back(uint8_t(i * 16));
	std::vector<uint8_t> png = opennova::png::encode_png_indexed(indices.data(), w, h, palette.data(), 256,
	                                                             transparent ? alpha.data() : nullptr, transparent ? 16 : 0);
	// After the signature (8 bytes) and IHDR (25): the ancillary chunks, which a reader skips.
	std::vector<uint8_t> ancillary;
	std::vector<uint8_t> phys, gama, chrm;
	append_u32_be(phys, 2835);
	append_u32_be(phys, 2835);
	phys.push_back(1);
	append_u32_be(gama, 45455);
	for (const uint32_t v : {31270u, 32900u, 64000u, 33000u, 30000u, 60000u, 15000u, 6000u}) append_u32_be(chrm, v);
	for (const auto &chunk : {png_chunk("pHYs", phys), png_chunk("gAMA", gama), png_chunk("cHRM", chrm)})
		ancillary.insert(ancillary.end(), chunk.begin(), chunk.end());
	if (png.size() > 33) png.insert(png.begin() + 33, ancillary.begin(), ancillary.end());
	return png;
}

// One file of each form the install ships (in parentheses, how many files of it the census counted).
inline std::vector<TextureForm> shipped_texture_forms() {
	std::vector<TextureForm> out;
	const auto add = [&](const char *name, std::vector<uint8_t> bytes, const char *what, bool packed = false) {
		TextureForm form;
		form.name = name;
		form.bytes = std::move(bytes);
		form.packed = packed;
		form.what = what;
		out.push_back(std::move(form));
	};
	// TGA, image type 2: 24 bits with descriptor 0 and 32 bits with descriptor 8, each ending with the TGA 2.0
	// footer (95 and 536 files), sides powers of two or not.
	add("skin24.tga", tga_true_colour(16, 16, 24, 0, true), "a 24-bit TGA, the footer");
	add("strip24.tga", tga_true_colour(4, 40, 24, 0, true), "a 24-bit TGA of sides no power of two, the footer");
	add("skin32.tga", tga_true_colour(16, 16, 32, 8, true), "a 32-bit TGA, descriptor 8, the footer");
	add("icon32.tga", tga_true_colour(14, 24, 32, 8, true), "a 32-bit TGA of sides no power of two, the footer");
	// Eight carry a TGA 2.0 extension area of 495 bytes between the texels and the footer (its size field 494),
	// the footer's extension offset naming it.
	{
		std::vector<uint8_t> bytes = tga_true_colour(8, 8, 32, 8, false);
		const uint32_t texels_end = uint32_t(bytes.size());
		std::vector<uint8_t> area(495, 0);
		area[0] = uint8_t(494);
		area[1] = uint8_t(494 >> 8);
		bytes.insert(bytes.end(), area.begin(), area.end());
		append_tga_footer(bytes, texels_end);
		add("cement32.tga", bytes, "a 32-bit TGA with a TGA 2.0 extension area before the footer");
	}
	// One run-length 24-bit TGA (image type 10) whose header names 24-bit colour-map entries with no colour map,
	// no footer.
	{
		std::vector<uint8_t> bytes = tga_header(0, 10, 0, 24, 8, 8, 24, 0);
		for (int row = 0; row < 8; ++row) {
			bytes.insert(bytes.end(), {uint8_t(0x80 | 3), uint8_t(row * 20), 0x40, 0xC0}); // a run of 4
			bytes.push_back(3);                                                         // 4 texels raw
			for (int x = 0; x < 4; ++x) bytes.insert(bytes.end(), {uint8_t(x * 50), uint8_t(row * 30), 0x80});
		}
		add("arrow24.tga", bytes, "a run-length 24-bit TGA (image type 10), no footer");
	}
	// One 8-bit grey TGA (image type 3) of descriptor 8 and sides no power of two, the footer.
	{
		std::vector<uint8_t> bytes = tga_header(0, 3, 0, 0, 16, 14, 8, 8);
		for (int i = 0; i < 16 * 14; ++i) bytes.push_back(uint8_t(i));
		append_tga_footer(bytes);
		add("sparks8.tga", bytes, "an 8-bit grey TGA (image type 3), the footer");
	}
	// Three colour-mapped TGAs (image type 1): 256 entries of 24 bits, 8-bit indices, descriptor 8, the footer.
	{
		std::vector<uint8_t> bytes = tga_header(1, 1, 256, 24, 16, 16, 8, 8);
		for (int i = 0; i < 256; ++i) bytes.insert(bytes.end(), {uint8_t(i), uint8_t(i / 2), uint8_t(255 - i)});
		for (int i = 0; i < 16 * 16; ++i) bytes.push_back(uint8_t((i * 3) % 256));
		append_tga_footer(bytes);
		add("flash8.tga", bytes, "a colour-mapped TGA (image type 1), 256 entries of 24 bits, the footer");
	}
	// BFC1-stored TGAs: 24 bits (39) and 32 bits (26), the footer.
	add("detail24.tga", bfc1_stored(tga_true_colour(16, 16, 24, 0, true)), "a BFC1-stored 24-bit TGA", true);
	add("detail32.tga", bfc1_stored(tga_true_colour(16, 16, 32, 8, true)), "a BFC1-stored 32-bit TGA", true);
	// .mdt: a TGA, BFC1-stored every one: 24 bits and the footer (42), 32 bits of descriptor 0 and no footer (22),
	// 32 bits of descriptor 8 and the footer (146).
	add("normal24.mdt", bfc1_stored(tga_true_colour(16, 16, 24, 0, true)), "a BFC1-stored 24-bit .mdt", true);
	add("normal32.mdt", bfc1_stored(tga_true_colour(16, 16, 32, 0, false)), "a BFC1-stored 32-bit .mdt, descriptor 0, no footer",
	    true);
	add("normal32a.mdt", bfc1_stored(tga_true_colour(16, 16, 32, 8, true)), "a BFC1-stored 32-bit .mdt, descriptor 8", true);
	// PCX: 8 bits in one plane, version 5, run-length, its rows its (even) width, the palette's mark, sides powers of
	// two (55) or not (91); and two of three planes (24-bit colour, no palette).
	{
		std::vector<uint8_t> bytes;
		std::string error;
		opennova::encode_pcx_indexed(indexed_pattern(16, 16), bytes, error);
		add("cloud.pcx", bytes, "an 8-bit PCX");
		bytes.clear();
		opennova::encode_pcx_indexed(indexed_pattern(20, 14), bytes, error);
		add("screen.pcx", bytes, "an 8-bit PCX of sides no power of two");
		opennova::RgbImage rgb;
		rgb.width = 20;
		rgb.height = 14;
		for (int i = 0; i < 20 * 14; ++i) rgb.pixels.insert(rgb.pixels.end(), {uint8_t(i), uint8_t(255 - i), uint8_t(i * 3)});
		bytes.clear();
		opennova::encode_pcx_rgb(rgb, bytes, error);
		add("loading24.pcx", bytes, "a 24-bit PCX (three planes)");
	}
	// DDS: DXT5 with its whole chain, BFC1-stored, of alpha (538) and opaque (954), sides powers of two; one cube map
	// of A8R8G8B8, one level.
	add("alpha.dds", bfc1_stored(dxt5_chain(32, 0, 255)), "a BFC1-stored DXT5 of graded alpha, its whole chain", true);
	add("opaque.dds", bfc1_stored(dxt5_chain(32, 255, 255)), "a BFC1-stored opaque DXT5, its whole chain", true);
	{
		std::vector<std::vector<uint8_t>> faces;
		const uint8_t *each[6] = {};
		for (int f = 0; f < 6; ++f) {
			faces.push_back(pattern_rgba(16, 16, false));
			for (size_t i = 0; i < faces.back().size(); i += 4) faces.back()[i] = uint8_t(faces.back()[i] + f * 40);
		}
		for (int f = 0; f < 6; ++f) each[f] = faces[size_t(f)].data();
		std::vector<uint8_t> bytes;
		std::string error;
		opennova::dds::dds_write_cube_a8r8g8b8(each, 16, bytes, error);
		add("cube.dds", bytes, "an A8R8G8B8 cube map, one level");
	}
	// PNG: 8-bit palette, sides no power of two, with and without transparency.
	add("credits.png", png_palette(23, 17, false), "a palette PNG");
	add("veil.png", png_palette(23, 17, true), "a palette PNG with transparency");
	return out;
}

} // namespace texture_forms
