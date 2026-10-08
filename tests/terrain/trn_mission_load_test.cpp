// D-TERRAIN-18: a mission's terrain configuration reads overcast.def and the mission's .env after its .trn,
// through the terrain's parser, its state carried across the three [orig: Terrain_LoadEnvironmentConfig
// @ 0x6109ad installs Terrain_ParseConfigCallback as the time-of-day load's hook; Environment_LoadTimeOfDayConfig
// @ 0x57db44 sets it once, the passes @ 0x57dbeb / @ 0x57dc3b / @ 0x57dcbf]. A terrain key in either later file
// is the terrain's, the .env's over overcast.def's over the .trn's; the .trn alone (load_trn, the terrain
// document's read) is unchanged. The runtime's own load (terrain_field_store_load) takes the .env by the
// mission's name. Retail leg: every JO:CA terrain under overcast.def and each shipped .env makes the configuration
// its .trn alone makes (no shipped file writes a terrain key after the .trn).

#include <formats/trn/trn.h>
#include <formats/trn/trn_io.h>

#include <base/vfs/file_source.h>
#include <base/vfs/vfs.h>
#include <runtime/terrain_query/terrain_field_build.h>
#include <runtime/terrain_query/terrain_field_store.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"

namespace {

using opennova::TrnConfig;
using opennova::TrnLaterLine;
using opennova::TrnLaterTexts;

std::string fixture(const std::string &relative) {
	return std::string(test_paths_repo_root(__FILE__)) + "/fixtures/" + relative;
}

std::string text_of(const std::vector<uint8_t> &bytes) { return std::string(bytes.begin(), bytes.end()); }

// CR LF lines, as the game's line reader cuts a file.
std::string crlf(const char *text) {
	std::string out;
	for (const char *c = text; *c; ++c) out += *c == '\n' ? std::string("\r\n") : std::string(1, *c);
	return out;
}

std::string lower(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return s;
}

// A project's files by flat name, compared without case.
class MemoryFiles : public opennova::FileSource {
public:
	void put(const std::string &name, std::vector<uint8_t> bytes) {
		files_[lower(name)] = std::move(bytes);
		++serial_;
	}
	void put(const std::string &name, const std::string &text) { put(name, std::vector<uint8_t>(text.begin(), text.end())); }
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const auto it = files_.find(lower(name));
		if (it == files_.end()) return false;
		out = it->second;
		return true;
	}
	uint64_t stamp(const std::string &name) const override { return files_.count(lower(name)) ? serial_ : 0; }

private:
	std::map<std::string, std::vector<uint8_t>> files_;
	uint64_t serial_ = 1;
};

