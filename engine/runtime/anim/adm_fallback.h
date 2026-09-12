// The .adm name substitution every entity / weapon spawn applies when the
// def-named animation map is not in any mounted root
// [orig: AnimMap_LoadAdmFile @0x40cc40 -- an empty name returns no map
//  @0x40cca1 (the FP renderer then submits every bone with the root matrix
//  @0x4def88..0x4defcf); otherwise the extension is stripped and ".adm"
//  appended @0x40ccb9..0x40ccf8, and a FileSystem_FileExists miss @0x40cd00
//  (loose search paths, the primary PFF, the 16 secondaries @0x75aa50)
//  replaces the WHOLE name with "default.adm" (12 bytes from 0x7c3314
//  @0x40cd0c..0x40cd25) before AnimMap_FindByName @0x40cd2f keys the cache on
//  the RESOLVED name and File_ParseASCIIFile reads it. Retail ships
//  default.adm (every anim key -> default.bad) in the base PFFs.]
// Callers: NapiNPClientMsg_FullEntitySpawn @0x433d86, Entity_SpawnFromItemDef
// @0x45257c, Entity_ReloadItemDefAndReinit @0x4a9f57, Anim_InitActions
// @0x541fef (the weapon.def 'end' path), i.e. every entity and weapon spawn.

#pragma once

#include <string>

namespace opennova::anim {

inline constexpr const char *kDefaultAdmName = "default.adm";

// The name to load: the def-named file when the mounted roots carry it, else
// default.adm. `named_file_exists` is the caller's FileSystem_FileExists
// answer for `adm_name` (the resource index's has_file).
inline std::string adm_name_or_default(const std::string &adm_name,
                                       bool named_file_exists) {
	return named_file_exists ? adm_name : std::string(kDefaultAdmName);
}

}  // namespace opennova::anim
