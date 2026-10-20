// The model's table (editor/documents/model_table.cpp, ADR 0046 S10; rows of the one table shape since
// S13 D10, from the format's property table it was) against the reader and the writer: a Set of every
// field of every record to the value it reads changes no byte (every model of the format's fixtures,
// the synthetic set and the forms of the shipped models the threedi rows mint, model_variant_files.h;
// and a model holding the values at the edges of what each field holds, as shipped models hold them,
// each name one past its width refused); a value read back is the value set; a light's position
// re-derives its view_proj and its colour does not; a generator's style keeps its parameter byte; the
// refusals; a group's members sit together, named apart; and the shape's parts as the model fills them
// (each kind's lists, a part animation's tracks by their flags).
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/documents/model_document.h>
#include <editor/project/project_document.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/model_variant_files.h"

using namespace opennova::editor;
using namespace opennova::threedi;
namespace fs = std::filesystem;

namespace {

const RecordTable &T() { return model_table(); }
constexpr NodeKind k(ModelKind kind) { return node_kind(kind); }

bool get(const RecordHandle &record, const std::string &id, Value &out) {
	const TableKind &kind = *T().kind(record.kind);
	const size_t place = kind.find(id);
	return place != TableKind::npos && kind.value(place).get(record, out);
}
bool set(const RecordHandle &record, const std::string &id, const Value &value, std::string &error) {
	const TableKind &kind = *T().kind(record.kind);
	const size_t place = kind.find(id);
	if (place == TableKind::npos) {
		error = "Unknown field.";
		return false;
	}
	return kind.value(place).set(record, value, error);
}
bool reads(const RecordHandle &record, const std::string &id) {
	const TableKind &kind = *T().kind(record.kind);
	const size_t place = kind.find(id);
	return place != TableKind::npos &&
	       (!kind.applies(place) || kind.applies(place)(record, RecordOwners{}) == Applicability::Reads);
}
ReferenceKind names(const RecordHandle &record, const std::string &id) {
	const TableKind &kind = *T().kind(record.kind);
	const size_t place = kind.find(id);
	if (place == TableKind::npos) return ReferenceKind::None;
	return kind.reference(place) ? kind.reference(place)(record, RecordOwners{}) : kind.fields()[place].reference;
}

// Every record a row holds, the row's own first, each list's records with what they hold before the
// next (the table's lists alone).
void each_record(const RecordHandle &record, const std::function<void(const RecordHandle &)> &fn) {
	fn(record);
	for (const TableList &list : T().kind(record.kind)->lists())
		for (size_t i = 0; i < list.ops.size(record); ++i) each_record(list.ops.at(record, i), fn);
}

// The model's two rows, copies of a document's that the test edits through the table, written as the
// document writes its own (compose_model).
struct Rows {
	std::shared_ptr<ModelRow> model;
	std::shared_ptr<CollisionRow> collision;
	std::vector<uint8_t> write() const {
		ComposedModel composed;
		compose_model(*model, collision.get(), composed);
		std::vector<uint8_t> out;
		threedi_3di3_write_memory(&composed.model, out);
		return out;
	}
};

bool open(const std::vector<uint8_t> &bytes, const std::string &name, ModelDocument &document, Rows &rows) {
	Diagnostic error;
	if (!document.load_bytes(bytes, name, opennova::editor::AssetKind::Model, "JO", error)) {
		std::fprintf(stderr, "%s: does not open: %s\n", name.c_str(), error.message.c_str());
		return false;
	}
	rows.model = std::static_pointer_cast<ModelRow>(document.model_row()->clone());
	rows.collision = std::static_pointer_cast<CollisionRow>(document.collision_row()->clone());
	return true;
}

// Set every writable field of every record to the value it reads; the written model must be the same
// bytes. Returns the number of fields set, or -1.
long set_everything_to_itself(const std::vector<uint8_t> &bytes, const std::string &name) {
	ModelDocument document;
	Rows rows;
	if (!open(bytes, name, document, rows)) return -1;
	const std::vector<uint8_t> before = rows.write();
	long count = 0;
	bool ok = true;
	const auto each = [&](const RecordHandle &record) {
		for (const FieldSchema &field : T().kind(record.kind)->fields()) {
			Value value;
			if (!get(record, field.id, value)) {
				std::fprintf(stderr, "%s: %s does not read\n", name.c_str(), field.id.c_str());
				ok = false;
				continue;
			}
			if (field.read_only) continue;
			std::string error;
			if (!set(record, field.id, value, error)) {
				std::fprintf(stderr, "%s: %s refuses its own value: %s\n", name.c_str(), field.id.c_str(), error.c_str());
				ok = false;
			}
			++count;
		}
	};
	each_record(rows.model->record(), each);
	each_record(rows.collision->record(), each);
	if (!ok) return -1;
	if (rows.write() != before) {
		std::fprintf(stderr, "%s: a Set of every field to itself changed the bytes\n", name.c_str());
		return -1;
	}
	return count;
}

// Every model of the format's fixtures (the synthetic set and the forms of the shipped models the threedi rows
// mint, each of which must be there), set to itself through the table.
int test_fixture_models() {
	const model_variants::Files models = model_variants::models();
	int count = 0;
	long fields = 0;
	for (const std::string &path : models.paths) {
		const long n = set_everything_to_itself(model_variants::bytes_of(path), fs::path(path).filename().string());
		TEST_EXPECT(n > 0);
		++count;
		fields += n;
	}
	TEST_EXPECT(count > 20);
	std::printf("fixtures: %d models, %ld fields set to themselves, bytes unchanged\n", count, fields);
	return model_variants::report_missing(models) == 0 ? 0 : 1;
}

std::string synth(const char *name) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth/" + name;
}

