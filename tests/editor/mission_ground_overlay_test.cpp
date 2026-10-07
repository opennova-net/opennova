// The ground's surface classes and foliage over a mission's terrain (ADR 0046 DI-29): over Tmap's .trn and .cpt
// (fixtures/terrain/tmap: the island layout, its foliage blocks bush1.3di matching 254 and bush2.3di matching 253)
// with a minted char map painting every class at a known place and a minted foliage map, read through the
// runtime's own load: the ground's foliage at a point (the map's code, the definitions it selects, what a placed
// tile keeps off) on the wire and in its line; the surface overlay (its extent the mapped sectors at the char
// map's texel, each texel the class the game's sampler reads with the legend's colour, a deciding tile outlined,
// the ocean's 7 past it, the legend's classes and shares); the foliage overlay (each definition's colour where it
// grows, a placed tile's square grey, the legend's definitions); no foliage map and no char map; the options'
// overlay on the wire. With --retail, both overlays over a JO:CA terrain (Dvxi5) as shipped: nothing in them is
// the base game's.
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_ground_overlay.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/mission_scene.h>
#include <formats/mission/bms.h>
#include <formats/pcx/pcx_io.h>
#include <formats/til/til_io.h>
#include <formats/trn/charmap_legend.h>

#include <base/resource_index/resource_index.h>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

std::string fixture(const std::string &rel) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + rel;
}

// Files by name (any case), each write moving its stamp.
struct GroundFiles final : opennova::FileSource {
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
	void drop(const std::string &name) {
		files.erase(key(name));
		stamps.erase(key(name));
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

std::vector<uint8_t> pcx_of(const opennova::IndexedImage8 &map) {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::encode_pcx_indexed(map, bytes, error)) std::fprintf(stderr, "the map: %s\n", error.c_str());
	return bytes;
}

// The block of 32 texels whose corner is texel (16 + 32 (c % 5), 16 + 32 (c / 5)): class c's in the char map.
template <typename Paint>
void paint_block(opennova::IndexedImage8 &map, int c, Paint value) {
	for (int z = 0; z < 32; ++z)
		for (int x = 0; x < 32; ++x)
			map.indices[size_t(16 + 32 * (c / 5) + z) * kSide + size_t(16 + 32 * (c % 5) + x)] = uint8_t(value);
}

// The char map: class c in its block, dirt (1) elsewhere, the legend its palette.
std::vector<uint8_t> charmap() {
	opennova::IndexedImage8 map;
	map.width = map.height = kSide;
	map.indices.assign(size_t(kSide) * kSide, 1);
	for (int c = 0; c < opennova::kCharmapLegendCount; ++c) paint_block(map, c, c);
	return pcx_of(map);
}

// The foliage map: 254 (bush1.3di) over class 0's block, 253 (bush2.3di) over class 1's, 77 (no definition)
// over class 2's, 0 elsewhere.
std::vector<uint8_t> foliage_map() {
	opennova::IndexedImage8 map;
	map.width = map.height = kSide;
	map.indices.assign(size_t(kSide) * kSide, 0);
	paint_block(map, 0, 254);
	paint_block(map, 1, 253);
	paint_block(map, 2, 77);
	return pcx_of(map);
}

// Class c's block's middle texel, mission metres (the island layout: texel (512 + x, 512 - y) of the
// 1024-unit atlas, a 256 map's texel four units), at the texel's middle.
void block_middle(int c, double &x, double &y) {
	const int tx = 32 + 32 * (c % 5), tz = 32 + 32 * (c / 5);
	x = 4.0 * tx - 512.0 + 2.0;
	y = 512.0 - 4.0 * tz - 2.0;
}

std::vector<uint8_t> tile_at(double x, double y, uint8_t index) {
	opennova::TilFile til;
	opennova::TilOverlayEntry entry;
	entry.x_fixed = opennova::bms::to_fixed_16_16(x);
	entry.z_fixed = -opennova::bms::to_fixed_16_16(y);
	entry.tile_index = index;
	til.entries.push_back(entry);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::save_til(til, bytes, error)) std::fprintf(stderr, "the .til: %s\n", error.c_str());
	return bytes;
}

