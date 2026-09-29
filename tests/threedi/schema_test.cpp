// The .3di property table (formats/threedi/threedi_schema.h) against the reader
// and the writer: a Set of every field of every record to the value it reads
// changes no byte (every synthetic model, and every retail model at the
// OPENNOVA_JO_ASSETS root as a SKIP-LEG leg); a value read back is the value set; a
// light's position re-derives its view_proj and its colour does not; a generator's
// style keeps its parameter byte; the refusals; a group's members sit together, named apart.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_schema.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova::threedi;

namespace {

using Shape = ThreediSchemaShape;

// Every record of the table a model holds.
void for_each_record(Threedi3di3 &m, const std::function<void(const ThreediSchemaRecord &)> &fn) {
	fn({Shape::Model, &m.header});
	for (size_t l = 0; l < m.lod_count; ++l) {
		fn({Shape::Lod, &m.lods[l]});
		for (size_t p = 0; p < m.lods[l].part_animation_count; ++p) fn({Shape::PartAnimation, &m.lods[l].part_animations[p]});
	}
	for (uint32_t i = 0; i < m.material_count; ++i) {
		fn({Shape::Material, &m.materials[i]});
		for (uint32_t t = 0; t < m.materials[i].texture_count && t < 24; ++t)
			fn({Shape::Texture, &m.materials[i].textures[t]});
	}
	for (size_t i = 0; i < m.light_count; ++i) fn({Shape::Light, &m.lights[i]});
	for (size_t i = 0; i < m.user_point_count; ++i) fn({Shape::UserPoint, &m.user_points[i]});
	for (uint32_t i = 0; i < m.ctrl.count; ++i) fn({Shape::Register, &m.ctrl.registers[i]});
	for (uint32_t i = 0; i < m.mtrx.count; ++i) fn({Shape::Frame, &m.mtrx.matrices[i]});
	if (m.collision != nullptr) {
		for (size_t i = 0; i < m.collision->object_count; ++i) fn({Shape::Section, &m.collision->objects[i]});
		for (size_t i = 0; i < m.collision->volume_count; ++i) fn({Shape::Volume, &m.collision->volumes[i]});
		for (size_t i = 0; i < m.collision->face_count; ++i) fn({Shape::Face, &m.collision->faces[i]});
	}
	for (size_t i = 0; i < m.occlusion_object_count; ++i) fn({Shape::Occlusion, &m.occlusion_objects[i]});
}

// Set every writable field of every record to the value it reads; the written
// model must be the same bytes. Returns the number of fields set, or -1.
long set_everything_to_itself(const std::vector<uint8_t> &bytes, const std::string &name) {
	Threedi3di3 m{};
	if (threedi_3di3_read_memory(bytes.data(), bytes.size(), &m) != 0) {
		std::fprintf(stderr, "%s: does not read\n", name.c_str());
		return -1;
	}
	std::vector<uint8_t> before, after;
	threedi_3di3_write_memory(&m, before);
	long count = 0;
	bool ok = true;
	for_each_record(m, [&](const ThreediSchemaRecord &record) {
		for (const ThreediSchemaField &field : threedi_schema_fields(record.shape)) {
			ThreediSchemaValue value;
			if (!threedi_schema_get(record, field.path, value)) {
				std::fprintf(stderr, "%s: %s does not read\n", name.c_str(), field.path);
				ok = false;
				continue;
			}
			if (field.read_only) continue;
			std::string error;
			if (!threedi_schema_set(record, field.path, value, error)) {
				std::fprintf(stderr, "%s: %s refuses its own value: %s\n", name.c_str(), field.path, error.c_str());
				ok = false;
			}
			++count;
		}
	});
	threedi_3di3_write_memory(&m, after);
	threedi_3di3_free(&m);
	if (!ok) return -1;
	if (after != before) {
		std::fprintf(stderr, "%s: a Set of every field to itself changed the bytes\n", name.c_str());
		return -1;
	}
	return count;
}

int test_synthetic_models() {
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth";
	int models = 0;
	long fields = 0;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
		if (entry.path().extension() != ".3di") continue;
		const long n = set_everything_to_itself(test_io::read_file(entry.path().string()), entry.path().filename().string());
		TEST_EXPECT(n >= 0);
		++models;
		fields += n;
	}
	TEST_EXPECT(models > 20);
	std::printf("synthetic: %d models, %ld fields set to themselves, bytes unchanged\n", models, fields);
	return 0;
}

