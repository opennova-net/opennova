#include <formats/score/score.h>

#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace opennova {
namespace score {
namespace {

// The g_GameType code words the ladder tests, named here so the switch reads
// like the original. These mirror engine/base/gameprofile/game_type.h, which the
// formats layer may not include (ADR 0030: formats depends only on base/io and
// other formats libs). [orig: ScoreConfig_LoadScoringTableForGameType @0x52D300 —
// the 0x10000 test @0x52d324 and the (g & 0xFFFDFFFF) == 0x10020 &&
// (g & 0x20000) family test @0x52d345; the same ladder is repeated in
// GameEvent_ProcessScoring @0x52f56c/@0x52f58c and ScoreConfig_GetRowEntry
// @0x52D430 (@0x52d45a/@0x52d47b)]
constexpr uint32_t kTeamDeathmatch = 0x10000;
constexpr uint32_t kWaypointFamilyMask = 0xFFFDFFFFu;
constexpr uint32_t kWaypointFamilyValue = 0x10020u;
constexpr uint32_t kObjectiveBit = 0x20000u;

// The FIELD name table, its rows in its order with their ids [orig: the FIELD names @ 0x830240].
const std::vector<Name> kFields = {
		{"NUMSUICIDES", 1},          {"NUMFRIENDLYKILLS", 2},        {"NUMENEMYKILLS", 3},
		{"NUMDEATHS", 4},            {"NUMSECONDSINZONE", 5},        {"NUMFLAGSCAPTURED", 6},
		{"NUMFLAGSSAVED", 7},        {"NUMTARGETSDESTROYED", 8},     {"NUMSHOTSFIRED", 9},
		{"NUMMEDICSAVES", 10},       {"NUMREVIVES", 11},             {"NUMPSPATTEMPTS", 12},
		{"NUMPSPTAKEOVERS", 13},     {"NUMMEINZONEKILLS", 22},       {"NUMTHEMINMYZONEKILLS", 23},
		{"NUMMEINMYZONEKILLS", 24},  {"NUMTHEMINTHEIRZONEKILLS", 25}, {"NUMMEINTHEIRZONEKILLS", 26},
		{"NUMSKILLKILL", 27},        {"NUMTHEMINFLAGZONEKILLS", 28}, {"NUMMEINFLAGZONEKILLS", 29},
		{"NUMASSISTS", 30},          {"NUMENEMYSNIPERKILLS", 31},    {"NUMFLAGCARRIERKILLS", 14},
		{"NUMMULTIPLEKILLS", 15},    {"NUMHEADSHOTKILLS", 16},       {"NUMKNIFEKILLS", 17},
		{"NUMTHEMINZONEKILLS", 18},  {"EXPERIENCEPOINTS", 19},       {"ITEMPOINTS", 20},
		{"NUMSHOTSPERKILL", 21},     {"NUMLFPTAKEOVERS", 32},
};

// The VAR name table [orig: the VAR names @ 0x830348]: a VAR's id is its word's place among the row's 39 (+296).
const std::vector<Name> kVars = {
		{"FIRE", 0},           {"HIT", 1},                  {"ENEMYKILL", 3},
		{"FRIENDLYKILL", 2},   {"SUICIDE", 4},              {"DEATH", 5},
		{"MEDICHEAL", 6},      {"MEDICSAVE", 7},            {"RESPAWN", 8},
		{"FLAGSAVE", 9},       {"FLAGCAPTURE", 10},         {"FLAGPICKUP", 11},
		{"ZONEQUANTUM", 12},   {"DESTROYTARGET", 13},       {"PSPATTEMPT", 14},
		{"PSPTAKEOVER", 15},   {"MULTIPLEKILL", 16},        {"HEADSHOTKILL", 17},
		{"KNIFEKILL", 18},     {"SKILLKILL", 19},           {"FLAGCARRIERKILL", 20},
		{"THEMINZONEKILL", 21}, {"MEINZONEKILL", 22},       {"THEMINMYZONEKILL", 23},
		{"MEINMYZONEKILL", 24}, {"THEMINTHEIRZONEKILL", 25}, {"MEINTHEIRZONEKILL", 26},
		{"ASSISTS", 27},       {"ENEMYSNIPERKILL", 28},     {"SNIPERSKILLKILLDISTANCEMIN", 29},
		{"SNIPERSKILLKILLDISTANCEMAX", 30}, {"INAZONE", 31}, {"INDZONE", 32},
		{"INZONE", 33},        {"LFPTAKEOVER", 34},         {"ALIVE", 35},
		{"ALIVEQUANTUM", 36},  {"VATTACHKILL", 37},
};

// The rows' names [orig: GameType_CreateDefaultSettings @ 0x52DD00: the strcpy and dword stores at +8 +
// 452 * row (TDM +460, COOP +912, TKOTH +1364, KOTH +1816, SD +2268, AD +2720, CTF +3172, FB +3624, AAS
// +4076, CAC +4528, DM +4980); row 0's name is left as the memset's empty string].
const std::vector<std::string> kGameTypes = {"",  "TDM", "COOP", "TKOTH", "KOTH", "SD",
                                             "AD", "CTF", "FB",   "AAS",   "CAC",  "DM"};

const char *kSeparator = "//---------------------------------------------------"; // [orig: g_ScoreConfigSeparatorLine @ 0x830480]

// A name's id in a table, compared as the reader does (stricmp [orig: ScoreConfig_LoadFile @ 0x52DBA6, @ 0x52DC2F]); -1 for none.
int32_t id_in(const std::vector<Name> &table, std::string_view name) {
	for (const Name &row : table)
		if (strutil::iequals(name, row.name)) return row.id;
	return -1;
}

// [orig: Text_SplitIntoTokens @ 0x52D1B0]: white space (isspace) between tokens; a quote toggles a quoted run
// and is dropped, a quote outside a token starting one.
std::vector<std::string> split_tokens(const std::string &line) {
	std::vector<std::string> out;
	bool in_token = false, quoted = false;
	for (const char c : line) {
		if (!std::isspace(static_cast<unsigned char>(c)) || quoted) {
			if (!in_token) {
				in_token = true;
				out.emplace_back();
			}
			if (c == '"') quoted = !quoted;
			else out.back() += c;
		} else if (in_token) {
			in_token = false;
		}
	}
	return out;
}

// One line as Text_ReadLine @ 0x52D110 cuts it, its buffer 2048 bytes at both calls: [begin, end) its text,
// [end, next) its ending (a CR LF, an LF or a lone CR; a NUL ends the line and is stepped over). A line of more
// than 2047 characters is cut there and the byte after the cut stepped over, whatever it is, the rest read as
// the next line [orig: the copy while `remaining > 1` @ 0x52D158, `*nextLine = &buffer[consumed + 1]` @
// 0x52D19D]. A NUL at a line's start ends the file [orig: @ 0x52D13E, -2].
struct Span {
	size_t begin = 0, end = 0, next = 0;
};
constexpr size_t kLineChars = 2047;
std::vector<Span> lines_of(const char *text, size_t size) {
	std::vector<Span> out;
	size_t at = 0;
	while (at < size && text[at] != '\0') {
		Span span;
		span.begin = at;
		size_t i = at;
		while (i < size && i - at < kLineChars && text[i] != '\0' && text[i] != '\n' && text[i] != '\r') ++i;
		span.end = i;
		if (i - at == kLineChars) i = i + 1 < size ? i + 1 : size;
		else if (i < size && text[i] == '\r' && i + 1 < size && text[i + 1] == '\n') i += 2;
		else if (i < size) ++i;
		span.next = i;
		out.push_back(span);
		at = i;
	}
	return out;
}

bool read_file(const uint8_t *data, size_t size, File &out) {
	out = File{};
	out.version = 0; // no VERSION line reads as none
	if (data == nullptr) return false;
	const char *text = reinterpret_cast<const char *>(data);
	const std::vector<Span> lines = lines_of(text, size);
	// The first pass: VERSION, the last [orig: ScoreConfig_LoadFile @ 0x52DA45..0x52DA5E].
	for (const Span &span : lines) {
		const std::string line(text + span.begin, span.end - span.begin);
		if (!line.empty() && (line[0] == '/' || (line.size() > 1 && line[1] == '/'))) continue;
		const std::vector<std::string> tokens = split_tokens(line);
		if (tokens.size() >= 2 && strutil::iequals(tokens[0], "VERSION")) out.version = io::retail_atol(tokens[1].c_str());
	}
	// The second pass, run only at version 40 [orig: `if (version == 40)` @ 0x52DA8A; any other version writes
	// the defaults over the file instead, @ 0x52DA9E]: a file of another version reads to no block.
	const bool read = out.version == kVersion;
	for (const Span &span : lines) {
		const std::string line(text + span.begin, span.end - span.begin);
		// A line whose first or second character is '/' is read for nothing [orig: @ 0x52DAF6].
		if (!line.empty() && (line[0] == '/' || (line.size() > 1 && line[1] == '/'))) continue;
		const std::vector<std::string> tokens = split_tokens(line);
		if (tokens.empty()) continue;
		if (!read) continue;
		if (tokens.size() > 1 && strutil::iequals(tokens[0], "GAMETYPE")) {
			GameTypeBlock block;
			block.name = tokens[1];
			out.blocks.push_back(std::move(block));
			continue;
		}
		if (tokens.size() >= 2 && strutil::iequals(tokens[0], "VERSION")) continue;
		if (tokens.size() <= 2) continue;
		if (strutil::iequals(tokens[0], "FIELD")) {
			if (out.blocks.empty() || id_in(kFields, tokens[1]) < 0) continue;
			Entry entry;
			entry.name = tokens[1];
			// The value is a byte [orig: `optionLookup = atol(token2)` @ 0x52DBD9 -> sub_52CD70's char].
			entry.value = int32_t(uint8_t(io::retail_atol(tokens[2].c_str())));
			out.blocks.back().fields.push_back(std::move(entry));
		} else if (strutil::iequals(tokens[0], "VAR")) {
			if (out.blocks.empty() || id_in(kVars, tokens[1]) < 0) continue;
			const int32_t value = io::retail_atol(tokens[2].c_str()); // [orig: @ 0x52DC50]
			std::vector<Entry> &vars = out.blocks.back().vars;
			const auto same = std::find_if(vars.begin(), vars.end(),
			                               [&](const Entry &e) { return strutil::iequals(e.name, tokens[1]); });
			if (same != vars.end()) same->value = value;
			else vars.push_back({tokens[1], value});
		} else if (strutil::iequals(tokens[0], "EXP_FANFARE")) {
			// Two bytes [orig: @ 0x52DC75 / @ 0x52DC7C], stored only when both are other than 0 and the second is
			// the greater [orig: @ 0x52DC75..0x52DC9F]: a line failing the gate leaves the pair as it was (the
			// defaults', or an earlier line's).
			const int32_t a = int32_t(uint8_t(io::retail_atol(tokens[1].c_str())));
			const int32_t b = int32_t(uint8_t(io::retail_atol(tokens[2].c_str())));
			if (a != 0 && b != 0 && b > a) {
				out.exp_fanfare[0] = a;
				out.exp_fanfare[1] = b;
			}
		}
	}
	return true;
}

// The quoted name a line writes, refused where the reader would not read it back as one token: a quote ends
// the quoted run, and a name of no character is no token.
bool quotable(const std::string &name) { return !name.empty() && name.find_first_of("\"\r\n") == std::string::npos && name.find('\0') == std::string::npos; }

// The text ScoreConfig_SaveFile @ 0x52CDD0 puts down for the file: the header, the version and the fanfare, the
// FIELD names' comments, then each block; each line ending CR LF [orig: File_WriteLineToHandle @ 0x437010 over
// g_FileLineEnding]. False with the reason for a block or an entry no line of the reader's reads.
bool text_of(const File &file, std::string &text, std::string &error) {
	text.clear();
	const auto line = [&text](const std::string &line) { text += line + "\r\n"; };
	char buffer[512];
	line(kSeparator);
	line("// NovaLogic Score INI file");
	line(kSeparator);
	line("");
	// The game writes its own version, 40 [orig: @ 0x52CE66]; a file read at another keeps its own (the reader takes
	// only 40, so it reads the same).
	std::snprintf(buffer, sizeof buffer, "VERSION %ld", long(file.version));
	line(buffer);
	line("");
	// The fanfare: the game writes the pair it stores always (the defaults' 0 0 [orig: @ 0x52CE9C]).
	std::snprintf(buffer, sizeof buffer, "EXP_FANFARE %d %d", file.exp_fanfare[0] & 0xFF, file.exp_fanfare[1] & 0xFF);
	line(buffer);
	line("");
	// Each FIELD name a comment, by its id from 1 to 33 [orig: @ 0x52CEC5..0x52CF0F].
	for (int32_t id = 1; id <= 33; ++id)
		for (const Name &row : kFields)
			if (row.id == id) {
				std::snprintf(buffer, sizeof buffer, "// FIELD \"%s\"", row.name);
				line(buffer);
			}
	line("");
	for (const GameTypeBlock &block : file.blocks) {
		if (!quotable(block.name)) {
			error = "A GAMETYPE's name \"" + block.name + "\" is none the reader reads back as one token.";
			return false;
		}
		line("");
		line("");
		std::snprintf(buffer, sizeof buffer, "GAMETYPE \"%s\"", block.name.c_str()); // [orig: @ 0x52CF8F]
		line(buffer);
		line("");
		for (const Entry &field : block.fields) {
			if (id_in(kFields, field.name) < 0) {
				error = "FIELD \"" + field.name + "\" is no name of the FIELD table: the reader reads it for nothing.";
				return false;
			}
			// [orig: @ 0x52CFF5, "FIELD \"%s\" %ld" of the byte]
			std::snprintf(buffer, sizeof buffer, "FIELD \"%s\" %ld", field.name.c_str(), long(uint8_t(field.value)));
			line(buffer);
		}
		line("");
		// The VARs in their ids' order [orig: @ 0x52D029..0x52D07A, j from 0 to 38].
		std::vector<const Entry *> vars;
		for (const Entry &var : block.vars) {
			if (id_in(kVars, var.name) < 0) {
				error = "VAR \"" + var.name + "\" is no name of the VAR table: the reader reads it for nothing.";
				return false;
			}
			vars.push_back(&var);
		}
		std::stable_sort(vars.begin(), vars.end(),
		                 [](const Entry *a, const Entry *b) { return id_in(kVars, a->name) < id_in(kVars, b->name); });
		for (const Entry *var : vars) {
			std::snprintf(buffer, sizeof buffer, "VAR \"%s\" %ld", var->name.c_str(), long(var->value)); // [orig: @ 0x52D06B]
			line(buffer);
		}
	}
	return true;
}

const Entry *find(const std::vector<Entry> &entries, std::string_view name) {
	for (const Entry &e : entries)
		if (strutil::iequals(e.name, name)) return &e;
	return nullptr;
}

bool entries_equal(const std::vector<Entry> &a, const std::vector<Entry> &b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); ++i)
		if (a[i].name != b[i].name || a[i].value != b[i].value) return false;
	return true;
}

} // namespace

