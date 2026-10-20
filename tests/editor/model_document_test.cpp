// The model document (ADR 0046 S10) over the neutral core: every model of the format's fixtures
// (the synthetic set and the forms of the shipped models the threedi rows mint, model_variant_files.h)
// loads and an untouched save writes its own bytes, each shipped form validating with no error (a
// model the game loads raises none, S11h); the forms the document's rows compose back (an empty LOD and an empty
// part-animation table, a strip naming no material, a material no strip draws); field edits land in
// the written model (a material moved keeps its strips drawing with it); the structural edits and
// their refusals; undo back to the file's bytes; a save and a reload keep the edits; the validator's
// findings, a part animation's frame and a record's register read as the runtime reads them (S11h),
// and the forms the shipped models use near its errors raising none; every Black Hawk Down GP model
// of the fixtures, migrated as the import does, validated with no error. Two versions of the model
// row alike but for their user points only when every other table is the same (S13 V8).
#include <formats/threedi_gp/threedi_gp.h>
#include <formats/threedi_gp/threedi_gp_migrate.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/document_types.h>
#include <editor/documents/model_document.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document_search.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm.h>

#include "common/file_io.h"
#include "common/gp_model_bytes.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/model_variant_files.h"
#include "editor/rig_model.h"

using namespace opennova::editor;
using namespace opennova::threedi;
namespace fs = std::filesystem;

namespace {

constexpr NodeKind kMaterial = node_kind(ModelKind::Material);
constexpr NodeKind kTexture = node_kind(ModelKind::Texture);
constexpr NodeKind kLight = node_kind(ModelKind::Light);
constexpr NodeKind kUserPoint = node_kind(ModelKind::UserPoint);
constexpr NodeKind kRegister = node_kind(ModelKind::Register);
constexpr NodeKind kFrame = node_kind(ModelKind::Frame);
constexpr NodeKind kPanm = node_kind(ModelKind::PartAnimation);
constexpr NodeKind kVolume = node_kind(ModelKind::Volume);

std::string synth_dir() { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/threedi/synth"; }

using editor_test::serialized;

bool load(ModelDocument &document, const std::string &path, const std::string &name) {
	Diagnostic error;
	return document.load(path, name, AssetKind::Model, "jo", error);
}

using editor_test::op_edit;
using editor_test::set_edit;

// The model validator over one model, as a project's validation runs it over each file.
std::vector<Diagnostic> validated(const std::shared_ptr<const ModelDocument> &document) {
	return document_type(DocumentTypeId::Model)->validate_file(*document);
}

// Every model of the format's fixtures opens as its two rows and saves untouched as its own bytes (the
// reader's and the writer's round trip, composed back from the rows: compose_model). Each form of the
// shipped models the format's rows mint is a model the game loads, and validates with no error: a model
// the game loads raises none (S11h; every shipped model, 967 of them, held to it while the corpus was
// read). (The synthetic set's own variants need not load: shed_lght0_sub2's light names a part the shed
// lacks, on purpose.) A form that is not there fails the sweep.
int untouched_saves() {
	const model_variants::Files models = model_variants::models();
	int count = 0;
	for (const std::string &path : models.paths) {
		const std::string name = fs::path(path).filename().generic_string();
		auto document = std::make_shared<ModelDocument>();
		TEST_EXPECT(load(*document, model_variants::repo_path(path), name));
		TEST_EXPECT(document->rows().size() == 2 && document->model_row() && document->collision_row());
		if (!document->model_row()) continue;
		TEST_EXPECT(serialized(*document) == model_variants::bytes_of(path));
		if (models.is_required(path))
			for (const Diagnostic &d : validated(document)) {
				if (d.severity != DiagnosticSeverity::Error) continue;
				std::printf("untouched: %s %s: %s: %s\n", path.c_str(), d.record.c_str(), d.code().c_str(), d.message.c_str());
				TEST_EXPECT(d.severity != DiagnosticSeverity::Error);
			}
		++count;
	}
	TEST_EXPECT(count > 20);
	std::printf("untouched: %d models save as their own bytes, the shipped forms with no error\n", count);
	return model_variants::report_missing(models) == 0 ? 0 : 1;
}

// A model the format's writer writes from `base` (a synthetic model) as `edit` leaves it: the edit
// works on a shallow copy whose tables it may point at arrays of its own (`hold` keeps them alive),
// the base's own freed untouched.
struct Held {
	std::vector<ThreediLod> lods;
	std::vector<std::vector<ThreediTriangleStrip>> strips;
	std::vector<ThreediMaterial> materials;
	ThreediPartAnimation none[1] = {}; // an empty PANM table's (a table there with no row)
};
std::vector<uint8_t> minted(const char *base, const std::function<void(Threedi3di3 &, Held &)> &edit) {
	Threedi3di3 model{};
	std::vector<uint8_t> bytes;
	if (threedi_3di3_read((synth_dir() + "/" + base).c_str(), &model) != 0) return bytes;
	Threedi3di3 copy = model;
	Held hold;
	hold.lods.assign(model.lods, model.lods + model.lod_count);
	copy.lods = hold.lods.data();
	hold.materials.assign(model.materials, model.materials + model.material_count);
	copy.materials = hold.materials.data();
	edit(copy, hold);
	if (threedi_3di3_write_memory(&copy, bytes) != 0) bytes.clear();
	threedi_3di3_free(&model);
	return bytes;
}

// The forms of the shipped models whose rows compose back otherwise than as the file's tables hold
// them (compose_model): LODs of no part, no strip and no vertex whose PANM tables are there and empty,
// after LOD 0's rows (Flsh_1st's LODs 1 and 2, CNet01's LOD 3); strips naming material 2 and 5 of a
// one-material table (Tent1's LODs 3 and 4); materials no strip draws (fxd200D draws three strips
// over six). Each saves untouched as its own bytes; a material moved takes the strips that name it
// along while the strips naming none keep their indices; an empty table stays its LOD's own; a
// material no strip draws is a note, never an error.
int composed_forms() {
	Diagnostic error;
	// The pump, its LOD 1 emptied.
	const std::vector<uint8_t> emptied = minted("pump.3di", [](Threedi3di3 &m, Held &hold) {
		ThreediLod &lod = m.lods[m.lod_count - 1];
		lod.vertices.count = 0;
		lod.vertices.items = nullptr;
		lod.indices.count = 0;
		lod.indices.indices = nullptr;
		lod.strips = nullptr;
		lod.strip_count = 0;
		lod.render_objects = nullptr;
		lod.render_object_count = 0;
		lod.rmdl_render_object_count = 0;
		lod.part_animations = hold.none;
		lod.part_animation_count = 0;
	});
	TEST_EXPECT(!emptied.empty());
	{
		ModelDocument document;
		TEST_EXPECT(document.load_bytes(emptied, "pump.3di", AssetKind::Model, "jo", error));
		const ModelRow *row = document.model_row();
		TEST_EXPECT(row && row->lods.size() >= 2 && row->lods.back().panm.empty() &&
		            row->lods.back().lod.render_object_count == 0 && row->lods.back().lod.strip_count == 0);
		TEST_EXPECT(serialized(document) == emptied);
		Threedi3di3 back{};
		const std::vector<uint8_t> bytes = serialized(document);
		TEST_EXPECT(threedi_3di3_read_memory(bytes.data(), bytes.size(), &back) == 0 && back.lod_count >= 2 &&
		            back.lods[back.lod_count - 1].part_animations != nullptr &&
		            back.lods[back.lod_count - 1].part_animation_count == 0 && back.lods[0].part_animation_count > 0);
		threedi_3di3_free(&back);
	}
	// The pump, two of its LOD 1's strips on materials one and four past its table (Tent1's LODs 3 and 4: 2 and
	// 5 of its one), and a material no strip draws added after its own.
	int32_t first_past = -1;
	const std::vector<uint8_t> past = minted("pump.3di", [&](Threedi3di3 &m, Held &hold) {
		ThreediLod &lod = m.lods[1];
		hold.strips.emplace_back(lod.strips, lod.strips + lod.strip_count);
		std::vector<ThreediTriangleStrip> &strips = hold.strips.back();
		first_past = int32_t(m.material_count) + 1;
		strips.front().material_index = first_past;
		strips.back().material_index = int32_t(m.material_count) + 4;
		lod.strips = strips.data();
		ThreediMaterial unused = hold.materials.front();
		unused.index = int32_t(hold.materials.size());
		hold.materials.push_back(unused);
		m.materials = hold.materials.data();
		m.material_count = uint32_t(hold.materials.size());
	});
	TEST_EXPECT(!past.empty() && first_past > 0);
	{
		auto document = std::make_shared<ModelDocument>();
		TEST_EXPECT(document->load_bytes(past, "pump.3di", AssetKind::Model, "jo", error));
		TEST_EXPECT(serialized(*document) == past);
		const ModelRow *row = document->model_row();
		TEST_EXPECT(row && row->materials.size() >= 2 && row->lods.size() >= 2 && row->lods[1].lod.strip_count >= 2);
		if (!row || row->materials.size() < 2 || row->lods.size() < 2) return 1;
		const size_t materials = row->materials.size();
		// The materials no strip draws: the one added and any the strips moved off left; each a note.
		size_t undrawn = 0, unused = 0;
		for (size_t m = 0; m < materials; ++m) {
			bool drawn = false;
			for (const ModelLod &lod : row->lods)
				for (size_t s = 0; s < lod.lod.strip_count; ++s) drawn = drawn || lod.lod.strips[s].material_index == int32_t(m);
			undrawn += drawn ? 0 : 1;
		}
		for (const Diagnostic &d : validated(document)) {
			TEST_EXPECT(d.severity != DiagnosticSeverity::Error);
			unused += d.code() == "model.material_unused" && d.severity == DiagnosticSeverity::Info ? 1 : 0;
		}
		TEST_EXPECT(undrawn >= 1 && unused == undrawn);
		// The first material moved last: the strips naming it follow, the ones naming none keep their bytes.
		const NodeAddress first{row->id, kMaterial, row->ids.lists[1][0].id};
		TEST_EXPECT(document->apply(op_edit(EditOperation::Move, first, 0, materials - 1), error));
		const std::vector<uint8_t> bytes = serialized(*document);
		Threedi3di3 before{}, after{};
		TEST_EXPECT(threedi_3di3_read_memory(past.data(), past.size(), &before) == 0 &&
		            threedi_3di3_read_memory(bytes.data(), bytes.size(), &after) == 0);
		for (size_t l = 0; l < before.lod_count; ++l)
			for (size_t s = 0; s < before.lods[l].strip_count; ++s) {
				const int32_t was = before.lods[l].strips[s].material_index;
				const int32_t now = after.lods[l].strips[s].material_index;
				if (was < 0 || uint32_t(was) >= before.material_count) TEST_EXPECT(now == was);
				else TEST_EXPECT(now == (was == 0 ? int32_t(materials - 1) : was - 1));
			}
		TEST_EXPECT(after.lods[1].strips[0].material_index == first_past);
		threedi_3di3_free(&before);
		threedi_3di3_free(&after);
		document->undo();
		TEST_EXPECT(serialized(*document) == past);
	}
	std::printf("composed forms: empty LODs and tables, strips naming no material, a material none draws\n");
	return 0;
}

int edits() {
	test_temp::TempDir dir("opennova_model_document_test");
	const std::vector<uint8_t> original = test_io::read_file(synth_dir() + "/armory.3di");
	TEST_EXPECT(!original.empty() && test_io::write_file(dir.file("armory.3di"), original));
	ModelDocument document;
	TEST_EXPECT(load(document, dir.file("armory.3di"), "armory.3di"));
	const ModelRow *row = document.model_row();
	TEST_EXPECT(row && row->materials.size() >= 2 && !row->lights.empty() && !row->user_points.empty());
	const NodeId model = row->id;
	Diagnostic error;

	// A light's colour and a user point's name, one undo step each.
	const NodeAddress light{model, kLight, row->ids.lists[2][0].id};
	TEST_EXPECT(document.apply(set_edit(light, "start.r", int64_t(12)), error));
	const NodeAddress point{model, kUserPoint, row->ids.lists[3][0].id};
	TEST_EXPECT(document.apply(set_edit(point, "name", std::string("muzzle")), error));
	Value value;
	TEST_EXPECT(document.get(point, "name", value) && std::get<std::string>(value) == "muzzle");
	TEST_EXPECT(document.dirty() && document.can_undo());

	// A material moved to the end: its strips draw with it at its new index.
	row = document.model_row();
	const std::string first_shader = row->materials[0].material.shader_name;
	const NodeAddress first{model, kMaterial, row->ids.lists[1][0].id};
	TEST_EXPECT(document.apply(op_edit(EditOperation::Move, first, 0, row->materials.size() - 1), error));
	{
		Threedi3di3 before{}, after{};
		const std::vector<uint8_t> bytes = serialized(document);
		TEST_EXPECT(threedi_3di3_read_memory(original.data(), original.size(), &before) == 0);
		TEST_EXPECT(threedi_3di3_read_memory(bytes.data(), bytes.size(), &after) == 0);
		TEST_EXPECT(std::strcmp(after.materials[after.material_count - 1].shader_name, first_shader.c_str()) == 0);
		for (size_t l = 0; l < before.lod_count; ++l)
			for (size_t s = 0; s < before.lods[l].strip_count; ++s) {
				const int was = before.lods[l].strips[s].material_index;
				const int now = after.lods[l].strips[s].material_index;
				TEST_EXPECT(std::strcmp(before.materials[was].shader_name, after.materials[now].shader_name) == 0);
			}
		TEST_EXPECT(after.lights[0].color_start[2] == 12 && std::strcmp(after.user_points[0].name, "muzzle") == 0);
		threedi_3di3_free(&before);
		threedi_3di3_free(&after);
	}

	// A drawn material stays; a new one is added, given a texture row, and removed.
	row = document.model_row();
	TEST_EXPECT(!document.apply(op_edit(EditOperation::Remove, {model, kMaterial, row->ids.lists[1][0].id}), error));
	TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {model, kMaterial, 0}), error));
	const NodeId added = document.last_added();
	TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {model, kTexture, 0}, added, SIZE_MAX), error));
	const NodeAddress texture{model, kTexture, document.last_added()};
	TEST_EXPECT(document.apply(set_edit(texture, "name", std::string("newtex.tga")), error));
	TEST_EXPECT(document.get(texture, "name", value) && std::get<std::string>(value) == "newtex.tga");
	TEST_EXPECT(document.apply(op_edit(EditOperation::Remove, {model, kMaterial, added}), error));

	// Registers: added at the end, the last removed; lights and user points freely.
	row = document.model_row();
	const size_t registers = row->registers.size();
	TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {model, kRegister, 0}), error));
	TEST_EXPECT(document.model_row()->registers.size() == registers + 1);
	TEST_EXPECT(document.apply(op_edit(EditOperation::Remove, {model, kRegister, document.last_added()}), error));
	TEST_EXPECT(document.apply(op_edit(EditOperation::Duplicate, light, 0, 1), error));
	TEST_EXPECT(document.model_row()->lights.size() == row->lights.size() + 1);
	TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {model, kUserPoint, 0}), error));

	// The fixed records: the collision's are the geometry's; a row stays.
	const CollisionRow *collision = document.collision_row();
	if (!collision->volumes.empty()) {
		const NodeAddress volume{collision->id, kVolume, collision->ids.lists[1][0].id};
		TEST_EXPECT(!document.apply(op_edit(EditOperation::Remove, volume), error));
		TEST_EXPECT(document.apply(set_edit(volume, "flags", int64_t(0x4)), error));
		// A blink box's letters read as the add-on writes them: a letter is there where its bit is clear
		// (flags 0x4: every letter but S); setting the letters writes the bits back, the others kept.
		Value letters;
		TEST_EXPECT(document.get(volume, "letters", letters) && std::get<int64_t>(letters) == (0x3E & ~0x4));
		TEST_EXPECT(document.apply(set_edit(volume, "letters", int64_t(0x2 | 0x8)), error)); // V and W
		Value flags;
		TEST_EXPECT(document.get(volume, "flags", flags) && std::get<int64_t>(flags) == (0x3E & ~(0x2 | 0x8)));
	}
	TEST_EXPECT(!document.apply(op_edit(EditOperation::Remove, {collision->id, node_kind(ModelKind::Collision), 0}), error));

	// Save and reload: the edits are the file's.
	TEST_EXPECT(document.save(error));
	ModelDocument reloaded;
	TEST_EXPECT(load(reloaded, dir.file("armory.3di"), "armory.3di"));
	TEST_EXPECT(reloaded.model_row()->lights.size() == document.model_row()->lights.size());
	TEST_EXPECT(std::strcmp(reloaded.model_row()->user_points[0].name, "muzzle") == 0);
	TEST_EXPECT(serialized(reloaded) == serialized(document));

	// Undo back to the start writes the file's own bytes again.
	while (document.can_undo()) document.undo();
	TEST_EXPECT(serialized(document) == original);
	std::printf("edits: fields, moves, structure, refusals, save, reload and undo\n");
	return 0;
}

