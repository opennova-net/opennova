// opennova-3di migrate: a Black Hawk Down GP model (GPM, GPS, GPP) written as the 3DI3 JO and
// OpenNova load. The reader and the migration are the engine's (formats/threedi_gp); this
// command opens the file, looks a bump material's .mdt normal map up beside it, prints what
// the migration could not carry exactly as `path: note:` lines and writes the model whole or
// not at all.

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

#include <base/io/strutil.h>
#include <formats/threedi_gp/threedi_gp.h>
#include <formats/threedi_gp/threedi_gp_migrate.h>

#include "threedi_cli.h"

namespace opennova::threedi_cli {

int cmd_migrate(const char *gp_path, const char *out_path, int region) {
	std::vector<uint8_t> bytes;
	{
		FILE *f = std::fopen(gp_path, "rb");
		if (f == nullptr) {
			std::fprintf(stderr, "opennova-3di: cannot open %s\n", gp_path);
			return 1;
		}
		uint8_t chunk[65536];
		size_t n = 0;
		while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) bytes.insert(bytes.end(), chunk, chunk + n);
		std::fclose(f);
	}
	opennova::threedi_gp::File file;
	std::string error;
	if (opennova::threedi_gp::detect(bytes.data(), bytes.size()) == opennova::threedi_gp::Kind::None) {
		std::fprintf(stderr, "%s: not a GP model (a 3DI3 needs no migration)\n", gp_path);
		return 1;
	}
	if (!opennova::threedi_gp::parse(bytes.data(), bytes.size(), file, error)) {
		std::fprintf(stderr, "%s: %s\n", gp_path, error.c_str());
		return 1;
	}
	// The texture files beside the model, by name without case (a retail tree mixes cases).
	const std::filesystem::path folder = std::filesystem::path(gp_path).parent_path();
	opennova::threedi_gp::MigrateOptions options;
	options.region = region;
	options.texture_exists = [&folder](const std::string &name) {
		std::error_code ec;
		const std::filesystem::path dir = folder.empty() ? std::filesystem::path(".") : folder;
		for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
			if (strutil::iequals(entry.path().filename().string(), name)) return true;
		return false;
	};
	std::vector<uint8_t> out;
	std::vector<opennova::threedi_gp::MigrateNote> notes;
	if (!opennova::threedi_gp::migrate(file, out, notes, error, options)) {
		std::fprintf(stderr, "%s: %s\n", gp_path, error.c_str());
		return 1;
	}
	for (const opennova::threedi_gp::MigrateNote &n : notes)
		std::fprintf(stderr, "%s: note: %s (%d)\n", gp_path, n.text.c_str(), n.count);
	if (!write_output(out_path, out.data(), out.size())) return 1;
	std::printf("wrote %s (%zu bytes)\n", out_path, out.size());
	return 0;
}

} // namespace opennova::threedi_cli
