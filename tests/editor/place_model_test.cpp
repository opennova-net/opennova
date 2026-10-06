// Place a model no item draws yet (ADR 0046 DI-12, editor/preview/model_placement, editor/documents/model_item)
// over a real session, a project holding the minted mission (fixtures/bms/synth_logic.bms), an items.def and the
// synth models: what item a model is drawn as from its parts (a decoration; a building by its occlusion or a
// blink box; a person, a vehicle and a mounted gun the editor does not make; the doors its registers turn); a
// model no item draws let go over the mission's picture making its item in items.def (the next free id, past
// the ids the engine keeps and those the project names; its TYPE, its graphic, a name no item has) and placing
// it in the pool its TYPE puts it in, an undo step in each file, said on the status line and kept among the
// recently placed, the item saved as items.def writes it; the refusals, nothing written (a person, a vehicle, a
// mounted gun, a model several items draw), and an entity never placed once its item's batch is refused; a
// model's Place in mission arming the Place tool of the mission last active with its item, made first where
// none draws it, refusing with no mission open and with several items drawing it. The retail leg (--retail,
// OPENNOVA_JO_DIR): the rule over every model the shipped ITEMS.DEF draws against the TYPE its row gives it, and a
// shipped building and decoration dropped, each made its row's TYPE.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/def_table.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/model_item.h>
#include <editor/graph/asset_graph.h>
#include <editor/preview/mission_viewport.h>
#include <editor/preview/model_placement.h>
#include <editor/preview/viewports.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/def/reserved_items.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_ctrl_catalog.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova::editor;
namespace threedi = opennova::threedi;
namespace def = opennova::def;

namespace {

using editor_test::FakeDevices;
using editor_test::NoProcess;

constexpr const char *kMission = "missions/synth_logic.bms";

// The pump drawn by two items, the armory by one; an item named "house" (drawing the shed) that a house's item
// keeps clear of; id 100001 taken.
constexpr const char *kItems = "begin \"Null\"\nid 100000\ntype marker\nend\n"
                               "begin \"Drop Pump\"\nid 106100\ntype object\ngraphic pump\nend\n"
                               "begin \"Drop Scaled Pump\"\nid 106103\ntype object\ngraphic pump\nend\n"
                               "begin \"Drop Armory\"\nid 106101\ntype building\ngraphic armory\nend\n"
                               "begin \"house\"\nid 100001\ntype decoration\ngraphic shed\nend\n";

std::string fixture(const std::string &rel) { return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + rel; }

// A synth model read, `change` applied to it in memory (null: as minted).
template <typename Change> ModelItemFacts facts_of(const char *name, Change change) {
	const std::vector<uint8_t> bytes = test_io::read_file(fixture(std::string("threedi/synth/") + name + ".3di"));
	threedi::Threedi3di3 model{};
	ModelItemFacts facts;
	facts.because = "unread";
	if (threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0) {
		change(model);
		facts = model_item_facts(model);
	}
	threedi::threedi_3di3_free(&model);
	return facts;
}
ModelItemFacts facts_of(const char *name) {
	return facts_of(name, [](threedi::Threedi3di3 &) {});
}

struct Rig {
	editor_test::TempProjectDir dir;
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	FakeDevices devices;
	std::string catalog; // items.def's project path
	std::string mission; // the mission's path as opened

	explicit Rig(const char *name) : dir(name) {}

