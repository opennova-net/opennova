// The item / weapon / ammo writers (ADR 0046 S5): every writer reparses its own output
// through the witnessed parser and compares the modeled fields, so a snippet that
// writes at all writes faithfully. Pinned here: minted records with nested
// collections, aliases and defaults round-trip; input the game ignores (an unknown
// key, an `attrib:` token outside the chain, a husk piece token the loop skips, an
// action block a later one of its name replaces, whatever it holds) is reported and
// dropped, never a blocker; malformed values and unterminated blocks still refuse the
// write; a name whose closing quote is missing reads to the end of the line as the
// retail tokenizer reads it, and a weapon or action name needs no quotes and ends its
// keyword at a comma or a quote as at a space; and, with a JO install configured, the
// three retail catalogs parse with zero blocking findings and write back. The numbers an
// editor shows in the units the file writes them (def_authored_get / def_authored_set, ADR
// 0046 S12 D5) read as the saved line's arguments, and a set of what one shows leaves its
// record byte for byte as it was, directly and after another number went through the line,
// over the inline catalogs, the items fixture and the three retail catalogs.
#include <formats/def/def_scan.h>
#include <formats/def/def_write.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_paths.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace opennova::def;

namespace {

struct Outcome {
	DefParseReport diagnostics;
	DefWriteResult written;
	size_t count = 0;
	size_t ignored() const {
		size_t n = 0;
		for (const auto &d : diagnostics) if (!d.blocks()) ++n;
		return n;
	}
	bool blocking() const { return def_report_blocks(diagnostics); }
};

Outcome run(const char *family, const std::vector<uint8_t> &bytes) {
	Outcome out;
	if (std::strcmp(family, "items.def") == 0) {
		DefItemsFile file{};
		def_parse_items_memory(bytes.data(), bytes.size(), &file, &out.diagnostics);
		out.count = file.count;
		out.written = def_write_items(file);
		def_free_items(&file);
	} else if (std::strcmp(family, "weapon.def") == 0) {
		DefWeaponsFile file{};
		def_parse_weapons_memory(bytes.data(), bytes.size(), &file, &out.diagnostics);
		out.count = file.count;
		out.written = def_write_weapons(file);
		def_free_weapons(&file);
	} else {
		DefAmmoFile file{};
		def_parse_ammo_memory(bytes.data(), bytes.size(), &file, &out.diagnostics);
		out.count = file.count;
		out.written = def_write_ammo(file);
		def_free_ammo(&file);
	}
	return out;
}

Outcome run(const char *family, const char *text) {
	return run(family, std::vector<uint8_t>(text, text + std::strlen(text)));
}

bool crlf_only(const std::string &text) {
	for (size_t i = 0; i < text.size(); ++i)
		if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) return false;
	return true;
}

// A snippet with nothing the game ignores writes cleanly.
int clean(const char *family, const char *text) {
	const Outcome out = run(family, text);
	if (!out.diagnostics.empty() || !out.written.ok() || !crlf_only(out.written.text)) {
		std::printf("FAIL clean %s: %zu findings, write %s\n", family, out.diagnostics.size(),
		            out.written.ok() ? "ok" : "refused");
		for (const auto &d : out.diagnostics)
			std::printf("  line %zu %s.%s: %s\n", d.line, d.record.c_str(), d.field.c_str(), d.message.c_str());
		for (const auto &d : out.written.diagnostics)
			std::printf("  %s.%s: %s\n", d.record.c_str(), d.field.c_str(), d.message.c_str());
		return 1;
	}
	return 0;
}

// A snippet the typed model cannot carry is reported as blocking and refused.
int refused(const char *family, const char *text) {
	const Outcome out = run(family, text);
	if (!out.blocking() || out.written.ok()) {
		std::printf("FAIL refused %s: blocking=%d write=%s for\n%s\n", family, int(out.blocking()),
		            out.written.ok() ? "ok" : "refused", text);
		return 1;
	}
	return 0;
}

