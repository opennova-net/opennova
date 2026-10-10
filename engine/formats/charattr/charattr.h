#pragma once

// charattr.def: the character classes' attributes, the table retail loads once at boot on every peer
// [orig: Game_Run @ 0x4A7FE3 -> CharAttr_LoadFromDef @ 0x412140] into sixteen 124-byte rows at g_CharAttr
// (0xA79540). A ConfigFile (formats/configfile/config_file.h): a class is a [CHARACTERn] section, each key
// one field of its row. The loader clears the whole table (memset 0x7C0 bytes @ 0x412168), then reads
// CHARACTER1, CHARACTER2 and so on, stopping at the first class the file has no section of (@ 0x4121bf),
// sixteen at most; of two sections of a label the first is read (config_file.h find_config_section).
//
// What the game reads of it (docs/net/novaworld-net-re.md, "charattr.def: the table and its readers"):
// a class's ATTRIBUTES Medic and KnifeBonus words (the medic heal and its HUD markers, the medic-filtered
// send, the knife's reach), and its three *_CAMMO item type ids (the item a player of the class spawns as
// by the mission's camouflage, ItemList_FindIndexByTypeId's items.def id less 100000, the first items.def
// row for a type no row has). Every other field (STEALTH, HPBONUS, MANABONUS, the five *_MUTE scales,
// RUN_MODIFIER) and the AutoScope, SpreadBonus and WaterGirl words are read by no game code: only the
// anti-cheat challenge hashes them with the row (S2C 0x39 -> C2S 0x1C) and a debug page shows them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <formats/textlayout/text_layout.h>