std::string synth(const char *name) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/" + name;
}

int test_edits() {
	const std::vector<uint8_t> bytes = test_io::read_file(synth("armory.3di"));
	Threedi3di3 m{};
	TEST_EXPECT(threedi_3di3_read_memory(bytes.data(), bytes.size(), &m) == 0);
	TEST_EXPECT(m.light_count > 0 && m.material_count > 0 && m.user_point_count > 0);
	std::string error;
	ThreediSchemaValue v;

	// The spot fields apply to a spot light only; a cone derives the cosine.
	ThreediLight &light = m.lights[0];
	const ThreediSchemaRecord lr{Shape::Light, &light};
	light.flags = 0;
	TEST_EXPECT(!threedi_schema_reads(lr, "cone") && !threedi_schema_reads(lr, "direction.z"));
	TEST_EXPECT(threedi_schema_set(lr, "flags", int64_t(THREEDI_LIGHT_FLAG_TYPE_TARGET), error));
	TEST_EXPECT(threedi_schema_reads(lr, "cone"));
	TEST_EXPECT(threedi_schema_set(lr, "cone", int64_t(30), error));
	TEST_EXPECT(light.falloff_byte == 30 && std::fabs(light.rotation[3] - std::cos(30.0 * 3.14159265358979 / 180.0)) < 1e-5);
	// Point the spot forward (mission x): straight down, the view's basis is
	// degenerate and a forward move would not show in it.
	TEST_EXPECT(threedi_schema_set(lr, "direction.x", 1.0, error));
	TEST_EXPECT(threedi_schema_set(lr, "direction.z", 0.0, error));
	TEST_EXPECT(threedi_schema_get(lr, "direction.x", v) && std::get<double>(v) == 1.0);

	// A light's colour leaves its view_proj alone; its position derives it again
	// (a spot light: an omni light's view_proj is NaN columns wherever it sits).
	float view_proj[16];
	std::memcpy(view_proj, light.view_proj, sizeof(view_proj));
	TEST_EXPECT(threedi_schema_set(lr, "start.r", int64_t(200), error));
	TEST_EXPECT(light.color_start[2] == 200);
	TEST_EXPECT(std::memcmp(view_proj, light.view_proj, sizeof(view_proj)) == 0);
	TEST_EXPECT(threedi_schema_get(lr, "position.x", v));
	const double x = std::get<double>(v) + 1.0;
	TEST_EXPECT(threedi_schema_set(lr, "position.x", x, error));
	TEST_EXPECT(threedi_schema_get(lr, "position.x", v));
	TEST_EXPECT(std::fabs(std::get<double>(v) - x) < 1e-5);
	TEST_EXPECT(std::memcmp(view_proj, light.view_proj, sizeof(view_proj)) != 0);
	TEST_EXPECT(threedi_schema_reference(lr, "param") == ThreediSchemaReference::None);
	light.style = 113;
	TEST_EXPECT(threedi_schema_reference(lr, "param") == ThreediSchemaReference::Register);

	// A user point reads back the 16.16 value set, truncated as the exporter stores it.
	const ThreediSchemaRecord ur{Shape::UserPoint, &m.user_points[0]};
	TEST_EXPECT(threedi_schema_set(ur, "position.y", 1.5, error));
	TEST_EXPECT(threedi_schema_get(ur, "position.y", v) && std::get<double>(v) == 1.5);
	TEST_EXPECT(m.user_points[0].y == 98304);
	TEST_EXPECT(threedi_schema_set(ur, "name", std::string("muzzle"), error));
	TEST_EXPECT(std::strcmp(m.user_points[0].name, "muzzle") == 0);
	TEST_EXPECT(!threedi_schema_set(ur, "name", std::string("a name far too long"), error));
	TEST_EXPECT(!threedi_schema_set(ur, "name", std::string("quote\"d"), error));

	// A generator's style keeps its parameter byte: a phase of 0x40 read as a
	// register 64 above 0x70, and back.
	ThreediMaterial &mat = m.materials[0];
	const ThreediSchemaRecord mr{Shape::Material, &mat};
	TEST_EXPECT(threedi_schema_set(mr, "rgbgen.style", int64_t(50), error));
	TEST_EXPECT(threedi_schema_set(mr, "rgbgen.param", int64_t(64), error));
	TEST_EXPECT(mat.rgb_gen.phase == 0.25f && mat.rgb_gen.reg == -1);
	TEST_EXPECT(threedi_schema_set(mr, "rgbgen.style", int64_t(113), error));
	TEST_EXPECT(mat.rgb_gen.reg == 64 && mat.rgb_gen.phase == 0.0f);
	TEST_EXPECT(threedi_schema_reference(mr, "rgbgen.param") == ThreediSchemaReference::Register);
	TEST_EXPECT(threedi_schema_set(mr, "rgbgen.style", int64_t(50), error));
	TEST_EXPECT(mat.rgb_gen.phase == 0.25f);
	TEST_EXPECT(threedi_schema_set(mr, "rgbgen.start.g", int64_t(128), error));
	TEST_EXPECT(threedi_schema_get(mr, "rgbgen.start.g", v) && std::get<int64_t>(v) == 128);
	// A flipbook's time names a register only when the flipbook reads one: frames on
	// the register clock.
	const ThreediTexAnim animation = mat.animation;
	mat.animation.num_frames = 0;
	mat.animation.animation_type = 1;
	TEST_EXPECT(threedi_schema_reference(mr, "texanim.time") == ThreediSchemaReference::None);
	mat.animation.num_frames = 4;
	TEST_EXPECT(threedi_schema_reference(mr, "texanim.time") == ThreediSchemaReference::Register);
	mat.animation.animation_type = 0;
	TEST_EXPECT(threedi_schema_reference(mr, "texanim.time") == ThreediSchemaReference::None);
	mat.animation = animation;

	// The refusals: read-only, out of range, the wrong type, an unknown field.
	TEST_EXPECT(!threedi_schema_set(mr, "glass", int64_t(1), error));
	TEST_EXPECT(!threedi_schema_set(mr, "alpha_test", int64_t(256), error));
	TEST_EXPECT(!threedi_schema_set(mr, "alpha_test", std::string("x"), error));
	TEST_EXPECT(!threedi_schema_set(mr, "no_such_field", int64_t(0), error));
	TEST_EXPECT(!threedi_schema_set(mr, "shader", std::string(40, 'A'), error));
	// A texture name is what build takes: the whole 16-byte field (retail's
	// bo105blur.dds.tg), printable ASCII, a file name alone.
	TEST_EXPECT(mat.texture_count > 0);
	const ThreediSchemaRecord tr{Shape::Texture, &mat.textures[0]};
	TEST_EXPECT(threedi_schema_set(tr, "name", std::string("bo105blur.dds.tg"), error));
	TEST_EXPECT(!threedi_schema_set(tr, "name", std::string("tex/skin.tga"), error));
	TEST_EXPECT(!threedi_schema_set(tr, "name", std::string("sk\xC3\xADn.tga"), error));

	// The edited model writes and reads back with the edits.
	std::vector<uint8_t> written;
	TEST_EXPECT(threedi_3di3_write_memory(&m, written) == 0);
	Threedi3di3 back{};
	TEST_EXPECT(threedi_3di3_read_memory(written.data(), written.size(), &back) == 0);
	TEST_EXPECT(back.user_points[0].y == 98304 && std::strcmp(back.user_points[0].name, "muzzle") == 0);
	TEST_EXPECT(back.lights[0].color_start[2] == 200 && back.lights[0].falloff_byte == 30);
	threedi_3di3_free(&back);
	threedi_3di3_free(&m);
	std::printf("edits: derived words follow their fields, generator bytes kept, refusals hold\n");
	return 0;
}

