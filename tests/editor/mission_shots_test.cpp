// DI-23 (ADR 0046, the deep-integration plan's "ammo impact rows: a board and a Shoot tool"): the mission view's
// Shoot tool fires an ammo where a click meets the terrain or an object, and the impact plays as the game plays it
// (preview/mission_shots). Headless, over minted fixtures alone: Tmap's .trn and .cpt (fixtures/terrain/tmap) with a
// minted char map painting every class at a known place, the synth crate (fixtures/threedi/synth: a 12-face box)
// drawn by an item of the catalog, an ammo.def whose ammo authors one row, AT_NULL twelve.
//
// Pinned: a terrain stop plays the class the game's sampler reads there + 4 (grass: tag 6), from ammo def 0's bank
// at the tag's place where the ammo authors none (the fallback rule), its effect spawned with no orientation, its
// sound readied there, no scar; the object stop plays the face's material + 4 from the ammo's own row, the effect
// along the flight, a ring scar on the object, its damage (its hit points before and after); an item of one hit
// point dies and names its death as DI-10 plans it; the run is a function of its shots (the same shots run again
// to the same events); a shot whose ammo ammo.def lacks is refused; the envelope; the shot's wire and refusals; and
// the board over the same table (each class's row, the bank's place, the scar).
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/ammo_impacts.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_shots.h>
#include <formats/def/def.h>
#include <formats/mission/bms.h>
#include <formats/pcx/pcx_io.h>
#include <formats/trn/charmap_legend.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ammo_table_build.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string fixture(const std::string &rel) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + rel;
}

// Each line ended CR LF, as the game's readers take a text (File_ParseASCIIFile splits on it).
std::string crlf(const std::string &text) {
	std::string out;
	for (const char c : text) {
		if (c == '\n') out += '\r';
		out += c;
	}
	return out;
}

// Files by name (any case), each write moving its stamp.
struct ShotFiles final : opennova::FileSource {
	std::map<std::string, std::vector<uint8_t>> files;
	std::map<std::string, uint64_t> stamps;
	uint64_t serial = 0;
	static std::string key(const std::string &name) {
		std::string out = name;
		for (char &c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}
	void put(const std::string &name, std::vector<uint8_t> bytes) {
		files[key(name)] = std::move(bytes);
		stamps[key(name)] = ++serial;
	}
	void put_text(const std::string &name, const std::string &text) {
		const std::string lines = crlf(text);
		put(name, std::vector<uint8_t>(lines.begin(), lines.end()));
	}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const auto found = files.find(key(name));
		if (found == files.end()) return false;
		out = found->second;
		return true;
	}
	uint64_t stamp(const std::string &name) const override {
		const auto found = stamps.find(key(name));
		return found == stamps.end() ? 0 : found->second;
	}
};

constexpr int kSide = 256;

// The char map: class c in the block of 32 texels whose corner is texel (16 + 32 (c % 5), 16 + 32 (c / 5)), dirt
// (1) elsewhere.
std::vector<uint8_t> charmap() {
	opennova::IndexedImage8 map;
	map.width = map.height = kSide;
	map.indices.assign(size_t(kSide) * kSide, 1);
	for (int c = 0; c < opennova::kCharmapLegendCount; ++c)
		for (int z = 0; z < 32; ++z)
			for (int x = 0; x < 32; ++x)
				map.indices[size_t(16 + 32 * (c / 5) + z) * kSide + size_t(16 + 32 * (c % 5) + x)] = uint8_t(c);
	std::vector<uint8_t> bytes;
	std::string error;
	opennova::encode_pcx_indexed(map, bytes, error);
	return bytes;
}

// Class c's block's middle, mission metres (the island layout: a 256 map's texel four units).
void block_middle(int c, double &x, double &y) {
	const int tx = 32 + 32 * (c % 5), tz = 32 + 32 * (c / 5);
	x = 4.0 * tx - 512.0 + 2.0;
	y = 512.0 - 4.0 * tz - 2.0;
}

