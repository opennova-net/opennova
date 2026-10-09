// An entity off the ground is a Problem with a fix (DI-28, editor/preview/mission_ground_rules and the
// mission type's project check, mission_ground_check). The rules over a minted terrain (Tmap) and the synth
// models: a static on the terrain, floating over it (its fix by its ground anchor, or by its lowest point
// where its anchor stands over its top), buried under it, on a house's roof, floating over the roof,
// leaning on the house's wall, on the water and over it; a vehicle the game's to place; a person set down
// by the warmup, standing on the roof, falling at the start (no finding), flying, and hanging under a class
// with no motor. Then the check over a real session: each off entity a mission.off_ground Warning on its z
// with its planned fix, applied from the closed mission and undone; a live edit floating a crate; nothing
// checked again while nothing moves. Mixed: the retail leg (OPENNOVA_JO_DIR) runs the rules over every
// shipped mission of the base game over the game's own item table, models, terrains, tables and clips and
// counts what they flag (`--list` prints each).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/mission_document.h>
#include <editor/preview/mission_ground_check.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_ground_rules.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_source.h>
#include <editor/preview/viewport_follow.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/world/collision.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;
namespace bms = opennova::bms;
namespace mission = opennova::mission;

namespace {

bool g_list = false;

std::string fixture(const std::string &relative) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + relative;
}

bool near(double a, double b, double within = 0.01) { return std::fabs(a - b) <= within; }

// Files by flat name, each put with a stamp of its own.
class MemoryFiles : public opennova::FileSource {
public:
	void put(const std::string &name, std::vector<uint8_t> bytes) {
		files_[opennova::strutil::to_lower(name)] = { std::move(bytes), ++serial_ };
	}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const auto found = files_.find(opennova::strutil::to_lower(name));
		if (found == files_.end()) return false;
		out = found->second.first;
		return true;
	}
	uint64_t stamp(const std::string &name) const override {
		const auto found = files_.find(opennova::strutil::to_lower(name));
		return found == files_.end() ? 0 : found->second.second;
	}

private:
	std::map<std::string, std::pair<std::vector<uint8_t>, uint64_t>> files_;
	uint64_t serial_ = 0;
};

// The game's mounted files as a FileSource: a name the mount resolves stamps 1.
class VfsFiles : public opennova::FileSource {
public:
	explicit VfsFiles(const opennova::Vfs &vfs) : vfs_(vfs) {}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override { return vfs_.read_file(name, out); }
	uint64_t stamp(const std::string &name) const override { return vfs_.has_file(name) ? 1 : 0; }

private:
	const opennova::Vfs &vfs_;
};

std::vector<uint8_t> bytes_of(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// The synth crate (a 1-unit box standing on its origin) with its `ground` point raised to `height` (the
// mission's z, the model's +y), through the engine's own 3DI writer.
std::vector<uint8_t> crate_anchored_at(double height) {
	const std::vector<uint8_t> bytes = test_io::read_file(fixture("threedi/synth/crate.3di"));
	opennova::threedi::Threedi3di3 model{};
	if (opennova::threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) != 0) return {};
	for (size_t i = 0; i < model.user_point_count; ++i)
		if (std::string(model.user_points[i].name) == "ground") model.user_points[i].z = int32_t(height * 65536.0);
	std::vector<uint8_t> written;
	const bool wrote = opennova::threedi::threedi_3di3_write_memory(&model, written) == 0;
	opennova::threedi::threedi_3di3_free(&model);
	return wrote ? written : std::vector<uint8_t>();
}