int run() {
	const std::string trn = text_of(test_io::read_file(fixture("terrain/tmap/Tmap.trn")));
	TEST_EXPECT(!trn.empty());
	std::string error;

	// The .trn alone, as the terrain document reads it.
	TrnConfig alone;
	{
		std::istringstream in(trn);
		TEST_EXPECT(opennova::load_trn(in, alone, error));
	}
	TEST_EXPECT(alone.detail_density == 128 && alone.wrap_x == 0 && alone.origin_x == -4 && alone.sector_rows == 8);

	// A mission's .env carrying terrain keys among its own (a key compares without case, a comment cuts a
	// line), and the environment keywords the .trn holds as written untouched by the .env's.
	const std::string environment = crlf("enviro_name \"T18\"\n"
	                                     "envscale 1.0\n"
	                                     "water_murk 0.6\n"
	                                     "water_height 40\n"
	                                     "POLYTRN_DETAILDENSITY 64 ; the .env's\n"
	                                     "polytrn_wrapx 1\n"
	                                     "fog_level 900\n");
	TrnConfig mission;
	std::vector<TrnLaterLine> taken;
	TrnLaterTexts later;
	later.environment = &environment;
	TEST_EXPECT(opennova::load_mission_trn(trn, later, mission, error, &taken));
	TEST_EXPECT(mission.detail_density == 64 && mission.wrap_x == 1);
	// Everything else is the .trn's: its grid, its origin, its foliage, its water as it writes it.
	TEST_EXPECT(mission.origin_x == -4 && mission.sector_rows == 8 && mission.sector_grid[3][3] == 1 &&
	            mission.foliage_defs.size() == alone.foliage_defs.size() && mission.colormap == alone.colormap);
	TEST_EXPECT(mission.water_height == 21 && !mission.water_murk_set);
	TEST_EXPECT(taken.size() == 2);
	TEST_EXPECT(taken[0].file == TrnLaterLine::File::Environment && taken[0].line == 5 &&
	            taken[0].key == "polytrn_detaildensity" && taken[0].value == "64");
	TEST_EXPECT(taken[1].key == "polytrn_wrapx" && taken[1].line == 6);

	// overcast.def before the .env: the .env's key over overcast.def's, overcast.def's over the .trn's.
	const std::string overcast = crlf("tod_begin\ntime 0600\nsun_rgb 10,10,10\ntod_end\n"
	                                  "polytrn_detaildensity 32\npolytrn_detaildensity2 4\n");
	later.overcast = &overcast;
	TEST_EXPECT(opennova::load_mission_trn(trn, later, mission, error, &taken));
	TEST_EXPECT(mission.detail_density == 64 && mission.detail_density2 == 4);
	TEST_EXPECT(taken.size() == 4 && taken[0].file == TrnLaterLine::File::Overcast && taken[0].line == 5 &&
	            taken[2].file == TrnLaterLine::File::Environment);
	later.environment = nullptr;
	TEST_EXPECT(opennova::load_mission_trn(trn, later, mission, error, &taken));
	TEST_EXPECT(mission.detail_density == 32 && mission.detail_density2 == 4 && mission.wrap_x == 0);

	// A later file's polytrn_sectors line is a row after the .trn's eight: nine, which the gate refuses
	// [orig: dword_31BCB30 carried, the power-of-two test @ 0x610a60].
	const std::string row = crlf("polytrn_sectors 0 0 0 0 0 0 0 0\n");
	later = TrnLaterTexts();
	later.environment = &row;
	TEST_EXPECT(!opennova::load_mission_trn(trn, later, mission, error));
	TEST_EXPECT(error.find("row count 9") != std::string::npos);

	// A foliage block the .trn leaves open reads the next file's lines [orig: dword_31BC904 carried]: here
	// overcast.def names its graphic and closes it, and the .env's foliage is a second block.
	const std::string open_block = trn + crlf("foliage\ncolor_lower 1\n");
	const std::string closes = crlf("graphic tree9.3di\ncolor_upper 2\nend\n");
	const std::string second = crlf("foliage\ngraphic fern.3di\nend\npolytrn_detaildensity 16\n");
	later = TrnLaterTexts();
	later.overcast = &closes;
	later.environment = &second;
	TEST_EXPECT(opennova::load_mission_trn(open_block, later, mission, error, &taken));
	const size_t own = alone.foliage_defs.size(); // the .trn's closed blocks
	TEST_EXPECT(own == 2 && mission.foliage_defs.size() == own + 2 && mission.foliage_defs[own].graphic == "tree9.3di" &&
	            mission.foliage_defs[own].color_lower == 1 && mission.foliage_defs[own].color_upper == 2 &&
	            mission.foliage_defs[own + 1].graphic == "fern.3di" && mission.detail_density == 16);
	// The .trn alone keeps its open block, read as a definition at the end of the file.
	{
		TrnConfig open_alone;
		std::istringstream in(open_block);
		TEST_EXPECT(opennova::load_trn(in, open_alone, error) && open_alone.foliage_defs.size() == own + 1 &&
		            open_alone.foliage_defs[own].graphic.empty());
	}

	// By name: overcast.def by its own, the .env by the mission's; a mission that names none reads no .env.
	MemoryFiles files;
	files.put("Tmap.trn", trn);
	files.put("Tmap.cpt", test_io::read_file(fixture("terrain/tmap/Tmap.cpt")));
	files.put("t18.env", environment);
	const opennova::TrnTextReader read = [&files](const std::string &name, std::string &text) {
		std::vector<uint8_t> bytes;
		if (!files.read(name, bytes)) return false;
		text = text_of(bytes);
		return true;
	};
	TEST_EXPECT(opennova::read_mission_trn(read, "Tmap.trn", "T18.env", mission, error) && mission.detail_density == 64);
	TEST_EXPECT(opennova::read_mission_trn(read, "Tmap.trn", "", mission, error) && mission.detail_density == 128);
	TEST_EXPECT(!opennova::read_mission_trn(read, "Nope.trn", "t18.env", mission, error));

	// The runtime's load: the store's wrap is the .env's.
	{
		opennova::terrain::TerrainFieldStore store;
		TEST_EXPECT(opennova::terrain::terrain_field_store_load(store, files, "Tmap", "", "", error));
		TEST_EXPECT(store.valid() && !store.height_field().wrap_x);
		opennova::terrain::TerrainFieldStore with_env;
		TrnConfig loaded;
		TEST_EXPECT(opennova::terrain::terrain_field_store_load(with_env, files, "Tmap", "", "t18", error, &loaded));
		TEST_EXPECT(with_env.valid() && with_env.height_field().wrap_x && loaded.detail_density == 64);
	}

	// What a preview keys the terrain on: the lines the parser can take. An edit of the .env that sets no
	// terrain key leaves them as they were; one that does changes them.
	TEST_EXPECT(opennova::trn_parser_lines(crlf("envscale 1.0\nfog_level 900\n")).empty());
	const std::string keyed = opennova::trn_parser_lines(environment);
	TEST_EXPECT(!keyed.empty());
	std::string edited = environment;
	edited.replace(edited.find("fog_level 900"), 13, "fog_level 700");
	TEST_EXPECT(opennova::trn_parser_lines(edited) == keyed);
	edited.replace(edited.find("DETAILDENSITY 64"), 16, "DETAILDENSITY 96");
	TEST_EXPECT(opennova::trn_parser_lines(edited) != keyed);

	std::printf("OK: a mission's terrain reads overcast.def and its .env after the .trn\n");
	return 0;
}

