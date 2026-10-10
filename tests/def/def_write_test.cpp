// The item / weapon / ammo writers (ADR 0046 S5) and the powerup writer (S13 D10): every
// writer reparses its own output through the witnessed parser and compares the modeled
// fields, so a snippet that writes at all writes faithfully. Pinned here: minted records with nested
// collections, aliases and defaults round-trip; input the game ignores (an unknown
// key, an `attrib:` token outside the chain, a husk piece token the loop skips, an
// action block a later one of its name replaces, whatever it holds) is reported and
// dropped, never a blocker; malformed values and unterminated blocks still refuse the
// write; a name whose closing quote is missing reads to the end of the line as the
// retail tokenizer reads it, and a weapon or action name needs no quotes and ends its
// keyword at a comma or a quote as at a space; jox01's bare `attrib:`, effect-only particle
// slot and addeweap placeholder tail read as the game reads them; and, with a JO install
// configured, the three retail catalogs (the base's and each installed expansion's) and the
// retail powerup.def parse with zero blocking findings and
// write back, the powerup table's canonical form a fixed point (written, parsed, written
// again: the same bytes and the same rows). The numbers an
// editor shows in the units the file writes them (def_authored_get / def_authored_set, ADR
// 0046 S12 D5) read as the saved line's arguments, and a set of what one shows leaves its
// record byte for byte as it was, directly and after another number went through the line,
// over the inline catalogs, the items fixture and the three retail catalogs.
#include <formats/def/def_notes.h>
#include <formats/def/def_scan.h>
#include <formats/def/def_write.h>
#include <formats/def/def_write_record.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_paths.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
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
	} else if (std::strcmp(family, "powerup.def") == 0) {
		DefPowerupFile file{};
		def_parse_powerup_memory(bytes.data(), bytes.size(), &file, &out.diagnostics);
		out.count = file.count;
		out.written = def_write_powerup(file);
		def_free_powerup(&file);
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

// powerup.def (S13 D10): every key the row parser stores, both action blocks, the ammo rows, the
// `auto` delay, and a file the row parser cuts at CR LF alone; the canonical form is a fixed point
// (written, parsed, written again: the same text, the same rows) and pinned line for line; `weapon all`
// after a name leaves no name.
const char *const kPowerupTable =
        "// Powerup definitions\r\n"
        "powerup \"PU_MED\"\r\nrespawn_time 30\r\nmax_respawns 3\r\nhp -1\r\nmana 5\r\nweapon WPN_TEST\r\n"
        "ammo AT_TEST -1\r\nammo AT_TWO 3\r\n"
        "action pickup\r\nfunction powerup_med\r\nanim pick\r\nsoundset SND_PICK\r\nsoundsetend SND_DONE\r\n"
        "particle FX_PICK\r\nparticleuserpoint FX00\r\ntexttoken TT_PICK\r\ndelaystart auto\r\ndelayend 10\r\n"
        "action_value 7\r\nend\r\n"
        "action respawn\r\nparticle FX_BACK\r\nend\r\nend\r\n"
        "powerup \"PU_ALL\"\r\nweapon all\r\nallammo\r\nend\r\n";

// The canonical form, line for line: the header comment, a row's keys in the line table's order (its
// scalars, its weapon, its ammo rows, then its pickup block and its respawn block, each block's keys in
// theirs), a tab before every line of a row, a blank line after each row.
const char *const kPowerupCanonical =
        "// Powerup definitions\r\n\r\n"
        "powerup \"PU_MED\"\r\n\trespawn_time 30\r\n\tmax_respawns 3\r\n\thp -1\r\n\tmana 5\r\n\tweapon WPN_TEST\r\n"
        "\tammo AT_TEST -1\r\n\tammo AT_TWO 3\r\n"
        "\taction \"pickup\"\r\n\tfunction powerup_med\r\n\tanim pick\r\n\tsoundset SND_PICK\r\n"
        "\tsoundsetend SND_DONE\r\n\tparticle FX_PICK\r\n\tparticleuserpoint FX00\r\n\ttexttoken TT_PICK\r\n"
        "\tdelaystart auto\r\n\tdelayend 10\r\n\taction_value 7\r\n\tend\r\n"
        "\taction \"respawn\"\r\n\tparticle FX_BACK\r\n\tend\r\nend\r\n\r\n"
        "powerup \"PU_ALL\"\r\n\tweapon all\r\n\tallammo\r\nend\r\n\r\n";

int powerup_table() {
	int failures = clean("powerup.def", kPowerupTable);
	const Outcome first = run("powerup.def", kPowerupTable);
	if (first.written.text != kPowerupCanonical) {
		std::printf("FAIL powerup.def canonical lines:\n%s\n", first.written.text.c_str());
		++failures;
	}
	// `weapon <name>` then `weapon all` fill one word of the row, the later line kept: no name is left
	// behind it [orig: PowerUpDef_ParseProperty @0x4431A7..0x443216, row+0x34].
	{
		const char *const twice = "powerup \"PU\"\r\nweapon WPN_A\r\nweapon all\r\nend\r\n";
		DefPowerupFile file{};
		def_parse_powerup_memory(reinterpret_cast<const uint8_t *>(twice), std::strlen(twice), &file, nullptr);
		if (file.count != 1 || !file.entries[0].weapon_all || file.entries[0].weapon[0] != 0) {
			std::printf("FAIL powerup.def weapon all after a name keeps the name\n");
			++failures;
		}
		def_free_powerup(&file);
	}
	const Outcome second = run("powerup.def", first.written.text.c_str());
	if (first.count != 2 || !second.written.ok() || second.written.text != first.written.text || second.count != 2) {
		std::printf("FAIL powerup.def is no fixed point:\n%s\n%s\n", first.written.text.c_str(),
		            second.written.text.c_str());
		++failures;
	}
	// An unknown key, an action of another name and a line outside a block are ignored, as the game
	// ignores them [orig: PowerUpDef_ParseProperty @0x44328C "unrecognized token", @0x443056 "Invalid
	// for powerup", the block test @0x44302C]; saving drops them.
	const Outcome dropped = run("powerup.def", "stray 1\r\npowerup \"PU\"\r\nshine 2\r\naction drop\r\nhp 4\r\nend\r\n");
	if (dropped.blocking() || dropped.ignored() != 3 || !dropped.written.ok() ||
	    dropped.written.text.find("shine") != std::string::npos ||
	    dropped.written.text.find("\thp 4\r\n") == std::string::npos) {
		std::printf("FAIL powerup.def ignored input: %zu ignored\n%s\n", dropped.ignored(), dropped.written.text.c_str());
		++failures;
	}
	// A block the file never closes is no row the game registers (it registers at `end`): blocking.
	failures += refused("powerup.def", "powerup \"Open\"\r\nhp 5\r\n");
	return failures;
}

// The three line forms of jox01's items.def the first authoring checks refused (281 times, ADR
// 0046 S16), as the game reads them. A bare `attrib:` sets nothing [orig: ItemDef_ParseProperty
// @ 0x4A06A8 -> 0x4A1CA6], and an addeweap's tokens past the sixth are never read [orig:
// @ 0x4A1B42..0x4A1C9F]: reported as ignored, never blocking, absent from the output. A particle
// slot with its effect alone stores no userpoint, the tokenizer having reset the token [orig:
// @ 0x4A13BF..0x4A13FC; Terrain_TokenizeConfigLine @ 0x53CB71..0x53CB81]: no finding, written back
// as the one token; and a later such line clears the userpoint an earlier one set, as the game's
// copy does.
int expansion_catalog_lines() {
	int failures = 0;
	const Outcome out = run("items.def",
		"begin \"Flyable Ka-52\"\r\nid 100090\r\ntype vehicle\r\n"
		"  addeweapG ewep01 100184 70 10 100 100   <down angle> <up angle> <right angle> <left angle>\r\nend\r\n"
		"begin \"Beach Hut 1\"\r\nid 101300\r\ntype building\r\n  attrib: \r\nend\r\n"
		"begin \"FX_Mosquitos 02M - 01 count\"\r\nid 100500\r\ntype effect\r\n  particlefx fx_Mosquitos_2m_L    \r\nend\r\n");
	const std::string &text = out.written.text;
	if (out.blocking() || out.ignored() != 9 || !out.written.ok() || out.count != 3 ||
	    text.find("\taddeweapg ewep01 100184 70 10 100 100\r\n") == std::string::npos ||
	    text.find("\tparticlefx fx_Mosquitos_2m_L\r\n") == std::string::npos || text.find("attrib:") != std::string::npos ||
	    text.find("angle") != std::string::npos) {
		std::printf("FAIL jox01 item lines: blocking=%d ignored=%zu write=%s\n%s\n", int(out.blocking()), out.ignored(),
		            out.written.ok() ? "ok" : "refused", text.c_str());
		for (const auto &d : out.diagnostics)
			std::printf("  line %zu %s.%s: %s\n", d.line, d.record.c_str(), d.field.c_str(), d.message.c_str());
		++failures;
	}
	if (!run("items.def", text.c_str()).diagnostics.empty()) { std::printf("FAIL jox01 item lines reparse\n"); ++failures; }
	// One to three angles read stale slots: still refused (the parser's own check), as is a line
	// past them with too few.
	failures += refused("items.def", "begin \"Partial\"\r\naddeweapG ewep01 100184 70 10 100\r\nend\r\n");
	const char *slot = "begin \"Slot\"\r\nparticlefx smoke exhaust\r\nparticlefx fire\r\nend\r\n";
	DefItemsFile parsed{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(slot), std::strlen(slot), &parsed, nullptr);
	if (parsed.count != 1 || std::strcmp(parsed.entries[0].particlefx.effect, "fire") != 0 ||
	    parsed.entries[0].particlefx.userpoint[0] != 0) {
		std::printf("FAIL a one-token particlefx clears the userpoint\n");
		++failures;
	}
	def_free_items(&parsed);
	// Two weapon.def keys the game reads, which the catalog once dropped as ignored: jox01's
	// `farpinfo <rounds> <interval>` [orig: WeaponDefs_ParseLineCallback @ 0x544da3 -> +0xE8 /
	// +0xEC] and the designator's `designation_time <seconds>`, stored x 62 [orig: @ 0x544895 ->
	// +0x458]. Kept, no finding, written back; a farpinfo without its interval reads it as 0.
	const char *keys = "weapon \"WPN_MINIGUN\"\r\ncategory 1\r\n\tfarpinfo\t100  1\r\ndesignation_time 30\r\nend\r\n"
	                   "weapon \"WPN_ROCKETS\"\r\ncategory 2\r\nfarpinfo 3\r\nend\r\n";
	const Outcome read = run("weapon.def", keys);
	DefWeaponsFile weapons{};
	def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(keys), std::strlen(keys), &weapons, nullptr);
	const bool stored = weapons.count == 2 && weapons.entries[0].farp_rounds == 100 &&
	                    weapons.entries[0].farp_interval == 1 && weapons.entries[0].designation_ticks == 1860 &&
	                    weapons.entries[1].farp_rounds == 3 && weapons.entries[1].farp_interval == 0;
	def_free_weapons(&weapons);
	if (!stored || !read.diagnostics.empty() || !read.written.ok() ||
	    read.written.text.find("\tfarpinfo 100 1\r\n") == std::string::npos ||
	    read.written.text.find("\tdesignation_time 30\r\n") == std::string::npos ||
	    read.written.text.find("\tfarpinfo 3 0\r\n") == std::string::npos ||
	    !run("weapon.def", read.written.text.c_str()).diagnostics.empty()) {
		std::printf("FAIL farpinfo / designation_time (stored %d):\n%s\n", int(stored), read.written.text.c_str());
		for (const auto &d : read.diagnostics)
			std::printf("  line %zu %s.%s: %s\n", d.line, d.record.c_str(), d.field.c_str(), d.message.c_str());
		++failures;
	}
	return failures;
}