// AT_NULL authors tags 1 to 12 in order, so its bank's place t holds tag t; AMMO_T authors its obj row alone.
const char *kAmmo =
		"ammo AT_NULL\n\teffects_table\n\t\tmove none none 0\n\t\tplayer none S_PLAYER 15\n\t\tzip none S_ZIP 15\n"
		"\t\tobj none S_OBJ 15\n\t\tdirt Hit_D S_DIRT 15\n\t\tgrass Hit_G S_GRASS 15\n\t\tsnow none S_SNOW 15\n"
		"\t\tcement none S_CEMENT 15\n\t\tsand none S_SAND 15\n\t\tpackeddirt none S_PACKED 15\n"
		"\t\twater Hit_W S_WATER 15\n\t\trailroad none S_RAIL 15\n\tend\nend\n"
		"ammo AMMO_T\n\tvelocity 800\n\tmax_age 2\n\tweight_in_grains 62\n\tscar_type 1\n"
		"\teffects_table\n\t\tobj Hit_O S_OBJ_T 15\n\tend\nend\n";

const char *kItems = "begin \"Shot Crate\"\nid 106190\ntype object\ngraphic crate\nhp 500\nend\n"
                     "begin \"Frail Crate\"\nid 106191\ntype object\ngraphic crate\nhusk cratehusk\nai_function gnrc\nhp 1\nend\n";

std::shared_ptr<ShotFiles> shot_files() {
	auto files = std::make_shared<ShotFiles>();
	files->put("Tmap.trn", test_io::read_file(fixture("terrain/tmap/Tmap.trn")));
	files->put("Tmap.cpt", test_io::read_file(fixture("terrain/tmap/Tmap.cpt")));
	files->put("Tmap_m.pcx", charmap());
	files->put("crate.3di", test_io::read_file(fixture("threedi/synth/crate.3di")));
	files->put("cratehusk.3di", test_io::read_file(fixture("threedi/synth/crate.3di")));
	files->put_text("ammo.def", kAmmo);
	files->put_text("items.def", kItems);
	return files;
}

opennova::bms::Entity item_at(int32_t type_id, double x, double y, double z) {
	opennova::bms::Entity e{};
	e.type = opennova::bms::ItemType::Item;
	e.type_id = type_id;
	e.id = 100 + type_id % 100;
	e.x = opennova::bms::to_fixed_16_16(x);
	e.y = opennova::bms::to_fixed_16_16(y);
	e.z = opennova::bms::to_fixed_16_16(z);
	e.team = 1;
	return e;
}

struct ShotRig {
	std::shared_ptr<ShotFiles> files = shot_files();
	MissionGround ground;
	double crate[3] = {0.0, 0.0, 0.0};
	double frail[3] = {0.0, 0.0, 0.0};
	MissionShotsSetup setup;
	MissionShots shots;

	double height(double x, double y) const {
		return double(opennova::terrain::height_field_height_world_bilinear(*ground.height_field(), float(x), float(-y)));
	}
	ShotRig() {
		MissionSceneHeader header;
		header.terrain = "Tmap";
		ground.follow(files, 1, header, "pad");
		block_middle(3, crate[0], crate[1]);
		crate[2] = height(crate[0], crate[1]);
		block_middle(4, frail[0], frail[1]);
		frail[2] = height(frail[0], frail[1]);
		auto mission = std::make_shared<opennova::bms::File>();
		mission->items.push_back(item_at(6190, crate[0], crate[1], crate[2]));
		mission->items.push_back(item_at(6191, frail[0], frail[1], frail[2]));
		setup.files = files;
		setup.key = 1;
		setup.mission = mission;
		setup.ground = &ground;
		const double *places[2] = {crate, frail};
		for (int i = 0; i < 2; ++i) {
			MissionEntityMark mark;
			mark.row = NodeId(11 + i);
			mark.pool = MissionPool::Item;
			mark.index = i;
			mark.item = 106190 + i;
			mark.x = places[i][0];
			mark.y = places[i][1];
			mark.z = places[i][2];
			setup.entities.push_back(mark);
			setup.titles[mark.row] = i == 0 ? "Shot Crate (SSN 190)" : "Frail Crate (SSN 191)";
		}
		shots.configure(setup);
	}
	// A shot fired straight down at (x, y) of the ground, or at a point over it.
	static MissionShot down_at(const std::string &ammo, double x, double y, double z, int32_t tick = 0) {
		MissionShot shot;
		shot.tick = tick;
		shot.ammo = ammo;
		shot.at[0] = x;
		shot.at[1] = y;
		shot.at[2] = z;
		shot.eye[0] = x;
		shot.eye[1] = y;
		shot.eye[2] = z + 60.0;
		return shot;
	}
};

