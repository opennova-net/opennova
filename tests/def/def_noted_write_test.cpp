// The def writers over a file's modeled layout (def_notes.h; the demo round's bugs 1 and 3, generated as
// the maintainer's ruling of 2026-10-04 has it: "model it, generate it"): a file read with its layout and
// saved unchanged is generated as the file was, byte for byte (its comments, its spacing, a number's own
// spelling, the words and lines the game skips, its line endings, a last line with none); a minted record
// with no source text is the writer's own form, byte for byte; a change of the layout data is in the
// output; one field changed changes that one line, keeping its blanks, its comment and its unchanged
// words; a flag, a row or a block added or removed adds or removes its own lines alone; a row moved or
// duplicated within its record follows the record's order; a record whose own form its edit cannot carry
// keeps its comment lines and says what it does not keep; a copy is written in the writer's form after the
// rest; a carry limit stands among the weapons where the file has it; an attribute past a full `attrib:`
// line goes on a line of its own; and every real the writer puts down of its own is the shortest decimal that reads back
// to the same value (`pos 4 -5 -186 0 0 0`, never `0.00000762939453125`; `21.76`, never
// `21.7600002288818359375`). With a JO install configured: the first save of every retail catalog (items,
// weapon with its action blocks, ammo, the base's and each installed expansion's, and powerup) is the
// original, one field changed changes one line of it, and the writer's own form of every catalog puts
// down no real with a long tail, nor one a decimal of one place fewer reads the same as (each record read
// alone again with it). The retail edits: a whole number, a real (its new line's reals the shortest their
// reader takes back) and, in items, an attribute (its `attrib:` line, the words the game skips kept), each
// one line.
#include <formats/def/def_notes.h>
#include <formats/def/def_scan.h>
#include <formats/def/def_write.h>
#include <formats/def/def_write_record.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include "common/retail_paths.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace opennova::def;

namespace {

std::vector<std::string> split_lines(const std::string &text) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at < text.size()) {
		const size_t end = text.find('\n', at);
		out.push_back(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
		at = end == std::string::npos ? text.size() : end + 1;
	}
	return out;
}

// The lines removed from `a` and added in `b` (a longest common subsequence of lines, over the window
// where they differ: the same lines before and after it are matched first).
std::pair<size_t, size_t> lines_changed(const std::string &a, const std::string &b) {
	std::vector<std::string> x = split_lines(a), y = split_lines(b);
	size_t head = 0;
	while (head < x.size() && head < y.size() && x[head] == y[head]) ++head;
	size_t tail = 0;
	while (tail < x.size() - head && tail < y.size() - head && x[x.size() - 1 - tail] == y[y.size() - 1 - tail]) ++tail;
	x = std::vector<std::string>(x.begin() + std::ptrdiff_t(head), x.end() - std::ptrdiff_t(tail));
	y = std::vector<std::string>(y.begin() + std::ptrdiff_t(head), y.end() - std::ptrdiff_t(tail));
	if (x.size() * y.size() > 4000000) return {x.size(), y.size()};
	std::vector<std::vector<uint32_t>> common(x.size() + 1, std::vector<uint32_t>(y.size() + 1, 0));
	for (size_t i = x.size(); i-- > 0;)
		for (size_t j = y.size(); j-- > 0;)
			common[i][j] = x[i] == y[j] ? common[i + 1][j + 1] + 1 : std::max(common[i + 1][j], common[i][j + 1]);
	return {x.size() - common[0][0], y.size() - common[0][0]};
}

// The first line of `b` that differs from `a`'s at its place (for the reports).
std::string first_difference(const std::string &a, const std::string &b) {
	const std::vector<std::string> x = split_lines(a), y = split_lines(b);
	for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
		const std::string l = i < x.size() ? x[i] : std::string("<none>"), r = i < y.size() ? y[i] : std::string("<none>");
		if (l != r) return "line " + std::to_string(i + 1) + ":\r\n  was: " + l + "\r\n  now: " + r;
	}
	return "none";
}

const uint8_t *bytes_of(const std::string &text) { return reinterpret_cast<const uint8_t *>(text.data()); }

// A family read with its notes and written over them, with an edit between.
struct Items {
	DefItemsFile file{};
	DefTextNotes notes;
	DefParseReport report;
	explicit Items(const std::string &text) { def_parse_items_memory(bytes_of(text), text.size(), &file, &report, notes); }
	~Items() { def_free_items(&file); }
	DefWriteResult save() const { return def_write_items(file, &notes); }
};
struct Weapons {
	DefWeaponsFile file{};
	DefTextNotes notes;
	DefParseReport report;
	explicit Weapons(const std::string &text) { def_parse_weapons_memory(bytes_of(text), text.size(), &file, &report, notes); }
	~Weapons() { def_free_weapons(&file); }
	DefWriteResult save() const { return def_write_weapons(file, &notes); }
};
struct Ammo {
	DefAmmoFile file{};
	DefTextNotes notes;
	DefParseReport report;
	explicit Ammo(const std::string &text) { def_parse_ammo_memory(bytes_of(text), text.size(), &file, &report, notes); }
	~Ammo() { def_free_ammo(&file); }
	DefWriteResult save() const { return def_write_ammo(file, &notes); }
};
struct Powerups {
	DefPowerupFile file{};
	DefTextNotes notes;
	DefParseReport report;
	explicit Powerups(const std::string &text) { def_parse_powerup_memory(bytes_of(text), text.size(), &file, &report, notes); }
	~Powerups() { def_free_powerup(&file); }
	DefWriteResult save() const { return def_write_powerup(file, &notes); }
};

void print_failures(const DefWriteResult &written) {
	for (const auto &d : written.diagnostics) std::printf("  %s.%s: %s\n", d.record.c_str(), d.field.c_str(), d.message.c_str());
}

// The save is the original text, byte for byte.
int same_as_read(const char *what, const std::string &original, const DefWriteResult &written) {
	if (written.ok() && written.text == original) return 0;
	std::printf("FAIL %s: the first save is not the file as read (%s)\n%s\n", what, written.ok() ? "written" : "refused",
	            first_difference(original, written.text).c_str());
	print_failures(written);
	return 1;
}