std::shared_ptr<GroundFiles> tmap_files() {
	auto files = std::make_shared<GroundFiles>();
	files->put("Tmap.trn", test_io::read_file(fixture("terrain/tmap/Tmap.trn")));
	files->put("Tmap.cpt", test_io::read_file(fixture("terrain/tmap/Tmap.cpt")));
	files->put("Tmap_m.pcx", charmap());
	files->put("Tmap_f.pcx", foliage_map());
	return files;
}

// The texel of the picture at mission (x, y).
const uint8_t *texel_at(const MissionOverlayImage &image, double x, double y) {
	const int i = int(std::floor((x - image.west) / image.texel));
	const int j = int(std::floor((image.north - y) / image.texel));
	if (i < 0 || j < 0 || i >= image.width || j >= image.height) return nullptr;
	return &image.rgba[(size_t(j) * size_t(image.width) + size_t(i)) * 4];
}

bool rgb_is(const uint8_t *p, int r, int g, int b) {
	return p && p[0] == r && p[1] == g && p[2] == b;
}

const MissionOverlayRow *row_of(const MissionOverlayImage &image, const std::string &key) {
	for (const MissionOverlayRow &row : image.legend)
		if (row.key == key) return &row;
	return nullptr;
}

// The foliage at a point: the map's code, what grows on it by the .trn's definitions, its words and wire form;
// a placed tile's square keeping the definitions off; no foliage map.
int test_foliage_facts() {
	auto files = tmap_files();
	MissionSceneHeader header;
	header.terrain = "Tmap";
	MissionGround ground;
	ground.follow(files, 1, header, "pad");
	TEST_EXPECT(ground.terrain() && ground.foliage_map() == "Tmap_f.pcx" && ground.foliage_defs().size() == 2);
	double x = 0.0, y = 0.0;
	block_middle(0, x, y);
	MissionGroundFacts facts = ground.terrain_at(x, y, 30.0);
	TEST_EXPECT(facts.foliage_code == 254 && facts.grows.size() == 1 && facts.kept_off.empty());
	if (facts.grows.size() == 1) TEST_EXPECT(facts.grows[0].slot == 0 && facts.grows[0].graphic == "bush1.3di");
	TEST_EXPECT(mission_ground_line(facts).find(" Foliage code 254: grows bush1.3di (foliage 1).") != std::string::npos);
	JsonValue json = mission_ground_to_json(facts);
	const JsonValue *foliage = json.get("foliage");
	TEST_EXPECT(foliage && foliage->get_number("code", -1) == 254 && foliage->get("grows") &&
	            foliage->get("grows")->array.size() == 1 && foliage->get("grows")->array[0].get_string("graphic", "") == "bush1.3di");
	block_middle(1, x, y);
	facts = ground.terrain_at(x, y, 30.0);
	TEST_EXPECT(facts.foliage_code == 253 && facts.grows.size() == 1 && facts.grows[0].slot == 1);
	block_middle(2, x, y);
	facts = ground.terrain_at(x, y, 30.0);
	TEST_EXPECT(facts.foliage_code == 77 && facts.grows.empty() &&
	            mission_ground_line(facts).find("Foliage code 77: no definition matches it") != std::string::npos);
	block_middle(3, x, y);
	facts = ground.terrain_at(x, y, 30.0);
	TEST_EXPECT(facts.foliage_code == 0 && mission_ground_line(facts).find(" No foliage (code 0).") != std::string::npos);
	// Off the island: an empty sector grows nothing (the sampler's 0).
	facts = ground.terrain_at(1500.0, 0.0, 30.0);
	TEST_EXPECT(facts.foliage_code == 0 && facts.grows.empty());

	// A placed tile's square keeps a definition without forceon off within 2 units of it.
	block_middle(0, x, y);
	files->put("pad.til", tile_at(x + 1.0, y - 8.0, 0));
	ground.follow(files, 2, header, "pad");
	facts = ground.terrain_at(x, y, 30.0);
	TEST_EXPECT(facts.foliage_code == 254 && facts.grows.empty() && facts.kept_off.size() == 1 &&
	            mission_ground_line(facts).find("bush1.3di (foliage 1) kept off by a placed tile") != std::string::npos);
	TEST_EXPECT(ground.foliage_kept_off(x, y, 1) == 1 && ground.foliage_kept_off(x - 40.0, y, 1) == 0);
	json = mission_ground_to_json(facts);
	TEST_EXPECT(json.get("foliage") && json.get("foliage")->get("kept_off") &&
	            json.get("foliage")->get("kept_off")->array.size() == 1);

	// No foliage map: nothing said of foliage.
	files->drop("Tmap_f.pcx");
	ground.follow(files, 3, header, "pad");
	facts = ground.terrain_at(x, y, 30.0);
	TEST_EXPECT(ground.foliage_map().empty() && facts.foliage_code == -1 &&
	            mission_ground_line(facts).find("Foliage") == std::string::npos);
	TEST_EXPECT(mission_ground_to_json(facts).get("foliage") && mission_ground_to_json(facts).get("foliage")->is_null());
	std::printf("test_foliage_facts passed\n");
	return 0;
}