// The catalog: a crate, the crate anchored 0.25 up, the crate anchored 1.5 up (over its 1-unit top), the
// synth house (a 4-unit body under a roof slab to 4.4), a player-controlled truck (cveh), a rifleman (org1),
// a statue (a person of org0's class, which has no motor), a bird (org1), a marker.
constexpr const char *kItems =
		"begin \"Null\"\nid 100000\ntype marker\nend\n"
		"begin \"Crate\"\nid 101001\ntype decoration\ngraphic crate\nend\n"
		"begin \"Anchored crate\"\nid 101002\ntype decoration\ngraphic acrate\nend\n"
		"begin \"Lifted crate\"\nid 101003\ntype decoration\ngraphic tcrate\nend\n"
		"begin \"House\"\nid 101004\ntype building\ngraphic house\nend\n"
		"begin \"Truck\"\nid 101005\ntype vehicle\ngraphic carrier\nmove_function cveh\nattrib: PlayerControl\nend\n"
		"begin \"Rifleman\"\nid 101006\ntype person\ngraphic shed\nanim_def people\nai_function org1\n"
		"move_function org1\nattrib: aidata\nend\n"
		"begin \"Statue\"\nid 101007\ntype person\ngraphic shed\nanim_def people\nai_function org0\n"
		"move_function org0\nattrib: aidata\nend\n"
		"begin \"Bird\"\nid 101008\ntype person\ngraphic shed\nanim_def people\nai_function org1\nmove_function org1\n"
		"attrib: aidata\nend\n"
		"begin \"Flag\"\nid 101009\ntype marker\nend\n";
constexpr const char *kPeopleAdm = "anim_reset\t\"idle\"\r\nanim_idle\t\"idle\" \"walk\"\r\nanim_idle_2\t\"idle\"\r\n"
								   "anim_walk_forward\t\"walk\"\r\nanim_guard\t\"walk\"\r\n";

// The terrain's height at (x, y), the column the game's probe writes.
double terrain_height(const opennova::terrain::TerrainHeightField &field, double x, double y) {
	const int32_t start[3] = { bms::to_fixed_16_16(x), bms::to_fixed_16_16(y), 0x7FFF0000 };
	int32_t end[3] = { start[0], start[1], -0x7FFF0000 };
	opennova::world::terrain_clip_segment(field, start, end, end);
	return double(end[2]) / 65536.0;
}

// Places over Tmap: three on land 6 units over the water and level within a unit over 12 units about them
// (a crate's, a house's, a person's), one over water 3 units under its plane.
struct Places {
	double land[3][2] = {};
	double sea[2] = {};
	bool found = false;
};
Places find_places(const opennova::terrain::TerrainHeightField &field, double water) {
	Places out;
	int lands = 0;
	bool sea = false;
	// The sea: off the island, east of it (an empty sector's floor), or wherever the island dips under.
	for (double x = 0.0; x <= 2000.0 && !sea; x += 32.0)
		if (terrain_height(field, x, 0.0) < water - 3.0) {
			out.sea[0] = x;
			out.sea[1] = 0.0;
			sea = true;
		}
	for (double y = -400.0; y <= 400.0 && lands < 3; y += 24.0)
		for (double x = -400.0; x <= 400.0 && lands < 3; x += 24.0) {
			const double h = terrain_height(field, x, y);
			if (h < water + 6.0) continue;
			bool level = true;
			for (double dy = -12.0; dy <= 12.0 && level; dy += 4.0)
				for (double dx = -12.0; dx <= 12.0 && level; dx += 4.0)
					level = std::fabs(terrain_height(field, x + dx, y + dy) - h) < 1.0;
			if (!level) continue;
			out.land[lands][0] = x;
			out.land[lands][1] = y;
			++lands;
		}
	out.found = lands == 3 && sea;
	return out;
}

// One record of the minted mission.
struct Placed {
	mission::EntityKind kind = mission::EntityKind::Item;
	int item = 0;
	double x = 0.0, y = 0.0, z = 0.0;
	uint32_t attributes = 0;
	int yaw = 0; // whole degrees
};

