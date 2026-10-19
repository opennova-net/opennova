// The GP reader, its migration to 3DI3, and the way back: the lowering of a 3DI3 and the GP
// writer (engine/formats/threedi_gp).
//
// Synthetic legs: the GP models common/gp_model_bytes.h builds byte by byte, read, migrated and
// read back through the 3DI3 reader; the loader's gates refused; the writer a fixed point of the
// reader (read, write, read, write: the two writes agree) on both models; the synthetic GPM
// lowered back from its migration, the lowering a fixed point after one step, and a skinned
// model or a technique BHD has no form for refused. The retail leg takes every GP model BHD
// ships, which the reference tree mirrors under fixtures/bhd/3di: each parses, migrates, reads
// back as 3DI3 with a runtime-safe collision block and keeps its LOD, primitive and section
// counts; the writer is a fixed point on each; and every rigid one (GPM) lowers back to the GP
// whose migration is the first migration byte for byte.

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi_gp/threedi_gp.h>
#include <formats/threedi_gp/threedi_gp_lower.h>
#include <formats/threedi_gp/threedi_gp_migrate.h>
#include <formats/threedi_gp/threedi_gp_write.h>
#include "common/file_io.h"
#include "common/gp_model_bytes.h"
#include "common/retail_paths.h"
#include "common/temp_dir.h"
#include "common/test_expect.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace opennova;
using namespace opennova::threedi;
namespace gp = opennova::threedi_gp;

namespace {

using gp_test::gpm_model;
using gp_test::gpp_model;
using gp_test::kOne;

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

