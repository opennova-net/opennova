// What the game reads of a .def line the typed model holds as the game reads it (formats/def/def_scan.cpp's
// validate_property; def_diagnostic.h's Reinterpreted, Unclosed and UnknownProperty): reported, never a blocker,
// the record holding the game's reading and the writer writing it. Each row, minted inline: the line's finding
// (its code, and its words: the value the arm's own reading takes it as), the member as the parser stores it,
// and the line the writer puts down. A number token the arm's reading takes only the start of (the CRT's atol
// [orig: _atol @0x76AB0A], its atof [orig: _atof @0x76B6A1], the engine's digit walker [orig:
// Math_ParseFixedPoint16 @0x6131F0]); a value left out of the first two value slots, which the tokenizer resets
// to "" for every line [orig: Config_TokenizeLine @0x53CB71..0x53CB81]; a name a table lacks, or a token past
// the ones an arm reads, which the arm reads for nothing; an item block no `end` closes whose alias the file
// states. And what still blocks: a value the game reads from a slot an earlier line left, or past a table's
// end.
#include <formats/def/def_write.h>

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace opennova::def;

namespace {

int failures = 0;

struct Read {
	DefParseReport report;
	DefWriteResult written;
	size_t count = 0;
	bool member = false; // the row's check of the member as stored
};

// The line's family parsed (one record or table row) and written again; `member` checks the first record.
Read read(const char *family, const std::string &text, const std::function<bool(const void *)> &member) {
	Read out;
	const auto *bytes = reinterpret_cast<const uint8_t *>(text.data());
	if (std::strcmp(family, "items.def") == 0) {
		DefItemsFile file{};
		def_parse_items_memory(bytes, text.size(), &file, &out.report);
		out.count = file.count;
		out.member = file.count && member(&file.entries[0]);
		out.written = def_write_items(file);
		def_free_items(&file);
	} else if (std::strcmp(family, "weapon.def") == 0) {
		DefWeaponsFile file{};
		def_parse_weapons_memory(bytes, text.size(), &file, &out.report);
		out.count = file.count + file.ammo_classes_count;
		out.member = member(&file);
		out.written = def_write_weapons(file);
		def_free_weapons(&file);
	} else {
		DefAmmoFile file{};
		def_parse_ammo_memory(bytes, text.size(), &file, &out.report);
		out.count = file.count;
		out.member = file.count && member(&file.entries[0]);
		out.written = def_write_ammo(file);
		def_free_ammo(&file);
	}
	return out;
}

const DefWeaponDef &weapon_of(const void *file) { return static_cast<const DefWeaponsFile *>(file)->entries[0]; }

// One row: nothing blocks, the one finding is `code` saying `words`, the member reads as the game does, and the
// written text holds `line` (each "\t<line>\r\n"; "" writes nothing of the key `absent`).
void reads(const char *what, const char *family, const std::string &text, DefIssueCode code, const char *words,
           const std::function<bool(const void *)> &member, const char *line, const char *absent = nullptr) {
	const Read out = read(family, text, member);
	size_t found = 0;
	for (const DefIssue &issue : out.report)
		found += issue.code == code && issue.message.find(words) != std::string::npos;
	const bool written = !*line || out.written.text.find("\t" + std::string(line) + "\r\n") != std::string::npos;
	const bool left_out = !absent || out.written.text.find(absent) == std::string::npos;
	if (def_report_blocks(out.report) || found != 1 || !out.member || !out.written.ok() || !written || !left_out) {
		std::printf("FAIL %s: blocks %d, findings %zu (%zu of the code and words), member %d, written %s\n%s\n", what,
		            int(def_report_blocks(out.report)), out.report.size(), found, int(out.member),
		            out.written.ok() ? "ok" : "refused", out.written.text.c_str());
		for (const DefIssue &issue : out.report)
			std::printf("  line %zu %s.%s (%d): %s\n", issue.line, issue.record.c_str(), issue.field.c_str(),
			            int(issue.code), issue.message.c_str());
		++failures;
	}
}

// A line the record cannot hold as the game reads it: blocking.
void blocks(const char *what, const char *family, const std::string &text) {
	const Read out = read(family, text, [](const void *) { return true; });
	if (!def_report_blocks(out.report)) {
		std::printf("FAIL %s still blocks\n", what);
		++failures;
	}
}

std::string item(const std::string &line) { return "begin \"Thing\"\r\nid 100001\r\n" + line + "\r\nend\r\n"; }
std::string weapon(const std::string &line) { return "weapon \"WPN_THING\"\r\n" + line + "\r\nend\r\n"; }
std::string ammo(const std::string &line) { return "ammo Round\r\n" + line + "\r\nend\r\n"; }

} // namespace

