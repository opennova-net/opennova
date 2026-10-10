// Synthetic Black Hawk Down GP models (formats/threedi_gp), built byte by byte in the layout
// BHD's loader reads [orig: GP_LoadModel @ 0x510E10 (dfbhd)]: nothing of ours writes GP, so no
// GP fixture can be minted. gpm_model() is a rigid two-part model with every section (lights,
// the vertex stream, occlusion, a collision section whose volume widens its box, a CTRL register,
// two MTRX rows, an opaque list and an alpha strip, an unlit additive material); gpp_model() a
// skinned one with a global batch over a two-part palette. Used by ctest threedi_gp and the
// editor's import tests.
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include <formats/threedi_gp/threedi_gp.h>

namespace gp_test {

namespace gp = opennova::threedi_gp;

struct Bytes {
	std::vector<uint8_t> b;
	void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v)); }
	void u16(uint32_t v) { u8(v); u8(v >> 8); }
	void u32(uint32_t v) { u16(v); u16(v >> 16); }
	void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
	void f32(float v) {
		uint32_t u;
		std::memcpy(&u, &v, 4);
		u32(u);
	}
	void zeros(size_t n) { b.insert(b.end(), n, 0); }
	void name(const char *s, size_t width) {
		const size_t n = std::strlen(s);
		for (size_t i = 0; i < width; ++i) u8(i < n ? static_cast<uint8_t>(s[i]) : 0);
	}
	void append(const Bytes &o) { b.insert(b.end(), o.b.begin(), o.b.end()); }
	void poke32(size_t at, uint32_t v) {
		for (int k = 0; k < 4; ++k) b[at + static_cast<size_t>(k)] = static_cast<uint8_t>(v >> (8 * k));
	}
};

inline constexpr int32_t kOne = 65536;

inline Bytes header(char kind, uint32_t flags, int lods, int vertices, int points, int registers, int matrices, int occlusion) {
	Bytes h;
	h.zeros(gp::kHeaderSize);
	h.b[0] = 'G';
	h.b[1] = 'P';
	h.b[2] = static_cast<uint8_t>(kind);
	h.b[3] = 2;
	h.poke32(0x04, 0x103);
	std::memcpy(&h.b[0x08], "crate", 5);
	h.poke32(0x18, flags);
	h.poke32(0x1C, static_cast<uint32_t>(lods));
	h.poke32(0x20, 200u << 16);
	h.poke32(0x40, 0x676E7263u); // 'gnrc' as the multi-character constant: bytes "crng"
	h.poke32(0x60, static_cast<uint32_t>(2 * kOne));
	h.poke32(0x88, static_cast<uint32_t>(vertices));
	h.poke32(0xB0, static_cast<uint32_t>(points));
	h.poke32(0xBC, static_cast<uint32_t>(registers));
	h.poke32(0xC4, static_cast<uint32_t>(matrices));
	h.poke32(0xD0, static_cast<uint32_t>(occlusion));
	return h;
}

inline void texture_row(Bytes &o, const char *name, int16_t id) {
	o.name(name, 16);
	o.zeros(16 + 4);
	o.u16(static_cast<uint16_t>(id));
	o.u16(0x0702);
	o.zeros(20);
}

// A collision block of one section: a triangle, its normal, one six-plane volume wider than
// the section's stored box, one translation.
inline void collision_block(Bytes &o, bool empty) {
	Bytes blob;
	if (!empty) {
		const int16_t v[3][3] = {{0, 0, 0}, {256, 0, 0}, {0, 256, 0}};
		for (const auto &p : v) {
			for (int16_t c : p) blob.u16(static_cast<uint16_t>(c));
			blob.u16(0);
		}
		blob.u16(0);
		blob.u16(0);
		blob.u16(16384);
		blob.u16(1);
		blob.u16(0); blob.u16(1); blob.u16(2); blob.u16(0); // corners, normal
		blob.i32(0);
		const int32_t face_box[6] = {0, kOne, 0, kOne, 0, 0};
		for (int32_t w : face_box) blob.i32(w);
		blob.u32(0x2); // a face BHD's rounds pass
		blob.u8(12);
		blob.zeros(3);
		// The section.
		blob.i32(1);
		blob.i32(3); blob.u32(0);
		blob.i32(1); blob.u32(0);
		blob.i32(1); blob.u32(0);
		blob.i32(1); blob.u32(0);
		blob.i32(0);
		blob.zeros(12);
		blob.i32(0); blob.i32(0); blob.i32(0);
		const int32_t box[6] = {0, kOne, 0, kOne, 0, 0};
		for (int32_t w : box) blob.i32(w);
		blob.i32(kOne / 2); blob.i32(kOne / 2); blob.i32(0);
		blob.i32(46341);
		blob.zeros(24);
		blob.i32(kOne); blob.i32(0); blob.i32(0); // translation
		for (int p = 0; p < 6; ++p) { // planes
			blob.i32(p == 0 ? -65535 : 0);
			blob.i32(p == 0 ? 3 : 0);
			blob.i32(p == 0 ? 0 : kOne);
			blob.i32(-2 * kOne);
		}
		blob.i32(1); blob.i32(0); // the volume
		blob.zeros(40);
		const int32_t vbox[6] = {-kOne, 2 * kOne, -kOne, kOne, 0, kOne};
		for (int32_t w : vbox) blob.i32(w);
		blob.i32(6);
		blob.zeros(20);
	}
	o.i32(0);
	o.u32(static_cast<uint32_t>(blob.b.size()));
	o.u32(0);
	o.i32(3 * kOne); o.i32(2 * kOne); o.i32(kOne);
	const int32_t box[6] = {0, kOne, 0, kOne, 0, 0};
	for (int32_t w : box) o.i32(w);
	const int32_t counts[7] = {3, 1, 1, 1, 1, 6, 1};
	for (int32_t c : counts) {
		o.i32(empty ? 0 : c);
		o.u32(0);
	}
	o.zeros(0x88 - 0x68);
	o.append(blob);
}