int ignored_input() {
	int failures = 0;
	// An unknown key, two attrib: tokens outside the chain and a husk token without a
	// slot are what the game skips: reported, not blocking, absent from the output.
	const Outcome items = run("items.def",
		"begin \"Dune Buggy\"\nid 100001\ntype vehicle\nsubtype Ruins\n"
		"attrib: AIData good exp1 nodie\nhusk_sub_part_types 01_HULL 02_WHEEL 03_CHUNK_M plain\nend\n");
	if (items.blocking() || items.ignored() != 4 || !items.written.ok()) {
		std::printf("FAIL ignored items: blocking=%d ignored=%zu write=%s\n", int(items.blocking()), items.ignored(),
		            items.written.ok() ? "ok" : "refused");
		for (const auto &d : items.diagnostics)
			std::printf("  line %zu %s.%s: %s\n", d.line, d.record.c_str(), d.field.c_str(), d.message.c_str());
		++failures;
	}
	const std::string &text = items.written.text;
	if (text.find("subtype") != std::string::npos || text.find("exp1") != std::string::npos ||
	    text.find("good") != std::string::npos || text.find("plain") != std::string::npos ||
	    text.find("nodie") == std::string::npos || text.find("aidata") == std::string::npos) {
		std::printf("FAIL ignored items output:\n%s\n", text.c_str());
		++failures;
	}
	// The written file has nothing left to ignore, and the husk pieces read back as the
	// game stores them (slot 1 is HULL, the zero row, whether or not it is spelled out).
	const Outcome again = run("items.def", text.c_str());
	if (!again.diagnostics.empty() || again.count != 1) { std::printf("FAIL ignored items reparse\n"); ++failures; }
	DefItemsFile pieces{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(text.data()), text.size(), &pieces, nullptr);
	if (pieces.count != 1 || pieces.entries[0].husk_sub_part_types[0] != 0 || pieces.entries[0].husk_sub_part_types[1] != 1 ||
	    pieces.entries[0].husk_sub_part_types[2] != 3 || pieces.entries[0].husk_sub_part_types[3] != 0) {
		std::printf("FAIL husk pieces after rewrite:\n%s\n", text.c_str());
		++failures;
	}
	def_free_items(&pieces);
	// The same rule for the other families: an unknown key is dropped, the record stays.
	const Outcome weapon = run("weapon.def", "weapon \"WPN_DESIGNATOR\"\ncategory 1\ndesignation_time 30\nend\n");
	if (weapon.blocking() || weapon.ignored() != 1 || !weapon.written.ok() ||
	    weapon.written.text.find("designation_time") != std::string::npos) { std::printf("FAIL ignored weapon\n"); ++failures; }
	const Outcome ammo = run("ammo.def", "ammo AMMO_SAW\npenetration 1\npenetration_impact 20\nend\n");
	if (ammo.blocking() || ammo.ignored() != 1 || !ammo.written.ok() ||
	    ammo.written.text.find("penetration_impact 20") == std::string::npos) { std::printf("FAIL ignored ammo\n"); ++failures; }
	// The unknown key's location is reported.
	const Outcome located = run("items.def", "begin \"Bad\"\nunknown_field 7\nend\n");
	if (located.diagnostics.size() != 1 || located.diagnostics[0].line != 2 || located.diagnostics[0].blocks() ||
	    located.diagnostics[0].code != DefIssueCode::UnknownProperty || !located.written.ok()) {
		std::printf("FAIL located unknown key\n"); ++failures;
	}
	// A later action block of a name replaces the row wholesale, as the game re-initializes
	// it: the earlier block is reported where it opens, and saving drops it.
	const Outcome twice = run("weapon.def", "weapon \"WPN_TWICE\"\naction \"FIRE\"\ndelaystart 4\nend\n"
	                                        "action \"fire\"\ndelayend 2\nend\nend\n");
	if (twice.blocking() || twice.ignored() != 1 || twice.diagnostics[0].line != 2 || !twice.written.ok() ||
	    twice.written.text.find("delaystart") != std::string::npos ||
	    twice.written.text.find("delayend 2") == std::string::npos) {
		std::printf("FAIL repeated action block:\n%s\n", twice.written.text.c_str()); ++failures;
	}
	// Nothing of a replaced block holds, a value the game never reads in it included: its
	// findings give way to the one notice, and the file saves.
	const Outcome replaced = run("weapon.def", "weapon \"WPN_TWICE\"\naction \"FIRE\"\ndelayend nope\nend\n"
	                                           "action \"FIRE\"\ndelayend 2\nend\nend\n");
	if (replaced.blocking() || replaced.diagnostics.size() != 1 || replaced.diagnostics[0].line != 2 ||
	    !replaced.written.ok() || replaced.written.text.find("delayend 2") == std::string::npos) {
		std::printf("FAIL replaced invalid block\n"); ++failures;
	}
	// A block between keeps its findings where they are (RELOAD's value still blocks), and
	// each replaced block leaves its notice at the line it opened on.
	const Outcome chain = run("weapon.def", "weapon \"WPN_CHAIN\"\naction \"FIRE\"\ndelayend nope\nend\n"
	                                        "action \"RELOAD\"\ndelayend bad\nend\naction \"fire\"\ndelayend 2\nend\n"
	                                        "action \"FIRE\"\ndelaystart 1\nend\nend\n");
	if (!chain.blocking() || chain.diagnostics.size() != 3 || chain.diagnostics[0].line != 2 ||
	    chain.diagnostics[0].blocks() || chain.diagnostics[1].line != 6 || !chain.diagnostics[1].blocks() ||
	    chain.diagnostics[2].line != 8 || chain.diagnostics[2].blocks()) {
		std::printf("FAIL replaced blocks' findings:\n");
		for (const auto &d : chain.diagnostics) std::printf("  line %zu %s.%s blocks=%d\n", d.line, d.record.c_str(), d.field.c_str(), int(d.blocks()));
		++failures;
	}
	return failures;
}