int main() {
	const auto reinterpreted = DefIssueCode::Reinterpreted;
	const auto ignored = DefIssueCode::UnknownProperty;

	// A token atol reads the start of: abs(atol) [orig: WeaponDefs_ParseLineCallback @0x54402B..0x544045].
	reads("ammobucket 9mm", "weapon.def", weapon("ammobucket 9mm"), reinterpreted, "reads this as 9.",
	      [](const void *f) { return weapon_of(f).ammobucket == 9; }, "ammobucket 9");
	reads("ammobucket 9mmSD", "weapon.def", weapon("ammobucket 9mmSD"), reinterpreted, "reads this as 9.",
	      [](const void *f) { return weapon_of(f).ammobucket == 9; }, "ammobucket 9");
	// A negative one is its size, what the record holds and the writer writes, said as such [orig: the cdq/xor/sub
	// @0x544037].
	reads("ammobucket -9mm", "weapon.def", weapon("ammobucket -9mm"), reinterpreted, "reads this as 9.",
	      [](const void *f) { return weapon_of(f).ammobucket == 9; }, "ammobucket 9");
	reads("ammobucket -4", "weapon.def", weapon("ammobucket -4"), reinterpreted, "reads this as 4.",
	      [](const void *f) { return weapon_of(f).ammobucket == 4; }, "ammobucket 4");
	// An item's number past its whole part, atol's [orig: ItemDef_ParseProperty, hp @0x49EB00].
	reads("hp 1.5", "items.def", item("hp 1.5"), reinterpreted, "reads this as 1.",
	      [](const void *r) { return static_cast<const DefItemDef *>(r)->hp == 1; }, "hp 1");
	// A one-value number key with no value: the reset slot's "", atol's 0 [orig: ItemDef_ParsePhysicsProperty,
	// minai @0x49D95E].
	reads("minai with no value", "items.def", item("minai"), reinterpreted, "reads this as 0.",
	      [](const void *r) { return static_cast<const DefItemDef *>(r)->min_ai == 0; }, "", "minai");
	reads("sqb_rate with no value", "items.def", item("sqb_rate"), reinterpreted, "reads this as 0.",
	      [](const void *r) { return static_cast<const DefItemDef *>(r)->deathtime_ticks == INT32_MIN; }, "sqb_rate 0");
	// atof's start: the item's scale, 16.16 of it [orig: ItemDef_ParseProperty @0x49F6E0..0x49F73D].
	reads("scale 2x", "items.def", item("scale 2x"), reinterpreted, "reads this as 2.",
	      [](const void *r) { return static_cast<const DefItemDef *>(r)->scale_q16 == 0x20000; }, "scale 2");
	// Every spawn-list token by atol [orig: @0x4A0255..0x4A02DE].
	reads("pcvehicle_spawnlist 5x", "items.def", item("pcvehicle_spawnlist 5x"), reinterpreted, "reads this as 5.",
	      [](const void *r) { return static_cast<const DefItemDef *>(r)->vehicle_spawn_mask == 1; },
	      "pcvehicle_spawnlist 5");
	// The digit walker's start, and its unsigned reading: no sign, so a minus reads as 0.
	reads("drag 1.5x", "ammo.def", ammo("drag 1.5x"), reinterpreted, "reads this as 1.5.",
	      [](const void *r) { return static_cast<const DefAmmoDef *>(r)->drag_fp16 == 0x18000; }, "drag 1.5");
	reads("pos -90", "weapon.def", weapon("pos 1 2 3 -90 0 0"), reinterpreted, "reads this as 0.",
	      [](const void *f) { return weapon_of(f).pos_rotation_deg_q16[0] == 0; }, "pos 1 2 3 0 0 0");
	// heat_det_range keeps the walker's integer part [orig: AmmoDef_ParseProperty @0x40a2d0].
	reads("heat_det_range 12.5", "ammo.def", ammo("heat_det_range 12.5"), reinterpreted, "reads this as 12.",
	      [](const void *r) { return static_cast<const DefAmmoDef *>(r)->heat_det_range == 12; }, "heat_det_range 12");
	// A tracer name the table lacks takes AmmoDef_ParseTypeName's atol tail [orig: @0x40A7A6].
	reads("tracer_type unknown", "ammo.def", ammo("tracer_type unknown"), reinterpreted, "reads this as 0.",
	      [](const void *r) {
		      const auto *a = static_cast<const DefAmmoDef *>(r);
		      return a->tracer_type_friendly == 0 && a->tracer_type_enemy == 0;
	      },
	      "", "tracer_type");
	// An effects row's value by atol [orig: AmmoDef_ParseProperty @0x40A583..0x40A587].
	reads("effects row value nope", "ammo.def", "ammo Round\r\neffects_table\r\nmetal hit snd nope\r\nend\r\nend\r\n",
	      reinterpreted, "reads this as 0.",
	      [](const void *r) {
		      const auto *a = static_cast<const DefAmmoDef *>(r);
		      return a->effects_table_count == 1 && a->effects_table[0].value == 0;
	      },
	      "\tmetal hit snd 0");
	// A carry limit's count: abs(atol) of the next slot, a reset one where the line stops at the class [orig:
	// WeaponDefs_ParseLineCallback @0x543862..0x543873].
	const auto carry_zero = [](const void *f) {
		const auto *file = static_cast<const DefWeaponsFile *>(f);
		return file->ammo_classes_count == 1 && file->ammo_classes[0].max_carry == 0;
	};
	reads("ammoclass_max_carry bullets nope", "weapon.def", "ammoclass_max_carry bullets nope\r\n", reinterpreted,
	      "reads this as 0.", carry_zero, "");
	reads("ammoclass_max_carry with no count", "weapon.def", "ammoclass_max_carry bullets\r\n", reinterpreted,
	      "reads this as 0.", carry_zero, "");
	// A class's rounds with no count, the reset slot's 0 [orig: atol(tokens[3]) @0x543B03..0x543B07].
	reads("classrounds medic", "weapon.def", weapon("classrounds medic"), reinterpreted, "reads this as 0.",
	      [](const void *f) { return weapon_of(f).classrounds[1] == 0; }, "", "classrounds");
	reads("classrounds medic 5 extra", "weapon.def", weapon("classrounds medic 5 extra"), ignored, "'extra'",
	      [](const void *f) { return weapon_of(f).classrounds[1] == 5; }, "classrounds medic 5");
	// A sight's slide frames by atol [orig: atol(tokens[9]) @0x544BC3].
	reads("sights slide 4x", "weapon.def", weapon("sights a 0 0 1 1 blend slide 4x"), reinterpreted,
	      "reads this as 4.",
	      [](const void *f) { return weapon_of(f).sights_count == 1 && weapon_of(f).sights[0].slide_frames == 4; },
	      "sights a 0 0 1 1 blend slide 4");
	// An attachment's user point alone: the item id the reset slot reads [orig: ItemDef_ParseProperty
	// @0x4A1B8E..0x4A1BAD]; an angle by atol [orig: @0x4A1BBD..0x4A1C49].
	reads("addeweap with no id", "items.def", item("addeweap ewep01"), reinterpreted, "reads this as 0.",
	      [](const void *r) {
		      const auto *i = static_cast<const DefItemDef *>(r);
		      return i->emplacement_attachments_count == 1 && i->emplacement_attachments[0].item_id == 0 &&
		             std::strcmp(i->emplacement_attachments[0].userpoint, "ewep01") == 0;
	      },
	      "addeweap ewep01 0");
	reads("addeweap angle 15x", "items.def", item("addeweap ewep01 100184 15x 37 0 0"), reinterpreted,
	      "reads this as 15.",
	      [](const void *r) {
		      const auto *i = static_cast<const DefItemDef *>(r);
		      return i->emplacement_attachments_count == 1 &&
		             i->emplacement_attachments[0].down_angle == 15 * 11930464;
	      },
	      "addeweap ewep01 100184 15 37 0 0");

	// A name a table lacks: the arm stores nothing. An ammo's kill zone keeps what an earlier line set ("bad kill
	// zone type" [orig: AmmoDef_ParseProperty @0x40ACB0..0x40ACD2]).
	reads("kztype rounds_kz_bullet", "ammo.def", ammo("kztype rounds_kz_bullet"), ignored, "'rounds_kz_bullet'",
	      [](const void *r) { return static_cast<const DefAmmoDef *>(r)->kztype == 0; }, "", "kztype");
	reads("kztype after another", "ammo.def",
	      "ammo Round\r\nkztype rounds_kz_C4\r\nkztype rounds_kz_bullet\r\nend\r\n", ignored, "'rounds_kz_bullet'",
	      [](const void *r) { return static_cast<const DefAmmoDef *>(r)->kztype == 5; }, "kztype rounds_kz_C4");
	reads("flag nosuch", "ammo.def", ammo("flag nosuch"), ignored, "'nosuch'",
	      [](const void *r) { return static_cast<const DefAmmoDef *>(r)->flags == 0; }, "", "flag");
	reads("type nosuch", "items.def", item("type nosuch"), ignored, "'nosuch'",
	      [](const void *r) { return static_cast<const DefItemDef *>(r)->type == DEF_ITEM_TYPE_UNSET; }, "", "type");
	// A token past the ones an arm reads: a weapon's second flag [orig: WeaponDefs_ParseLineCallback
	// @0x543DC6, tokens[2] alone].
	reads("flags Auto Burst", "weapon.def", weapon("flags Auto Burst"), ignored, "'Burst'",
	      [](const void *f) { return weapon_of(f).flags == DEF_WEAPON_FLAG_AUTO; }, "flags Auto");
	// A view pose of fewer than six values is read for nothing ("too few params" [orig:
	// WeaponDefs_ParseLineCallback @0x5445EE..0x544604]).
	reads("pos 1 2 3", "weapon.def", weapon("pos 1 2 3"), ignored, "ignores this line",
	      [](const void *f) { return weapon_of(f).pos[0] == 0.0f; }, "", "pos");

	// An item block no `end` closes, whose alias the file states: the file's last line `end` with no CR LF loses
	// its last byte to the walk [orig: File_ParseASCIIFile @0x53D8E9..0x53D8F5], and the item the file's end
	// leaves open is the one an `end` closes [orig: ItemDef_ParseProperty @0x49EB2F..0x49EB5F].
	reads("an item the file's end closes", "items.def", "begin \"Heli\"\r\nid 101243\r\nsid Dblkhwk1\r\nend",
	      DefIssueCode::Unclosed, "No end closes the block",
	      [](const void *r) { return std::strcmp(static_cast<const DefItemDef *>(r)->sid, "Dblkhwk1") == 0; },
	      "sid Dblkhwk1");
	reads("an item the next begin closes", "items.def",
	      "begin \"One\"\r\nid 100001\r\nsid One1\r\nbegin \"Two\"\r\nid 100002\r\nend\r\n", DefIssueCode::Unclosed,
	      "No end closes the block",
	      [](const void *r) { return std::strcmp(static_cast<const DefItemDef *>(r)->sid, "One1") == 0; },
	      "sid One1");

	// What the record cannot hold as the game reads it still blocks: an item with no alias that no `end` closes
	// (its `end` would derive one), a class the table walk runs past, a slide's frame count read from a slot an
	// earlier line left, one to three attachment angles, an attachment line with nothing, a function's fifth
	// argument, a carry limit with no class, a light with no values.
	blocks("an item the file's end closes, no alias", "items.def", "begin \"Open\"\r\nid 100001\r\nend");
	blocks("classrounds unknown", "weapon.def", weapon("classrounds nobody 5"));
	blocks("sights slide with no count", "weapon.def", weapon("sights a 0 0 1 1 blend slide"));
	blocks("addeweap two angles", "items.def", item("addeweap ewep01 100201 15 37"));
	blocks("addeweap alone", "items.def", item("addeweap"));
	blocks("function of five arguments", "weapon.def",
	       "weapon \"WPN_THING\"\r\naction \"FIRE\"\r\nfunction fire 1 2 3 4 5\r\nend\r\nend\r\n");
	blocks("ammoclass_max_carry alone", "weapon.def", "ammoclass_max_carry\r\n");
	blocks("light_move alone", "ammo.def", ammo("light_move"));
	// A powerup's and a number's members in one item the union cannot carry, however the block closes: here the
	// file's end, its `end` lost to the walk.
	blocks("a powerup and a clip size the file's end closes", "items.def",
	       "begin \"Box\"\r\nid 100001\r\nsid Box1\r\npowerupdef PU_X\r\nclipsize 5\r\nend");
	{
		// A dupsound of no values: the parser's own finding alone, no "reads as 0" beside it.
		const Read out = read("weapon.def", "weapon \"WPN_THING\"\r\naction \"FIRE\"\r\ndupsound\r\nend\r\nend\r\n",
		                      [](const void *) { return true; });
		bool reinterpreted_too = false;
		for (const DefIssue &issue : out.report) reinterpreted_too |= issue.code == DefIssueCode::Reinterpreted;
		if (!def_report_blocks(out.report) || reinterpreted_too) {
			std::printf("FAIL dupsound alone: one blocking finding, not a reading beside it\n");
			++failures;
		}
	}

	if (failures) std::printf("%d failure(s)\n", failures);
	else std::printf("def_game_reads: ok\n");
	return failures ? 1 : 0;
}
