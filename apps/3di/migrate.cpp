// opennova-3di migrate: a Black Hawk Down GP model (GPM, GPS, GPP) written as the 3DI3 JO and
// OpenNova load. The reader and the migration are the engine's (formats/threedi_gp); this
// command opens the file, looks a bump material's .mdt normal map up beside it, prints what
// the migration could not carry exactly as `path: note:` lines and writes the model whole or
// not at all.

#include <cstdio>
#include <vector>

#include <base/io/file_io.h>
#include <formats/threedi_gp/threedi_gp.h>
#include <formats/threedi_gp/threedi_gp_migrate.h>

#include "threedi_cli.h"

namespace opennova::threedi_cli {

int cmd_migrate(const char *gp_path, const char *out_path, int region) {
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::io::read_file_bytes(gp_path, bytes, error)) {
		std::fprintf(stderr, "opennova-3di: cannot read %s: %s\n", gp_path, error.c_str());
		return 1;
	}
	opennova::threedi_gp::File file;
	if (opennova::threedi_gp::detect(bytes.data(), bytes.size()) == opennova::threedi_gp::Kind::None) {
		std::fprintf(stderr, "%s: not a GP model (a 3DI3 needs no migration)\n", gp_path);
		return 1;
	}
	if (!opennova::threedi_gp::parse(bytes.data(), bytes.size(), file, error)) {
		std::fprintf(stderr, "%s: %s\n", gp_path, error.c_str());
		return 1;
	}
	opennova::threedi_gp::MigrateOptions options;
	options.region = region;
	options.texture_exists = opennova::threedi_gp::names_beside(gp_path);
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