// The save after an edit differs from the original by `removed` lines gone and `added` lines new, and holds
// `line` (when given) as a line of its own.
int changed(const char *what, const std::string &original, const DefWriteResult &written, size_t removed, size_t added,
            const char *line = nullptr) {
	const auto [gone, put] = written.ok() ? lines_changed(original, written.text) : std::pair<size_t, size_t>{0, 0};
	const bool holds = !line || ("\n" + written.text).find(std::string("\n") + line) != std::string::npos;
	if (written.ok() && gone == removed && put == added && holds) return 0;
	std::printf("FAIL %s: -%zu +%zu lines (want -%zu +%zu)%s\n%s\n", what, gone, put, removed, added,
	            holds ? "" : ", the line is not there", written.ok() ? first_difference(original, written.text).c_str() : "");
	if (line && !holds) std::printf("  wanted: %s\n", line);
	print_failures(written);
	return 1;
}

const std::string kItems =
        "// Item definitions, made by hand\r\n"
        "\r\n"
        "BEGIN \"Truck\"   // a truck\r\n"
        "  id     100100\r\n"
        "  TYPE vehicle\r\n"
        "  attrib: AIData neutral PlayerControl exp1\r\n"
        "  hp 3000 ; tough\r\n"
        "  shadow shadow8.tga 4.50 8.9 0.0 -0.14\r\n"
        "  unknownkey 5\r\n"
        "  particlefx smoke exhaust\r\n"
        "  addeweap ewep01 101419\r\n"
        "  sound_profile SP_Truck\r\n"
        "end\r\n"
        "\t\r\n"
        "begin \"Car\"\r\n"
        "\tid 100101\r\n"
        "\ttype vehicle\r\n"
        // The walk drops the last byte of a tail line with no CR LF [orig: File_ParseASCIIFile
        // @0x53D8E9 / @0x53D8EC], so a last `end` with none would read `en`: the file ends its
        // last line (the retail catalogs end in a NUL the walk drops, their leg below).
        "end\r\n";

int noted_items() {
	int failures = 0;
	{
		Items read(kItems);
		failures += same_as_read("items", kItems, read.save());
	}
	{
		Items read(kItems);
		read.file.entries[0].hp = 2500;
		failures += changed("items: hp", kItems, read.save(), 1, 1, "  hp 2500 ; tough\r");
	}
	{
		Items read(kItems);
		read.file.entries[0].attrib &= ~DEF_ITEM_ATTRIB_PLAYERCONTROL;
		failures += changed("items: an attribute cleared", kItems, read.save(), 1, 1, "  attrib: AIData neutral exp1\r");
	}
	{
		Items read(kItems);
		read.file.entries[0].attrib |= DEF_ITEM_ATTRIB_MISSILE;
		failures += changed("items: an attribute set", kItems, read.save(), 1, 1,
		                    "  attrib: AIData neutral PlayerControl exp1 missile\r");
	}
	{
		Items read(kItems);
		read.file.entries[0].shadow_width = 5.0f;
		failures += changed("items: a shadow's width", kItems, read.save(), 1, 1, "  shadow shadow8.tga 5 8.9 0.0 -0.14\r");
	}
	{
		// A line the file did not have: after the record's lines.
		Items read(kItems);
		read.file.entries[1].hp = 40;
		failures += changed("items: a line added", kItems, read.save(), 0, 1, "\thp 40\r");
	}
	{
		// The attachment row removed: its line alone.
		Items read(kItems);
		read.file.entries[0].emplacement_attachments_count = 0;
		failures += changed("items: a row removed", kItems, read.save(), 1, 0);
	}
	{
		// A copy (its notes cleared, as a Duplicate's): the original's lines stand, the copy's follow.
		Items read(kItems);
		DefItemsFile grown = read.file;
		std::vector<DefItemDef> entries(read.file.entries, read.file.entries + read.file.count);
		DefItemDef copy = entries[1];
		def_clear_notes(DefRecordKind::Item, &copy);
		std::strcpy(copy.display_name, "Car (copy)");
		copy.id = 100102;
		entries.push_back(copy);
		grown.entries = entries.data();
		grown.count = entries.size();
		const DefWriteResult written = def_write_items(grown, &read.notes);
		const auto [gone, put] = lines_changed(kItems, written.text);
		// (written in the file's indentation; the original's last line ends with CR LF, so it stands)
		if (!written.ok() || gone != 0 || put < 5 || written.text.find("begin \"Car (copy)\"\r\n  id 100102\r\n") == std::string::npos) {
			std::printf("FAIL items: a copy (-%zu +%zu)\n%s\n", gone, put, written.text.c_str());
			print_failures(written);
			++failures;
		}
	}
	{
		// Read without notes: the writer's own form (none of the file's spelling).
		DefItemsFile plain{};
		def_parse_items_memory(bytes_of(kItems), kItems.size(), &plain, nullptr);
		const DefWriteResult written = def_write_items(plain);
		if (!written.ok() || written.text == kItems || written.text.find("neutral") != std::string::npos) {
			std::printf("FAIL items: a write without notes is the writer's own form\n%s\n", written.text.c_str());
			++failures;
		}
		def_free_items(&plain);
	}
	return failures;
}

const std::string kWeapons =
        "\r\n"
        "ammoclass_max_carry CLASS_9mm\t\t\t120 \r\n"
        "ammoclass_max_carry CLASS_45cal\t\t90\r\n"
        "\r\n"
        "weapon \"WPN_A\"\r\n"
        "\tcategory 2\r\n"
        "\trank     0\r\n"
        "\r\n"
        "\tflags Underwater\r\n"
        "\tclipsize 15 // rounds\r\n"
        "\tflags Scoped\r\n"
        "\tpos 21.76, -11.72, 3.5, 0.0, 0, 0\r\n"
        "\tsights scope.tga 0 0 640 480 blend\r\n"
        "\tsights reticle.tga 0 0 64 64 add\r\n"
        "\taction \"fire\"\r\n"
        "\t\tfunction wpn_std_fire\r\n"
        "\t\tdelayend   4\r\n"
        "\tend\r\n"
        "\r\n"
        "\taction \"reload\"\r\n"
        "\t\tanim reload\r\n"
        "\tend\r\n"
        "end\r\n";