// The #752 review's def lines, each as the game reads it:
// - a particle slot whose effect-only line follows a full one keeps the secondary with no
//   userpoint, a state no single line holds: written as the full line then the effect-only
//   one, which read back to it [orig: ItemDef_ParseProperty @ 0x4A140B.., the secondary only
//   past three tokens @ 0x4A145E]; with no effect either, the bare key last;
// - an attachment's angle that is a word reads as 0 (atol) [orig: @ 0x4A1BB4..0x4A1C49]:
//   reported as such, never blocking, written 0;
// - weapon.def's `sameas` (strncpy 32 -> +0x34 [orig: @ 0x544062..0x544072]) is kept and
//   written; `animcal` fills the one anim-map buffer `animadm` does [orig: @ 0x543D77 /
//   0x543D47] and is written as `animadm`; `gfx1`/`gfx3`'s `nocheckdepth` option (compared
//   without case [orig: @ 0x544F92]) is kept, any other token there ignored.
int review_def_lines() {
	int failures = 0;
	const char *slots = "begin \"Slot\"\r\nid 100001\r\nparticlefxs a b c\r\nparticlefxs d\r\nend\r\n"
	                    "begin \"Bare\"\r\nid 100002\r\nparticlefxw1 a b c\r\nparticlefxw1\r\nend\r\n";
	const Outcome slot = run("items.def", slots);
	DefItemsFile parsed{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(slots), std::strlen(slots), &parsed, nullptr);
	const bool read = parsed.count == 2 && std::strcmp(parsed.entries[0].particlefxs.effect, "d") == 0 &&
	                  parsed.entries[0].particlefxs.userpoint[0] == 0 &&
	                  std::strcmp(parsed.entries[0].particlefxs.secondary_effect, "c") == 0 &&
	                  parsed.entries[1].particlefxw1.effect[0] == 0 &&
	                  std::strcmp(parsed.entries[1].particlefxw1.secondary_effect, "c") == 0;
	def_free_items(&parsed);
	DefItemsFile again{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(slot.written.text.data()), slot.written.text.size(), &again,
	                       nullptr);
	const bool kept = again.count == 2 && std::strcmp(again.entries[0].particlefxs.effect, "d") == 0 &&
	                  again.entries[0].particlefxs.userpoint[0] == 0 &&
	                  std::strcmp(again.entries[0].particlefxs.secondary_effect, "c") == 0 &&
	                  again.entries[1].particlefxw1.effect[0] == 0 && again.entries[1].particlefxw1.userpoint[0] == 0 &&
	                  std::strcmp(again.entries[1].particlefxw1.secondary_effect, "c") == 0;
	def_free_items(&again);
	if (!read || !kept || !slot.diagnostics.empty() || !slot.written.ok() ||
	    slot.written.text.find("\tparticlefxs d d c\r\n\tparticlefxs d\r\n") == std::string::npos ||
	    slot.written.text.find("\tparticlefxw1 c c c\r\n\tparticlefxw1\r\n") == std::string::npos) {
		std::printf("FAIL an effect-only particle line over a secondary (read %d, kept %d):\n%s\n", int(read), int(kept),
		            slot.written.text.c_str());
		for (const auto &d : slot.written.diagnostics) std::printf("  write %s.%s: %s\n", d.record.c_str(), d.field.c_str(), d.message.c_str());
		++failures;
	}
	const Outcome angles = run("items.def", "begin \"Gun\"\r\nid 100090\r\ntype vehicle\r\n"
	                                        "addeweapG ewep01 100184 <down angle> <up angle>\r\nend\r\n");
	size_t zeros = 0;
	for (const auto &d : angles.diagnostics)
		zeros += d.code == DefIssueCode::Reinterpreted && d.message.find("reads this as 0") != std::string::npos;
	if (angles.blocking() || zeros != 4 || !angles.written.ok() ||
	    angles.written.text.find("\taddeweapg ewep01 100184 0 0 0 0\r\n") == std::string::npos) {
		std::printf("FAIL addeweap angles that are words: blocking=%d zeros=%zu\n%s\n", int(angles.blocking()), zeros,
		            angles.written.text.c_str());
		++failures;
	}
	const char *weapons = "weapon \"WPN_SAME\"\r\ncategory 1\r\nsameas WPN_OTHER\r\nanimcal anim_same\r\n"
	                      "gfx1 m_gun NOCHECKDEPTH\r\ngfx3 m_far other\r\nend\r\n";
	const Outcome weapon = run("weapon.def", weapons);
	DefWeaponsFile wp{};
	def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(weapons), std::strlen(weapons), &wp, nullptr);
	const bool stored = wp.count == 1 && std::strcmp(wp.entries[0].sameas, "WPN_OTHER") == 0 &&
	                    std::strcmp(wp.entries[0].animadm, "anim_same") == 0 &&
	                    std::strcmp(wp.entries[0].gfx1, "m_gun") == 0 && wp.entries[0].gfx1_nocheckdepth == 1 &&
	                    std::strcmp(wp.entries[0].gfx3, "m_far") == 0 && wp.entries[0].gfx3_nocheckdepth == 0;
	def_free_weapons(&wp);
	if (!stored || weapon.blocking() || weapon.ignored() != 1 || !weapon.written.ok() ||
	    weapon.written.text.find("\tsameas WPN_OTHER\r\n") == std::string::npos ||
	    weapon.written.text.find("\tanimadm anim_same\r\n") == std::string::npos ||
	    weapon.written.text.find("\tgfx1 m_gun nocheckdepth\r\n") == std::string::npos ||
	    weapon.written.text.find("\tgfx3 m_far\r\n") == std::string::npos ||
	    !run("weapon.def", weapon.written.text.c_str()).diagnostics.empty()) {
		std::printf("FAIL sameas / animcal / nocheckdepth (stored %d, ignored %zu):\n%s\n", int(stored), weapon.ignored(),
		            weapon.written.text.c_str());
		for (const auto &d : weapon.diagnostics)
			std::printf("  line %zu %s.%s: %s\n", d.line, d.record.c_str(), d.field.c_str(), d.message.c_str());
		++failures;
	}
	return failures;
}

