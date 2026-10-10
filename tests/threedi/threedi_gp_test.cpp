// The GP reader and its migration to 3DI3 (engine/formats/threedi_gp).
//
// Synthetic legs: GP models built byte by byte here (no GP fixture is committed: nothing of
// ours writes GP, so none can be minted), in the layout BHD's loader reads [orig: GP_LoadModel
// @ 0x510E10 (dfbhd)], read, migrated and read back through the 3DI3 reader; and the loader's
// gates refused. The retail leg migrates every GP model BHD ships, which the reference tree
// mirrors under fixtures/bhd/3di: each parses, migrates, reads back as 3DI3 with a
// runtime-safe collision block, and keeps its LOD, primitive and section counts.

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi_gp/threedi_gp.h>
#include <formats/threedi_gp/threedi_gp_migrate.h>
#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::threedi;
namespace gp = opennova::threedi_gp;

namespace {

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

constexpr int32_t kOne = 65536;

Bytes header(char kind, uint32_t flags, int lods, int vertices, int points, int registers, int matrices, int occlusion) {
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

void texture_row(Bytes &o, const char *name, int16_t id) {
	o.name(name, 16);
	o.zeros(16 + 4);
	o.u16(static_cast<uint16_t>(id));
	o.u16(0x0702);
	o.zeros(20);
}

// A collision block of one section: a triangle, its normal, one six-plane volume wider than
// the section's stored box, one translation.
void collision_block(Bytes &o, bool empty) {
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

void generator(Bytes &o, uint8_t style, uint8_t param, int16_t rate, int16_t start, int16_t end) {
	o.u8(style); o.u8(param);
	o.u16(static_cast<uint16_t>(rate)); o.u16(static_cast<uint16_t>(start)); o.u16(static_cast<uint16_t>(end));
}

void material(Bytes &o, uint32_t attributes, int8_t region, uint8_t blend, uint8_t rgb_style) {
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
std::vector<uint8_t> gpm_model() {
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
std::vector<uint8_t> gpp_model() {
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

bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

bool has_note(const std::vector<gp::MigrateNote> &notes, const char *fragment) {
	for (const gp::MigrateNote &n : notes)
		if (n.text.find(fragment) != std::string::npos) return true;
	return false;
}

int synthetic_gpm() {
	const std::vector<uint8_t> bytes = gpm_model();
	gp::File file;
	std::string error;
	if (!gp::parse(bytes.data(), bytes.size(), file, error)) {
		std::fprintf(stderr, "gpm parse: %s\n", error.c_str());
		return 1;
	}
	TEST_EXPECT(file.header.kind == gp::Kind::Gpm && file.header.name == "crate");
	TEST_EXPECT(file.user_points.size() == 1 && file.user_points[0].name == "eye");
	TEST_EXPECT(file.textures.size() == 2 && file.textures[1].name == "glow.tga" && file.textures[1].id == 1);
	TEST_EXPECT(file.collision.sections.size() == 1 && file.collision.planes.size() == 6);
	TEST_EXPECT(file.lods.size() == 1 && file.lods[0].primitives.size() == 2);
	TEST_EXPECT(file.lods[0].primitives[1].alpha && file.lods[0].primitives[1].part == 1);
	TEST_EXPECT(file.lods[0].materials[1].rgb.style == 113 && file.lods[0].materials[1].blend == 4);
	TEST_EXPECT(file.lods[0].part_animations.size() == 2 && file.lods[0].part_animations[1].flags == 5);
	TEST_EXPECT(file.control_registers.size() == 1 && file.control_registers[0] == "HELO_ROTOR");
	TEST_EXPECT(file.lights.size() == 1 && file.lights[0].style == 24);
	TEST_EXPECT(file.vertex_stream.size() == 4 * 24);
	TEST_EXPECT(file.occlusion.size() == 1 && file.occlusion[0].vertices.size() == 3);

	std::vector<uint8_t> out;
	std::vector<gp::MigrateNote> notes;
	if (!gp::migrate(file, out, notes, error)) {
		std::fprintf(stderr, "gpm migrate: %s\n", error.c_str());
		return 1;
	}
	TEST_EXPECT(has_note(notes, "flag 0x2"));
	Threedi3di3 m{};
	TEST_EXPECT(threedi_3di3_read_memory(out.data(), out.size(), &m) == 0);
	TEST_EXPECT(std::strcmp(m.header.name, "crate") == 0 && m.header.mesh_type == THREEDI_MESH_BASIC);
	TEST_EXPECT(m.header.max_radius_fp16 == 2 * kOne && m.lod_count == 1);
	const ThreediLod &lod = m.lods[0];
	TEST_EXPECT(std::memcmp(lod.model_type, "crng", 4) == 0 && lod.lod_threshold == 200);
	TEST_EXPECT(lod.vertices.count == 4 && lod.vertices.flags == 0x15u && lod.vertices.stride == 64u);
	TEST_EXPECT(near(lod.vertices.items[1].tangent[0], 1.0f) && near(lod.vertices.items[1].bitangent[1], -1.0f));
	TEST_EXPECT(lod.strip_count == 2 && lod.indices.count == 7);
	TEST_EXPECT(lod.strips[0].num_triangles == 1 && lod.strips[0].num_vertices == 3 && lod.strips[0].is_strip == 0);
	TEST_EXPECT(lod.strips[1].index_offset == 3 && lod.strips[1].num_triangles == 2 && lod.strips[1].is_strip == 1);
	TEST_EXPECT(lod.strips[1].num_vertices == 4 && near(lod.strips[1].max[0], 1.0f));
	TEST_EXPECT(lod.render_object_count == 2);
	TEST_EXPECT(lod.render_objects[0].num_strips == 1 && lod.render_objects[0].num_alpha_strips == 0);
	TEST_EXPECT(lod.render_objects[1].num_strips == 0 && lod.render_objects[1].num_alpha_strips == 1);
	TEST_EXPECT(near(lod.render_objects[1].rel[0], 1.0f) && near(lod.render_objects[1].bounding_radius, 1.0f));
	TEST_EXPECT(lod.part_animation_count == 2);
	TEST_EXPECT(((lod.part_animations[1].flags >> 8) & 0xFF) == 2 && lod.part_animations[1].rotation_x.control == 113);
	TEST_EXPECT(lod.part_animations[1].subobject_index == 1 && lod.part_animations[1].matrix_index == 1);
	TEST_EXPECT(m.material_count == 2);
	TEST_EXPECT(std::strcmp(m.materials[0].shader_name, "FF_ST_OP") == 0);
	TEST_EXPECT(m.materials[0].material_flags == 5 && m.materials[0].alpha_test_value_byte == 128);
	TEST_EXPECT(m.materials[0].texture_count == 1 && std::strcmp(m.materials[0].textures[0].name, "box.tga") == 0);
	TEST_EXPECT(std::strcmp(m.materials[1].shader_name, "FF_ST_AD_LUM") == 0 && m.materials[1].emissive_type == 2);
	TEST_EXPECT(std::strcmp(m.materials[1].textures[0].name, "glow.tga") == 0);
	TEST_EXPECT(m.materials[1].rgb_gen.style == 113 && m.materials[1].rgb_gen.reg == 0);
	TEST_EXPECT(near(m.materials[1].rgb_gen.start_color[2], 0x10 / 255.0f));
	TEST_EXPECT(m.ctrl.count == 1 && std::strcmp(m.ctrl.registers[0].name, "HELO_ROTOR") == 0);
	TEST_EXPECT(m.mtrx.count == 2 && near(m.mtrx.matrices[1].m[0], -1.0f));
	TEST_EXPECT(m.user_point_count == 1 && m.user_points[0].x == kOne && m.user_points[0].userpoint_type == 'S');
	TEST_EXPECT(m.light_count == 1 && m.lights[0].style == 24 && m.lights[0].color_start[0] == 1);
	TEST_EXPECT(near(m.lights[0].offset[0], 0.5f) && m.lights[0].subobj_index == 1);
	const ThreediCollisionModel &c = *m.collision;
	TEST_EXPECT(c.vertex_count == 3 && near(c.vertices[1].position[0], 1.0f));
	TEST_EXPECT(c.face_count == 1 && c.faces[0].max_x_fp16 == kOne && c.faces[0].min_y_fp16 == 0);
	TEST_EXPECT(c.faces[0].material_flags == 0x2 && c.faces[0].poly_type == 12);
	TEST_EXPECT(c.object_count == 1 && c.objects[0].unk0 == 1);
	TEST_EXPECT(c.objects[0].min[0] == -kOne && c.objects[0].max[0] == 2 * kOne && c.objects[0].max[2] == kOne);
	TEST_EXPECT(c.objects[0].med[0] == kOne / 2 && c.objects[0].radius == 46341);
	TEST_EXPECT(c.translation_count == 1 && c.translations[0].translation[0] == kOne);
	TEST_EXPECT(c.plane_count == 6 && near(c.planes[0].normal[0], -16383.0f / 16384.0f) && c.planes[0].normal[1] == 0.0f);
	TEST_EXPECT(near(c.planes[0].radius, -2.0f));
	TEST_EXPECT(c.volume_count == 1 && c.volumes[0].max_x_fp16 == 2 * kOne && c.volumes[0].plane_count == 6);
	TEST_EXPECT(c.model_data.bbox_fp16[3] == kOne && near(c.model_data.radii[0], 3.0f));
	TEST_EXPECT(threedi_3di3_collision_is_runtime_safe(&c));
	TEST_EXPECT(m.occlusion_object_count == 1 && m.occlusion_objects[0].num_vertices == 3);
	TEST_EXPECT(near(m.occlusion_objects[0].position[0], 2.0f / 3.0f) && m.occlusion_objects[0].slot_priority_scale > 0.0f);
	TEST_EXPECT(m.occlusion_vertex_count == 3 && m.occlusion_face_count == 1);
	threedi_3di3_free(&m);

	// The mission region picks a material's texture row.
	std::vector<uint8_t> again;
	gp::MigrateOptions region;
	region.region = 2;
	TEST_EXPECT(gp::migrate(file, again, notes, error, region));
	TEST_EXPECT(again == out);
	return 0;
}

int synthetic_gpp() {
	const std::vector<uint8_t> bytes = gpp_model();
	gp::File file;
	std::string error;
	if (!gp::parse(bytes.data(), bytes.size(), file, error)) {
		std::fprintf(stderr, "gpp parse: %s\n", error.c_str());
		return 1;
	}
	TEST_EXPECT(file.header.kind == gp::Kind::Gpp && file.vertices[2].palette[0] == 1);
	TEST_EXPECT(file.lods[0].primitives.size() == 1 && file.lods[0].primitives[0].palette_length == 2);
	std::vector<uint8_t> out;
	std::vector<gp::MigrateNote> notes;
	TEST_EXPECT(gp::migrate(file, out, notes, error));
	Threedi3di3 m{};
	TEST_EXPECT(threedi_3di3_read_memory(out.data(), out.size(), &m) == 0);
	TEST_EXPECT(m.header.mesh_type == THREEDI_MESH_SKINNED);
	const ThreediLod &lod = m.lods[0];
	TEST_EXPECT(lod.vertices.flags == 0x41u && lod.vertices.stride == 56u && lod.vertices.items[2].bone_indices[0] == 1);
	TEST_EXPECT(lod.strip_count == 1 && lod.strips[0].num_vertices == 3 && lod.strips[0].bone_table_length == 2);
	TEST_EXPECT(lod.strips[0].bone_table[1] == 1);
	TEST_EXPECT(lod.render_objects[0].num_strips == 1 && lod.render_objects[1].num_strips == 0);
	TEST_EXPECT(std::strcmp(m.materials[0].shader_name, "VS_SKBASIC") == 0);
	threedi_3di3_free(&m);
	return 0;
}

// The loader's gates [orig: GP_LoadModel @ 0x510EA2..0x510EF9, @ 0x51127E (dfbhd)].
int rejections() {
	const std::vector<uint8_t> good = gpm_model();
	gp::File file;
	std::string error;
	auto refused = [&](std::vector<uint8_t> bytes) {
		return !gp::parse(bytes.data(), bytes.size(), file, error) && !error.empty();
	};
	std::vector<uint8_t> b = good;
	b[2] = 'X';
	TEST_EXPECT(refused(b));
	TEST_EXPECT(gp::detect(b.data(), b.size()) == gp::Kind::None);
	b = good;
	b[3] = 3;
	TEST_EXPECT(refused(b));
	b = good;
	b[4] = 0x04; // revision 0x104
	TEST_EXPECT(refused(b));
	b = good;
	b[0x1C] = 5;
	TEST_EXPECT(refused(b));
	TEST_EXPECT(refused(std::vector<uint8_t>(good.begin(), good.begin() + 0x80)));
	TEST_EXPECT(refused(std::vector<uint8_t>(good.begin(), good.end() - 4)));
	std::vector<uint8_t> skinned = gpp_model();
	const size_t first_vertex = gp::kHeaderSize + 4 + 60 + 0x88;
	skinned[first_vertex + 24] = 16; // a palette slot past 15
	TEST_EXPECT(refused(skinned));
	TEST_EXPECT(gp::detect(good.data(), good.size()) == gp::Kind::Gpm);
	return 0;
}

int retail_corpus() {
	const std::vector<std::string> files = retail::reference_fixture_files("bhd/3di", ".3di");
	if (files.empty()) return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/bhd/3di (the models BHD ships)");
	std::map<gp::Kind, int> kinds;
	std::map<std::string, int> notes_seen;
	int other = 0, failed = 0;
	for (const std::string &path : files) {
		const std::vector<uint8_t> bytes = test_io::read_file(path);
		const gp::Kind kind = gp::detect(bytes.data(), bytes.size());
		if (kind == gp::Kind::None) {
			++other;
			continue;
		}
		gp::File file;
		std::string error;
		std::vector<uint8_t> out;
		std::vector<gp::MigrateNote> notes;
		if (!gp::parse(bytes.data(), bytes.size(), file, error) || !gp::migrate(file, out, notes, error)) {
			std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
			++failed;
			continue;
		}
		Threedi3di3 m{};
		if (threedi_3di3_read_memory(out.data(), out.size(), &m) != 0) {
			std::fprintf(stderr, "%s: the 3DI3 does not read back\n", path.c_str());
			++failed;
			continue;
		}
		bool ok = m.lod_count == file.lods.size() && m.collision != nullptr &&
				m.collision->object_count == file.collision.sections.size() &&
				threedi_3di3_collision_is_runtime_safe(m.collision);
		for (size_t li = 0; ok && li < m.lod_count; ++li)
			ok = m.lods[li].render_object_count == file.lods[li].parts.size() &&
					m.lods[li].strip_count <= file.lods[li].primitives.size();
		if (!ok) {
			std::fprintf(stderr, "%s: the 3DI3 does not keep the model's counts\n", path.c_str());
			++failed;
		}
		threedi_3di3_free(&m);
		++kinds[kind];
		for (const gp::MigrateNote &n : notes) notes_seen[n.text] += n.count;
	}
	std::printf("threedi_gp: %d GPM, %d GPS, %d GPP migrated; %d not GP; %d failed\n", kinds[gp::Kind::Gpm],
			kinds[gp::Kind::Gps], kinds[gp::Kind::Gpp], other, failed);
	for (const auto &n : notes_seen) std::printf("  %6d  %s\n", n.second, n.first.c_str());
	TEST_EXPECT(failed == 0);
	TEST_EXPECT(kinds[gp::Kind::Gpm] + kinds[gp::Kind::Gps] + kinds[gp::Kind::Gpp] > 0);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (synthetic_gpm() != 0) return 1;
	if (synthetic_gpp() != 0) return 1;
	if (rejections() != 0) return 1;
	return retail_corpus();
}