// A Set to the value a field reads changes nothing, so what a shipped model's values ask of the table is that
// each reads and each is one the field takes: within its range and its width. The armory, holding the values
// at those edges as the shipped models hold them (the GHDR name's 15 characters, a LOD type's 4, a shader's
// 32, a texture file's 16 (retail's bo105blur.dds.tg), a user point's 15 and a register's 24; a flipbook on
// the register clock, the top of its clock's range (ArmGlove's); an MTRX row of NaN words (US01's row 1, a
// quiet NaN of each sign) and a light's axis word of -0.0 (CmpFire1's); texture rows flagged 1, 2 and 3 on
// frames (ArmGlove's, IJava05's, JntOpsB1's); a volume's flag bits above the letters (Armry02's); a user point
// on no part (Bed01's); a face's flag word 0x501 (Comtowr1's) and a surface past the game's 24 (Black Hawk
// Down's FSUN, migrated: 60)), set to itself, changes no byte; each name one past its width is refused.
int test_edges() {
	const std::vector<uint8_t> source = test_io::read_file(synth("armory.3di"));
	Threedi3di3 model{};
	TEST_EXPECT(threedi_3di3_read_memory(source.data(), source.size(), &model) == 0);
	TEST_EXPECT(model.lod_count > 0 && model.material_count > 0 && model.light_count > 0 && model.user_point_count > 0 &&
	            model.ctrl.count > 0 && model.collision && model.collision->face_count > 0 &&
	            model.collision->volume_count > 0);
	// A shallow copy over tables of its own; the model's own freed untouched.
	Threedi3di3 copy = model;
	std::vector<ThreediLod> lods(model.lods, model.lods + model.lod_count);
	std::vector<ThreediMaterial> materials(model.materials, model.materials + model.material_count);
	std::vector<ThreediLight> lights(model.lights, model.lights + model.light_count);
	std::vector<ThreediUserPoint> points(model.user_points, model.user_points + model.user_point_count);
	std::vector<ThreediControlRegister> registers(model.ctrl.registers, model.ctrl.registers + model.ctrl.count);
	std::vector<ThreediMatrix4x4> frames(model.mtrx.matrices, model.mtrx.matrices + model.mtrx.count);
	ThreediCollisionModel collision = *model.collision;
	std::vector<ThreediBoundingVolume> volumes(collision.volumes, collision.volumes + collision.volume_count);
	std::vector<ThreediCollisionFace> faces(collision.faces, collision.faces + collision.face_count);
	const auto fill = [](char *field, size_t length) {
		for (size_t i = 0; i < length; ++i) field[i] = char('A' + i % 26);
		field[length] = '\0';
	};
	fill(copy.header.name, 15);
	fill(lods[0].model_type, 4);
	fill(materials[0].shader_name, 32);
	std::memcpy(materials[0].textures[0].name, "bo105blur.dds.tg", 17);
	materials[0].animation = ThreediTexAnim{8, 1, SHRT_MAX};
	for (uint32_t t = 0; t < materials[0].texture_count && t < 3; ++t) {
		materials[0].textures[t].flags = uint8_t(t + 1);
		materials[0].textures[t].frame = uint8_t(t);
	}
	fill(points[0].name, 15);
	points[0].subobject_index = -1;
	fill(registers[0].name, 24);
	ThreediMatrix4x4 nan_row{};
	threedi_mat4_identity(&nan_row);
	const uint32_t quiet[2] = {0xFFC00000u, 0x7FC00000u};
	for (int k = 0; k < 15; ++k)
		if (k % 4 != 3) std::memcpy(&nan_row.m[k], &quiet[k % 2], sizeof(float));
	frames.push_back(nan_row);
	lights[0].rotation[1] = -0.0f;
	volumes[0].flags |= 0x100;
	faces[0].material_flags = 0x501;
	faces[0].poly_type = 60;
	copy.lods = lods.data();
	copy.materials = materials.data();
	copy.lights = lights.data();
	copy.user_points = points.data();
	copy.ctrl.registers = registers.data();
	copy.mtrx.matrices = frames.data();
	copy.mtrx.count = uint32_t(frames.size());
	collision.volumes = volumes.data();
	collision.faces = faces.data();
	copy.collision = &collision;
	std::vector<uint8_t> bytes;
	TEST_EXPECT(threedi_3di3_write_memory(&copy, bytes) == 0);
	threedi_3di3_free(&model);
	const long n = set_everything_to_itself(bytes, "edges.3di");
	TEST_EXPECT(n > 0);

	// One past each width refused.
	ModelDocument document;
	Rows rows;
	TEST_EXPECT(open(bytes, "edges.3di", document, rows));
	ModelRow &m = *rows.model;
	TEST_EXPECT(m.frames.size() == frames.size() && std::isnan(m.frames.back().m[0]) && std::signbit(m.frames.back().m[0]));
	std::string error;
	Value v;
	const auto past = [&](const RecordHandle &record, const char *id, size_t length) {
		return get(record, id, v) && std::get<std::string>(v).size() == length &&
		       !set(record, id, std::string(length + 1, 'Z'), error) && set(record, id, std::string(length, 'Z'), error);
	};
	TEST_EXPECT(past(m.record(), "name", 15));
	TEST_EXPECT(past({k(ModelKind::Lod), &m.lods[0]}, "type", 4));
	TEST_EXPECT(past({k(ModelKind::Material), &m.materials[0]}, "shader", 32));
	TEST_EXPECT(past({k(ModelKind::Texture), &m.materials[0].material.textures[0]}, "name", 16));
	TEST_EXPECT(past({k(ModelKind::UserPoint), &m.user_points[0]}, "name", 15));
	TEST_EXPECT(past({k(ModelKind::Register), &m.registers[0]}, "name", 24));
	TEST_EXPECT(!set({k(ModelKind::Material), &m.materials[0]}, "texanim.type", int64_t(2), error));
	std::printf("edges: %ld fields at the edges the shipped models reach set to themselves; each name one past its "
	            "width refused\n",
	            n);
	return 0;
}

