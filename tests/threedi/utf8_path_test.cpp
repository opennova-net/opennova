// A 3DI written and read again under a UTF-8 path whose name no ANSI code page spells. The
// narrow fopen took the path in the system's ANSI code page inside a process with no UTF-8
// manifest (the Godot editor, ObjectData.open_file), so such a path failed to open; the
// readers and writers open through io::fopen_utf8 (engine/CLAUDE.md's io rule).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#include <base/io/os_path.h>
#include <formats/threedi/threedi_3di3.h>

#include "common/temp_dir.h"
#include "common/test_paths.h"

using namespace opennova::threedi;

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
	if (condition) return;
	std::fprintf(stderr, "FAIL: %s\n", message);
	++failures;
}

} // namespace

int main() {
	Threedi3di3 model;
	std::memset(&model, 0, sizeof(model));
	const std::string fixture = test_paths_fixture("threedi/synth/crate.3di");
	if (threedi_3di3_read(fixture.c_str(), &model) != 0) {
		std::fprintf(stderr, "FAIL: the fixture %s reads\n", fixture.c_str());
		return 1;
	}

	test_temp::TempDir temp("threedi_utf8_path");
	// "jörg Ωμέγα": Latin-1, Greek; no single-byte ANSI page spells both.
	const std::string dir = temp.file("j\xC3\xB6rg \xCE\xA9\xCE\xBC\xCE\xAD\xCE\xB3\xCE\xB1");
	std::error_code ec;
	std::filesystem::create_directories(opennova::io::os_path(dir), ec);
	expect(!ec, "the UTF-8 directory is made");
	const std::string path = opennova::io::utf8_join(dir, "crate.3di");

	expect(threedi_3di3_write(path.c_str(), &model) == 0, "the writer opens a UTF-8 path");
	expect(std::filesystem::exists(opennova::io::os_path(path), ec),
			"the file lands under its UTF-8 name");

	Threedi3di3 again;
	std::memset(&again, 0, sizeof(again));
	expect(threedi_3di3_read(path.c_str(), &again) == 0, "the reader opens a UTF-8 path");
	expect(again.material_count == model.material_count && again.lod_count == model.lod_count,
			"the model read back is the model written");
	threedi_3di3_free(&again);
	threedi_3di3_free(&model);

	if (failures != 0) return 1;
	std::puts("threedi_utf8_path_test: OK");
	return 0;
}
