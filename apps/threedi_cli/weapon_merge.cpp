// opennova-3di weapon merge: set the keys an edits file names in a copy of a
// weapon.def, and nothing else. The def is walked the way the game reads it,
// line by line through the retail tokenizer, so the keys it finds are the
// ones the parser binds; every byte outside the values it rewrites and the
// lines it adds is copied as it was. The result is read back through the
// engine's parser before it is written.

#include "weapon_timing.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

#include <base/io/ascii_config.h>
#include <base/io/strutil.h>
#include <formats/def/def.h>

#include "scene_text.h"
#include "threedi_cli.h"

namespace threedi_cli {

namespace {

using namespace opennova::world;
namespace wa = opennova::world::weapon_action;
namespace strutil = opennova::strutil;

bool fail(std::string &error, const std::string &why) {
	error = why;
	return false;
}

// One line of the def: its content (no line break) and the tokens the retail
// tokenizer cuts from it, as spans of the file.
struct Token {
	size_t begin = 0, end = 0;
};

struct Line {
	size_t begin = 0, end = 0; // the content, without the CR LF
	size_t cut = 0;            // where a comment cuts the line, else `end`
	std::vector<Token> tokens;
	std::string key; // token 0, lower case; empty for a line the parser skips
};

// Lines as File_ParseASCIIFile cuts them and tokens as its callback receives
// them, located in the file by the engine's span walk (every line counted, so
// the indices are the ones the parser records on its entries); the line is
// skipped when it has no token or its first starts with '/'. A token the
// comment cut left unterminated runs on to the line's end; `cut` is where
// the value really stops. [orig: File_ParseASCIIFile @ 0x53D8C7..0x53D8F5,
//  @ 0x53D90D..0x53D91E; Terrain_TokenizeConfigLine @ 0x53CB60, the cut
//  @ 0x53CC16..0x53CC31; io/ascii_config.h for_each_config_line_span]
std::vector<Line> split_lines(const std::string &def) {
	std::vector<Line> lines;
	opennova::io::for_each_config_line_span(def.data(), def.size(),
			[&](const opennova::io::ConfigTokens &tokens, const opennova::io::ConfigLineSpan &span) {
				Line line;
				line.begin = span.begin;
				line.end = span.end;
				line.cut = span.cut;
				for (int i = 0; i < tokens.count; ++i)
					line.tokens.push_back({span.token_begin[i], span.token_end[i]});
				if (tokens.count > 0 && tokens.tokens[0][0] != '/') line.key = strutil::to_lower(tokens.tokens[0]);
				lines.push_back(line);
			});
	return lines;
}

std::string token_text(const std::string &def, const Line &line, size_t i) {
	return i < line.tokens.size() ? def.substr(line.tokens[i].begin, line.tokens[i].end - line.tokens[i].begin)
	                               : std::string();
}

struct Block {
	std::string name;
	size_t action_line = 0;
	size_t end_line = 0;
	std::vector<size_t> keys; // the lines inside the block the parser reads a key from
};

struct Entry {
	std::string name;
	const opennova::def::DefWeaponDef *parsed = nullptr; // the parser's record of it
	size_t weapon_line = 0;
	size_t end_line = 0;
	size_t first_action_line = 0;   // the entry's first `action` line, else end_line
	bool closed = true;             // the game reads the recorded `end` lines as `end`
	std::vector<size_t> view_lines; // pos and tpos lines
	std::vector<Block> blocks;      // the live blocks, in line order
};

// Whether line `l` of the retail walk is an `end`: the recorded line exists
// and its first token is `end` in any case.
bool reads_end(const std::vector<Line> &lines, size_t l) {
	return l < lines.size() && lines[l].key == "end";
}

// The def's structure as the engine's parser read it: each entry's `weapon`
// and `end` lines and each live ACTION block's `action` and `end` lines are
// the parser's own records (an entry the text never closes is not parsed;
// of repeated blocks only the last is a row, the earlier ones dead data the
// merge leaves as it is). The key lines of a block and the pos/tpos lines of
// an entry are the lines between them the parser reads a key from: a pos or
// tpos line inside ANY action ... end span, a dead block's too, is forwarded to
// the action parser, which reads no such key, so it is no view line;
// `ammoclass_max_carry` is a table row wherever it stands.
// The parser keeps an unterminated tail line whole, where the game drops its
// last byte (a final `end` with no CR LF reads `en` and closes nothing), so
// every recorded `end` line is checked against the game's own reading of it
// (the retail walk): an entry whose `end` the game does not read is not closed.
// [orig: WeaponDefs_ParseLineCallback @ 0x543680, the in-block forward
//  @ 0x54388D, the block flag set @ 0x54393B and cleared at `end` @ 0x543790;
//  ActionDef_ParseScriptLine @ 0x4023c0; File_ParseASCIIFile's tail leg
//  @ 0x53D8E9 / @ 0x53D8EC; formats/def/def_weapons.cpp]
std::vector<Entry> entries_of(const opennova::def::DefWeaponsFile &parsed, const std::vector<Line> &lines) {
	std::vector<Entry> entries;
	for (size_t e = 0; e < parsed.count; ++e) {
		const opennova::def::DefWeaponDef &def = parsed.entries[e];
		Entry entry;
		entry.name = def.weapon_name;
		entry.parsed = &def;
		entry.weapon_line = def.open_line;
		entry.end_line = def.end_line;
		entry.first_action_line = def.end_line;
		entry.closed = reads_end(lines, def.end_line);
		for (size_t a = 0; a < def.actions_count; ++a) {
			const opennova::def::DefWeaponAction &row = def.actions[a];
			Block block;
			block.name = row.name;
			block.action_line = row.open_line;
			block.end_line = row.end_line;
			if (!reads_end(lines, block.end_line)) entry.closed = false;
			for (size_t l = block.action_line + 1; l < block.end_line && l < lines.size(); ++l)
				if (!lines[l].key.empty() && lines[l].key != "ammoclass_max_carry") block.keys.push_back(l);
			entry.blocks.push_back(block);
		}
		std::sort(entry.blocks.begin(), entry.blocks.end(),
		          [](const Block &a, const Block &b) { return a.action_line < b.action_line; });
		// The view lines: pos/tpos outside every action ... end span, as the
		// parser tracks its block state between the entry's own lines.
		bool in_block = false;
		for (size_t l = entry.weapon_line + 1; l < entry.end_line && l < lines.size(); ++l) {
			const std::string &key = lines[l].key;
			if (key == "ammoclass_max_carry") continue;
			if (!in_block && key == "action") {
				in_block = true;
				if (entry.first_action_line == entry.end_line) entry.first_action_line = l;
			} else if (in_block && key == "end") {
				in_block = false;
			} else if (!in_block && (key == "pos" || key == "tpos")) {
				entry.view_lines.push_back(l);
			}
		}
		entries.push_back(entry);
	}
	return entries;
}

// Whether the retail walk holds a `weapon <name>` line: the game allocates the
// entry's slot at that line, whether or not an `end` ever closes it
// [orig: WeaponDefs_ParseLineCallback @ 0x5436D3..0x543737].
bool opens_entry(const std::string &def, const std::vector<Line> &lines, const std::string &name) {
	for (const Line &line : lines)
		if (line.key == "weapon" && strutil::iequals(token_text(def, line, 1), name)) return true;
	return false;
}

// A change to the def: `remove` bytes at `at` replaced by `insert`.
struct Patch {
	size_t at = 0;
	size_t remove = 0;
	std::string insert;
};

// The keys a block line writes: `delay` is delayend's alias.
// [orig: ActionDef_ParseScriptLine @ 0x40279a / @ 0x402b2c]
bool writes(const std::string &line_key, const std::string &edit_key) {
	return line_key == edit_key || (edit_key == "delayend" && line_key == "delay");
}

// Set token `index` of `line` (1-based values) to `value`, or add it after the
// line's last token; a comment that cut the line stays after the value.
void set_token(const Line &line, size_t index, const std::string &value, std::vector<Patch> &patches) {
	if (index < line.tokens.size()) {
		const Token &t = line.tokens[index];
		patches.push_back({t.begin, std::min(t.end, line.cut) - t.begin, value});
	} else {
		const size_t after = line.tokens.empty() ? line.end : std::min(line.tokens.back().end, line.cut);
		patches.push_back({after, 0, "\t" + value});
	}
}

// The spelling of the lines around an insertion: the indentation, and whether
// its keys are upper case.
std::string indent_of(const std::string &def, const Line &line) {
	size_t i = line.begin;
	while (i < line.end && (def[i] == ' ' || def[i] == '\t')) ++i;
	return def.substr(line.begin, i - line.begin);
}

bool upper_keys(const std::string &def, const Line &line) {
	const std::string key = token_text(def, line, 0);
	return !key.empty() && key == strutil::to_upper(key);
}

// The indentation of an entry's own keys: its first key line's.
std::string entry_indent(const std::string &def, const std::vector<Line> &lines, const Entry &entry) {
	for (size_t l = entry.weapon_line + 1; l < entry.end_line; ++l)
		if (!lines[l].key.empty() && !indent_of(def, lines[l]).empty()) return indent_of(def, lines[l]);
	return "\t";
}

std::string spelled(const std::string &word, bool upper) {
	return upper ? strutil::to_upper(word) : strutil::to_lower(word);
}

// The keys `edit` sets on the blocks of one action.
std::vector<const WeaponEditKey *> keys_for(const WeaponEditEntry &edit, int action) {
	std::vector<const WeaponEditKey *> keys;
	for (const auto &k : edit.keys)
		if (k.action == action) keys.push_back(&k);
	return keys;
}

bool same_value(const std::string &key, const std::string &value, const opennova::def::DefWeaponAction &row) {
	if (key == "anim") return strutil::iequals(row.anim, value);
	const int parsed = strutil::iequals(value, "auto") ? -1 : std::atoi(value.c_str());
	return key == "delaystart" ? row.delaystart == parsed : row.delayend == parsed;
}

std::string def_mode(int flags) {
	const bool automatic = (flags & opennova::def::DEF_WEAPON_FLAG_AUTO) != 0;
	const bool burst = (flags & opennova::def::DEF_WEAPON_FLAG_BURST) != 0;
	return automatic ? (burst ? "auto and burst" : "auto") : (burst ? "burst" : "semi");
}

} // namespace

bool parse_weapon_edits(const std::string &text, std::vector<WeaponEditEntry> &out, std::string &error) {
	out.clear();
	bool header = false;
	size_t at = 0;
	int number = 0;
	while (at <= text.size()) {
		const size_t nl = text.find('\n', at);
		const std::string raw = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
		at = nl == std::string::npos ? text.size() + 1 : nl + 1;
		++number;
		SceneLine in(strip_comment(raw), SceneNumbers::finite);
		if (in.tokens.empty() && in.bad.empty()) continue;
		const std::string where = "edits line " + std::to_string(number) + ": ";
		if (!in.bad.empty()) return fail(error, where + in.bad);
		const std::string key = in.key();
		if (!header) {
			long long version = 0;
			if (key != "weapon_edits" || !in.integer(version, 1, 1) || in.more())
				return fail(error, where + "an edits file starts with `weapon_edits 1`");
			header = true;
			continue;
		}
		if (key == "entry") {
			WeaponEditEntry entry;
			if (!in.name(entry.name) || !in.name(entry.mode) || in.more())
				return fail(error, where + "entry needs a weapon.def entry name and its fire mode");
			if (!weapon_plain_name(entry.name, kWeaponEntryNameMax))
				return fail(error, where + "a weapon.def entry name is 1 to 31 letters, digits, _, - or .");
			if (entry.mode != "semi" && entry.mode != "auto" && entry.mode != "burst")
				return fail(error, where + "the fire mode is semi, auto or burst");
			for (const auto &e : out)
				if (strutil::iequals(e.name, entry.name)) return fail(error, where + entry.name + " is named twice");
			out.push_back(entry);
			continue;
		}
		if (out.empty()) return fail(error, where + "`" + key + "` before any entry");
		WeaponEditEntry &entry = out.back();
		if (key == "pos" || key == "tpos") {
			bool &given = key == "pos" ? entry.pos_given : entry.tpos_given;
			std::string *values = key == "pos" ? entry.pos : entry.tpos;
			double v[3];
			if (given) return fail(error, where + key + " is given twice");
			if (!in.numbers(v, 3) || in.more()) return fail(error, where + key + " needs x y z");
			for (int i = 0; i < 3; ++i) values[i] = in.tokens[1 + i];
			given = true;
		} else if (key == "action") {
			WeaponEditKey edit;
			std::string suffix;
			if (!in.name(suffix) || !in.name(edit.key) || !in.name(edit.value) || in.more())
				return fail(error, where + "action needs a suffix, a key and a value");
			edit.action = weapon_action_named(suffix);
			if (edit.action < 0) return fail(error, where + "'" + suffix + "' is no weapon action suffix");
			if (edit.key == "anim") {
				if (!weapon_plain_name(edit.value, kWeaponActionAnimMax))
					return fail(error, where + "an ANIM is 1 to 63 letters, digits, _, - or .");
			} else if (edit.key == "delaystart" || edit.key == "delayend") {
				char *end = nullptr;
				const long long ticks = std::strtoll(edit.value.c_str(), &end, 10);
				if (!strutil::iequals(edit.value, "auto") &&
				    (edit.value.empty() || *end != '\0' || ticks < 0 || ticks > 0x7FFFFFFF))
					return fail(error, where + "a delay is a whole number of ticks or auto");
			} else {
				return fail(error, where + "an action key is anim, delaystart or delayend");
			}
			for (const auto &k : entry.keys)
				if (k.action == edit.action && k.key == edit.key)
					return fail(error, where + suffix + " " + edit.key + " is given twice");
			entry.keys.push_back(edit);
		} else {
			return fail(error, where + "unknown record `" + key + "`");
		}
	}
	if (!header) return fail(error, "the edits file is empty");
	if (out.empty()) return fail(error, "the edits file names no entry");
	return true;
}

bool merge_weapon_def(const std::string &def, const std::vector<WeaponEditEntry> &edits, std::string &out,
		std::vector<std::string> &notes, std::string &error) {
	out.clear();
	notes.clear();
	// File_ParseASCIIFile decrypts a file that opens with SCR 0x01; the text
	// here must be plain. [orig: File_ParseASCIIFile @ 0x53d899]
	if (def.size() >= 4 && def.compare(0, 4, std::string("SCR\x01", 4)) == 0)
		return fail(error, "the weapon.def is encrypted (SCR): extract it as text first (opennova-extract)");
	// The game ends a line only at CR LF; a lone LF is a byte of the line, so
	// the keys after it are no keys. [orig: File_ParseASCIIFile @ 0x53D8C7..0x53D8F5]
	for (size_t i = 0; i < def.size(); ++i)
		if (def[i] == '\n' && (i == 0 || def[i - 1] != '\r'))
			return fail(error, "the weapon.def ends a line with LF alone, and the game splits lines only at "
			                   "CR LF: save it with CR LF line ends");
	opennova::def::DefWeaponsFile before{};
	if (opennova::def::def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(def.data()), def.size(), &before) != 0)
		return fail(error, "the weapon.def does not parse");
	const std::vector<Line> lines = split_lines(def);
	const std::vector<Entry> entries = entries_of(before, lines);
	const std::string eol = "\r\n";

