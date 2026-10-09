// charattr.def's loader (charattr.h) [orig: CharAttr_LoadFromDef @ 0x412140].

#include <formats/charattr/charattr.h>

#include <formats/configfile/config_file.h>

#include <base/io/strutil.h>

#include <cstdio>
#include <cstring>

namespace opennova::charattr {

namespace {

void put_u32(std::array<uint8_t, kRowBytes> &row, size_t offset, uint32_t value) {
	row[offset + 0] = static_cast<uint8_t>(value);
	row[offset + 1] = static_cast<uint8_t>(value >> 8);
	row[offset + 2] = static_cast<uint8_t>(value >> 16);
	row[offset + 3] = static_cast<uint8_t>(value >> 24);
}

uint32_t float_bits(float value) {
	uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value), "retail charattr float is 32-bit");
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

float *real_field(ClassRow &row, Property property) {
	switch (property) {
		case kStealth: return &row.stealth;
		case kHpBonus: return &row.hp_bonus;
		case kManaBonus: return &row.mana_bonus;
		case kRecoilMute: return &row.recoil_mute;
		case kXhairMute: return &row.xhair_mute;
		case kScopeMute: return &row.scope_mute;
		case kXhairDxMute: return &row.xhairdx_mute;
		case kReloadMute: return &row.reload_mute;
		default: return nullptr;
	}
}

int32_t *integer_field(ClassRow &row, Property property) {
	switch (property) {
		case kJungleCammo: return &row.jungle_cammo;
		case kDesertCammo: return &row.desert_cammo;
		case kArcticCammo: return &row.arctic_cammo;
		case kRunModifier: return &row.run_modifier;
		default: return nullptr;
	}
}

// The value the accessor just read: the section's current entry's value `index` (read_config_value and
// read_current_config_value leave the cursor on it).
ValueSource source_of(const configfile::ConfigSection &section, int index) {
	ValueSource source;
	if (section.current >= section.entries.size()) return source;
	const configfile::ConfigEntry &entry = section.entries[section.current];
	if (index < 1 || static_cast<size_t>(index) > entry.values.size()) return source;
	const configfile::ConfigValue &value = entry.values[static_cast<size_t>(index - 1)];
	source.read = true;
	source.offset = value.offset;
	source.length = value.text.size();
	source.written = value.text;
	return source;
}

// The class a label names, 1..16, from "character<digits>" exactly as the loader's sprintf spells it; 0
// for any other label.
int label_class(const std::string &label) {
	for (int n = 1; n <= static_cast<int>(kClassCount); ++n) {
		char name[32];
		std::snprintf(name, sizeof name, "character%d", n);
		if (label == name) return n;
	}
	return 0;
}

} // namespace

uint32_t attribute_flag(std::string_view word) {
	for (const AttributeName &name : kAttributeNames)
		if (strutil::iequals(word, name.name)) return name.flag;
	return 0;
}

const char *property_key(uint8_t property) {
	if (property == kAttributes) return kAttributesKey;
	for (const KeySpec &spec : kScalarKeys)
		if (spec.property == property) return spec.key;
	return nullptr;
}

std::array<uint8_t, kRowBytes> row_bytes(const ClassRow &row) {
	std::array<uint8_t, kRowBytes> out{};
	put_u32(out, 0, row.active ? 1u : 0u);
	out[4] = row.class_id;
	put_u32(out, 8, float_bits(row.stealth));
	put_u32(out, 12, float_bits(row.hp_bonus));
	put_u32(out, 16, float_bits(row.mana_bonus));
	put_u32(out, 20, float_bits(row.recoil_mute));
	put_u32(out, 24, float_bits(row.reload_mute));
	put_u32(out, 28, float_bits(row.xhair_mute));
	put_u32(out, 32, float_bits(row.xhairdx_mute));
	put_u32(out, 36, float_bits(row.scope_mute));
	put_u32(out, 40, row.attributes);
	put_u32(out, 44, static_cast<uint32_t>(row.jungle_cammo));
	put_u32(out, 48, static_cast<uint32_t>(row.desert_cammo));
	put_u32(out, 52, static_cast<uint32_t>(row.arctic_cammo));
	put_u32(out, 56, static_cast<uint32_t>(row.run_modifier));
	return out;
}

int32_t cammo_of(const ClassRow &row, Property property) {
	switch (property) {
		case kJungleCammo: return row.jungle_cammo;
		case kDesertCammo: return row.desert_cammo;
		case kArcticCammo: return row.arctic_cammo;
		default: return 0;
	}
}

Property cammo_property_for_camouflage(int camouflage) {
	return camouflage == 1 ? kJungleCammo : camouflage == 2 ? kArcticCammo : kDesertCammo;
}

bool same_rows(const Table &a, const Table &b) {
	for (size_t i = 0; i < kClassCount; ++i)
		if (row_bytes(a.rows[i]) != row_bytes(b.rows[i])) return false;
	return true;
}

