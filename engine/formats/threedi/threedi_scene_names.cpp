#include <formats/threedi/threedi_scene_names.h>

#include <cstdio>
#include <cstring>

namespace opennova::threedi {

namespace {

// Two decimal digits -> ordinal (1-based in the name, 0-based out). False when
// the two characters are not both digits or the value is 0.
bool parse_two_digits(std::string_view s, size_t at, int &index_out) {
	if (s.size() < at + 2) return false;
	const char a = s[at], b = s[at + 1];
	if (a < '0' || a > '9' || b < '0' || b > '9') return false;
	const int value = (a - '0') * 10 + (b - '0');
	if (value == 0) return false;
	index_out = value - 1;
	return true;
}

std::string two_digits(int index) {
	char buf[8];
	std::snprintf(buf, sizeof(buf), "%02d", index + 1);
	return buf;
}

// Prefixed two-digit names ("PN01", "LP03"): the prefix, exactly two digits,
// then either the end or a space (a label follows).
bool parse_prefixed(std::string_view name, std::string_view prefix, int &index_out, bool allow_label) {
	if (name.size() < prefix.size() + 2 || name.substr(0, prefix.size()) != prefix) return false;
	int index = 0;
	if (!parse_two_digits(name, prefix.size(), index)) return false;
	const size_t rest = prefix.size() + 2;
	if (rest == name.size()) {
		index_out = index;
		return true;
	}
	if (allow_label && name[rest] == ' ') {
		index_out = index;
		return true;
	}
	return false;
}

struct CollidableCode {
	int32_t type;
	const char code[3];
};

// The collidable type -> two-letter code table of the naming contract.
constexpr CollidableCode kCollidableCodes[] = {
	{1, "CB"}, {2, "CS"}, {3, "CC"}, {4, "CL"}, {5, "CV"}, {6, "CA"}, {7, "VC"}, {8, "BB"}, {9, "CD"},
	{10, "CT"}, {11, "CM"}, {12, "VK"}, {13, "CF"}, {14, "LP"}, {16, "DH"}, {17, "DM"}, {18, "DL"},
	{19, "CP"},
};

constexpr int32_t kBlinkBoxType = 8;
constexpr char kBlinkFlagLetters[] = {'V', 'S', 'W', 'L', 'O'}; // bits 1..5

std::string base26_suffix(int duplicate_ordinal) {
	// 1 -> "a", 26 -> "z", 27 -> "aa" (bijective base 26).
	std::string out;
	int n = duplicate_ordinal;
	while (n > 0) {
		const int rem = (n - 1) % 26;
		out.insert(out.begin(), static_cast<char>('a' + rem));
		n = (n - 1) / 26;
	}
	return out;
}

} // namespace

std::string threedi_scene_part_name(int part_index) { return "PN" + two_digits(part_index); }

bool threedi_scene_parse_part_name(std::string_view name, int &part_index) {
	return parse_prefixed(name, "PN", part_index, false);
}

std::string threedi_scene_bone_name(int part_index) { return "BN" + two_digits(part_index); }

bool threedi_scene_parse_bone_name(std::string_view name, int &part_index) {
	return parse_prefixed(name, "BN", part_index, true);
}

std::string threedi_scene_mesh_name(int part_index, int mesh_ordinal) {
	return two_digits(part_index) + " Mesh" + std::to_string(mesh_ordinal);
}

bool threedi_scene_parse_mesh_name(std::string_view name, int &part_index, int &mesh_ordinal) {
	int part = 0;
	if (!parse_two_digits(name, 0, part)) return false;
	static constexpr std::string_view kMesh = " Mesh";
	if (name.size() < 2 + kMesh.size() + 1 || name.substr(2, kMesh.size()) != kMesh) return false;
	int ordinal = 0;
	for (size_t i = 2 + kMesh.size(); i < name.size(); ++i) {
		if (name[i] < '0' || name[i] > '9') return false;
		ordinal = ordinal * 10 + (name[i] - '0');
	}
	part_index = part;
	mesh_ordinal = ordinal;
	return true;
}

std::string threedi_scene_lod_name(int lod_index) { return "LOD" + std::to_string(lod_index); }

bool threedi_scene_parse_lod_name(std::string_view name, int &lod_index) {
	if (name.size() < 4 || name.substr(0, 3) != "LOD") return false;
	int value = 0;
	for (size_t i = 3; i < name.size(); ++i) {
		if (name[i] < '0' || name[i] > '9') return false;
		value = value * 10 + (name[i] - '0');
	}
	lod_index = value;
	return true;
}

bool threedi_scene_user_point_name(int32_t point_type, int part_index, std::string_view label,
		std::string &out) {
	if (point_type < 'A' || point_type > 'Z') return false;
	out = "UP";
	out += static_cast<char>(point_type);
	out += two_digits(part_index);
	if (!label.empty()) {
		out += ' ';
		out.append(label.data(), label.size());
	}
	return true;
}

bool threedi_scene_parse_user_point_name(std::string_view name, int32_t &point_type, int &part_index,
		std::string &label) {
	if (name.size() < 5 || name.substr(0, 2) != "UP") return false;
	const char c = name[2];
	if (c < 'A' || c > 'Z') return false;
	int part = 0;
	if (!parse_two_digits(name, 3, part)) return false;
	if (name.size() == 5) {
		label.clear();
	} else {
		if (name[5] != ' ') return false;
		label.assign(name.data() + 6, name.size() - 6);
	}
	point_type = c;
	part_index = part;
	return true;
}

std::string threedi_scene_light_name(int light_index) { return "LP" + two_digits(light_index); }

bool threedi_scene_parse_light_name(std::string_view name, int &light_index) {
	return parse_prefixed(name, "LP", light_index, false);
}

std::string threedi_scene_collision_section_name(int section_index) { return "CO" + two_digits(section_index); }

bool threedi_scene_parse_collision_section_name(std::string_view name, int &section_index) {
	return parse_prefixed(name, "CO", section_index, false);
}

std::string threedi_scene_collision_volume_name(int32_t collidable_type, int32_t flags, int ordinal,
		int duplicate_ordinal) {
	std::string out = "CX";
	for (const CollidableCode &entry : kCollidableCodes) {
		if (entry.type == collidable_type) {
			out = entry.code;
			break;
		}
	}
	if (collidable_type == kBlinkBoxType) {
		for (int bit = 1; bit <= 5; ++bit) {
			if ((flags & (1 << bit)) == 0) out += kBlinkFlagLetters[bit - 1];
		}
	}
	out += two_digits(ordinal);
	if (duplicate_ordinal > 0) out += base26_suffix(duplicate_ordinal);
	out += "-colonly";
	return out;
}

bool threedi_scene_parse_collision_volume_name(std::string_view name, int32_t &collidable_type,
		int32_t &flags, int &ordinal) {
	static constexpr std::string_view kSuffix = "-colonly";
	if (name.size() < 2 + 2 + kSuffix.size()) return false;
	if (name.substr(name.size() - kSuffix.size()) != kSuffix) return false;
	return threedi_scene_parse_collision_volume_stem(name.substr(0, name.size() - kSuffix.size()),
			collidable_type, flags, ordinal);
}

bool threedi_scene_parse_collision_volume_stem(std::string_view stem, int32_t &collidable_type,
		int32_t &flags, int &ordinal) {
	if (stem.size() < 4) return false;
	std::string_view body = stem;
	const std::string_view code = body.substr(0, 2);
	int32_t type = -1;
	for (const CollidableCode &entry : kCollidableCodes) {
		if (code == entry.code) {
			type = entry.type;
			break;
		}
	}
	if (type < 0 && code != "CX") return false;
	body = body.substr(2);
	int32_t parsed_flags = 0;
	if (type == kBlinkBoxType) {
		// Every clear bit is named; the bits without a letter are set.
		parsed_flags = 0x3E;
		while (!body.empty() && body[0] >= 'A' && body[0] <= 'Z') {
			bool known = false;
			for (int bit = 1; bit <= 5; ++bit) {
				if (body[0] == kBlinkFlagLetters[bit - 1]) {
					parsed_flags &= ~(1 << bit);
					known = true;
				}
			}
			if (!known) return false;
			body = body.substr(1);
		}
	}
	int index = 0;
	if (!parse_two_digits(body, 0, index)) return false;
	body = body.substr(2);
	for (const char c : body) {
		if (c < 'a' || c > 'z') return false; // the duplicate suffix, if any
	}
	collidable_type = type;
	flags = parsed_flags;
	ordinal = index;
	return true;
}

bool threedi_scene_is_occlusion_name(std::string_view name) {
	static constexpr std::string_view kSuffix = "-oconly";
	return name.size() >= kSuffix.size() && name.substr(name.size() - kSuffix.size()) == kSuffix;
}

bool threedi_scene_has_dedup_suffix(std::string_view name) {
	const size_t dot = name.rfind('.');
	if (dot == std::string_view::npos || dot + 1 >= name.size()) return false;
	for (size_t i = dot + 1; i < name.size(); ++i) {
		if (name[i] < '0' || name[i] > '9') return false;
	}
	return true;
}

} // namespace opennova::threedi