int row_for_game_type(uint32_t game_type) {
	// [orig: ScoreConfig_LoadScoringTableForGameType @ 0x52D300 — the ladder is tested
	// in this exact order; the `<= 11` guard and the 0 -> 2 remap are its own.]
	int index;
	if (game_type == 0) {
		index = 11; // DM
	} else if (game_type == kTeamDeathmatch) {
		index = 1;
	} else if ((game_type & kWaypointFamilyMask) == kWaypointFamilyValue &&
			(game_type & kObjectiveBit) != 0) {
		index = 2; // objective Co-op
	} else {
		switch (game_type) {        // [orig: the switch arms @0x52d353..0x52d3b1]
		case 65537: index = 3; break;   // @0x52d353 TKOTH
		case 1: index = 4; break;       // @0x52d35f KOTH
		case 589826: index = 5; break;  // @0x52d36d S&D
		case 65538: index = 6; break;   // @0x52d37b A&D
		case 65540: index = 7; break;   // @0x52d389 CTF
		case 65544: index = 8; break;   // @0x52d397 FlagBall
		case 8: index = 12; break;      // @0x52d3a3 Flag Me (fails the <= 11 gate)
		case 65552: index = 9; break;   // @0x52d3b1 A&S
		default: index = (game_type != 327696) ? 0 : 0xA; break; // C&C -> 0xA
		}
	}
	if (index > 11) return -1;      // [orig: the `score_type_index <= 11` gate @0x52d3db]
	if (index == 0) index = 2;      // [orig: `if (!score_type_index) score_type_index = 2` @0x52d3df]
	return index;
}

