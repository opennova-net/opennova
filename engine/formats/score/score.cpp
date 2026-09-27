#include <formats/score/score.h>

#include <base/io/strutil.h>

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

struct Cursor {
	const char *p = nullptr;
	const char *end = nullptr;
	int line = 1;
};

bool at_end(const Cursor &c) { return c.p >= c.end; }

// Skip whitespace and `//` comments. Newlines are ordinary whitespace: the
// grammar is statement-oriented, not line-oriented.
void skip_trivia(Cursor &c) {
	while (!at_end(c)) {
		const char ch = *c.p;
		if (ch == '\n') {
			++c.line;
			++c.p;
		} else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v') {
			++c.p;
		} else if (ch == '/' && c.p + 1 < c.end && c.p[1] == '/') {
			while (!at_end(c) && *c.p != '\n') ++c.p;
		} else {
			return;
		}
	}
}

// A bare token: everything up to the next whitespace.
std::string next_token(Cursor &c) {
	skip_trivia(c);
	const char *start = c.p;
	while (!at_end(c) && *c.p != ' ' && *c.p != '\t' && *c.p != '\r' && *c.p != '\n' &&
			*c.p != '\f' && *c.p != '\v')
		++c.p;
	return std::string(start, static_cast<size_t>(c.p - start));
}

// A `"quoted name"`. Returns false when the next token is not quoted.
bool next_quoted(Cursor &c, std::string &out) {
	skip_trivia(c);
	if (at_end(c) || *c.p != '"') return false;
	++c.p;
	const char *start = c.p;
	while (!at_end(c) && *c.p != '"' && *c.p != '\n') ++c.p;
	if (at_end(c) || *c.p != '"') return false;
	out.assign(start, static_cast<size_t>(c.p - start));
	++c.p;
	return true;
}

bool next_int(Cursor &c, int32_t &out) {
	const std::string tok = next_token(c);
	if (tok.empty()) return false;
	char *tail = nullptr;
	const long v = std::strtol(tok.c_str(), &tail, 10);
	if (tail == tok.c_str() || (tail != nullptr && *tail != '\0')) return false;
	out = static_cast<int32_t>(v);
	return true;
}

std::string fail(int line, const char *what) {
	char buf[128];
	std::snprintf(buf, sizeof(buf), "score.ini line %d: %s", line, what);
	return std::string(buf);
}

void append(std::vector<uint8_t> &out, const std::string &s) {
	out.insert(out.end(), s.begin(), s.end());
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

bool parse(const uint8_t *data, size_t size, File &out, std::string &error) {
	out = File{};
	if (data == nullptr && size != 0) {
		error = "score.ini: null data";
		return false;
	}
	Cursor c;
	c.p = reinterpret_cast<const char *>(data);
	c.end = c.p + size;

	for (;;) {
		skip_trivia(c);
		if (at_end(c)) break;
		const int line = c.line;
		const std::string keyword = next_token(c);
		if (keyword.empty()) break;

		if (strutil::iequals(keyword, "VERSION")) {
			if (!next_int(c, out.version)) {
				error = fail(line, "VERSION wants one integer");
				return false;
			}
		} else if (strutil::iequals(keyword, "EXP_FANFARE")) {
			if (!next_int(c, out.exp_fanfare[0]) || !next_int(c, out.exp_fanfare[1])) {
				error = fail(line, "EXP_FANFARE wants two integers");
				return false;
			}
		} else if (strutil::iequals(keyword, "GAMETYPE")) {
			GameTypeBlock block;
			if (!next_quoted(c, block.name)) {
				error = fail(line, "GAMETYPE wants a quoted name");
				return false;
			}
			out.blocks.push_back(std::move(block));
		} else if (strutil::iequals(keyword, "FIELD") || strutil::iequals(keyword, "VAR")) {
			// A FIELD/VAR outside any GAMETYPE has no row to belong to; the
			// shipped file never does this, and silently dropping it would hide
			// a malformed edit.
			if (out.blocks.empty()) {
				error = fail(line, "FIELD/VAR before any GAMETYPE block");
				return false;
			}
			Entry e;
			if (!next_quoted(c, e.name) || !next_int(c, e.value)) {
				error = fail(line, "FIELD/VAR wants a quoted name then an integer");
				return false;
			}
			if (strutil::iequals(keyword, "FIELD"))
				out.blocks.back().fields.push_back(std::move(e));
			else
				out.blocks.back().vars.push_back(std::move(e));
		} else {
			error = fail(line, ("unknown statement '" + keyword + "'").c_str());
			return false;
		}
	}
	return true;
}

bool write(const File &file, std::vector<uint8_t> &out, std::string &error) {
	error.clear();
	out.clear();
	char buf[128];
	std::snprintf(buf, sizeof(buf), "VERSION %d\n\n", file.version);
	append(out, buf);
	std::snprintf(buf, sizeof(buf), "EXP_FANFARE %d %d\n",
			file.exp_fanfare[0], file.exp_fanfare[1]);
	append(out, buf);
	for (const GameTypeBlock &block : file.blocks) {
		append(out, "\nGAMETYPE \"" + block.name + "\"\n\n");
		for (const Entry &e : block.fields) {
			std::snprintf(buf, sizeof(buf), " %d\n", e.value);
			append(out, "FIELD \"" + e.name + "\"" + buf);
		}
		if (!block.fields.empty() && !block.vars.empty()) append(out, "\n");
		for (const Entry &e : block.vars) {
			std::snprintf(buf, sizeof(buf), " %d\n", e.value);
			append(out, "VAR \"" + e.name + "\"" + buf);
		}
	}
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
	if (a.version != b.version || a.exp_fanfare[0] != b.exp_fanfare[0] ||
			a.exp_fanfare[1] != b.exp_fanfare[1] || a.blocks.size() != b.blocks.size())
		return false;
	for (size_t i = 0; i < a.blocks.size(); ++i) {
		if (a.blocks[i].name != b.blocks[i].name) return false;
		if (!entries_equal(a.blocks[i].fields, b.blocks[i].fields)) return false;
		if (!entries_equal(a.blocks[i].vars, b.blocks[i].vars)) return false;
	}
	return true;
}

} // namespace score
} // namespace opennova
