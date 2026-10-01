// The model document (ADR 0046 S10) over the neutral core: every synthetic model loads
// and an untouched save writes its own bytes; field edits land in the written model
// (a material moved keeps its strips drawing with it); the structural edits and their
// refusals; undo back to the file's bytes; a save and a reload keep the edits; the
// validator's findings, a part animation's frame and a record's register read as the runtime
// reads them (S11h); (a SKIP-LEG without OPENNOVA_JO_ASSETS) every retail model saved
// untouched is its own bytes; and (a SKIP-LEG without OPENNOVA_JO_DIR) every model of the game
// install validated with no error (S11h).
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

#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <base/gameprofile/gameprofile.h>
#include <base/vfs/vfs.h>
#include <formats/threedi/threedi_3di3.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
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

std::vector<uint8_t> serialized(const Document &document) {
	const SerializeResult result = document.serialize();
	return std::vector<uint8_t>(result.text.begin(), result.text.end());
}

bool load(ModelDocument &document, const std::string &path, const std::string &name) {
	Diagnostic error;
	return document.load(path, name, AssetKind::Model, "jo", error);
}

Edit set(NodeAddress address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

Edit op(EditOperation operation, NodeAddress address, size_t position = SIZE_MAX, NodeId parent = 0) {
	Edit edit;
	edit.operation = operation;
	edit.address = address;
	edit.position = position;
	edit.parent = parent;
	return edit;
}

// The model validator over one model, as a project's validation runs it over each file.
std::vector<Diagnostic> validated(const std::shared_ptr<const ModelDocument> &document) {
	return document_type(DocumentTypeId::Model)->validate_file(*document);
}

int untouched_saves() {
	int models = 0;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(synth_dir(), ec)) {
		if (entry.path().extension() != ".3di") continue;
		ModelDocument document;
		TEST_EXPECT(load(document, entry.path().generic_string(), entry.path().filename().generic_string()));
		TEST_EXPECT(document.rows().size() == 2 && document.model_row() && document.collision_row());
		TEST_EXPECT(serialized(document) == test_io::read_file(entry.path().string()));
		++models;
	}
	TEST_EXPECT(models > 20);
	std::printf("untouched: %d synthetic models save as their own bytes\n", models);
	return 0;
}