	bool open(bool open_mission = true) {
		session.handle(request::new_project(dir.file("project"), "Place a model"));
		session.run_operations();
		editor_test::create_missing_files(session);
		const SessionView &view = session.view();
		const AssetEntry *items = view.project.scan ? view.project.scan->find("items.def") : nullptr;
		if (!items) return false;
		catalog = items->relative_path;
		if (!editor_test::write_text(view.project.root + "/" + catalog, kItems)) return false;
		if (!editor_test::write_bytes(view.project.root + "/" + kMission, test_io::read_file(fixture("bms/synth_logic.bms"))))
			return false;
		for (const char *model : { "crate", "armory", "pump", "house", "shed", "person", "carrier", "mount" })
			if (!editor_test::write_bytes(view.project.root + "/models/" + model + ".3di",
			                              test_io::read_file(fixture(std::string("threedi/synth/") + model + ".3di"))))
				return false;
		session.handle(request::rescan());
		session.run_operations();
		if (!open_mission) return true;
		session.handle(request::open_document(kMission));
		if (!session.outcome().done()) return false;
		mission = session.document_for(kMission)->path();
		devices.sync(session);
		return true;
	}
	const MissionDocument &missions() { return static_cast<const MissionDocument &>(*session.document_for(kMission)); }
	// The entities of the mission's pool `kind` that place `item`.
	size_t placing(MissionKind kind, int64_t item) {
		const MissionDocument &document = missions();
		size_t count = 0;
		for (const Node *row : document.rows_of(kind)) {
			Value value;
			if (document.get({ row->id, row->kind, 0 }, "item", value) && std::get_if<int64_t>(&value) &&
			    std::get<int64_t>(value) == item)
				++count;
		}
		return count;
	}
	// The catalog's item named `name` (null: none, or items.def not open), its fields read into `out`.
	bool item_named(const std::string &name, int64_t &id, int64_t &type, std::string &graphic) {
		const Document *document = session.document_for(catalog);
		if (!document) return false;
		for (const auto &row : document->rows()) {
			if (row->name() != name) continue;
			Value value;
			const NodeAddress at{ row->id, row->kind, 0 };
			if (document->get(at, "id", value)) id = std::get<int64_t>(value);
			if (document->get(at, "type", value)) type = std::get<int64_t>(value);
			if (document->get(at, "graphic", value)) graphic = std::get<std::string>(value);
			return true;
		}
		return false;
	}
	// A model let go at the middle of the mission's picture.
	void drop(const char *file) {
		const auto *viewport = static_cast<const MissionViewport *>(session.viewports().find(mission, ViewportKind::Mission));
		ViewportDrop drop;
		drop.file = file;
		drop.kind = ViewportKind::Mission;
		if (viewport) {
			const ViewportContext context = viewport_context(session.view(), *viewport);
			drop.x = float(context.width) * 0.5f;
			drop.y = float(context.height) * 0.5f;
		}
		session.handle(request::edit_in_viewport(mission, drop));
	}
};

// The lowest id from 100000 the engine keeps none of, no item of the project has and no file names, as the
// project's graph says (the test's own walk, beside free_project_item_id's).
int64_t lowest_free_id(const SessionView &view) {
	std::set<int64_t> used;
	for (const GraphSymbol *symbol : view.findings.graph->symbols_of_kind(ReferenceKind::Item))
		if (const auto id = opennova::strutil::parse_int(symbol->name)) used.insert(*id);
	view.findings.graph->for_each_edge([&](const GraphEdge &edge) {
		if (edge.kind == ReferenceKind::Item)
			if (const auto id = opennova::strutil::parse_int(edge.value)) used.insert(*id);
	});
	for (int64_t id = def::DEF_ITEM_ID_BASE;; ++id)
		if (!def::reserved_item_by_id(int(id)) && !used.count(id)) return id;
}

std::string refusal(ProjectSession &session) {
	return session.outcome().findings.empty() ? std::string() : session.outcome().findings.front().message;
}

} // namespace