int tokenizer_rules() {
	int failures = 0;
	// A name without its closing quote runs to the end of the line and is written closed.
	const Outcome open = run("items.def", "begin \"Telephone pole,single pole w\nid 102130\ntype building\nend\n");
	if (!open.diagnostics.empty() || !open.written.ok() ||
	    open.written.text.find("begin \"Telephone pole,single pole w\"\r\n") == std::string::npos) {
		std::printf("FAIL unterminated name:\n%s\n", open.written.text.c_str());
		for (const auto &d : open.diagnostics)
			std::printf("  line %zu %s.%s: %s\n", d.line, d.record.c_str(), d.field.c_str(), d.message.c_str());
		++failures;
	}
	failures += clean("weapon.def", "weapon \"WPN_OPEN\ncategory 1\nend\n");
	// A weapon or action name is its header's second token, quotes optional (the shipped
	// AT4 and RPG entries open `ACTION SCOPEUP` bare), and is written quoted; a token
	// after the name is not one the writer could give back.
	failures += clean("weapon.def", "weapon WPN_BARE // a comment\naction SCOPEUP\ndelaystart 1\nend\n"
	                                "action \"SCOPEDOWN\"\ndelaystart 2\nend\nend\n");
	failures += refused("weapon.def", "weapon WPN_BARE extra\ncategory 1\nend\n");
	// A comma or a quote ends a header's keyword as a space does, where the retail
	// tokenizer cuts it, so `action,SCOPEUP` and `action"SCOPEDOWN"` read and save as
	// `action SCOPEUP` would. A keyword run on into its name (`actionFIRE`) is no header:
	// the game compares the whole first token, so it is an unknown weapon key, and the
	// block's lines read as weapon keys up to the weapon's `end` (the second `end` is a
	// stray top-level line). All three are ignored and saving drops them.
	const char cut[] = "weapon,WPN_COMMA\naction,SCOPEUP\ndelaystart 1\nend\naction\"SCOPEDOWN\"\ndelaystart 2\nend\nend\n";
	failures += clean("weapon.def", cut);
	DefWeaponsFile weapons{};
	def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(cut), sizeof(cut) - 1, &weapons, nullptr);
	if (weapons.count != 1 || std::strcmp(weapons.entries[0].weapon_name, "WPN_COMMA") != 0 ||
	    weapons.entries[0].actions_count != 2 || std::strcmp(weapons.entries[0].actions[0].name, "SCOPEUP") != 0 ||
	    std::strcmp(weapons.entries[0].actions[1].name, "SCOPEDOWN") != 0) {
		std::printf("FAIL comma and quote after the keyword\n"); ++failures;
	}
	def_free_weapons(&weapons);
	const Outcome run_on = run("weapon.def", "weapon WPN_RUN\nactionFIRE\ndelaystart 1\nend\nend\n");
	if (run_on.blocking() || run_on.ignored() != 3 || !run_on.written.ok() ||
	    run_on.written.text.find("FIRE") != std::string::npos) {
		std::printf("FAIL run-on keyword:\n%s\n", run_on.written.text.c_str()); ++failures;
	}
	// `;` ends a line outside quotes; `//` and `;` inside a quoted name are the name.
	const Outcome semi = run("items.def", "begin \"Semi; colon\"\nid 7 ; the id\ntype marker\nend\n");
	DefItemsFile parsed{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(semi.written.text.data()), semi.written.text.size(), &parsed, nullptr);
	if (!semi.diagnostics.empty() || !semi.written.ok() || parsed.count != 1 || parsed.entries[0].id != 7 ||
	    std::strcmp(parsed.entries[0].display_name, "Semi; colon") != 0) {
		std::printf("FAIL semicolon rules: count=%zu id=%d name='%s'\n", parsed.count,
		            parsed.count ? parsed.entries[0].id : 0, parsed.count ? parsed.entries[0].display_name : "");
		++failures;
	}
	def_free_items(&parsed);
	failures += clean("items.def", "begin \"Slashes // inside\"\nid 8\ntype marker\nend\n");
	// An unquoted ammo name cannot carry a comment start: the tokenizer would end the
	// line there, so an authored name of that shape is refused by the writer.
	DefAmmoDef bad{};
	def_init_ammo(bad);
	std::strcpy(bad.name, "BAD//NAME");
	DefAmmoFile file{}; file.entries = &bad; file.count = 1;
	if (def_write_ammo(file).ok()) { std::printf("FAIL an unquoted name with a comment start was written\n"); ++failures; }
	return failures;
}

// --- the numbers in the units the file writes them (ADR 0046 S12 D5) ---------------------

// Which argument of its line a member's number is (def_authored.cpp's rule, restated): a
// light's colour takes three, a region-0 timing with no flag name writes no name.
size_t argument_of(const DefProperty &property, size_t index, const void *record, DefRecordKind kind) {
	if (property.encoding == DefEncoding::LightImpact) return index == 0 ? 0 : 4;
	if (property.encoding == DefEncoding::ShotTiming)
		return std::get<std::string>(def_get(record, *def_field(kind, property.fields[0]))).empty() ? index - 1 : index;
	return index;
}

// The writer's lines of each top-level record of a written catalog, by its header.
std::vector<std::string> record_blocks(const std::string &text, const char *header) {
	std::vector<std::string> blocks;
	for (size_t at = text.find(header); at != std::string::npos; at = text.find(header, at + 1)) {
		if (at != 0 && text[at - 1] != '\n') continue;
		const size_t end = text.find("\nend\r\n", at);
		blocks.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
	}
	return blocks;
}

// A written line's arguments: "\t<key> a b c" in `block`, none when the line is not written.
bool line_arguments(const std::string &block, const std::string &key, std::vector<std::string> &args) {
	const size_t at = block.find("\n\t" + key + " ");
	if (at == std::string::npos) return false;
	const size_t start = at + 2 + key.size() + 1;
	std::string line = block.substr(start, block.find("\r\n", start) - start);
	if (!line.empty() && line.back() == '\r') line.pop_back(); // the block's last line
	args.clear();
	for (size_t i = 0; i <= line.size();) {
		const size_t space = line.find(' ', i);
		args.push_back(line.substr(i, space == std::string::npos ? std::string::npos : space - i));
		if (space == std::string::npos) break;
		i = space + 1;
	}
	return true;
}

struct AuthoredCounts {
	size_t members = 0, lines = 0, rewritten = 0, refused = 0, stored = 0;
};