// The surface overlay: the mapped sectors at the char map's texel, each class's legend colour where the game
// reads it, a deciding tile outlined and named, the ocean's 7 past the picture, the legend's shares.
int test_surface_overlay() {
	auto files = tmap_files();
	MissionSceneHeader header;
	header.terrain = "Tmap";
	MissionGround ground;
	ground.follow(files, 1, header, "pad");
	MissionOverlayImage image = mission_ground_overlay(ground, MissionGroundOverlay::Surfaces);
	TEST_EXPECT(image.kind == MissionGroundOverlay::Surfaces && image.west == -512.0 && image.north == 512.0 &&
	            image.texel == 4.0 && image.width == 256 && image.height == 256 && image.rgba.size() == size_t(256) * 256 * 4);
	TEST_EXPECT(image.title.find("Tmap_m.pcx (256 a side)") != std::string::npos);
	for (int c = 0; c < opennova::kCharmapLegendCount; ++c) {
		double x = 0.0, y = 0.0;
		block_middle(c, x, y);
		const uint8_t *p = texel_at(image, x, y);
		const opennova::CharmapLegendColour &want = opennova::kCharmapLegend[c];
		TEST_EXPECT(rgb_is(p, want.r, want.g, want.b) && p[3] == kMissionOverlayAlpha);
		const MissionOverlayRow *row = row_of(image, std::to_string(c));
		TEST_EXPECT(row && row->rgb[0] == want.r && std::fabs(row->share - (c == 1 ? 1.0 - 19.0 * 1024.0 / 65536.0 : 1024.0 / 65536.0)) < 1e-9);
	}
	TEST_EXPECT(row_of(image, "3") && row_of(image, "3")->words.find("Snow (TSD_SNOW): the snow footsteps") == 0);
	// Past the picture, the grid's empty sectors: the ocean's 7.
	const opennova::CharmapLegendColour &sea = opennova::kCharmapLegend[7];
	TEST_EXPECT(image.outside && image.outside_rgba[0] == sea.r && image.outside_rgba[2] == sea.b &&
	            image.outside_rgba[3] == kMissionOverlayAlpha && image.words.find("Underwater (surface 7)") != std::string::npos);
	TEST_EXPECT(!row_of(image, "tile"));

	// A placed tile over the snow decides there (TSD_NULL): its square reads 0, outlined, a legend row.
	double sx = 0.0, sy = 0.0;
	block_middle(3, sx, sy);
	files->put("pad.til", tile_at(sx, sy, 5));
	ground.follow(files, 2, header, "pad");
	image = mission_ground_overlay(ground, MissionGroundOverlay::Surfaces);
	const opennova::CharmapLegendColour &null = opennova::kCharmapLegend[0];
	TEST_EXPECT(rgb_is(texel_at(image, sx + 8.0, sy + 8.0), null.r, null.g, null.b));
	const uint8_t *edge = texel_at(image, sx + 0.5, sy + 8.0);
	TEST_EXPECT(rgb_is(edge, 255, 255, 255) && edge[3] == 255);
	TEST_EXPECT(row_of(image, "tile") && row_of(image, "tile")->words.find("1 placed tile(s)") == 0);
	const opennova::CharmapLegendColour &snow = opennova::kCharmapLegend[3];
	TEST_EXPECT(rgb_is(texel_at(image, sx + 40.0, sy), snow.r, snow.g, snow.b));

	// No char map: 1 everywhere, no picture, the one value past it.
	files->drop("Tmap_m.pcx");
	ground.follow(files, 3, header, "pad");
	image = mission_ground_overlay(ground, MissionGroundOverlay::Surfaces);
	const opennova::CharmapLegendColour &dirt = opennova::kCharmapLegend[1];
	TEST_EXPECT(image.empty() && image.outside && image.outside_rgba[0] == dirt.r &&
	            image.words.find("no char map") != std::string::npos && image.legend.size() == 1);
	// No terrain: nothing, and why.
	MissionGround none;
	MissionSceneHeader empty;
	none.follow(files, 1, empty, "pad");
	image = mission_ground_overlay(none, MissionGroundOverlay::Surfaces);
	TEST_EXPECT(image.empty() && !image.outside && !image.words.empty());
	std::printf("test_surface_overlay passed\n");
	return 0;
}

