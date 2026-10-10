// charattr.def's loader and writer (formats/charattr/charattr.h, ADR 0003). The synthetic legs read a JO-shaped
// file (a class after a missing one, a second section of a label, the legacy block JO:CA ships below its live
// classes, a word no attribute is, a value the ConfigFile reads as text, an LF alone) as CharAttr_LoadFromDef
// fills the table, with where each value was read and which sections are never read; write the table and read
// it back to the same rows byte for byte (a float by its bits), a key at 0 left out; refuse a table no file loads
// as, and one whose text would hold more values than the ConfigFile reader's pool of its text values takes. The
// retail legs read JO:CA's own charattr.def (the packed install, base and each expansion, and the extracted
// tree): classes 1 to 9 read, the class-8 row hashing to the capture-derived 0x22A25E01, the file written and
// read again the same table, and the shipped and the written text each under the pool.
// [orig: CharAttr_LoadFromDef @0x412140; CharAttr_GetClassChecksum @0x412aa0; ConfigFile_ParseText @0x7609e8]
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <base/io/crc32_mpeg2.h>
#include <base/vfs/vfs.h>
#include <formats/cbin/binary_config.h>
#include <formats/charattr/charattr.h>
#include <formats/configfile/config_file.h>

#include "common/retail_paths.h"

using namespace opennova;