std::string saved(const TrnConfig &config) {
	std::ostringstream out;
	std::string error;
	return opennova::save_trn(out, config, error) ? out.str() : std::string("(unwritable) ") + error;
}

// Every terrain of the install under its overcast.def and each of its .env files (and none): the configuration the
// .trn alone makes, no later line taken.
int run_retail() {
	const std::string install = retail::install();
	if (install.empty()) {
		retail::skip_leg("OPENNOVA_JO_DIR (every terrain under overcast.def and each shipped .env)");
		return 0;
	}
	opennova::Vfs vfs;
	TEST_EXPECT(vfs.mount_game(install.c_str(), std::string(), opennova::VfsMountMode::Packed));
	std::vector<std::string> terrains, environments;
	for (const auto &location : vfs.list_files()) {
		const std::string name = lower(location.logical_name);
		if (name.size() < 4) continue;
		const std::string extension = name.substr(name.size() - 4);
		if (extension == ".trn") terrains.push_back(location.logical_name);
		if (extension == ".env") environments.push_back(location.logical_name);
	}
	std::vector<uint8_t> bytes;
	TEST_EXPECT(vfs.read_file_raw("overcast.def", bytes) && !bytes.empty());
	const std::string overcast = text_of(bytes);
	std::vector<std::string> environment_texts(1); // the first: none
	for (const std::string &name : environments) {
		TEST_EXPECT(vfs.read_file_raw(name, bytes));
		environment_texts.push_back(text_of(bytes));
	}
	size_t loads = 0;
	for (const std::string &name : terrains) {
		TEST_EXPECT(vfs.read_file_raw(name, bytes));
		const std::string trn = text_of(bytes);
		TrnConfig alone;
		std::string alone_error;
		std::istringstream in(trn);
		const bool alone_ok = opennova::load_trn(in, alone, alone_error);
		for (size_t e = 0; e < environment_texts.size(); ++e) {
			TrnLaterTexts later;
			later.overcast = &overcast;
			if (e > 0) later.environment = &environment_texts[e];
			TrnConfig mission;
			std::string error;
			std::vector<TrnLaterLine> taken;
			const bool ok = opennova::load_mission_trn(trn, later, mission, error, &taken);
			if (ok != alone_ok || error != alone_error || saved(mission) != saved(alone) || !taken.empty())
				std::fprintf(stderr, "FAIL: %s under %s reads otherwise than alone\n", name.c_str(),
				             e > 0 ? environments[e - 1].c_str() : "no .env");
			TEST_EXPECT(ok == alone_ok && error == alone_error && saved(mission) == saved(alone) && taken.empty());
			++loads;
		}
	}
	std::printf("retail: %zu terrains under overcast.def and %zu environments (%zu loads) read as the .trn alone\n",
	            terrains.size(), environments.size(), loads);
	TEST_EXPECT(terrains.size() >= 30 && environments.size() >= 10);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	if (const int failed = run()) return failed;
	return run_retail();
}