	// The mission region picks a material's texture row: material 1 names row 1 in regions 0
	// and 2 and row 0 in region 1, so region 1 takes box.tga and says the texture depends on it;
	// region 2 writes the model region 0 does.
	file.lods[0].materials[1].region_textures = {1, 0, 1};
	std::vector<uint8_t> again;
	gp::MigrateOptions region;
	region.region = 2;
	TEST_EXPECT(gp::migrate(file, again, notes, error, region));
	TEST_EXPECT(again == out && has_note(notes, "mission region"));
	region.region = 1;
	TEST_EXPECT(gp::migrate(file, again, notes, error, region));
	Threedi3di3 regional{};
	TEST_EXPECT(threedi_3di3_read_memory(again.data(), again.size(), &regional) == 0);
	TEST_EXPECT(std::strcmp(regional.materials[1].textures[0].name, "box.tga") == 0);
	threedi_3di3_free(&regional);
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
	// A palette length past the 16 matrices a strip's table holds (the +47 byte takes any value): refused,
	// never written as a 3DI3 our reader refuses; 16 still migrates.
	file.lods[0].primitives[0].palette_length = 16;
	TEST_EXPECT(gp::migrate(file, out, notes, error));
	file.lods[0].primitives[0].palette_length = 17;
	out.clear();
	TEST_EXPECT(!gp::migrate(file, out, notes, error) && error.find("more than 16") != std::string::npos);
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

// The files beside a model, listed once, found without case; a folder that is not there holds none.
int files_beside() {
	test_temp::TempDir temp("opennova_threedi_gp_beside");
	const std::filesystem::path dir = temp.path;
	std::FILE *f = std::fopen((dir / "Bump.MDT").string().c_str(), "wb");
	TEST_EXPECT(f != nullptr);
	std::fclose(f);
	const auto beside = gp::names_beside((dir / "model.3di").string());
	TEST_EXPECT(beside("bump.mdt") && beside("BUMP.MDT") && !beside("other.mdt"));
	TEST_EXPECT(!gp::names_beside((dir / "missing" / "model.3di").string())("bump.mdt"));
	return 0;
}

// Read, write, read, write: the writer puts back every word the reader takes.
bool writer_fixed_point(const std::vector<uint8_t> &bytes, std::vector<uint8_t> &written, std::string &error) {
	gp::File a, b;
	std::vector<uint8_t> again;
	if (!gp::parse(bytes.data(), bytes.size(), a, error) || !gp::write(a, written, error)) return false;
	if (!gp::parse(written.data(), written.size(), b, error) || !gp::write(b, again, error)) return false;
	if (again != written) {
		error = "the writer is not a fixed point of the reader";
		return false;
	}
	return true;
}

// The 3DI3 bytes a migration wrote, lowered back to GP and written.
bool lower_bytes(const std::vector<uint8_t> &threedi3, std::vector<uint8_t> &gp_bytes, gp::File &file,
		std::vector<gp::MigrateNote> &notes, std::string &error) {
	Threedi3di3 m{};
	if (threedi_3di3_read_memory(threedi3.data(), threedi3.size(), &m) != 0) {
		error = "the 3DI3 does not read";
		return false;
	}
	const bool ok = gp::lower(m, file, notes, error) && gp::write(file, gp_bytes, error);
	threedi_3di3_free(&m);
	return ok;
}

bool migrate_bytes(const std::vector<uint8_t> &gp_bytes, std::vector<uint8_t> &out, std::string &error) {
	gp::File file;
	std::vector<gp::MigrateNote> notes;
	return gp::parse(gp_bytes.data(), gp_bytes.size(), file, error) && gp::migrate(file, out, notes, error);
}

int synthetic_write() {
	for (const std::vector<uint8_t> &bytes : {gpm_model(), gpp_model()}) {
		std::vector<uint8_t> written;
		std::string error;
		if (!writer_fixed_point(bytes, written, error)) {
			std::fprintf(stderr, "write: %s\n", error.c_str());
			return 1;
		}
		gp::File file;
		TEST_EXPECT(gp::parse(written.data(), written.size(), file, error));
		TEST_EXPECT(written.size() == bytes.size());
		// The vertex colour the loader drops, 0xFF000000 from the tool [orig: GP_WriteModel
		// @ 0x4490EF..0x449238 (df4oed.exe)].
		const size_t first_vertex = gp::kHeaderSize + file.user_points.size() * 48 + 4 + file.textures.size() * 60 + 0x88 +
				file.collision.vertices.size() * 8 + file.collision.normals.size() * 8 + file.collision.faces.size() * 44 +
				file.collision.sections.size() * 128 + file.collision.translations.size() * 12 +
				file.collision.planes.size() * 16 + file.collision.volumes.size() * 96;
		uint32_t colour = 0;
		std::memcpy(&colour, written.data() + first_vertex + (file.header.kind == gp::Kind::Gpm ? 24 : 40), 4);
		TEST_EXPECT(colour == 0xFF000000u);
	}
	return 0;
}

// The synthetic GPM through migrate -> lower -> write: the GP it writes carries what the
// migration keeps, with each record's words, and one step makes it a fixed point (the synthetic
// model streams tangents no bump material reads, which the lowering leaves out).
int synthetic_lower() {
	std::vector<uint8_t> m1, gp1, m2, gp2, m3;
	gp::File file;
	std::vector<gp::MigrateNote> notes;
	std::string error;
	if (!migrate_bytes(gpm_model(), m1, error) || !lower_bytes(m1, gp1, file, notes, error) ||
			!migrate_bytes(gp1, m2, error)) {
		std::fprintf(stderr, "lower: %s\n", error.c_str());
		return 1;
	}
	TEST_EXPECT(file.header.kind == gp::Kind::Gpm && file.header.name == "crate" && file.header.revision == 0x103);
	TEST_EXPECT(file.header.tags[0] == 0x676E7263u && file.header.thresholds[0] == 200 * kOne);
	TEST_EXPECT(file.header.flags == (gp::kFlagLights | gp::kFlagOcclusion));
	TEST_EXPECT(file.user_points.size() == 1 && file.user_points[0].name == "eye" && file.user_points[0].type == 'S');
	TEST_EXPECT(file.textures.size() == 2 && file.textures[0].name == "box.tga" && file.textures[0].flags == 0x207);
	TEST_EXPECT(file.textures[0].width == 256 && has_note(notes, "size was not found"));
	const gp::RenderModel &rm = file.lods[0];
	TEST_EXPECT(rm.parts.size() == 2 && rm.batches.size() == 2 && rm.primitives.size() == 2);
	TEST_EXPECT(rm.batches[0].opaque_count == 1 && rm.batches[1].alpha_count == 1 && rm.primitives[1].alpha);
	TEST_EXPECT(rm.primitives[0].vertex_word == 2 && rm.primitives[1].topology == 1);
	TEST_EXPECT(rm.attach_points.size() == 1 && near(rm.attach_points[0][0], 1.0f));
	TEST_EXPECT(rm.materials.size() == 2 && rm.materials[0].attributes == 0x3 && rm.materials[0].alpha_ref == 128);
	TEST_EXPECT(rm.materials[0].alpha_source == 2 && rm.materials[0].blend == 1 && rm.materials[0].name == "box.tga");
	TEST_EXPECT(rm.materials[1].attributes == 0x140 && rm.materials[1].blend == 4 && rm.materials[1].rgb.style == 113);
	TEST_EXPECT((rm.flags & gp::kRenderPartAnimations) && rm.part_animations.size() == 2);
	TEST_EXPECT(rm.part_animations[1].flags == 5 && rm.part_animations[1].tracks[0].style == 113);
	TEST_EXPECT(file.control_registers.size() == 1 && file.matrices.size() == 2);
	TEST_EXPECT(file.lights.size() == 1 && file.occlusion.size() == 1 && file.occlusion[0].vertices.size() == 3);
	const gp::Collision &c = file.collision;
	TEST_EXPECT(c.sections.size() == 1 && c.volumes.size() == 1 && c.planes.size() == 6 && c.faces[0].flags == 0x2);
	TEST_EXPECT(c.planes[0].normal[0] == -65532); // JO's Q14 keeps two bits fewer than BHD's 16.16
	if (!lower_bytes(m2, gp2, file, notes, error) || !migrate_bytes(gp2, m3, error)) {
		std::fprintf(stderr, "lower again: %s\n", error.c_str());
		return 1;
	}
	TEST_EXPECT(gp2 == gp1 && m3 == m2);
	return 0;
}

int lower_refusals() {
	std::vector<uint8_t> skinned, out;
	gp::File file;
	std::vector<gp::MigrateNote> notes;
	std::string error;
	TEST_EXPECT(migrate_bytes(gpp_model(), skinned, error));
	TEST_EXPECT(!lower_bytes(skinned, out, file, notes, error) && error.find("skinned") != std::string::npos);
	std::vector<uint8_t> rigid;
	TEST_EXPECT(migrate_bytes(gpm_model(), rigid, error));
	Threedi3di3 m{};
	TEST_EXPECT(threedi_3di3_read_memory(rigid.data(), rigid.size(), &m) == 0);
	std::strcpy(m.materials[0].shader_name, "FFP_GLASS");
	TEST_EXPECT(!gp::lower(m, file, notes, error) && error.find("FFP_GLASS") != std::string::npos);
	threedi_3di3_free(&m);
	return 0;
}

int retail_corpus() {
	const std::vector<std::string> files = retail::reference_fixture_files("bhd/3di", ".3di");
	if (files.empty()) return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/bhd/3di (the models BHD ships)");
	std::map<gp::Kind, int> kinds;
	std::map<std::string, int> notes_seen;
	int other = 0, failed = 0, rewritten = 0, retail_bytes = 0, lowered = 0;
	std::vector<std::string> rewritten_apart;
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
		// The writer: a fixed point of the reader, and how many files it writes back byte for
		// byte (the others differ in words no loader reads, the tool's leftovers there).
		std::vector<uint8_t> written;
		if (!writer_fixed_point(bytes, written, error)) {
			std::fprintf(stderr, "%s: %s\n", path.c_str(), error.c_str());
			++failed;
			continue;
		}
		++rewritten;
		if (written == bytes) ++retail_bytes;
		else rewritten_apart.push_back(std::filesystem::path(path).filename().string());
		// A rigid model lowers back to a GP whose migration is the first one.
		if (kind != gp::Kind::Gpm) continue;
		std::vector<uint8_t> gp_bytes, again;
		gp::File lowered_file;
		std::vector<gp::MigrateNote> lower_notes;
		if (!lower_bytes(out, gp_bytes, lowered_file, lower_notes, error) || !migrate_bytes(gp_bytes, again, error)) {
			std::fprintf(stderr, "%s: lower: %s\n", path.c_str(), error.c_str());
			++failed;
			continue;
		}
		if (again != out) {
			std::fprintf(stderr, "%s: the lowered model migrates to another 3DI3\n", path.c_str());
			++failed;
			continue;
		}
		++lowered;
	}
	std::printf("threedi_gp: %d GPM, %d GPS, %d GPP migrated; %d not GP; %d failed\n", kinds[gp::Kind::Gpm],
			kinds[gp::Kind::Gps], kinds[gp::Kind::Gpp], other, failed);
	std::printf("threedi_gp: %d written back (%d byte for byte); %d GPM lowered back to their migration\n", rewritten,
			retail_bytes, lowered);
	for (const std::string &name : rewritten_apart) std::printf("  written back apart from retail: %s\n", name.c_str());
	for (const auto &n : notes_seen) std::printf("  %6d  %s\n", n.second, n.first.c_str());
	TEST_EXPECT(failed == 0);
	TEST_EXPECT(kinds[gp::Kind::Gpm] + kinds[gp::Kind::Gps] + kinds[gp::Kind::Gpp] > 0);
	TEST_EXPECT(lowered == kinds[gp::Kind::Gpm]);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (synthetic_gpm() != 0) return 1;
	if (synthetic_gpp() != 0) return 1;
	if (rejections() != 0) return 1;
	if (files_beside() != 0) return 1;
	if (synthetic_write() != 0) return 1;
	if (synthetic_lower() != 0) return 1;
	if (lower_refusals() != 0) return 1;
	return retail_corpus();
}
