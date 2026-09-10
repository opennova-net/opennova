// The scene naming contract (docs/threedi/scene-naming-contract.md): format
// and parse are inverses, identity lives in the digits, DCC dedup suffixes
// are refused, the blink-box flag letters round-trip.
#include <cstdint>
#include <string>

#include "common/test_expect.h"
#include <formats/threedi/threedi_scene_names.h>

using namespace opennova::threedi;

int main() {
	int index = -1, ordinal = -1;
	// Parts, bones (with and without a label), meshes, LODs, lights, sections.
	TEST_EXPECT(threedi_scene_part_name(0) == "PN01");
	TEST_EXPECT(threedi_scene_part_name(18) == "PN19");
	TEST_EXPECT(threedi_scene_parse_part_name("PN19", index) && index == 18);
	TEST_EXPECT(!threedi_scene_parse_part_name("PN00", index));
	TEST_EXPECT(!threedi_scene_parse_part_name("PN1", index));
	TEST_EXPECT(!threedi_scene_parse_part_name("PN01.001", index));
	TEST_EXPECT(threedi_scene_bone_name(13) == "BN14");
	TEST_EXPECT(threedi_scene_parse_bone_name("BN14", index) && index == 13);
	TEST_EXPECT(threedi_scene_parse_bone_name("BN01 Hips", index) && index == 0);
	TEST_EXPECT(!threedi_scene_parse_bone_name("BN01Hips", index));
	TEST_EXPECT(threedi_scene_mesh_name(4, 2) == "05 Mesh2");
	TEST_EXPECT(threedi_scene_parse_mesh_name("05 Mesh2", index, ordinal) && index == 4 && ordinal == 2);
	TEST_EXPECT(threedi_scene_parse_mesh_name("46 Mesh10", index, ordinal) && index == 45 && ordinal == 10);
	TEST_EXPECT(!threedi_scene_parse_mesh_name("05 Mesh", index, ordinal));
	TEST_EXPECT(!threedi_scene_parse_mesh_name("Mesh2", index, ordinal));
	TEST_EXPECT(threedi_scene_lod_name(0) == "LOD0");
	TEST_EXPECT(threedi_scene_parse_lod_name("LOD3", index) && index == 3);
	TEST_EXPECT(!threedi_scene_parse_lod_name("LOD", index));
	TEST_EXPECT(threedi_scene_light_name(0) == "LP01");
	TEST_EXPECT(threedi_scene_parse_light_name("LP02", index) && index == 1);
	TEST_EXPECT(threedi_scene_collision_section_name(2) == "CO03");
	TEST_EXPECT(threedi_scene_parse_collision_section_name("CO03", index) && index == 2);

	// User points: the type letter, the part, the optional label.
	std::string name, label;
	int32_t type = 0;
	TEST_EXPECT(threedi_scene_user_point_name(71, 13, "LOOK", name) && name == "UPG14 LOOK");
	TEST_EXPECT(threedi_scene_user_point_name(83, 15, "MFlash01", name) && name == "UPS16 MFlash01");
	TEST_EXPECT(threedi_scene_user_point_name(71, 0, "", name) && name == "UPG01");
	TEST_EXPECT(!threedi_scene_user_point_name(7, 0, "x", name));
	TEST_EXPECT(threedi_scene_parse_user_point_name("UPG14 LOOK", type, index, label) &&
			type == 71 && index == 13 && label == "LOOK");
	TEST_EXPECT(threedi_scene_parse_user_point_name("UPS01 sitex00d", type, index, label) &&
			type == 83 && index == 0 && label == "sitex00d");
	TEST_EXPECT(threedi_scene_parse_user_point_name("UPG01", type, index, label) && label.empty());
	TEST_EXPECT(!threedi_scene_parse_user_point_name("UPg01", type, index, label));
	TEST_EXPECT(!threedi_scene_parse_user_point_name("UPG01x", type, index, label));

	// Collision volumes: codes, blink-box letters, duplicate suffixes.
	int32_t flags = 0;
	TEST_EXPECT(threedi_scene_collision_volume_name(1, 0, 0, 0) == "CB01-colonly");
	TEST_EXPECT(threedi_scene_collision_volume_name(19, 0, 1, 0) == "CP02-colonly");
	TEST_EXPECT(threedi_scene_collision_volume_name(15, 0, 0, 0) == "CX01-colonly");
	TEST_EXPECT(threedi_scene_collision_volume_name(1, 0, 0, 1) == "CB01a-colonly");
	TEST_EXPECT(threedi_scene_collision_volume_name(1, 0, 0, 27) == "CB01aa-colonly");
	// Blink box: clear bits 1, 2, 5 -> V S O; 0x2E clears only bit 4 -> L.
	TEST_EXPECT(threedi_scene_collision_volume_name(8, 0x3E & ~0x26, 2, 0) == "BBVSO03-colonly");
	TEST_EXPECT(threedi_scene_collision_volume_name(8, 0x2E, 0, 0) == "BBL01-colonly");
	TEST_EXPECT(threedi_scene_collision_volume_name(8, 0x3E, 0, 0) == "BB01-colonly");
	TEST_EXPECT(threedi_scene_parse_collision_volume_name("CB01-colonly", type, flags, ordinal) &&
			type == 1 && flags == 0 && ordinal == 0);
	TEST_EXPECT(threedi_scene_parse_collision_volume_name("CB01a-colonly", type, flags, ordinal) &&
			type == 1 && ordinal == 0);
	TEST_EXPECT(threedi_scene_parse_collision_volume_name("BBVSO03-colonly", type, flags, ordinal) &&
			type == 8 && flags == (0x3E & ~0x26) && ordinal == 2);
	TEST_EXPECT(threedi_scene_parse_collision_volume_name("BBL01-colonly", type, flags, ordinal) &&
			type == 8 && flags == 0x2E);
	TEST_EXPECT(threedi_scene_parse_collision_volume_name("CX01-colonly", type, flags, ordinal) && type == -1);
	TEST_EXPECT(!threedi_scene_parse_collision_volume_name("CB01", type, flags, ordinal));
	// The importer-converted stem (Godot strips its own -colonly hint).
	TEST_EXPECT(threedi_scene_parse_collision_volume_stem("CB01", type, flags, ordinal) && type == 1 && ordinal == 0);
	TEST_EXPECT(threedi_scene_parse_collision_volume_stem("BBL02", type, flags, ordinal) && type == 8 && flags == 0x2E && ordinal == 1);
	TEST_EXPECT(!threedi_scene_parse_collision_volume_stem("CO01", type, flags, ordinal));
	TEST_EXPECT(!threedi_scene_parse_collision_volume_stem("CB", type, flags, ordinal));
	TEST_EXPECT(!threedi_scene_parse_collision_volume_name("ZZ01-colonly", type, flags, ordinal));
	TEST_EXPECT(!threedi_scene_parse_collision_volume_name("BBQ01-colonly", type, flags, ordinal));

	// Occlusion and dedup suffixes.
	TEST_EXPECT(threedi_scene_is_occlusion_name("OP02-03-oconly"));
	TEST_EXPECT(!threedi_scene_is_occlusion_name("CB01-colonly"));
	TEST_EXPECT(threedi_scene_has_dedup_suffix("PN01.001"));
	TEST_EXPECT(threedi_scene_has_dedup_suffix("Cube.12"));
	TEST_EXPECT(!threedi_scene_has_dedup_suffix("wall.tga"));
	TEST_EXPECT(!threedi_scene_has_dedup_suffix("PN01"));
	std::printf("OK: threedi_scene_names\n");
	return 0;
}