namespace opennova::charattr {

inline constexpr size_t kClassCount = 16;
inline constexpr size_t kRowBytes = 124;      // 31 dwords [orig: the 0x7C stride, CharAttr_LoadFromDef @ 0x412454]
inline constexpr size_t kPropertyCount = 14;  // the disable latches g_CharAttrPropertyDisabled[0..13] @ 0xA79508

// A field by the id the table's accessors and S2C 0x41 name it by [orig: CharAttr_SetProperty @ 0x412890,
// CharAttr_GetAttributeFloat @ 0x412640, CharAttr_GetIntProperty @ 0x412800]. Id 1 names none.
enum Property : uint8_t {
	kAttributes = 0,
	kStealth = 2,
	kHpBonus = 3,
	kManaBonus = 4,
	kRecoilMute = 5,
	kXhairMute = 6,
	kScopeMute = 7,
	kXhairDxMute = 8,
	kReloadMute = 9,
	kJungleCammo = 10,
	kDesertCammo = 11,
	kArcticCammo = 12,
	kRunModifier = 13,
};

// The ATTRIBUTES words [orig: g_CharAttrAttributeNames @ 0x813F18, 20-byte rows: AutoScope 1, SpreadBonus 2,
// KnifeBonus 4, Medic 8, WaterGirl 0x20, then an empty name], each matched without case.
inline constexpr uint32_t kAutoScope = 0x01;
inline constexpr uint32_t kSpreadBonus = 0x02;
inline constexpr uint32_t kKnifeBonus = 0x04;
inline constexpr uint32_t kMedic = 0x08;
inline constexpr uint32_t kWaterGirl = 0x20;
struct AttributeName {
	const char *name;
	uint32_t flag;
};
inline constexpr std::array<AttributeName, 5> kAttributeNames{{
		{"AutoScope", kAutoScope},
		{"SpreadBonus", kSpreadBonus},
		{"KnifeBonus", kKnifeBonus},
		{"Medic", kMedic},
		{"WaterGirl", kWaterGirl},
}};
// A word's flag, 0 for a word the table has none of [orig: CharAttr_LoadFromDef @ 0x4123b0, stricmp over
// the 20-byte rows to the empty name].
uint32_t attribute_flag(std::string_view word);

// One class's row as the loader fills it [orig: g_CharAttr + 124 * (class - 1)].
struct ClassRow {
	bool active = false;     // +0: the loader found the class's section (@ 0x4121e0)
	uint8_t class_id = 0;    // +4: the class, 1..16 (@ 0x412451)
	float stealth = 0.0f;      // +8  STEALTH
	float hp_bonus = 0.0f;     // +12 HPBONUS
	float mana_bonus = 0.0f;   // +16 MANABONUS
	float recoil_mute = 0.0f;  // +20 RECOIL_MUTE
	float reload_mute = 0.0f;  // +24 RELOAD_MUTE
	float xhair_mute = 0.0f;   // +28 XHAIR_MUTE
	float xhairdx_mute = 0.0f; // +32 XHAIRDX_MUTE
	float scope_mute = 0.0f;   // +36 SCOPE_MUTE
	uint32_t attributes = 0;   // +40 ATTRIBUTES, the words' flags
	int32_t jungle_cammo = 0;  // +44 JUNGLE_CAMMO, an item type id
	int32_t desert_cammo = 0;  // +48 DESERT_CAMMO
	int32_t arctic_cammo = 0;  // +52 ARCTIC_CAMMO
	int32_t run_modifier = 0;  // +56 RUN_MODIFIER
	// +60..+123: no key fills them; zero.
	// The file's modeled layout this class was read with (textlayout: its section's note there); 0 for a class no
	// file's layout names (written in the writer's form). No byte of the row.
	uint64_t note = 0;
};

struct Table {
	std::array<ClassRow, kClassCount> rows{};
	uint64_t note = 0; // the file's own record in the layout the table was read with
};

// A class's camouflage item type id by its property: JUNGLE_CAMMO (10), DESERT_CAMMO (11) or ARCTIC_CAMMO
// (12); 0 for any other property [orig: CharAttr_GetCammoTypeId @ 0x4127b0 -- 10/11/12 @ 0x4127d2..0x4127dc,
// any other id 0 @ 0x4127c9].
int32_t cammo_of(const ClassRow &row, Property property);

// The camouflage property a mission's camouflage selector (the BMS mission's) picks a class's item by: 1
// JUNGLE_CAMMO, 2 ARCTIC_CAMMO, any other DESERT_CAMMO
// [orig: Entity_SpawnFromAnimSlotProperty @ 0x43c399..0x43c3be].
Property cammo_property_for_camouflage(int camouflage);

// The keys the loader reads, each with the property it fills, in the loader's order [orig:
// CharAttr_LoadFromDef @ 0x4121e7..0x412353]: a float (ConfigFile type 2) or an integer (type 1); then
// ATTRIBUTES, its words (@ 0x412381).
struct KeySpec {
	const char *key;
	Property property;
	bool real;
};
inline constexpr std::array<KeySpec, 12> kScalarKeys{{
		{"STEALTH", kStealth, true},
		{"HPBONUS", kHpBonus, true},
		{"MANABONUS", kManaBonus, true},
		{"RECOIL_MUTE", kRecoilMute, true},
		{"XHAIR_MUTE", kXhairMute, true},
		{"XHAIRDX_MUTE", kXhairDxMute, true},
		{"SCOPE_MUTE", kScopeMute, true},
		{"RELOAD_MUTE", kReloadMute, true},
		{"JUNGLE_CAMMO", kJungleCammo, false},
		{"DESERT_CAMMO", kDesertCammo, false},
		{"ARCTIC_CAMMO", kArcticCammo, false},
		{"RUN_MODIFIER", kRunModifier, false},
}};
inline constexpr const char *kAttributesKey = "ATTRIBUTES";
// The key a property is written under; null for id 1 and ids past 13.
const char *property_key(uint8_t property);

// The row's 124 bytes as retail holds them: the anti-cheat challenge's CRC input [orig:
// CharAttr_GetClassChecksum @ 0x412aa0 -> CRC_ComputeCustomTable(row, 124)].
std::array<uint8_t, kRowBytes> row_bytes(const ClassRow &row);
// The same rows byte for byte (a float by its bits: 0.0 and -0.0 differ, as the challenge's CRC sees them).
bool same_rows(const Table &a, const Table &b);

// Where the loader read a value: the value's token (its byte offset into the text and its length) and the
// token as written.
struct ValueSource {
	bool read = false;
	size_t offset = 0;
	size_t length = 0;
	std::string written;
};

// What the loader read of one class.
struct ClassSource {
	bool read = false;          // the loader reached the class's section
	size_t section_offset = 0;  // its '[' line
	std::array<ValueSource, kPropertyCount> values{}; // by Property (ATTRIBUTES: its first word)
	std::vector<ValueSource> attribute_words;          // each ATTRIBUTES word read, in order
};

// A section the loader never reads: one of a class after the first class the file has no section of, a
// second of a label (the first is read), or one whose label names no class 1 to 16.
struct UnreadSection {
	enum class Why { AfterMissing, Repeated, NotAClass };
	Why why = Why::NotAClass;
	std::string label;  // as the file writes it, upper case
	int class_id = 0;   // the class it names, 0 for none
	size_t offset = 0;  // its '[' line
};

struct Reading {
	size_t classes = 0;  // the classes read: 1 to `classes`
	std::array<ClassSource, kClassCount> sources{};
	std::vector<UnreadSection> unread;
};

// The table from charattr.def's bytes as the loader fills it [orig: CharAttr_LoadFromDef @ 0x412140]: `out`
// cleared first, then each class read, from the text form or the CBIN form alike (ConfigFile_LoadFromFile
// reads either into the sections the accessors walk; no shipped charattr.def is a CBIN). False when the
// load fails and the table stays cleared: no bytes, or a CBIN file the binary reader (formats/cbin) refuses,
// among them one with an entry of more than two values, an ATTRIBUTES of three words or more, which the game's
// reader takes (D-CBIN-3). `reading`, when given, says where each value came from and which sections are
// never read; of a CBIN file, which has no text, every offset is 0 and a number's `written` empty.
bool read_table(const uint8_t *data, size_t size, Table &out, Reading *reading = nullptr);
// The same read with the file's layout modeled (`notes` filled: each class's section a record, the lines the
// loader read its values from its entries, every other line, an unread section's among them, read for
// nothing; each class's note and the table's set). A CBIN file has no lines: `notes` comes back empty and
// every note 0, so a write puts the table down in the writer's form.
bool read_table(const uint8_t *data, size_t size, Table &out, Reading *reading, textlayout::Notes &notes);

// charattr.def's text holding `table` (ADR 0003: from the table, not from any file's bytes), which
// read_table reads back to the same table: each active class's section (CR LF line ends, the ConfigFile's)
// with every key the loader reads that holds other than 0 (a key the section lacks reads 0: the loader
// clears the table first; a float of all-zero bits is that 0, a -0.0 is written), a float always with a
// point and the digits that read back to its bits, ATTRIBUTES the words of its flags (left out at 0).
// False with the reason for a table no file loads as: an active class after an inactive one, a class id
// other than its row's, a row the loader leaves zero holding a value, a flag no word has, a float that
// is no number; or for a text whose values the ConfigFile reader's pool of its words cannot take, which
// would overrun the game's heap (configfile::data_strings_pool, ConfigFile_ParseText @ 0x7609e8).
bool write_table(const Table &table, std::string &text, std::string &error);
// The same text over the file's modeled layout where the table has one (the file as it was but for the lines of
// a changed value: its comments, its spacing, a number's own spelling, the sections and lines the loader reads
// nothing of; a key set anew after the key before it in the writer's order; a class added after the file's
// classes in the writer's form). The text is read again: one that would not read back as the table (a key put
// down where the section's cursor would not find it) is written in the writer's form, `rewritten` set.
bool write_table(const Table &table, const textlayout::Notes *notes, std::string &text, std::string &error,
                 bool *rewritten = nullptr);
// The text write_table makes, the ConfigFile pool left unchecked: what a file of the table holds where its values
// would overrun that pool (configfile::data_strings_pool says by how much). False with the reason for a table no
// file loads as, as write_table.
bool compose_table(const Table &table, const textlayout::Notes *notes, std::string &text, std::string &error,
                   bool *rewritten = nullptr);
// The notes' lines modeled against what the writer puts down for the table as read (textlayout::model; the
// noted read_table runs it last).
void model_layout(const Table &table, textlayout::Notes &notes);

} // namespace opennova::charattr