const std::vector<Name> &field_names() { return kFields; }
const std::vector<Name> &var_names() { return kVars; }
int32_t field_id(std::string_view name) { return id_in(kFields, name); }
int32_t var_id(std::string_view name) { return id_in(kVars, name); }
const std::vector<std::string> &game_type_names() { return kGameTypes; }

int game_type_row(std::string_view name) {
	for (size_t row = 0; row < kGameTypes.size(); ++row)
		if (strutil::iequals(name, kGameTypes[row])) return int(row);
	return -1;
}

bool exp_fanfare_kept(const File &file) {
	const int32_t high = file.exp_fanfare[0] & 0xFF, low = file.exp_fanfare[1] & 0xFF;
	return high != 0 && low != 0 && low > high;
}

bool parse(const uint8_t *data, size_t size, File &out, std::string &error) {
	if (!read_file(data, size, out)) {
		error = "score.ini: no data";
		return false;
	}
	return true;
}

bool write(const File &file, std::vector<uint8_t> &out, std::string &error) {
	error.clear();
	out.clear();
	std::string text;
	if (!text_of(file, text, error)) return false;
	out.assign(text.begin(), text.end());
	return true;
}

const GameTypeBlock *block_at(const File &file, int row) {
	if (row < 0 || static_cast<size_t>(row) >= file.blocks.size()) return nullptr;
	return &file.blocks[static_cast<size_t>(row)];
}