// A group's members are neighbours, each named apart (its component: Position X, Start
// red, Row 2, column 3), a channel group three of them; a unit is apart from the name; the
// words no witness explains are read only.
int test_rows() {
	for (int s = 0; s <= static_cast<int>(Shape::Occlusion); ++s) {
		const std::vector<ThreediSchemaField> &fields = threedi_schema_fields(static_cast<Shape>(s));
		for (size_t i = 0; i < fields.size(); ++i) {
			TEST_EXPECT(std::strchr(fields[i].label, '(') == nullptr || std::strstr(fields[i].label, ": none") != nullptr ||
			            std::strstr(fields[i].label, "1/256") != nullptr || std::strstr(fields[i].label, "MTRX") != nullptr);
			TEST_EXPECT(!fields[i].unverified || fields[i].read_only);
			if (!*fields[i].group) continue;
			size_t end = i;
			while (end < fields.size() && std::strcmp(fields[end].group, fields[i].group) == 0) ++end;
			// Siblings: one path up to the component.
			const std::string first = fields[i].path, prefix = first.substr(0, first.rfind('.') + 1);
			for (size_t a = i; a < end; ++a) {
				TEST_EXPECT(std::string(fields[a].path).compare(0, prefix.size(), prefix) == 0);
				for (size_t b = a + 1; b < end; ++b) TEST_EXPECT(std::strcmp(fields[a].label, fields[b].label) != 0);
			}
			if (fields[i].channel) TEST_EXPECT(end - i == 3);
			i = end - 1;
		}
	}
	std::printf("rows: a group's members neighbours and named apart, units apart from names\n");
	return 0;
}

