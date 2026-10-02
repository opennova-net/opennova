#include <base/resource_index/resource_kind.h>

#include <cstring>

#include <base/io/os_path.h>
#include <base/io/strutil.h>

namespace opennova {

namespace {

bool has_magic(const std::vector<uint8_t> &bytes, const char (&want)[5]) {
	return bytes.size() >= 4 &&
	       bytes[0] == want[0] &&
	       bytes[1] == want[1] &&
	       bytes[2] == want[2] &&
	       bytes[3] == want[3];
}

} // namespace

// The names are UTF-8 strings (the VFS's), taken apart as strings: a std::filesystem
// round trip reads them in the ANSI code page on Windows (base/io/os_path.h).
std::string resource_extension_for_name(const std::string &name) {
	const std::string file = io::utf8_file_name(name);
	const size_t dot = file.rfind('.');
	return (dot == std::string::npos || dot == 0) ? std::string() : strutil::to_lower(file.substr(dot));
}

bool resource_bin_has_rtxt_magic(const std::vector<uint8_t> &bytes) {
	return has_magic(bytes, "RTXT");
}

// MUS bytecode is stored in an .bin wrapper whose first four bytes are "SCR0"
// (the music script loader's SCR container). This disambiguates a music .bin
// from a localized-strings .bin (RTXT magic) and a raw .bin (neither).
bool resource_bin_has_scr_magic(const std::vector<uint8_t> &bytes) {
	return has_magic(bytes, "SCR0");
}

const std::vector<ResourceKindRule> &resource_kind_rules() {
	static const std::vector<ResourceKindRule> rules = {
	        // Avatars.def is the singular player-character database. Matched by NAME, not
	        // extension: the .def extension is
	        // shared with weapon/items/ammo/hudpos.def, which the engine consumes by name at
	        // runtime and which stay unbrowsable (like .dbf). [orig: CAvatarDefs_Init @ 0x57b180
	        // opens "Avatars.def" by exact name]
	        {"avatars.def", ".def", "", "avatar"},
	        // The .def family is name-keyed, not extension-keyed (items/weapon/ammo/avatars all
	        // share .def and are consumed at runtime by name). Only hudpos.def is a browsable kind,
	        // for the HUD layout catalog; the rest stay unclassified.
	        {"hudpos.def", ".def", "", "hudpos"},
	        // The game's one mission file [orig: Mission_LoadBMSFile @ 0x40f4e0; the mission list
	        // scans *.bms, *.npj and *.npz, MissionList_ScanAndBuildFromFiles @ 0x563170]. A `.mis`
	        // is the original mission editor's text, which the game never reads (no literal of it
	        // in the image): no kind here.
	        {"", ".bms", "", "mission"},
	        {"", ".trn", "", "terrain"},
	        {"", ".env", "", "environment"},
	        {"", ".3di", "", "object_model"},
	        {"", ".kda", "", "credits"},
	        {"", ".fnt", "", "font"},
	        // The effect catalog is one grammar across three extensions. `.ptl` is the base
	        // set; `.ptu` and `.ptg` are the US and German gore sets, parsed by the SAME
	        // section callback and differing only in which one the runtime selects
	        // [orig: CEffectSystem_Init @ 0x5f6070 matches an archive entry against ".ptl"
	        // OR the selected extension @0x5f64f3 and parses both through
	        // CEffectWorld_ParseSectionCallback @ 0x5ecb40; the loose `.ptu` leg's
	        // CEffectWorld_LoadDefinitionFile @ 0x5ecf70 is a thin wrapper on the same
	        // File_ParseASCIIFile + callback]. The index carries all three so the browser
	        // and the loader can both see them; particle_extension() owns the SELECTION.
	        {"", ".ptl", "", "particle"},
	        {"", ".ptu", "", "particle"},
	        {"", ".ptg", "", "particle"},
	        {"", ".mnu", "", "menu"},
	        {"", ".mns", "", "menu_style"},
	        {"", ".sbf", "", "sbf"},
	        {"", ".lwf", "", "sound"},
	        // NOTE: .dbf (dialog bank) is intentionally NOT classified as a browsable kind.
	        // It is consumed at runtime by name (DbfData); classifying it as an LWF sound
	        // profile would conflate two unrelated formats.
	        // A `.bin` by its content: the music script's SCR container, then the RTXT
	        // string table (resource_bin_has_scr_magic, resource_bin_has_rtxt_magic).
	        {"", ".bin", "SCR0", "music_script"},
	        {"", ".bin", "RTXT", "strings"},
	};
	return rules;
}

std::string resource_kind_for_name_and_magic(const std::string &name, bool is_rtxt_bin,
                                             bool is_scr_bin) {
	const std::string extension = resource_extension_for_name(name);
	const std::string basename = strutil::to_lower(io::utf8_file_name(name));
	for (const ResourceKindRule &rule : resource_kind_rules()) {
		if (*rule.name) {
			if (basename == rule.name) {
				return rule.kind;
			}
			continue;
		}
		if (extension != rule.extension) {
			continue;
		}
		if (!*rule.magic) {
			return rule.kind;
		}
		// The two peeks the caller made of a `.bin`'s first four bytes.
		const bool scr = std::strcmp(rule.magic, "SCR0") == 0;
		if ((scr && is_scr_bin) || (!scr && std::strcmp(rule.magic, "RTXT") == 0 && is_rtxt_bin)) {
			return rule.kind;
		}
	}
	return "";
}

std::string resource_kind_for_file(const std::string &name, const std::vector<uint8_t> *bytes) {
	if (resource_extension_for_name(name) != ".bin" || bytes == nullptr) {
		return resource_kind_for_name_and_magic(name, false, false);
	}
	return resource_kind_for_name_and_magic(name, resource_bin_has_rtxt_magic(*bytes),
	                                        resource_bin_has_scr_magic(*bytes));
}

} // namespace opennova