int test_edits() {
	ModelDocument document;
	Rows rows;
	TEST_EXPECT(open(test_io::read_file(synth("armory.3di")), "armory.3di", document, rows));
	ModelRow &m = *rows.model;
	TEST_EXPECT(!m.lights.empty() && !m.materials.empty() && !m.user_points.empty());
	std::string error;
	Value v;

	// The spot fields apply to a spot light only; a cone derives the cosine.
	ThreediLight &light = m.lights[0];
	const RecordHandle lr{k(ModelKind::Light), &light};
	light.flags = 0;
	TEST_EXPECT(!reads(lr, "cone") && !reads(lr, "direction.z"));
	TEST_EXPECT(set(lr, "flags", int64_t(THREEDI_LIGHT_FLAG_TYPE_TARGET), error));
	TEST_EXPECT(reads(lr, "cone"));
	TEST_EXPECT(set(lr, "cone", int64_t(30), error));
	TEST_EXPECT(light.falloff_byte == 30 && std::fabs(light.rotation[3] - std::cos(30.0 * 3.14159265358979 / 180.0)) < 1e-5);
	// Point the spot forward (mission x): straight down, the view's basis is degenerate and a forward
	// move would not show in it.
	TEST_EXPECT(set(lr, "direction.x", 1.0, error));
	TEST_EXPECT(set(lr, "direction.z", 0.0, error));
	TEST_EXPECT(get(lr, "direction.x", v) && std::get<double>(v) == 1.0);

	// A light's colour leaves its view_proj alone; its position derives it again (a spot light: an
	// omni light's view_proj is NaN columns wherever it sits).
	float view_proj[16];
	std::memcpy(view_proj, light.view_proj, sizeof(view_proj));
	TEST_EXPECT(set(lr, "start.r", int64_t(200), error));
	TEST_EXPECT(light.color_start[2] == 200);
	TEST_EXPECT(std::memcmp(view_proj, light.view_proj, sizeof(view_proj)) == 0);
	TEST_EXPECT(get(lr, "position.x", v));
	const double x = std::get<double>(v) + 1.0;
	TEST_EXPECT(set(lr, "position.x", x, error));
	TEST_EXPECT(get(lr, "position.x", v));
	TEST_EXPECT(std::fabs(std::get<double>(v) - x) < 1e-5);
	TEST_EXPECT(std::memcmp(view_proj, light.view_proj, sizeof(view_proj)) != 0);
	TEST_EXPECT(names(lr, "param") == ReferenceKind::None);
	light.style = 113;
	TEST_EXPECT(names(lr, "param") == ReferenceKind::ModelRegister);

	// A user point reads back the 16.16 value set, truncated as the exporter stores it.
	const RecordHandle ur{k(ModelKind::UserPoint), &m.user_points[0]};
	TEST_EXPECT(set(ur, "position.y", 1.5, error));
	TEST_EXPECT(get(ur, "position.y", v) && std::get<double>(v) == 1.5);
	TEST_EXPECT(m.user_points[0].y == 98304);
	TEST_EXPECT(set(ur, "name", std::string("muzzle"), error));
	TEST_EXPECT(std::strcmp(m.user_points[0].name, "muzzle") == 0);
	TEST_EXPECT(!set(ur, "name", std::string("a name far too long"), error));
	TEST_EXPECT(!set(ur, "name", std::string("quote\"d"), error));

	// A generator's style keeps its parameter byte: a phase of 0x40 read as a register 64 above 0x70,
	// and back.
	ModelMaterial &held = m.materials[0];
	ThreediMaterial &mat = held.material;
	const RecordHandle mr{k(ModelKind::Material), &held};
	TEST_EXPECT(set(mr, "rgbgen.style", int64_t(50), error));
	TEST_EXPECT(set(mr, "rgbgen.param", int64_t(64), error));
	TEST_EXPECT(mat.rgb_gen.phase == 0.25f && mat.rgb_gen.reg == -1);
	TEST_EXPECT(set(mr, "rgbgen.style", int64_t(113), error));
	TEST_EXPECT(mat.rgb_gen.reg == 64 && mat.rgb_gen.phase == 0.0f);
	TEST_EXPECT(names(mr, "rgbgen.param") == ReferenceKind::ModelRegister);
	TEST_EXPECT(set(mr, "rgbgen.style", int64_t(50), error));
	TEST_EXPECT(mat.rgb_gen.phase == 0.25f);
	TEST_EXPECT(set(mr, "rgbgen.start.g", int64_t(128), error));
	TEST_EXPECT(get(mr, "rgbgen.start.g", v) && std::get<int64_t>(v) == 128);
	// A flipbook's time names a register only when the flipbook reads one: frames on the register
	// clock.
	const ThreediTexAnim animation = mat.animation;
	mat.animation.num_frames = 0;
	mat.animation.animation_type = 1;
	TEST_EXPECT(names(mr, "texanim.time") == ReferenceKind::None);
	mat.animation.num_frames = 4;
	TEST_EXPECT(names(mr, "texanim.time") == ReferenceKind::ModelRegister);
	mat.animation.animation_type = 0;
	TEST_EXPECT(names(mr, "texanim.time") == ReferenceKind::None);
	mat.animation = animation;

	// The refusals: read-only, out of range, the wrong type, an unknown field.
	TEST_EXPECT(!set(mr, "glass", int64_t(1), error));
	TEST_EXPECT(!set(mr, "alpha_test", int64_t(256), error));
	TEST_EXPECT(!set(mr, "alpha_test", std::string("x"), error));
	TEST_EXPECT(!set(mr, "no_such_field", int64_t(0), error));
	TEST_EXPECT(!set(mr, "shader", std::string(40, 'A'), error));
	// A texture name is what build takes: the whole 16-byte field (retail's bo105blur.dds.tg), printable
	// ASCII, a file name alone.
	TEST_EXPECT(mat.texture_count > 0);
	const RecordHandle tr{k(ModelKind::Texture), &mat.textures[0]};
	TEST_EXPECT(set(tr, "name", std::string("bo105blur.dds.tg"), error));
	TEST_EXPECT(!set(tr, "name", std::string("tex/skin.tga"), error));
	TEST_EXPECT(!set(tr, "name", std::string("sk\xC3\xADn.tga"), error));

	// The edited model writes and reads back with the edits.
	const std::vector<uint8_t> written = rows.write();
	Threedi3di3 back{};
	TEST_EXPECT(threedi_3di3_read_memory(written.data(), written.size(), &back) == 0);
	TEST_EXPECT(back.user_points[0].y == 98304 && std::strcmp(back.user_points[0].name, "muzzle") == 0);
	TEST_EXPECT(back.lights[0].color_start[2] == 200 && back.lights[0].falloff_byte == 30);
	threedi_3di3_free(&back);
	std::printf("edits: derived words follow their fields, generator bytes kept, refusals hold\n");
	return 0;
}