int part_animations() {
	ModelDocument document;
	TEST_EXPECT(load(document, synth_dir() + "/house_lod0_sine_rotx.3di", "house.3di"));
	const ModelRow *row = document.model_row();
	TEST_EXPECT(row && !row->lods.empty());
	const ModelLod &lod = row->lods[0];
	const NodeId lod_id = row->ids.lists[0][0].id;
	Diagnostic error;
	if (lod.panm.size() < lod.lod.render_object_count) {
		TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {row->id, kPanm, 0}, lod_id, SIZE_MAX), error));
		TEST_EXPECT(document.model_row()->lods[0].panm.size() == lod.panm.size() + 1);
	} else {
		TEST_EXPECT(!document.apply(op_edit(EditOperation::Add, {row->id, kPanm, 0}, lod_id, SIZE_MAX), error));
	}
	row = document.model_row();
	const std::vector<RecordIds> &ids = row->ids.lists[0][0].lists[0];
	TEST_EXPECT(!ids.empty());
	if (ids.size() > 1) TEST_EXPECT(!document.apply(op_edit(EditOperation::Remove, {row->id, kPanm, ids.front().id}), error));
	const NodeAddress first{row->id, kPanm, ids.front().id};
	TEST_EXPECT(document.apply(set_edit(first, "rotx.rate", int64_t(-300)), error));
	Value value;
	TEST_EXPECT(document.get(first, "rotx.rate", value) && std::get<int64_t>(value) == -300);
	std::printf("part animations: appended in part order, tracks set\n");
	return 0;
}

