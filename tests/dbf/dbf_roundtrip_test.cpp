// Parse + byte-exact round-trip for opennova::dbf over a real JO dialog bank,
// and verify the dlg-id -> def-id (LWF set) mapping. Mirrors lwf_roundtrip_test.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "common/test_paths.h"
#include "dbf/dbf.h"

namespace {

std::vector<uint8_t> read_file(const std::string &path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.good()) {
		return {};
	}
	const std::streamsize size = file.tellg();
	file.seekg(0, std::ios::beg);
	std::vector<uint8_t> data(static_cast<size_t>(size));
	file.read(reinterpret_cast<char *>(data.data()), size);
	if (!file.good()) {
		return {};
	}
	return data;
}

} // namespace

int main() {
	const std::string root = test_paths_repo_root(__FILE__);
	const std::string path = root + "/fixtures/dbf/00TRg.DBF";

	const std::vector<uint8_t> original = read_file(path);
	TEST_EXPECT(!original.empty());

	opennova::dbf::File file;
	std::string error;
	TEST_EXPECT(opennova::dbf::parse_dbf_memory(original.data(), original.size(), file, error));
	TEST_EXPECT(file.header.magic == opennova::dbf::kMagic);
	TEST_EXPECT(file.header.header_size == 28);
	TEST_EXPECT(file.groups.size() == 11);

	// dlg001 -> Z00gR100 (the dialog id -> LWF set-name mapping the runtime uses).
	const opennova::dbf::Group *g = opennova::dbf::find_group(file, "dlg001");
	TEST_EXPECT(g != nullptr);
	TEST_EXPECT(!g->lines.empty());
	TEST_EXPECT(g->lines[0].def_id_name == "Z00gR100");

	// Case-insensitive lookup contract.
	TEST_EXPECT(opennova::dbf::find_group(file, "DLG001") != nullptr);
	TEST_EXPECT(opennova::dbf::find_group(file, "nope") == nullptr);

	// Byte-exact round-trip: all parsed fields are preserved on encode.
	std::vector<uint8_t> encoded;
	TEST_EXPECT(opennova::dbf::encode_dbf(file, encoded, error));
	TEST_EXPECT(encoded.size() == original.size());
	TEST_EXPECT(std::memcmp(encoded.data(), original.data(), original.size()) == 0);

	return 0;
}