std::vector<uint8_t> mint_mission(const std::vector<Placed> &placed) {
	bms::File file;
	std::string error;
	mission::BlankMission blank;
	blank.name = "Ground";
	blank.terrain = "Tmap";
	if (!mission::make_blank(file, blank, error)) return {};
	int ssn = 1;
	for (const Placed &p : placed) {
		bms::Entity entity = mission::new_entity(p.kind, p.item, ssn++);
		entity.x = bms::to_fixed_16_16(p.x);
		entity.y = bms::to_fixed_16_16(p.y);
		entity.z = bms::to_fixed_16_16(p.z);
		entity.bmsi_attributes = p.attributes;
		entity.yaw = int16_t(p.yaw);
		switch (p.kind) {
		case mission::EntityKind::Item: file.items.push_back(entity); break;
		case mission::EntityKind::Building: file.buildings.push_back(entity); break;
		case mission::EntityKind::Marker: file.markers.push_back(entity); break;
		default: file.organics.push_back(entity); break;
		}
	}
	mission::sync_counts(file);
	std::vector<uint8_t> bytes;
	if (!bms::write(file, bytes, error)) bytes.clear();
	return bytes;
}

// The project's files of the minted rig: the terrain, the catalog, the models, the people's table and clips.
void put_rig(MemoryFiles &files) {
	files.put("Tmap.trn", test_io::read_file(fixture("terrain/tmap/Tmap.trn")));
	files.put("Tmap.cpt", test_io::read_file(fixture("terrain/tmap/Tmap.cpt")));
	// CR LF, as the game's line reader cuts a .def (editor_test::write_text stages it so for a file).
	std::string items;
	for (const char *c = kItems; *c; ++c) items += *c == '\n' ? std::string("\r\n") : std::string(1, *c);
	files.put("items.def", bytes_of(items));
	files.put("crate.3di", test_io::read_file(fixture("threedi/synth/crate.3di")));
	files.put("acrate.3di", crate_anchored_at(0.25));
	files.put("tcrate.3di", crate_anchored_at(1.5));
	files.put("house.3di", test_io::read_file(fixture("threedi/synth/house.3di")));
	files.put("carrier.3di", test_io::read_file(fixture("threedi/synth/carrier.3di")));
	files.put("people.adm", bytes_of(kPeopleAdm));
	files.put("idle.bad", test_io::read_file(fixture("anim/idle.bad")));
	files.put("walk.bad", test_io::read_file(fixture("anim/walk.bad")));
}

// The rig's records, by what each tests (its index in the records).
enum RigRecord {
	kOnTerrain,
	kFloats,
	kAnchoredFloats,
	kLiftedFloats,
	kBuried,
	kHouse,
	kOnRoof,
	kOverRoof,
	kLeaning,
	kOnWater,
	kOverWater,
	kTruck,
	kSettled,
	kOnRoofPerson,
	kFalls,
	kHangs,
	kFlying,
	kMarker,
	kRigCount
};