JsonValue parsed(const std::string &text) {
	JsonValue out;
	std::string error;
	opennova::io::json_parse(text, out, error);
	return out;
}

std::string replaced(std::string text, const std::string &from, const std::string &to) {
	const size_t at = text.find(from);
	if (at != std::string::npos) text.replace(at, from.size(), to);
	return text;
}

std::vector<MissionShotEvent> events_of(const MissionShots &shots, MissionShotEvent::Kind kind) {
	std::vector<MissionShotEvent> out;
	for (const MissionShotEvent &event : shots.events())
		if (event.kind == kind) out.push_back(event);
	return out;
}

// The terrain: the class the game's sampler reads + 4, the fallback from the bank, no scar.
int test_terrain_shot() {
	ShotRig rig;
	TEST_EXPECT(rig.ground.terrain());
	double x = 0.0, y = 0.0;
	block_middle(2, x, y);
	rig.shots.set_shots({ShotRig::down_at("AMMO_T", x, y, rig.height(x, y))});
	rig.shots.run_to(12);
	TEST_EXPECT(rig.shots.why().empty());
	const std::vector<MissionShotEvent> fired = events_of(rig.shots, MissionShotEvent::Kind::Fired);
	const std::vector<MissionShotEvent> impacts = events_of(rig.shots, MissionShotEvent::Kind::Impact);
	if (impacts.size() != 1) {
		std::printf("why: %s\n", rig.shots.why().c_str());
		for (const MissionShotEvent &event : rig.shots.events()) std::printf("  %d %s\n", event.tick, event.words.c_str());
	}
	TEST_EXPECT(fired.size() == 1 && impacts.size() == 1);
	const MissionShotEvent &hit = impacts[0];
	TEST_EXPECT(hit.on == "terrain" && hit.surface == 2 && hit.ground.from == MissionSurfaceFrom::Map);
	// AMMO_T authors no grass row: the game plays AT_NULL's bank at place 6, its grass row.
	TEST_EXPECT(hit.pick.tag == 6 && hit.pick.from == opennova::world::ImpactRowFrom::NullBank && hit.pick.bank_tag == 6 &&
	            hit.effect == "Hit_G" && hit.set == "S_GRASS");
	TEST_EXPECT(std::fabs(hit.at[0] - x) < 0.05 && std::fabs(hit.at[1] - y) < 0.05);
	// A terrain stop spawns its effect with no orientation and leaves no scar.
	TEST_EXPECT(hit.direction.x == 0.0f && hit.direction.y == 0.0f && hit.direction.z == 0.0f && hit.scar.empty());
	TEST_EXPECT(rig.shots.spawns().size() == 1 && rig.shots.spawns()[0].effect == "Hit_G" && rig.shots.scar_count() == 0);
	bool heard = false;
	for (const MissionShotEvent &sound : events_of(rig.shots, MissionShotEvent::Kind::Sound)) heard = heard || sound.set == "S_GRASS";
	TEST_EXPECT(heard);
	TEST_EXPECT(hit.words.find("bank at place 6") != std::string::npos);
	std::printf("test_terrain_shot passed\n");
	return 0;
}