int validation() {
	test_temp::TempDir dir("opennova_model_validation_test");
	const std::string root = dir.file("Game");
	ProjectDocument project;
	Diagnostic created;
	TEST_EXPECT(create_project(root, "Model Game", "jo", project, created));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	ModelDocument document;
	TEST_EXPECT(load(document, synth_dir() + "/armory.3di", "armory.3di"));
	const ModelRow *row = document.model_row();
	Diagnostic error;
	// A light on a part LOD 0 does not have, and nine seats.
	const NodeAddress light{row->id, kLight, row->ids.lists[2][0].id};
	TEST_EXPECT(document.apply(set_edit(light, "part", int64_t(200)), error));
	for (int i = 0; i < 9; ++i) {
		TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {row->id, kUserPoint, 0}), error));
		TEST_EXPECT(document.apply(set_edit({row->id, kUserPoint, document.last_added()}, "name",
		                               std::string("sitex") + std::to_string(i)), error));
	}
	TEST_EXPECT(test_io::write_file(root + "/armory.3di", serialized(document)));
	const AssetScan scan = scan_project_assets(paths, project);
	AssetGraph graph;
	ValidationCache cache;
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	const std::vector<Diagnostic> findings =
			validate_project({ paths, project, scan, open }, graph, cache);
	const auto has = [&](const char *code, DiagnosticSeverity severity) {
		for (const Diagnostic &d : findings)
			if (d.code() == code && d.severity == severity) return true;
		return false;
	};
	// S11h: a ninth seat takes the control seat and ends the scan; the model still loads.
	TEST_EXPECT(has("model.light_part", DiagnosticSeverity::Error) && has("model.seats", DiagnosticSeverity::Warning));
	std::printf("validation: a light off LOD 0's parts is an error, a ninth seat a warning\n");
	return 0;
}