// The foliage overlay: each definition's colour where the map selects it, nothing where it selects none, a
// placed tile's square grey; its legend the definitions that grow, their codes and shares; no foliage map.
int test_foliage_overlay() {
	auto files = tmap_files();
	MissionSceneHeader header;
	header.terrain = "Tmap";
	MissionGround ground;
	ground.follow(files, 1, header, "pad");
	MissionOverlayImage image = mission_ground_overlay(ground, MissionGroundOverlay::Foliage);
	TEST_EXPECT(image.width == 256 && image.height == 256 && image.title.find("Tmap_f.pcx (256 a side)") != std::string::npos);
	double x = 0.0, y = 0.0;
	block_middle(0, x, y);
	const uint8_t *bush1 = texel_at(image, x, y);
	block_middle(1, x, y);
	const uint8_t *bush2 = texel_at(image, x, y);
	block_middle(2, x, y);
	const uint8_t *none = texel_at(image, x, y);
	TEST_EXPECT(bush1 && bush2 && none && bush1[3] == kMissionOverlayAlpha && bush2[3] == kMissionOverlayAlpha && none[3] == 0);
	TEST_EXPECT(bush1 && bush2 && (bush1[0] != bush2[0] || bush1[1] != bush2[1] || bush1[2] != bush2[2]));
	const MissionOverlayRow *first = row_of(image, "foliage 1");
	const MissionOverlayRow *second = row_of(image, "foliage 2");
	TEST_EXPECT(first && second && first->words == "grows bush1.3di (foliage 1, codes 254)" &&
	            second->words == "grows bush2.3di (foliage 2, codes 253)" && std::fabs(first->share - 1024.0 / 65536.0) < 1e-9);
	TEST_EXPECT(image.outside && image.outside_rgba[3] == 0 && image.legend.size() == 2);

	// A placed tile in the bush1 block: its square, 2 units round, kept off and grey.
	block_middle(0, x, y);
	files->put("pad.til", tile_at(x, y, 0));
	ground.follow(files, 2, header, "pad");
	image = mission_ground_overlay(ground, MissionGroundOverlay::Foliage);
	TEST_EXPECT(rgb_is(texel_at(image, x + 8.0, y + 8.0), 128, 128, 128) && row_of(image, "kept off"));
	const uint8_t *beside = texel_at(image, x + 30.0, y + 8.0);
	TEST_EXPECT(beside && beside[3] == kMissionOverlayAlpha && !rgb_is(beside, 128, 128, 128));

	// No foliage map: no picture, nothing grows.
	files->drop("Tmap_f.pcx");
	ground.follow(files, 3, header, "pad");
	image = mission_ground_overlay(ground, MissionGroundOverlay::Foliage);
	TEST_EXPECT(image.empty() && image.legend.empty() && image.words.find("no foliage map") != std::string::npos);
	std::printf("test_foliage_overlay passed\n");
	return 0;
}

JsonValue parsed(const char *text) {
	JsonValue out;
	std::string error;
	if (!opennova::io::json_parse(text, out, error)) std::fprintf(stderr, "json: %s\n", error.c_str());
	return out;
}

