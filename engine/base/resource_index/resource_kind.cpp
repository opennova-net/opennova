#include <base/resource_index/resource_kind.h>

#include <filesystem>

#include <base/io/strutil.h>

namespace fs = std::filesystem;

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

std::string resource_extension_for_name(const std::string &name) {
	return strutil::to_lower(fs::path(name).extension().string());
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

std::string resource_kind_for_name_and_magic(const std::string &name, bool is_rtxt_bin,
                                             bool is_scr_bin) {
	const std::string extension = resource_extension_for_name(name);
	// Avatars.def is the singular player-character database. Matched by NAME, not
	// extension: the .def extension is
	// shared with weapon/items/ammo/hudpos.def, which the engine consumes by name at
	// runtime and which stay unbrowsable (like .dbf). [orig: CAvatarDefs_Init @ 0x57b180
	// opens "Avatars.def" by exact name]
	if (strutil::to_lower(fs::path(name).filename().string()) == "avatars.def") {
		return "avatar";
	}
	if (extension == ".bms" || extension == ".mis") {
		return "mission";
	}
	if (extension == ".trn") {
		return "terrain";
	}
	if (extension == ".env") {
		return "environment";
	}
	if (extension == ".3di") {
		return "object_model";
	}
	if (extension == ".kda") {
		return "credits";
	}
	if (extension == ".fnt") {
		return "font";
	}
	// The effect catalog is one grammar across three extensions. `.ptl` is the base
	// set; `.ptu` and `.ptg` are the US and German gore sets, parsed by the SAME
	// section callback and differing only in which one the runtime selects
	// [orig: CEffectSystem_Init @ 0x5f6070 matches an archive entry against ".ptl"
	// OR the selected extension @0x5f64f3 and parses both through
	// CEffectWorld_ParseSectionCallback @ 0x5ecb40; the loose `.ptu` leg's
	// CEffectWorld_LoadDefinitionFile @ 0x5ecf70 is a thin wrapper on the same
	// File_ParseASCIIFile + callback]. The index carries all three so the browser
	// and the loader can both see them; particle_extension() owns the SELECTION.
	if (extension == ".ptl" || extension == ".ptu" || extension == ".ptg") {
		return "particle";
	}
	if (extension == ".mnu") {
		return "menu";
	}
	if (extension == ".mns") {
		return "menu_style";
	}
	if (extension == ".sbf") {
		return "sbf";
	}
	if (extension == ".lwf") {
		return "sound";
	}
	// The .def family is name-keyed, not extension-keyed (items/weapon/ammo/avatars all share
	// .def and are consumed at runtime by name). Only hudpos.def is a browsable kind, for the
	// HUD layout catalog; the rest stay unclassified.
	if (extension == ".def" && strutil::to_lower(fs::path(name).filename().string()) == "hudpos.def") {
		return "hudpos";
	}
	// NOTE: .dbf (dialog bank) is intentionally NOT classified as a browsable kind.
	// It is consumed at runtime by name (DbfData); classifying it as an LWF sound
	// profile would conflate two unrelated formats.
	if (extension == ".bin" && is_scr_bin) {
		return "music_script";
	}
	if (extension == ".bin" && is_rtxt_bin) {
		return "strings";
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