int32_t var_value(const GameTypeBlock &block, std::string_view name, int32_t fallback) {
	const Entry *e = find(block.vars, name);
	return e != nullptr ? e->value : fallback;
}

int32_t field_value(const GameTypeBlock &block, std::string_view name, int32_t fallback) {
	const Entry *e = find(block.fields, name);
	return e != nullptr ? e->value : fallback;
}

bool equal(const File &a, const File &b) {
	if (a.version != b.version || (a.exp_fanfare[0] & 0xFF) != (b.exp_fanfare[0] & 0xFF) ||
	    (a.exp_fanfare[1] & 0xFF) != (b.exp_fanfare[1] & 0xFF) || a.blocks.size() != b.blocks.size())
		return false;
	for (size_t i = 0; i < a.blocks.size(); ++i) {
		if (a.blocks[i].name != b.blocks[i].name) return false;
		if (!entries_equal(a.blocks[i].fields, b.blocks[i].fields)) return false;
		std::vector<Entry> x = a.blocks[i].vars, y = b.blocks[i].vars;
		const auto by_id = [](const Entry &p, const Entry &q) { return var_id(p.name) < var_id(q.name); };
		std::stable_sort(x.begin(), x.end(), by_id);
		std::stable_sort(y.begin(), y.end(), by_id);
		if (!entries_equal(x, y)) return false;
	}
	return true;
}

} // namespace score
} // namespace opennova