int noted_weapons() {
	int failures = 0;
	{
		Weapons read(kWeapons);
		failures += same_as_read("weapons", kWeapons, read.save());
	}
	{
		Weapons read(kWeapons);
		read.file.entries[0].clipsize = 30;
		failures += changed("weapons: clipsize", kWeapons, read.save(), 1, 1, "\tclipsize 30 // rounds");
	}
	{
		Weapons read(kWeapons);
		read.file.entries[0].pos[0] = 22.5f;
		failures += changed("weapons: a pose's x", kWeapons, read.save(), 1, 1, "\tpos 22.5, -11.72, 3.5, 0.0, 0, 0");
	}
	{
		Weapons read(kWeapons);
		read.file.entries[0].flags |= DEF_WEAPON_FLAG_AUTO;
		failures += changed("weapons: a flag set", kWeapons, read.save(), 0, 1, "\tflags auto");
	}
	{
		Weapons read(kWeapons);
		read.file.entries[0].flags &= ~DEF_WEAPON_FLAG_UNDERWATER;
		failures += changed("weapons: a flag cleared", kWeapons, read.save(), 1, 0);
	}
	{
		Weapons read(kWeapons);
		read.file.ammo_classes[1].max_carry = 100;
		failures += changed("weapons: a carry limit", kWeapons, read.save(), 1, 1, "ammoclass_max_carry CLASS_45cal\t\t100");
	}
	{
		// The first sight removed: its line alone.
		Weapons read(kWeapons);
		DefWeaponDef &weapon = read.file.entries[0];
		std::memmove(&weapon.sights[0], &weapon.sights[1], sizeof(DefSightEntry));
		weapon.sights_count = 1;
		failures += changed("weapons: a sight removed", kWeapons, read.save(), 1, 0);
	}
	{
		// The first action removed: its lines and the blank line after it, its next's (the lines before a
		// block are its own).
		Weapons read(kWeapons);
		DefWeaponDef &weapon = read.file.entries[0];
		std::memmove(&weapon.actions[0], &weapon.actions[1], sizeof(DefWeaponAction));
		weapon.actions_count = 1;
		failures += changed("weapons: an action removed", kWeapons, read.save(), 4, 0);
	}
	{
		Weapons read(kWeapons);
		read.file.entries[0].actions[1].delayend = 9;
		failures += changed("weapons: an action's line added", kWeapons, read.save(), 0, 1, "\t\tdelayend 9");
	}
	{
		// A carry limit after a weapon block stays after it (the review's Y4): the carry limits and the
		// weapons read into two tables, so the order between them is the file's alone.
		const std::string late = "weapon \"WPN_B\"\r\n\tclipsize 5\r\nend\r\n\r\nammoclass_max_carry CLASS_X 10\r\n"
		                         "weapon \"WPN_C\"\r\n\tclipsize 6\r\nend\r\n";
		Weapons read(late);
		failures += same_as_read("weapons: a carry limit between weapons", late, read.save());
		read.file.ammo_classes[0].max_carry = 12;
		failures += changed("weapons: that carry limit changed", late, read.save(), 1, 1, "ammoclass_max_carry CLASS_X 12");
	}
	return failures;
}

const std::string kAmmo =
        "// Ammo definitions\r\n"
        "ammo AT_A\r\n"
        "    velocity 1500\r\n"
        "    max_age 2.50\r\n"
        "    effects_table\r\n"
        "        metal   hit spark 4\r\n"
        "        dirt    hit dust  2\r\n"
        "    end\r\n"
        "    error 0.125\r\n"
        "end\r\n";

int noted_ammo() {
	int failures = 0;
	{
		Ammo read(kAmmo);
		failures += same_as_read("ammo", kAmmo, read.save());
	}
	{
		Ammo read(kAmmo);
		read.file.entries[0].velocity = 1600;
		failures += changed("ammo: velocity", kAmmo, read.save(), 1, 1, "    velocity 1600\r");
	}
	{
		Ammo read(kAmmo);
		read.file.entries[0].effects_table_count = 1;
		failures += changed("ammo: an effect row removed", kAmmo, read.save(), 1, 0);
	}
	{
		Ammo read(kAmmo);
		read.file.entries[0].effects_table[1].value = 3;
		failures += changed("ammo: an effect row's value", kAmmo, read.save(), 1, 1, "        dirt    hit dust  3\r");
	}
	return failures;
}

const std::string kPowerups =
        "// Powerup definitions\r\n"
        "powerup \"PU_MED\"\r\n"
        "  hp -1\r\n"
        "  respawn_time 30\r\n"
        "  ammo AT_TEST -1\r\n"
        "  action pickup\r\n"
        "    function powerup_med\r\n"
        "  end\r\n"
        "end\r\n";

int noted_powerups() {
	int failures = 0;
	{
		Powerups read(kPowerups);
		failures += same_as_read("powerups", kPowerups, read.save());
	}
	{
		Powerups read(kPowerups);
		read.file.entries[0].respawn_time = 45;
		failures += changed("powerups: respawn_time", kPowerups, read.save(), 1, 1, "  respawn_time 45\r");
	}
	return failures;
}

