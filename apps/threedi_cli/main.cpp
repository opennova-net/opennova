// opennova-3di — build a .3di from an authored scene, write one back out as
// a scene, inspect or compare models.
//
//   opennova-3di build   <scene.o3d> -o <out.3di>
//   opennova-3di scene   <model.3di> -o <scene.o3d>
//   opennova-3di info    <model.3di> [--verbose | --planes | --verts]
//   opennova-3di compare <expected.3di> <actual.3di>
//   opennova-3di catalog
//
// The DCC front ends (the Blender add-on under tools/blender/opennova_3di is
// the first one; ADR 0047) speak the .o3d scene text
// (docs/threedi/o3d-scene-format.md); every 3DI3 byte is read and written by
// the engine (formats/threedi), so there is one encoder and one decoder.
// Exit codes: 0 ok (compare: same model), 1 error (compare: differences),
// 2 usage.

#include <cstdio>
#include <cstring>
#include <string>

#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>
#include <runtime/renderer/material_descriptor.h>

#include "threedi_cli.h"

using namespace opennova::threedi;

namespace {

int usage(const char *why) {
	if (why != nullptr) std::fprintf(stderr, "opennova-3di: %s\n", why);
	std::fprintf(stderr,
			"usage: opennova-3di build   <scene.o3d> -o <out.3di>\n"
			"       opennova-3di scene   <model.3di> -o <scene.o3d>\n"
			"       opennova-3di info    <model.3di> [--verbose | --planes | --verts]\n"
			"       opennova-3di compare <expected.3di> <actual.3di>\n"
			"       opennova-3di catalog\n");
	return 2;
}

// The engine's CTRL register catalog, generator-style names and shader tags
// with their capability words, one per line (`register NAME`, `style CODE
// NAME`, `shader TAG 0xFLAGS`), so a front end offers exactly what the builder
// and the renderer know without keeping its own copy. The flag bits are
// runtime/renderer/material_descriptor.h's (BLENDING 0x1000 puts a strip in
// the alpha pass, GLASS 0x2000, TANGENT 0x8000).
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
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc < 2) return usage(nullptr);
	const std::string cmd = argv[1];
	if (cmd == "catalog") return argc == 2 ? cmd_catalog() : usage("catalog takes no arguments");
	if (argc < 3) return usage(nullptr);
	if (cmd == "info") {
		const std::string flag = argc > 3 ? argv[3] : "";
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
		if (argc != 4) return usage("compare needs <expected.3di> <actual.3di>");
		return threedi_cli::cmd_compare(argv[2], argv[3]);
	}
	return usage("unknown command");
}