// What item a model is drawn as from its parts: the plain props decorations (type 2); the armory a building
// (type 5) by its occlusion and its blink boxes, and a crate given one blink box a building too; the person and
// the bird (skinned), the carrier and the tank (seats, a driver's place) and the mount (a UseGun point) not made,
// each saying why; a part turned by DOOR_00 counted as a door.
static int test_model_item_facts() {
	for (const char *prop : { "crate", "house", "pump", "shed", "gun" }) {
		const ModelItemFacts facts = facts_of(prop);
		TEST_EXPECT(facts.kind == ModelItemKind::Decoration && facts.type == def::DEF_ITEM_TYPE_DECORATION && facts.makes &&
		            facts.doors == 0);
	}
	ModelItemFacts facts = facts_of("armory");
	TEST_EXPECT(facts.kind == ModelItemKind::Building && facts.type == def::DEF_ITEM_TYPE_BUILDING && facts.makes);
	TEST_EXPECT(facts.because.find("its occlusion, 8 objects, and") != std::string::npos &&
	            facts.because.find("blink box") != std::string::npos);
	TEST_EXPECT(model_item_words(facts).rfind("a building (", 0) == 0);
	facts = facts_of("crate", [](threedi::Threedi3di3 &model) {
		if (model.collision && model.collision->volume_count) model.collision->volumes[0].collidable_type = 8;
	});
	TEST_EXPECT(facts.kind == ModelItemKind::Building && facts.because == "its 1 blink box");
	for (const char *person : { "person", "bird" }) {
		facts = facts_of(person);
		TEST_EXPECT(facts.kind == ModelItemKind::Person && !facts.makes && facts.type == def::DEF_ITEM_TYPE_PERSON);
	}
	facts = facts_of("carrier");
	TEST_EXPECT(facts.kind == ModelItemKind::Vehicle && !facts.makes && facts.because.find("ctrlx13") != std::string::npos);
	facts = facts_of("tank");
	TEST_EXPECT(facts.kind == ModelItemKind::Vehicle && !facts.makes);
	facts = facts_of("mount");
	TEST_EXPECT(facts.kind == ModelItemKind::MountedWeapon && !facts.makes && facts.because.find("Usegun") != std::string::npos);
	// A part a register DOOR_00 turns: a door, which the item made leaves shut.
	facts = facts_of("house_lod0_sine_rotx", [](threedi::Threedi3di3 &model) {
		auto *registers = static_cast<threedi::ThreediControlRegister *>(std::calloc(1, sizeof(threedi::ThreediControlRegister)));
		std::snprintf(registers[0].name, sizeof(registers[0].name), "DOOR_00");
		std::free(model.ctrl.registers);
		model.ctrl.registers = registers;
		model.ctrl.count = 1;
		threedi::ThreediLod &lod = model.lods[0];
		for (size_t i = 0; i < lod.part_animation_count; ++i)
			if (lod.part_animations[i].rotation_x.control != 0) {
				lod.part_animations[i].rotation_x.control = 113; // SET_CONTROL_REGISTER
				lod.part_animations[i].rotation_x.control_param = 0;
			}
	});
	TEST_EXPECT(facts.kind == ModelItemKind::Decoration && facts.doors == 1);
	std::printf("test_model_item_facts passed\n");
	return 0;
}