std::vector<Placed> rig_records(const opennova::terrain::TerrainHeightField &field, const Places &at, double water) {
	const auto h = [&](double x, double y) { return terrain_height(field, x, y); };
	const double cx = at.land[0][0], cy = at.land[0][1];
	const double hx = at.land[1][0], hy = at.land[1][1];
	const double px = at.land[2][0], py = at.land[2][1];
	const double sx = at.sea[0], sy = at.sea[1];
	const double roof = h(hx, hy) + 4.4;
	using K = mission::EntityKind;
	std::vector<Placed> placed(kRigCount);
	placed[kOnTerrain] = { K::Item, 101001, cx, cy, h(cx, cy) };
	placed[kFloats] = { K::Item, 101001, cx + 4.0, cy, h(cx + 4.0, cy) + 3.0 };
	placed[kAnchoredFloats] = { K::Item, 101002, cx - 4.0, cy, h(cx - 4.0, cy) + 2.0 };
	placed[kLiftedFloats] = { K::Item, 101003, cx, cy + 4.0, h(cx, cy + 4.0) + 3.0 };
	placed[kBuried] = { K::Item, 101001, cx, cy - 4.0, h(cx, cy - 4.0) - 5.0 };
	// Facing 90 degrees, its model's axes the mission's (the engine's heading, 90 less the yaw, is 0).
	placed[kHouse] = { K::Building, 101004, hx, hy, h(hx, hy), 0, 90 };
	placed[kOnRoof] = { K::Item, 101001, hx - 2.0, hy - 2.0, roof };
	placed[kOverRoof] = { K::Item, 101001, hx - 2.0, hy + 2.0, roof + 3.0 };
	// Beside the house's body (its x from -4 to 4), its box against the wall, 2 units off the terrain.
	placed[kLeaning] = { K::Item, 101001, hx + 4.5, hy + 3.0, h(hx + 4.5, hy + 3.0) + 2.0 };
	placed[kOnWater] = { K::Item, 101001, sx, sy, water };
	placed[kOverWater] = { K::Item, 101001, sx + 4.0, sy, water + 3.0 };
	placed[kTruck] = { K::Item, 101005, px + 8.0, py, h(px + 8.0, py) + 5.0 };
	placed[kSettled] = { K::Organic, 101006, px, py, h(px, py) };
	placed[kOnRoofPerson] = { K::Organic, 101006, hx + 2.0, hy - 2.0, roof };
	placed[kFalls] = { K::Organic, 101006, px - 4.0, py, h(px - 4.0, py) + 5.0 };
	placed[kHangs] = { K::Organic, 101007, px, py + 4.0, h(px, py + 4.0) + 5.0 };
	placed[kFlying] = { K::Organic, 101008, px, py - 4.0, h(px, py - 4.0) + 5.0,
		uint32_t(bms::BmsiAttributeFlags::FlyingOrganic) };
	placed[kMarker] = { K::Marker, 101009, px + 4.0, py, h(px + 4.0, py) + 7.0 };
	return placed;
}

} // namespace