// The maintainer's ruling of 2026-10-04 ("model it, generate it"): a file made from records alone, with
// no source text at all, is the writer's own form byte for byte; and the file's layout is data the writer
// generates from, so a change of that data (a key's spelling, a separator, a comment) is in the output.
int minted_parity() {
	int failures = 0;
	DefItemDef item{};
	def_init_item(item);
	std::strcpy(item.display_name, "Minted Truck");
	item.id = 100500;
	item.type = 1;
	item.hp = 250;
	item.attrib = DEF_ITEM_ATTRIB_PLAYERCONTROL | DEF_ITEM_ATTRIB_LANDABLE;
	DefItemsFile file{};
	file.entries = &item;
	file.count = 1;
	const DefWriteResult written = def_write_items(file);
	const std::string expected = "// Item definitions\r\n\r\nbegin \"Minted Truck\"\r\n\tid 100500\r\n\thp 250\r\n"
	                             "\ttype vehicle\r\n\tattrib: playercontrol landable\r\nend\r\n\r\n";
	if (!written.ok() || written.text != expected) {
		std::printf("FAIL a minted item: the writer's own form is not\n%s\nbut\n%s\n", expected.c_str(), written.text.c_str());
		print_failures(written);
		++failures;
	}
	return failures;
}

int layout_is_data() {
	int failures = 0;
	Items read(kItems);
	DefNotedRecord &truck = read.notes.records[0];
	bool found = false;
	for (DefNotedLine &line : truck.lines)
		if (line.role == DefNotedRole::Line && line.shape.entry.rfind("hp", 0) == 0 && !line.shape.words.empty()) {
			line.shape.words[0].spelling = "HP"; // the key as the file spells it
			// (Three blanks, not a tab: our items parser matches `hp ` with its blank, a divergence from the
			// game's tokenizer listed for the formats' next batch.)
			if (!line.gaps.empty()) line.gaps[0] = "   ";
			line.tail = " ; very tough";
			found = true;
		}
	if (!read.notes.leading.empty()) read.notes.leading[0].tail = "// Items, made by hand";
	const DefWriteResult written = read.save();
	if (!found || !written.ok() || written.text.find("  HP   3000 ; very tough\r\n") == std::string::npos ||
	    !written.rewritten.empty() ||
	    written.text.rfind("// Items, made by hand\r\n", 0) != 0) {
		std::printf("FAIL the layout is data the writer generates from:\n%s\n", written.text.c_str());
		for (const std::string &line : written.rewritten) std::printf("  rewritten: %s\n", line.c_str());
		print_failures(written);
		++failures;
	}
	return failures;
}

// The review's Y2: a sight moved within its weapon, or duplicated, follows the weapon's order (an
// effects row of an ammo's table likewise), each keeping the comment above it, and the record keeps its
// own form (no record rewritten); a Duplicate's copy (its notes cleared, as the catalog's prepare_record
// does) in the writer's form where the order has it.
const std::string kSights = "weapon \"WPN_S\"\r\n"
                            "\t// the scope\r\n"
                            "\tsights scope.tga 0 0 640 480 blend\r\n"
                            "\t// the dot\r\n"
                            "\tsights reticle.tga 0 0 64 64 add\r\n"
                            "end\r\n";

int nested_order() {
	int failures = 0;
	{
		Weapons read(kSights);
		DefWeaponDef &weapon = read.file.entries[0];
		std::swap(weapon.sights[0], weapon.sights[1]);
		const DefWriteResult written = read.save();
		const std::string moved = "weapon \"WPN_S\"\r\n\t// the dot\r\n\tsights reticle.tga 0 0 64 64 add\r\n"
		                          "\t// the scope\r\n\tsights scope.tga 0 0 640 480 blend\r\nend\r\n";
		if (!written.ok() || written.text != moved || !written.rewritten.empty()) {
			std::printf("FAIL a sight moved:\n%s\n", written.text.c_str());
			print_failures(written);
			++failures;
		}
	}
	{
		Weapons read(kSights);
		DefWeaponDef &weapon = read.file.entries[0];
		DefSightEntry *const own = weapon.sights;
		const size_t own_count = weapon.sights_count;
		std::vector<DefSightEntry> sights(weapon.sights, weapon.sights + weapon.sights_count);
		DefSightEntry copy = sights[0];
		def_clear_notes(DefRecordKind::Sight, &copy);
		sights.insert(sights.begin() + 1, copy);
		weapon.sights = sights.data();
		weapon.sights_count = sights.size();
		const DefWriteResult written = read.save();
		weapon.sights = own; // the file's, freed with it
		weapon.sights_count = own_count;
		const std::string duplicated = "weapon \"WPN_S\"\r\n\t// the scope\r\n\tsights scope.tga 0 0 640 480 blend\r\n"
		                               "\tsights scope.tga 0 0 640 480 blend\r\n\t// the dot\r\n\tsights reticle.tga 0 0 64 64 add\r\nend\r\n";
		if (!written.ok() || written.text != duplicated || !written.rewritten.empty()) {
			std::printf("FAIL a sight duplicated:\n%s\n", written.text.c_str());
			print_failures(written);
			++failures;
		}
	}
	{
		Ammo read(kAmmo);
		DefAmmoDef &ammo = read.file.entries[0];
		std::swap(ammo.effects_table[0], ammo.effects_table[1]);
		const DefWriteResult written = read.save();
		if (!written.ok() || written.text.find("        dirt    hit dust  2\r\n        metal   hit spark 4\r\n") == std::string::npos ||
		    !written.rewritten.empty()) {
			std::printf("FAIL an effects row moved:\n%s\n", written.text.c_str());
			print_failures(written);
			++failures;
		}
	}
	return failures;
}

// The review's Y3: a record whose own form its edit cannot carry (the surviving `action "fire"` removed,
// so the earlier block the game skipped would read again as the action) is written in the writer's form
// with its comment lines kept, the earlier block left out, and both said (rewritten).
const std::string kReplaced = "weapon \"WPN_R\"\r\n"
                              "\t// fire, first form\r\n"
                              "\taction \"fire\"\r\n"
                              "\t\tdelayend 3\r\n"
                              "\tend\r\n"
                              "\taction \"fire\"\r\n"
                              "\t\tdelayend 4\r\n"
                              "\tend\r\n"
                              "\t// reload\r\n"
                              "\taction \"reload\"\r\n"
                              "\t\tanim reload\r\n"
                              "\tend\r\n"
                              "end\r\n";

