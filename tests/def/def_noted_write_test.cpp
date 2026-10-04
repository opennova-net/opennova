// The def writers over a file's notes (def_notes.h; the demo round's bugs 1 and 3): a file read with its
// notes and saved unchanged is the file as it was, byte for byte (its comments, its spacing, a number's
// own spelling, the words and lines the game skips, its line endings, a last line with none); one field
// changed changes that one line, keeping its blanks, its comment and its unchanged words; a flag, a row or
// a block added or removed adds or removes its own lines alone; a copy is written in the writer's form
// after the rest; and every real the writer puts down of its own is the shortest decimal that reads back
// to the same value (`pos 4 -5 -186 0 0 0`, never `0.00000762939453125`; `21.76`, never
// `21.7600002288818359375`). With a JO install configured: the first save of every retail catalog (items,
// weapon with its action blocks, ammo, the base's and each installed expansion's, and powerup) is the
// original, one field changed changes one line of it, and the writer's own form of every catalog puts
// down no real with a long tail a shorter form reads back the same as.
#include <formats/def/def_notes.h>
#include <formats/def/def_scan.h>
#include <formats/def/def_write.h>
#include <base/vfs/vfs.h>
#include <base/vfs/vfs_decode.h>
#include "common/retail_paths.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
		if (l != r) return "line " + std::to_string(i + 1) + ":\n  was: " + l + "\n  now: " + r;
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
        "end";

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
		// (the original's last line, which had no ending, gains one before the copy, written in the file's
		// indentation)
		if (!written.ok() || gone != 1 || put < 5 || written.text.find("begin \"Car (copy)\"\r\n  id 100102\r\n") == std::string::npos) {
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
        "\n"
        "ammoclass_max_carry CLASS_9mm\t\t\t120 \n"
        "ammoclass_max_carry CLASS_45cal\t\t90\n"
        "\n"
        "weapon \"WPN_A\"\n"
        "\tcategory 2\n"
        "\trank     0\n"
        "\n"
        "\tflags Underwater\n"
        "\tclipsize 15 // rounds\n"
        "\tflags Scoped\n"
        "\tpos 21.76, -11.72, 3.5, 0.0, 0, 0\n"
        "\tsights scope.tga 0 0 640 480 blend\n"
        "\tsights reticle.tga 0 0 64 64 add\n"
        "\taction \"fire\"\n"
        "\t\tfunction wpn_std_fire\n"
        "\t\tdelayend   4\n"
        "\tend\n"
        "\n"
        "\taction \"reload\"\n"
        "\t\tanim reload\n"
        "\tend\n"
        "end\n";

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

// The writer's own reals (bug 1): the shortest decimal that reads back the same.
int shortest_reals() {
	int failures = 0;
	const std::string weapons = "weapon \"WPN_A\"\n\tpos 4 -5 -186 0 0 0\n\ttpos 21.76, -11.72, 3.5, 0, 0.5, 359.99\n"
	                            "\theat_values 35 20\n\terror_hiptheta 0.25\nend\n";
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
	const std::string items = "begin \"Thing\"\nid 100001\nscale 1.5\ndestroy_timing 0.5 1 2.5\nkz 3.25\n"
	                          "shadow s.tga 4.5 8.9 0 -0.14\nhusk_swap_at_sec 2\nhusk_swap_at 3.5\ndawnshot fx 1.5 0.25\nend\n";
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
	const std::string ammo = "ammo A\nmax_age 2.5\narm_age 0.1\nturnrate_maxyaw 1.5\nlight_impact 4 10 20 30 0.3\nend\n";
	DefAmmoFile rounds{};
	def_parse_ammo_memory(bytes_of(ammo), ammo.size(), &rounds, nullptr);
	const DefWriteResult round = def_write_ammo(rounds);
	def_free_ammo(&rounds);
	for (const char *line : {"\tmax_age 2.5\r\n", "\tarm_age 0.1\r\n", "\tturnrate_maxyaw 1.5\r\n", "\tlight_impact 4 10 20 30 0.3\r\n"})
		if (round.text.find(line) == std::string::npos) {
			std::printf("FAIL shortest reals, ammo: no %s\n%s\n", line, round.text.c_str());
			++failures;
		}
	return failures;
}

// --- retail -----------------------------------------------------------------------------------------------

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

int retail_catalog(const std::string &label, const char *family, const std::vector<uint8_t> &bytes) {
	int failures = 0;
	const std::string original(bytes.begin(), bytes.end());
	const std::string what = label + family;
	DefWriteResult first, edited, plain;
	std::string field;
	if (std::strcmp(family, "items.def") == 0) {
		Items read(original);
		first = read.save();
		for (size_t i = 0; i < read.file.count && field.empty(); ++i)
			if (read.file.entries[i].hp > 0) {
				read.file.entries[i].hp += 1;
				field = std::string(read.file.entries[i].display_name) + " hp";
			}
		edited = read.save();
		DefItemsFile bare{};
		def_parse_items_memory(bytes.data(), bytes.size(), &bare, nullptr);
		plain = def_write_items(bare);
		def_free_items(&bare);
	} else if (std::strcmp(family, "weapon.def") == 0) {
		Weapons read(original);
		first = read.save();
		for (size_t i = 0; i < read.file.count && field.empty(); ++i)
			if (read.file.entries[i].clipsize > 0) {
				read.file.entries[i].clipsize += 1;
				field = std::string(read.file.entries[i].weapon_name) + " clipsize";
			}
		edited = read.save();
		DefWeaponsFile bare{};
		def_parse_weapons_memory(bytes.data(), bytes.size(), &bare, nullptr);
		plain = def_write_weapons(bare);
		def_free_weapons(&bare);
	} else if (std::strcmp(family, "ammo.def") == 0) {
		Ammo read(original);
		first = read.save();
		for (size_t i = 0; i < read.file.count && field.empty(); ++i)
			if (read.file.entries[i].velocity > 0) {
				read.file.entries[i].velocity += 1;
				field = std::string(read.file.entries[i].name) + " velocity";
			}
		edited = read.save();
		DefAmmoFile bare{};
		def_parse_ammo_memory(bytes.data(), bytes.size(), &bare, nullptr);
		plain = def_write_ammo(bare);
		def_free_ammo(&bare);
	} else {
		Powerups read(original);
		first = read.save();
		for (size_t i = 0; i < read.file.count && field.empty(); ++i) {
			read.file.entries[i].respawn_time += 1;
			field = std::string(read.file.entries[i].name) + " respawn_time";
		}
		edited = read.save();
		DefPowerupFile bare{};
		def_parse_powerup_memory(bytes.data(), bytes.size(), &bare, nullptr);
		plain = def_write_powerup(bare);
		def_free_powerup(&bare);
	}
	failures += same_as_read(what.c_str(), original, first);
	const auto [gone, put] = edited.ok() ? lines_changed(original, edited.text) : std::pair<size_t, size_t>{0, 0};
	std::printf("%s: %zu lines; first save %s; %s changed: -%zu +%zu lines\n", what.c_str(), split_lines(original).size(),
	            first.ok() && first.text == original ? "is the original" : "DIFFERS", field.c_str(), gone, put);
	if (field.empty() || !edited.ok() || gone != 1 || put != 1) {
		std::printf("FAIL %s: one field changed is not one line changed\n", what.c_str());
		print_failures(edited);
		++failures;
	}
	// The writer's own form: no long tail a shorter form reads back the same as (each counted; a number
	// with more than six digits after its point is one no field of these catalogs needs).
	const std::vector<std::string> tails = plain.ok() ? long_reals(plain.text) : std::vector<std::string>{"<refused>"};
	if (!tails.empty()) {
		std::printf("FAIL %s: the writer's own form puts down %zu real(s) with a long tail, the first %s\n", what.c_str(),
		            tails.size(), tails.front().c_str());
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
	failures += noted_items();
	failures += noted_weapons();
	failures += noted_ammo();
	failures += noted_powerups();
	failures += retail_catalogs();
	if (failures) std::printf("%d failure(s)\n", failures);
	return failures ? 1 : 0;
}