// The rules over the minted rig (mission_ground_rules.h): each class grounded by the game's own rule.
static int test_rules() {
	auto files = std::make_shared<MemoryFiles>();
	put_rig(*files);
	MissionGround ground;
	MissionSceneHeader header;
	header.terrain = "Tmap";
	ground.follow(files, 1, header, "ground");
	TEST_EXPECT(ground.terrain() && ground.water());
	if (!ground.terrain()) return 1;
	const double water = ground.water_height();
	const Places at = find_places(*ground.height_field(), water);
	TEST_EXPECT(at.found);
	if (!at.found) return 1;
	const std::vector<Placed> placed = rig_records(*ground.height_field(), at, water);
	MissionDocument document;
	Diagnostic error;
	TEST_EXPECT(document.load_bytes(mint_mission(placed), "missions/ground.bms", AssetKind::Mission, "jo", error));
	const std::unique_ptr<MissionSceneSource> source = mission_scene_source(document);
	MissionScene scene;
	scene.read(*source);
	ground.follow(files, 2, scene.header(), "ground");
	MissionGroundReads reads;
	const auto stamped = std::make_shared<opennova::StampedFiles>(files);
	const std::vector<MissionGroundVerdict> verdicts = mission_ground_verdicts(scene, ground, reads, stamped);
	TEST_EXPECT(verdicts.size() == placed.size());
	// Each verdict by its record, found again by the record's item and position.
	std::map<int, const MissionGroundVerdict *> of;
	for (const MissionGroundVerdict &verdict : verdicts) {
		const MissionEntityMark *mark = scene.entity(verdict.row);
		for (size_t i = 0; mark && i < placed.size(); ++i)
			if (near(mark->x, placed[i].x) && near(mark->y, placed[i].y) && near(mark->z, placed[i].z) &&
					mark->item == placed[i].item)
				of[int(i)] = &verdict;
	}
	TEST_EXPECT(of.size() == placed.size());
	if (g_list)
		for (int i = 0; i < kRigCount; ++i)
			std::printf("  %d: %s %s why '%s' base %.3f bottom %.3f top %.3f %s %.3f fix %.3f\n", i,
					mission_ground_rule_token(of[i]->rule), mission_ground_state_token(of[i]->state),
					of[i]->why.c_str(), of[i]->base, of[i]->bottom, of[i]->top, mission_support_token(of[i]->on),
					of[i]->support, of[i]->fix_z);
	const auto state = [&](int i) { return of[i]->state; };
	const auto rule = [&](int i) { return of[i]->rule; };
	using S = MissionGroundState;

	// A crate on the terrain stands on it; 3 units up it floats, set down by its anchor (its origin).
	TEST_EXPECT(rule(kOnTerrain) == MissionGroundRule::Static && state(kOnTerrain) == S::Grounded &&
			of[kOnTerrain]->on == MissionSupport::Terrain);
	const MissionGroundVerdict &floats = *of[kFloats];
	TEST_EXPECT(floats.state == S::Floats && floats.off() && floats.on == MissionSupport::Terrain &&
			near(floats.bottom - floats.support, 3.0) && floats.by_anchor && near(floats.fix_z, floats.support));
	// The anchored crate: its anchor set on the terrain, its z the ground less the anchor's height.
	const MissionGroundVerdict &anchored = *of[kAnchoredFloats];
	TEST_EXPECT(anchored.state == S::Floats && anchored.by_anchor && near(anchored.fix_z, anchored.support - 0.25) &&
			near(anchored.base - placed[kAnchoredFloats].z, 0.25));
	// The crate anchored over its own top: the anchor would bury it, so its lowest point is set down.
	const MissionGroundVerdict &lifted = *of[kLiftedFloats];
	TEST_EXPECT(lifted.state == S::Floats && !lifted.by_anchor && near(lifted.fix_z, lifted.support));
	// Wholly under the terrain: buried, lifted by its anchor.
	const MissionGroundVerdict &buried = *of[kBuried];
	TEST_EXPECT(buried.state == S::Buried && buried.off() && buried.top < buried.support && near(buried.fix_z, buried.support));
	// The house stands on the terrain; a crate on its roof stands on it, one 3 units over it floats over it.
	TEST_EXPECT(state(kHouse) == S::Grounded && rule(kHouse) == MissionGroundRule::Static);
	const MissionGroundVerdict &roof = *of[kOnRoof];
	TEST_EXPECT(roof.state == S::Grounded && roof.on == MissionSupport::Record && roof.on_row == of[kHouse]->row &&
			roof.on_name == "House" && near(roof.support, placed[kOnRoof].z, 0.02));
	const MissionGroundVerdict &over_roof = *of[kOverRoof];
	TEST_EXPECT(over_roof.state == S::Floats && over_roof.on_row == of[kHouse]->row &&
			near(over_roof.fix_z, placed[kOnRoof].z, 0.02));
	// Against the house's wall, off the ground: it touches the house, left as authored.
	const MissionGroundVerdict &leaning = *of[kLeaning];
	TEST_EXPECT(leaning.state == S::Grounded && leaning.why == "touches" && leaning.on_row == of[kHouse]->row);
	// On the water plane it stands on it; 3 units over it floats over it, set down on it.
	TEST_EXPECT(state(kOnWater) == S::Grounded && of[kOnWater]->on == MissionSupport::Water);
	const MissionGroundVerdict &over_water = *of[kOverWater];
	TEST_EXPECT(over_water.state == S::Floats && over_water.on == MissionSupport::Water && near(over_water.fix_z, water));
	// The truck's class moves it: the game places it.
	TEST_EXPECT(rule(kTruck) == MissionGroundRule::Mover && state(kTruck) == S::Grounded && of[kTruck]->why == "move:cveh");
	// People: set down by the warmup; standing on the roof; falling at the start (the game lands him, no
	// finding); a bird flying; a statue of org0's class hanging in the air.
	TEST_EXPECT(rule(kSettled) == MissionGroundRule::Person && state(kSettled) == S::Grounded &&
			of[kSettled]->why == "settled");
	TEST_EXPECT(state(kOnRoofPerson) == S::Grounded && of[kOnRoofPerson]->why == "standing" &&
			of[kOnRoofPerson]->on_name == "House");
	const MissionGroundVerdict &falls = *of[kFalls];
	TEST_EXPECT(falls.state == S::Falls && !falls.off() && falls.why == "move:org1" &&
			near(falls.fix_z, placed[kFalls].z - (falls.base - falls.support)));
	TEST_EXPECT(state(kFlying) == S::Grounded && of[kFlying]->why == "flying");
	const MissionGroundVerdict &hangs = *of[kHangs];
	TEST_EXPECT(hangs.state == S::Hangs && hangs.off() && hangs.why == "move:org0" && hangs.base - hangs.support > 1.0);
	TEST_EXPECT(rule(kMarker) == MissionGroundRule::None && of[kMarker]->why == "marker");

	// The words: a float names what lies under it, its fix the anchor; the fallback says why.
	TEST_EXPECT(mission_ground_message(floats, "Crate #2").find("Crate #2 floats 3.0 m above the terrain") == 0);
	std::string label, detail;
	mission_ground_fix_words(over_roof, label, detail);
	TEST_EXPECT(label == "Set it on House" && detail.find("ground anchor") != std::string::npos);
	mission_ground_fix_words(lifted, label, detail);
	TEST_EXPECT(label == "Set it on the terrain" && detail.find("lowest point") != std::string::npos);
	TEST_EXPECT(mission_ground_message(hangs, "Statue #16").find("(org0)") != std::string::npos);
	// The wire.
	const opennova::io::JsonValue json = mission_ground_verdict_json(over_roof);
	TEST_EXPECT(json.get_string("rule", "") == "static" && json.get_string("state", "") == "floats" &&
			json.get_string("on", "") == "record" && json.get_string("on_name", "") == "House" &&
			near(json.get_number("fix_z", 0.0), over_roof.fix_z));
	// Every file read noted (the catalog, the models, the people's table and clips).
	TEST_EXPECT(stamped->stamps().files().size() >= 8);
	std::printf("test_rules passed\n");
	return 0;
}