int fallback_keeps_and_says() {
	int failures = 0;
	Weapons read(kReplaced);
	DefWeaponDef &weapon = read.file.entries[0];
	if (weapon.actions_count != 2) {
		std::printf("FAIL the replaced block: %zu actions read\n", weapon.actions_count);
		return 1;
	}
	std::swap(weapon.actions[0], weapon.actions[1]);
	weapon.actions_count = 1; // the fire block gone (its memory the file's, freed with it)
	const DefWriteResult written = read.save();
	weapon.actions_count = 2;
	std::swap(weapon.actions[0], weapon.actions[1]);
	const bool said = written.rewritten.size() == 1 && written.rewritten[0].find("fire") != std::string::npos;
	if (!written.ok() || written.text.find("delayend") != std::string::npos ||
	    written.text.find("// fire, first form") == std::string::npos || written.text.find("// reload") == std::string::npos ||
	    !said) {
		std::printf("FAIL a record written in the writer's form keeps its comments and says what it leaves out:\n%s\n",
		            written.text.c_str());
		for (const std::string &line : written.rewritten) std::printf("  rewritten: %s\n", line.c_str());
		print_failures(written);
		++failures;
	}
	return failures;
}

// The review's Y3: an attribute set anew on an item whose `attrib:` line holds as many words as a line
// holds goes on a line of its own (our parser reads no more of one).
int attribute_lines_full() {
	int failures = 0;
	DefItemDef item{};
	def_init_item(item);
	std::strcpy(item.display_name, "Full");
	item.id = 100501;
	item.type = 6;
	// A line holds 29 words (the tokenizer's 30-token cap, the key one of them): every first-word
	// attribute but Door (whose line also sets a door count) and NoShadow (the one added below), and a
	// second-word one.
	const uint32_t bits[] = {DEF_ITEM_ATTRIB_MOVECB, DEF_ITEM_ATTRIB_NOMOVESHOOT, DEF_ITEM_ATTRIB_NOTOOL, DEF_ITEM_ATTRIB_SNAP,
	                         DEF_ITEM_ATTRIB_EWEAP, DEF_ITEM_ATTRIB_PLAYERCONTROL, DEF_ITEM_ATTRIB_NOTARGET,
	                         DEF_ITEM_ATTRIB_LANDABLE, DEF_ITEM_ATTRIB_MISSILE, DEF_ITEM_ATTRIB_TIRE, DEF_ITEM_ATTRIB_FASTROPE,
	                         DEF_ITEM_ATTRIB_TAKEABLE, DEF_ITEM_ATTRIB_EASY, DEF_ITEM_ATTRIB_SD, DEF_ITEM_ATTRIB_4TEAM,
	                         DEF_ITEM_ATTRIB_CHANGETEAM, DEF_ITEM_ATTRIB_SPAWNPOINT, DEF_ITEM_ATTRIB_ARMORY,
	                         DEF_ITEM_ATTRIB_POWERUP, DEF_ITEM_ATTRIB_AIDATA, DEF_ITEM_ATTRIB_LEAVECORPSE,
	                         DEF_ITEM_ATTRIB_NODISMEMBER, DEF_ITEM_ATTRIB_NOWEAPON, DEF_ITEM_ATTRIB_REFLECT,
	                         DEF_ITEM_ATTRIB_CONCAVE, DEF_ITEM_ATTRIB_NOSCAR, DEF_ITEM_ATTRIB_NOHUD, DEF_ITEM_ATTRIB_NODIE};
	size_t words = 0;
	for (; words < def_attrib_words_per_line() && words < std::size(bits); ++words) item.attrib |= bits[words];
	if (words < def_attrib_words_per_line()) item.attrib2 |= DEF_ITEM_ATTRIB2_VEHICLEBAY;
	DefItemsFile file{};
	file.entries = &item;
	file.count = 1;
	const DefWriteResult made = def_write_items(file);
	if (!made.ok()) {
		std::printf("FAIL a full attrib line: the minted item does not write\n");
		print_failures(made);
		return 1;
	}
	Items read(made.text);
	read.file.entries[0].attrib |= DEF_ITEM_ATTRIB_NOSHADOW;
	failures += changed("items: an attribute past a full line", made.text, read.save(), 0, 1);
	return failures;
}