	std::vector<Patch> patches;
	bool ok = true;
	for (const WeaponEditEntry &edit : edits) {
		bool found = false;
		for (const Entry &entry : entries) {
			if (!strutil::iequals(entry.name, edit.name)) continue;
			found = true;
			if (!entry.closed) {
				ok = fail(error, edit.name + " has no `end` in the weapon.def (the game reads a final line with no "
				                 "CR LF one byte short)");
				break;
			}
			// The entry's FLAGS decide how the FSM fires; the timing was
			// measured in the edits' mode, so they must agree.
			{
				const opennova::def::DefWeaponDef &parsed = *entry.parsed;
				const std::string mode = def_mode(parsed.flags);
				if (mode == "auto and burst")
					ok = fail(error, edit.name + " fires auto and burst in the weapon.def (its FLAGS), and weapon "
					                 "timing measures one mode");
				else if (mode != edit.mode)
					ok = fail(error, edit.name + " fires " + mode + " in the weapon.def (its FLAGS), but the timing "
					                 "was measured as " + edit.mode + ": give the entry's own mode to weapon timing");
				// The SIGHTS card replaces the model once aiming settles.
				// [orig: Render_ProcessMainSceneFrame @ 0x5ca0f0 (@0x5ca299..0x5ca304)]
				WeaponFsmDef fsm;
				fsm.flags = parsed.flags;
				fsm.flags2 = parsed.flags2;
				if (edit.tpos_given && weapon_sights_card_eligible(fsm, WeaponSlotState{}))
					notes.push_back(edit.name + ": its FLAGS show a SIGHTS card" +
					                (parsed.sights_count ? " (" + std::string(parsed.sights[0].texture) + ")" : "") +
					                " in place of the model once aiming settles, so the tpos view shows only "
					                "while the view eases in");
				// The timing ran each action's own handler; a FUNCTION the merge
				// keeps may bind another one to the slot.
				// [orig: Anim_InitActions @ 0x541fa0, the rewrite @ 0x542117..0x542139]
				std::set<int> noted;
				for (const auto &k : edit.keys) {
					if (!noted.insert(k.action).second) continue;
					for (size_t r = 0; r < parsed.actions_count; ++r) {
						const auto &row = parsed.actions[r];
						if (!strutil::iequals(row.name, kWeaponActionSuffixes[k.action])) continue;
						const int8_t bound = weapon_handler_named(row.function);
						if (bound != weapon_handler::kPlaceholder && bound != weapon_action_default_handler(k.action))
							notes.push_back(edit.name + ": its " + strutil::to_upper(kWeaponActionSuffixes[k.action]) +
							                " block keeps FUNCTION " + kWeaponHandlerNames[bound] +
							                ", so that slot runs that handler, not the " +
							                kWeaponActionSuffixes[k.action] + " one the timing measured");
					}
				}
			}
			if (!ok) break;
			const std::string key_indent = entry_indent(def, lines, entry);
			// pos / tpos: the first three values, the rotation columns kept; a
			// line short of six values is read as none, so it gets zeros there.
			// [orig: WeaponDefs_ParseLineCallback, the six-value gates
			//  @ 0x5445EE / @ 0x544735]
			for (const char *which : {"pos", "tpos"}) {
				const bool is_pos = std::strcmp(which, "pos") == 0;
				if (!(is_pos ? edit.pos_given : edit.tpos_given)) continue;
				const std::string *values = is_pos ? edit.pos : edit.tpos;
				bool set = false;
				for (size_t l : entry.view_lines) {
					if (lines[l].key != which) continue;
					set = true;
					for (size_t v = 1; v <= 6; ++v)
						if (v <= 3 || v >= lines[l].tokens.size()) set_token(lines[l], v, v <= 3 ? values[v - 1] : "0", patches);
				}
				if (!set) {
					const size_t at = entry.first_action_line;
					patches.push_back({lines[at].begin, 0, key_indent + which + "\t" + values[0] + "\t" + values[1] + "\t" +
					                                              values[2] + "\t0\t0\t0" + eol});
				}
			}
			// The ACTION keys: set in the action's block, a key the block lacks
			// added before its END, an action with no block appended after the
			// last one. Of repeated blocks only the last is live: each
			// re-initializes the one row of its name, so a key set in an
			// earlier block would be lost and a second block appended for an
			// action that has one would wipe its other keys.
			// [orig: ActionDef_ParseScriptLine @ 0x4024a1 -> ActionDef_InitDefaults
			//  @ 0x4024da]
			std::set<int> actions;
			for (const auto &k : edit.keys) actions.insert(k.action);
			for (int action : actions) {
				const auto keys = keys_for(edit, action);
				const Block *live = nullptr;
				for (const Block &block : entry.blocks)
					if (strutil::iequals(block.name, kWeaponActionSuffixes[action])) live = &block;
				if (live != nullptr) {
					const Block &block = *live;
					const Line &style = block.keys.empty() ? lines[block.action_line] : lines[block.keys.front()];
					const std::string indent = indent_of(def, style) + (block.keys.empty() ? "\t" : "");
					const bool upper = upper_keys(def, style);
					for (const WeaponEditKey *k : keys) {
						bool present = false;
						for (size_t l : block.keys) {
							if (!writes(lines[l].key, k->key)) continue;
							present = true;
							set_token(lines[l], 1, k->value, patches);
						}
						if (!present)
							patches.push_back({lines[block.end_line].begin, 0,
							                   indent + spelled(k->key, upper) + "\t" + k->value + eol});
					}
					continue;
				}
				// A new block names no FUNCTION: the suffix's own handler binds.
				// [orig: Anim_InitActions @ 0x541fa0, the default table @ 0x830B90]
				const Block *last = entry.blocks.empty() ? nullptr : &entry.blocks.back();
				const bool upper = last == nullptr || upper_keys(def, lines[last->action_line]);
				const std::string indent = last == nullptr ? key_indent : indent_of(def, lines[last->action_line]);
				const std::string inner = last != nullptr && !last->keys.empty()
				                                  ? indent_of(def, lines[last->keys.front()])
				                                  : indent + "\t";
				std::string block = eol + indent + spelled("action", upper) + "\t\"" +
				                    spelled(kWeaponActionSuffixes[action], upper) + "\"" + eol;
				for (const WeaponEditKey *k : keys) block += inner + spelled(k->key, upper) + "\t" + k->value + eol;
				block += indent + spelled("end", upper) + eol;
				// The line after the last block's END is the entry's own at
				// the latest, so the block lands inside the entry.
				const size_t at = last == nullptr ? lines[entry.end_line].begin : lines[last->end_line + 1].begin;
				patches.push_back({at, 0, block});
			}
		}
		if (!ok) break;
		if (!found) {
			// The game allocates an entry at its `weapon` line; one the text
			// never closes is an entry with no `end`, not a missing one.
			// [orig: WeaponDefs_ParseLineCallback @ 0x5436D3..0x543737]
			ok = fail(error, opens_entry(def, lines, edit.name)
			                         ? edit.name + " has no `end` in the weapon.def"
			                         : "the weapon.def holds no entry " + edit.name);
			break;
		}
	}
	if (!ok) {
		opennova::def::def_free_weapons(&before);
		return false;
	}