// The mission editor's AI keys and side words (D-MIS-10): the game knows the four keys and keeps nothing of
// them [orig: ItemDef_ParseProperty @0x4a1a7b..0x4a1ac6 -> @0x4a1ca6], nor of Good and Evil [orig: @0x4a06c3,
// @0x4a06db], while the original mission editor reads them all [orig: JOTACmed.exe ItemsDef_ParseToken
// @0x4310d0]: read into the model with no finding, written back, the editor's seeds (16, 320, 16, 10) where a
// record has none, which a record made from nothing does not write.
int mission_editor_keys() {
	int failures = 0;
	const char *text = "begin \"Rifleman\"\r\nid 100300\r\ntype person\r\nmax_attack_dist 30\r\n"
	                   "max_engagement_dist 400\r\nmin_engagement_dist 20\r\nfire_timer 5\r\nattrib: AIData Evil Good\r\nend\r\n"
	                   "begin \"Crate\"\r\nid 100301\r\ntype building\r\nend\r\n";
	failures += clean("items.def", text);
	DefItemsFile file{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(text), std::strlen(text), &file, nullptr);
	const DefItemDef *rifleman = file.count == 2 ? &file.entries[0] : nullptr;
	const DefItemDef *crate = file.count == 2 ? &file.entries[1] : nullptr;
	if (!rifleman || rifleman->max_attack_dist != 30 || rifleman->max_engagement_dist != 400 ||
	    rifleman->min_engagement_dist != 20 || rifleman->fire_timer != 5 || !rifleman->attrib_good || !rifleman->attrib_evil ||
	    rifleman->attrib != DEF_ITEM_ATTRIB_AIDATA || rifleman->attrib2 != 0) {
		std::printf("FAIL mission editor keys read\n");
		++failures;
	}
	if (!crate || crate->max_attack_dist != 16 || crate->max_engagement_dist != 320 || crate->min_engagement_dist != 16 ||
	    crate->fire_timer != 10 || crate->attrib_good || crate->attrib_evil) {
		std::printf("FAIL mission editor seeds\n");
		++failures;
	}
	// The file written again over its modeled layout as it was, byte for byte, a value changed on its own line;
	// and from nothing: each key and word where the record holds one, none where it holds the seed.
	{
		DefItemsFile noted{};
		DefTextNotes notes;
		DefParseReport report;
		def_parse_items_memory(reinterpret_cast<const uint8_t *>(text), std::strlen(text), &noted, &report, notes);
		const DefWriteResult kept = def_write_items(noted, &notes);
		if (!kept.ok() || kept.text != text) {
			std::printf("FAIL mission editor keys kept:\n%s\n", kept.text.c_str());
			++failures;
		}
		if (noted.count == 2) noted.entries[0].max_engagement_dist = 450;
		const DefWriteResult edited = def_write_items(noted, &notes);
		std::string expected = text;
		expected.replace(expected.find("max_engagement_dist 400"), 23, "max_engagement_dist 450");
		if (!edited.ok() || edited.text != expected) {
			std::printf("FAIL mission editor keys edited:\n%s\n", edited.text.c_str());
			++failures;
		}
		def_free_items(&noted);
	}
	const DefWriteResult made = def_write_items(file);
	const size_t crate_at = made.text.find("Crate");
	if (!made.ok() || made.text.find("max_attack_dist 30") == std::string::npos ||
	    made.text.find("max_engagement_dist 400") == std::string::npos || made.text.find("min_engagement_dist 20") == std::string::npos ||
	    made.text.find("fire_timer 5") == std::string::npos || made.text.find("evil") == std::string::npos ||
	    made.text.find("good") == std::string::npos || crate_at == std::string::npos ||
	    made.text.find("_dist", crate_at) != std::string::npos || made.text.find("fire_timer", crate_at) != std::string::npos) {
		std::printf("FAIL mission editor keys written:\n%s\n", made.text.c_str());
		++failures;
	}
	DefItemsFile again{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(made.text.data()), made.text.size(), &again, nullptr);
	if (again.count != 2 || again.entries[0].max_engagement_dist != 400 || !again.entries[0].attrib_evil ||
	    !again.entries[0].attrib_good || again.entries[1].fire_timer != 10) {
		std::printf("FAIL mission editor keys reparse\n");
		++failures;
	}
	def_free_items(&again);
	def_free_items(&file);
	return failures;
}