int edits() {
	editor_test::TempProjectDir dir("opennova_model_document_test");
	const std::vector<uint8_t> original = test_io::read_file(synth_dir() + "/armory.3di");
	TEST_EXPECT(!original.empty() && editor_test::write_bytes(dir.file("armory.3di"), original));
	ModelDocument document;
	TEST_EXPECT(load(document, dir.file("armory.3di"), "armory.3di"));
	const ModelRow *row = document.model_row();
	TEST_EXPECT(row && row->materials.size() >= 2 && !row->lights.empty() && !row->user_points.empty());
	const NodeId model = row->id;
	Diagnostic error;

	// A light's colour and a user point's name, one undo step each.
	const NodeAddress light{model, kLight, row->collections[2][0]};
	TEST_EXPECT(document.apply(set(light, "start.r", int64_t(12)), error));
	const NodeAddress point{model, kUserPoint, row->collections[3][0]};
	TEST_EXPECT(document.apply(set(point, "name", std::string("muzzle")), error));
	Value value;
	TEST_EXPECT(document.get(point, "name", value) && std::get<std::string>(value) == "muzzle");
	TEST_EXPECT(document.dirty() && document.can_undo());

	// A material moved to the end: its strips draw with it at its new index.
	row = document.model_row();
	const std::string first_shader = row->materials[0].material.shader_name;
	const NodeAddress first{model, kMaterial, row->collections[1][0]};
	TEST_EXPECT(document.apply(op(EditOperation::Move, first, row->materials.size() - 1), error));
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
	TEST_EXPECT(!document.apply(op(EditOperation::Remove, {model, kMaterial, row->collections[1][0]}), error));
	TEST_EXPECT(document.apply(op(EditOperation::Add, {model, kMaterial, 0}), error));
	const NodeId added = document.last_added();
	TEST_EXPECT(document.apply(op(EditOperation::Add, {model, kTexture, 0}, SIZE_MAX, added), error));
	const NodeAddress texture{model, kTexture, document.last_added()};
	TEST_EXPECT(document.apply(set(texture, "name", std::string("newtex.tga")), error));
	TEST_EXPECT(document.get(texture, "name", value) && std::get<std::string>(value) == "newtex.tga");
	TEST_EXPECT(document.apply(op(EditOperation::Remove, {model, kMaterial, added}), error));

	// Registers: added at the end, the last removed; lights and user points freely.
	row = document.model_row();
	const size_t registers = row->registers.size();
	TEST_EXPECT(document.apply(op(EditOperation::Add, {model, kRegister, 0}), error));
	TEST_EXPECT(document.model_row()->registers.size() == registers + 1);
	TEST_EXPECT(document.apply(op(EditOperation::Remove, {model, kRegister, document.last_added()}), error));
	TEST_EXPECT(document.apply(op(EditOperation::Duplicate, light, 1), error));
	TEST_EXPECT(document.model_row()->lights.size() == row->lights.size() + 1);
	TEST_EXPECT(document.apply(op(EditOperation::Add, {model, kUserPoint, 0}), error));

	// The fixed records: the collision's are the geometry's; a row stays.
	const CollisionRow *collision = document.collision_row();
	if (!collision->volumes.empty()) {
		const NodeAddress volume{collision->id, kVolume, collision->collections[1][0]};
		TEST_EXPECT(!document.apply(op(EditOperation::Remove, volume), error));
		TEST_EXPECT(document.apply(set(volume, "flags", int64_t(0x4)), error));
	}
	TEST_EXPECT(!document.apply(op(EditOperation::Remove, {collision->id, node_kind(ModelKind::Collision), 0}), error));

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
	const NodeId lod_id = row->collections[0][0];
	Diagnostic error;
	if (lod.panm.size() < lod.lod.render_object_count) {
		TEST_EXPECT(document.apply(op(EditOperation::Add, {row->id, kPanm, 0}, SIZE_MAX, lod_id), error));
		TEST_EXPECT(document.model_row()->lods[0].panm.size() == lod.panm.size() + 1);
	} else {
		TEST_EXPECT(!document.apply(op(EditOperation::Add, {row->id, kPanm, 0}, SIZE_MAX, lod_id), error));
	}
	row = document.model_row();
	const std::vector<NodeId> &ids = row->lods[0].panm_ids;
	TEST_EXPECT(!ids.empty());
	if (ids.size() > 1) TEST_EXPECT(!document.apply(op(EditOperation::Remove, {row->id, kPanm, ids.front()}), error));
	const NodeAddress first{row->id, kPanm, ids.front()};
	TEST_EXPECT(document.apply(set(first, "rotx.rate", int64_t(-300)), error));
	Value value;
	TEST_EXPECT(document.get(first, "rotx.rate", value) && std::get<int64_t>(value) == -300);
	std::printf("part animations: appended in part order, tracks set\n");
	return 0;
}

// S11a: a light's colour edited is a change of the light, not of the model row, and reverts;
// a user point added is Added and changes the row that holds it.
int changes_since_save() {
	using Change = Document::RecordChange;
	ModelDocument document;
	TEST_EXPECT(load(document, synth_dir() + "/armory.3di", "armory.3di"));
	const ModelRow *row = document.model_row();
	TEST_EXPECT(row && !row->lights.empty());
	if (!row || row->lights.empty()) return 1;
	const NodeAddress model{row->id, node_kind(ModelKind::Model), 0};
	const NodeAddress light{row->id, kLight, row->collections[2][0]};
	Value red;
	TEST_EXPECT(document.get(light, "start.r", red));
	const int64_t was = std::get<int64_t>(red);
	Diagnostic error;
	TEST_EXPECT(document.apply(set(light, "start.r", int64_t(was == 12 ? 13 : 12)), error));
	TEST_EXPECT(document.field_changed(light, "start.r") && document.record_change(light) == Change::Changed);
	TEST_EXPECT(document.record_change(model) == Change::Unchanged);
	TEST_EXPECT(document.apply(document.revert_edits(light, "start.r"), error) && !document.field_changed(light, "start.r"));
	TEST_EXPECT(document.get(light, "start.r", red) && std::get<int64_t>(red) == was);
	TEST_EXPECT(document.record_change(light) == Change::Unchanged && serialized(document) == test_io::read_file(synth_dir() + "/armory.3di"));
	TEST_EXPECT(document.apply(op(EditOperation::Add, {row->id, kUserPoint, 0}), error));
	TEST_EXPECT(document.record_change({row->id, kUserPoint, document.last_added()}) == Change::Added &&
	            document.record_change(model) == Change::Changed);
	std::printf("changes: a light's field changed and reverted, a user point added\n");
	return 0;
}

