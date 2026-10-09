// The .adm name every entity / weapon spawn loads, the one rule of the one
// load [orig: AnimMap_LoadAdmFile @0x40cc40 -- an empty name returns no map
//  @0x40cca1 (the FP renderer then submits every bone with the root matrix
//  @0x4def88..0x4defcf); otherwise the name is cut at its last '.'
//  @0x40ccb9..0x40cccd and ".adm" appended @0x40ccea..0x40ccf8, and a
//  FileSystem_FileExists miss @0x40cd00 (loose search paths, the primary PFF,
//  the 16 secondaries @0x75aa50) replaces the WHOLE name with "default.adm"
//  (12 bytes from 0x7c3314 @0x40cd0c..0x40cd25) before AnimMap_FindByName
//  @0x40cd2f keys the cache on the RESOLVED name and File_ParseASCIIFile reads
//  it. Retail ships default.adm (every anim key -> default.bad) in the base
//  PFFs.]
// Callers: NapiNPClientMsg_FullEntitySpawn @0x433d86, Entity_SpawnFromItemDef
// @0x45257c, Entity_ReloadItemDefAndReinit @0x4a9f57 (each the item def's
// +0xC0 name as authored), Anim_InitActions @0x541fef (the weapon.def 'end'
// path, the block's animadm as authored), i.e. every entity and weapon spawn.

#pragma once

#include <string>

namespace opennova::anim {

inline constexpr const char *kDefaultAdmName = "default.adm";

// The file the load opens for an authored name: the name to its last '.' (all
// of it where it has none) with ".adm" appended, so "ak47_1st", "AK47_1st.ADM"
// and "ak47_1st.txt" all open "<stem>.adm". Empty for an empty name, which
// names no map.
inline std::string adm_file_name(const std::string &name) {
	if (name.empty()) return std::string();
	const size_t dot = name.find_last_of('.');
	return (dot == std::string::npos ? name : name.substr(0, dot)) + ".adm";
}

// The name the load keys its table on and reads: adm_file_name's file when
// the mounted roots carry it, else default.adm; empty (no map) for an empty
// name. `file_exists(file)` is the caller's FileSystem_FileExists answer (the
// resource index's has_file).
template <typename FileExists>
std::string adm_load_name(const std::string &name, FileExists &&file_exists) {
	const std::string file = adm_file_name(name);
	if (file.empty()) return file;
	return file_exists(file) ? file : std::string(kDefaultAdmName);
}

}  // namespace opennova::anim