int ignored_input() {
	int failures = 0;
	// An unknown key, an attrib: token outside the chain and a husk token without a slot are
	// what the game skips: reported, not blocking, absent from the output. `good`, the mission
	// editor's side word the game keeps nothing of, is the model's and stays (mission_editor_keys).
	const Outcome items = run("items.def",
		"begin \"Dune Buggy\"\r\nid 100001\r\ntype vehicle\r\nsubtype Ruins\r\n"
		"attrib: AIData good exp1 nodie\r\nhusk_sub_part_types 01_HULL 02_WHEEL 03_CHUNK_M plain\r\nend\r\n");
	if (items.blocking() || items.ignored() != 3 || !items.written.ok()) {
		std::printf("FAIL ignored items: blocking=%d ignored=%zu write=%s\n", int(items.blocking()), items.ignored(),
		            items.written.ok() ? "ok" : "refused");
		for (const auto &d : items.diagnostics)
			std::printf("  line %zu %s.%s: %s\n", d.line, d.record.c_str(), d.field.c_str(), d.message.c_str());
		++failures;
	}
	const std::string &text = items.written.text;
	if (text.find("subtype") != std::string::npos || text.find("exp1") != std::string::npos ||
	    text.find("good") == std::string::npos || text.find("plain") != std::string::npos ||
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
	const Outcome weapon = run("weapon.def", "weapon \"WPN_DESIGNATOR\"\r\ncategory 1\r\ndesignation_range 30\r\nend\r\n");
	if (weapon.blocking() || weapon.ignored() != 1 || !weapon.written.ok() ||
	    weapon.written.text.find("designation_range") != std::string::npos) { std::printf("FAIL ignored weapon\n"); ++failures; }
	const Outcome ammo = run("ammo.def", "ammo AMMO_SAW\r\npenetration 1\r\npenetration_impact 20\r\nend\r\n");
	if (ammo.blocking() || ammo.ignored() != 1 || !ammo.written.ok() ||
	    ammo.written.text.find("penetration_impact 20") == std::string::npos) { std::printf("FAIL ignored ammo\n"); ++failures; }
	// The unknown key's location is reported.
	const Outcome located = run("items.def", "begin \"Bad\"\r\nunknown_field 7\r\nend\r\n");
	if (located.diagnostics.size() != 1 || located.diagnostics[0].line != 2 || located.diagnostics[0].blocks() ||
	    located.diagnostics[0].code != DefIssueCode::UnknownProperty || !located.written.ok()) {
		std::printf("FAIL located unknown key\n"); ++failures;
	}
	// A later action block of a name replaces the row wholesale, as the game re-initializes
	// it: the earlier block is reported where it opens, and saving drops it.
	const Outcome twice = run("weapon.def", "weapon \"WPN_TWICE\"\r\naction \"FIRE\"\r\ndelaystart 4\r\nend\r\n"
	                                        "action \"fire\"\r\ndelayend 2\r\nend\r\nend\r\n");
	if (twice.blocking() || twice.ignored() != 1 || twice.diagnostics[0].line != 2 || !twice.written.ok() ||
	    twice.written.text.find("delaystart") != std::string::npos ||
	    twice.written.text.find("delayend 2") == std::string::npos) {
		std::printf("FAIL repeated action block:\n%s\n", twice.written.text.c_str()); ++failures;
	}
	// Nothing of a replaced block holds, a value the game never reads in it included: its
	// findings give way to the one notice, and the file saves.
	const Outcome replaced = run("weapon.def", "weapon \"WPN_TWICE\"\r\naction \"FIRE\"\r\ndelayend nope\r\nend\r\n"
	                                           "action \"FIRE\"\r\ndelayend 2\r\nend\r\nend\r\n");
	if (replaced.blocking() || replaced.diagnostics.size() != 1 || replaced.diagnostics[0].line != 2 ||
	    !replaced.written.ok() || replaced.written.text.find("delayend 2") == std::string::npos) {
		std::printf("FAIL replaced invalid block\n"); ++failures;
	}
	// A block between keeps its findings where they are (RELOAD's value still blocks), and
	// each replaced block leaves its notice at the line it opened on.
	const Outcome chain = run("weapon.def", "weapon \"WPN_CHAIN\"\r\naction \"FIRE\"\r\ndelayend nope\r\nend\r\n"
	                                        "action \"RELOAD\"\r\ndelayend bad\r\nend\r\naction \"fire\"\r\ndelayend 2\r\nend\r\n"
	                                        "action \"FIRE\"\r\ndelaystart 1\r\nend\r\nend\r\n");
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
	const Outcome open = run("items.def", "begin \"Telephone pole,single pole w\r\nid 102130\r\ntype building\r\nend\r\n");
	if (!open.diagnostics.empty() || !open.written.ok() ||
	    open.written.text.find("begin \"Telephone pole,single pole w\"\r\n") == std::string::npos) {
		std::printf("FAIL unterminated name:\n%s\n", open.written.text.c_str());
		for (const auto &d : open.diagnostics)
			std::printf("  line %zu %s.%s: %s\n", d.line, d.record.c_str(), d.field.c_str(), d.message.c_str());
		++failures;
	}
	failures += clean("weapon.def", "weapon \"WPN_OPEN\r\ncategory 1\r\nend\r\n");
	// A weapon or action name is its header's second token, quotes optional (the shipped
	// AT4 and RPG entries open `ACTION SCOPEUP` bare), and is written quoted; a token
	// after the name is not one the writer could give back.
	failures += clean("weapon.def", "weapon WPN_BARE // a comment\r\naction SCOPEUP\r\ndelaystart 1\r\nend\r\n"
	                                "action \"SCOPEDOWN\"\r\ndelaystart 2\r\nend\r\nend\r\n");
	failures += refused("weapon.def", "weapon WPN_BARE extra\r\ncategory 1\r\nend\r\n");
	// A comma or a quote ends a header's keyword as a space does, where the retail
	// tokenizer cuts it, so `action,SCOPEUP` and `action"SCOPEDOWN"` read and save as
	// `action SCOPEUP` would. A keyword run on into its name (`actionFIRE`) is no header:
	// the game compares the whole first token, so it is an unknown weapon key, and the
	// block's lines read as weapon keys up to the weapon's `end` (the second `end` is a
	// stray top-level line). All three are ignored and saving drops them.
	const char cut[] = "weapon,WPN_COMMA\r\naction,SCOPEUP\r\ndelaystart 1\r\nend\r\naction\"SCOPEDOWN\"\r\ndelaystart 2\r\nend\r\nend\r\n";
	failures += clean("weapon.def", cut);
	DefWeaponsFile weapons{};
	def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(cut), sizeof(cut) - 1, &weapons, nullptr);
	if (weapons.count != 1 || std::strcmp(weapons.entries[0].weapon_name, "WPN_COMMA") != 0 ||
	    weapons.entries[0].actions_count != 2 || std::strcmp(weapons.entries[0].actions[0].name, "SCOPEUP") != 0 ||
	    std::strcmp(weapons.entries[0].actions[1].name, "SCOPEDOWN") != 0) {
		std::printf("FAIL comma and quote after the keyword\n"); ++failures;
	}
	def_free_weapons(&weapons);
	const Outcome run_on = run("weapon.def", "weapon WPN_RUN\r\nactionFIRE\r\ndelaystart 1\r\nend\r\nend\r\n");
	if (run_on.blocking() || run_on.ignored() != 3 || !run_on.written.ok() ||
	    run_on.written.text.find("FIRE") != std::string::npos) {
		std::printf("FAIL run-on keyword:\n%s\n", run_on.written.text.c_str()); ++failures;
	}
	// `;` ends a line outside quotes; `//` and `;` inside a quoted name are the name.
	const Outcome semi = run("items.def", "begin \"Semi; colon\"\r\nid 7 ; the id\r\ntype marker\r\nend\r\n");
	DefItemsFile parsed{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(semi.written.text.data()), semi.written.text.size(), &parsed, nullptr);
	if (!semi.diagnostics.empty() || !semi.written.ok() || parsed.count != 1 || parsed.entries[0].id != 7 ||
	    std::strcmp(parsed.entries[0].display_name, "Semi; colon") != 0) {
		std::printf("FAIL semicolon rules: count=%zu id=%d name='%s'\n", parsed.count,
		            parsed.count ? parsed.entries[0].id : 0, parsed.count ? parsed.entries[0].display_name : "");
		++failures;
	}
	def_free_items(&parsed);
	failures += clean("items.def", "begin \"Slashes // inside\"\r\nid 8\r\ntype marker\r\nend\r\n");
	// An ammo name holding a comment start is written quoted: the reader takes the name from
	// token 1, quoted or not [orig: AmmoDef_ParseProperty @0x40A347..0x40A397], and the
	// tokenizer keeps `//` inside quotes, so the name reads back whole.
	DefAmmoDef bad{};
	def_init_ammo(bad);
	std::strcpy(bad.name, "BAD//NAME");
	DefAmmoFile file{}; file.entries = &bad; file.count = 1;
	const DefWriteResult quoted = def_write_ammo(file);
	if (!quoted.ok() || quoted.text.find("ammo \"BAD//NAME\"\r\n") == std::string::npos) {
		std::printf("FAIL a name with a comment start was not written quoted:\n%s\n", quoted.text.c_str());
		++failures;
	}
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
		// A whole number shows as one, but a scaled one whose word no whole number of the line makes, which
		// shows the quotient (def_authored.cpp's unscaled_real).
		const bool whole = std::holds_alternative<int64_t>(shown);
		if (written) {
			const size_t at = argument_of(property, index, record, kind);
			const bool same = at < args.size() &&
			                  (whole ? std::to_string(std::get<int64_t>(shown)) == args[at]
			                         : std::strtod(args[at].c_str(), nullptr) == std::get<double>(shown));
			if (!same) {
				std::printf("FAIL %s.%s: shows %s, the file writes '%s'\n", name.c_str(), field.id.c_str(),
				            whole ? std::to_string(std::get<int64_t>(shown)).c_str() : std::to_string(std::get<double>(shown)).c_str(),
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
		const DefValue other = whole ? DefValue(std::get<int64_t>(shown) + 1) : DefValue(std::get<double>(shown) + 1.0);
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
		            "refused another number, %zu shown as stored\r\n",
		            family, counts.members, counts.lines, counts.rewritten, counts.refused, counts.stored);
	if (counts.members == 0) { std::printf("FAIL %s: no authored member checked\n", family); ++failures; }
	return failures;
}

int authored_catalog(const char *family, const char *text) {
	return authored_catalog(family, std::vector<uint8_t>(text, text + std::strlen(text)), false);
}

// The demo round's bug 2: a scaled whole number whose word is no multiple of its scale (an item's climb
// speed at the word 1 its allocation gives it [orig: ItemDef_AllocateWithDefaults @0x49E3B0]) shows the
// quotient in the line's units, never the number the parser's 32-bit wrap reads back to it (-2066861395
// km/h for every item without a climb_speed line), and a set of what it shows changes nothing; swept over
// every kind's defaults, each scaled member a whole number where one of the line makes its word (within 16
// rounds of the 32-bit product: a boresight's 360 degrees is the word -256, and shows 360). A number set
// in degrees that wraps the product more than once (540, 720, -720) shows as set, and the line writes it so.
int scaled_defaults() {
	int failures = 0;
	size_t reals = 0;
	for (size_t k = 0; k < kDefRecordKindCount; ++k) {
		const DefRecordKind kind = DefRecordKind(k);
		const size_t size = def_record_size(kind);
		std::vector<uint64_t> record((size + 7) / 8);
		def_init_record(kind, record.data());
		for (const DefField &field : def_fields(kind)) {
			const DefMember member = def_member(kind, field.id);
			if (member.authored != DefAuthored::Integer || !member.property) continue;
			const DefEncoding encoding = member.property->encoding;
			const double scale = encoding == DefEncoding::ScaledInteger ? member.property->factor
			                     : encoding == DefEncoding::Degrees     ? 11930464.0
			                     : encoding == DefEncoding::HalfDegrees ? 11930464.0 / 2.0
			                                                            : 0.0;
			if (scale == 0.0) continue;
			const int64_t word = std::get<int64_t>(def_get(record.data(), field));
			DefValue shown;
			const bool got = def_authored_get(member, record.data(), shown);
			// A whole number of the line makes the word within 16 rounds of the 32-bit product.
			const double round = 4294967296.0;
			bool multiple = false;
			for (int r = 0; r <= 16 && !multiple; ++r)
				multiple = std::fmod(double(word) + r * round, scale) == 0.0 || std::fmod(double(word) - r * round, scale) == 0.0;
			const bool right = got && (multiple ? std::holds_alternative<int64_t>(shown) &&
			                                              uint32_t(int64_t(double(std::get<int64_t>(shown)) * scale)) == uint32_t(word)
			                                    : std::holds_alternative<double>(shown) && std::get<double>(shown) == double(word) / scale);
			std::vector<uint64_t> copy = record;
			std::string error;
			const bool kept = got && def_authored_set(member, copy.data(), shown, error) && std::memcmp(copy.data(), record.data(), size) == 0;
			reals += multiple ? 0 : 1;
			if (!right || !kept) {
				std::printf("FAIL scaled default %d.%s: word %lld shows %s (%s)\n", int(k), field.id.c_str(), (long long)word,
				            !got ? "nothing" : std::holds_alternative<int64_t>(shown) ? std::to_string(std::get<int64_t>(shown)).c_str()
				                                                                      : std::to_string(std::get<double>(shown)).c_str(),
				            kept ? "a set of it keeps the record" : error.c_str());
				++failures;
			}
		}
	}
	// The climb speed's word 1 is one such (1/293 km/h).
	DefItemDef item{};
	def_init_item(item);
	DefValue climb;
	if (reals == 0 || !def_authored_get(def_member(DefRecordKind::Item, "climb_speed"), &item, climb) ||
	    !std::holds_alternative<double>(climb) || std::get<double>(climb) != 1.0 / 293.0) {
		std::printf("FAIL the climb speed's default shows %s\n",
		            std::holds_alternative<double>(climb) ? std::to_string(std::get<double>(climb)).c_str() : "a whole number");
		++failures;
	}
	// Degrees past a turn: set, shown and written as set, read back to the same word.
	const struct {
		DefRecordKind kind;
		const char *field, *key;
	} wrapped[] = {{DefRecordKind::Ammo, "boresight_maxang", "boresight_maxang"},
	               {DefRecordKind::Item, "max_slope", "max_slope"}};
	for (const auto &w : wrapped)
		for (const int64_t degrees : {int64_t(540), int64_t(720), int64_t(-720), int64_t(5760)}) {
			const DefMember member = def_member(w.kind, w.field);
			std::vector<uint64_t> record((def_record_size(w.kind) + 7) / 8);
			def_init_record(w.kind, record.data());
			std::string error;
			DefValue shown;
			const bool set = def_authored_set(member, record.data(), DefValue(degrees), error);
			const bool got = set && def_authored_get(member, record.data(), shown);
			const bool same = got && std::holds_alternative<int64_t>(shown) && std::get<int64_t>(shown) == degrees;
			// The line the writer puts down for it.
			DefRecordWriter writer;
			std::string key;
			std::vector<std::string> args;
			std::vector<DefValue> values;
			for (const DefField *field : member.line) values.push_back(def_get(record.data(), *field));
			writer.property_args(w.kind, *member.property, record.data(), values, "", key, args);
			const bool written = !args.empty() && args[0] == std::to_string(degrees);
			if (!same || !written) {
				std::printf("FAIL %s %lld degrees: %s, shows %s, the line writes '%s'\n", w.field, (long long)degrees,
				            set ? "set" : error.c_str(),
				            !got ? "nothing" : std::holds_alternative<int64_t>(shown) ? std::to_string(std::get<int64_t>(shown)).c_str()
				                                                                      : std::to_string(std::get<double>(shown)).c_str(),
				            args.empty() ? "" : args[0].c_str());
				++failures;
			}
		}
	return failures;
}

// What the Inspector shows is what the file writes: a speed in km/h, a turn rate in degrees a
// second, a slope in degrees, a whole percent, the ammo's seconds and half-angle cone as
// written; a set stores what the game's parser makes of the line, and only that line's
// members (an acceleration leaves the deceleration its line gave).
int authored_units() {
	int failures = 0;
	const char *text = "begin \"Units\"\r\nid 100001\r\ntype vehicle\r\nplayer_speed 30\r\nturn_rate 45\r\nmax_slope 30\r\n"
	                   "acceleration 5\r\nlight_transfer 35\r\nscale 1.5\r\nend\r\n";
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
	const char *ammo_text = "ammo UNITS\r\nmax_age 1.5\r\nkz_pieslice 180\r\nlight_impact 4 10 20 30 0.3\r\nend\r\n";
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
	const char *overflow_text = "ammo FAST\r\nturnrate_maxyaw 11161\r\nend\r\n";
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
	const char *bright_text = "begin \"Bright\"\r\nlight_transfer 35\r\nend\r\n";
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(bright_text), std::strlen(bright_text), &bright, nullptr);
	if (def_authored_set(def_member(DefRecordKind::Item, "light_transfer"), &bright.entries[0], DefValue(int64_t(2147483648LL)), error) ||
	    bright.entries[0].light_transfer != 0.35f) {
		std::printf("FAIL a whole number past 32 bits was taken\n");
		++failures;
	}
	def_free_items(&bright);
	// The inline catalogs above, each member through its line.
	failures += authored_catalog("items.def",
		"begin \"Test item\"\r\nid 100001\r\ntype vehicle\r\nplayer_speed 30\r\nwater_speed 12\r\nclimb_speed 3\r\n"
		"turn_rate 90\r\nturn_rate2 45\r\nmax_slope 35\r\nslip_slope 20\r\nslip_speed 6\r\nacceleration 2\r\ndeceleration 9\r\n"
		"light_transfer 35\r\nscale 0.75\r\ndestroy_timing 1.5 2 0.25\r\ndawnshot flag 1.5 3\r\nnightshot night 0.5 2\r\n"
		"particletesttime 90 0\r\nend\r\n");
	failures += authored_catalog("weapon.def",
		"weapon \"WPN_TEST\"\r\nerror_hiptheta 0.3\r\nerror_uptheta 1.25\r\nstability 1, 0.5, 0\r\n"
		"pos 1 2 3 359.99 180 90\r\ntpos -0.5 0.25 7 10 20 30\r\nheat_values 5, 25\r\nheat_effect heat, .5, 30, 60\r\nend\r\n");
	failures += authored_catalog("ammo.def",
		"ammo AT_TEST\r\nmax_age 1.5\r\narm_age 0.1\r\nerror 0.125\r\ndrag 0.01\r\nbullet_radius 0.3\r\nkz_minradius 1\r\n"
		"kz_maxradius 7.5\r\ntumble_error 2\r\nturnrate_maxyaw 1.5\r\nturnrate_maxpit 30\r\nboresight_maxang 360\r\n"
		"kz_pieslice 180\r\nlight_move 3 255 120 20\r\nlight_impact 4 10 20 30 0.3\r\nend\r\n");
	return failures;
}

} // namespace