// A model no item draws let go over the mission's picture: its item made in items.def (opened first) on the next
// free id with its TYPE and graphic, a name no item has, then placed in the pool its TYPE puts it in; the status
// line saying both, the item first among the recently placed; Undo of the mission takes the entity back, Undo of
// items.def the item; saved, items.def writes the item as the game reads it; the item then draws the model (a
// second drop places it, makes none).
static int test_drop_makes_item() {
	Rig rig("opennova_editor_place_model_drop");
	TEST_EXPECT(rig.open());
	if (rig.mission.empty()) return 1;
	const SessionView &view = rig.session.view();
	TEST_EXPECT(mission_items_of_model(view, "models/crate.3di").empty());
	const int64_t expected = lowest_free_id(view);
	TEST_EXPECT(expected >= 100002 && free_project_item_id(view, rig.catalog) == expected);
	const std::string before = rig.session.document_for(kMission)->serialize().text;
	const size_t buildings = rig.missions().rows_of(MissionKind::Building).size();

	rig.drop("crate.3di");
	TEST_EXPECT(rig.session.outcome().done());
	int64_t id = 0, type = 0;
	std::string graphic;
	TEST_EXPECT(rig.item_named("crate", id, type, graphic) && id == expected && type == def::DEF_ITEM_TYPE_DECORATION &&
	            graphic == "crate");
	TEST_EXPECT(rig.missions().rows_of(MissionKind::Building).size() == buildings + 1 &&
	            rig.placing(MissionKind::Building, expected) == 1);
	TEST_EXPECT(view.documents.active == rig.mission);
	const std::string &status = view.activity.status;
	TEST_EXPECT(status.find("Made item crate (" + std::to_string(expected) + "), a decoration") != std::string::npos &&
	            status.find("items.def") != std::string::npos && status.find("synth_logic.bms") != std::string::npos);
	TEST_EXPECT(!view.project.recent_items.empty() && view.project.recent_items.front() == expected);
	// Undo of the mission takes the entity back; of items.def the item.
	rig.session.handle(request::undo(kMission));
	TEST_EXPECT(rig.session.outcome().done() && rig.session.document_for(kMission)->serialize().text == before);
	rig.session.handle(request::undo(rig.catalog));
	TEST_EXPECT(rig.session.outcome().done() && !rig.item_named("crate", id, type, graphic));
	rig.session.handle(request::redo(rig.catalog));
	rig.session.handle(request::redo(kMission));
	TEST_EXPECT(rig.placing(MissionKind::Building, expected) == 1);

	// Saved: the item as items.def writes it, read back by the game's parser.
	rig.session.handle(request::save(rig.catalog));
	TEST_EXPECT(rig.session.outcome().done());
	const std::vector<uint8_t> saved = test_io::read_file(view.project.root + "/" + rig.catalog);
	def::DefItemsFile items{};
	bool found = false;
	if (def::def_parse_items_memory(saved.data(), saved.size(), &items) == 0)
		for (size_t i = 0; i < items.count; ++i)
			if (items.entries[i].id == expected) {
				found = std::string(items.entries[i].display_name) == "crate" && items.entries[i].type == def::DEF_ITEM_TYPE_DECORATION &&
				        std::string(items.entries[i].graphic) == "crate" && items.entries[i].hp == 0 &&
				        items.entries[i].ai_function[0] == '\0';
			}
	def::def_free_items(&items);
	TEST_EXPECT(found);

	// The armory's twin with no item: a building by its occlusion; the house's: a decoration named "house 2".
	TEST_EXPECT(editor_test::write_bytes(view.project.root + "/models/bunker.3di", test_io::read_file(fixture("threedi/synth/armory.3di"))));
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	rig.devices.sync(rig.session);
	const int64_t bunker = lowest_free_id(view);
	rig.drop("bunker.3di");
	TEST_EXPECT(rig.session.outcome().done() && rig.item_named("bunker", id, type, graphic) && id == bunker &&
	            type == def::DEF_ITEM_TYPE_BUILDING && rig.placing(MissionKind::Building, bunker) == 1);
	TEST_EXPECT(view.activity.status.find("a building (its occlusion, ") != std::string::npos);
	rig.session.run_operations();
	rig.drop("house.3di");
	TEST_EXPECT(rig.session.outcome().done() && rig.item_named("house 2", id, type, graphic) && graphic == "house");

	// The graph follows the unsaved catalog: the item draws the model, and a second drop places it, making none.
	rig.session.run_operations();
	TEST_EXPECT(mission_items_of_model(view, "models/crate.3di") == std::vector<int64_t>({ expected }));
	const size_t rows = rig.session.document_for(rig.catalog)->rows().size();
	rig.drop("crate.3di");
	TEST_EXPECT(rig.session.outcome().done() && rig.placing(MissionKind::Building, expected) == 2 &&
	            rig.session.document_for(rig.catalog)->rows().size() == rows);
	std::printf("test_drop_makes_item passed\n");
	return 0;
}