// An object: its face's material + 4 from the ammo's own row, the effect along the flight, a ring scar, its damage;
// one of one hit point dies, its death named as DI-10 plans it.
int test_object_shot() {
	ShotRig rig;
	rig.shots.set_shots({ShotRig::down_at("AMMO_T", rig.crate[0], rig.crate[1], rig.crate[2])});
	rig.shots.run_to(12);
	TEST_EXPECT(rig.shots.entities_loaded() >= 1 && rig.shots.entities_collidable() >= 1);
	const std::vector<MissionShotEvent> impacts = events_of(rig.shots, MissionShotEvent::Kind::Impact);
	TEST_EXPECT(impacts.size() == 1);
	if (impacts.size() != 1) return 1;
	const MissionShotEvent &hit = impacts[0];
	TEST_EXPECT(hit.on == "object" && hit.row == 11 && hit.record == "Shot Crate (SSN 190)" && hit.item == 106190);
	TEST_EXPECT(hit.surface >= 0 && hit.pick.tag == hit.surface + 4);
	if (hit.surface == 0) TEST_EXPECT(hit.pick.from == opennova::world::ImpactRowFrom::Own && hit.effect == "Hit_O" && hit.set == "S_OBJ_T");
	// Along the round's flight (straight down), and a ring scar on the crate.
	TEST_EXPECT(hit.direction.z < -0.99f && !hit.scar.empty() && rig.shots.scar_count() == 1);
	TEST_EXPECT(!rig.shots.scars().batches.empty());
	const std::vector<MissionShotEvent> damage = events_of(rig.shots, MissionShotEvent::Kind::Damage);
	TEST_EXPECT(damage.size() == 1 && damage[0].row == 11 && damage[0].health_before == 500 &&
	            damage[0].health_after < 500 && damage[0].health_max == 500);
	TEST_EXPECT(events_of(rig.shots, MissionShotEvent::Kind::Death).empty());
	// The frail crate dies of one round, its death named.
	rig.shots.set_shots({ShotRig::down_at("AMMO_T", rig.frail[0], rig.frail[1], rig.frail[2])});
	rig.shots.run_to(12);
	const std::vector<MissionShotEvent> deaths = events_of(rig.shots, MissionShotEvent::Kind::Death);
	TEST_EXPECT(deaths.size() == 1 && deaths[0].row == 12 && deaths[0].health_after <= 0 &&
	            deaths[0].words.find("is destroyed") != std::string::npos);
	// S23 C: the death kept for the device, its husk swapped in from the plan's swap tick on (none before it), the
	// destroy fade's registers and the sections the pieces left with it.
	TEST_EXPECT(rig.shots.deaths().size() == 1);
	if (rig.shots.deaths().size() == 1 && !deaths.empty()) {
		const MissionShotDeath &death = rig.shots.deaths()[0];
		TEST_EXPECT(death.row == 12 && death.item == 106191 && death.tick == deaths[0].tick && death.plan.husk == "cratehusk");
		TEST_EXPECT(death.plan.swaps);
		const int32_t swap = death.tick + death.plan.swap_tick;
		TEST_EXPECT(!mission_husk_frame(death, death.tick - 1).husked);
		if (death.plan.swap_tick > 0) TEST_EXPECT(!mission_husk_frame(death, swap - 1).husked);
		const MissionHuskFrame husked = mission_husk_frame(death, swap);
		TEST_EXPECT(husked.husked && husked.row == 12 && husked.husk == "cratehusk");
		const MissionHuskFrame later = mission_husk_frame(death, swap + 600);
		TEST_EXPECT(later.husked && later.husk == "cratehusk");
		const opennova::io::JsonValue json = rig.shots.to_json();
		const opennova::io::JsonValue *listed = json.get("deaths");
		TEST_EXPECT(listed && listed->array.size() == 1 && listed->array[0].get_string("husk", "") == "cratehusk" &&
		            int(listed->array[0].get("swap_tick")->number) == swap);
	}
	// No shot: no death.
	rig.shots.set_shots({});
	rig.shots.run_to(12);
	TEST_EXPECT(rig.shots.deaths().empty());
	std::printf("test_object_shot passed\n");
	return 0;
}

