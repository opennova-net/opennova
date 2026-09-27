// opennova-3di — build a .3di from an authored scene, write one back out as
// a scene, inspect or compare models.
//
//   opennova-3di build   <scene.o3d> -o <out.3di>
//   opennova-3di scene   <model.3di> -o <scene.o3d>
//   opennova-3di info    <model.3di> [--verbose | --planes | --verts]
//   opennova-3di compare [--strict] <expected.3di> <actual.3di>
//   opennova-3di anim    build|scene|info|compare  (the .bad/.adm clip set)
//   opennova-3di weapon  timing|merge  (the weapon.def keys the clips need)
//   opennova-3di catalog
//
// The DCC front ends (the Blender add-on under tools/blender/opennova_3di is
// the first one; ADR 0047) speak the .o3d scene text
// (docs/threedi/o3d-scene-format.md) for a model and the .o3a clip-set text
// (docs/anim/o3a-scene-format.md) for its animations; every 3DI3, .bad and
// .adm byte is read and written by the engine (formats/threedi, formats/bad,
// formats/adm), so there is one encoder and one decoder.
// Exit codes: 0 ok (compare: same model, drift notes allowed), 1 error
// (compare: a difference, or a file it cannot read or that is malformed;
// with --strict, drift too), 2 usage.

#include <cstdio>
#include <cstring>
#include <string>

#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>
#include <runtime/anim/anim_event_bits.h>
#include <runtime/renderer/material_descriptor.h>
#include <runtime/world/infantry.h>
#include <runtime/world/weapon_fsm.h>

#include "anim_cli.h"
#include "threedi_cli.h"
#include "weapon_timing.h"

using namespace opennova::threedi;