// The lines of two texts that differ, place by place (two writes of the same rows: the same lines but
// the changed ones), and the lines of the second past the first's.
size_t lines_differing(const std::string &a, const std::string &b) {
	const auto split = [](const std::string &text) {
		std::vector<std::string> out;
		size_t at = 0;
		while (at < text.size()) {
			const size_t end = text.find('\n', at);
			out.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
			at = end == std::string::npos ? text.size() : end + 1;
		}
		return out;
	};
	const std::vector<std::string> left = split(a), right = split(b);
	size_t differ = left.size() > right.size() ? left.size() - right.size() : right.size() - left.size();
	for (size_t i = 0; i < std::min(left.size(), right.size()); ++i) differ += left[i] != right[i];
	return differ;
}

// What the writer keeps of a file it read (the UX round's plain-words lane; def.h's DefLineOrder and
// DefLayout): each record's lines in the order the file has them, its rows and its blocks where they
// stood, the file's indentation, an item's attributes on one line, each item's spawn list on its own
// line; a record made from nothing in the table's order. A one-field change is a one-line change; an
// order the reparse would read otherwise (a deceleration before the acceleration that defaults it,
// once the deceleration is cleared) is written in the table's order instead.
int kept_layout() {
	int failures = 0;
	const char *items =
	        "begin \"Buggy\"\r\n"
	        "  sid dbuggy1\r\n"
	        "  id 101291\r\n"
	        "  type vehicle\r\n"
	        "  attrib: AIData noscar PlayerControl DynamicShadow\r\n"
	        "  hp 3000\r\n"
	        "  addeweap ewep01 101419\r\n"
	        "  pcvehicle_spawnlist 101291\r\n"
	        "  sound_profile SP_DuneBuggy\r\n"
	        "end\r\n"
	        "begin \"Truck\"\r\n"
	        "  id 101300\r\n"
	        "  pcvehicle_spawnlist 101291 101300\r\n"
	        "  type vehicle\r\n"
	        "end\r\n";
	DefItemsFile file{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(items), std::strlen(items), &file, nullptr);
	const DefWriteResult first = def_write_items(file);
	const std::string expected =
	        "// Item definitions\r\n\r\n"
	        "begin \"Buggy\"\r\n"
	        "  sid dbuggy1\r\n"
	        "  id 101291\r\n"
	        "  type vehicle\r\n"
	        "  attrib: playercontrol aidata noscar dynamicshadow\r\n"
	        "  hp 3000\r\n"
	        "  addeweap ewep01 101419\r\n"
	        "  pcvehicle_spawnlist 101291\r\n"
	        "  sound_profile SP_DuneBuggy\r\n"
	        "end\r\n\r\n"
	        "begin \"Truck\"\r\n"
	        "  id 101300\r\n"
	        "  pcvehicle_spawnlist 101291 101300\r\n"
	        "  type vehicle\r\n"
	        "end\r\n\r\n";
	if (!first.ok() || first.text != expected) {
		std::printf("FAIL kept layout, items:\n%s\n", first.text.c_str());
		for (const auto &d : first.diagnostics) std::printf("  %s.%s: %s\n", d.record.c_str(), d.field.c_str(), d.message.c_str());
		++failures;
	}
	file.entries[0].hp = 2500;
	const DefWriteResult second = def_write_items(file);
	if (!second.ok() || lines_differing(first.text, second.text) != 1 || second.text.find("  hp 2500\r\n") == std::string::npos) {
		std::printf("FAIL kept layout: one field, %zu lines\n", lines_differing(first.text, second.text));
		++failures;
	}
	// A record made from nothing: the table's order, the file's indentation.
	file.entries[0].line_order.count = 0;
	const DefWriteResult table = def_write_items(file);
	if (!table.ok() || table.text.find("begin \"Buggy\"\r\n  sid dbuggy1\r\n  id 101291\r\n  hp 2500\r\n  sound_profile "
	                                   "SP_DuneBuggy\r\n  type vehicle\r\n") == std::string::npos) {
		std::printf("FAIL kept layout: a record of no order\n%s\n", table.text.c_str());
		++failures;
	}
	def_free_items(&file);

	// The order the reparse reads otherwise: a deceleration of 0 written before the acceleration that
	// defaults an unset one, which the table's order writes after it.
	const char *ordered = "begin \"Car\"\r\n\tid 100100\r\n\ttype vehicle\r\n\tdeceleration 70\r\n\tacceleration 15\r\nend\r\n";
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(ordered), std::strlen(ordered), &file, nullptr);
	file.entries[0].deceleration = 0;
	const DefWriteResult reordered = def_write_items(file);
	DefItemsFile back{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(reordered.text.data()), reordered.text.size(), &back, nullptr);
	if (!reordered.ok() || back.count != 1 || back.entries[0].deceleration != 0 || back.entries[0].acceleration != 60) {
		std::printf("FAIL kept layout: an order the reparse refuses\n%s\n", reordered.text.c_str());
		++failures;
	}
	def_free_items(&back);
	def_free_items(&file);

	// The names sharing the death, door and clip words, each written under the name the file gave it from
	// the words as they stand (a helicopter's deathtime, whose every byte its rotor_parts and aux_parts set
	// after it, left out: the game keeps nothing of it); deathtime in whole seconds; a real in the form a
	// person writes; a line at the default kept; every debris slot up to the last, in the files' two digits;
	// no attrib line for the Door a door line raises.
	const char *shared =
	        "begin \"Heli\"\r\n\tid 100200\r\n\ttype vehicle\r\n\tdeathtime 5\r\n\tscore 0\r\n\trotor_parts 1 2 3 4\r\n\taux_parts 5 6 7 8\r\n"
	        "\tshadow shadow8.tga 4.5 8.9 0.0 -0.14\r\n\thusk_sub_part_types 01_HULL 02_WHEEL 03_CHUNK_M\r\nend\r\n"
	        "begin \"Door\"\r\n\tid 100201\r\n\ttype building\r\n\tnum_doors 2\r\n\tfirst_door 3\r\n\topen_rate 2\r\nend\r\n"
	        "begin \"Squib\"\r\n\tid 100202\r\n\ttype effect\r\n\tsqb_rate 6\r\n\tsqb_distance 1.25\r\n\tsqb_error 20\r\nend\r\n"
	        "begin \"Man\"\r\n\tid 100203\r\n\ttype person\r\n\tdeathtime 5\r\nend\r\n";
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(shared), std::strlen(shared), &file, nullptr);
	const DefWriteResult names = def_write_items(file);
	DefItemsFile again{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(names.text.data()), names.text.size(), &again, nullptr);
	bool same = again.count == file.count;
	for (size_t i = 0; same && i < file.count; ++i)
		same = again.entries[i].deathtime_ticks == file.entries[i].deathtime_ticks && again.entries[i].clipsize == file.entries[i].clipsize &&
		       again.entries[i].door_type == file.entries[i].door_type && again.entries[i].attrib == file.entries[i].attrib &&
		       again.entries[i].door_open_rate_q16 == file.entries[i].door_open_rate_q16;
	for (const char *line : {"\tdeathtime 5\r\n", "\tscore 0\r\n", "\trotor_parts 1 2 3 4\r\n", "\taux_parts 5 6 7 8\r\n",
	                         "\tshadow shadow8.tga 4.5 8.9 0 -0.14\r\n", "\thusk_sub_part_types 01_HULL 02_WHEEL 03_CHUNK_M\r\n",
	                         "\tnum_doors 2\r\n\tfirst_door 3\r\n\topen_rate 2\r\n", "\tsqb_rate 6\r\n\tsqb_distance 1.25\r\n\tsqb_error 20\r\n"})
		same = same && names.text.find(line) != std::string::npos;
	if (!names.ok() || !same || names.text.find("clipsize") != std::string::npos || names.text.find("attrib") != std::string::npos ||
	    names.text.find("deathtime") != names.text.rfind("deathtime")) {
		std::printf("FAIL kept layout: the shared words' names\n%s\n", names.text.c_str());
		++failures;
	}
	def_free_items(&again);
	def_free_items(&file);

	// A weapon's lines, its sights and its actions where they stood; its actions a level in, their lines
	// a level further, by the file's own indentation; the carry limits at the top level.
	const char *weapons =
	        "ammoclass_max_carry CLASS_9mm 120\r\n"
	        "weapon \"WPN_A\"\r\n"
	        "    category 2\r\n"
	        "    action \"fire\"\r\n"
	        "        function wpn_std_fire\r\n"
	        "        delayend 4\r\n"
	        "    end\r\n"
	        "    clipsize 15\r\n"
	        "end\r\n";
	DefWeaponsFile guns{};
	def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(weapons), std::strlen(weapons), &guns, nullptr);
	const DefWriteResult gun = def_write_weapons(guns);
	const std::string gun_expected =
	        "// Weapon definitions\r\n\r\n"
	        "ammoclass_max_carry CLASS_9mm 120\r\n"
	        "weapon \"WPN_A\"\r\n"
	        "    category 2\r\n"
	        "    action \"fire\"\r\n"
	        "        function wpn_std_fire\r\n"
	        "        delayend 4\r\n"
	        "    end\r\n"
	        "    clipsize 15\r\n"
	        "end\r\n\r\n";
	if (!gun.ok() || gun.text != gun_expected) {
		std::printf("FAIL kept layout, weapons:\n%s\n", gun.text.c_str());
		for (const auto &d : gun.diagnostics) std::printf("  %s.%s: %s\n", d.record.c_str(), d.field.c_str(), d.message.c_str());
		++failures;
	}
	def_free_weapons(&guns);
	return failures;
}

