// The weapon.def name cap. The game copies a weapon's name 32 bytes into def+0x14 [orig:
// WeaponDefs_ParseLineCallback, strncpy(def+0x14, tokens[2], 0x20) @0x543737], right ahead of
// `sameas` at def+0x34 [orig: strncpy(def+0x34, value, 0x20) @0x544056..0x544072]. A 32-character
// name keeps no terminator, so it reads back as itself only while the block has no `sameas`, and
// runs on into it otherwise (D-ITEMDEF-10). Pinned here: def_weapon_name_chars says so (32, 31 in
// a block with `sameas`); the writer puts down a 32-character name only in a block with no `sameas`
// (31 otherwise; a longer one is refused, as a name past the cap always was); the parser keeps 32
// characters either way, as the game's copy does, reports a 32-character name in a block with
// `sameas` as one a save cannot give back (blocking, on the `weapon` line, among that line's
// findings), and leaves a name past 32 in a block with no `sameas` a reinterpretation, never a
// blocker.
#include <formats/def/def_notes.h>
#include <formats/def/def_write.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace opennova::def;

namespace {

int fail = 0;
#define CHECK(c, m)                                                                                 \
	do {                                                                                            \
		if (!(c)) {                                                                                 \
			std::fprintf(stderr, "FAIL: %s\n", m);                                                  \
			++fail;                                                                                 \
		}                                                                                           \
	} while (0)

const std::string kName31 = "WPN_ABCDEFGHIJKLMNOPQRSTUVWXYZ0"; // 31 characters
const std::string kName32 = kName31 + "1";                      // 32
const std::string kName40 = kName32 + "23456789";               // 40

struct Parsed {
	DefWeaponsFile file{};
	DefParseReport report;
	explicit Parsed(const std::string &text) {
		def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text.data()), text.size(), &file, &report);
	}
	~Parsed() { def_free_weapons(&file); }
	Parsed(const Parsed &) = delete;
	Parsed &operator=(const Parsed &) = delete;
};

// One weapon made from nothing (no source text): the writer's own form.
DefWriteResult write_minted(const std::string &name, const char *sameas) {
	DefWeaponDef weapon;
	def_init_weapon(weapon);
	std::snprintf(weapon.weapon_name, sizeof(weapon.weapon_name), "%s", name.c_str());
	std::snprintf(weapon.sameas, sizeof(weapon.sameas), "%s", sameas);
	DefWeaponsFile file{};
	file.entries = &weapon;
	file.count = 1;
	return def_write_weapons(file);
}

bool refused_for_name(const DefWriteResult &written) {
	if (written.ok() || !written.text.empty()) return false;
	for (const DefIssue &d : written.diagnostics)
		if (d.field == "weapon" && d.message == "Name exceeds its capacity.") return true;
	return false;
}

// What the writer's text reads back as.
bool reads_back(const DefWriteResult &written, const std::string &name, const char *sameas) {
	Parsed back(written.text);
	return back.report.empty() && back.file.count == 1 && name == back.file.entries[0].weapon_name &&
	       std::strcmp(back.file.entries[0].sameas, sameas) == 0;
}

} // namespace

