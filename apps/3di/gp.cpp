// opennova-3di gp: a 3DI3 model (a .3di, or an .o3d scene built first) written as the GP model
// Black Hawk Down loads, the inverse of `migrate`. The lowering and the writer are the engine's
// (formats/threedi_gp); this command opens the file, names the model after the output file as
// BHD's model tool does, reads each texture's size from the file beside the model, prints what
// the lowering could not carry exactly as `path: note:` lines and writes the model whole or not
// at all.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_o3d_lower.h>
#include <formats/threedi_gp/threedi_gp.h>
#include <formats/threedi_gp/threedi_gp_lower.h>
#include <formats/threedi_gp/threedi_gp_write.h>
#include <runtime/renderer/model_target.h>

#include "threedi_cli.h"

namespace opennova::threedi_cli {
namespace {

namespace fs = std::filesystem;

// A TGA's or DDS's size in texels from its header.
bool image_size(const fs::path &path, uint16_t &width, uint16_t &height) {
	std::ifstream f(path, std::ios::binary);
	uint8_t h[20] = {};
	if (!f.read(reinterpret_cast<char *>(h), sizeof(h))) return false;
	auto u16 = [&](int at) { return static_cast<uint16_t>(h[at] | (h[at + 1] << 8)); };
	if (h[0] == 'D' && h[1] == 'D' && h[2] == 'S' && h[3] == ' ') {
		height = u16(12);
		width = u16(16);
	} else {
		width = u16(12);
		height = u16(14);
	}
	return width != 0 && height != 0;
}

} // namespace

int cmd_gp(const char *in_path, const char *out_path) {
	std::vector<uint8_t> bytes;
	std::string error;
	const fs::path in = opennova::io::os_path(in_path);
	if (in.extension() == ".o3d") {
		std::ifstream scene(in);
		if (!scene) {
			std::fprintf(stderr, "opennova-3di: cannot open %s\n", in_path);
			return 1;
		}
		std::vector<opennova::threedi::SceneFinding> findings;
		const bool built =
				opennova::threedi::threedi_o3d_build(scene, opennova::renderer::retail_model_target(), bytes, findings);
		if (!print_findings(in_path, findings) || !built) return 1;
	} else if (!opennova::io::read_file_bytes(in_path, bytes, error)) {
		std::fprintf(stderr, "opennova-3di: cannot read %s: %s\n", in_path, error.c_str());
		return 1;
	}
	if (opennova::threedi_gp::detect(bytes.data(), bytes.size()) != opennova::threedi_gp::Kind::None) {
		std::fprintf(stderr, "%s: already a GP model\n", in_path);
		return 1;
	}
	opennova::threedi::Threedi3di3 model{};
	if (opennova::threedi::threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) != 0) {
		std::fprintf(stderr, "%s: not a 3DI3 model this tool reads\n", in_path);
		return 1;
	}
	opennova::threedi_gp::LowerOptions options;
	// The model is named after the file it is written to, up to its extension and at most 15
	// characters [orig: GP_WriteModel @ 0x448A94..0x448AD1 (df4oed)].
	options.name = opennova::io::utf8_path(opennova::io::os_path(out_path).stem()).substr(0, 15);
	const fs::path folders[2] = {in.parent_path(), opennova::io::os_path(out_path).parent_path()};
	options.texture_size = [&](const std::string &name, uint16_t &w, uint16_t &h) {
		const fs::path file = opennova::io::os_path(name);
		for (const fs::path &dir : folders)
			for (const fs::path &candidate : {file, fs::path(file).replace_extension(".dds"), fs::path(file).replace_extension(".tga")})
				if (image_size((dir.empty() ? fs::path(".") : dir) / candidate, w, h)) return true;
		return false;
	};
	opennova::threedi_gp::File gp;
	std::vector<opennova::threedi_gp::MigrateNote> notes;
	const bool lowered = opennova::threedi_gp::lower(model, gp, notes, error, options);
	opennova::threedi::threedi_3di3_free(&model);
	if (!lowered) {
		std::fprintf(stderr, "%s: %s\n", in_path, error.c_str());
		return 1;
	}
	std::vector<uint8_t> out;
	if (!opennova::threedi_gp::write(gp, out, error)) {
		std::fprintf(stderr, "%s: %s\n", in_path, error.c_str());
		return 1;
	}
	for (const opennova::threedi_gp::MigrateNote &n : notes)
		std::fprintf(stderr, "%s: note: %s (%d)\n", in_path, n.text.c_str(), n.count);
	if (!write_output(out_path, out.data(), out.size())) return 1;
	std::printf("wrote %s (%zu bytes)\n", out_path, out.size());
	return 0;
}

} // namespace opennova::threedi_cli