// Every member of `record` a line writes as a number of its own: its number reads (the
// writer's argument, as the saved `block` holds it where the line is written), a set of that
// number leaves the record byte for byte as it was, and, where the saved file writes the
// line, so does the same set after another number went through the line first (the rewrite
// and the parser's read back). A line the file leaves out is not held to that: once written
// it reads the parser's own defaults for what it leaves unsaid (an unset fade, 10 ticks).
int authored_record(DefRecordKind kind, const void *record, const std::string &block, const std::string &name,
                    AuthoredCounts &counts) {
	int failures = 0;
	const size_t size = def_record_size(kind);
	for (const DefField &field : def_fields(kind)) {
		const DefAuthored type = def_member(kind, field.id).authored;
		if (type == DefAuthored::None) continue;
		++counts.members;
		size_t index = 0;
		const DefProperty &property = *def_member_property(kind, field.id, &index);
		const std::string key = property.key == "dawnshot" && argument_of(property, 1, record, kind) == 0
		                                ? std::string("particletesttime")
		                                : property.key;
		std::vector<std::string> args;
		const bool written = !block.empty() && line_arguments(block, key, args);
		DefValue shown;
		if (!def_authored_get(def_member(kind, field.id), record, shown)) {
			// Shown as stored (its line, alone, does not keep the word): never a line the saved
			// file writes, and a set of the stored number changes nothing.
			std::vector<uint64_t> copy((size + 7) / 8);
			std::memcpy(copy.data(), record, size);
			std::string error;
			if (written || !def_authored_set(def_member(kind, field.id), copy.data(), def_get(record, field), error) ||
			    std::memcmp(copy.data(), record, size) != 0) {
				std::printf("FAIL %s.%s: shown as stored, %s\n", name.c_str(), field.id.c_str(),
				            written ? "yet the saved file writes its line" : "and a set of it changed the record");
				++failures;
			}
			++counts.stored;
			continue;
		}
		if (written) {
			const size_t at = argument_of(property, index, record, kind);
			const bool same = at < args.size() &&
			                  (type == DefAuthored::Integer ? std::to_string(std::get<int64_t>(shown)) == args[at]
			                                                : std::strtod(args[at].c_str(), nullptr) == std::get<double>(shown));
			if (!same) {
				std::printf("FAIL %s.%s: shows %s, the file writes '%s'\n", name.c_str(), field.id.c_str(),
				            type == DefAuthored::Integer ? std::to_string(std::get<int64_t>(shown)).c_str()
				                                         : std::to_string(std::get<double>(shown)).c_str(),
				            at < args.size() ? args[at].c_str() : "");
				++failures;
			}
			++counts.lines;
		}
		std::vector<uint64_t> copy((size + 7) / 8);
		std::memcpy(copy.data(), record, size);
		std::string error;
		if (!def_authored_set(def_member(kind, field.id), copy.data(), shown, error) || std::memcmp(copy.data(), record, size) != 0) {
			std::printf("FAIL %s.%s: a set of the number it shows changed the record (%s)\n", name.c_str(), field.id.c_str(),
			            error.c_str());
			++failures;
			continue;
		}
		if (!written) continue;
		const DefValue other = type == DefAuthored::Integer ? DefValue(std::get<int64_t>(shown) + 1)
		                                                    : DefValue(std::get<double>(shown) + 1.0);
		if (!def_authored_set(def_member(kind, field.id), copy.data(), other, error)) {
			++counts.refused;
			continue;
		}
		if (!def_authored_set(def_member(kind, field.id), copy.data(), shown, error) || std::memcmp(copy.data(), record, size) != 0) {
			std::printf("FAIL %s.%s: set back through its line, the record differs (%s)\n", name.c_str(), field.id.c_str(),
			            error.c_str());
			++failures;
			continue;
		}
		++counts.rewritten;
	}
	return failures;
}

// A catalog's records through authored_record, against the file as the writer puts it down.
int authored_catalog(const char *family, const std::vector<uint8_t> &bytes, bool report) {
	int failures = 0;
	AuthoredCounts counts;
	const Outcome written = run(family, bytes);
	if (std::strcmp(family, "items.def") == 0) {
		DefItemsFile file{};
		def_parse_items_memory(bytes.data(), bytes.size(), &file, nullptr);
		const std::vector<std::string> blocks = record_blocks(written.written.text, "begin \"");
		for (size_t i = 0; i < file.count; ++i)
			failures += authored_record(DefRecordKind::Item, &file.entries[i], i < blocks.size() ? blocks[i] : std::string(),
			                            file.entries[i].display_name, counts);
		def_free_items(&file);
	} else if (std::strcmp(family, "weapon.def") == 0) {
		DefWeaponsFile file{};
		def_parse_weapons_memory(bytes.data(), bytes.size(), &file, nullptr);
		const std::vector<std::string> blocks = record_blocks(written.written.text, "weapon \"");
		for (size_t i = 0; i < file.count; ++i)
			failures += authored_record(DefRecordKind::Weapon, &file.entries[i], i < blocks.size() ? blocks[i] : std::string(),
			                            file.entries[i].weapon_name, counts);
		def_free_weapons(&file);
	} else {
		DefAmmoFile file{};
		def_parse_ammo_memory(bytes.data(), bytes.size(), &file, nullptr);
		const std::vector<std::string> blocks = record_blocks(written.written.text, "ammo ");
		for (size_t i = 0; i < file.count; ++i)
			failures += authored_record(DefRecordKind::Ammo, &file.entries[i], i < blocks.size() ? blocks[i] : std::string(),
			                            file.entries[i].name, counts);
		def_free_ammo(&file);
	}
	if (report)
		std::printf("%s authored: %zu member(s), %zu written line(s) matched, %zu set back through the line, %zu "
		            "refused another number, %zu shown as stored\n",
		            family, counts.members, counts.lines, counts.rewritten, counts.refused, counts.stored);
	if (counts.members == 0) { std::printf("FAIL %s: no authored member checked\n", family); ++failures; }
	return failures;
}