int test_panm() {
	const std::vector<uint8_t> bytes = test_io::read_file(synth("house_lod0_sine_rotx.3di"));
	Threedi3di3 m{};
	TEST_EXPECT(threedi_3di3_read_memory(bytes.data(), bytes.size(), &m) == 0);
	TEST_EXPECT(m.lod_count > 0 && m.lods[0].part_animation_count > 0);
	ThreediPartAnimation *row = nullptr;
	for (size_t p = 0; p < m.lods[0].part_animation_count; ++p)
		if (m.lods[0].part_animations[p].rotation_x.control != 0) row = &m.lods[0].part_animations[p];
	TEST_EXPECT(row != nullptr);
	const ThreediSchemaRecord r{Shape::PartAnimation, row};
	std::string error;
	TEST_EXPECT(threedi_schema_reads(r, "rotx.style") && !threedi_schema_reads(r, "trans.style"));
	TEST_EXPECT(threedi_schema_set(r, "rotx.rate", int64_t(-300), error) && row->rotation_x.rate == -300);
	TEST_EXPECT(threedi_schema_set(r, "flags.trans_axis", int64_t(2), error));
	TEST_EXPECT(threedi_panm_translate_type(row->flags) == 2 && threedi_schema_reads(r, "trans.end"));
	TEST_EXPECT(!threedi_schema_set(r, "rotx.start", int64_t(40000), error));
	threedi_3di3_free(&m);
	std::printf("panm: tracks apply by their flags, words and flag bytes set\n");
	return 0;
}

int test_retail() {
	const std::string root = retail::assets();
	if (!retail::dir_exists(root)) return retail::skip_leg("OPENNOVA_JO_ASSETS (the retail models at its root)");
	int models = 0;
	long fields = 0;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
		if (!entry.is_regular_file(ec) || retail::lower_ascii(entry.path().extension().string()) != ".3di") continue;
		const long n = set_everything_to_itself(test_io::read_file(entry.path().string()), entry.path().filename().string());
		TEST_EXPECT(n > 0);
		++models;
		fields += n;
	}
	if (models == 0) return retail::skip_leg("OPENNOVA_JO_ASSETS with the retail .3di models at its root");
	std::printf("retail: %d models, %ld fields set to themselves, bytes unchanged\n", models, fields);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (test_synthetic_models() != 0) return 1;
	if (test_edits() != 0) return 1;
	if (test_panm() != 0) return 1;
	if (test_rows() != 0) return 1;
	return test_retail();
}