// The options' overlay on the wire: its token, set and refused; and the overlay's own wire form.
int test_overlay_wire() {
	MissionViewportOptions options;
	TEST_EXPECT(mission_options_to_json(options).get_string("overlay", "") == "none");
	std::string error;
	TEST_EXPECT(mission_options_from_json(parsed(R"({"overlay": "surfaces"})"), options, error) &&
	            options.overlay == MissionGroundOverlay::Surfaces);
	TEST_EXPECT(mission_options_from_json(parsed(R"({"overlay": "foliage"})"), options, error) &&
	            options.overlay == MissionGroundOverlay::Foliage && options != MissionViewportOptions());
	TEST_EXPECT(!mission_options_from_json(parsed(R"({"overlay": "rain"})"), options, error) &&
	            error.find("none, surfaces") != std::string::npos && options.overlay == MissionGroundOverlay::Foliage);
	auto files = tmap_files();
	MissionSceneHeader header;
	header.terrain = "Tmap";
	MissionGround ground;
	ground.follow(files, 1, header, "pad");
	const JsonValue json = mission_overlay_to_json(mission_ground_overlay(ground, MissionGroundOverlay::Surfaces));
	TEST_EXPECT(json.get_string("kind", "") == "surfaces" && json.get_number("width", 0) == 256 && json.get_number("texel", 0) == 4 &&
	            json.get_number("west", 0) == -512 && json.get("legend") && json.get("legend")->array.size() == 20 &&
	            json.get("outside") && json.get("outside")->array.size() == 4);
	std::printf("test_overlay_wire passed\n");
	return 0;
}

// The game's files through its own index, each present one stamped alike.
struct InstallFiles final : opennova::FileSource {
	opennova::ResourceIndex index;
	bool read(const std::string &name, std::vector<uint8_t> &out) const override { return index.read_file(name, out); }
	uint64_t stamp(const std::string &name) const override { return index.has_file(name) ? 1 : 0; }
};

// With --retail, a JO:CA terrain (Dvxi5, nothing of the base game's): its char map (512 a side, class 1 at every
// texel) read as one class over its four mapped sectors at its own texel, the ocean past them; its foliage map
// (256 a side) to its two shipped definitions, mveg5b.3di on 254 and mveg5.3di on 253, each by the share of the
// texels the map gives its code, the 255s growing nothing.
int test_retail_overlays() {
	const std::string install = retail::install();
	if (install.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (a shipped terrain, Dvxi5)");
	auto files = std::make_shared<InstallFiles>();
	TEST_EXPECT(files->index.scan(install));
	MissionSceneHeader header;
	header.terrain = "Dvxi5";
	MissionGround ground;
	ground.follow(files, 1, header, "");
	TEST_EXPECT(ground.terrain() && ground.surface_map() == "Dvxi5_m.pcx" && ground.foliage_map() == "Dvxi5_f.pcx");
	MissionOverlayImage image = mission_ground_overlay(ground, MissionGroundOverlay::Surfaces);
	TEST_EXPECT(image.width == 512 && image.height == 512 && image.texel == 2.0 && image.west == -512.0 &&
	            image.north == 512.0 && image.legend.size() == 1 && image.outside);
	TEST_EXPECT(row_of(image, "1") && row_of(image, "1")->share == 1.0);
	image = mission_ground_overlay(ground, MissionGroundOverlay::Foliage);
	TEST_EXPECT(image.width == 256 && image.texel == 4.0 && image.legend.size() == 2);
	const MissionOverlayRow *first = row_of(image, "foliage 1");
	const MissionOverlayRow *second = row_of(image, "foliage 2");
	TEST_EXPECT(first && first->words == "grows mveg5b.3di (foliage 1, codes 254)" && std::fabs(first->share - 13293.0 / 65536.0) < 1e-9);
	TEST_EXPECT(second && second->words == "grows mveg5.3di (foliage 2, codes 253)" && std::fabs(second->share - 5888.0 / 65536.0) < 1e-9);
	std::printf("test_retail_overlays passed\n");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	TEST_EXPECT(test_retail_overlays() == 0);
	TEST_EXPECT(test_foliage_facts() == 0);
	TEST_EXPECT(test_surface_overlay() == 0);
	TEST_EXPECT(test_foliage_overlay() == 0);
	TEST_EXPECT(test_overlay_wire() == 0);
	std::printf("editor_mission_ground_overlay: all tests passed\n");
	return 0;
}