// S11h: a shipped model loads in the game, so the validator finds no error in one; the gate blocks
// exactly where retail fails. The forms the shipped models use beside the validator's errors (a
// register past the table, a light naming one with no table, a light on a part LOD 0 lacks, a frame
// past the MTRX table), each minted on the mount (three CTRL registers) or the house (none), raise
// none of them; the forms the shipped corpus raised warnings and notes for raise that warning, never
// an error (the install's 967 models: 20 LOD orders, 5 unknown registers, 84 repeated user point
// names, 3 materials no strip draws; Black Hawk Down's migrated 677 the same three warnings).
int shipped_forms_validate() {
	constexpr NodeKind kLod = node_kind(ModelKind::Lod);
	int forms = 0;
	// `what` minted on `base` by `make`; `warning` the one finding it must raise, "" for none.
	const auto form = [&](const char *base, const char *what, const std::function<bool(ModelDocument &)> &make,
	                      const char *warning = "") {
		auto document = std::make_shared<ModelDocument>();
		Diagnostic error;
		bool ok = load(*document, synth_dir() + "/" + base, base) && document->model_row();
		// Two MTRX rows at least, for a frame byte of 1 to name one.
		while (ok && document->model_row()->frames.size() < 2)
			ok = document->apply(op_edit(EditOperation::Add, {document->model_row()->id, kFrame, 0}), error);
		ok = ok && make(*document);
		bool clean = ok, warned = !*warning;
		for (const Diagnostic &d : validated(document)) {
			if (d.severity == DiagnosticSeverity::Error) {
				std::printf("shipped forms: %s on %s: %s: %s\n", what, base, d.code().c_str(), d.message.c_str());
				clean = false;
			}
			warned = warned || d.code() == warning;
		}
		if (!ok) std::printf("shipped forms: %s on %s: not minted (%s)\n", what, base, error.message.c_str());
		if (!warned) std::printf("shipped forms: %s on %s: no %s\n", what, base, warning);
		++forms;
		return clean && warned;
	};
	// Each edit through the document; false when one is refused.
	const auto edits = [](ModelDocument &document, std::initializer_list<Edit> list) {
		Diagnostic error;
		return document.apply(std::vector<Edit>(list), error);
	};
	const auto model_of = [](const ModelDocument &document) { return document.model_row()->id; };
	const auto material = [&](const ModelDocument &d) { return NodeAddress{model_of(d), kMaterial, d.model_row()->ids.lists[1][0].id}; };
	// LOD 0's first part animation (none: an address no edit takes).
	const auto panm = [&](const ModelDocument &d) {
		const std::vector<RecordIds> &rows = d.model_row()->ids.lists[0][0].lists[0];
		return rows.empty() ? NodeAddress{} : NodeAddress{model_of(d), kPanm, rows[0].id};
	};
	const auto added = [&](ModelDocument &d, NodeKind kind) {
		Diagnostic error;
		return d.apply(op_edit(EditOperation::Add, {model_of(d), kind, 0}), error)
		               ? NodeAddress{model_of(d), kind, d.last_added()}
		               : NodeAddress{};
	};
	// A flipbook on the register clock naming a register of the table (ArmGlove's: 8 frames on register 0).
	TEST_EXPECT(form("mount.3di", "a flipbook on a table register", [&](ModelDocument &d) {
		return edits(d, {set_edit(material(d), "texanim.frames", int64_t(8)), set_edit(material(d), "texanim.type", int64_t(1)),
		                 set_edit(material(d), "texanim.time", int64_t(2))});
	}));
	// The U and V generators reading a table register (dM1A1's style 117 on registers 0 and 3), the RGB one adding
	// one (B50Cal's style 114, 0x72, on register 0).
	TEST_EXPECT(form("mount.3di", "generators on table registers", [&](ModelDocument &d) {
		return edits(d, {set_edit(material(d), "ugen.style", int64_t(117)), set_edit(material(d), "ugen.param", int64_t(0)),
		                 set_edit(material(d), "vgen.style", int64_t(117)), set_edit(material(d), "vgen.param", int64_t(2)),
		                 set_edit(material(d), "rgbgen.style", int64_t(114)), set_edit(material(d), "rgbgen.param", int64_t(1))});
	}));
	// A light naming a table register on part 0 (FireBrl1's style 113 on register 0), one on a part LOD 0 has
	// (Armry01's).
	TEST_EXPECT(form("mount.3di", "lights on table registers and LOD 0's parts", [&](ModelDocument &d) {
		const NodeAddress lit = added(d, kLight);
		if (!lit.child || !edits(d, {set_edit(lit, "style", int64_t(113)), set_edit(lit, "param", int64_t(2))})) return false;
		const NodeAddress on_part = added(d, kLight);
		const int64_t parts = int64_t(d.model_row()->lods[0].lod.render_object_count);
		return on_part.child && parts > 1 && edits(d, {set_edit(on_part, "part", parts - 1)});
	}));
	// A part turning through a row of the MTRX table (B50Cal's rows of tracks on row 1, its rotx on register 1).
	TEST_EXPECT(form("mount.3di", "a part turning through an MTRX row", [&](ModelDocument &d) {
		return edits(d, {set_edit(panm(d), "flags.rotation", int64_t(2)), set_edit(panm(d), "matrix", int64_t(1)),
		                 set_edit(panm(d), "rotx.style", int64_t(113)), set_edit(panm(d), "rotx.param", int64_t(1))});
	}));
	// A frame byte of 0x80 and above names no row: on a row that turns through none (ArmGlove's), on a row of
	// tracks (Black Hawk Down's minigund, migrated).
	TEST_EXPECT(form("mount.3di", "frame bytes past 0x7F", [&](ModelDocument &d) {
		return edits(d, {set_edit(panm(d), "flags.rotation", int64_t(2)), set_edit(panm(d), "matrix", int64_t(0x80))});
	}));
	TEST_EXPECT(form("mount.3di", "frame byte 0xFF on a still row", [&](ModelDocument &d) {
		return edits(d, {set_edit(panm(d), "flags.rotation", int64_t(0)), set_edit(panm(d), "matrix", int64_t(255))});
	}));
	// A frame byte on a row that turns through no frame, past the table (the install's 3,040 such bytes: a still
	// row's, a billboard's).
	TEST_EXPECT(form("mount.3di", "a frame byte on a still row past the table", [&](ModelDocument &d) {
		return edits(d, {set_edit(panm(d), "flags.rotation", int64_t(0)), set_edit(panm(d), "matrix", int64_t(100))});
	}));
	TEST_EXPECT(form("mount.3di", "a frame byte on a billboard past the table", [&](ModelDocument &d) {
		return edits(d, {set_edit(panm(d), "flags.rotation", int64_t(3)), set_edit(panm(d), "matrix", int64_t(100))});
	}));
	// A loaded track on a phase style above 0x70 (0x74: a track reads its register for 0x71 alone) naming a table
	// register (the install's one, FTruck1's), and tracks the load does not copy naming registers past the table
	// (Black Hawk Down's Ship01, migrated).
	TEST_EXPECT(form("mount.3di", "tracks on table registers, unloaded ones past the table", [&](ModelDocument &d) {
		return edits(d, {set_edit(panm(d), "flags.rotation", int64_t(2)), set_edit(panm(d), "flags.scale", int64_t(0)),
		                 set_edit(panm(d), "roty.style", int64_t(0x74)), set_edit(panm(d), "roty.param", int64_t(2)),
		                 set_edit(panm(d), "scalex.style", int64_t(113)), set_edit(panm(d), "scalex.param", int64_t(9)),
		                 set_edit(panm(d), "flags.trans_axis", int64_t(0)), set_edit(panm(d), "trans.style", int64_t(113)),
		                 set_edit(panm(d), "trans.param", int64_t(200))});
	}));
	// With no CTRL table: an unloaded track naming a register (Black Hawk Down's AirHang1, migrated) and a light
	// of a style that names none (CmpFire1's style 24).
	TEST_EXPECT(form("house.3di", "with no table, an unloaded track naming a register and a plain light", [&](ModelDocument &d) {
		const NodeAddress lit = added(d, kLight);
		return lit.child && edits(d, {set_edit(panm(d), "flags.scale", int64_t(0)), set_edit(panm(d), "scalex.style", int64_t(113)),
		                              set_edit(panm(d), "scalex.param", int64_t(9)), set_edit(lit, "style", int64_t(24))});
	}));
	// A user point on no part (Bed01's -1).
	TEST_EXPECT(form("mount.3di", "a user point on no part", [&](ModelDocument &d) {
		return edits(d, {set_edit({model_of(d), kUserPoint, d.model_row()->ids.lists[3][0].id}, "part", int64_t(-1))});
	}));
	// The warnings: a register of no name the engine knows, an empty one (IBlock02's register 1) and one past the
	// catalog (DT801's VEHICLE_TIRE14); a LOD that takes over at more pixels than the one before it (Cboat05's LOD
	// 1 at 57 after LOD 0's 0); two user points of one name, in any case (CmbtHlmt's GFLASH01).
	TEST_EXPECT(form("mount.3di", "an empty register name", [&](ModelDocument &d) {
		return edits(d, {set_edit({model_of(d), kRegister, d.model_row()->ids.lists[4][1].id}, "name", std::string())});
	}, "model.register_unknown"));
	TEST_EXPECT(form("mount.3di", "a register past the catalog", [&](ModelDocument &d) {
		return edits(d, {set_edit({model_of(d), kRegister, d.model_row()->ids.lists[4][2].id}, "name",
		                          std::string("VEHICLE_TIRE14"))});
	}, "model.register_unknown"));
	TEST_EXPECT(form("pump.3di", "a LOD out of order", [&](ModelDocument &d) {
		return d.model_row()->lods.size() >= 2 &&
		       edits(d, {set_edit({model_of(d), kLod, d.model_row()->ids.lists[0][0].id}, "threshold", int64_t(0)),
		                 set_edit({model_of(d), kLod, d.model_row()->ids.lists[0][1].id}, "threshold", int64_t(57))});
	}, "model.lod_order"));
	TEST_EXPECT(form("mount.3di", "two user points of one name", [&](ModelDocument &d) {
		const ModelRow &row = *d.model_row();
		std::string name = row.user_points[0].name;
		for (char &c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
		return row.user_points.size() >= 2 && edits(d, {set_edit({model_of(d), kUserPoint, row.ids.lists[3][1].id}, "name", name)});
	}, "model.user_point_duplicate"));
	std::printf("shipped forms: %d forms the shipped models use beside the validator's errors raise none\n", forms);
	return 0;
}

// Every Black Hawk Down GP model of the fixtures (the synthetic GPM and GPP, common/gp_model_bytes.h, and the
// variants the threedi_gp rows mint), migrated to 3DI3 as the editor's import does (formats/threedi_gp), opens
// as a model and validates with no error: the migration writes nothing a project's model check refuses (BHD's
// 677 shipped models held to it while the corpus was read).
int bhd_migrated() {
	const model_variants::Files gp = model_variants::gp_models();
	std::vector<std::pair<std::string, std::vector<uint8_t>>> files = {{"gpm_model", gp_test::gpm_model()},
	                                                                   {"gpp_model", gp_test::gpp_model()}};
	for (const std::string &path : gp.paths) files.push_back({path, model_variants::bytes_of(path)});
	size_t models = 0;
	for (const auto &file : files) {
		// A GP model is no model the game's loader takes, its chunk tag not 3DI3 [orig:
		// ThreediGp_LoadFromFile @ 0x5B5806, CChunkFile_LoadFromFile with the tag 0x33494433]: the editor migrates it
		// before it opens.
		TEST_EXPECT(file.second.size() >= 4 && std::memcmp(file.second.data(), "3DI3", 4) != 0);
		std::string message;
		opennova::threedi_gp::File parsed;
		std::vector<uint8_t> model;
		std::vector<opennova::threedi_gp::MigrateNote> notes;
		const bool migrated = opennova::threedi_gp::parse(file.second.data(), file.second.size(), parsed, message) &&
		                      opennova::threedi_gp::migrate(parsed, model, notes, message);
		if (!migrated) std::printf("bhd migrated: %s does not migrate: %s\n", file.first.c_str(), message.c_str());
		TEST_EXPECT(migrated);
		if (!migrated) continue;
		auto document = std::make_shared<ModelDocument>();
		Diagnostic error;
		const ProjectDocument project;
		TEST_EXPECT(document->load_bytes(model, "models/" + fs::path(file.first).filename().generic_string(), AssetKind::Model,
		                                 project.target_game, error));
		for (const Diagnostic &d : validated(document)) {
			if (d.severity != DiagnosticSeverity::Error) continue;
			std::printf("bhd migrated: %s %s: %s: %s\n", file.first.c_str(), d.record.c_str(), d.code().c_str(), d.message.c_str());
			TEST_EXPECT(d.severity != DiagnosticSeverity::Error);
		}
		++models;
	}
	std::printf("bhd migrated: %zu GP models migrate, open and validate with no error\n", models);
	return model_variants::report_missing(gp) == 0 ? 0 : 1;
}

// S11h: the frame a part animation turns through and the registers the records name, as the
// load and the runtime read them. A frame byte of 0, 128 or 255 names none (a signed byte, a
// frame only above zero) and only a spinner or Euler row (rotation types 1 and 2) reads one, so
// only a positive row past the MTRX table on such a row is an error, and the field applies there
// alone. An index past the end of a CTRL table is an error; with no table, the index is the
// global register the game reads (past the 96, an error) and a light's does not load.
int frames_and_registers() {
	auto document = std::make_shared<ModelDocument>();
	TEST_EXPECT(load(*document, synth_dir() + "/house_lod0_sine_rotx.3di", "house.3di"));
	const ModelRow *row = document->model_row();
	TEST_EXPECT(row && !row->lods.empty() && !row->lods[0].panm.empty() && !row->materials.empty());
	if (!row || row->lods.empty() || row->lods[0].panm.empty() || row->materials.empty()) return 1;
	const NodeId model = row->id;
	Diagnostic error;
	// Row 0 and a frame after it.
	while (document->model_row()->frames.size() < 2)
		TEST_EXPECT(document->apply(op_edit(EditOperation::Add, {model, kFrame, 0}), error));
	const int64_t frames = static_cast<int64_t>(document->model_row()->frames.size());
	const NodeAddress panm{model, kPanm, document->model_row()->ids.lists[0][0].lists[0][0].id};
	const FieldSchema *matrix = nullptr;
	for (const FieldSchema &field : document->fields(kPanm))
		if (field.id == "matrix") matrix = &field;
	TEST_EXPECT(matrix != nullptr);
	if (!matrix) return 1;
	const auto on_row = [&](const char *code) {
		std::vector<Diagnostic> out;
		for (const Diagnostic &d : validated(document))
			if (d.code() == code && d.child_id == panm.child) out.push_back(d);
		return out;
	};
	for (const int64_t rotation : {0, 1, 2, 3, 4}) {
		TEST_EXPECT(document->apply(set_edit(panm, "flags.rotation", rotation), error));
		const bool reads = rotation == 1 || rotation == 2;
		TEST_EXPECT(document->field_on(panm, *matrix).applies == (reads ? Applicability::Reads : Applicability::Ignored));
		for (const int64_t byte : {int64_t(0), int64_t(128), int64_t(255), int64_t(192), frames - 1, frames, int64_t(127)}) {
			TEST_EXPECT(document->apply(set_edit(panm, "matrix", byte), error));
			const std::vector<Diagnostic> found = on_row("model.frame_missing");
			const bool past = reads && byte > 0 && byte < 128 && byte >= frames;
			TEST_EXPECT(found.size() == (past ? 1u : 0u));
			if (past && !found.empty())
				TEST_EXPECT(found[0].severity == DiagnosticSeverity::Error && found[0].field == "matrix" &&
				            found[0].message == "Rotation frame " + std::to_string(byte) + " is not one of the model's " +
				                                        std::to_string(frames) + ".");
		}
	}
	// A frame a part animation turns through stays while it does; a byte that names none
	// lets the last row go.
	TEST_EXPECT(document->apply(set_edit(panm, "flags.rotation", int64_t(2)), error));
	TEST_EXPECT(document->apply(set_edit(panm, "matrix", frames - 1), error));
	const NodeAddress last{model, kFrame, document->model_row()->ids.lists[5].back().id};
	TEST_EXPECT(!document->apply(op_edit(EditOperation::Remove, last), error));
	TEST_EXPECT(document->apply(set_edit(panm, "matrix", int64_t(255)), error));
	TEST_EXPECT(document->apply(op_edit(EditOperation::Remove, last), error));

	// The registers the records name, first with no CTRL table (the house has none): a style
	// that reads a register reads the global one its index numbers (a warning naming it; past
	// the 96, an error), another style above 0x70 takes the index as its phase, a light's does
	// not load, and a flipbook with no frames or a track the load does not copy names none.
	TEST_EXPECT(document->model_row()->registers.empty());
	const NodeAddress material{model, kMaterial, document->model_row()->ids.lists[1][0].id};
	const auto set_all = [&](const NodeAddress &at, std::initializer_list<std::pair<const char *, int64_t>> fields) {
		bool applied = true;
		for (const auto &field : fields) applied = document->apply(set_edit(at, field.first, field.second), error) && applied;
		return applied;
	};
	TEST_EXPECT(set_all(material, {{"rgbgen.style", 0x71}, {"rgbgen.param", 0}, {"alphagen.style", 0x72},
	                               {"alphagen.param", 5}, {"ugen.style", 0x74}, {"ugen.param", 5}, {"vgen.style", 0x71},
	                               {"vgen.param", 200}, {"texanim.frames", 4}, {"texanim.type", 1}, {"texanim.time", 3}}));
	TEST_EXPECT(set_all(panm, {{"flags.rotation", 2}, {"flags.scale", 0}, {"rotx.style", 0x71}, {"rotx.param", 0},
	                           {"roty.style", 0x72}, {"roty.param", 7}, {"scalex.style", 0x71}, {"scalex.param", 9}}));
	TEST_EXPECT(document->apply(op_edit(EditOperation::Add, {model, kLight, 0}), error));
	const NodeAddress light{model, kLight, document->last_added()};
	TEST_EXPECT(set_all(light, {{"style", 0x71}, {"param", 0}}));
	// The register findings of a model, by record and field.
	using Found = std::map<std::string, std::pair<DiagnosticSeverity, std::string>>;
	bool codes_hold = true;
	const auto registers_found = [&](const std::shared_ptr<const ModelDocument> &of) {
		Found out;
		for (const Diagnostic &d : validated(of)) {
			if (d.code() != "model.register_missing" && d.code() != "model.light_no_registers") continue;
			// The light's crash is its own code, the one of them that refuses a build (the gate follows
			// retail): every other register past its table is listed.
			const bool crash = d.code() == "model.light_no_registers";
			codes_hold = codes_hold && crash == (d.child_id == light.child && of->model_row()->registers.empty()) &&
			             d.row() && d.row()->gates_build == crash;
			const char *record = d.child_id == material.child ? "material"
			                     : d.child_id == panm.child   ? "panm"
			                     : d.child_id == light.child  ? "light"
			                                                  : "?";
			out[std::string(record) + " " + d.field] = {d.severity, d.message};
		}
		return out;
	};
	const auto is = [](const Found &found, const std::string &key, DiagnosticSeverity severity,
	                   const std::string &message) {
		const auto it = found.find(key);
		return it != found.end() && it->second.first == severity && it->second.second == message;
	};
	const DiagnosticSeverity warning = DiagnosticSeverity::Warning, error_ = DiagnosticSeverity::Error;
	const std::string no_table = "The model has no CTRL registers: the game reads ";
	Found found = registers_found(document);
	TEST_EXPECT(found.size() == 6);
	TEST_EXPECT(is(found, "material rgbgen.param", warning, no_table + "register 0 as the global register LOD_FRAC."));
	TEST_EXPECT(is(found, "material ugen.param", warning, no_table + "register 5 as the global register TALK."));
	TEST_EXPECT(is(found, "material vgen.param", error_,
	               no_table + "register 200 as a global register, past the 96 it has."));
	TEST_EXPECT(is(found, "material texanim.time", warning, no_table + "register 3 as the global register FLICKER."));
	TEST_EXPECT(is(found, "panm rotx.param", warning, no_table + "register 0 as the global register LOD_FRAC."));
	TEST_EXPECT(is(found, "light param", error_,
	               "The model has no CTRL registers, and the game crashes loading a light that names one."));
	TEST_EXPECT(codes_hold);
	TEST_EXPECT(set_all(material, {{"texanim.frames", 0}}));
	TEST_EXPECT(registers_found(document).count("material texanim.time") == 0);
	TEST_EXPECT(set_all(material, {{"texanim.frames", 4}}));

	// A first register gives the model a table, which changes what every material's and loaded
	// track's register byte names (a global register now, an entry of the table then): refused,
	// nothing committed, while the game reads one (the X track's, met first), taken once none does.
	// The light's index, past the table the model lacks, stays past the one the register makes.
	const uint64_t tableless = document->revision();
	TEST_EXPECT(!document->apply(op_edit(EditOperation::Add, {model, kRegister, 0}), error) &&
	            error.code() == "document.collection" &&
	            error.message.find(" rotx.param reads global register 0 now. Point it at no register first.") !=
	                    std::string::npos &&
	            document->revision() == tableless);
	TEST_EXPECT(set_all(material, {{"rgbgen.style", 0}, {"alphagen.style", 0}, {"ugen.style", 0}, {"vgen.style", 0},
	                               {"texanim.type", 0}}) &&
	            set_all(panm, {{"rotx.style", 0}, {"roty.style", 0}}));
	TEST_EXPECT(document->apply(op_edit(EditOperation::Add, {model, kRegister, 0}), error));
	Value lit;
	TEST_EXPECT(document->get(light, "param", lit) && std::get<int64_t>(lit) == 1);
	TEST_EXPECT(set_all(material, {{"rgbgen.style", 0x71}, {"alphagen.style", 0x72}, {"ugen.style", 0x74},
	                               {"vgen.style", 0x71}, {"texanim.type", 1}}) &&
	            set_all(panm, {{"rotx.style", 0x71}, {"roty.style", 0x72}}));
	// With a table, an index past its end is an error whatever the style reads, as the load
	// reads past the table to swap it; an index in it is none.
	const std::string past = " is not one of the model's 1: the game reads past the end of its registers.";
	found = registers_found(document);
	TEST_EXPECT(found.size() == 6);
	TEST_EXPECT(is(found, "material alphagen.param", error_, "Register 5" + past));
	TEST_EXPECT(is(found, "material ugen.param", error_, "Register 5" + past));
	TEST_EXPECT(is(found, "material vgen.param", error_, "Register 200" + past));
	TEST_EXPECT(is(found, "material texanim.time", error_, "Register 3" + past));
	TEST_EXPECT(is(found, "panm roty.param", error_, "Register 7" + past));
	TEST_EXPECT(is(found, "light param", error_, "Register 1" + past));
	TEST_EXPECT(set_all(light, {{"param", 1}}));
	TEST_EXPECT(set_all(panm, {{"flags.scale", 1}}));
	found = registers_found(document);
	TEST_EXPECT(found.size() == 7);
	TEST_EXPECT(is(found, "light param", error_, "Register 1" + past));
	TEST_EXPECT(is(found, "panm scalex.param", error_, "Register 9" + past));

	// The second RGB generator, which no field shows, is read as the first is; its finding
	// names it.
	Threedi3di3 house{};
	TEST_EXPECT(threedi_3di3_read((synth_dir() + "/house_lod0_sine_rotx.3di").c_str(), &house) == 0);
	std::vector<uint8_t> bytes;
	if (house.material_count > 0) {
		house.materials[0].rgb_gen2.style = 0x72;
		house.materials[0].rgb_gen2.reg = 3;
		TEST_EXPECT(threedi_3di3_write_memory(&house, bytes) == 0);
	}
	threedi_3di3_free(&house);
	auto second = std::make_shared<ModelDocument>();
	TEST_EXPECT(second->load_bytes(bytes, "house.3di", AssetKind::Model, "jo", error));
	size_t seconds = 0;
	for (const Diagnostic &d : validated(second)) {
		if (d.code() != "model.register_missing") continue;
		TEST_EXPECT(d.severity == warning && d.record_kind == kMaterial && d.field.empty() &&
		            d.message == no_table + "the second RGB generator's register 3 as the global register FLICKER.");
		++seconds;
	}
	TEST_EXPECT(seconds == 1);
	std::printf("frames and registers: a frame byte read as the pose reads it, a register as the load swaps it\n");
	return 0;
}

// S13 D8: what names a model's CTRL register or MTRX row by its index is a Record reference
// (ModelRegister, ModelFrame), each an edge of the model's extraction into its record sets (every
// register and every row, a symbol of its index whose value is the record's own name). A register
// nothing names removed from the middle moves every later register's references down in the same
// step, read back from the written model too; its undo gives both back, the model's own bytes; a
// register a field the game reads names is refused, nothing committed, the refusal naming the
// field; one moved to the front, one added there and one duplicated renumber; a batch's later edit
// names the registers as the renumbering left them; an index past the table stays past it. Fields
// the game does not read (a track the load does not copy, a frame byte on a row that turns through
// none) are renumbered, and never refuse. A frame nothing names removed or moved moves the arm's
// frame, a duplicated one too; one the arm turns through is refused; row 0, never read, takes
// edits, and a byte would never move onto it nor past 127; a byte naming none (0, 255) stays. A
// field cannot be given an index it cannot hold (a register byte past 255). The second RGB
// generator, which no field sets, keeps a register it names where it is, and is a Record reference
// all the same. Find in document finds a register field by its register's name.
int record_references() {
	const std::vector<uint8_t> original = rig_model::bytes();
	TEST_EXPECT(!original.empty());
	ModelDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(original, "rig.3di", AssetKind::Model, "jo", error));
	TEST_EXPECT(serialized(document) == original);
	const ModelRow *row = document.model_row();
	TEST_EXPECT(row && row->registers.size() == 4 && row->frames.size() == 3 && row->lods[0].panm.size() == 2 &&
	            row->materials.size() == 2 && row->lights.size() == 1);
	if (!row || row->registers.size() != 4 || row->frames.size() != 3 || row->lods[0].panm.size() != 2) return 1;
	const NodeId model = row->id;
	const NodeAddress light{model, kLight, row->ids.lists[2][0].id};
	const NodeAddress glow{model, kMaterial, row->ids.lists[1][1].id};
	const NodeAddress base{model, kPanm, row->ids.lists[0][0].lists[0][0].id};
	const NodeAddress arm{model, kPanm, row->ids.lists[0][0].lists[0][1].id};
	// What the validator finds of a register or a frame the model lacks.
	const auto lacking = [&]() {
		size_t found = 0;
		for (const Diagnostic &d : document_type(DocumentTypeId::Model)->validate_file(document))
			found += d.code() == "model.register_missing" || d.code() == "model.frame_missing";
		return found;
	};
	TEST_EXPECT(lacking() == 0);
	const auto reg = [&](size_t i) { return NodeAddress{model, kRegister, document.model_row()->ids.lists[4][i].id}; };
	const auto frame = [&](size_t i) { return NodeAddress{model, kFrame, document.model_row()->ids.lists[5][i].id}; };
	const auto value_of = [&](const NodeAddress &at, const char *field) {
		Value value;
		return document.get(at, field, value) ? std::get<int64_t>(value) : int64_t(-1);
	};
	// The collections a Record reference names, and the edges and record sets the extraction makes.
	const std::vector<Document::TargetedCollection> targets = document.targeted_collections();
	TEST_EXPECT(targets.size() == 2 && targets[0].reference == ReferenceKind::ModelRegister &&
	            targets[0].kind == kRegister && targets[1].reference == ReferenceKind::ModelFrame &&
	            targets[1].kind == kFrame);
	const auto references = [](const Document &of) {
		Extracted extracted;
		extract_from_document(of, extracted);
		std::map<std::string, std::string> named; // "record field" -> the index it names
		for (const GraphEdge &edge : extracted.edges)
			if (edge.kind == ReferenceKind::ModelRegister || edge.kind == ReferenceKind::ModelFrame) {
				// One in another file or scope fails the comparisons below.
				const std::string elsewhere = edge.scope == of.path() && edge.source == of.path() ? "" : " elsewhere";
				const std::string frame = edge.kind == ReferenceKind::ModelFrame ? "frame " : "";
				named[frame + of.kind_token(edge.address.kind) + " " + edge.field + elsewhere] = edge.value;
			}
		return named;
	};
	TEST_EXPECT(references(document) == (std::map<std::string, std::string>{{"material rgbgen.param", "2"},
	                                                                        {"light param", "3"},
	                                                                        {"part_animation rotx.param", "3"},
	                                                                        {"part_animation roty.param", "2"},
	                                                                        {"frame part_animation matrix", "2"}}));
	Extracted extracted;
	extract_from_document(document, extracted);
	std::vector<std::string> register_names;
	size_t frames = 0;
	for (const GraphSymbol &symbol : extracted.symbols) {
		if (symbol.kind == ReferenceKind::ModelRegister) {
			TEST_EXPECT(symbol.name == std::to_string(register_names.size()) && symbol.scope == "rig.3di" &&
			            symbol.field.empty());
			register_names.push_back(symbol.value);
		}
		if (symbol.kind == ReferenceKind::ModelFrame)
			TEST_EXPECT(symbol.name == std::to_string(frames++) && symbol.value.empty());
	}
	TEST_EXPECT(register_names ==
	                    (std::vector<std::string>{"HEAT_GLOW", "EWEAP_GUNYAW", "EWEAP_GUNPITCH", "FLICKER"}) &&
	            frames == 3);
	// What the written model holds: the generator's, the light's and the tracks' registers.
	const auto written = [&](int rgb, int light_reg, int rotx, int roty, int matrix) {
		const std::vector<uint8_t> bytes = serialized(document);
		Threedi3di3 read{};
		bool same = threedi_3di3_read_memory(bytes.data(), bytes.size(), &read) == 0 && read.material_count == 2 &&
		            read.light_count == 1 && read.lod_count == 1 && read.lods[0].part_animation_count == 2;
		if (same) {
			const ThreediPartAnimation &turns = read.lods[0].part_animations[1];
			same = read.materials[1].rgb_gen.reg == rgb && read.lights[0].phase == light_reg &&
			       turns.rotation_x.control_param == rotx && turns.rotation_y.control_param == roty &&
			       turns.matrix_index == matrix;
		}
		threedi_3di3_free(&read);
		return same;
	};
	TEST_EXPECT(written(2, 3, 3, 2, 2));
	// Find in document finds a field naming a register by the register's name, as the picker shows
	// it: FLICKER's own name, the light's and the X track's parameters.
	std::set<std::string> flicker;
	for (const DocumentHit &hit : find_in_document(document, "FLICKER", SearchOptions{}))
		flicker.insert(std::string(document.kind_token(hit.address.kind)) + " " + hit.field + " " + hit.text);
	TEST_EXPECT(flicker == (std::set<std::string>{"register name FLICKER", "light param FLICKER",
	                                              "part_animation rotx.param FLICKER"}));

	// A register nothing names removed from the middle: the later ones' references move down, one step.
	TEST_EXPECT(document.apply(op_edit(EditOperation::Remove, reg(1)), error));
	TEST_EXPECT(written(1, 2, 2, 1, 2) && document.model_row()->registers.size() == 3 &&
	            std::strcmp(document.model_row()->registers[1].name, "EWEAP_GUNPITCH") == 0);
	TEST_EXPECT(value_of(light, "param") == 2 && lacking() == 0);
	document.undo();
	TEST_EXPECT(serialized(document) == original && !document.can_undo() && !document.dirty());
	document.redo();
	TEST_EXPECT(written(1, 2, 2, 1, 2));
	document.undo();
	// A register a field the game reads names: refused, nothing committed, the refusal naming it
	// (the arm's Y track, met first: a LOD's part animations come before the materials).
	const uint64_t revision = document.revision();
	TEST_EXPECT(!document.apply(op_edit(EditOperation::Remove, reg(2)), error) && error.code() == "document.collection" &&
	            error.message == "rig/LOD 1/Part animation 2 roty.param reads this register: point it at another first." &&
	            document.revision() == revision && serialized(document) == original);
	// FLICKER moved to the front; a register added there; GUNYAW duplicated (its copy right after it).
	TEST_EXPECT(document.apply(op_edit(EditOperation::Move, reg(3), 0, 0), error) && written(3, 0, 0, 3, 2));
	document.undo();
	TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {model, kRegister, 0}, 0, 0), error) && written(3, 4, 4, 3, 2));
	document.undo();
	TEST_EXPECT(document.apply(op_edit(EditOperation::Duplicate, reg(1)), error) && written(3, 4, 4, 3, 2) &&
	            std::strcmp(document.model_row()->registers[2].name, "EWEAP_GUNYAW") == 0);
	document.undo();
	// A batch: the Remove, then the light pointed at register 0 as the Remove left them; one step.
	TEST_EXPECT(document.apply({op_edit(EditOperation::Remove, reg(1)), set_edit(light, "param", int64_t(0))}, error) &&
	            written(1, 0, 2, 1, 2));
	document.undo();
	TEST_EXPECT(serialized(document) == original && !document.can_undo());
	// An index past the table (the light's 4 of four) stays past it: across an Add at the front it is
	// 5, still a register the model lacks, never FLICKER sliding into its place.
	TEST_EXPECT(document.apply(set_edit(light, "param", int64_t(4)), error) && lacking() == 1);
	TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {model, kRegister, 0}, 0, 0), error) &&
	            value_of(light, "param") == 5 && lacking() == 1);
	document.undo();
	document.undo();
	TEST_EXPECT(serialized(document) == original);

	// Fields the game does not read: the glow's generator off its register and the arm's rotation off
	// (its tracks no longer loaded), so nothing the game reads names GUNPITCH. Its Remove is taken;
	// the unread Y track naming it keeps its byte, the unread X track's FLICKER moves down with the
	// light's. Removing GUNYAW renumbers the unread tracks as it does the read ones.
	TEST_EXPECT(document.apply({set_edit(glow, "rgbgen.style", int64_t(0)), set_edit(arm, "flags.rotation", int64_t(0))}, error));
	TEST_EXPECT(references(document) == (std::map<std::string, std::string>{{"light param", "3"}}));
	TEST_EXPECT(document.apply(op_edit(EditOperation::Remove, reg(2)), error) && value_of(arm, "roty.param") == 2 &&
	            value_of(arm, "rotx.param") == 2 && value_of(light, "param") == 2);
	document.undo();
	TEST_EXPECT(document.apply(op_edit(EditOperation::Remove, reg(1)), error) && value_of(arm, "roty.param") == 1 &&
	            value_of(arm, "rotx.param") == 2 && value_of(light, "param") == 2);
	document.undo();
	document.undo();
	TEST_EXPECT(serialized(document) == original);

	// A field cannot be given an index it cannot hold: the glow's byte on register 255 of 256, a
	// register added at the front would make it 256. Refused, nothing committed.
	std::vector<Edit> more;
	for (int i = 0; i < 252; ++i) more.push_back(op_edit(EditOperation::Add, {model, kRegister, 0}));
	TEST_EXPECT(document.apply(more, error) && document.model_row()->registers.size() == 256);
	TEST_EXPECT(document.apply(set_edit(glow, "rgbgen.param", int64_t(255)), error));
	const uint64_t full = document.revision();
	// (The material named by its place, not its shader tag: S17.)
	TEST_EXPECT(document.record_path(glow).rfind("rig/Material ", 0) == 0);
	TEST_EXPECT(!document.apply(op_edit(EditOperation::Add, {model, kRegister, 0}, 0, 0), error) &&
	            error.code() == "document.collection" &&
	            error.message == "Register 255 would move to 256, which " + document.record_path(glow) +
	                                     " rgbgen.param cannot hold." &&
	            document.revision() == full);
	document.undo();
	document.undo();
	TEST_EXPECT(serialized(document) == original);

	// Frames: one nothing names removed moves the arm's frame; the one it turns through is refused;
	// one added, one moved and one duplicated before it move it again.
	TEST_EXPECT(document.apply(op_edit(EditOperation::Remove, frame(1)), error) && written(2, 3, 3, 2, 1));
	document.undo();
	TEST_EXPECT(!document.apply(op_edit(EditOperation::Remove, frame(2)), error) &&
	            error.message == "rig/LOD 1/Part animation 2 matrix turns through this rotation frame: point it at another "
	                             "first.");
	TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {model, kFrame, 0}, 0, 1), error) && written(2, 3, 3, 2, 3));
	document.undo();
	TEST_EXPECT(document.apply(op_edit(EditOperation::Move, frame(2), 0, 1), error) && written(2, 3, 3, 2, 1));
	document.undo();
	TEST_EXPECT(document.apply(op_edit(EditOperation::Duplicate, frame(1)), error) && written(2, 3, 3, 2, 3) &&
	            document.model_row()->frames.size() == 4);
	document.undo();
	// Row 0 is never read (a frame byte of 0 names none): it takes a Remove, an Add before it, a Move;
	// but a byte the game reads never moves onto it.
	TEST_EXPECT(document.apply(op_edit(EditOperation::Remove, frame(0)), error) && written(2, 3, 3, 2, 1));
	document.undo();
	TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {model, kFrame, 0}, 0, 0), error) && written(2, 3, 3, 2, 3));
	document.undo();
	TEST_EXPECT(!document.apply(op_edit(EditOperation::Move, frame(2), 0, 0), error) &&
	            error.message == "Rotation frame 2 would move to 0, which rig/LOD 1/Part animation 2 matrix cannot "
	                             "name (a frame byte names rows 1 to 127)." &&
	            serialized(document) == original);
	// A byte naming no row (0, 255) stays through a shift; one on a row that turns through no frame
	// (the base's) is renumbered all the same.
	for (const int64_t none : {int64_t(0), int64_t(255)}) {
		TEST_EXPECT(document.apply({set_edit(base, "flags.rotation", int64_t(2)), set_edit(base, "matrix", none)}, error));
		TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {model, kFrame, 0}, 0, 1), error) &&
		            value_of(base, "matrix") == none && written(2, 3, 3, 2, 3));
		document.undo();
		document.undo();
	}
	TEST_EXPECT(document.apply(set_edit(base, "matrix", int64_t(2)), error));
	TEST_EXPECT(document.apply(op_edit(EditOperation::Add, {model, kFrame, 0}, 0, 1), error) &&
	            value_of(base, "matrix") == 3 && written(2, 3, 3, 2, 3));
	document.undo();
	document.undo();
	// A frame byte names rows 1 to 127: on row 127 of 128, a frame added before it is refused.
	std::vector<Edit> rows;
	for (int i = 0; i < 125; ++i) rows.push_back(op_edit(EditOperation::Add, {model, kFrame, 0}));
	TEST_EXPECT(document.apply(rows, error) && document.model_row()->frames.size() == 128);
	TEST_EXPECT(document.apply(set_edit(arm, "matrix", int64_t(127)), error));
	const uint64_t rows_full = document.revision();
	TEST_EXPECT(!document.apply(op_edit(EditOperation::Add, {model, kFrame, 0}, 0, 1), error) &&
	            error.message == "Rotation frame 127 would move to 128, which rig/LOD 1/Part animation 2 matrix cannot "
	                             "name (a frame byte names rows 1 to 127)." &&
	            document.revision() == rows_full);
	document.undo();
	document.undo();
	TEST_EXPECT(serialized(document) == original && !document.can_undo());

	// The second RGB generator, which no field sets, keeps the register it names where it is; it is
	// a Record reference all the same (its edge, its field's reference, read-only).
	ModelDocument second;
	TEST_EXPECT(second.load_bytes(rig_model::bytes(true), "rig.3di", AssetKind::Model, "jo", error));
	const NodeAddress gunyaw{second.model_row()->id, kRegister, second.model_row()->ids.lists[4][1].id};
	TEST_EXPECT(!second.apply(op_edit(EditOperation::Remove, gunyaw), error) &&
	            error.message == "A material's second RGB generator names register 3, which the editor shows and "
	                             "never sets: that register stays where it is.");
	TEST_EXPECT(second.apply(op_edit(EditOperation::Add, {second.model_row()->id, kRegister, 0}), error));
	TEST_EXPECT(references(second).count("material rgbgen2.param") == 1 &&
	            references(second).at("material rgbgen2.param") == "3");
	const NodeAddress paint{second.model_row()->id, kMaterial, second.model_row()->ids.lists[1][0].id};
	for (const FieldSchema &field : second.fields(kMaterial))
		if (field.id == "rgbgen2.param") {
			const FieldUse use = second.field_on(paint, field);
			TEST_EXPECT(use.reference == ReferenceKind::ModelRegister && use.read_only);
		}
	std::printf("record references: registers and frames renumbered with the edit that moves them\n");
	return 0;
}

} // namespace