int authored_catalog(const char *family, const char *text) {
	return authored_catalog(family, std::vector<uint8_t>(text, text + std::strlen(text)), false);
}

// What the Inspector shows is what the file writes: a speed in km/h, a turn rate in degrees a
// second, a slope in degrees, a whole percent, the ammo's seconds and half-angle cone as
// written; a set stores what the game's parser makes of the line, and only that line's
// members (an acceleration leaves the deceleration its line gave).
int authored_units() {
	int failures = 0;
	const char *text = "begin \"Units\"\nid 100001\ntype vehicle\nplayer_speed 30\nturn_rate 45\nmax_slope 30\n"
	                   "acceleration 5\nlight_transfer 35\nscale 1.5\nend\n";
	DefItemsFile items{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(text), std::strlen(text), &items, nullptr);
	DefItemDef &item = items.entries[0];
	auto number = [&](DefRecordKind kind, const void *record, const char *id) -> DefValue {
		DefValue out;
		return def_authored_get(def_member(kind, id), record, out) ? out : DefValue(std::string("none"));
	};
	if (number(DefRecordKind::Item, &item, "player_speed") != DefValue(int64_t(30)) ||
	    number(DefRecordKind::Item, &item, "turn_rate") != DefValue(int64_t(45)) ||
	    number(DefRecordKind::Item, &item, "max_slope") != DefValue(int64_t(30)) ||
	    number(DefRecordKind::Item, &item, "acceleration") != DefValue(int64_t(5)) ||
	    number(DefRecordKind::Item, &item, "deceleration") != DefValue(int64_t(10)) ||
	    number(DefRecordKind::Item, &item, "light_transfer") != DefValue(int64_t(35)) ||
	    number(DefRecordKind::Item, &item, "scale_q16") != DefValue(1.5)) {
		std::printf("FAIL item numbers in written units\n");
		++failures;
	}
	std::string error;
	if (!def_authored_set(def_member(DefRecordKind::Item, "player_speed"), &item, DefValue(int64_t(50)), error) ||
	    item.player_speed != 50 * 293 ||
	    !def_authored_set(def_member(DefRecordKind::Item, "acceleration"), &item, DefValue(int64_t(7)), error) ||
	    item.acceleration != 28 || item.deceleration != 40 ||
	    !def_authored_set(def_member(DefRecordKind::Item, "light_transfer"), &item, DefValue(int64_t(150)), error) ||
	    item.light_transfer != 1.0f) {
		std::printf("FAIL item sets through the line (%s): speed %d, acceleration %d, deceleration %d, transfer %g\n",
		            error.c_str(), item.player_speed, item.acceleration, item.deceleration, double(item.light_transfer));
		++failures;
	}
	if (def_member(DefRecordKind::Item, "id").authored != DefAuthored::None ||
	    def_member(DefRecordKind::Item, "light_move_color").authored != DefAuthored::None ||
	    def_member(DefRecordKind::Item, "dawnshot").authored != DefAuthored::None ||
	    def_authored_set(def_member(DefRecordKind::Item, "hp"), &item, DefValue(int64_t(5)), error)) {
		std::printf("FAIL a member with no number of its own\n");
		++failures;
	}
	def_free_items(&items);
	const char *ammo_text = "ammo UNITS\nmax_age 1.5\nkz_pieslice 180\nlight_impact 4 10 20 30 0.3\nend\n";
	DefAmmoFile ammo{};
	def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(ammo_text), std::strlen(ammo_text), &ammo, nullptr);
	DefAmmoDef &round = ammo.entries[0];
	if (number(DefRecordKind::Ammo, &round, "max_age_ticks") != DefValue(1.5) ||
	    number(DefRecordKind::Ammo, &round, "kz_pieslice_bam") != DefValue(int64_t(180)) ||
	    !def_authored_set(def_member(DefRecordKind::Ammo, "light_impact_ticks"), &round, DefValue(0.5), error) ||
	    round.light_impact_ticks != 31 || round.light_impact_color != ((10 << 16) | (20 << 8) | 30)) {
		std::printf("FAIL ammo numbers in written units (%s): fade %d\n", error.c_str(), round.light_impact_ticks);
		++failures;
	}
	def_free_ammo(&ammo);
	// A stored word past what its line's numbers carry: 11161 deg/s overflows the 32-bit turn
	// rate [orig: AmmoDef_ParseTurnRate @ 0x40a130], and the writer's argument for that word
	// reads back one off. It shows as stored, a set of the stored word changes nothing, and a
	// number in the file's units goes through the line as any other. A whole number past the
	// 32 bits the parser reads is refused (each platform's strtol would narrow it its own way).
	const char *overflow_text = "ammo FAST\nturnrate_maxyaw 11161\nend\n";
	DefAmmoFile fast{};
	def_parse_ammo_memory(reinterpret_cast<const uint8_t *>(overflow_text), std::strlen(overflow_text), &fast, nullptr);
	DefAmmoDef &fastest = fast.entries[0];
	const int32_t overflowed = fastest.turnrate_maxyaw;
	DefValue shown;
	if (def_authored_get(def_member(DefRecordKind::Ammo, "turnrate_maxyaw"), &fastest, shown) ||
	    !def_authored_set(def_member(DefRecordKind::Ammo, "turnrate_maxyaw"), &fastest, DefValue(int64_t(overflowed)), error) ||
	    fastest.turnrate_maxyaw != overflowed ||
	    !def_authored_set(def_member(DefRecordKind::Ammo, "turnrate_maxyaw"), &fastest, DefValue(30.0), error) ||
	    !def_authored_get(def_member(DefRecordKind::Ammo, "turnrate_maxyaw"), &fastest, shown) || shown != DefValue(30.0)) {
		std::printf("FAIL an overflowed turn rate (%s): stored %d, now %d\n", error.c_str(), overflowed,
		            fastest.turnrate_maxyaw);
		++failures;
	}
	def_free_ammo(&fast);
	DefItemsFile bright{};
	const char *bright_text = "begin \"Bright\"\nlight_transfer 35\nend\n";
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(bright_text), std::strlen(bright_text), &bright, nullptr);
	if (def_authored_set(def_member(DefRecordKind::Item, "light_transfer"), &bright.entries[0], DefValue(int64_t(2147483648LL)), error) ||
	    bright.entries[0].light_transfer != 0.35f) {
		std::printf("FAIL a whole number past 32 bits was taken\n");
		++failures;
	}
	def_free_items(&bright);
	// The inline catalogs above, each member through its line.
	failures += authored_catalog("items.def",
		"begin \"Test item\"\nid 100001\ntype vehicle\nplayer_speed 30\nwater_speed 12\nclimb_speed 3\n"
		"turn_rate 90\nturn_rate2 45\nmax_slope 35\nslip_slope 20\nslip_speed 6\nacceleration 2\ndeceleration 9\n"
		"light_transfer 35\nscale 0.75\ndestroy_timing 1.5 2 0.25\ndawnshot flag 1.5 3\nnightshot night 0.5 2\n"
		"particletesttime 90 0\nend\n");
	failures += authored_catalog("weapon.def",
		"weapon \"WPN_TEST\"\nerror_hiptheta 0.3\nerror_uptheta 1.25\nstability 1, 0.5, 0\n"
		"pos 1 2 3 359.99 180 90\ntpos -0.5 0.25 7 10 20 30\nheat_values 5, 25\nheat_effect heat, .5, 30, 60\nend\n");
	failures += authored_catalog("ammo.def",
		"ammo AT_TEST\nmax_age 1.5\narm_age 0.1\nerror 0.125\ndrag 0.01\nbullet_radius 0.3\nkz_minradius 1\n"
		"kz_maxradius 7.5\ntumble_error 2\nturnrate_maxyaw 1.5\nturnrate_maxpit 30\nboresight_maxang 360\n"
		"kz_pieslice 180\nlight_move 3 255 120 20\nlight_impact 4 10 20 30 0.3\nend\n");
	return failures;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	for (const auto &empty : {def_write_items({}), def_write_weapons({}), def_write_ammo({})})
		if (!empty.ok() || empty.text.empty()) ++failures; // runtime reads distinguish empty payloads from empty tables
	failures += clean("items.def",
		"// source comment\nbegin \"Test item\"\nid 100001\ntype vehicle\nhp 125\narmor 20 30\n"
		"graphic model\nanim_def anim\nkz 0.5\ndebris_scale 1.25\n"
		"particlefx smoke exhaust\naddeweap gun 100002 30 40 50 60\n"
		"pcvehicle_spawnlist 8 4\npcvehicle_spawnlist 4\nend\n"
		"begin \"Second\"\npcvehicle_spawnlist 8\nend\n");
	failures += clean("weapon.def",
		"ammoclass_max_carry bullets 300\nweapon \"WPN_TEST\"\ncategory 1\n"
		"round_type AT_TEST\nweaponweight 3.25\nclipweight 0.1\n"
		"stability 1, 0.5, 0\nerror 0.1, 0.2, 0.3, 0.4, 0.5, 0.6\n"
		"pos 1 2 3 359.99 180 90\nsights scope 0 0 640 480 blend scale slide 3\n"
		"action \"Fire\"\nfunction Shoot 1 2 3 4\nctrlreg trigger\nctrlreginc 2\n"
		"texttoken Fire\ndupsound 2 4\ndelay auto\nend\nend\n");
	failures += clean("ammo.def",
		"ammo AT_TEST\nvelocity 1500\nflag silenced\nmax_age 1.5\nerror 0.125\n"
		"turnrate_maxyaw 1.5\nboresight_maxang 360\nkz_pieslice 180\n"
		"light_move 3 255 120 20\nlight_impact 4 10 20 30 0.3\n"
		"effects_table\nmetal hit spark 4\nend\nend\n");
	failures += clean("items.def",
		"begin \"Coupled\"\nsound_profile male\nsound_profilefemale \"\"\nacceleration 2\ndeceleration 0\n"
		"score -23\ngraphicenemy enemy\ntextid label\nrotor_parts 1 2 3 4\naux_parts 5 6 7 8\nend\n"
		"begin \"Powerup\"\ntype powerup\npowerupdef PU_TEST\nend\n");
	failures += clean("ammo.def", "ammo TRACER\ntracer_type stdred stdgreen\ndopplerdiv 3\nkz_sound hit\n"
	                              "secondary_effect Effect_Burn\nend\n");
	// Retail forms the first writer refused: a percentage stored as a float, and the
	// nameless region-0 timing that `particletesttime` authors.
	failures += clean("items.def", "begin \"Hut\"\nid 101202\ntype building\ngraphic JHut1\nlight_transfer 35\nend\n");
	failures += clean("items.def", "begin \"Emit\"\nid 100059\ntype building\nai_function emit\n"
	                               "particlefxw1 Effect_rocketAft FX00\nparticletesttime     90 0\nend\n");
	// A number the game reads as another: a category past 0..11 and a rank past 0..64 as 0
	// [orig: WeaponDefs_ParseLineCallback @ 0x5439a8 / 0x5439fd], a unit_type past a byte as its
	// low byte [orig: ItemDef_ParseProperty @ 0x49ee47]. The record holds what the game reads and
	// the line is reported, never a blocker; saving writes what the game reads.
	{
		const Outcome wide = run("weapon.def", "weapon \"WPN_WIDE\"\ncategory 13\nrank 70\nend\n");
		const Outcome kept = run("weapon.def", "weapon \"WPN_KEPT\"\ncategory 11\nrank 64\nend\n");
		const Outcome byte = run("items.def", "begin \"Wide\"\nid 100001\nunit_type 259\nend\n");
		const auto reads_as = [](const Outcome &out, const char *field, const char *value) {
			for (const DefIssue &d : out.diagnostics)
				if (d.code == DefIssueCode::Reinterpreted && d.field == field && !d.blocks() &&
				    d.message.find(std::string("reads this as ") + value) != std::string::npos)
					return true;
			return false;
		};
		if (wide.blocking() || wide.diagnostics.size() != 2 || !reads_as(wide, "category", "0") ||
		    !reads_as(wide, "rank", "0") || !wide.written.ok() || wide.written.text.find("category 13") != std::string::npos ||
		    wide.written.text.find("rank 70") != std::string::npos || !kept.diagnostics.empty() ||
		    kept.written.text.find("category 11") == std::string::npos ||
		    kept.written.text.find("rank 64") == std::string::npos || byte.blocking() || byte.diagnostics.size() != 1 ||
		    !reads_as(byte, "unit_type", "3") || byte.written.text.find("unit_type 3\r\n") == std::string::npos) {
			std::printf("FAIL numbers the game reads as others:\n%s\n%s\n", wide.written.text.c_str(), byte.written.text.c_str());
			++failures;
		}
		DefWeaponsFile parsed{};
		const char *text = "weapon \"WPN_WIDE\"\ncategory -1\nrank 65\nend\n";
		def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text), std::strlen(text), &parsed, nullptr);
		if (parsed.count != 1 || parsed.entries[0].category != 0 || parsed.entries[0].rank != 0) {
			std::printf("FAIL category -1 and rank 65 read as 0\n");
			++failures;
		}
		def_free_weapons(&parsed);
	}
	// Past 32 bits, the game's atol saturates (2147483647, or -2147483648 when negative) on
	// every platform [orig: strtoxl @ 0x76b26f..0x76b295], and the bounds and the low byte apply
	// to that: category 4294967297 is 2147483647, read as 0 (not 1); rank -4294967297 as 0;
	// unit_type 2147483651 is byte 255 (not 3), -2147483649 byte 0 (written as no line, which the
	// game reads as 0 too, as a category of 0 is).
	{
		const Outcome huge = run("weapon.def", "weapon \"WPN_HUGE\"\ncategory 4294967297\nrank -4294967297\nend\n");
		const Outcome high = run("items.def", "begin \"High\"\nid 100001\nunit_type 2147483651\nend\n");
		const Outcome low = run("items.def", "begin \"Low\"\nid 100002\nunit_type -2147483649\nend\n");
		const auto reads_as = [](const Outcome &out, const char *field, const char *value) {
			for (const DefIssue &d : out.diagnostics)
				if (d.code == DefIssueCode::Reinterpreted && d.field == field &&
				    d.message.find(std::string("reads this as ") + value + ";") != std::string::npos)
					return true;
			return false;
		};
		if (huge.blocking() || huge.diagnostics.size() != 2 || !reads_as(huge, "category", "0") ||
		    !reads_as(huge, "rank", "0") || huge.written.text.find("4294967297") != std::string::npos ||
		    huge.written.text.find("category 1\r\n") != std::string::npos ||
		    high.blocking() || !reads_as(high, "unit_type", "255") ||
		    high.written.text.find("unit_type 255\r\n") == std::string::npos || low.blocking() ||
		    !reads_as(low, "unit_type", "0") || low.written.text.find("unit_type") != std::string::npos) {
			std::printf("FAIL numbers past 32 bits:\n%s\n%s\n%s\n", huge.written.text.c_str(), high.written.text.c_str(),
			            low.written.text.c_str());
			++failures;
		}
		// The read itself: saturation both ways, the bounds exact, the whole token read (no
		// digits dropped past a buffer), white space and a sign first, digits up to another
		// character, none at all 0.
		const auto atol_of = [](const char *s) { return opennova::defscan::parse_int_n(s, std::strlen(s)); };
		const std::string zeros = std::string(40, '0') + "7";
		if (atol_of("2147483647") != INT32_MAX || atol_of("2147483648") != INT32_MAX ||
		    atol_of("4294967297") != INT32_MAX || atol_of("99999999999999999999") != INT32_MAX ||
		    atol_of("-2147483648") != INT32_MIN || atol_of("-2147483649") != INT32_MIN ||
		    atol_of("-4294967297") != INT32_MIN || atol_of(zeros.c_str()) != 7 || atol_of(" \t-12abc") != -12 ||
		    atol_of("+7") != 7 || atol_of("abc") != 0 || atol_of("") != 0 || atol_of("-") != 0) {
			std::printf("FAIL the game's atol\n");
			++failures;
		}
	}
	// `heat_sound` is a key the game reads [orig: WeaponDefs_ParseLineCallback @ 0x543e85]:
	// kept and written back, never reported as ignored.
	{
		const Outcome heat = run("weapon.def", "weapon \"WPN_HOT\"\nheat_sound OVERHEAT\nend\n");
		if (!heat.diagnostics.empty() || !heat.written.ok() ||
		    heat.written.text.find("\theat_sound OVERHEAT\r\n") == std::string::npos) {
			std::printf("FAIL heat_sound:\n%s\n", heat.written.text.c_str());
			++failures;
		}
	}
	// weapon.def reads its lines through the retail tokenizer, where a quoted run is one
	// token (spaces, commas and a ';' included) and a space, comma or tab ends an unquoted
	// one [orig: Terrain_TokenizeConfigLine @0x53CB60, delimiters @0x53CC33..0x53CC4C,
	// ';' @0x53CC2A..0x53CC31, quote @0x53CC4E..0x53CC70]: such a text is written quoted,
	// on a weapon's, an action's and a sight's lines alike, and reads back whole.
	{
		const char *text = "weapon \"WPN_HOT\"\nheat_sound \"HOT LOOP\"\nsoundhead \"HEAD,ONE\"\nround_type \"AT;X\"\n"
		                   "sights \"scope tex\" 0 0 640 480 blend\n"
		                   "action \"FIRE\"\nsoundset \"SHOT\tTAIL\"\nend\nend\n";
		const Outcome spaced = run("weapon.def", text);
		const std::string &out = spaced.written.text;
		DefWeaponsFile back{};
		def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(out.data()), out.size(), &back, nullptr);
		const bool whole = back.count == 1 && std::strcmp(back.entries[0].heat_sound, "HOT LOOP") == 0 &&
		                   std::strcmp(back.entries[0].soundhead, "HEAD,ONE") == 0 &&
		                   std::strcmp(back.entries[0].round_type, "AT;X") == 0 && back.entries[0].sights_count == 1 &&
		                   std::strcmp(back.entries[0].sights[0].texture, "scope tex") == 0 &&
		                   back.entries[0].actions_count == 1 &&
		                   std::strcmp(back.entries[0].actions[0].soundset, "SHOT\tTAIL") == 0;
		def_free_weapons(&back);
		if (!spaced.diagnostics.empty() || !spaced.written.ok() || !whole ||
		    out.find("\theat_sound \"HOT LOOP\"\r\n") == std::string::npos) {
			std::printf("FAIL quoted text tokens: %zu findings, write %s\n%s\n", spaced.diagnostics.size(),
			            spaced.written.ok() ? "ok" : "refused", out.c_str());
			for (const auto &d : spaced.written.diagnostics)
				std::printf("  %s.%s: %s\n", d.record.c_str(), d.field.c_str(), d.message.c_str());
			++failures;
		}
	}
	failures += ignored_input();
	failures += tokenizer_rules();
	failures += authored_units();
	{
		// The minted items fixture, each member through its line.
		const std::vector<uint8_t> fixture = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/def/items.def");
		if (fixture.empty()) { std::printf("FAIL fixtures/def/items.def unread\n"); ++failures; }
		else failures += authored_catalog("items.def", fixture, true);
	}
	// Malformed values and blocks still refuse the write.
	failures += refused("weapon.def", "ammoclass_max_carry bullets nope\n");
	failures += refused("weapon.def", "weapon \"Bad\"\nsights a 0 0 1 1 unknown\nend\n");
	failures += refused("weapon.def", "weapon \"Open\"\ncategory 1\n");
	failures += refused("ammo.def", "ammo Bad\neffects_table\nmetal hit snd nope\nend\nend\n");
	failures += refused("ammo.def", "effects_table\nend\n");
	failures += refused("ammo.def", "ammo Bad\ntracer_type unknown\nend\n");
	failures += refused("items.def", "begin \"Bad\"\nhp twelve\nend\n");
	failures += refused("items.def", "begin \"Partial\"\naddeweap ewep01 100201 15 37\nend\n");

	const std::string root = retail::install();
	if (root.empty()) retail::skip_leg("def_write retail corpus needs OPENNOVA_JO_DIR");
	else {
		opennova::Vfs vfs;
		vfs.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		if (!vfs.mount_game(root, "", opennova::VfsMountMode::Packed)) return 1;
		for (const char *family : {"items.def", "weapon.def", "ammo.def"}) {
			std::vector<uint8_t> bytes;
			if (!vfs.read_file(family, bytes)) { std::printf("Cannot read %s\n", family); return 1; }
			// Every retail catalog must open in the editor: nothing blocking, and the
			// canonical rewrite must succeed. Ignored lines are counted for the record.
			const Outcome out = run(family, bytes);
			std::map<std::string, int> ignored;
			for (const auto &d : out.diagnostics) ++ignored[d.field];
			std::printf("%s: %zu records, %zu ignored line(s), %s\n", family, out.count, out.ignored(),
			            out.written.ok() ? "written" : "REFUSED");
			for (const auto &[key, n] : ignored) std::printf("  ignored %s: %d\n", key.c_str(), n);
			if (out.blocking() || !out.written.ok()) {
				for (const auto &d : out.diagnostics)
					if (d.blocks()) std::printf("  BLOCKING line %zu %s.%s: %s\n", d.line, d.record.c_str(), d.field.c_str(), d.message.c_str());
				for (const auto &d : out.written.diagnostics)
					std::printf("  write %s.%s: %s\n", d.record.c_str(), d.field.c_str(), d.message.c_str());
				++failures;
			}
			// Every number the Inspector shows in written units reads back from the line
			// the saved file writes, and a set of it leaves the record as it was.
			failures += authored_catalog(family, bytes, true);
		}
	}
	return failures ? 1 : 0;
}
