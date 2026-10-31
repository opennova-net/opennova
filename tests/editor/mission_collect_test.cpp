// The mission view's collect (ADR 0046; editor/preview/mission_collect): the game's main-view collect run over the
// view's camera on the mission's records as the game's load spawns them (the editor is a small module over the
// engine: the view draws and animates what the game's main view would from the same camera). Headless, over minted
// fixtures alone: Tmap's .trn and .cpt (fixtures/terrain/tmap) and the synth crate (fixtures/threedi/synth: a 12-face
// box) drawn by an item of the catalog.
//
// Pinned: a crate ahead of the camera is admitted and one behind it dropped, each verdict naming its record's row; a
// record moved under a gesture is collected where it stands now (a crate brought in front is admitted, one taken
// behind the eye dropped); a load over no records collects nothing; a closed collect drops nothing. The world a load
// over a project's files makes runs the project's target game's rules (MissionFiles::game): a DFX2 project's holds
// DFX2's TitleRules, a JO project's JO's.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/preview/mission_collect.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_world.h>
#include <formats/mission/bms.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/title_rules.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/mission_test_support.h"

using namespace opennova::editor;

namespace {

// Each line ended CR LF, as the game's readers take a text.
std::string crlf(const std::string &text) {
	std::string out;
	for (const char c : text) {
		if (c == '\n') out += '\r';
		out += c;
	}
	return out;
}

// Files by name (any case).
struct CollectFiles final : opennova::FileSource {
	std::map<std::string, std::vector<uint8_t>> files;
	uint64_t serial = 0;
	std::map<std::string, uint64_t> stamps;
	static std::string key(const std::string &name) {
		std::string out = name;
		for (char &c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}
	void put(const std::string &name, std::vector<uint8_t> bytes) {
		files[key(name)] = std::move(bytes);
		stamps[key(name)] = ++serial;
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

// The list opens on a Null row: the load's first row is ordinal 0, which the pool-0/1 tables never list.
const char *kItems = "begin \"Null\"\nid 100000\ntype marker\nend\n"
                     "begin \"Collect Crate\"\nid 106190\ntype object\ngraphic crate\nhp 500\nend\n";

std::shared_ptr<CollectFiles> collect_files() {
	auto files = std::make_shared<CollectFiles>();
	files->put("Tmap.trn", test_io::read_file(test_paths_fixture("terrain/tmap/Tmap.trn")));
	files->put("Tmap.cpt", test_io::read_file(test_paths_fixture("terrain/tmap/Tmap.cpt")));
	files->put("crate.3di", test_io::read_file(test_paths_fixture("threedi/synth/crate.3di")));
	const std::string items = crlf(kItems);
	files->put("items.def", std::vector<uint8_t>(items.begin(), items.end()));
	return files;
}

opennova::bms::Entity crate_at(int32_t id, double x, double y, double z) {
	opennova::bms::Entity e{};
	e.type = opennova::bms::ItemType::Item;
	e.type_id = 6190;
	e.id = id;
	e.x = opennova::bms::to_fixed_16_16(x);
	e.y = opennova::bms::to_fixed_16_16(y);
	e.z = opennova::bms::to_fixed_16_16(z);
	e.team = 1;
	return e;
}

struct CollectRig {
	std::shared_ptr<CollectFiles> files = collect_files();
	MissionGround ground;
	opennova::bms::File mission;
	std::vector<MissionEntityMark> marks;
	MissionCollect collect;
	double eye[3] = {0.0, 0.0, 0.0}; // mission metres

	double height(double x, double y) const {
		return double(opennova::terrain::height_field_height_world_bilinear(
				*ground.height_field(), float(x), float(-y)));
	}
	MissionEntityMark mark_at(NodeId row, int index, double x, double y) const {
		MissionEntityMark mark;
		mark.row = row;
		mark.pool = MissionPool::Item;
		mark.index = index;
		mark.item = 106190;
		mark.x = x;
		mark.y = y;
		mark.z = height(x, y);
		return mark;
	}
	CollectRig() {
		MissionSceneHeader header;
		header.terrain = "Tmap";
		ground.follow(files, 1, header, "pad");
		// Crate 11 in block 3's middle, crate 12 in block 4's, 128 m east of it; the eye half way between them, 40 m
		// over the ground, looking west at crate 11 and 30 degrees down.
		double x = 0.0, y = 0.0;
		mission_test::block_middle(3, x, y);
		marks.push_back(mark_at(11, 0, x, y));
		mission_test::block_middle(4, x, y);
		marks.push_back(mark_at(12, 1, x, y));
		for (size_t i = 0; i < marks.size(); ++i)
			mission.items.push_back(crate_at(int32_t(101 + i), marks[i].x, marks[i].y, marks[i].z));
		eye[0] = (marks[0].x + marks[1].x) / 2.0;
		eye[1] = (marks[0].y + marks[1].y) / 2.0;
		eye[2] = height(eye[0], eye[1]) + 40.0;
	}
	void load() { collect.load(std::make_shared<MissionFiles>(files, "jo"), mission, ground, marks); }
	// The view over the eye looking west and 30 degrees down, in the presentation frame (x, z up, -y).
	opennova::world::OcclusionViewSpec view() const {
		opennova::world::OcclusionViewSpec spec;
		const float down = 30.0f * 3.14159265f / 180.0f;
		const float eye_at[3] = {float(eye[0]), float(eye[2]), float(-eye[1])};
		const float forward[3] = {-std::cos(down), -std::sin(down), 0.0f};
		const float right[3] = {0.0f, 0.0f, -1.0f};
		const float up[3] = {-std::sin(down), std::cos(down), 0.0f};
		for (int i = 0; i < 3; ++i) {
			spec.eye[i] = eye_at[i];
			spec.forward[i] = forward[i];
			spec.right[i] = right[i];
			spec.up[i] = up[i];
		}
		spec.fov_h_deg = 91.4928f; // 60 degrees vertical over a 16:9 rect
		spec.viewport_width = 1920.0f;
		spec.viewport_height = 1080.0f;
		spec.fog_dist_units = 1000.0f;
		spec.water_z_units = -100000.0f;
		return spec;
	}
	bool dropped(const MissionCollectVerdicts &verdicts, NodeId row) const {
		for (const NodeId at : verdicts.dropped)
			if (at == row) return true;
		return false;
	}
};

int test_ahead_and_behind() {
	CollectRig rig;
	TEST_EXPECT(rig.ground.height_field() != nullptr);
	rig.load();
	TEST_EXPECT(rig.collect.loaded());
	MissionCollectVerdicts verdicts;
	rig.collect.collect(rig.view(), verdicts);
	if (!(!rig.dropped(verdicts, 11) && rig.dropped(verdicts, 12))) {
		std::printf("  dropped:");
		for (const NodeId row : verdicts.dropped) std::printf(" %llu", (unsigned long long)row);
		std::printf("\n");
	}
	TEST_EXPECT(!rig.dropped(verdicts, 11)); // ahead of the eye: admitted
	TEST_EXPECT(rig.dropped(verdicts, 12)); // behind it: dropped
	TEST_EXPECT(verdicts.buildings.empty());
	// The same view again: the same verdicts (the admitted crate's latch holds).
	rig.collect.collect(rig.view(), verdicts);
	TEST_EXPECT(!rig.dropped(verdicts, 11) && rig.dropped(verdicts, 12));
	return 0;
}

int test_moves() {
	CollectRig rig;
	rig.load();
	MissionCollectVerdicts verdicts;
	// Crate 12 brought in front of the eye, 8 m north of crate 11; crate 11 taken behind the eye.
	const MissionEntityMark ahead = rig.mark_at(12, 1, rig.marks[0].x, rig.marks[0].y + 8.0);
	const MissionEntityMark behind = rig.mark_at(11, 0, rig.marks[1].x, rig.marks[1].y);
	TEST_EXPECT(rig.collect.move(ahead));
	TEST_EXPECT(rig.collect.move(behind));
	rig.collect.collect(rig.view(), verdicts);
	TEST_EXPECT(!rig.dropped(verdicts, 12));
	TEST_EXPECT(rig.dropped(verdicts, 11));
	// A row the world holds no entity of moves nothing.
	TEST_EXPECT(!rig.collect.move(rig.mark_at(99, 2, rig.marks[0].x, rig.marks[0].y)));
	// A body stood where the view draws it: crate 12 stood back behind the eye, crate 11 in front of it.
	TEST_EXPECT(rig.collect.stand(12, rig.marks[1].x, rig.marks[1].y, rig.marks[1].z + 1.0));
	TEST_EXPECT(rig.collect.stand(11, rig.marks[0].x, rig.marks[0].y, rig.marks[0].z + 1.0));
	rig.collect.collect(rig.view(), verdicts);
	TEST_EXPECT(rig.dropped(verdicts, 12) && !rig.dropped(verdicts, 11));
	TEST_EXPECT(!rig.collect.stand(99, 0.0, 0.0, 0.0));
	return 0;
}

int test_empty_and_closed() {
	CollectRig rig;
	MissionCollectVerdicts verdicts;
	// Not loaded: nothing dropped.
	rig.collect.collect(rig.view(), verdicts);
	TEST_EXPECT(verdicts.dropped.empty() && verdicts.buildings.empty());
	// No records: nothing to collect.
	rig.mission.items.clear();
	rig.marks.clear();
	rig.load();
	rig.collect.collect(rig.view(), verdicts);
	TEST_EXPECT(verdicts.dropped.empty());
	rig.collect.close();
	TEST_EXPECT(!rig.collect.loaded());
	return 0;
}

// The world loaded over a project's files runs that project's target game's rules, never another title's: a DFX2
// project's holds DFX2's (its own TitleRules row, not Joint Operations').
int test_project_game_rules() {
	namespace world = opennova::world;
	CollectRig rig;
	TEST_EXPECT(&world::title_rules("dfx2") != &world::title_rules("jo"));
	for (const char *game : {"dfx2", "jo"}) {
		MissionWorld loaded;
		loaded.load(std::make_shared<MissionFiles>(rig.files, game), rig.mission, rig.ground);
		TEST_EXPECT(loaded.world.rules.title_rules == &world::title_rules(game));
	}
	return 0;
}

} // namespace

int main() {
	TEST_EXPECT(test_ahead_and_behind() == 0);
	TEST_EXPECT(test_moves() == 0);
	TEST_EXPECT(test_empty_and_closed() == 0);
	TEST_EXPECT(test_project_game_rules() == 0);
	std::printf("editor_mission_collect OK\n");
	return 0;
}
