#pragma once

#include "trn.h"

#include <cstdint>
#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace opennova {

// Parses a .trn config. Returns false (with `error` set) when the retail
// admission gate rejects it: empty polytrn_colormap / polytrn_detailmap /
// polytrn_polydata, or a polytrn_sectors row count or polytrn_sectorcount
// that is > 16 or not a power of two [orig: Terrain_LoadEnvironmentConfig
// @0x610940]. `out` is left partially filled on rejection. The .trn alone, as
// its file holds it (the terrain document's read); a mission's terrain is
// load_mission_trn's.
bool load_trn(std::istream &input, TrnConfig &out, std::string &error);
bool save_trn(std::ostream &output, const TrnConfig &cfg, std::string &error);

// The files a mission's terrain configuration reads after its .trn (D-TERRAIN-18), each null where it is not
// there: overcast.def (env::kOvercastFile) and the mission's <environment>.env, neither when the mission names
// no environment [orig: Environment_LoadTimeOfDayConfig @ 0x57dc23, @ 0x57dc6a, @ 0x57dca3].
struct TrnLaterTexts {
	const std::string *overcast = nullptr;
	const std::string *environment = nullptr;
};

// A line of overcast.def or the mission's .env that set the terrain's configuration (D-TERRAIN-18): which file, its
// line (1-based, every CR LF line of the file counted), its keyword (lower case) and its values (the tokens after
// the keyword, a space apart).
struct TrnLaterLine {
	enum class File : uint8_t { Overcast, Environment };
	File file = File::Overcast;
	int line = 0;
	std::string key;
	std::string value;
};

// A mission's terrain configuration as its load reads it [orig: Terrain_LoadEnvironmentConfig @ 0x610940]: the
// configuration reset to its defaults [orig: the memset @ 0x610959, @ 0x61096d..0x610981, the block state
// @ 0x6109b4..0x6109ba], then the terrain's parser over every line of three files in turn, its state carried across
// them, before the admission gate: the load installs the parser as the time-of-day load's hook [orig: @ 0x6109ad ->
// Environment_LoadTimeOfDayConfig @ 0x57db44, Env_ParseHook], which every line of the .trn, of overcast.def and of
// the mission's .env reaches [orig: the passes @ 0x57dbeb, @ 0x57dc3b, @ 0x57dcbf; TimeOfDay_ParseProperty
// @ 0x57c5ac calls the hook on each] and which no pass removes. So a key a later file writes is the terrain's
// (overcast.def's over the .trn's, the .env's over both), a `polytrn_sectors` line there adds a row after the
// .trn's [orig: dword_31BCB30 not reset between the passes], and a foliage block the .trn leaves open reads the
// next file's lines [orig: dword_31BC904, dword_31BC900]. The environment's keywords and the editor's that the .trn
// holds (TrnConfig's water_*, horizon, terrain_name, terrain_creator) are the .trn's as written: no arm of the
// terrain's parser reads them (the environment's load reads them, env::load_mission_env). `taken`, where given,
// lists the later files' lines the parser took. False (with `error`) where the gate refuses the result.
bool load_mission_trn(const std::string &terrain, const TrnLaterTexts &later, TrnConfig &out, std::string &error,
		std::vector<TrnLaterLine> *taken = nullptr);

// The same load over a reader of the files by name (`read` false: no such file): the mission's .trn by
// `terrain_file` (one that is not there fails the load, as it aborts retail's before every pass [orig:
// Environment_LoadTimeOfDayConfig @ 0x57dbcc]), overcast.def by its name, the .env by `environment_file` (empty:
// the mission names none).
using TrnTextReader = std::function<bool(const std::string &name, std::string &text)>;
bool read_mission_trn(const TrnTextReader &read, const std::string &terrain_file, const std::string &environment_file,
		TrnConfig &out, std::string &error, std::vector<TrnLaterLine> *taken = nullptr);

// Whether an arm of the terrain's parser compares `key` (lower case) outside a foliage block: a `foliage`, a
// `polytrn_*` or a `lock_*` line [orig: Terrain_ParseConfigCallback @ 0x60f330, the arms after the block test
// @ 0x60f5fa]. `block` too: the keys a block's arms compare (`end`, `graphic`, `match`...), which a file's line is
// read by only inside a block, one the file before it may leave open.
bool trn_parser_key(const std::string &key, bool block = false);

// The lines of `text` the terrain's parser can take, in order, each as its tokens: a line whose keyword no arm of
// the parser compares (trn_parser_key, a block's included) changes no terrain whatever the parser's state [orig:
// Terrain_ParseConfigCallback @ 0x60f330, every arm a stricmp of tokens[1]]. What a preview keys a mission's terrain
// on beside the .trn, so that an edit of overcast.def or a .env that sets nothing of the terrain loads none again.
std::string trn_parser_lines(const std::string &text);

} // namespace opennova