inline void generator(Bytes &o, uint8_t style, uint8_t param, int16_t rate, int16_t start, int16_t end) {
	o.u8(style); o.u8(param);
	o.u16(static_cast<uint16_t>(rate)); o.u16(static_cast<uint16_t>(start)); o.u16(static_cast<uint16_t>(end));
}

inline void material(Bytes &o, uint32_t attributes, int8_t region, uint8_t blend, uint8_t rgb_style) {
	o.name("ignored", 16);
	o.u32(attributes);
	o.zeros(4);
	o.zeros(16);
	o.u8(static_cast<uint8_t>(region)); o.u8(static_cast<uint8_t>(region)); o.u8(static_cast<uint8_t>(region));
	o.u8(0);
	o.zeros(4);
	o.u8(0); o.u8(0); o.u8(0); // environment pass, its VS form, the shader
	o.zeros(5);
	o.u8(2); o.u8(2); o.u8(0); o.u8(128); o.u8(blend); o.u8(0);
	o.zeros(18);
	generator(o, 0, 0, 0, 0, 0);
	generator(o, 0, 0, 0, 0, 0);
	o.u8(rgb_style); o.u8(0); o.u16(0);
	o.u8(0x10); o.u8(0x20); o.u8(0x30); o.u8(0);
	o.u8(0x40); o.u8(0x50); o.u8(0x60); o.u8(0);
	generator(o, 0, 0, 0, 0, 0);
	o.zeros(36);
}

