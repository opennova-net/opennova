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
// Every keyword trn_parser_key takes, in the parser's order: the arms' out of a block, then (`block`) a block's.
std::vector<std::string> trn_parser_keys(bool block = false);

// The lines of `text` the terrain's parser can take, in order, each as its tokens: a line whose keyword no arm of
// the parser compares (trn_parser_key, a block's included) changes no terrain whatever the parser's state [orig:
// Terrain_ParseConfigCallback @ 0x60f330, every arm a stricmp of tokens[1]]. What a preview keys a mission's terrain
// on beside the .trn, so that an edit of overcast.def or a .env that sets nothing of the terrain loads none again.
std::string trn_parser_lines(const std::string &text);

// A line of a file the terrain's parser reads after the .trn (overcast.def, a mission's .env; D-TERRAIN-18) whose
// keyword an arm compares (trn_parser_key, a block's included): what a reader of such a file keeps for the terrain,
// read and written again in its order. Its keyword lower case (the parser compares without case) and the values after
// it, each a token as the game's tokenizer cuts it (a quoted run one token, its quotes not kept; never empty).
struct TrnKeyLine {
	std::string key;
	std::vector<std::string> values;
	bool operator==(const TrnKeyLine &o) const { return key == o.key && values == o.values; }
	bool operator!=(const TrnKeyLine &o) const { return !(*this == o); }
};

// The lines of `text` whose keyword an arm of the terrain's parser compares (trn_parser_key with a block's), in order,
// each as the game's walk cuts it (io::for_each_config_line: CR LF lines, the comment cut, the 30-token cap)
// [orig: File_ParseASCIIFile @0x53D810; Terrain_TokenizeConfigLine @0x53CB60]; `lines`, where given, their 1-based
// lines (every CR LF line counted, as load_mission_trn's TrnLaterLine counts them).
std::vector<TrnKeyLine> read_trn_key_lines(const std::string &text, std::vector<int> *lines = nullptr);

// Whether a value can be written on a line for the tokenizer to read back whole: not empty, no '"' and no control
// character (a quoted run holds any other) [orig: Terrain_TokenizeConfigLine @0x53CB60, the quote
// @0x53CC4E..0x53CC70]. `error` says why not.
bool trn_value_writable(const std::string &value, std::string &error);

// The values from `from` on as a line writes them after its keyword: a space apart, each quoted where it holds a
// separator (space, comma, tab) or a comment's start (';', "//"), so the tokenizer reads each back whole.
std::string trn_values_text(const std::vector<std::string> &values, size_t from = 0);

// The values a text holds, cut as the game's tokenizer cuts a line's (a quoted run one value): false with `error`
// where it holds a comment (';' or "//" outside quotes, which cuts the game's line there), a quote left open, a
// control character, or more values than a line's 30 tokens hold after its keyword.
bool trn_values_of_text(const std::string &text, std::vector<std::string> &values, std::string &error);

// One line written from scratch, CR LF ended: its keyword, then its values (trn_values_text), checked by reading it
// back through the game's tokenizer. False with `error` (writing nothing) for a keyword no arm compares, a value
// trn_value_writable refuses, or a line the tokenizer would read otherwise (past its 30 tokens or 1000 characters
// [orig: Terrain_TokenizeConfigLine @0x53CBBB, @0x53CC8C..0x53CC93]).
bool write_trn_key_line(std::string &out, const TrnKeyLine &line, std::string &error);

// The value a configuration holds for a keyword whose arm sets it whole (a map's name, a density, the grid's width, a
// wrap, the origin's or a lock's two numbers), as a line writes it; "" for one it holds no such value of (a foliage
// block's keys, a grid row, polytrn_scale and polytrn_depthmap, which TrnConfig does not keep).
std::string trn_key_value(const TrnConfig &config, const std::string &key);

// Whether the arm of `key` reads its first value alone, so values after it change nothing (a map's name, a number);
// false for the arms that read two (the origin, the locks), a row of them (polytrn_sectors, match, attrib) or none
// (foliage, end) [orig: Terrain_ParseConfigCallback @0x60f330]. A block key no port reads (the stampdown_* arms) is
// not claimed.
bool trn_parser_reads_one_value(const std::string &key);

} // namespace opennova