namespace {

constexpr const char *kMission = "missions/ground.bms";

// A session over a project holding the rig: the terrain, the catalog, the models, the people's table and
// clips, and the minted mission (closed).
struct Session {
	editor_test::TempProjectDir dir;
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{ platform, preferences };
	std::string root;
	std::vector<Placed> placed;

	explicit Session(const char *name) : dir(name) {}
	bool open() {
		editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Ground"));
		editor_test::create_missing_files(session);
		editor_test::set_missions(session, true);
		root = session.view().project.root;
		auto files = std::make_shared<MemoryFiles>();
		put_rig(*files);
		const AssetEntry *items = session.view().project.scan->find("items.def");
		const std::string items_path = items ? items->relative_path : std::string("defs/items.def");
		for (const char *name : { "Tmap.trn", "Tmap.cpt", "crate.3di", "acrate.3di", "tcrate.3di", "house.3di",
					 "carrier.3di", "people.adm", "idle.bad", "walk.bad" }) {
			std::vector<uint8_t> bytes;
			if (!files->read(name, bytes) || !editor_test::write_bytes(root + "/" + name, bytes)) return false;
		}
		if (!editor_test::write_text(root + "/" + items_path, kItems)) return false;
		// The places over the terrain as the rules read it.
		MissionGround ground;
		MissionSceneHeader header;
		header.terrain = "Tmap";
		ground.follow(files, 1, header, "ground");
		if (!ground.terrain()) return false;
		const Places at = find_places(*ground.height_field(), ground.water_height());
		if (!at.found) return false;
		placed = rig_records(*ground.height_field(), at, ground.water_height());
		if (!editor_test::write_bytes(root + "/" + kMission, mint_mission(placed))) return false;
		editor_test::handle_to_end(session, request::rescan());
		return true;
	}
	std::vector<const Diagnostic *> off() const {
		std::vector<const Diagnostic *> out;
		for (const Diagnostic &d : session.view().findings.diagnostics)
			if (d.code() == "mission.off_ground" && d.asset == kMission) out.push_back(&d);
		return out;
	}
	const MissionGroundCheck *check() const { return mission_ground_check(session.view().findings.project_checks.get()); }
};

} // namespace