// A rigid two-part model: the parts' first batches hold an opaque list and an alpha strip.
inline std::vector<uint8_t> gpm_model() {
	Bytes f = header('M', gp::kFlagLights | gp::kFlagVertexStream | gp::kFlagOcclusion, 1, 4, 1, 1, 2, 1);
	f.i32(kOne); f.i32(0); f.i32(0); f.i32(0); f.i32(0); f.i32(kOne); f.i32(0); f.u8('S'); f.zeros(3); f.name("eye", 16);
	f.u32(2);
	texture_row(f, "box.tga", 0);
	texture_row(f, "glow.tga", 1);
	collision_block(f, false);
	const float pos[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
	for (const auto &p : pos) {
		for (float c : p) f.f32(c);
		f.f32(0); f.f32(0); f.f32(1);
		f.u32(0xFF000000u);
		f.f32(p[0]); f.f32(p[1]); f.f32(0); f.f32(0);
	}
	Bytes blob;
	blob.f32(1); blob.f32(0); blob.f32(0); // the attach point
	for (int part = 0; part < 2; ++part) {
		blob.u32(0); blob.i32(1); blob.u32(0); blob.i32(0);
		blob.f32(9); blob.f32(9); blob.f32(9); // rel: no reader
		blob.f32(part == 0 ? 0.0f : 1.0f); blob.f32(0); blob.f32(0);
		blob.f32(0.5f); blob.f32(0.5f); blob.f32(0);
		blob.i32(kOne);
		blob.zeros(16);
	}
	blob.u32(0); blob.i32(1); blob.u32(0); blob.i32(0); blob.zeros(16);
	blob.u32(0); blob.i32(0); blob.u32(0); blob.i32(1); blob.zeros(16);
	blob.i32(0); blob.u32(0x72646441u); blob.u16(3); blob.u16(0); blob.i32(0); blob.i32(0); blob.i32(2); blob.zeros(16);
	blob.u16(0); blob.u16(1); blob.u16(2);
	blob.i32(1); blob.u32(0x72646441u); blob.u16(4); blob.u16(0); blob.i32(1); blob.i32(0); blob.i32(3); blob.zeros(16);
	blob.u16(0); blob.u16(1); blob.u16(2); blob.u16(3);
	material(blob, 0x3, 0, 1, 0);
	material(blob, 0x140, 1, 4, 113);
	for (int part = 0; part < 2; ++part) {
		blob.u32(part == 0 ? 0u : 5u);
		blob.u8(0); blob.u8(static_cast<uint32_t>(part)); blob.zeros(2);
		blob.i32(part);
		generator(blob, part == 0 ? 0 : 113, 0, 0, 0, 100);
		blob.zeros(40);
		blob.zeros(32);
	}
	f.u32(static_cast<uint32_t>(blob.b.size()));
	f.u32(0); f.u32(2); f.u32(0); f.u32(2); f.u32(0); f.u32(1); f.u32(0);
	f.zeros(0x50 - 0x20);
	f.u32(gp::kRenderPartAnimations);
	f.zeros(0x88 - 0x54);
	f.append(blob);
	f.name("HELO_ROTOR", 16); f.zeros(28);
	const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	const float turn[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1};
	for (float v : identity) f.f32(v);
	for (float v : turn) f.f32(v);
	f.u32(256); f.u32(60 + 48);
	f.zeros(36); f.u32(1); f.zeros(20);
	f.u8(24); f.u8(0); f.u16(0);
	f.u8(1); f.u8(2); f.u8(3); f.u8(0);
	f.u8(4); f.u8(5); f.u8(6); f.u8(0);
	f.f32(0.5f); f.f32(1); f.f32(0); f.f32(1); f.f32(5); f.i32(1); f.zeros(12);
	f.u32(0); f.u32(0); f.u32(0); f.u32(0);
	for (int v = 0; v < 4; ++v) {
		f.f32(1); f.f32(0); f.f32(0);
		f.f32(0); f.f32(1); f.f32(0);
	}
	f.u8(0); f.u8(0); f.u8(0); f.u8(0);
	f.f32(9); f.f32(9); f.f32(9); f.f32(9);
	f.i32(3); f.u32(0); f.i32(1); f.u32(0); f.i32(1); f.u32(0);
	f.zeros(16);
	f.f32(0); f.f32(0); f.f32(0);
	f.f32(2); f.f32(0); f.f32(0);
	f.f32(0); f.f32(1); f.f32(0);
	f.f32(0); f.f32(0); f.f32(1); f.f32(0);
	f.u32(0x00010002u); f.u32(0); f.u32(0);
	return f.b;
}

// A skinned model: one global batch, one list over a two-part palette.
inline std::vector<uint8_t> gpp_model() {
	Bytes f = header('P', 0, 1, 3, 0, 0, 0, 0);
	f.u32(1);
	texture_row(f, "skin.tga", 0);
	collision_block(f, true);
	for (int v = 0; v < 3; ++v) {
		f.f32(static_cast<float>(v)); f.f32(0); f.f32(0);
		f.f32(1); f.f32(0); f.f32(0);
		f.u8(v == 2 ? 1 : 0); f.u8(0); f.u8(0); f.u8(0);
		f.f32(0); f.f32(0); f.f32(1);
		f.u32(0xFF000000u);
		f.zeros(16);
	}
	Bytes blob;
	for (int part = 0; part < 2; ++part) {
		blob.u32(0); blob.i32(1); blob.u32(0); blob.i32(0);
		blob.zeros(12);
		blob.f32(static_cast<float>(part)); blob.f32(0); blob.f32(0);
		blob.zeros(12);
		blob.i32(kOne);
		blob.zeros(16);
	}
	blob.u32(0); blob.i32(1); blob.zeros(20);
	blob.u32(0); blob.i32(1); blob.u32(0); blob.i32(0); blob.zeros(16);
	blob.i32(0); blob.u32(0x72646441u); blob.u16(3); blob.u16(0); blob.i32(0); blob.i32(0); blob.i32(3);
	blob.u8(0); blob.u8(1); blob.zeros(14); blob.zeros(7); blob.u8(2);
	blob.u16(0); blob.u16(1); blob.u16(2);
	material(blob, 0, 0, 1, 0);
	f.u32(static_cast<uint32_t>(blob.b.size()));
	f.u32(0); f.u32(1); f.u32(0); f.u32(2); f.u32(0); f.u32(0); f.u32(0);
	f.zeros(0x50 - 0x20);
	f.u32(gp::kRenderGlobalBatch);
	f.zeros(0x88 - 0x54);
	f.append(blob);
	return f.b;
}

} // namespace gp_test