int main() {
	// The characters of a name the reader keeps as that name: 32 with no `sameas`, 31 with one.
	{
		DefWeaponDef bare;
		def_init_weapon(bare);
		DefWeaponDef based = bare;
		std::snprintf(based.sameas, sizeof(based.sameas), "%s", "WPN_M16");
		CHECK(def_weapon_name_chars(bare) == 32 && def_weapon_name_chars(based) == 31,
		      "def_weapon_name_chars is 32 with no sameas, 31 with one");
	}

	// The writer's own form: 32 characters with no `sameas`, 31 with one.
	{
		const DefWriteResult full = write_minted(kName32, "");
		CHECK(full.ok() && full.text.find("weapon \"" + kName32 + "\"\r\n") != std::string::npos,
		      "a 32-character name with no sameas is written");
		CHECK(full.ok() && reads_back(full, kName32, ""), "a 32-character name with no sameas reads back whole");

		const DefWriteResult runs_on = write_minted(kName32, "WPN_OTHER");
		CHECK(refused_for_name(runs_on), "a 32-character name with sameas is refused");

		const DefWriteResult held = write_minted(kName31, "WPN_OTHER");
		CHECK(held.ok() && held.text.find("weapon \"" + kName31 + "\"\r\n") != std::string::npos &&
		          held.text.find("\tsameas WPN_OTHER\r\n") != std::string::npos,
		      "a 31-character name with sameas is written");
		CHECK(held.ok() && reads_back(held, kName31, "WPN_OTHER"), "a 31-character name with sameas reads back whole");

		CHECK(refused_for_name(write_minted(kName40, "")), "a name past 32 is refused");
	}

	// A 32-character name in a block with no `sameas`: no finding, saved as the file has it.
	{
		const std::string text = "weapon \"" + kName32 + "\"\r\ncategory 1\r\nend\r\n";
		Parsed parsed(text);
		CHECK(parsed.report.empty() && parsed.file.count == 1 && kName32 == parsed.file.entries[0].weapon_name &&
		          parsed.file.entries[0].unmodeled_count == 0,
		      "a 32-character name with no sameas parses whole and clean");
		CHECK(def_write_weapons(parsed.file).ok(), "a parsed 32-character name with no sameas saves");

		DefWeaponsFile noted{};
		DefParseReport report;
		DefTextNotes notes;
		def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text.data()), text.size(), &noted, &report, notes);
		const DefWriteResult kept = def_write_weapons(noted, &notes);
		CHECK(report.empty() && kept.ok() && kept.text == text,
		      "a 32-character name with no sameas saves over its layout as the file was");
		def_free_weapons(&noted);
	}

	// A 32-character name in a block with `sameas`: the record keeps 32 characters and the sameas, as the
	// game's two copies do; the line is reported as one a save cannot give back, blocking, and the save
	// is refused.
	{
		const std::string text = "weapon \"" + kName32 + "\"\r\nunknownkey 1\r\nsameas WPN_OTHER\r\nend\r\n";
		Parsed parsed(text);
		const DefWeaponDef *w = parsed.file.count == 1 ? &parsed.file.entries[0] : nullptr;
		CHECK(w && kName32 == w->weapon_name && std::strcmp(w->sameas, "WPN_OTHER") == 0 && w->unmodeled_count == 1,
		      "a 32-character name with sameas keeps both");
		// Its finding sits among the `weapon` line's, ahead of the later line's.
		CHECK(parsed.report.size() == 2 && parsed.report[0].code == DefIssueCode::Unrepresentable &&
		          parsed.report[0].blocks() && parsed.report[0].line == 1 && parsed.report[0].field == "weapon" &&
		          parsed.report[0].record == kName32 && parsed.report[1].code == DefIssueCode::UnknownProperty &&
		          parsed.report[1].line == 2,
		      "a 32-character name with sameas is reported on its weapon line, blocking");
		CHECK(!def_write_weapons(parsed.file).ok(), "a parsed 32-character name with sameas is not saved");

		// Without a report (the runtime's read) the record is the same.
		DefWeaponsFile runtime{};
		def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text.data()), text.size(), &runtime);
		CHECK(runtime.count == 1 && kName32 == runtime.entries[0].weapon_name &&
		          std::strcmp(runtime.entries[0].sameas, "WPN_OTHER") == 0,
		      "the runtime read keeps 32 characters and the sameas");
		def_free_weapons(&runtime);
	}

	// A 31-character name in a block with `sameas`: clean, and saved.
	{
		Parsed parsed("weapon \"" + kName31 + "\"\r\nsameas WPN_OTHER\r\nend\r\n");
		CHECK(parsed.report.empty() && def_write_weapons(parsed.file).ok(),
		      "a 31-character name with sameas parses clean and saves");
	}

	// A name past 32 in a block with no `sameas` reads as its first 32: a reinterpretation, never a
	// blocker, and saved as what the game reads.
	{
		Parsed parsed("weapon \"" + kName40 + "\"\r\nend\r\n");
		const DefWriteResult written = def_write_weapons(parsed.file);
		CHECK(parsed.report.size() == 1 && parsed.report[0].code == DefIssueCode::Reinterpreted &&
		          !parsed.report[0].blocks() && parsed.file.count == 1 && kName32 == parsed.file.entries[0].weapon_name,
		      "a 40-character name with no sameas reads as its first 32");
		CHECK(written.ok() && written.text.find("weapon \"" + kName32 + "\"\r\n") != std::string::npos,
		      "a 40-character name with no sameas saves as its first 32");
	}

	// An entry no `end` closes is checked too: the name's finding ahead of the missing `end`'s.
	{
		Parsed parsed("weapon \"" + kName32 + "\"\r\nsameas WPN_OTHER\r\n");
		CHECK(parsed.report.size() == 2 && parsed.report[0].code == DefIssueCode::Unrepresentable &&
		          parsed.report[0].line == 1 && parsed.report[1].code == DefIssueCode::MalformedBlock,
		      "an unclosed entry's 32-character name with sameas is reported");
	}

	if (fail) {
		std::fprintf(stderr, "%d failure(s)\n", fail);
		return 1;
	}
	std::printf("def_weapon_name_cap OK\n");
	return 0;
}