namespace {

int usage(const char *why) {
	if (why != nullptr) std::fprintf(stderr, "opennova-3di: %s\n", why);
	std::fprintf(stderr,
			"usage: opennova-3di build   <scene.o3d> -o <out.3di>\n"
			"       opennova-3di scene   <model.3di> -o <scene.o3d>\n"
			"       opennova-3di info    <model.3di> [--verbose | --planes | --verts]\n"
			"       opennova-3di compare [--strict] <expected.3di> <actual.3di>\n"
			"       opennova-3di anim build   <set.o3a> -o <out.adm|out.bad>\n"
			"       opennova-3di anim scene   <in.adm|in.bad> -o <set.o3a>\n"
			"       opennova-3di anim info    <in.adm|in.bad> [--verbose | --keys]\n"
			"       opennova-3di anim compare <expected.adm|.bad> <actual.adm|.bad>\n"
			"       opennova-3di weapon timing <timing.txt> -o <edits.txt>\n"
			"       opennova-3di weapon merge  <weapon.def> <edits.txt> -o <out.def>\n"
			"       opennova-3di catalog\n");
	return 2;
}

// The engine's CTRL register catalog, generator-style names and shader tags
// with their capability words, the anim slot keys a table row can name, the
// weapon actions an author times and the event trigger bits the runtime
// consumes, one per line (`register NAME`, `style CODE NAME`, `shader TAG
// 0xFLAGS`, `animslot KEY`, `weaponaction SUFFIX KEY`, `trigger 0xMASK NAME`),
// so a front end offers exactly what the builder and the runtime know without
// keeping its own copy. The shader flag bits are
// runtime/renderer/material_descriptor.h's (BLENDING 0x1000 puts a strip in
// the alpha pass, GLASS 0x2000, TANGENT 0x8000); the slot keys are all 252 of
// the slot table a row's key is looked up in (a row naming none registers
// nothing) [orig: AnimMap_FindSlotByName @0x40cfa0 over g_AnimStateNameTable
// @0x8135F0]; the weapon actions are the ones with a slot of their own, each
// with its slot's key (runtime/world/weapon_fsm.h); the trigger bits are
// runtime/anim/anim_event_bits.h's.
int cmd_catalog() {
	for (size_t i = 0; i < static_cast<size_t>(THREEDI_CTRL_REGISTER_COUNT); ++i) {
		const char *name = threedi_ctrl_register_name(i);
		if (name != nullptr && name[0] != '\0') std::printf("register %s\n", name);
	}
	for (int code = 0; code < 256; ++code) {
		const ThreediControlFuncInfo *info = threedi_control_func_info(static_cast<uint8_t>(code));
		if (info != nullptr && info->name != nullptr) std::printf("style %d %s\n", code, info->name);
	}
	for (const opennova::renderer::MaterialDescriptorRecord &d : opennova::renderer::kMaterialDescriptorTable)
		std::printf("shader %s 0x%x\n", d.name, static_cast<unsigned>(d.shader_flags));
	for (int slot = 0; slot < opennova::world::kInfantryAnimStateCount; ++slot)
		std::printf("animslot %s\n", opennova::world::infantry_anim_key(slot).c_str());
	for (int32_t action = 0; action < opennova::world::weapon_action::kCount; ++action) {
		const int32_t slot = opennova::world::weapon_action_anim_slot(action);
		if (slot >= 0)
			std::printf("weaponaction %s %s\n", opennova::world::kWeaponActionSuffixes[action],
					opennova::world::infantry_anim_key(slot).c_str());
	}
	for (const opennova::anim::AnimEventBit &bit : opennova::anim::kAnimEventBits)
		std::printf("trigger 0x%x %s\n", static_cast<unsigned>(bit.mask), bit.name);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) return usage(nullptr);
	const std::string cmd = argv[1];
	if (cmd == "catalog") return argc == 2 ? cmd_catalog() : usage("catalog takes no arguments");
	if (argc < 3) return usage(nullptr);
	if (cmd == "weapon") {
		const std::string sub = argv[2];
		if (sub == "timing") {
			if (argc != 6 || std::strcmp(argv[4], "-o") != 0)
				return usage("weapon timing needs <timing.txt> -o <edits.txt>");
			return threedi_cli::cmd_weapon_timing(argv[3], argv[5]);
		}
		if (sub == "merge") {
			if (argc != 7 || std::strcmp(argv[5], "-o") != 0)
				return usage("weapon merge needs <weapon.def> <edits.txt> -o <out.def>");
			return threedi_cli::cmd_weapon_merge(argv[3], argv[4], argv[6]);
		}
		return usage("weapon takes timing or merge");
	}
	if (cmd == "info") {
		const std::string flag = argc > 3 ? argv[3] : "";
		if (argc > 4 || (!flag.empty() && flag != "--verbose" && flag != "--planes" && flag != "--verts"))
			return usage("info takes --verbose, --planes or --verts");
		const int verbose = flag == "--verts" ? 3 : flag == "--planes" ? 2 : flag == "--verbose" ? 1 : 0;
		return threedi_cli::cmd_info(argv[2], verbose);
	}
	if (cmd == "build") {
		if (argc != 5 || std::strcmp(argv[3], "-o") != 0) return usage("build needs <scene.o3d> -o <out.3di>");
		return threedi_cli::cmd_build(argv[2], argv[4]);
	}
	if (cmd == "scene") {
		if (argc != 5 || std::strcmp(argv[3], "-o") != 0) return usage("scene needs <model.3di> -o <scene.o3d>");
		return threedi_cli::cmd_scene(argv[2], argv[4]);
	}
	if (cmd == "compare") {
		const bool strict = argc == 5 && std::strcmp(argv[2], "--strict") == 0;
		if (argc != (strict ? 5 : 4)) return usage("compare needs [--strict] <expected.3di> <actual.3di>");
		return threedi_cli::cmd_compare(argv[strict ? 3 : 2], argv[strict ? 4 : 3], strict);
	}
	if (cmd == "anim") {
		const std::string sub = argv[2];
		if (sub == "info") {
			if (argc < 4) return usage("anim info needs <in.adm|in.bad>");
			const std::string flag = argc > 4 ? argv[4] : "";
			if (argc > 5 || (!flag.empty() && flag != "--verbose" && flag != "--keys"))
				return usage("anim info takes --verbose or --keys");
			const int verbose = flag == "--keys" ? 2 : flag == "--verbose" ? 1 : 0;
			return threedi_cli::cmd_anim_info(argv[3], verbose);
		}
		if (sub == "build") {
			if (argc != 6 || std::strcmp(argv[4], "-o") != 0)
				return usage("anim build needs <set.o3a> -o <out.adm|out.bad>");
			return threedi_cli::cmd_anim_build(argv[3], argv[5]);
		}
		if (sub == "scene") {
			if (argc != 6 || std::strcmp(argv[4], "-o") != 0)
				return usage("anim scene needs <in.adm|in.bad> -o <set.o3a>");
			return threedi_cli::cmd_anim_scene(argv[3], argv[5]);
		}
		if (sub == "compare") {
			if (argc != 5) return usage("anim compare needs <expected> <actual>");
			return threedi_cli::cmd_anim_compare(argv[3], argv[4]);
		}
		return usage("anim takes build, scene, info or compare");
	}
	return usage("unknown command");
}