bool read_table(const uint8_t *data, size_t size, Table &out, Reading *reading) {
	// The whole table cleared first [orig: memset(g_CharAttr, 0, 0x7C0) @ 0x412168].
	out = Table{};
	if (reading != nullptr) *reading = Reading{};
	// ConfigFile_LoadFromFile fails on no file; a "CBIN" file takes its binary reader [orig:
	// ConfigFile_LoadFromFile @ 0x760a10], which no shipped charattr.def needs: read as failed here.
	if (data == nullptr || size == 0) return false;
	if (size >= 4 && std::memcmp(data, "CBIN", 4) == 0) return false;
	std::vector<configfile::ConfigSection> sections = configfile::parse_config_text(data, size);

	// CHARACTER1, CHARACTER2, ... until the first the file has no section of, sixteen at most
	// [orig: @ 0x4121a6 sprintf "CHARACTER%d", @ 0x4121b5 ConfigFile_FindSection, the break @ 0x4121bf,
	//  the loop's end @ 0x41245e].
	size_t classes = 0;
	for (size_t index = 0; index < kClassCount; ++index) {
		char label[32];
		std::snprintf(label, sizeof label, "CHARACTER%zu", index + 1);
		configfile::ConfigSection *section = configfile::find_config_section(sections, label);
		if (section == nullptr) break;
		ClassRow &row = out.rows[index];
		ClassSource *source = reading != nullptr ? &reading->sources[index] : nullptr;
		row.active = true; // [orig: @ 0x4121e0]
		if (source != nullptr) {
			source->read = true;
			source->section_offset = section->offset;
		}
		// Each key through ConfigFile_FindSectionAndReadValue, the section's cursor reset each time: its
		// first entry of the key, value 1, a float (type 2) or an integer (type 1); a missing key leaves
		// the field 0 (the accessor zeroes its output first) [orig: @ 0x4121e7..0x412353 ->
		// ConfigFile_FindSectionAndReadValue @0x75ff00 -> ConfigFile_ReadKeyValue @0x75fc90].
		for (const KeySpec &spec : kScalarKeys) {
			configfile::find_config_section(sections, label);
			bool read = false;
			if (spec.real) {
				read = configfile::read_config_value(*section, spec.key, 1, nullptr, real_field(row, spec.property), nullptr);
			} else {
				read = configfile::read_config_value(*section, spec.key, 1, nullptr, nullptr, integer_field(row, spec.property));
			}
			if (read && source != nullptr) source->values[spec.property] = source_of(*section, 1);
		}
		// ATTRIBUTES: its first value through the accessor, then each further value of the same entry, each
		// word's flag ORed in, a word the table does not have adding none [orig: @ 0x412365 FindSection,
		// @ 0x412381 ConfigFile_ReadKeyValue(index 1), @ 0x4123de ini_read_key_value_from_current_line from
		// index 2; the words as strings into a 0x7F-byte buffer: a number reads as "%d" or "%f"].
		configfile::find_config_section(sections, label);
		std::string word;
		if (configfile::read_config_value(*section, kAttributesKey, 1, &word, nullptr, nullptr)) {
			row.attributes |= attribute_flag(word);
			if (source != nullptr) {
				source->values[kAttributes] = source_of(*section, 1);
				source->attribute_words.push_back(source->values[kAttributes]);
			}
			for (int value = 2; configfile::read_current_config_value(*section, kAttributesKey, value, &word, nullptr, nullptr);
					++value) {
				row.attributes |= attribute_flag(word);
				if (source != nullptr) source->attribute_words.push_back(source_of(*section, value));
			}
		}
		row.class_id = static_cast<uint8_t>(index + 1); // [orig: @ 0x412451]
		++classes;
	}
	if (reading == nullptr) return true;
	reading->classes = classes;
	// The sections no lookup reads: each class's after the first the file lacks, a later section of a label,
	// and a section of any other label.
	std::vector<bool> found(sections.size(), false);
	for (size_t index = 0; index < classes; ++index) {
		char label[32];
		std::snprintf(label, sizeof label, "character%zu", index + 1);
		for (size_t s = 0; s < sections.size(); ++s)
			if (sections[s].label == label) {
				found[s] = true;
				break;
			}
	}
	for (size_t s = 0; s < sections.size(); ++s) {
		if (found[s]) continue;
		UnreadSection unread;
		unread.label = strutil::to_upper(sections[s].label);
		unread.offset = sections[s].offset;
		unread.class_id = label_class(sections[s].label);
		if (unread.class_id == 0) {
			unread.why = UnreadSection::Why::NotAClass;
		} else if (static_cast<size_t>(unread.class_id) <= classes) {
			unread.why = UnreadSection::Why::Repeated;
		} else {
			unread.why = UnreadSection::Why::AfterMissing;
		}
		reading->unread.push_back(std::move(unread));
	}
	return true;
}

} // namespace opennova::charattr