// The lines removed from `a` and added in `b` (a longest common subsequence of lines; small texts).
std::pair<size_t, size_t> lines_changed(const std::string &a, const std::string &b) {
	const auto split = [](const std::string &text) {
		std::vector<std::string> out;
		size_t at = 0;
		while (at < text.size()) {
			const size_t end = text.find('\n', at);
			out.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
			at = end == std::string::npos ? text.size() : end + 1;
		}
		return out;
	};
	const std::vector<std::string> x = split(a), y = split(b);
	std::vector<std::vector<size_t>> common(x.size() + 1, std::vector<size_t>(y.size() + 1, 0));
	for (size_t i = x.size(); i-- > 0;)
		for (size_t j = y.size(); j-- > 0;)
			common[i][j] = x[i] == y[j] ? common[i + 1][j + 1] + 1 : std::max(common[i + 1][j], common[i][j + 1]);
	return {x.size() - common[0][0], y.size() - common[0][0]};
}

// An edit the kept order cannot carry (the review's W5, W1): a present line unticked is no line (only that
// line goes); a particle slot emptied is no line, never `particlefx ""`; an item of more attributes than an
// `attrib:` line holds saves over several; a record whose own order reads back otherwise is written in the
// table's order alone, named (DefWriteResult::reordered), every other record keeping its own; and a table
// with room for every step (W12).
int kept_layout_edits() {
	int failures = 0;
	// A weapon's switchcategory unticked.
	const char *weapons = "weapon \"WPN_A\"\r\n\tcategory 2\r\n\tswitchcategory 3\r\n\tclipsize 15\r\nend\r\n"
	                      "weapon \"WPN_B\"\r\n\tclipsize 30\r\n\tcategory 1\r\nend\r\n";
	DefWeaponsFile guns{};
	def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(weapons), std::strlen(weapons), &guns, nullptr);
	const DefWriteResult before = def_write_weapons(guns);
	guns.entries[0].has_switchcategory = 0;
	const DefWriteResult after = def_write_weapons(guns);
	const auto [removed, added] = lines_changed(before.text, after.text);
	if (!before.ok() || !after.ok() || removed != 1 || added != 0 || after.text.find("switchcategory") != std::string::npos ||
	    !after.reordered.empty()) {
		std::printf("FAIL kept layout: a present line unticked (-%zu +%zu)\n%s\n", removed, added, after.text.c_str());
		++failures;
	}
	def_free_weapons(&guns);

	// A particle slot emptied, and an item of thirty attributes (the writer puts 29 on a line, as the
	// tokenizer keeps 29 values after the key).
	std::string items = "begin \"Smoky\"\r\n\tid 100100\r\n\ttype vehicle\r\n\tparticlefx smoke exhaust\r\n\thp 50\r\nend\r\nbegin \"Flagged\"\r\n\tid 100101\r\n";
	std::string line;
	size_t flags = 0;
	for (int i = 0; i < def_item_attrib_keyword_count() && flags < 30; ++i, ++flags) {
		if (flags % 10 == 0) line += (line.empty() ? "" : "\r\n") + std::string("\tattrib:");
		line += std::string(" ") + def_item_attrib_keyword(i);
	}
	items += line + "\r\nend\r\n";
	DefItemsFile file{};
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(items.data()), items.size(), &file, nullptr);
	const DefWriteResult full = def_write_items(file);
	std::memset(file.entries[0].particlefx.effect, 0, sizeof(file.entries[0].particlefx.effect));
	std::memset(file.entries[0].particlefx.userpoint, 0, sizeof(file.entries[0].particlefx.userpoint));
	const DefWriteResult emptied = def_write_items(file);
	const auto [gone, put] = lines_changed(full.text, emptied.text);
	size_t attrib_lines = 0, widest = 0;
	for (size_t at = full.text.find("attrib:"); at != std::string::npos; at = full.text.find("attrib:", at + 1)) {
		++attrib_lines;
		const std::string one = full.text.substr(at, full.text.find('\r', at) - at);
		widest = std::max(widest, size_t(std::count(one.begin(), one.end(), ' ')));
	}
	if (!full.ok() || !emptied.ok() || gone != 1 || put != 0 || emptied.text.find("particlefx") != std::string::npos ||
	    !emptied.reordered.empty() || attrib_lines != 2 || widest != 29 || flags != 30) {
		std::printf("FAIL kept layout: a slot emptied (-%zu +%zu), %zu attrib line(s) of up to %zu words\n%s\n", gone, put,
		            attrib_lines, widest, emptied.text.c_str());
		++failures;
	}
	def_free_items(&file);

	// The record whose own order reads back otherwise (a deceleration of 0 before the acceleration that
	// defaults an unset one) in the table's order, named; the other keeps its own (hp before id).
	const char *ordered = "begin \"Car\"\r\n\tid 100100\r\n\ttype vehicle\r\n\tdeceleration 70\r\n\tacceleration 15\r\nend\r\n"
	                      "begin \"Kept\"\r\n\thp 40\r\n\tid 100101\r\n\ttype vehicle\r\nend\r\n";
	def_parse_items_memory(reinterpret_cast<const uint8_t *>(ordered), std::strlen(ordered), &file, nullptr);
	file.entries[0].deceleration = 0;
	const DefWriteResult reordered = def_write_items(file);
	if (!reordered.ok() || reordered.reordered != std::vector<std::string>{"Car"} ||
	    reordered.text.find("begin \"Kept\"\r\n\thp 40\r\n\tid 100101\r\n") == std::string::npos) {
		std::printf("FAIL kept layout: one record in the table's order\n%s\n", reordered.text.c_str());
		++failures;
	}
	def_free_items(&file);

	for (size_t k = 0; k < kDefRecordKindCount; ++k)
		if (def_properties(DefRecordKind(k)).size() >= DEF_LINE_ORDER_SQB_RATE) {
			std::printf("FAIL kind %zu's property table reaches the alias steps\n", k);
			++failures;
		}
	return failures;
}