// The run is a function of its shots; the clock stepped back runs it again; an ammo ammo.def lacks is refused; the
// world built again where its files moved.
int test_the_run() {
	ShotRig rig;
	double x = 0.0, y = 0.0;
	block_middle(1, x, y);
	rig.shots.set_shots({ShotRig::down_at("AMMO_T", x, y, rig.height(x, y)),
	                     ShotRig::down_at("AMMO_T", rig.crate[0], rig.crate[1], rig.crate[2], 5)});
	rig.shots.run_to(20);
	const size_t events = rig.shots.events().size();
	const uint64_t runs = rig.shots.runs();
	TEST_EXPECT(events_of(rig.shots, MissionShotEvent::Kind::Impact).size() == 2);
	rig.shots.run_to(3);
	rig.shots.run_to(20);
	TEST_EXPECT(rig.shots.runs() > runs && rig.shots.events().size() == events);
	// Shots late on the clock: the run counts from the first of them.
	rig.shots.set_shots({ShotRig::down_at("AMMO_T", x, y, rig.height(x, y), 50000)});
	rig.shots.run_to(50010);
	const std::vector<MissionShotEvent> late = events_of(rig.shots, MissionShotEvent::Kind::Impact);
	TEST_EXPECT(late.size() == 1 && late[0].tick > 50000 && rig.shots.tick() == 50010);
	// An ammo ammo.def lacks: no round.
	rig.shots.set_shots({ShotRig::down_at("AMMO_NOPE", x, y, rig.height(x, y))});
	rig.shots.run_to(5);
	TEST_EXPECT(events_of(rig.shots, MissionShotEvent::Kind::Refused).size() == 1 &&
	            events_of(rig.shots, MissionShotEvent::Kind::Impact).empty());
	// ammo.def written again: the run starts over with the new row.
	rig.shots.set_shots({ShotRig::down_at("AMMO_T", x, y, rig.height(x, y))});
	rig.shots.run_to(10);
	TEST_EXPECT(events_of(rig.shots, MissionShotEvent::Kind::Impact).size() == 1 &&
	            events_of(rig.shots, MissionShotEvent::Kind::Impact)[0].set == "S_DIRT");
	rig.files->put_text("ammo.def", replaced(kAmmo, "obj Hit_O S_OBJ_T 15", "dirt Hit_X S_DIRT_T 15"));
	TEST_EXPECT(rig.shots.files_moved());
	rig.shots.configure(rig.setup);
	rig.shots.run_to(10);
	const std::vector<MissionShotEvent> again = events_of(rig.shots, MissionShotEvent::Kind::Impact);
	TEST_EXPECT(again.size() == 1 && again[0].set == "S_DIRT_T" && again[0].pick.from == opennova::world::ImpactRowFrom::Own);
	std::printf("test_the_run passed\n");
	return 0;
}

// The shot's wire and its refusals; the envelope.
int test_wire() {
	MissionShot shot = ShotRig::down_at("AMMO_T", 1.0, 2.0, 3.0, 7);
	JsonValue json = mission_shot_to_json(shot);
	MissionShot read;
	std::string error;
	TEST_EXPECT(read_mission_shot(json, read, error) && read == shot);
	TEST_EXPECT(!read_mission_shot(parsed(R"({"ammo": "A", "at": [1, 2]})"), read, error) &&
	            error.find("A shot is") != std::string::npos);
	TEST_EXPECT(!read_mission_shot(parsed(R"({"ammo": "", "at": [1, 2, 3], "eye": [1, 2, 4]})"), read, error));
	ShotRig rig;
	rig.shots.set_shots({ShotRig::down_at("AMMO_T", rig.crate[0], rig.crate[1], rig.crate[2])});
	rig.shots.run_to(12);
	const JsonValue envelope = rig.shots.to_json();
	TEST_EXPECT(envelope.get("shots") && envelope.get("shots")->array.size() == 1 && envelope.get_number("scars", 0) == 1);
	bool impact = false;
	if (const JsonValue *events = envelope.get("events"))
		for (const JsonValue &event : events->array)
			if (event.get_string("kind", "") == "impact") {
				impact = event.get_string("on", "") == "object" && event.get_number("record_row", 0) == 11 &&
				         event.get_number("tag", -1) == event.get_number("surface", -9) + 4 &&
				         !event.get_string("row_from", "").empty() && !event.get_string("scar", "").empty();
				if (!impact) std::printf("the impact: %s\n", opennova::io::json_write(event).c_str());
			}
	TEST_EXPECT(impact);
	std::printf("test_wire passed\n");
	return 0;
}