// The writer's own reals (bug 1): the shortest decimal that reads back the same.
int shortest_reals() {
	int failures = 0;
	const std::string weapons = "weapon \"WPN_A\"\r\n\tpos 4 -5 -186 0 0 0\r\n\ttpos 21.76, -11.72, 3.5, 0, 0.5, 359.99\r\n"
	                            "\theat_values 35 20\r\n\terror_hiptheta 0.25\r\nend\r\n";
	DefWeaponsFile guns{};
	def_parse_weapons_memory(bytes_of(weapons), weapons.size(), &guns, nullptr);
	const DefWriteResult gun = def_write_weapons(guns);
	def_free_weapons(&guns);
	for (const char *line : {"\tpos 4 -5 -186 0 0 0\r\n", "\ttpos 21.76 -11.72 3.5 0 0.5 359.99\r\n", "\theat_values 35 20\r\n",
	                         "\terror_hiptheta 0.25\r\n"})
		if (gun.text.find(line) == std::string::npos) {
			std::printf("FAIL shortest reals, weapons: no %s\n%s\n", line, gun.text.c_str());
			++failures;
		}
	const std::string items = "begin \"Thing\"\r\nid 100001\r\nscale 1.5\r\ndestroy_timing 0.5 1 2.5\r\nkz 3.25\r\n"
	                          "shadow s.tga 4.5 8.9 0 -0.14\r\nhusk_swap_at_sec 2\r\nhusk_swap_at 3.5\r\ndawnshot fx 1.5 0.25\r\nend\r\n";
	DefItemsFile things{};
	def_parse_items_memory(bytes_of(items), items.size(), &things, nullptr);
	const DefWriteResult thing = def_write_items(things);
	def_free_items(&things);
	for (const char *line : {"\tscale 1.5\r\n", "\tdestroy_timing 0.5 1 2.5\r\n", "\tkz 3.25\r\n", "\tshadow s.tga 4.5 8.9 0 -0.14\r\n",
	                         "\thusk_swap_at_sec 2\r\n", "\thusk_swap_at 3.5\r\n", "\tdawnshot fx 1.5 0.25\r\n"})
		if (thing.text.find(line) == std::string::npos) {
			std::printf("FAIL shortest reals, items: no %s\n%s\n", line, thing.text.c_str());
			++failures;
		}
	const std::string ammo = "ammo A\r\nmax_age 2.5\r\narm_age 0.1\r\nturnrate_maxyaw 1.5\r\nlight_impact 4 10 20 30 0.3\r\nend\r\n";
	DefAmmoFile rounds{};
	def_parse_ammo_memory(bytes_of(ammo), ammo.size(), &rounds, nullptr);
	const DefWriteResult round = def_write_ammo(rounds);
	def_free_ammo(&rounds);
	for (const char *line : {"\tmax_age 2.5\r\n", "\tarm_age 0.1\r\n", "\tturnrate_maxyaw 1.5\r\n", "\tlight_impact 4 10 20 30 0.3\r\n"})
		if (round.text.find(line) == std::string::npos) {
			std::printf("FAIL shortest reals, ammo: no %s\n%s\n", line, round.text.c_str());
			++failures;
		}
	// A float's edges (the review's Y9): a negative zero keeps its sign, a tiny value is written by its
	// significant digits, a huge one by its digits and zeros, never a 48-place expansion.
	DefWeaponsFile edges{};
	const std::string edge = "weapon \"WPN_E\"\r\n\tpos -0 0.00000000000000000001 1e30 0 0 0\r\nend\r\n";
	def_parse_weapons_memory(bytes_of(edge), edge.size(), &edges, nullptr);
	const float tiny = edges.count ? edges.entries[0].pos[1] : 0.0f;
	const DefWriteResult edged = def_write_weapons(edges);
	def_free_weapons(&edges);
	if (!edged.ok() || edged.text.find("\tpos -0 0.00000000000000000001 1000000000000000000000000000000 0 0 0\r\n") ==
	                           std::string::npos) {
		std::printf("FAIL shortest reals, a float's edges (tiny %g):\n%s\n", double(tiny), edged.text.c_str());
		++failures;
	}
	return failures;
}

// --- retail -----------------------------------------------------------------------------------------------

// The family's writer's own form of a text read without notes ("" where it does not write): two texts of one
// form read as one model.
std::string plain_form(const char *family, const std::string &text) {
	const uint8_t *bytes = bytes_of(text);
	DefWriteResult written;
	if (std::strcmp(family, "items.def") == 0) {
		DefItemsFile file{};
		def_parse_items_memory(bytes, text.size(), &file, nullptr);
		written = def_write_items(file);
		def_free_items(&file);
	} else if (std::strcmp(family, "weapon.def") == 0) {
		DefWeaponsFile file{};
		def_parse_weapons_memory(bytes, text.size(), &file, nullptr);
		written = def_write_weapons(file);
		def_free_weapons(&file);
	} else if (std::strcmp(family, "ammo.def") == 0) {
		DefAmmoFile file{};
		def_parse_ammo_memory(bytes, text.size(), &file, nullptr);
		written = def_write_ammo(file);
		def_free_ammo(&file);
	} else {
		DefPowerupFile file{};
		def_parse_powerup_memory(bytes, text.size(), &file, nullptr);
		written = def_write_powerup(file);
		def_free_powerup(&file);
	}
	return written.ok() ? written.text : std::string();
}

// A word that is a plain decimal with a point (a real as a line writes it).
bool is_real(const std::string &word) {
	return word.find('.') != std::string::npos && word.find_first_not_of("-0123456789.") == std::string::npos &&
	       word.find_first_of("0123456789") != std::string::npos;
}

// The whole number `k` with `places` digits after the point (the writer's with_places, restated).
std::string with_places(long long k, int places) {
	if (k == 0) return "0";
	const bool negative = k < 0;
	std::string digits = std::to_string(negative ? -k : k);
	if (places > 0) {
		if (digits.size() <= size_t(places)) digits.insert(0, size_t(places) + 1 - digits.size(), '0');
		digits.insert(digits.size() - size_t(places), 1, '.');
		while (digits.back() == '0') digits.pop_back();
		if (digits.back() == '.') digits.pop_back();
	}
	return negative ? "-" + digits : digits;
}

// The decimals of one place fewer than `word` has, the nearest and its two neighbours: what the writer's
// shortest search tries before `word`'s count of places.
std::vector<std::string> one_place_fewer(const std::string &word) {
	const int places = int(word.size() - word.find('.') - 1);
	const double scaled = std::strtod(word.c_str(), nullptr) * std::pow(10.0, places - 1);
	const long long k = std::llround(scaled);
	std::vector<std::string> out;
	for (const long long step : {0LL, -1LL, 1LL}) {
		const std::string candidate = with_places(k + step, places - 1);
		if (candidate != word) out.push_back(candidate);
	}
	return out;
}

// The lines of a text, each with its ending.
std::vector<std::string> lines_of(const std::string &text) {
	std::vector<std::string> out;
	size_t at = 0;
	while (at < text.size()) {
		const size_t end = text.find('\n', at);
		const size_t next = end == std::string::npos ? text.size() : end + 1;
		out.push_back(text.substr(at, next - at));
		at = next;
	}
	return out;
}