// The squib and door numbers (the review's W7) at both of the game's FPU precisions: each text the writer
// puts down reads back to its word through the item parser's arithmetic at 53 bits and at the 24 bits of
// the reloads after the D3D device is up (each result rounded to a float's mantissa), over the ranges the
// review counted: 16.16 words 0..70000 and 20000 more below 2^24, open rates 1..60000, squib rates
// -2000..20000.
int numbers_at_both_precisions() {
	const auto at = [](double value, bool coarse) { return coarse ? double(float(value)) : value; };
	const auto ftol = [](double value) { return int32_t(std::trunc(value)); };
	size_t wrong = 0;
	const auto check = [&](const std::string &text, const auto &reads, const char *what, int32_t word) {
		const double read = std::strtod(text.c_str(), nullptr);
		if (reads(read, false) != word || reads(read, true) != word) {
			if (++wrong <= 5) std::printf("FAIL %s %d written %s\n", what, word, text.c_str());
		}
	};
	const auto q16 = [&](double read, bool coarse) { return ftol(at(read * 65536.0, coarse)); };
	for (int32_t word = 0; word <= 70000; ++word) check(def_squib_q16_text(word), q16, "q16", word);
	uint32_t seed = 12345;
	for (int i = 0; i < 20000; ++i) {
		seed = seed * 1103515245u + 12345u;
		const int32_t word = int32_t(seed % (1u << 24));
		check(def_squib_q16_text(word), q16, "q16", word);
	}
	const auto open = [&](double read, bool coarse) { return ftol(at(65536.0 / at(read * 62.0, coarse), coarse)); };
	for (int32_t rate = 1; rate <= 60000; ++rate) check(def_door_open_rate_text(rate), open, "open_rate", rate);
	const auto squib = [&](double read, bool coarse) { return ftol(at(62.0 / read, coarse)); };
	for (int32_t ticks = -2000; ticks <= 20000; ++ticks)
		if (ticks) check(def_squib_rate_text(ticks), squib, "sqb_rate", ticks);
	if (wrong) std::printf("FAIL %zu number(s) read back otherwise at a precision\n", wrong);
	return wrong ? 1 : 0;
}

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	for (const auto &empty : {def_write_items({}), def_write_weapons({}), def_write_ammo({})})
		if (!empty.ok() || empty.text.empty()) ++failures; // runtime reads distinguish empty payloads from empty tables
	failures += clean("items.def",
		"// source comment\r\nbegin \"Test item\"\r\nid 100001\r\ntype vehicle\r\nhp 125\r\narmor 20 30\r\n"
		"graphic model\r\nanim_def anim\r\nkz 0.5\r\ndebris_scale 1.25\r\n"
		"particlefx smoke exhaust\r\naddeweap gun 100002 30 40 50 60\r\n"
		"pcvehicle_spawnlist 8 4\r\npcvehicle_spawnlist 4\r\nend\r\n"
		"begin \"Second\"\r\npcvehicle_spawnlist 8\r\nend\r\n");
	// A SIGHTS row reads one flag, `scale` or `slide`, by its place (its eighth token [orig: the sights
	// arm @0x544B7A..0x544BC3]): one row each.
	failures += clean("weapon.def",
		"ammoclass_max_carry bullets 300\r\nweapon \"WPN_TEST\"\r\ncategory 1\r\n"
		"round_type AT_TEST\r\nweaponweight 3.25\r\nclipweight 0.1\r\n"
		"stability 1, 0.5, 0\r\nerror 0.1, 0.2, 0.3, 0.4, 0.5, 0.6\r\n"
		"pos 1 2 3 359.99 180 90\r\nsights scope 0 0 640 480 blend scale\r\nsights scope 0 0 640 480 blend slide 3\r\n"
		"action \"Fire\"\r\nfunction Shoot 1 2 3 4\r\nctrlreg trigger\r\nctrlreginc 2\r\n"
		"texttoken Fire\r\ndupsound 2 4\r\ndelay auto\r\nend\r\nend\r\n");
	failures += clean("ammo.def",
		"ammo AT_TEST\r\nvelocity 1500\r\nflag silenced\r\nmax_age 1.5\r\nerror 0.125\r\n"
		"turnrate_maxyaw 1.5\r\nboresight_maxang 360\r\nkz_pieslice 180\r\n"
		"light_move 3 255 120 20\r\nlight_impact 4 10 20 30 0.3\r\n"
		"effects_table\r\nmetal hit spark 4\r\nend\r\nend\r\n");
	failures += clean("items.def",
		"begin \"Coupled\"\r\nsound_profile male\r\nsound_profilefemale \"\"\r\nacceleration 2\r\ndeceleration 0\r\n"
		"score -23\r\ngraphicenemy enemy\r\ntextid label\r\nrotor_parts 1 2 3 4\r\naux_parts 5 6 7 8\r\nend\r\n"
		"begin \"Powerup\"\r\ntype powerup\r\npowerupdef PU_TEST\r\nend\r\n");
	failures += clean("ammo.def", "ammo TRACER\r\ntracer_type stdred stdgreen\r\ndopplerdiv 3\r\nkz_sound hit\r\n"
	                              "secondary_effect Effect_Burn\r\nend\r\n");
	// Retail forms the first writer refused: a percentage stored as a float, and the
	// nameless region-0 timing that `particletesttime` authors.
	failures += clean("items.def", "begin \"Hut\"\r\nid 101202\r\ntype building\r\ngraphic JHut1\r\nlight_transfer 35\r\nend\r\n");
	failures += clean("items.def", "begin \"Emit\"\r\nid 100059\r\ntype building\r\nai_function emit\r\n"
	                               "particlefxw1 Effect_rocketAft FX00\r\nparticletesttime     90 0\r\nend\r\n");
	// A number the game reads as another: a category past 0..11 and a rank past 0..64 as 0
	// [orig: WeaponDefs_ParseLineCallback @ 0x5439a8 / 0x5439fd], a unit_type past a byte as its
	// low byte [orig: ItemDef_ParseProperty @ 0x49ee47]. The record holds what the game reads and
	// the line is reported, never a blocker; saving writes what the game reads.
	{
		const Outcome wide = run("weapon.def", "weapon \"WPN_WIDE\"\r\ncategory 13\r\nrank 70\r\nend\r\n");
		const Outcome kept = run("weapon.def", "weapon \"WPN_KEPT\"\r\ncategory 11\r\nrank 64\r\nend\r\n");
		const Outcome byte = run("items.def", "begin \"Wide\"\r\nid 100001\r\nunit_type 259\r\nend\r\n");
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
		const char *text = "weapon \"WPN_WIDE\"\r\ncategory -1\r\nrank 65\r\nend\r\n";
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
	// unit_type 2147483651 is byte 255 (not 3), -2147483649 byte 0 (written as `unit_type 0`: the
	// file has the line, which a save keeps at the default as at any value).
	{
		const Outcome huge = run("weapon.def", "weapon \"WPN_HUGE\"\r\ncategory 4294967297\r\nrank -4294967297\r\nend\r\n");
		const Outcome high = run("items.def", "begin \"High\"\r\nid 100001\r\nunit_type 2147483651\r\nend\r\n");
		const Outcome low = run("items.def", "begin \"Low\"\r\nid 100002\r\nunit_type -2147483649\r\nend\r\n");
		const auto reads_as = [](const Outcome &out, const char *field, const char *value) {
			for (const DefIssue &d : out.diagnostics)
				if (d.code == DefIssueCode::Reinterpreted && d.field == field &&
				    d.message.find(std::string("reads this as ") + value + ".") != std::string::npos)
					return true;
			return false;
		};
		if (huge.blocking() || huge.diagnostics.size() != 2 || !reads_as(huge, "category", "0") ||
		    !reads_as(huge, "rank", "0") || huge.written.text.find("4294967297") != std::string::npos ||
		    huge.written.text.find("category 1\r\n") != std::string::npos ||
		    high.blocking() || !reads_as(high, "unit_type", "255") ||
		    high.written.text.find("unit_type 255\r\n") == std::string::npos || low.blocking() ||
		    !reads_as(low, "unit_type", "0") || low.written.text.find("unit_type 0\r\n") == std::string::npos) {
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
		const Outcome heat = run("weapon.def", "weapon \"WPN_HOT\"\r\nheat_sound OVERHEAT\r\nend\r\n");
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
		const char *text = "weapon \"WPN_HOT\"\r\nheat_sound \"HOT LOOP\"\r\nsoundhead \"HEAD,ONE\"\r\nround_type \"AT;X\"\r\n"
		                   "sights \"scope tex\" 0 0 640 480 blend\r\n"
		                   "action \"FIRE\"\r\nsoundset \"SHOT\tTAIL\"\r\nend\r\nend\r\n";
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
	failures += mission_editor_keys();
	failures += expansion_catalog_lines();
	failures += review_def_lines();
	failures += powerup_table();
	failures += tokenizer_rules();
	failures += authored_units();
	failures += kept_layout();
	failures += kept_layout_edits();
	failures += numbers_at_both_precisions();
	failures += scaled_defaults();
	{
		// The minted items fixture, each member through its line.
		const std::vector<uint8_t> fixture = test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/def/items.def");
		if (fixture.empty()) { std::printf("FAIL fixtures/def/items.def unread\n"); ++failures; }
		else failures += authored_catalog("items.def", fixture, true);
	}
	// Malformed values and blocks still refuse the write.
	failures += refused("weapon.def", "ammoclass_max_carry bullets nope\r\n");
	// A SIGHTS row reads its sixth value as the blend mode, a name the material maker does not know
	// as `blend` [orig: WeaponDef_CreateBlendNamedMaterial @0x540190 over tokens[7] @0x544B3F]:
	// reported, never a blocker; a `slide` with no frame count reads a slot an earlier line left.
	{
		const Outcome mode = run("weapon.def", "weapon \"Mode\"\r\nsights a 0 0 1 1 unknown\r\nend\r\n");
		if (mode.blocking() || mode.diagnostics.size() != 1 || mode.diagnostics[0].code != DefIssueCode::Reinterpreted ||
		    mode.diagnostics[0].message.find("reads this as blend") == std::string::npos || !mode.written.ok() ||
		    mode.written.text.find("\tsights a 0 0 1 1 blend\r\n") == std::string::npos) {
			std::printf("FAIL an unknown sight blend reads as blend:\n%s\n", mode.written.text.c_str());
			++failures;
		}
	}
	failures += refused("weapon.def", "weapon \"Bad\"\r\nsights a 0 0 1 1 blend slide\r\nend\r\n");
	failures += refused("weapon.def", "weapon \"Open\"\r\ncategory 1\r\n");
	failures += refused("ammo.def", "ammo Bad\r\neffects_table\r\nmetal hit snd nope\r\nend\r\nend\r\n");
	failures += refused("ammo.def", "effects_table\r\nend\r\n");
	failures += refused("ammo.def", "ammo Bad\r\ntracer_type unknown\r\nend\r\n");
	failures += refused("items.def", "begin \"Bad\"\r\nhp twelve\r\nend\r\n");
	failures += refused("items.def", "begin \"Partial\"\r\naddeweap ewep01 100201 15 37\r\nend\r\n");

	const std::string root = retail::install();
	if (root.empty()) retail::skip_leg("def_write retail corpus needs OPENNOVA_JO_DIR");
	else {
		opennova::Vfs vfs;
		vfs.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		if (!vfs.mount_game(root, "", opennova::VfsMountMode::Packed)) return 1;
		// The base catalogs, then each installed expansion's (what `/exp <name>` serves: JO:CA's
		// jox01, whose items.def carries bare `attrib:` lines, effect-only particle slots and
		// addeweap placeholder text, ADR 0046 S16).
		const auto catalogs = [&](opennova::Vfs &mounted, const std::string &label) {
			for (const char *family : {"items.def", "weapon.def", "ammo.def"}) {
				std::vector<uint8_t> bytes;
				if (!mounted.read_file(family, bytes)) { std::printf("Cannot read %s%s\n", label.c_str(), family); ++failures; continue; }
				// Every retail catalog must open in the editor: nothing blocking, and the
				// canonical rewrite must succeed. Ignored lines are counted for the record.
				const Outcome out = run(family, bytes);
				std::map<std::string, int> ignored;
				for (const auto &d : out.diagnostics) ++ignored[d.field];
				std::printf("%s%s: %zu records, %zu ignored line(s), %s\n", label.c_str(), family, out.count, out.ignored(),
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
				// Every record keeps the order it was read in (none written in the table's: reordered), and
				// a save after one field changed changes that one line of what a save before it writes (the
				// writer keeps the file's order and indentation): an item's hp, a weapon's clipsize, an
				// ammo's velocity, each on the first record whose file has the line.
				const auto layout = [&](const DefWriteResult &before, const DefWriteResult &after, const char *what) {
					const size_t changed = lines_differing(before.text, after.text);
					std::printf("%s%s: %zu record(s) in the table's order; one %s changed, %zu line(s) of the save change\n",
					            label.c_str(), family, before.reordered.size(), what, changed);
					for (const std::string &name : before.reordered) std::printf("  reordered: %s\n", name.c_str());
					if (!before.ok() || !after.ok() || changed != 1 || !before.reordered.empty() || !after.reordered.empty())
						++failures;
				};
				const auto first_with = [](DefRecordKind kind, const DefLineOrder &order, const char *key) {
					const std::vector<DefProperty> &properties = def_properties(kind);
					for (size_t i = 0; i < order.count; ++i)
						if (order.steps[i] < properties.size() && properties[order.steps[i]].key == key) return true;
					return false;
				};
				if (std::strcmp(family, "items.def") == 0) {
					DefItemsFile parsed{};
					def_parse_items_memory(bytes.data(), bytes.size(), &parsed, nullptr);
					const DefWriteResult before = def_write_items(parsed);
					// No retail item text holds a space (the writer would quote it for the game's tokenizer,
					// where our parser kept the rest of the line): a quote is a block header's alone.
					size_t quoted = 0;
					for (size_t at = before.text.find('"'); at != std::string::npos; at = before.text.find('"', at + 1)) {
						const size_t line = before.text.rfind('\n', at) + 1;
						if (before.text.compare(line, 6, "begin ") != 0) ++quoted;
					}
					if (quoted) {
						std::printf("FAIL %sitems.def: %zu quote(s) outside a begin line\n", label.c_str(), quoted);
						++failures;
					}
					for (size_t i = 0; i < parsed.count; ++i)
						if (first_with(DefRecordKind::Item, parsed.entries[i].line_order, "hp")) {
							parsed.entries[i].hp += 1;
							break;
						}
					layout(before, def_write_items(parsed), "hp");
					def_free_items(&parsed);
				} else if (std::strcmp(family, "weapon.def") == 0) {
					DefWeaponsFile parsed{};
					def_parse_weapons_memory(bytes.data(), bytes.size(), &parsed, nullptr);
					const DefWriteResult before = def_write_weapons(parsed);
					for (size_t i = 0; i < parsed.count; ++i)
						if (first_with(DefRecordKind::Weapon, parsed.entries[i].line_order, "clipsize")) {
							parsed.entries[i].clipsize += 1;
							break;
						}
					layout(before, def_write_weapons(parsed), "clipsize");
					def_free_weapons(&parsed);
				} else {
					DefAmmoFile parsed{};
					def_parse_ammo_memory(bytes.data(), bytes.size(), &parsed, nullptr);
					const DefWriteResult before = def_write_ammo(parsed);
					for (size_t i = 0; i < parsed.count; ++i)
						if (first_with(DefRecordKind::Ammo, parsed.entries[i].line_order, "velocity")) {
							parsed.entries[i].velocity += 1;
							break;
						}
					layout(before, def_write_ammo(parsed), "velocity");
					def_free_ammo(&parsed);
				}
			}
		};
		catalogs(vfs, "");
		for (const std::string &expansion : retail::expansions()) {
			opennova::Vfs served;
			served.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
			if (!served.mount_game(root, expansion, opennova::VfsMountMode::Packed)) return 1;
			catalogs(served, "/exp " + expansion + ": ");
		}
		// The powerup table opens with nothing blocking and writes; its canonical form written again
		// is the same text (the writer compares the rows it parses back).
		std::vector<uint8_t> powerup;
		if (!vfs.read_file("powerup.def", powerup)) { std::printf("Cannot read powerup.def\n"); return 1; }
		const Outcome first = run("powerup.def", powerup);
		const Outcome second = run("powerup.def", first.written.text.c_str());
		std::printf("powerup.def: %zu records, %zu ignored line(s), %s\n", first.count, first.ignored(),
		            first.written.ok() ? "written" : "REFUSED");
		if (first.blocking() || !first.written.ok() || first.count == 0 || !second.written.ok() ||
		    second.written.text != first.written.text || second.count != first.count || !second.diagnostics.empty()) {
			for (const auto &d : first.diagnostics)
				if (d.blocks()) std::printf("  BLOCKING line %zu %s.%s: %s\n", d.line, d.record.c_str(), d.field.c_str(), d.message.c_str());
			for (const auto &d : first.written.diagnostics)
				std::printf("  write %s.%s: %s\n", d.record.c_str(), d.field.c_str(), d.message.c_str());
			std::printf("FAIL retail powerup.def is no fixed point\n");
			++failures;
		}
	}
	return failures ? 1 : 0;
}