	// Apply the patches in file order; an insertion at a point sorts before a
	// replacement there, and two insertions keep the order they were made in.
	std::stable_sort(patches.begin(), patches.end(), [](const Patch &a, const Patch &b) {
		if (a.at != b.at) return a.at < b.at;
		return a.remove < b.remove;
	});
	size_t cursor = 0;
	for (const Patch &p : patches) {
		out.append(def, cursor, p.at - cursor);
		out += p.insert;
		cursor = p.at + p.remove;
	}
	out.append(def, cursor, std::string::npos);

	// Read the result back through the parser: every entry it named before,
	// the edited ones holding each key as set.
	opennova::def::DefWeaponsFile after{};
	const bool parsed = opennova::def::def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(out.data()),
	                                                            out.size(), &after) == 0;
	std::string differs = !parsed ? "the merged weapon.def does not parse"
	                              : after.count != before.count ? "the merged weapon.def holds another entry count"
	                                                            : std::string();
	for (size_t i = 0; differs.empty() && i < after.count; ++i) {
		const auto &a = after.entries[i];
		const auto &b = before.entries[i];
		if (std::strcmp(a.weapon_name, b.weapon_name) != 0) differs = "entry " + std::to_string(i) + " changed its name";
		const WeaponEditEntry *edit = nullptr;
		for (const auto &e : edits)
			if (strutil::iequals(e.name, a.weapon_name)) edit = &e;
		if (edit == nullptr) {
			if (a.actions_count != b.actions_count) differs = std::string(a.weapon_name) + " changed its ACTION blocks";
			continue;
		}
		const auto view_set = [&](const float got[3], const std::string *want) {
			for (int k = 0; k < 3; ++k)
				if (got[k] != static_cast<float>(std::strtod(want[k].c_str(), nullptr))) return false;
			return true;
		};
		if ((edit->pos_given && !view_set(a.pos, edit->pos)) || (edit->tpos_given && !view_set(a.tpos, edit->tpos)))
			differs = std::string(a.weapon_name) + ": pos or tpos does not read back as set";
		for (const auto &k : edit->keys) {
			bool seen = false;
			for (size_t r = 0; r < a.actions_count; ++r) {
				if (!strutil::iequals(a.actions[r].name, kWeaponActionSuffixes[k.action])) continue;
				seen = true;
				if (!same_value(k.key, k.value, a.actions[r]))
					differs = std::string(a.weapon_name) + ": " + kWeaponActionSuffixes[k.action] + " " + k.key +
					          " does not read back as set";
			}
			if (!seen)
				differs = std::string(a.weapon_name) + ": no " + kWeaponActionSuffixes[k.action] + " block reads back";
		}
	}
	opennova::def::def_free_weapons(&before);
	opennova::def::def_free_weapons(&after);
	if (!differs.empty()) {
		out.clear();
		return fail(error, differs);
	}
	return true;
}

