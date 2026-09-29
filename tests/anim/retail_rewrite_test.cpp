// Every retail animation file through the engine's reader and writer, the path
// the editor saves them by (ADR 0046 S10):
//   .bad: parse -> write -> parse is field-equal to the file (common/bad_equal.h:
//         every field the reader keeps; the bone table's stale child/parent
//         addresses and the translation pad row are the writer's own), and the
//         write is a fixed point (writing the re-read clip gives the same bytes).
//   .adm: the canonical write reads back as the file's rows, and is a fixed point.
// Gated on OPENNOVA_JO_ASSETS (docs/asset-gated-tests.md): every *.bad and
// *.adm loose at the asset root.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_write.h>

#include "common/bad_equal.h"
#include "common/file_io.h"
#include "common/retail_paths.h"

using namespace opennova;

namespace {

std::string check_clip(const std::vector<uint8_t> &original) {
	bad::BadFile parsed = {}, reread = {};
	std::string problem;
	std::vector<uint8_t> first, second;
	if (bad::bad_parse_buffer(original.data(), original.size(), &parsed) != 0) problem = "does not parse";
	else if (bad::bad_write_buffer(&parsed, first) != 0) problem = "does not write";
	else if (bad::bad_parse_buffer(first.data(), first.size(), &reread) != 0) problem = "its rewrite does not parse";
	else if (!bad_equal::field_equal(parsed, reread)) problem = "its rewrite reads back different fields";
	else if (bad::bad_write_buffer(&reread, second) != 0 || second != first) problem = "the write is not a fixed point";
	bad::bad_free(&parsed);
	bad::bad_free(&reread);
	return problem;
}

bool same_rows(const adm::AdmFile &a, const adm::AdmFile &b) {
	if (a.count != b.count) return false;
	for (size_t i = 0; i < a.count; ++i) {
		const adm::AdmEntry &x = a.entries[i];
		const adm::AdmEntry &y = b.entries[i];
		if (std::strcmp(x.key, y.key) != 0 || x.variant_count != y.variant_count) return false;
		for (size_t v = 0; v < x.variant_count; ++v)
			if (std::strcmp(x.variants[v], y.variants[v]) != 0) return false;
	}
	return true;
}

std::string check_table(const std::vector<uint8_t> &original) {
	adm::AdmFile parsed = {}, reread = {};
	std::string problem, first, second;
	if (adm::adm_parse_buffer(reinterpret_cast<const char *>(original.data()), original.size(), &parsed) != 0)
		problem = "does not parse";
	else if (adm::adm_write_buffer(&parsed, first) != 0) problem = "does not write";
	else if (adm::adm_parse_buffer(first.data(), first.size(), &reread) != 0) problem = "its rewrite does not parse";
	else if (!same_rows(parsed, reread)) problem = "its rewrite reads back different rows";
	else if (adm::adm_write_buffer(&reread, second) != 0 || second != first) problem = "the write is not a fixed point";
	adm::adm_free(&parsed);
	adm::adm_free(&reread);
	return problem;
}

} // namespace

int main() {
	const std::string root = retail::assets();
	if (!retail::dir_exists(root)) return retail::skip("OPENNOVA_JO_ASSETS (the retail clips and tables at its root)");

	size_t clips = 0, tables = 0;
	int failures = 0;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		const std::string extension = retail::lower_ascii(entry.path().extension().string());
		if (extension != ".bad" && extension != ".adm") continue;
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().string());
		const std::string problem = extension == ".bad" ? check_clip(bytes) : check_table(bytes);
		++(extension == ".bad" ? clips : tables);
		if (!problem.empty()) {
			std::printf("FAIL %s: %s\n", entry.path().filename().string().c_str(), problem.c_str());
			++failures;
		}
	}
	if (clips + tables == 0) return retail::skip("OPENNOVA_JO_ASSETS with the retail .bad/.adm files at its root");
	std::printf("%zu retail clips, %zu retail tables, %d rewritten differently\n", clips, tables, failures);
	return failures == 0 ? 0 : 1;
}