// The check over a real session: the findings and their fixes, applied and undone; a live edit; reuse.
static int test_check() {
	Session s("opennova_editor_mission_ground");
	TEST_EXPECT(s.open());
	if (s.placed.empty()) return 1;
	const MissionGroundCheck *check = s.check();
	TEST_EXPECT(check != nullptr);
	if (!check) return 1;
	// The floating crates, the buried one, the crate over the roof and over the water, and the statue: seven.
	std::vector<const Diagnostic *> off = s.off();
	TEST_EXPECT(off.size() == 7);
	for (const Diagnostic *d : off)
		TEST_EXPECT(d->severity == DiagnosticSeverity::Warning && d->field == "z" && d->planned.size() == 1 &&
				d->planned[0].edits.size() == 1 && d->planned[0].edits[0].field == "z" && !d->record_title.empty());
	const std::vector<MissionGroundVerdict> *verdicts = check->verdicts(kMission);
	TEST_EXPECT(verdicts && verdicts->size() == s.placed.size());
	// A crate floating over the terrain: its fix is an edit of the closed mission, one step Undo takes back.
	const Diagnostic *floats = nullptr;
	for (const Diagnostic *d : off)
		if (d->message.find("floats 3.0 m above the terrain") != std::string::npos && d->record_title.find("Crate #") == 0)
			floats = d;
	TEST_EXPECT(floats != nullptr && s.session.document_for(kMission) == nullptr);
	if (!floats) return 1;
	const NodeAddress crate{ floats->row_id, floats->record_kind, 0 };
	const double fix_z = std::get<double>(floats->planned[0].edits[0].value);
	const std::vector<ProblemFix> fixes = fixes_for(*floats, s.session.view());
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Set it on the terrain" &&
			fixes[0].request.kind == EditorRequestKind::EditRecord && fixes[0].request.open_first &&
			fixes[0].detail.find("Undo takes it back") != std::string::npos);
	if (fixes.size() != 1) return 1;
	editor_test::handle_to_end(s.session, fixes[0].request);
	const Document *document = s.session.document_for(kMission);
	Value z;
	TEST_EXPECT(document && document->dirty() && document->get(crate, "z", z) && near(std::get<double>(z), fix_z, 0.001));
	TEST_EXPECT(s.off().size() == 6);
	editor_test::handle_to_end(s.session, request::undo(kMission));
	TEST_EXPECT(s.off().size() == 7);
	// Live: the crate on the terrain lifted 4 units in the open document floats; that mission alone is checked.
	const MissionGroundVerdict *on_terrain = nullptr;
	for (const MissionGroundVerdict &verdict : *check->verdicts(kMission))
		if (verdict.state == MissionGroundState::Grounded && verdict.rule == MissionGroundRule::Static &&
				verdict.item == 101001 && verdict.on == MissionSupport::Terrain && verdict.why.empty())
			on_terrain = &verdict;
	TEST_EXPECT(on_terrain != nullptr);
	if (!on_terrain) return 1;
	const NodeAddress lifted{ on_terrain->row, on_terrain->kind, 0 };
	const double terrain = on_terrain->support;
	Edit raise;
	raise.address = lifted;
	raise.field = "z";
	raise.value = terrain + 4.0;
	editor_test::handle_to_end(s.session, request::edit_record(kMission, raise));
	TEST_EXPECT(s.off().size() == 8 && check->checked() == 1);
	const MissionGroundVerdict *now = check->verdict(kMission, lifted.row);
	TEST_EXPECT(now && now->state == MissionGroundState::Floats && near(now->fix_z, terrain, 0.001));
	// A validation with nothing moved checks no mission again.
	editor_test::handle_to_end(s.session, request::rescan());
	TEST_EXPECT(check->checked() == 0 && s.off().size() == 8);
	editor_test::handle_to_end(s.session, request::undo(kMission));
	TEST_EXPECT(s.off().size() == 7);
	std::printf("test_check passed\n");
	return 0;
}