// The model's lists through the table: a material's texture rows in its fixed table of 24 (a new one a
// diffuse row, the 25th refused, a row out leaving the rest zero), a register's list, the LODs, the
// collision records.
int test_lists() {
	ModelDocument document;
	Rows rows;
	TEST_EXPECT(open(test_io::read_file(synth("armory.3di")), "armory.3di", document, rows));
	const TableKind &material = *T().kind(k(ModelKind::Material));
	TEST_EXPECT(material.lists().size() == 1 && material.lists()[0].spec.kind == k(ModelKind::Texture) &&
	            material.lists()[0].spec.max == 24);
	const RecordHandle owner{k(ModelKind::Material), &rows.model->materials[0]};
	const ListOps &textures = material.lists()[0].ops;
	ThreediMaterial &mat = rows.model->materials[0].material;
	const size_t had = textures.size(owner);
	std::string error;
	while (textures.size(owner) < 24) TEST_EXPECT(textures.insert(owner, textures.size(owner), nullptr, error));
	TEST_EXPECT(mat.texture_count == 24 && mat.textures[23].slot == THREEDI_TEX_SLOT_DIFFUSE);
	TEST_EXPECT(!textures.insert(owner, 0, nullptr, error) && error == "A material holds 24 texture rows.");
	while (textures.size(owner) > had) TEST_EXPECT(textures.erase(owner, textures.size(owner) - 1));
	TEST_EXPECT(mat.texture_count == had);
	for (size_t i = had; i < 24; ++i) TEST_EXPECT(mat.textures[i].name[0] == 0 && mat.textures[i].slot == 0);
	const TableKind &model = *T().kind(k(ModelKind::Model));
	TEST_EXPECT(model.lists().size() == 6 && model.lists()[kModelLods].spec.fixed &&
	            !model.lists()[kModelMaterials].spec.fixed);
	// The named places are the table's order (what the overlay and the edits index the row's ids by).
	TEST_EXPECT(model.lists()[kModelLods].spec.kind == k(ModelKind::Lod) &&
	            model.lists()[kModelMaterials].spec.kind == k(ModelKind::Material) &&
	            model.lists()[kModelLights].spec.kind == k(ModelKind::Light) &&
	            model.lists()[kModelUserPoints].spec.kind == k(ModelKind::UserPoint) &&
	            model.lists()[kModelRegisters].spec.kind == k(ModelKind::Register) &&
	            model.lists()[kModelFrames].spec.kind == k(ModelKind::Frame));
	TEST_EXPECT(T().kind(k(ModelKind::Lod))->lists()[kModelOwnList].spec.kind == k(ModelKind::PartAnimation) &&
	            T().kind(k(ModelKind::Material))->lists()[kModelOwnList].spec.kind == k(ModelKind::Texture));
	for (const TableList &list : T().kind(k(ModelKind::Collision))->lists()) TEST_EXPECT(list.spec.fixed);
	TEST_EXPECT(T().well_formed());
	std::printf("lists: 24 texture rows at most, the LODs and the collision records fixed\n");
	return 0;
}