int cmd_weapon_merge(const char *def_path, const char *edits_path, const char *output) {
	const auto read = [](const char *path, std::string &text) {
		std::ifstream file(path, std::ios::binary);
		if (!file) return false;
		text.assign(std::istreambuf_iterator<char>(file), {});
		return true;
	};
	std::string def, edits_text, merged, error;
	if (!read(def_path, def)) {
		std::fprintf(stderr, "opennova-3di: cannot read %s\n", def_path);
		return 1;
	}
	if (!read(edits_path, edits_text)) {
		std::fprintf(stderr, "opennova-3di: cannot read %s\n", edits_path);
		return 1;
	}
	std::vector<WeaponEditEntry> edits;
	std::vector<std::string> notes;
	if (!parse_weapon_edits(edits_text, edits, error) || !merge_weapon_def(def, edits, merged, notes, error)) {
		std::fprintf(stderr, "opennova-3di: weapon merge: %s\n", error.c_str());
		return 1;
	}
	if (!write_output(output, merged.data(), merged.size())) return 1;
	for (const std::string &n : notes) std::fprintf(stderr, "opennova-3di: note: %s\n", n.c_str());
	size_t keys = 0;
	for (const auto &e : edits) keys += e.keys.size() + (e.pos_given ? 1 : 0) + (e.tpos_given ? 1 : 0);
	std::printf("wrote %s: %zu keys in %zu entries\n", output, keys, edits.size());
	return 0;
}

} // namespace threedi_cli