// S12 D6: what the table says of a field reaches the Inspector: a unit apart from the name,
// each component of a group named apart (a light's Position X / Y / Z, one row; its colour's
// three channels, one row with a swatch; a frame's rows), an integer's range; a part index
// offers LOD 0's parts with the value the table calls none, any other typed; a register's index
// and a frame's row name records of the model (S13 D8: Record references, whose picker offers the
// model's registers and MTRX rows, only those the field can hold); the words the reader keeps and
// no witness explains (the second channel's material words, a light's byte 34, the header's
// derived radius) are shown, never set, the unwitnessed ones marked so.
int field_metadata() {
	auto document = std::make_shared<ModelDocument>();
	TEST_EXPECT(load(*document, synth_dir() + "/house_lod0_sine_rotx.3di", "house.3di"));
	const ModelRow *row = document->model_row();
	TEST_EXPECT(row && !row->lods.empty() && !row->lods[0].panm.empty() && !row->materials.empty());
	if (!row || row->lods.empty() || row->lods[0].panm.empty() || row->materials.empty()) return 1;
	const NodeId model = row->id;
	Diagnostic error;
	// A field's use on a record, and what its picker offers there: the graph over the document as it
	// stands (its snapshot, the one file of a project).
	const auto use_of = [&](const NodeAddress &at, const char *id) {
		for (const FieldSchema &field : document->fields(at.kind))
			if (field.id == id) return document->field_on(at, field);
		return FieldUse();
	};
	const auto picker = [&](const NodeAddress &at, const char *id) {
		AssetScan scan;
		AssetEntry entry;
		entry.logical_name = "house.3di";
		entry.relative_path = document->path();
		entry.kind = AssetKind::Model;
		scan.entries.push_back(entry);
		scan.index();
		AssetGraph graph;
		graph.update(ProjectPaths::for_root("."), ProjectDocument(), scan,
		             {std::shared_ptr<const DocumentBase>(document->snapshot())});
		return reference_choices(graph, use_of(at, id));
	};
	// A field as the Inspector shows it on a record: its table's words, what the record makes of
	// it (FieldUse) and the choices it offers there (Document::choices_on: an index's own), in one.
	auto schema = [&](const NodeAddress &at, const char *id) {
		for (const FieldSchema &field : document->fields(at.kind))
			if (field.id == id) {
				const FieldUse use = document->field_on(at, field);
				FieldSchema out = field;
				out.applies = use.applies;
				out.reference = use.reference;
				out.defines = use.defines;
				out.scope = use.scope;
				out.color = use.color;
				out.read_only = use.read_only;
				std::vector<FieldChoice> own;
				out.choices = document->choices_on(at, use, own);
				return out;
			}
		return FieldSchema();
	};
	TEST_EXPECT(document->apply(op_edit(EditOperation::Add, {model, kLight, 0}), error));
	const NodeAddress light{model, kLight, document->last_added()};
	// Where a record sits is the core's index of its row (Document::path_in, S13 D8): the new light
	// the model row's third table's last record.
	const Document::RecordPath at = document->path_in(*document->model_row(), light.child);
	TEST_EXPECT(at.size() == 1 && at[0].collection == 2 && at[0].index + 1 == document->model_row()->lights.size());
	const FieldSchema x = schema(light, "position.x"), y = schema(light, "position.y");
	TEST_EXPECT(x.label == "Position X" && y.label == "Position Y" && x.unit == "m" && x.group == "Position" &&
	            y.group == "Position");
	const FieldSchema red = schema(light, "start.r");
	TEST_EXPECT(red.color == FieldColor::Channel && red.group == "Start colour" && red.label == "Start red" &&
	            schema(light, "end.b").group == "End colour");
	TEST_EXPECT(schema(light, "cone").unit == "deg" && schema(light, "cone").label == "Cone half-angle");
	TEST_EXPECT(schema(light, "atten_end").label == "Reach" && schema(light, "atten_end").unit == "m");
	const FieldSchema byte34 = schema(light, "unknown1");
	TEST_EXPECT(byte34.read_only && byte34.applies == Applicability::Unverified && !byte34.description.empty());
	TEST_EXPECT(!document->apply(set_edit(light, "unknown1", int64_t(1)), error));
	// The light's part: LOD 0's parts, any other index typed.
	const FieldSchema part = schema(light, "part");
	const size_t parts = row->base->lods[0].render_object_count;
	TEST_EXPECT(part.open_choices && part.choices.size() == parts && parts > 0 && part.choices[0].label == "PN01");
	// A part animation's parent: none (255) and the parts, its record's own choices.
	const NodeAddress panm{model, kPanm, row->ids.lists[0][0].lists[0][0].id};
	const FieldSchema parent = schema(panm, "parent");
	TEST_EXPECT(parent.choices.size() == parts + 1 && parent.choices[0].value == 255 && parent.choices[0].label == "None");
	while (document->model_row()->frames.size() < 3)
		TEST_EXPECT(document->apply(op_edit(EditOperation::Add, {model, kFrame, 0}), error));
	TEST_EXPECT(document->apply(set_edit(panm, "flags.rotation", int64_t(2)), error));
	// Its frame byte names an MTRX row once its rotation turns through one, whatever the byte names
	// now: the picker offers the rows above 0 (0 names none); a row that turns through no frame (a
	// billboard) names none.
	for (const int64_t from : {int64_t(0), int64_t(255), int64_t(1)}) {
		TEST_EXPECT(document->apply(set_edit(panm, "matrix", from), error));
		const FieldUse matrix = use_of(panm, "matrix");
		const std::vector<ReferenceChoice> rows = picker(panm, "matrix");
		TEST_EXPECT(matrix.reference == ReferenceKind::ModelFrame && !matrix.own_choices &&
		            !matrix.schema->open_choices && schema(panm, "matrix").choices.empty());
		TEST_EXPECT(rows.size() == 2 && rows[0].name == "1" && rows[1].name == "2" &&
		            rows[1].status == ReferenceStatus::Present && rows[1].file == "house.3di");
		TEST_EXPECT(document->apply(set_edit(panm, "matrix", int64_t(2)), error));
	}
	TEST_EXPECT(document->apply(set_edit(panm, "flags.rotation", int64_t(3)), error));
	TEST_EXPECT(use_of(panm, "matrix").reference == ReferenceKind::None && picker(panm, "matrix").empty());
	TEST_EXPECT(document->apply(set_edit(panm, "flags.rotation", int64_t(2)), error));
	// With no CTRL table the load leaves a material's and a track's register bytes as the global
	// registers they number (no record of the model); a light's it swaps through the table it lacks.
	TEST_EXPECT(document->model_row()->registers.empty());
	const NodeAddress first_material{model, kMaterial, document->model_row()->ids.lists[1][0].id};
	TEST_EXPECT(document->apply(
	        {set_edit(first_material, "rgbgen.style", int64_t(0x71)), set_edit(light, "style", int64_t(0x71))}, error));
	TEST_EXPECT(use_of(first_material, "rgbgen.param").reference == ReferenceKind::None &&
	            use_of(light, "param").reference == ReferenceKind::ModelRegister);
	document->undo();
	// A generator's parameter names a register above style 0x70: the picker offers the model's
	// registers by index, each with its name in its record and as its label.
	TEST_EXPECT(document->apply(op_edit(EditOperation::Add, {model, kRegister, 0}), error));
	const NodeAddress reg{model, kRegister, document->last_added()};
	TEST_EXPECT(document->apply(set_edit(reg, "name", std::string("ENGINE_RPM")), error));
	const NodeAddress material{model, kMaterial, document->model_row()->ids.lists[1][0].id};
	TEST_EXPECT(document->apply(set_edit(material, "rgbgen.style", int64_t(0x71)), error));
	const std::vector<ReferenceChoice> named = picker(material, "rgbgen.param");
	TEST_EXPECT(use_of(material, "rgbgen.param").reference == ReferenceKind::ModelRegister &&
	            schema(material, "rgbgen.param").choices.empty());
	TEST_EXPECT(named.size() == 1 && named[0].name == "0" && named[0].kind == ReferenceKind::ModelRegister &&
	            named[0].record.size() >= 11 && named[0].record.substr(named[0].record.size() - 11) == "/ENGINE_RPM" &&
	            named[0].label == "ENGINE_RPM");
	TEST_EXPECT(document->apply(set_edit(material, "rgbgen.style", int64_t(0)), error));
	TEST_EXPECT(use_of(material, "rgbgen.param").reference == ReferenceKind::None);
	// Only a register the field can hold is offered: with 257 of them, the byte-sized
	// generator parameter takes registers 0..255, the 16-bit flipbook time all 257.
	while (document->model_row()->registers.size() < 257)
		TEST_EXPECT(document->apply(op_edit(EditOperation::Add, {model, kRegister, 0}), error));
	TEST_EXPECT(document->apply(set_edit(material, "rgbgen.style", int64_t(0x71)), error));
	const std::vector<ReferenceChoice> byte_param = picker(material, "rgbgen.param");
	TEST_EXPECT(byte_param.size() == 256 && byte_param.back().name == "255");
	TEST_EXPECT(document->apply(set_edit(material, "rgbgen.param", int64_t(255)), error));
	TEST_EXPECT(document->apply({set_edit(material, "texanim.frames", int64_t(4)), set_edit(material, "texanim.type", int64_t(1))}, error));
	const std::vector<ReferenceChoice> time = picker(material, "texanim.time");
	TEST_EXPECT(time.size() == 257 && time.back().name == "256");
	TEST_EXPECT(document->apply(set_edit(material, "rgbgen.style", int64_t(0)), error));
	// The material's words: a range, the reflection one row, the second channel shown only.
	TEST_EXPECT(schema(material, "alpha_test").ranged && schema(material, "alpha_test").max == 255.0);
	TEST_EXPECT(schema(material, "reflect.a").group == "Reflection" && schema(material, "reflect.a").label == "Reflection alpha");
	for (const char *id : {"rgbgen2.style", "rgbgen2.start.r", "reflect2.r", "emissive2", "glass2"}) {
		const FieldSchema second = schema(material, id);
		Value value;
		TEST_EXPECT(second.read_only && second.applies == Applicability::Unverified && document->get(material, id, value));
	}
	TEST_EXPECT(schema(material, "rgbgen2.end.g").section == "Second colour generator");
	// A frame's cells by row; the header's radius, derived, in metres.
	const NodeAddress frame{model, kFrame, document->model_row()->ids.lists[5][0].id};
	TEST_EXPECT(schema(frame, "r12").label == "Row 2, column 3" && schema(frame, "r12").group == "Row 2");
	const NodeAddress header{model, node_kind(ModelKind::Model), 0};
	const FieldSchema radius = schema(header, "max_radius");
	Value value;
	TEST_EXPECT(radius.read_only && radius.unit == "m" && document->get(header, "max_radius", value) &&
	            std::get<double>(value) > 0.0);
	std::printf("metadata: units, components, rows, part choices and the record pickers; the unwitnessed "
	            "words shown only\n");
	return 0;
}