// Refused, nothing written: a person's model, a vehicle's, a mounted gun's (each saying what its item needs), a
// model two items draw (naming both); an entity never placed once its item's batch is refused (a catalog that
// does not read: its document cannot open).
static int test_drop_refusals() {
	Rig rig("opennova_editor_place_model_refusals");
	TEST_EXPECT(rig.open());
	if (rig.mission.empty()) return 1;
	const std::string before = rig.session.document_for(kMission)->serialize().text;
	for (const auto &[file, words] : { std::pair<const char *, const char *>{ "person.3di", "a person" },
	                                   { "carrier.3di", "a vehicle" }, { "mount.3di", "a mounted weapon" } }) {
		rig.drop(file);
		TEST_EXPECT(rig.session.outcome().refused && refusal(rig.session).find(words) != std::string::npos);
		TEST_EXPECT(rig.session.document_for(rig.catalog) == nullptr);
	}
	// What to add says the graphic as items.def names it.
	rig.drop("person.3di");
	TEST_EXPECT(refusal(rig.session).find("(Add record, graphic person)") != std::string::npos);
	rig.drop("pump.3di");
	TEST_EXPECT(rig.session.outcome().refused && refusal(rig.session).find("Drop Pump (106100)") != std::string::npos &&
	            refusal(rig.session).find("Drop Scaled Pump (106103)") != std::string::npos);
	TEST_EXPECT(rig.session.document_for(kMission)->serialize().text == before);

	// items.def that no longer reads: the item's batch refused, so the entity is not placed.
	const SessionView &view = rig.session.view();
	TEST_EXPECT(editor_test::write_text(view.project.root + "/" + rig.catalog, "begin \"Broken\"\nid 100000\ntype marker\n"));
	rig.session.handle(request::rescan());
	rig.session.run_operations();
	rig.devices.sync(rig.session);
	rig.drop("crate.3di");
	TEST_EXPECT(rig.session.outcome().refused);
	TEST_EXPECT(rig.session.document_for(kMission)->serialize().text == before);
	std::printf("test_drop_refusals passed\n");
	return 0;
}

// A model's Place in mission (the model viewport's place_in_mission command): with the mission open before the
// model, its item made in items.def, the mission made active and its Place tool armed with the item, said on
// the status line; a model one item draws armed with that item, nothing made; refused with ids, with several
// items drawing the model, and with no mission open.
static int test_place_in_mission() {
	Rig rig("opennova_editor_place_model_command");
	TEST_EXPECT(rig.open());
	if (rig.mission.empty()) return 1;
	const SessionView &view = rig.session.view();
	const auto command = [&](const std::string &model, std::vector<NodeId> ids = {}) {
		ViewportCommand place;
		place.name = "place_in_mission";
		place.kind = ViewportKind::Model;
		place.ids = std::move(ids);
		rig.session.handle(request::edit_in_viewport(model, place));
	};
	const auto armed = [&](int64_t item) {
		const auto *viewport = static_cast<const MissionViewport *>(rig.session.viewports().find(rig.mission, ViewportKind::Mission));
		return viewport && viewport->options().tool == MissionTool::Place && viewport->options().item == item &&
		       view.documents.active == rig.mission;
	};
	rig.session.handle(request::open_document("models/crate.3di"));
	TEST_EXPECT(rig.session.outcome().done() && view.documents.active == "models/crate.3di");
	rig.devices.sync(rig.session);
	TEST_EXPECT(place_in_mission_target(view) == rig.mission);
	const int64_t crate = lowest_free_id(view);
	command("models/crate.3di");
	int64_t id = 0, type = 0;
	std::string graphic;
	TEST_EXPECT(rig.session.outcome().done() && rig.item_named("crate", id, type, graphic) && id == crate && armed(crate));
	TEST_EXPECT(view.activity.status.find("Made item crate") != std::string::npos &&
	            view.activity.status.find("Place is armed in synth_logic.bms") != std::string::npos);
	// The armory, which Drop Armory draws: armed with it, no item made.
	rig.session.handle(request::open_document("models/armory.3di"));
	rig.devices.sync(rig.session);
	const size_t rows = rig.session.document_for(rig.catalog)->rows().size();
	command("models/armory.3di");
	TEST_EXPECT(rig.session.outcome().done() && armed(106101) && rig.session.document_for(rig.catalog)->rows().size() == rows);
	TEST_EXPECT(view.activity.status.find("Drop Armory (106101) draws armory.3di") != std::string::npos);
	// Refused: ids; two items drawing the pump.
	rig.session.handle(request::open_document("models/pump.3di"));
	rig.devices.sync(rig.session);
	command("models/pump.3di", { 1 });
	TEST_EXPECT(rig.session.outcome().refused && refusal(rig.session).find("no ids") != std::string::npos);
	command("models/pump.3di");
	TEST_EXPECT(rig.session.outcome().refused && refusal(rig.session).find("Drop Scaled Pump (106103)") != std::string::npos);
	// No mission open.
	rig.session.handle(request::close_document(rig.mission));
	TEST_EXPECT(place_in_mission_target(view).empty());
	command("models/pump.3di");
	TEST_EXPECT(rig.session.outcome().refused && refusal(rig.session).find("No mission is open") != std::string::npos);
	std::printf("test_place_in_mission passed\n");
	return 0;
}