// A group's members are neighbours, each named apart (its component: Position X, Start red, Row 2,
// column 3), a channel group three of them; a unit is apart from the name; the words no witness
// explains are read only.
int test_rows() {
	for (const RecordKindRow &row : T().kinds()) {
		const std::vector<FieldSchema> &fields = T().fields(row.kind);
		for (size_t i = 0; i < fields.size(); ++i) {
			const std::string &label = fields[i].label;
			TEST_EXPECT(label.find('(') == std::string::npos || label.find(": none") != std::string::npos ||
			            label.find("1/256") != std::string::npos || label.find("MTRX") != std::string::npos);
			TEST_EXPECT(fields[i].applies != Applicability::Unverified || fields[i].read_only);
			if (fields[i].group.empty()) continue;
			size_t end = i;
			while (end < fields.size() && fields[end].group == fields[i].group) ++end;
			// Siblings: one path up to the component.
			const std::string first = fields[i].id, prefix = first.substr(0, first.rfind('.') + 1);
			for (size_t a = i; a < end; ++a) {
				TEST_EXPECT(fields[a].id.compare(0, prefix.size(), prefix) == 0);
				for (size_t b = a + 1; b < end; ++b) TEST_EXPECT(fields[a].label != fields[b].label);
			}
			if (fields[i].color == FieldColor::Channel) TEST_EXPECT(end - i == 3);
			i = end - 1;
		}
	}
	std::printf("rows: a group's members neighbours and named apart, units apart from names\n");
	return 0;
}

