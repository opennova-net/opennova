// opennova-3di build: mint a .3di from the .o3d scene text a DCC exporter
// writes (the Blender add-on under tools/blender/opennova_3di is the first
// one). The reader and the mint are the engine's
// (formats/threedi/threedi_o3d_read.h); this command opens the file, prints
// the findings as `path:line: message` and writes the model whole or not at
// all.

#include <cstdio>
#include <fstream>
#include <vector>

#include <formats/threedi/threedi_o3d_read.h>
// The shader table, and the texture-name cut the loader applies.
#include <runtime/renderer/material_descriptor.h>
#include <runtime/renderer/material_texture.h>

#include "threedi_cli.h"

using namespace opennova::threedi;

namespace opennova::threedi_cli {

int cmd_build(const char *scene_path, const char *out_path) {
	std::ifstream file(scene_path);
	if (!file) {
		std::fprintf(stderr, "opennova-3di: cannot open %s\n", scene_path);
		return 1;
	}
	std::vector<opennova::threedi::SceneFinding> findings;
	std::vector<uint8_t> bytes;
	const bool built = opennova::threedi::threedi_o3d_build(file, opennova::renderer::material_descriptor_tangent_lookup,
			opennova::renderer::material_texture_dds_only, bytes, findings);
	if (!print_findings(scene_path, findings) || !built) return 1;
	if (!write_output(out_path, bytes.data(), bytes.size())) return 1;
	std::printf("wrote %s (%zu bytes)\n", out_path, bytes.size());
	return 0;
}

} // namespace opennova::threedi_cli