// The retail leg: every shipped mission's entities as the rules ground them.
static int test_retail() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (every shipped mission's entities grounded)");
	opennova::Vfs game;
	game.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
	TEST_EXPECT(game.mount_game(root, std::string(), opennova::VfsMountMode::Packed));
	const auto files = std::make_shared<VfsFiles>(game);
	std::vector<std::string> missions;
	for (const opennova::VfsFileLocation &file : game.list_files()) {
		const std::string name = std::filesystem::path(file.logical_name).filename().string();
		if (retail::lower_ascii(std::filesystem::path(name).extension().string()) == ".bms") missions.push_back(name);
	}
	std::sort(missions.begin(), missions.end());
	if (missions.empty()) return retail::skip_leg("OPENNOVA_JO_DIR with the game's missions in its archives");
	const auto started = std::chrono::steady_clock::now();
	MissionGroundReads reads;
	MissionGround ground;
	std::map<std::string, size_t> by_state, by_why;
	size_t entities = 0, statics = 0, off = 0, grounded_missions = 0;
	uint64_t generation = 0;
	for (const std::string &name : missions) {
		std::vector<uint8_t> bytes;
		TEST_EXPECT(game.read_file(name, bytes));
		MissionDocument document;
		Diagnostic error;
		TEST_EXPECT(document.load_bytes(bytes, name, AssetKind::Mission, "jo", error));
		const std::unique_ptr<MissionSceneSource> source = mission_scene_source(document);
		if (!source) continue;
		MissionScene scene;
		scene.read(*source);
		const auto stamped = std::make_shared<opennova::StampedFiles>(files);
		ground.follow(stamped, ++generation, scene.header(), name.substr(0, name.find_last_of('.')));
		grounded_missions += ground.terrain() ? 1 : 0;
		if (g_list && !ground.terrain()) std::printf("  %s: no terrain (%s)\n", name.c_str(), ground.error().c_str());
		for (const MissionGroundVerdict &verdict : mission_ground_verdicts(scene, ground, reads, stamped)) {
			++entities;
			++by_state[mission_ground_state_token(verdict.state)];
			++by_why[std::string(mission_ground_rule_token(verdict.rule)) + "/" + verdict.why];
			statics += verdict.rule == MissionGroundRule::Static && verdict.why != "no_terrain" ? 1 : 0;
			if (!verdict.off()) continue;
			++off;
			if (g_list)
				std::printf("  %s row %llu item %lld \"%s\": %s %s base %.2f bottom %.2f top %.2f %s %.2f fix %.2f\n",
						name.c_str(), (unsigned long long)verdict.row, (long long)verdict.item, verdict.name.c_str(),
						mission_ground_rule_token(verdict.rule), mission_ground_state_token(verdict.state), verdict.base,
						verdict.bottom, verdict.top, mission_support_token(verdict.on), verdict.support, verdict.fix_z);
		}
	}
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::printf("retail: %zu missions (%zu on a terrain read), %zu entities (%zu statics checked), %zu off the ground, "
				"%zu files parsed, in %.1f s\n",
			missions.size(), grounded_missions, entities, statics, off, reads.parsed(), seconds);
	for (const auto &entry : by_state) std::printf("  state %s: %zu\n", entry.first.c_str(), entry.second);
	for (const auto &entry : by_why) std::printf("  %s: %zu\n", entry.first.c_str(), entry.second);
	// The shipped missions' entities stand where the rules ground them but for a handful of statics in a
	// thousand (38 of the 75,746 statics on a terrain the mount reads; the people the warmup leaves in the
	// air fall where the game puts them, no finding).
	TEST_EXPECT(grounded_missions > 0 && statics > 0 && off * 1000 < statics);
	std::printf("test_retail passed\n");
	return 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--list") == 0) g_list = true;
	TEST_EXPECT(test_rules() == 0);
	TEST_EXPECT(test_check() == 0);
	TEST_EXPECT(test_retail() == 0);
	std::printf("editor_mission_ground: all tests passed\n");
	return 0;
}