// S13 V8: two versions of the model row draw alike, their user points aside, only when every other
// table holds the same records (alike_but_user_points: the model's viewport patches its held model with
// the user points where they are alike, and reads the document again where not). A user point moved or
// added is alike; the base, the header, a LOD, a part animation, a material, a light, a register or a
// frame of another value or of another count is not.
int alike_but_user_points_tables() {
	ModelDocument document;
	TEST_EXPECT(load(document, synth_dir() + "/armory.3di", "armory.3di"));
	const ModelRow *row = document.model_row();
	TEST_EXPECT(row && !row->lods.empty() && !row->lods[0].panm.empty() && !row->materials.empty() &&
	            !row->lights.empty() && !row->user_points.empty() && !row->registers.empty());
	if (!row || row->lods.empty() || row->lods[0].panm.empty() || row->materials.empty() || row->lights.empty() ||
	    row->user_points.empty() || row->registers.empty())
		return 1;
	const auto alike_after = [&](auto change) {
		const std::shared_ptr<ModelRow> other = std::static_pointer_cast<ModelRow>(row->clone());
		change(*other);
		return alike_but_user_points(*row, *other);
	};
	TEST_EXPECT(alike_after([](ModelRow &) {}));
	TEST_EXPECT(alike_after([](ModelRow &r) { r.user_points[0].x += 65536; }));
	TEST_EXPECT(alike_after([](ModelRow &r) { r.user_points.push_back(r.user_points[0]); }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.base = opennova::assets::Model(); }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.header.max_radius_fp16 += 1; }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.lods[0].lod.lod_threshold += 1; }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.lods.pop_back(); }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.lods[0].panm[0].flags ^= 1u; }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.lods[0].panm.pop_back(); }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.materials[0].material.alpha_test_value_byte ^= 1u; }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.materials.pop_back(); }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.lights[0].atten_start += 1.0f; }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.lights.pop_back(); }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.registers[0].name[0] ^= 1; }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.registers.pop_back(); }));
	TEST_EXPECT(!alike_after([](ModelRow &r) { r.frames.push_back(ThreediMatrix4x4{}); }));
	// A frame of another value: two rows given one frame each.
	const std::shared_ptr<ModelRow> framed = std::static_pointer_cast<ModelRow>(row->clone());
	ThreediMatrix4x4 frame{};
	threedi_mat4_identity(&frame);
	framed->frames.push_back(frame);
	const std::shared_ptr<ModelRow> moved = std::static_pointer_cast<ModelRow>(framed->clone());
	TEST_EXPECT(alike_but_user_points(*framed, *moved));
	moved->frames.back().m[12] += 1.0f;
	TEST_EXPECT(!alike_but_user_points(*framed, *moved));
	std::printf("alike_but_user_points: the user points alone alike; every other table compared\n");
	return 0;
}

int main() {
	if (edits() != 0) return 1;
	if (composed_forms() != 0) return 1;
	if (alike_but_user_points_tables() != 0) return 1;
	if (part_animations() != 0) return 1;
	if (validation() != 0) return 1;
	if (shipped_forms_validate() != 0) return 1;
	if (frames_and_registers() != 0) return 1;
	if (record_references() != 0) return 1;
	if (field_metadata() != 0) return 1;
	// The sweeps over the formats' fixtures, each naming every variant it lacks.
	const int saves = untouched_saves();
	const int migrated = bhd_migrated();
	return saves != 0 || migrated != 0 ? 1 : 0;
}
