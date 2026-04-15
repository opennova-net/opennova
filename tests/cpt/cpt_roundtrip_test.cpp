#include <cpt/cpt.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>

namespace {

bool expect(bool condition, const char *message) {
	if (condition) {
		return true;
	}
	std::fprintf(stderr, "FAIL: %s\n", message);
	return false;
}

} // namespace

int main() {
	const std::filesystem::path out_path = std::filesystem::temp_directory_path() / "opennova_cpt_roundtrip_test.cpt";

	opennova::CptFile saved;
	std::strncpy(saved.header.terrain_name, "RoundtripTerrain", sizeof(saved.header.terrain_name) - 1);
	std::strncpy(saved.header.creator, "tester", sizeof(saved.header.creator) - 1);
	std::strncpy(saved.header.version, "unit", sizeof(saved.header.version) - 1);
	saved.depth_format = opennova::DepthFormat::DPTH;
	saved.depth_buffer.resize(1024u * 1024u);
	for (size_t i = 0; i < saved.depth_buffer.size(); ++i) {
		saved.depth_buffer[i] = static_cast<uint16_t>(i % 1024u);
	}

	try {
		saved.write(out_path.string());
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: CptFile::write threw: %s\n", e.what());
		return 1;
	}

	opennova::CptFile loaded;
	try {
		loaded = opennova::CptFile::read(out_path.string());
	} catch (const std::exception &e) {
		std::filesystem::remove(out_path);
		std::fprintf(stderr, "FAIL: CptFile::read threw: %s\n", e.what());
		return 1;
	}

	std::filesystem::remove(out_path);

	if (!expect(loaded.depth_format == opennova::DepthFormat::DPTH, "depth_format should round-trip")) return 1;
	if (!expect(loaded.depth_buffer.size() == saved.depth_buffer.size(), "depth buffer size should round-trip")) return 1;
	if (!expect(std::strcmp(loaded.header.terrain_name, saved.header.terrain_name) == 0, "terrain_name should round-trip")) return 1;
	if (!expect(std::strcmp(loaded.header.creator, saved.header.creator) == 0, "creator should round-trip")) return 1;
	if (!expect(loaded.depth_buffer[0] == saved.depth_buffer[0] &&
	                loaded.depth_buffer[511] == saved.depth_buffer[511] &&
	                loaded.depth_buffer[900000] == saved.depth_buffer[900000],
	            "depth samples should round-trip")) return 1;

	std::printf("OK: cpt round-trip preserved DPTH depth payload\n");
	return 0;
}