int test_panm() {
	ModelDocument document;
	Rows rows;
	TEST_EXPECT(open(test_io::read_file(synth("house_lod0_sine_rotx.3di")), "house_lod0_sine_rotx.3di", document, rows));
	TEST_EXPECT(!rows.model->lods.empty() && !rows.model->lods[0].panm.empty());
	ThreediPartAnimation *row = nullptr;
	for (ThreediPartAnimation &pa : rows.model->lods[0].panm)
		if (pa.rotation_x.control != 0) row = &pa;
	TEST_EXPECT(row != nullptr);
	const RecordHandle r{k(ModelKind::PartAnimation), row};
	std::string error;
	TEST_EXPECT(reads(r, "rotx.style") && !reads(r, "trans.style"));
	TEST_EXPECT(set(r, "rotx.rate", int64_t(-300), error) && row->rotation_x.rate == -300);
	TEST_EXPECT(set(r, "flags.trans_axis", int64_t(2), error));
	TEST_EXPECT(threedi_panm_translate_type(row->flags) == 2 && reads(r, "trans.end"));
	TEST_EXPECT(!set(r, "rotx.start", int64_t(40000), error));
	std::printf("panm: tracks apply by their flags, words and flag bytes set\n");
	return 0;
}

} // namespace

int main() {
	if (test_edits() != 0) return 1;
	if (test_lists() != 0) return 1;
	if (test_panm() != 0) return 1;
	if (test_rows() != 0) return 1;
	if (test_edges() != 0) return 1;
	// The sweep over the format's fixtures last: it names every variant it lacks.
	return test_fixture_models();
}
