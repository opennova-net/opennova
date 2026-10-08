// charattr.def's writer (charattr.h write_table): the text made from the table alone (ADR 0003), which the
// loader reads back to the same table.

#include <formats/charattr/charattr.h>

#include <formats/configfile/config_file.h>

#include <base/io/crt_ftol.h>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace opennova::charattr {

namespace {

bool same_bits(float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; }

// The value as the ConfigFile reads it back: a float (it holds a point) through atof [orig:
// ConfigFile_ParseValues @ 0x7606f0 over String_ClassifyNumeric @ 0x75d830].
bool reads_back(const std::string &text, float value) {
	return configfile::classify_numeric(text) == 2 && same_bits(static_cast<float>(io::retail_atof(text.c_str())), value);
}

// A float in the fewest fixed digits that read back to its bits, always with a point (an integer would read
// as an integer, which a value past the int32 range saturates); more digits where atof's double rounds the
// shortest spelling otherwise. False for an infinity or a NaN, which no ConfigFile number spells.
bool spell_float(float value, std::string &out) {
	if (!std::isfinite(value)) return false;
	char buffer[128];
	const std::to_chars_result shortest = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::fixed);
	if (shortest.ec == std::errc()) {
		out.assign(buffer, shortest.ptr);
		if (out.find('.') == std::string::npos) out += ".0";
		if (reads_back(out, value)) return true;
	}
	for (int digits = 1; digits <= 80; ++digits) {
		std::snprintf(buffer, sizeof buffer, "%.*f", digits, static_cast<double>(value));
		out = buffer;
		if (reads_back(out, value)) return true;
	}
	return false;
}

float real_value(const ClassRow &row, Property property) {
	switch (property) {
		case kStealth: return row.stealth;
		case kHpBonus: return row.hp_bonus;
		case kManaBonus: return row.mana_bonus;
		case kRecoilMute: return row.recoil_mute;
		case kXhairMute: return row.xhair_mute;
		case kScopeMute: return row.scope_mute;
		case kXhairDxMute: return row.xhairdx_mute;
		case kReloadMute: return row.reload_mute;
		default: return 0.0f;
	}
}

int32_t integer_value(const ClassRow &row, Property property) {
	switch (property) {
		case kJungleCammo: return row.jungle_cammo;
		case kDesertCammo: return row.desert_cammo;
		case kArcticCammo: return row.arctic_cammo;
		case kRunModifier: return row.run_modifier;
		default: return 0;
	}
}

} // namespace

bool write_table(const Table &table, std::string &text, std::string &error) {
	text.clear();
	error.clear();
	uint32_t named = 0;
	for (const AttributeName &name : kAttributeNames) named |= name.flag;
	bool ended = false;
	for (size_t index = 0; index < kClassCount; ++index) {
		const ClassRow &row = table.rows[index];
		const std::string label = "CHARACTER" + std::to_string(index + 1);
		if (!row.active) {
			// The loader leaves a class it does not reach zero, and reaches none after it.
			if (row_bytes(row) != row_bytes(ClassRow{})) {
				error = label + " is not read (no section of it) but holds values.";
				return false;
			}
			ended = true;
			continue;
		}
		if (ended) {
			error = label + " follows a class with no section: the game stops reading at the first class it lacks.";
			return false;
		}
		if (row.class_id != index + 1) {
			error = label + " holds class " + std::to_string(row.class_id) + ": the loader writes its row's class.";
			return false;
		}
		if ((row.attributes & ~named) != 0) {
			char flags[16];
			std::snprintf(flags, sizeof flags, "0x%X", row.attributes & ~named);
			error = label + "'s attributes hold " + flags + ", which no word of ATTRIBUTES names.";
			return false;
		}
		text += "[" + label + "]\r\n";
		for (const KeySpec &spec : kScalarKeys) {
			std::string value;
			if (spec.real) {
				const float real = real_value(row, spec.property);
				// A key the section lacks reads 0: the table is cleared first [orig: CharAttr_LoadFromDef @
				// 0x412168] and the accessor zeroes its output [orig: effect_get_param_value_0 @ 0x75fa00]. Only a
				// float of all-zero bits is that 0 (a -0.0 is written).
				if (same_bits(real, 0.0f)) continue;
				if (!spell_float(real, value)) {
					error = label + "'s " + spec.key + " is no number a file can hold.";
					return false;
				}
			} else {
				const int32_t integer = integer_value(row, spec.property);
				if (integer == 0) continue;
				value = std::to_string(integer);
			}
			text += std::string(spec.key) + " = " + value + "\r\n";
		}
		if (row.attributes != 0) {
			std::string words;
			for (const AttributeName &name : kAttributeNames)
				if (row.attributes & name.flag) words += (words.empty() ? "" : " ") + std::string(name.name);
			text += std::string(kAttributesKey) + " = " + words + "\r\n";
		}
		text += "\r\n";
	}
	// The ConfigFile reader clears a pool of the words' bytes one byte per value: a file of more values than that
	// pool overruns the game's heap [orig: ConfigFile_ParseText @ 0x7609e8], so it is refused, never written.
	const configfile::DataStringsPool pool =
			configfile::data_strings_pool(reinterpret_cast<const uint8_t *>(text.data()), text.size());
	if (pool.overrun() != 0) {
		error = "The file would hold " + std::to_string(pool.values) + " values against " +
		        std::to_string(pool.string_bytes) + " bytes of words (a " + std::to_string(pool.pool_bytes) +
		        "-byte buffer): the game's ConfigFile reader would clear " + std::to_string(pool.overrun()) +
		        " bytes past that buffer into the game's heap (ConfigFile_ParseText @ 0x7609e8). Fewer keys away "
		        "from 0 would fit it.";
		text.clear();
		return false;
	}
	return true;
}

} // namespace opennova::charattr
