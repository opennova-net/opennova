// The scene naming contract (docs/threedi/scene-naming-contract.md): the
// format-neutral node names an ordinary scene carries so a converter can pair
// its parts, bones, meshes, LODs, user points, lights and collision records
// with the 3DI3 records they stand for. Format on the way out, parse on the
// way in; identity lives ONLY in the zero-padded numbers (a DCC dedup suffix
// like ".001" is rejected, never reinterpreted). Godot-free so the contract is
// one ctest (tests/threedi/scene_names_test.cpp) and one owner.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace opennova::threedi {

// Every ordinal below is 0-based in C++ and 1-based, two-digit in the name.

// Parts: "PN01".
std::string threedi_scene_part_name(int part_index);
bool threedi_scene_parse_part_name(std::string_view name, int &part_index);

// Bones: "BN01", optionally followed by a space and a human label ("BN01 Hips").
std::string threedi_scene_bone_name(int part_index);
bool threedi_scene_parse_bone_name(std::string_view name, int &part_index);

// Part meshes: "01 Mesh0" (mesh ordinal 0-based, as authored).
std::string threedi_scene_mesh_name(int part_index, int mesh_ordinal);
bool threedi_scene_parse_mesh_name(std::string_view name, int &part_index, int &mesh_ordinal);

// LOD containers: "LOD0" (0-based, the file's RLOD order).
std::string threedi_scene_lod_name(int lod_index);
bool threedi_scene_parse_lod_name(std::string_view name, int &lod_index);

// User points: "UPcNN <label>" where c is the type word as a letter (71 'G'
// gameplay, 83 'S' effect), NN the owning part, and the label the USRP name.
// Formatting refuses a type outside 'A'..'Z'.
bool threedi_scene_user_point_name(int32_t point_type, int part_index, std::string_view label,
		std::string &out);
bool threedi_scene_parse_user_point_name(std::string_view name, int32_t &point_type, int &part_index,
		std::string &label);

// Lights: "LP01".
std::string threedi_scene_light_name(int light_index);
bool threedi_scene_parse_light_name(std::string_view name, int &light_index);

// Collision sections (one per COBJ): "CO01".
std::string threedi_scene_collision_section_name(int section_index);
bool threedi_scene_parse_collision_section_name(std::string_view name, int &section_index);

// Bounding volumes: "<TYPE>NN[<dup>]-colonly". TYPE is the two-letter code of
// the collidable type (unknown types "CX"); a blink box ("BB", type 8) carries
// the enabled-flag letters V S W L O for the CLEAR bits 1..5 of `flags` before
// NN; NN is the volume's 1-based ordinal inside its section; <dup> is the
// base-26 lowercase suffix of the 2nd and later otherwise-identical names
// (2nd "a", 27th "z", 28th "aa"). Parsing recovers the type and ordinal, and
// for a blink box the flag bits the letters name (other bits stay 0).
std::string threedi_scene_collision_volume_name(int32_t collidable_type, int32_t flags, int ordinal,
		int duplicate_ordinal);
bool threedi_scene_parse_collision_volume_name(std::string_view name, int32_t &collidable_type,
		int32_t &flags, int &ordinal);

// Occlusion records ("-oconly" suffix): recognised so a converter can refuse
// them explicitly while the occlusion scene form is undefined.
bool threedi_scene_is_occlusion_name(std::string_view name);

// A DCC deduplication suffix (".001", ".12"): ambiguous identity, always rejected.
bool threedi_scene_has_dedup_suffix(std::string_view name);

} // namespace opennova::threedi