// The reals of line `index` of `text` a decimal of one place fewer reads the same as (the whole text read
// again with that word changed: its model as before), each "word -> shorter"; with `was` (the line as the
// file had it), only the words it changed (a word the file spells its own way stands as the file has it).
std::vector<std::string> longer_than_read(const char *family, const std::string &text, size_t index,
                                          const std::string *reference = nullptr, const std::string *was = nullptr) {
	std::vector<std::string> out;
	std::vector<std::string> lines = lines_of(text);
	if (index >= lines.size()) return out;
	const std::string form = reference ? *reference : plain_form(family, text);
	if (form.empty()) return out;
	DefNotedLine line = def_noted_line(lines[index].data(), lines[index].size());
	const std::vector<std::string> before = was ? def_noted_line(was->data(), was->size()).words : std::vector<std::string>();
	for (size_t w = 1; w < line.words.size(); ++w) {
		if (!is_real(line.words[w])) continue;
		if (w < before.size() && before[w] == line.words[w]) continue;
		const std::string word = line.words[w];
		for (const std::string &shorter : one_place_fewer(word)) {
			line.words[w] = shorter;
			std::vector<std::string> changed = lines;
			changed[index] = line.text();
			std::string again;
			for (const std::string &l : changed) again += l;
			if (plain_form(family, again) == form) {
				out.push_back(word + " -> " + shorter);
				break;
			}
		}
		line.words[w] = word;
	}
	return out;
}

// The reals of a writer's own text a decimal of one place fewer reads the same as, each record read alone
// (a block from its header at the margin to its `end` there), at most `cap` reported.
std::vector<std::string> longer_than_needed(const char *family, const std::string &text, size_t cap = 5) {
	std::vector<std::string> out;
	const std::vector<std::string> lines = lines_of(text);
	for (size_t i = 0; i < lines.size() && out.size() < cap; ++i) {
		const DefNotedLine head = def_noted_line(lines[i].data(), lines[i].size());
		if (!head.indent.empty() || head.words.empty()) continue;
		const std::string key = head.words[0];
		if (key != "begin" && key != "weapon" && key != "ammo" && key != "powerup") continue;
		size_t end = i + 1;
		while (end < lines.size()) {
			const DefNotedLine at = def_noted_line(lines[end].data(), lines[end].size());
			if (at.indent.empty() && at.words.size() == 1 && at.words[0] == "end") break;
			++end;
		}
		if (end >= lines.size()) break;
		std::string block;
		for (size_t j = i; j <= end; ++j) block += lines[j];
		const std::string form = plain_form(family, block);
		for (size_t j = 1; j < end - i && out.size() < cap; ++j)
			for (const std::string &found : longer_than_read(family, block, j, &form))
				out.push_back(head.words.size() > 1 ? head.words[1] + ": " + found : found);
		i = end;
	}
	return out;
}

// Each number of a text with more than six digits after its point (a long tail).
std::vector<std::string> long_reals(const std::string &text) {
	std::vector<std::string> out;
	for (const std::string &line : split_lines(text)) {
		const DefNotedLine words = def_noted_line(line.data(), line.size());
		for (const std::string &word : words.words) {
			const size_t point = word.find('.');
			if (point == std::string::npos || word.find_first_not_of("-0123456789.") != std::string::npos) continue;
			if (word.size() - point - 1 > 6) out.push_back(word);
		}
	}
	return out;
}

// The line of `b` the one changed line of `a` became (its index in `b`; npos where the change is not one
// line for one).
size_t changed_line(const std::string &a, const std::string &b) {
	const std::vector<std::string> x = lines_of(a), y = lines_of(b);
	if (x.size() != y.size()) return std::string::npos;
	size_t found = std::string::npos;
	for (size_t i = 0; i < x.size(); ++i)
		if (x[i] != y[i]) {
			if (found != std::string::npos) return std::string::npos;
			found = i;
		}
	return found;
}

// An edit of a retail catalog read with its notes: what changed, the save, and how many lines it changed
// (`gone` removed, `put` added) against the file.
struct RetailEdit {
	std::string field;
	DefWriteResult saved;
	size_t gone = 0, put = 0;
};