// The board over the same table: each class's row as the game picks it.
int test_board() {
	opennova::def::DefAmmoFile ammo{};
	const std::string text = crlf(kAmmo);
	TEST_EXPECT(opennova::def::def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(text.data()), text.size(), &ammo) == 0);
	const opennova::world::AmmoTable table = opennova::world::build_ammo_table(ammo);
	opennova::def::def_free_ammo(&ammo);
	const AmmoImpactBoard board = ammo_impact_board(table, "ammo_t");
	TEST_EXPECT(board.found && board.ammo == "AMMO_T" && board.null_rows == 12 && board.surfaces.size() == 20);
	if (board.surfaces.size() != 20) return 1;
	// Null (class 0): its own obj row. Dirt to railroad (1 to 8): AT_NULL's bank at the place, its own tag there.
	TEST_EXPECT(board.surfaces[0].pick.from == opennova::world::ImpactRowFrom::Own && board.surfaces[0].pick.sound == "S_OBJ_T");
	TEST_EXPECT(board.surfaces[2].pick.from == opennova::world::ImpactRowFrom::NullBank && board.surfaces[2].pick.bank_tag == 6 &&
	            board.surfaces[2].pick.effect == "Hit_G");
	TEST_EXPECT(board.surfaces[7].tag == 11 && board.surfaces[7].pick.effect == "Hit_W");
	// Past AT_NULL's twelve: the bank holds nothing there.
	TEST_EXPECT(board.surfaces[9].tag == 13 && board.surfaces[9].pick.bank_tag == 0 && board.surfaces[9].pick.sound.empty());
	TEST_EXPECT(ammo_impact_from_words(board, board.surfaces[9]).find("holds no row") != std::string::npos);
	// A sparse AT_NULL: the bank's place is not the tag (its fifth authored row, at place 5, is snow's).
	const std::string sparse = crlf("ammo AT_NULL\n\teffects_table\n\t\tmove none none 0\n\t\tobj none S_OBJ 15\n"
	                           "\t\tdirt none S_DIRT 15\n\t\tgrass none S_GRASS 15\n\t\tsnow none S_SNOW 15\n\tend\nend\n"
	                           "ammo AMMO_S\n\teffects_table\n\t\tmetal none S_METAL 15\n\tend\nend\n");
	opennova::def::DefAmmoFile sparse_ammo{};
	TEST_EXPECT(opennova::def::def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(sparse.data()), sparse.size(), &sparse_ammo) == 0);
	const opennova::world::AmmoTable sparse_table = opennova::world::build_ammo_table(sparse_ammo);
	opennova::def::def_free_ammo(&sparse_ammo);
	const AmmoImpactBoard sparse_board = ammo_impact_board(sparse_table, "AMMO_S");
	TEST_EXPECT(sparse_board.surfaces[1].pick.bank_tag == 7 && sparse_board.surfaces[1].pick.sound == "S_SNOW");
	TEST_EXPECT(ammo_impact_from_words(sparse_board, sparse_board.surfaces[1]).find("not its dirt row") != std::string::npos);
	TEST_EXPECT(!ammo_impact_board(table, "AMMO_NOPE").found);
	const JsonValue json = ammo_impact_board_to_json(board);
	TEST_EXPECT(json.get("surfaces") && json.get("surfaces")->array.size() == 20 && json.get("others") &&
	            json.get("others")->array.size() == 4 && !json.get_string("rule", "").empty());
	std::printf("test_board passed\n");
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_terrain_shot() == 0);
	TEST_EXPECT(test_object_shot() == 0);
	TEST_EXPECT(test_the_run() == 0);
	TEST_EXPECT(test_wire() == 0);
	TEST_EXPECT(test_board() == 0);
	std::printf("editor_mission_shots OK\n");
	return 0;
}