int validation() {
	editor_test::TempProjectDir dir("opennova_model_validation_test");
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
	const NodeAddress light{row->id, kLight, row->collections[2][0]};
	TEST_EXPECT(document.apply(set(light, "part", int64_t(200)), error));
	for (int i = 0; i < 9; ++i) {
		TEST_EXPECT(document.apply(op(EditOperation::Add, {row->id, kUserPoint, 0}), error));
		TEST_EXPECT(document.apply(set({row->id, kUserPoint, document.last_added()}, "name",
		                               std::string("sitex") + std::to_string(i)), error));
	}
	TEST_EXPECT(editor_test::write_bytes(root + "/armory.3di", serialized(document)));
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
		TEST_EXPECT(document->apply(op(EditOperation::Add, {model, kFrame, 0}), error));
	const int64_t frames = static_cast<int64_t>(document->model_row()->frames.size());
	const NodeAddress panm{model, kPanm, document->model_row()->lods[0].panm_ids[0]};
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
		TEST_EXPECT(document->apply(set(panm, "flags.rotation", rotation), error));
		const bool reads = rotation == 1 || rotation == 2;
		TEST_EXPECT(document->field_on(panm, *matrix).applies == (reads ? Applicability::Reads : Applicability::Ignored));
		for (const int64_t byte : {int64_t(0), int64_t(128), int64_t(255), int64_t(192), frames - 1, frames, int64_t(127)}) {
			TEST_EXPECT(document->apply(set(panm, "matrix", byte), error));
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
	TEST_EXPECT(document->apply(set(panm, "flags.rotation", int64_t(2)), error));
	TEST_EXPECT(document->apply(set(panm, "matrix", frames - 1), error));
	const NodeAddress last{model, kFrame, document->model_row()->collections[5].back()};
	TEST_EXPECT(!document->apply(op(EditOperation::Remove, last), error));
	TEST_EXPECT(document->apply(set(panm, "matrix", int64_t(255)), error));
	TEST_EXPECT(document->apply(op(EditOperation::Remove, last), error));

	// The registers the records name, first with no CTRL table (the house has none): a style
	// that reads a register reads the global one its index numbers (a warning naming it; past
	// the 96, an error), another style above 0x70 takes the index as its phase, a light's does
	// not load, and a flipbook with no frames or a track the load does not copy names none.
	TEST_EXPECT(document->model_row()->registers.empty());
	const NodeAddress material{model, kMaterial, document->model_row()->collections[1][0]};
	const auto set_all = [&](const NodeAddress &at, std::initializer_list<std::pair<const char *, int64_t>> fields) {
		bool applied = true;
		for (const auto &field : fields) applied = document->apply(set(at, field.first, field.second), error) && applied;
		return applied;
	};
	TEST_EXPECT(set_all(material, {{"rgbgen.style", 0x71}, {"rgbgen.param", 0}, {"alphagen.style", 0x72},
	                               {"alphagen.param", 5}, {"ugen.style", 0x74}, {"ugen.param", 5}, {"vgen.style", 0x71},
	                               {"vgen.param", 200}, {"texanim.frames", 4}, {"texanim.type", 1}, {"texanim.time", 3}}));
	TEST_EXPECT(set_all(panm, {{"flags.rotation", 2}, {"flags.scale", 0}, {"rotx.style", 0x71}, {"rotx.param", 0},
	                           {"roty.style", 0x72}, {"roty.param", 7}, {"scalex.style", 0x71}, {"scalex.param", 9}}));
	TEST_EXPECT(document->apply(op(EditOperation::Add, {model, kLight, 0}), error));
	const NodeAddress light{model, kLight, document->last_added()};
	TEST_EXPECT(set_all(light, {{"style", 0x71}, {"param", 0}}));
	// The register findings of a model, by record and field.
	using Found = std::map<std::string, std::pair<DiagnosticSeverity, std::string>>;
	const auto registers_found = [&](const std::shared_ptr<const ModelDocument> &of) {
		Found out;
		for (const Diagnostic &d : validated(of)) {
			if (d.code() != "model.register_missing") continue;
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
	TEST_EXPECT(set_all(material, {{"texanim.frames", 0}}));
	TEST_EXPECT(registers_found(document).count("material texanim.time") == 0);
	TEST_EXPECT(set_all(material, {{"texanim.frames", 4}}));

	// A first register gives the model a table, which changes what every material's and loaded
	// track's register byte names (a global register now, an entry of the table then): refused,
	// nothing committed, while the game reads one (the X track's, met first), taken once none does.
	// The light's index, past the table the model lacks, stays past the one the register makes.
	const uint64_t tableless = document->revision();
	TEST_EXPECT(!document->apply(op(EditOperation::Add, {model, kRegister, 0}), error) &&
	            error.code() == "document.collection" &&
	            error.message.find(" rotx.param reads global register 0 now. Point it at no register first.") !=
	                    std::string::npos &&
	            document->revision() == tableless);
	TEST_EXPECT(set_all(material, {{"rgbgen.style", 0}, {"alphagen.style", 0}, {"ugen.style", 0}, {"vgen.style", 0},
	                               {"texanim.type", 0}}) &&
	            set_all(panm, {{"rotx.style", 0}, {"roty.style", 0}}));
	TEST_EXPECT(document->apply(op(EditOperation::Add, {model, kRegister, 0}), error));
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
	const NodeAddress light{model, kLight, row->collections[2][0]};
	const NodeAddress glow{model, kMaterial, row->collections[1][1]};
	const NodeAddress base{model, kPanm, row->lods[0].panm_ids[0]};
	const NodeAddress arm{model, kPanm, row->lods[0].panm_ids[1]};
	// What the validator finds of a register or a frame the model lacks.
	const auto lacking = [&]() {
		size_t found = 0;
		for (const Diagnostic &d : document_type(DocumentTypeId::Model)->validate_file(document))
			found += d.code() == "model.register_missing" || d.code() == "model.frame_missing";
		return found;
	};
	TEST_EXPECT(lacking() == 0);
	const auto reg = [&](size_t i) { return NodeAddress{model, kRegister, document.model_row()->collections[4][i]}; };
	const auto frame = [&](size_t i) { return NodeAddress{model, kFrame, document.model_row()->collections[5][i]}; };
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
	TEST_EXPECT(document.apply(op(EditOperation::Remove, reg(1)), error));
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
	TEST_EXPECT(!document.apply(op(EditOperation::Remove, reg(2)), error) && error.code() == "document.collection" &&
	            error.message == "rig/LOD 1/Part animation 2 roty.param reads this register: point it at another first." &&
	            document.revision() == revision && serialized(document) == original);
	// FLICKER moved to the front; a register added there; GUNYAW duplicated (its copy right after it).
	TEST_EXPECT(document.apply(op(EditOperation::Move, reg(3), 0), error) && written(3, 0, 0, 3, 2));
	document.undo();
	TEST_EXPECT(document.apply(op(EditOperation::Add, {model, kRegister, 0}, 0), error) && written(3, 4, 4, 3, 2));
	document.undo();
	TEST_EXPECT(document.apply(op(EditOperation::Duplicate, reg(1)), error) && written(3, 4, 4, 3, 2) &&
	            std::strcmp(document.model_row()->registers[2].name, "EWEAP_GUNYAW") == 0);
	document.undo();
	// A batch: the Remove, then the light pointed at register 0 as the Remove left them; one step.
	TEST_EXPECT(document.apply({op(EditOperation::Remove, reg(1)), set(light, "param", int64_t(0))}, error) &&
	            written(1, 0, 2, 1, 2));
	document.undo();
	TEST_EXPECT(serialized(document) == original && !document.can_undo());
	// An index past the table (the light's 4 of four) stays past it: across an Add at the front it is
	// 5, still a register the model lacks, never FLICKER sliding into its place.
	TEST_EXPECT(document.apply(set(light, "param", int64_t(4)), error) && lacking() == 1);
	TEST_EXPECT(document.apply(op(EditOperation::Add, {model, kRegister, 0}, 0), error) &&
	            value_of(light, "param") == 5 && lacking() == 1);
	document.undo();
	document.undo();
	TEST_EXPECT(serialized(document) == original);

	// Fields the game does not read: the glow's generator off its register and the arm's rotation off
	// (its tracks no longer loaded), so nothing the game reads names GUNPITCH. Its Remove is taken;
	// the unread Y track naming it keeps its byte, the unread X track's FLICKER moves down with the
	// light's. Removing GUNYAW renumbers the unread tracks as it does the read ones.
	TEST_EXPECT(document.apply({set(glow, "rgbgen.style", int64_t(0)), set(arm, "flags.rotation", int64_t(0))}, error));
	TEST_EXPECT(references(document) == (std::map<std::string, std::string>{{"light param", "3"}}));
	TEST_EXPECT(document.apply(op(EditOperation::Remove, reg(2)), error) && value_of(arm, "roty.param") == 2 &&
	            value_of(arm, "rotx.param") == 2 && value_of(light, "param") == 2);
	document.undo();
	TEST_EXPECT(document.apply(op(EditOperation::Remove, reg(1)), error) && value_of(arm, "roty.param") == 1 &&
	            value_of(arm, "rotx.param") == 2 && value_of(light, "param") == 2);
	document.undo();
	document.undo();
	TEST_EXPECT(serialized(document) == original);

	// A field cannot be given an index it cannot hold: the glow's byte on register 255 of 256, a
	// register added at the front would make it 256. Refused, nothing committed.
	std::vector<Edit> more;
	for (int i = 0; i < 252; ++i) more.push_back(op(EditOperation::Add, {model, kRegister, 0}));
	TEST_EXPECT(document.apply(more, error) && document.model_row()->registers.size() == 256);
	TEST_EXPECT(document.apply(set(glow, "rgbgen.param", int64_t(255)), error));
	const uint64_t full = document.revision();
	TEST_EXPECT(!document.apply(op(EditOperation::Add, {model, kRegister, 0}, 0), error) &&
	            error.code() == "document.collection" &&
	            error.message == "Register 255 would move to 256, which rig/FF_ST_AD_LUM rgbgen.param cannot hold." &&
	            document.revision() == full);
	document.undo();
	document.undo();
	TEST_EXPECT(serialized(document) == original);

	// Frames: one nothing names removed moves the arm's frame; the one it turns through is refused;
	// one added, one moved and one duplicated before it move it again.
	TEST_EXPECT(document.apply(op(EditOperation::Remove, frame(1)), error) && written(2, 3, 3, 2, 1));
	document.undo();
	TEST_EXPECT(!document.apply(op(EditOperation::Remove, frame(2)), error) &&
	            error.message == "rig/LOD 1/Part animation 2 matrix turns through this rotation frame: point it at another "
	                             "first.");
	TEST_EXPECT(document.apply(op(EditOperation::Add, {model, kFrame, 0}, 1), error) && written(2, 3, 3, 2, 3));
	document.undo();
	TEST_EXPECT(document.apply(op(EditOperation::Move, frame(2), 1), error) && written(2, 3, 3, 2, 1));
	document.undo();
	TEST_EXPECT(document.apply(op(EditOperation::Duplicate, frame(1)), error) && written(2, 3, 3, 2, 3) &&
	            document.model_row()->frames.size() == 4);
	document.undo();
	// Row 0 is never read (a frame byte of 0 names none): it takes a Remove, an Add before it, a Move;
	// but a byte the game reads never moves onto it.
	TEST_EXPECT(document.apply(op(EditOperation::Remove, frame(0)), error) && written(2, 3, 3, 2, 1));
	document.undo();
	TEST_EXPECT(document.apply(op(EditOperation::Add, {model, kFrame, 0}, 0), error) && written(2, 3, 3, 2, 3));
	document.undo();
	TEST_EXPECT(!document.apply(op(EditOperation::Move, frame(2), 0), error) &&
	            error.message == "Rotation frame 2 would move to 0, which rig/LOD 1/Part animation 2 matrix cannot "
	                             "name (a frame byte names rows 1 to 127)." &&
	            serialized(document) == original);
	// A byte naming no row (0, 255) stays through a shift; one on a row that turns through no frame
	// (the base's) is renumbered all the same.
	for (const int64_t none : {int64_t(0), int64_t(255)}) {
		TEST_EXPECT(document.apply({set(base, "flags.rotation", int64_t(2)), set(base, "matrix", none)}, error));
		TEST_EXPECT(document.apply(op(EditOperation::Add, {model, kFrame, 0}, 1), error) &&
		            value_of(base, "matrix") == none && written(2, 3, 3, 2, 3));
		document.undo();
		document.undo();
	}
	TEST_EXPECT(document.apply(set(base, "matrix", int64_t(2)), error));
	TEST_EXPECT(document.apply(op(EditOperation::Add, {model, kFrame, 0}, 1), error) &&
	            value_of(base, "matrix") == 3 && written(2, 3, 3, 2, 3));
	document.undo();
	document.undo();
	// A frame byte names rows 1 to 127: on row 127 of 128, a frame added before it is refused.
	std::vector<Edit> rows;
	for (int i = 0; i < 125; ++i) rows.push_back(op(EditOperation::Add, {model, kFrame, 0}));
	TEST_EXPECT(document.apply(rows, error) && document.model_row()->frames.size() == 128);
	TEST_EXPECT(document.apply(set(arm, "matrix", int64_t(127)), error));
	const uint64_t rows_full = document.revision();
	TEST_EXPECT(!document.apply(op(EditOperation::Add, {model, kFrame, 0}, 1), error) &&
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
	const NodeAddress gunyaw{second.model_row()->id, kRegister, second.model_row()->collections[4][1]};
	TEST_EXPECT(!second.apply(op(EditOperation::Remove, gunyaw), error) &&
	            error.message == "A material's second RGB generator names register 3, which the editor shows and "
	                             "never sets: that register stays where it is.");
	TEST_EXPECT(second.apply(op(EditOperation::Add, {second.model_row()->id, kRegister, 0}), error));
	TEST_EXPECT(references(second).count("material rgbgen2.param") == 1 &&
	            references(second).at("material rgbgen2.param") == "3");
	const NodeAddress paint{second.model_row()->id, kMaterial, second.model_row()->collections[1][0]};
	for (const FieldSchema &field : second.fields(kMaterial))
		if (field.id == "rgbgen2.param") {
			const FieldUse use = second.field_on(paint, field);
			TEST_EXPECT(use.reference == ReferenceKind::ModelRegister && use.read_only);
		}
	std::printf("record references: registers and frames renumbered with the edit that moves them\n");
	return 0;
}

int retail_models() {
	const std::string root = retail::assets();
	if (!retail::dir_exists(root)) return retail::skip_leg("OPENNOVA_JO_ASSETS (the retail models at its root)");
	int models = 0;
	std::error_code ec;
	for (const auto &entry : fs::directory_iterator(root, ec)) {
		if (!entry.is_regular_file(ec) || retail::lower_ascii(entry.path().extension().string()) != ".3di") continue;
		ModelDocument document;
		TEST_EXPECT(load(document, entry.path().generic_string(), entry.path().filename().generic_string()));
		TEST_EXPECT(serialized(document) == test_io::read_file(entry.path().string()));
		++models;
	}
	if (models == 0) return retail::skip_leg("OPENNOVA_JO_ASSETS with the retail .3di models at its root");
	std::printf("retail: %d models open and save untouched as their own bytes\n", models);
	return 0;
}

// S11h: a retail model loads in the game, so the validator finds no error in one. Every model
// the game install serves (the base game's archives and each expansion's, each file once) is
// opened from its bytes and validated as each model of a project is (validate_model_file); every
// error is listed with its model, record, code and message before the leg fails. A file the
// game's model loader refuses is no model it loads, and is counted apart: one whose chunk tag is
// not 3DI3 (an install may carry GP-era GPM files among its models) [orig:
// ThreediGp_LoadFromFile @ 0x5B5806, CChunkFile_LoadFromFile with the tag 0x33494433].
int retail_validation() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every model of the game install validated)");
	const ProjectDocument project;
	std::vector<std::string> expansions = opennova::vfs_list_expansions(root);
	expansions.insert(expansions.begin(), std::string()); // the base game first
	std::set<std::string> seen;
	size_t models = 0, errors = 0, refused = 0, part_animations = 0, no_frame = 0;
	std::map<std::string, size_t> notes; // the warnings and notes, by code
	for (const std::string &expansion : expansions) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::gameprofile::gameprofile_scr_policy_for_code(project.target_game.c_str()));
		TEST_EXPECT(game.mount_game(root, expansion) && game.has_mounted_archive());
		for (const opennova::VfsFileLocation &file : game.list_files()) {
			if (retail::lower_ascii(fs::path(file.logical_name).extension().string()) != ".3di") continue;
			if (!seen.insert(file.source_path + "|" + normalized_logical_name(file.logical_name)).second) continue;
			std::vector<uint8_t> bytes;
			TEST_EXPECT(game.read_file(file.logical_name, bytes));
			if (bytes.size() < 4 || std::memcmp(bytes.data(), "3DI3", 4) != 0) {
				std::printf("retail validation: %s (%s) is no 3DI3 model: the game's loader refuses it too\n",
				            file.logical_name.c_str(), file.source_path.c_str());
				++refused;
				continue;
			}
			auto document = std::make_shared<ModelDocument>();
			Diagnostic error;
			if (!document->load_bytes(bytes, file.logical_name, AssetKind::Model, project.target_game, error)) {
				std::printf("retail validation: %s (%s) does not open: %s\n", file.logical_name.c_str(),
				            file.source_path.c_str(), error.message.c_str());
				++errors;
				continue;
			}
			for (const Diagnostic &d : validated(document)) {
				if (d.severity != DiagnosticSeverity::Error) {
					++notes[d.code()];
					continue;
				}
				std::printf("retail validation: %s (%s) %s: %s: %s\n", file.logical_name.c_str(), file.source_path.c_str(),
				            d.record.c_str(), d.code().c_str(), d.message.c_str());
				++errors;
			}
			for (const ModelLod &lod : document->model_row()->lods)
				for (const ThreediPartAnimation &pa : lod.panm) {
					++part_animations;
					no_frame += pa.matrix_index >= 0x80 ? 1 : 0;
				}
			++models;
		}
	}
	if (models == 0) return retail::skip_leg("OPENNOVA_JO_DIR with the game's models in its archives");
	std::printf("retail validation: %zu models, %zu errors (%zu files no model); %zu part animations, %zu of them a "
	            "frame byte 0x80 or above (no frame)\n",
	            models, errors, refused, part_animations, no_frame);
	for (const auto &note : notes) std::printf("retail validation: %zu x %s\n", note.second, note.first.c_str());
	TEST_EXPECT(errors == 0);
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
	TEST_EXPECT(document->apply(op(EditOperation::Add, {model, kLight, 0}), error));
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
	TEST_EXPECT(!document->apply(set(light, "unknown1", int64_t(1)), error));
	// The light's part: LOD 0's parts, any other index typed.
	const FieldSchema part = schema(light, "part");
	const size_t parts = row->base->lods[0].render_object_count;
	TEST_EXPECT(part.open_choices && part.choices.size() == parts && parts > 0 && part.choices[0].label == "Part 0");
	// A part animation's parent: none (255) and the parts, its record's own choices.
	const NodeAddress panm{model, kPanm, row->lods[0].panm_ids[0]};
	const FieldSchema parent = schema(panm, "parent");
	TEST_EXPECT(parent.choices.size() == parts + 1 && parent.choices[0].value == 255 && parent.choices[0].label == "None");
	while (document->model_row()->frames.size() < 3)
		TEST_EXPECT(document->apply(op(EditOperation::Add, {model, kFrame, 0}), error));
	TEST_EXPECT(document->apply(set(panm, "flags.rotation", int64_t(2)), error));
	// Its frame byte names an MTRX row once its rotation turns through one, whatever the byte names
	// now: the picker offers the rows above 0 (0 names none); a row that turns through no frame (a
	// billboard) names none.
	for (const int64_t from : {int64_t(0), int64_t(255), int64_t(1)}) {
		TEST_EXPECT(document->apply(set(panm, "matrix", from), error));
		const FieldUse matrix = use_of(panm, "matrix");
		const std::vector<ReferenceChoice> rows = picker(panm, "matrix");
		TEST_EXPECT(matrix.reference == ReferenceKind::ModelFrame && !matrix.own_choices &&
		            !matrix.schema->open_choices && schema(panm, "matrix").choices.empty());
		TEST_EXPECT(rows.size() == 2 && rows[0].name == "1" && rows[1].name == "2" &&
		            rows[1].status == ReferenceStatus::Present && rows[1].file == "house.3di");
		TEST_EXPECT(document->apply(set(panm, "matrix", int64_t(2)), error));
	}
	TEST_EXPECT(document->apply(set(panm, "flags.rotation", int64_t(3)), error));
	TEST_EXPECT(use_of(panm, "matrix").reference == ReferenceKind::None && picker(panm, "matrix").empty());
	TEST_EXPECT(document->apply(set(panm, "flags.rotation", int64_t(2)), error));
	// With no CTRL table the load leaves a material's and a track's register bytes as the global
	// registers they number (no record of the model); a light's it swaps through the table it lacks.
	TEST_EXPECT(document->model_row()->registers.empty());
	const NodeAddress first_material{model, kMaterial, document->model_row()->collections[1][0]};
	TEST_EXPECT(document->apply(
	        {set(first_material, "rgbgen.style", int64_t(0x71)), set(light, "style", int64_t(0x71))}, error));
	TEST_EXPECT(use_of(first_material, "rgbgen.param").reference == ReferenceKind::None &&
	            use_of(light, "param").reference == ReferenceKind::ModelRegister);
	document->undo();
	// A generator's parameter names a register above style 0x70: the picker offers the model's
	// registers by index, each with its name in its record and as its label.
	TEST_EXPECT(document->apply(op(EditOperation::Add, {model, kRegister, 0}), error));
	const NodeAddress reg{model, kRegister, document->last_added()};
	TEST_EXPECT(document->apply(set(reg, "name", std::string("ENGINE_RPM")), error));
	const NodeAddress material{model, kMaterial, document->model_row()->collections[1][0]};
	TEST_EXPECT(document->apply(set(material, "rgbgen.style", int64_t(0x71)), error));
	const std::vector<ReferenceChoice> named = picker(material, "rgbgen.param");
	TEST_EXPECT(use_of(material, "rgbgen.param").reference == ReferenceKind::ModelRegister &&
	            schema(material, "rgbgen.param").choices.empty());
	TEST_EXPECT(named.size() == 1 && named[0].name == "0" && named[0].kind == ReferenceKind::ModelRegister &&
	            named[0].record.size() >= 11 && named[0].record.substr(named[0].record.size() - 11) == "/ENGINE_RPM" &&
	            named[0].label == "ENGINE_RPM");
	TEST_EXPECT(document->apply(set(material, "rgbgen.style", int64_t(0)), error));
	TEST_EXPECT(use_of(material, "rgbgen.param").reference == ReferenceKind::None);
	// Only a register the field can hold is offered: with 257 of them, the byte-sized
	// generator parameter takes registers 0..255, the 16-bit flipbook time all 257.
	while (document->model_row()->registers.size() < 257)
		TEST_EXPECT(document->apply(op(EditOperation::Add, {model, kRegister, 0}), error));
	TEST_EXPECT(document->apply(set(material, "rgbgen.style", int64_t(0x71)), error));
	const std::vector<ReferenceChoice> byte_param = picker(material, "rgbgen.param");
	TEST_EXPECT(byte_param.size() == 256 && byte_param.back().name == "255");
	TEST_EXPECT(document->apply(set(material, "rgbgen.param", int64_t(255)), error));
	TEST_EXPECT(document->apply({set(material, "texanim.frames", int64_t(4)), set(material, "texanim.type", int64_t(1))}, error));
	const std::vector<ReferenceChoice> time = picker(material, "texanim.time");
	TEST_EXPECT(time.size() == 257 && time.back().name == "256");
	TEST_EXPECT(document->apply(set(material, "rgbgen.style", int64_t(0)), error));
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
	const NodeAddress frame{model, kFrame, document->model_row()->collections[5][0]};
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

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (untouched_saves() != 0) return 1;
	if (edits() != 0) return 1;
	if (part_animations() != 0) return 1;
	if (changes_since_save() != 0) return 1;
	if (validation() != 0) return 1;
	if (frames_and_registers() != 0) return 1;
	if (record_references() != 0) return 1;
	if (field_metadata() != 0) return 1;
	if (retail_models() != 0) return 1;
	return retail_validation();
}