int retail_catalog(const std::string &label, const char *family, const std::vector<uint8_t> &bytes) {
	int failures = 0;
	const std::string original(bytes.begin(), bytes.end());
	const std::string what = label + family;
	DefWriteResult first;
	// Each edit on a read of its own: a whole number, a real (the writer's decimal of it: bug 1 over a
	// retail line), and an item's attribute (its `attrib:` line, the words the game skips kept).
	RetailEdit whole, real, attribute;
	const auto edit = [&](auto &read, RetailEdit &out, auto change) {
		out.field = change(read.file);
		out.saved = read.save();
	};
	if (std::strcmp(family, "items.def") == 0) {
		{
			Items read(original);
			first = read.save();
		}
		Items a(original), b(original), c(original);
		edit(a, whole, [](DefItemsFile &file) -> std::string {
			for (size_t i = 0; i < file.count; ++i)
				if (file.entries[i].hp > 0) {
					file.entries[i].hp += 1;
					return std::string(file.entries[i].display_name) + " hp";
				}
			return std::string();
		});
		edit(b, real, [](DefItemsFile &file) -> std::string {
			for (size_t i = 0; i < file.count; ++i)
				if (file.entries[i].shadow_width > 0.0f) {
					file.entries[i].shadow_width += 0.3f;
					return std::string(file.entries[i].display_name) + " shadow width";
				}
			return std::string();
		});
		edit(c, attribute, [](DefItemsFile &file) -> std::string {
			const uint32_t own = ~(DEF_ITEM_ATTRIB_DOOR | DEF_ITEM_ATTRIB_POWERUP);
			for (size_t i = 0; i < file.count; ++i) {
				const uint32_t bits = uint32_t(file.entries[i].attrib) & own;
				if ((bits & DEF_ITEM_ATTRIB_LANDABLE) && (bits & ~DEF_ITEM_ATTRIB_LANDABLE)) {
					file.entries[i].attrib &= ~DEF_ITEM_ATTRIB_LANDABLE;
					return std::string(file.entries[i].display_name) + " Landable";
				}
			}
			return std::string();
		});
	} else if (std::strcmp(family, "weapon.def") == 0) {
		{
			Weapons read(original);
			first = read.save();
		}
		Weapons a(original), b(original);
		edit(a, whole, [](DefWeaponsFile &file) -> std::string {
			for (size_t i = 0; i < file.count; ++i)
				if (file.entries[i].clipsize > 0) {
					file.entries[i].clipsize += 1;
					return std::string(file.entries[i].weapon_name) + " clipsize";
				}
			return std::string();
		});
		edit(b, real, [](DefWeaponsFile &file) -> std::string {
			for (size_t i = 0; i < file.count; ++i)
				if (file.entries[i].pos[0] != 0.0f) {
					file.entries[i].pos[0] += 0.3f;
					return std::string(file.entries[i].weapon_name) + " pos x";
				}
			return std::string();
		});
	} else if (std::strcmp(family, "ammo.def") == 0) {
		{
			Ammo read(original);
			first = read.save();
		}
		Ammo a(original), b(original);
		edit(a, whole, [](DefAmmoFile &file) -> std::string {
			for (size_t i = 0; i < file.count; ++i)
				if (file.entries[i].velocity > 0) {
					file.entries[i].velocity += 1;
					return std::string(file.entries[i].name) + " velocity";
				}
			return std::string();
		});
		edit(b, real, [](DefAmmoFile &file) -> std::string {
			for (size_t i = 0; i < file.count; ++i)
				if (file.entries[i].max_age_ticks > 0) {
					file.entries[i].max_age_ticks += 1;
					return std::string(file.entries[i].name) + " max_age";
				}
			return std::string();
		});
	} else {
		{
			Powerups read(original);
			first = read.save();
		}
		Powerups a(original);
		edit(a, whole, [](DefPowerupFile &file) -> std::string {
			if (!file.count) return std::string();
			file.entries[0].respawn_time += 1;
			return std::string(file.entries[0].name) + " respawn_time";
		});
	}
	failures += same_as_read(what.c_str(), original, first);
	std::printf("%s: %zu lines; first save %s\n", what.c_str(), split_lines(original).size(),
	            first.ok() && first.text == original ? "is the original" : "DIFFERS");
	for (RetailEdit *e : {&whole, &real, &attribute}) {
		if (e == &attribute && std::strcmp(family, "items.def") != 0) continue;
		if (e == &real && std::strcmp(family, "powerup.def") == 0) continue;
		const auto [gone, put] = e->saved.ok() ? lines_changed(original, e->saved.text) : std::pair<size_t, size_t>{0, 0};
		std::printf("  %s changed: -%zu +%zu lines\n", e->field.c_str(), gone, put);
		// One field changed, one line changed (an attribute's removed word may take its line with it).
		const bool one = gone == 1 && (put == 1 || (e == &attribute && put == 0));
		if (e->field.empty() || !e->saved.ok() || !one) {
			std::printf("FAIL %s: %s changed is not one line changed\n", what.c_str(), e->field.c_str());
			print_failures(e->saved);
			++failures;
			continue;
		}
		// The real's new line: its reals the shortest the reader takes back to their words.
		if (e == &real) {
			const size_t at = changed_line(original, e->saved.text);
			const std::string was = at == std::string::npos ? std::string() : lines_of(original)[at];
			const std::vector<std::string> longer = at == std::string::npos ? std::vector<std::string>{"<no line>"}
			                                                                 : longer_than_read(family, e->saved.text, at, nullptr, &was);
			if (!longer.empty()) {
				std::printf("FAIL %s: %s is written longer than it reads: %s\n", what.c_str(), e->field.c_str(),
				            longer.front().c_str());
				++failures;
			}
		}
	}
	// The writer's own form: no long tail (a number with more than six digits after its point is one no
	// field of these catalogs needs), and no real a decimal of one place fewer reads the same as, each record
	// read alone.
	const std::string plain = plain_form(family, original);
	const std::vector<std::string> tails = !plain.empty() ? long_reals(plain) : std::vector<std::string>{"<refused>"};
	if (!tails.empty()) {
		std::printf("FAIL %s: the writer's own form puts down %zu real(s) with a long tail, the first %s\n", what.c_str(),
		            tails.size(), tails.front().c_str());
		++failures;
	}
	const auto started = std::chrono::steady_clock::now();
	const std::vector<std::string> longer = plain.empty() ? std::vector<std::string>() : longer_than_needed(family, plain);
	std::printf("  the writer's own reals each the shortest its reader takes back (%.1f s)\n",
	            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
	for (const std::string &found : longer) {
		std::printf("FAIL %s: the writer's own form writes a real longer than it reads: %s\n", what.c_str(), found.c_str());
		++failures;
	}
	return failures;
}

int retail_catalogs() {
	const std::string root = retail::install();
	if (root.empty()) return retail::skip_leg("the def notes' retail catalogs need OPENNOVA_JO_DIR");
	int failures = 0;
	opennova::Vfs vfs;
	vfs.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
	if (!vfs.mount_game(root, "", opennova::VfsMountMode::Packed)) return 1;
	const auto catalogs = [&](opennova::Vfs &mounted, const std::string &label, bool powerup) {
		std::vector<const char *> families = {"items.def", "weapon.def", "ammo.def"};
		if (powerup) families.push_back("powerup.def");
		for (const char *family : families) {
			std::vector<uint8_t> bytes;
			if (!mounted.read_file(family, bytes)) {
				std::printf("FAIL cannot read %s%s\n", label.c_str(), family);
				++failures;
				continue;
			}
			failures += retail_catalog(label, family, bytes);
		}
	};
	catalogs(vfs, "", true);
	for (const std::string &expansion : retail::expansions()) {
		opennova::Vfs served;
		served.set_scr_policy(opennova::VFS_SCR_FORCE_JO_DFX2);
		if (!served.mount_game(root, expansion, opennova::VfsMountMode::Packed)) return 1;
		catalogs(served, "/exp " + expansion + ": ", false);
	}
	return failures;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failures = 0;
	failures += shortest_reals();
	failures += minted_parity();
	failures += layout_is_data();
	failures += nested_order();
	failures += fallback_keeps_and_says();
	failures += attribute_lines_full();
	failures += noted_items();
	failures += noted_weapons();
	failures += noted_ammo();
	failures += noted_powerups();
	failures += retail_catalogs();
	if (failures) std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