namespace {

int failures = 0;

#define CHECK(cond)                                                                    \
	do {                                                                               \
		if (!(cond)) {                                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
			++failures;                                                                \
		}                                                                              \
	} while (0)

// CR LF line ends, as charattr.def is stored: the ConfigFile reader ends a line there alone.
std::string crlf(const std::string &text) {
	std::string out;
	for (char c : text) {
		if (c == '\n') out += '\r';
		out += c;
	}
	return out;
}

bool read(const std::string &text, charattr::Table &table, charattr::Reading *reading = nullptr) {
	return charattr::read_table(reinterpret_cast<const uint8_t *>(text.data()), text.size(), table, reading);
}

// How many CR LF lines differ between two texts of as many lines (-1 for another count).
int lines_changed(const std::string &a, const std::string &b) {
	const auto split = [](const std::string &t) {
		std::vector<std::string> out;
		size_t at = 0;
		while (at < t.size()) {
			const size_t end = t.find("\r\n", at);
			out.push_back(t.substr(at, end == std::string::npos ? std::string::npos : end - at));
			at = end == std::string::npos ? t.size() : end + 2;
		}
		return out;
	};
	const std::vector<std::string> x = split(a), y = split(b);
	if (x.size() != y.size()) return -1;
	int n = 0;
	for (size_t i = 0; i < x.size(); ++i) n += x[i] != y[i];
	return n;
}

uint32_t bits(float value) {
	uint32_t out = 0;
	std::memcpy(&out, &value, sizeof out);
	return out;
}

uint32_t row_crc(const charattr::ClassRow &row) {
	const std::array<uint8_t, charattr::kRowBytes> bytes = charattr::row_bytes(row);
	return io::crc32_mpeg2_update(io::kCrc32Mpeg2Init, bytes.data(), bytes.size());
}

// Written and read again: the same rows, byte for byte; written again from that read: the same text.
void round_trip(const charattr::Table &table, const char *what) {
	std::string text, error;
	CHECK(charattr::write_table(table, text, error));
	if (!error.empty()) std::printf("  %s: %s\n", what, error.c_str());
	charattr::Table again;
	CHECK(read(text, again));
	CHECK(charattr::same_rows(table, again));
	std::string twice;
	CHECK(charattr::write_table(again, twice, error) && twice == text);
	CHECK(text.find('\n') == std::string::npos || text.find("\r\n") != std::string::npos);
	for (size_t i = 0; i < text.size(); ++i)
		if (text[i] == '\n') CHECK(i > 0 && text[i - 1] == '\r');
}

const char *kShaped =
		"//////////////////////////////////\n"
		"// Attributes: AutoScope KnifeBonus Medic  WaterGirl\n"
		"[CHARACTER1]\n"
		"STEALTH\t\t\t= 25\n"
		"HPBONUS\t\t\t= 2\t\n"
		"RECOIL_MUTE \t= 0.75\t\n"
		"XHAIR_MUTE\t\t= 2.5\t\n"
		"SCOPE_MUTE  \t= 0.0\t\n"
		"JUNGLE_CAMMO\t= 5310\n"
		"DESERT_CAMMO\t= 5310\n"
		"ARCTIC_CAMMO\t= 5310\n"
		"RUN_MODIFIER\t= -1\n"
		"ATTRIBUTES  \t= AutoScope Medick, KnifeBonus\n"
		"\n"
		"[CHARACTER2]\n"
		"STEALTH\t\t\t= high\n"
		"JUNGLE_CAMMO\t= 5305.9\n"
		"ATTRIBUTES  \t= NULL\n"
		"[CHARACTER5]\n"
		"ATTRIBUTES  \t= Medic\n"
		"//[CHARACTER3]\n"
		"[CHARACTER1]\n"
		"STRENGTH\t\t= 28.0\n"
		"ATTRIBUTES  \t= WaterGirl\n"
		"[NOTES]\n"
		"TEXT = x\n";

void synthetic_loader() {
	charattr::Table table;
	charattr::Reading reading;
	const std::string text = crlf(kShaped);
	CHECK(read(text, table, &reading));
	// Classes 1 and 2 read; 3 is missing, so 5 never is.
	CHECK(reading.classes == 2);
	const charattr::ClassRow &one = table.rows[0];
	CHECK(one.active && one.class_id == 1);
	CHECK(bits(one.stealth) == bits(25.0f) && bits(one.hp_bonus) == bits(2.0f) && bits(one.recoil_mute) == bits(0.75f));
	CHECK(bits(one.xhair_mute) == bits(2.5f) && bits(one.scope_mute) == bits(0.0f) && one.mana_bonus == 0.0f);
	CHECK(one.jungle_cammo == 5310 && one.desert_cammo == 5310 && one.arctic_cammo == 5310 && one.run_modifier == -1);
	// AutoScope and KnifeBonus; the unknown word adds nothing. The first [CHARACTER1]'s, not the later one's.
	CHECK(one.attributes == (charattr::kAutoScope | charattr::kKnifeBonus));
	const charattr::ClassRow &two = table.rows[1];
	// A word read as text is 0; a float read as an integer truncates; NULL is no word.
	CHECK(two.active && two.class_id == 2 && two.stealth == 0.0f && two.jungle_cammo == 5305 && two.attributes == 0);
	CHECK(!table.rows[2].active && !table.rows[4].active);
	// Where each value came from.
	const charattr::ValueSource &jungle = reading.sources[0].values[charattr::kJungleCammo];
	CHECK(jungle.read && jungle.written == "5310" && text.compare(jungle.offset, jungle.length, "5310") == 0);
	CHECK(reading.sources[0].attribute_words.size() == 3 && reading.sources[0].attribute_words[1].written == "Medick");
	CHECK(text.compare(reading.sources[0].attribute_words[2].offset, 10, "KnifeBonus") == 0);
	CHECK(reading.sources[1].values[charattr::kStealth].written == "high");
	CHECK(!reading.sources[0].values[charattr::kManaBonus].read);
	// The unread sections: CHARACTER5 after the missing 3, the second CHARACTER1, NOTES.
	CHECK(reading.unread.size() == 3);
	if (reading.unread.size() == 3) {
		CHECK(reading.unread[0].label == "CHARACTER5" && reading.unread[0].class_id == 5 &&
		      reading.unread[0].why == charattr::UnreadSection::Why::AfterMissing);
		CHECK(reading.unread[1].label == "CHARACTER1" &&
		      reading.unread[1].why == charattr::UnreadSection::Why::Repeated &&
		      text.compare(reading.unread[1].offset, 12, "[CHARACTER1]") == 0 && reading.unread[1].offset > jungle.offset);
		CHECK(reading.unread[2].label == "NOTES" && reading.unread[2].why == charattr::UnreadSection::Why::NotAClass);
	}
	// The class row's bytes: active, class, the floats' bits, the words, the ids, zero past +60.
	const std::array<uint8_t, charattr::kRowBytes> row = charattr::row_bytes(one);
	CHECK(row[0] == 1 && row[4] == 1 && row[5] == 0 && row[40] == 0x05 && row[44] == (5310 & 0xFF));
	for (size_t i = 60; i < row.size(); ++i) CHECK(row[i] == 0);

	// An LF alone ends no line: the whole file is the first section's '[' line, which holds no entry.
	charattr::Table lf;
	CHECK(read("[CHARACTER1]\nSTEALTH = 25\n[CHARACTER2]\n", lf));
	CHECK(lf.rows[0].active && lf.rows[0].stealth == 0.0f && !lf.rows[1].active);
	// No bytes, or a CBIN file cut short: the load fails and the table stays cleared.
	charattr::Table none;
	none.rows[3].active = true;
	CHECK(!charattr::read_table(nullptr, 0, none) && !none.rows[3].active);
	CHECK(!read("CBIN\x01\x00\x00\x00", none));
	// A key the first entries of the section shadow: the first of a key is read.
	charattr::Table twice;
	CHECK(read(crlf("[CHARACTER1]\nSTEALTH = 1\nSTEALTH = 2\n"), twice) && twice.rows[0].stealth == 1.0f);
	// An entry with no value stops a lookup past it [orig: effect_get_param_value_0 @ 0x75faa3].
	charattr::Table stopped;
	CHECK(read(crlf("[CHARACTER1]\nNOTE = ,\nSTEALTH = 3\n"), stopped) && stopped.rows[0].stealth == 0.0f);
	std::printf("loader: classes in order to the first missing, the first section of a label, values and words\n");
}

// A file read with its layout (textlayout) and written again is itself: its comments, a number's own spelling, a
// `;` comment, the attribute words in the file's order, a class before another, a section never read. One value
// changed changes its one line; a key set anew goes after the key before it in the writer's order, where the
// section's cursor still finds it; an attribute removed leaves the others where they stood; a class added comes
// after the file's in the writer's form.
void synthetic_noted() {
	const std::string text = crlf(
			"// classes\n"
			"[CHARACTER2]\n"
			"STEALTH\t= 3.50 ; spelled\n"
			"ATTRIBUTES = Medic  KnifeBonus\n"
			"\n"
			"[CHARACTER1]\n"
			"HPBONUS = 2\n"
			"JUNGLE_CAMMO = 5310\n"
			"\n"
			"[CHARACTER1]\n"
			"STEALTH = 9\n");
	textlayout::Notes notes;
	charattr::Table table;
	CHECK(charattr::read_table(reinterpret_cast<const uint8_t *>(text.data()), text.size(), table, nullptr, notes));
	CHECK(table.rows[0].active && table.rows[1].active && !table.rows[2].active);
	std::string written, error;
	bool rewritten = true;
	CHECK(charattr::write_table(table, &notes, written, error, &rewritten) && !rewritten && written == text);
	charattr::Table changed = table;
	changed.rows[0].hp_bonus = 3.0f;
	CHECK(charattr::write_table(changed, &notes, written, error, &rewritten) && !rewritten);
	CHECK(lines_changed(text, written) == 1 && written.find("HPBONUS = 3.0\r\n") != std::string::npos);
	changed = table;
	changed.rows[0].run_modifier = 1;
	CHECK(charattr::write_table(changed, &notes, written, error, &rewritten) && !rewritten);
	CHECK(written.find("JUNGLE_CAMMO = 5310\r\nRUN_MODIFIER = 1\r\n") != std::string::npos);
	charattr::Table again;
	CHECK(read(written, again) && charattr::same_rows(again, changed));
	changed = table;
	changed.rows[1].attributes = charattr::kKnifeBonus;
	CHECK(charattr::write_table(changed, &notes, written, error, &rewritten) && !rewritten);
	CHECK(written.find("ATTRIBUTES = KnifeBonus\r\n") != std::string::npos && lines_changed(text, written) == 1);
	changed = table;
	changed.rows[2].active = true;
	changed.rows[2].class_id = 3;
	changed.rows[2].stealth = 1.0f;
	CHECK(charattr::write_table(changed, &notes, written, error, &rewritten) && !rewritten);
	CHECK(written.size() > text.size() && written.find("[CHARACTER3]\r\nSTEALTH = 1.0\r\n") != std::string::npos);
	CHECK(read(written, again) && charattr::same_rows(again, changed));
	std::printf("noted: an authored file written again as it was; a value, a key set anew, an attribute removed and "
	            "a class added each where the file has room\n");
}

void synthetic_writer() {
	charattr::Table table;
	CHECK(read(crlf(kShaped), table));
	round_trip(table, "the shaped file");
	// Floats that need care: a fraction, a tiny one, a negative zero, one past the int32 range, a denormal, a
	// whole one (which must keep a point, or it reads as an integer).
	charattr::Table floats;
	for (size_t i = 0; i < 4; ++i) {
		floats.rows[i].active = true;
		floats.rows[i].class_id = static_cast<uint8_t>(i + 1);
	}
	floats.rows[0].stealth = 0.1f;
	floats.rows[0].hp_bonus = 1e-30f;
	floats.rows[0].mana_bonus = -0.0f;
	floats.rows[0].recoil_mute = 3.0e9f;
	floats.rows[0].reload_mute = std::numeric_limits<float>::denorm_min();
	floats.rows[0].xhair_mute = 16777217.0f;
	floats.rows[1].xhairdx_mute = -123.456f;
	floats.rows[1].scope_mute = 25.0f;
	floats.rows[2].attributes = charattr::kAutoScope | charattr::kSpreadBonus | charattr::kKnifeBonus | charattr::kMedic |
	                            charattr::kWaterGirl;
	floats.rows[3].jungle_cammo = -5;
	floats.rows[3].run_modifier = std::numeric_limits<int32_t>::min();
	round_trip(floats, "the floats");
	// A table no file loads as is refused.
	std::string text, error;
	charattr::Table gap = floats;
	gap.rows[1] = charattr::ClassRow{};
	CHECK(!charattr::write_table(gap, text, error) && error.find("CHARACTER3") != std::string::npos);
	charattr::Table wrong = floats;
	wrong.rows[0].class_id = 7;
	CHECK(!charattr::write_table(wrong, text, error));
	charattr::Table flag = floats;
	flag.rows[0].attributes = 0x10;
	CHECK(!charattr::write_table(flag, text, error));
	charattr::Table nan = floats;
	nan.rows[0].stealth = std::numeric_limits<float>::quiet_NaN();
	CHECK(!charattr::write_table(nan, text, error));
	charattr::Table stray;
	stray.rows[5].stealth = 1.0f;
	CHECK(!charattr::write_table(stray, text, error));
	// No class: an empty text, which loads as no class.
	CHECK(charattr::write_table(charattr::Table{}, text, error) && text.empty());
	// A key at 0 is left out: the loader clears the table first, so a key the section lacks reads 0. Only a float
	// of all-zero bits is that 0: a -0.0 is written.
	charattr::Table zeros;
	zeros.rows[0].active = true;
	zeros.rows[0].class_id = 1;
	zeros.rows[0].stealth = 25.0f;
	zeros.rows[0].hp_bonus = -0.0f;
	zeros.rows[0].jungle_cammo = 5305;
	zeros.rows[1].active = true;
	zeros.rows[1].class_id = 2;
	CHECK(charattr::write_table(zeros, text, error) &&
	      text == "[CHARACTER1]\r\nSTEALTH = 25.0\r\nHPBONUS = -0.0\r\nJUNGLE_CAMMO = 5305\r\n\r\n[CHARACTER2]\r\n\r\n");
	round_trip(zeros, "the zeros");
	// The ConfigFile reader clears its pool of the text values' bytes (at least 64) one byte per value [orig:
	// ConfigFile_ParseText @ 0x7609e8]: five classes of twelve keys away from 0 and no word (60 values) fit it,
	// six (72) would overrun the game's heap and are refused, nothing written.
	charattr::Table full;
	for (size_t i = 0; i < charattr::kClassCount; ++i) {
		charattr::ClassRow &row = full.rows[i];
		row.active = i < 5;
		row.class_id = row.active ? static_cast<uint8_t>(i + 1) : 0;
		if (!row.active) continue;
		row.stealth = row.hp_bonus = row.mana_bonus = row.recoil_mute = row.reload_mute = 1.5f;
		row.xhair_mute = row.xhairdx_mute = row.scope_mute = 2.5f;
		row.jungle_cammo = row.desert_cammo = row.arctic_cammo = 5305;
		row.run_modifier = 1;
	}
	CHECK(charattr::write_table(full, text, error));
	const configfile::DataStringsPool five =
			configfile::data_strings_pool(reinterpret_cast<const uint8_t *>(text.data()), text.size());
	CHECK(five.values == 60 && five.string_bytes == 0 && five.overrun() == 0);
	full.rows[5] = full.rows[4];
	full.rows[5].class_id = 6;
	CHECK(!charattr::write_table(full, text, error) && text.empty() && error.find("72 values") != std::string::npos &&
	      error.find("8 bytes past") != std::string::npos && error.find("0x7609e8") != std::string::npos);
	// The same text, the pool unchecked: what the file would hold.
	CHECK(charattr::compose_table(full, nullptr, text, error) &&
	      configfile::data_strings_pool(reinterpret_cast<const uint8_t *>(text.data()), text.size()).overrun() == 8);
	std::printf("writer: every key away from 0, CR LF, the floats by their bits; a table no file loads as, or whose "
	            "text would overrun the reader's pool, refused (composed all the same)\n");
}

// A charattr.def in the CBIN form reads as its text twin: ConfigFile_LoadFromFile takes it through the binary
// reader into the sections the loader's accessors walk [orig: CharAttr_LoadFromDef @ 0x412177 ->
// ConfigFile_LoadFromFile @ 0x760aa3 -> ConfigFile_ParseBinary @ 0x75e8a0]. It has no lines, so its notes are
// empty and a write puts the table down in the writer's form.
void synthetic_cbin() {
	cbin::BinaryConfig config;
	config.xor_key = 0x2468ACE1u;
	config.strings = {"CHARACTER1", "STEALTH", "JUNGLE_CAMMO", "ATTRIBUTES", "Medic", "KnifeBonus", "CHARACTER2"};
	const auto real = [](float value) {
		cbin::BinaryConfig::Value v;
		std::memcpy(&v.raw, &value, sizeof v.raw);
		v.flags = cbin::BinaryConfig::kFloat;
		return v;
	};
	const auto integer = [](int32_t value) {
		return cbin::BinaryConfig::Value{static_cast<uint32_t>(value), cbin::BinaryConfig::kInteger};
	};
	const auto text = [](uint32_t index) { return cbin::BinaryConfig::Value{index, cbin::BinaryConfig::kString}; };
	config.labels = {
			{1, {{2, {real(25.0f)}}, {3, {integer(5310)}}, {4, {text(5), text(6)}}}},
			{7, {{2, {real(0.5f)}}, {3, {integer(5305)}}}},
	};
	std::vector<uint8_t> bytes;
	std::string error;
	CHECK(cbin::encode_binary_config(config, bytes, error));
	CHECK(bytes.size() >= 4 && std::memcmp(bytes.data(), "CBIN", 4) == 0);

	charattr::Table binary;
	charattr::Reading reading;
	CHECK(charattr::read_table(bytes.data(), bytes.size(), binary, &reading));
	charattr::Table twin;
	CHECK(read(crlf("[CHARACTER1]\nSTEALTH = 25.0\nJUNGLE_CAMMO = 5310\nATTRIBUTES = Medic, KnifeBonus\n"
	                "[CHARACTER2]\nSTEALTH = 0.5\nJUNGLE_CAMMO = 5305\n"),
	           twin));
	CHECK(charattr::same_rows(binary, twin));
	CHECK(reading.classes == 2 && binary.rows[0].active && binary.rows[1].active && !binary.rows[2].active);
	CHECK(bits(binary.rows[0].stealth) == bits(25.0f) && binary.rows[0].jungle_cammo == 5310);
	CHECK(binary.rows[0].attributes == (charattr::kMedic | charattr::kKnifeBonus));
	CHECK(bits(binary.rows[1].stealth) == bits(0.5f) && binary.rows[1].jungle_cammo == 5305);
	// A CBIN file has no text: the Reading copies none of its table's strings.
	CHECK(reading.sources[0].attribute_words.size() == 2 && reading.sources[0].attribute_words[1].read &&
	      reading.sources[0].attribute_words[1].written.empty());

	// No lines to lay out: the notes come back empty, and the writer's form reads back to the same rows.
	textlayout::Notes notes;
	charattr::Table noted;
	CHECK(charattr::read_table(bytes.data(), bytes.size(), noted, nullptr, notes));
	CHECK(notes.records.empty() && noted.note == 0 && noted.rows[0].note == 0 && charattr::same_rows(noted, twin));
	std::string written;
	bool rewritten = true;
	charattr::Table again;
	CHECK(charattr::write_table(noted, &notes, written, error, &rewritten) && read(written, again) &&
	      charattr::same_rows(again, twin));

	// A CBIN file the binary reader refuses fails the load and leaves the table cleared.
	charattr::Table cut;
	cut.rows[0].active = true;
	CHECK(!charattr::read_table(bytes.data(), bytes.size() - 1, cut) && !cut.rows[0].active);

	// The game's reader takes an ATTRIBUTES of three words [orig: ConfigFile_ParseBinary @ 0x75eb21] and copies a
	// float into a float property through the x87, which quiets a signaling NaN [orig: Effect_GetParamValue_0
	// @ 0x75fa00, fld @ 0x75fb7f, fstp @ 0x75fb83] (D-CBIN-3).
	cbin::BinaryConfig wide = config;
	wide.strings.push_back("WaterGirl");
	wide.labels[0].entries[2].values.push_back(text(8));
	cbin::BinaryConfig::Value snan;
	snan.raw = 0x7FA00000u;
	snan.flags = cbin::BinaryConfig::kFloat;
	wide.labels[0].entries[0].values[0] = snan;
	std::vector<uint8_t> wide_bytes;
	CHECK(cbin::encode_binary_config(wide, wide_bytes, error));
	charattr::Table three;
	CHECK(charattr::read_table(wide_bytes.data(), wide_bytes.size(), three));
	CHECK(three.rows[0].attributes == (charattr::kMedic | charattr::kKnifeBonus | charattr::kWaterGirl));
	CHECK(bits(three.rows[0].stealth) == 0x7FE00000u);

	// A null-string STEALTH: the loader reads every scalar key as a number, which never touches the word
	// [orig: ConfigFile_ReadKeyValue @ 0x75fd00 hands Effect_GetParamValue_0 no text buffer; @ 0x75fb39..0x75fb46],
	// so every class loads, STEALTH 0.
	cbin::BinaryConfig null_stealth = config;
	null_stealth.labels[0].entries[0].values[0] = text(0);
	std::vector<uint8_t> null_bytes;
	CHECK(cbin::encode_binary_config(null_stealth, null_bytes, error));
	charattr::Table nulled;
	CHECK(charattr::read_table(null_bytes.data(), null_bytes.size(), nulled) && nulled.rows[0].active &&
	      nulled.rows[1].active && bits(nulled.rows[0].stealth) == 0 && nulled.rows[0].jungle_cammo == 5310);
	// A null ATTRIBUTES word, read as text, faults: the game ends, the load fails here.
	cbin::BinaryConfig null_word = config;
	null_word.labels[0].entries[2].values[1] = text(0);
	CHECK(cbin::encode_binary_config(null_word, null_bytes, error));
	CHECK(!charattr::read_table(null_bytes.data(), null_bytes.size(), nulled) && !nulled.rows[0].active);
	// A null label after the classes: the lookup for the class after the last scans to it and faults
	// [orig: @ 0x4121b5 ConfigFile_FindSection -> ConfigFile_FindLabelLinear @ 0x75ee50].
	cbin::BinaryConfig null_label = config;
	null_label.labels.push_back(cbin::BinaryConfig::Label{});
	CHECK(cbin::encode_binary_config(null_label, null_bytes, error));
	CHECK(!charattr::read_table(null_bytes.data(), null_bytes.size(), nulled) && !nulled.rows[0].active);
	std::printf("cbin: a CBIN charattr.def reads as its text twin, with no notes\n");
}

// A class's camouflage items by their property [orig: CharAttr_GetCammoTypeId @ 0x4127b0], and the property a
// mission's camouflage selector picks [orig: Entity_SpawnFromAnimSlotProperty @ 0x43c399..0x43c3be]: 1 jungle,
// 2 arctic, any other desert.
void synthetic_cammo() {
	charattr::ClassRow row;
	row.jungle_cammo = 10;
	row.desert_cammo = 11;
	row.arctic_cammo = 12;
	row.run_modifier = 13;
	CHECK(charattr::cammo_of(row, charattr::kJungleCammo) == 10 && charattr::cammo_of(row, charattr::kDesertCammo) == 11 &&
	      charattr::cammo_of(row, charattr::kArcticCammo) == 12);
	CHECK(charattr::cammo_of(row, charattr::kRunModifier) == 0 && charattr::cammo_of(row, charattr::kStealth) == 0 &&
	      charattr::cammo_of(row, static_cast<charattr::Property>(1)) == 0);
	CHECK(charattr::cammo_property_for_camouflage(1) == charattr::kJungleCammo &&
	      charattr::cammo_property_for_camouflage(2) == charattr::kArcticCammo &&
	      charattr::cammo_property_for_camouflage(0) == charattr::kDesertCammo &&
	      charattr::cammo_property_for_camouflage(3) == charattr::kDesertCammo &&
	      charattr::cammo_property_for_camouflage(-1) == charattr::kDesertCammo);
	std::printf("cammo: each property's item, none for another; the selector 1 jungle, 2 arctic, else desert\n");
}

int retail_legs() {
	int ran = 0;
	auto leg = [&](const std::vector<uint8_t> &bytes, const std::string &what) {
		charattr::Table table;
		charattr::Reading reading;
		CHECK(charattr::read_table(bytes.data(), bytes.size(), table, &reading));
		// CHARACTER1 and 5..9 live, 2..4 from the legacy block; the legacy block's second CHARACTER1 and
		// CHARACTER5 are never read.
		CHECK(reading.classes == 9);
		CHECK(table.rows[0].jungle_cammo == 5310 && table.rows[4].attributes == charattr::kMedic &&
		      table.rows[7].attributes == charattr::kKnifeBonus && table.rows[1].jungle_cammo == 5300);
		CHECK(row_crc(table.rows[7]) == 0x22A25E01u);
		size_t repeated = 0;
		for (const charattr::UnreadSection &unread : reading.unread)
			repeated += unread.why == charattr::UnreadSection::Why::Repeated ? 1 : 0;
		CHECK(repeated == 2);
		round_trip(table, what.c_str());
		// The file as shipped and the file written from its table, each under the ConfigFile reader's pool.
		const configfile::DataStringsPool shipped = configfile::data_strings_pool(bytes.data(), bytes.size());
		CHECK(shipped.values == 278 && shipped.string_bytes == 288 && shipped.overrun() == 0);
		std::string written, error;
		CHECK(charattr::write_table(table, written, error));
		const configfile::DataStringsPool pool =
				configfile::data_strings_pool(reinterpret_cast<const uint8_t *>(written.data()), written.size());
		CHECK(pool.overrun() == 0);
		std::printf("%s: classes 1..9, CHARACTER8's row CRC 0x22A25E01, written and read the same; 278 values "
		            "over 288 bytes as shipped, %u over %u written\n",
		            what.c_str(), pool.values, pool.string_bytes);
		// Read with its layout and written again: the shipped file byte for byte (its banners, its spacing, the
		// legacy block's sections, the classes in the file's order); one value changed, one line.
		textlayout::Notes notes;
		charattr::Table noted;
		CHECK(charattr::read_table(bytes.data(), bytes.size(), noted, nullptr, notes));
		bool rewritten = true;
		CHECK(charattr::write_table(noted, &notes, written, error, &rewritten) && !rewritten);
		CHECK(written == std::string(bytes.begin(), bytes.end()));
		noted.rows[7].run_modifier = 2;
		CHECK(charattr::write_table(noted, &notes, written, error, &rewritten) && !rewritten);
		CHECK(lines_changed(std::string(bytes.begin(), bytes.end()), written) == 1);
		charattr::Table again;
		CHECK(read(written, again) && charattr::same_rows(again, noted));
		std::printf("%s: written again over its layout byte for byte; one value changed one line\n", what.c_str());
		++ran;
	};
	const std::string install = retail::install();
	if (!install.empty()) {
		std::vector<std::string> mounts{ std::string() };
		for (const std::string &expansion : retail::expansions()) mounts.push_back(expansion);
		for (const std::string &expansion : mounts) {
			opennova::Vfs vfs;
			std::vector<uint8_t> bytes;
			if (!vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed)) {
				std::printf("FAIL mount_game(%s, %s): %s\n", install.c_str(), expansion.c_str(), vfs.last_error().c_str());
				++failures;
			} else if (!vfs.read_file("charattr.def", bytes) || bytes.empty()) {
				std::printf("FAIL charattr.def is not on the install mount %s %s\n", install.c_str(), expansion.c_str());
				++failures;
			} else {
				leg(bytes, "the install's charattr.def" + (expansion.empty() ? std::string() : " (" + expansion + ")"));
			}
		}
	} else {
		retail::skip_leg("OPENNOVA_JO_DIR (the packed install's charattr.def)");
	}
	const std::string path = retail::asset_file("charattr.def");
	if (!path.empty()) {
		std::FILE *file = std::fopen(path.c_str(), "rb");
		std::vector<uint8_t> bytes;
		if (file) {
			uint8_t buffer[4096];
			size_t got = 0;
			while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) bytes.insert(bytes.end(), buffer, buffer + got);
			std::fclose(file);
		}
		CHECK(!bytes.empty());
		leg(bytes, path);
	}
	if (ran == 0 && retail::assets().empty()) retail::skip_leg("OPENNOVA_JO_ASSETS (the extracted charattr.def)");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	synthetic_loader();
	synthetic_writer();
	synthetic_noted();
	synthetic_cbin();
	synthetic_cammo();
	retail_legs();
	if (failures == 0) std::printf("charattr: all passed\n");
	return failures == 0 ? 0 : 1;
}
