#include "texture_registry.h"

#include <base/io/strutil.h>

namespace opennova::renderer {

namespace {

// The key as the lookup compares it: GTexture_FindByName's stricmp folds ASCII letters
// alone [orig: GTexture_FindByName @ 0x687C7F]. A volume key starts with a NUL byte, so
// it never meets a 2D key (the volume list is its own, GStaticVB_FindByName @ 0x686B60).
std::string folded_key(std::string_view name, const char *suffix, bool volume = false) {
	std::string key = volume ? std::string(1, '\0') : std::string();
	key += strutil::to_lower(name);
	key += strutil::to_lower(suffix);
	return key;
}

} // namespace

// [orig: Material_LoadStageTexture @ 0x5B16F0 — types 0 and 2/8 through
//  Texture_LoadByNameWithChannel (@ 0x5B1742, @ 0x5B176E: "%s:%c" with channel 0 + '1'
//  @ 0x58B4AD), type 1 through Texture_LoadAndRegister (@ 0x5B1758: the same @ 0x58B7CD),
//  4/5 through Texture_LoadAsNormalMap (@ 0x5B1790: "%s:BA:%c" @ 0x58C4CE), 6 through
//  sub_58A580 (@ 0x5B17AD: "%s:HZ:%c" @ 0x58A5BA, the volume list), 7 through sub_58CE10
//  (@ 0x5B17CA: "%s:AO:%c" @ 0x58CE4A), 16 through sub_58F350 ("%s:NML" @ 0x58F399), 17
//  through sub_58F470 ("%s:HZ" @ 0x58F4B9, the volume list), 18 through
//  Texture_LoadTGAAlphaOverlay ("%s:AO" @ 0x58F5D9); the default @ 0x5B17F4 loads nothing]
std::string texture_registry_key(std::string_view name, uint8_t runtime_type) {
	if (name.empty()) return std::string();
	switch (runtime_type) {
	case 0:
	case 1:
	case 2:
	case 8: return folded_key(name, ":1");
	case 4:
	case 5: return folded_key(name, ":BA:1");
	case 6: return folded_key(name, ":HZ:1", true);
	case 7: return folded_key(name, ":AO:1");
	case 16: return folded_key(name, ":NML");
	case 17: return folded_key(name, ":HZ", true);
	case 18: return folded_key(name, ":AO");
	default: return std::string();
	}
}

} // namespace opennova::renderer
