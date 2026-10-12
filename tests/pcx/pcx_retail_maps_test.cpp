// Retail terrain maps through the indexed PCX form: every char map (_m.pcx) and foliage map (_f.pcx) directly
// under the extracted asset tree reads with its carried form (the shipped maps' coding, the _f maps' leftover
// header bytes 16..63 among the carried bytes) and, written again in that form, is the same bytes. Reports
// Skipped without OPENNOVA_JO_ASSETS.

#include <formats/pcx/pcx.h>
#include <formats/pcx/pcx_io.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "common/file_io.h"
#include "common/retail_paths.h"

namespace {

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool is_map(const std::string &name) {
	const std::string l = lower(name);
	const auto ends = [&l](const char *tail) {
		const std::string t(tail);
		return l.size() >= t.size() && l.compare(l.size() - t.size(), t.size(), t) == 0;
	};
	return ends("_m.pcx") || ends("_f.pcx");
}

} // namespace

int main() {
	const std::string assets = retail::assets();
	if (assets.empty())
		return retail::skip("OPENNOVA_JO_ASSETS (the extracted retail tree with its loose _m/_f.pcx terrain maps)");
	int failures = 0, swept = 0, foliage = 0;
	for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(assets)) {
		if (!entry.is_regular_file()) continue;
		const std::string name = entry.path().filename().string();
		if (!is_map(name)) continue;
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().string());
		opennova::IndexedImage8 image;
		opennova::PcxIndexedForm form;
		std::string error;
		if (!opennova::decode_pcx_indexed(bytes.data(), bytes.size(), image, error, &form)) {
			std::printf("FAIL: %s: %s\n", name.c_str(), error.c_str());
			++failures;
			continue;
		}
		++swept;
		if (lower(name).find("_f.pcx") != std::string::npos) ++foliage;
		if (form.coding != opennova::PcxCoding::Shipped) {
			std::printf("FAIL: %s: its rows are not the shipped coding\n", name.c_str());
			++failures;
		}
		std::vector<uint8_t> again;
		if (!opennova::encode_pcx_indexed(image, again, error, &form) || again != bytes) {
			std::printf("FAIL: %s written again in its form differs (%zu bytes, written %zu)\n", name.c_str(),
			            bytes.size(), again.size());
			++failures;
		}
	}
	if (swept == 0 || foliage == 0) {
		std::printf("FAIL: no _m.pcx and _f.pcx under %s\n", assets.c_str());
		return 1;
	}
	std::printf("%s: %d terrain maps (%d foliage) swept, %d failures\n", failures == 0 ? "OK" : "FAIL", swept, foliage,
	            failures);
	return failures == 0 ? 0 : 1;
}