// The retail leg (--retail, OPENNOVA_JO_DIR): the rule over the game's own models, nothing of the base game's.
// For the base game and each installed expansion (its mount: jox01's ITEMS.DEF over the base's), every model its
// ITEMS.DEF draws (the first row drawing it) read through the game's mount and told its item's kind from its parts
// alone, against the TYPE the shipped row gives it: every model with a seat or a driver's place a vehicle there and
// refused here (but the one decoration retail parks a boat as), every UseGun model an object, every skinned model
// a person; of the models with occlusion or a blink box, the buildings retail makes of them nine in ten at least
// (its land-for-points stations the objects), and of the others the decorations and foliage four in five at least.
// Then a shipped building model and a shipped decoration model, no item of the project drawing them, dropped on a
// mission: each made the TYPE its shipped row has.
static int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (the shipped models against their items)");
	std::vector<std::string> mounts = opennova::vfs_list_expansions(root);
	mounts.insert(mounts.begin(), std::string());
	std::string building, decoration; // the first base-game model of each the rule and retail agree on
	opennova::Vfs base;
	base.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
	TEST_EXPECT(base.mount_game(root, std::string(), opennova::VfsMountMode::Packed));
	for (const std::string &expansion : mounts) {
		opennova::Vfs game;
		game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		TEST_EXPECT(game.mount_game(root, expansion, opennova::VfsMountMode::Packed));
		std::vector<uint8_t> bytes;
		TEST_EXPECT(game.read_file("ITEMS.DEF", bytes));
		def::DefItemsFile items{};
		TEST_EXPECT(def::def_parse_items_memory(bytes.data(), bytes.size(), &items) == 0);
		// By the kind told from the model: how many models, and how many of them retail's row gives each TYPE.
		std::map<ModelItemKind, std::map<int, size_t>> tally;
		std::set<std::string> seen;
		size_t read = 0;
		for (size_t i = 0; i < items.count; ++i) {
			const std::string graphic = retail::lower_ascii(items.entries[i].graphic);
			if (graphic.empty() || !seen.insert(graphic).second) continue;
			std::vector<uint8_t> model_bytes;
			if (!game.read_file(graphic + ".3di", model_bytes)) continue;
			threedi::Threedi3di3 model{};
			if (threedi::threedi_3di3_read_memory(model_bytes.data(), model_bytes.size(), &model) == 0) {
				++read;
				const ModelItemFacts facts = model_item_facts(model);
				const int type = items.entries[i].type;
				++tally[facts.kind][type];
				if (expansion.empty() && graphic.size() <= 8) {
					if (facts.kind == ModelItemKind::Building && type == def::DEF_ITEM_TYPE_BUILDING && building.empty())
						building = graphic;
					if (facts.kind == ModelItemKind::Decoration && type == def::DEF_ITEM_TYPE_DECORATION && decoration.empty())
						decoration = graphic;
				}
			}
			threedi::threedi_3di3_free(&model);
		}
		def::def_free_items(&items);
		const auto total = [&](ModelItemKind kind) {
			size_t n = 0;
			for (const auto &[type, count] : tally[kind]) n += count;
			return n;
		};
		const auto of = [&](ModelItemKind kind, int type) { return tally[kind][type]; };
		std::printf("  %s: %zu shipped models read\n", expansion.empty() ? "the base game" : expansion.c_str(), read);
		for (const ModelItemKind kind : { ModelItemKind::Vehicle, ModelItemKind::MountedWeapon, ModelItemKind::Person,
		                                  ModelItemKind::Building, ModelItemKind::Decoration }) {
			std::printf("    %-14s %3zu models:", model_item_kind_token(kind), total(kind));
			for (const auto &[type, count] : tally[kind]) std::printf(" %s %zu", def::def_item_type_name(type), count);
			std::printf("\n");
		}
		TEST_EXPECT(read > 400);
		// Seats or a driver's place: a vehicle in retail, but one boat parked as a decoration.
		TEST_EXPECT(total(ModelItemKind::Vehicle) > 25 &&
		            of(ModelItemKind::Vehicle, def::DEF_ITEM_TYPE_VEHICLE) + 1 >= total(ModelItemKind::Vehicle));
		// A UseGun point: an object (the ewep class's) every one; skinned: a person every one.
		TEST_EXPECT(total(ModelItemKind::MountedWeapon) > 15 &&
		            of(ModelItemKind::MountedWeapon, def::DEF_ITEM_TYPE_OBJECT) == total(ModelItemKind::MountedWeapon));
		TEST_EXPECT(total(ModelItemKind::Person) > 25 &&
		            of(ModelItemKind::Person, def::DEF_ITEM_TYPE_PERSON) == total(ModelItemKind::Person));
		// Occlusion or a blink box: buildings, but for the land-for-points stations (objects), nine in ten at least.
		const size_t buildings = total(ModelItemKind::Building) - of(ModelItemKind::Building, def::DEF_ITEM_TYPE_OBJECT);
		TEST_EXPECT(buildings > 80 && of(ModelItemKind::Building, def::DEF_ITEM_TYPE_BUILDING) * 10 >= buildings * 9);
		// None of those parts: decorations and foliage (one TYPE, 2), four in five at least.
		TEST_EXPECT(total(ModelItemKind::Decoration) > 250 &&
		            of(ModelItemKind::Decoration, def::DEF_ITEM_TYPE_DECORATION) * 5 >= total(ModelItemKind::Decoration) * 4);
	}

	// A shipped building and a shipped decoration, dropped where no item draws them: the TYPE their rows have.
	TEST_EXPECT(!building.empty() && !decoration.empty());
	if (building.empty() || decoration.empty()) return 1;
	Rig rig("opennova_editor_place_model_retail");
	TEST_EXPECT(rig.open());
	if (rig.mission.empty()) return 1;
	const SessionView &view = rig.session.view();
	for (const auto &[graphic, type] : { std::pair<std::string, int>{ building, def::DEF_ITEM_TYPE_BUILDING },
	                                     { decoration, def::DEF_ITEM_TYPE_DECORATION } }) {
		std::vector<uint8_t> model_bytes;
		TEST_EXPECT(base.read_file(graphic + ".3di", model_bytes) &&
		            editor_test::write_bytes(view.project.root + "/models/" + graphic + ".3di", model_bytes));
		rig.session.handle(request::rescan());
		rig.session.run_operations();
		rig.devices.sync(rig.session);
		rig.drop((graphic + ".3di").c_str());
		int64_t id = 0, made = 0;
		std::string named;
		TEST_EXPECT(rig.session.outcome().done() && rig.item_named(graphic, id, made, named) && made == type && named == graphic);
		std::printf("  %s.3di dropped: item %s (%lld), %s\n", graphic.c_str(), graphic.c_str(), (long long)id,
		            def::def_item_type_name(int(made)));
		rig.session.run_operations();
	}
	std::printf("test_retail passed\n");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failed = 0;
	failed += test_model_item_facts();
	failed += test_drop_makes_item();
	failed += test_drop_refusals();
	failed += test_place_in_mission();
	failed += test_retail();
	return failed ? 1 : 0;
}
