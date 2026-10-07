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
				if (!spell_float(real_value(row, spec.property), value)) {
					error = label + "'s " + spec.key + " is no number a file can hold.";
					return false;
				}
			} else {
				value = std::to_string(integer_value(row, spec.property));
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
	return true;
}

} // namespace opennova::charattr
