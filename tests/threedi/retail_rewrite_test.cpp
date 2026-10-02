// Every retail .3di through the engine's one reader and writer: read -> write
// gives the file's own bytes, and a second read -> write gives the first
// write's bytes. The editor saves a model with exactly this writer (ADR 0046
// S10), so a retail model opened and saved untouched must come back as the
// file. A mismatch names the first chunk whose bytes differ.
// Gated on OPENNOVA_JO_ASSETS (docs/asset-gated-tests.md): every *.3di loose at
// the asset root.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <formats/threedi/threedi.h>
#include <formats/threedi/threedi_3di3.h>

#include "common/file_io.h"
#include "common/retail_paths.h"

using namespace opennova::threedi;

namespace {

// The path of the first chunk whose id, size or payload differs ("" when the
// trees match), e.g. "RLOD[1]/PANM".
std::string first_difference(const ThreediChunk &a, const ThreediChunk &b, const std::string &at) {
	const std::string here = at.empty() ? std::string(a.id) : at + "/" + a.id;
	if (std::strcmp(a.id, b.id) != 0) return here + " (id " + a.id + " vs " + b.id + ")";
	if (a.is_parent != b.is_parent || a.child_count != b.child_count)
		return here + " (children " + std::to_string(a.child_count) + " vs " + std::to_string(b.child_count) + ")";
	if (!a.is_parent) {
		if (a.data_len != b.data_len)
			return here + " (size " + std::to_string(a.data_len) + " vs " + std::to_string(b.data_len) + ")";
		for (size_t i = 0; i < a.data_len; ++i)
			if (a.data[i] != b.data[i]) return here + " (byte " + std::to_string(i) + ")";
		return std::string();
	}
	for (size_t i = 0; i < a.child_count; ++i) {
		const std::string child = first_difference(a.children[i], b.children[i],
				here + "[" + std::to_string(i) + "]");
		if (!child.empty()) return child;
	}
	return std::string();
}

std::string describe(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
	ThreediFile fa = {}, fb = {};
	std::string where = "the chunk trees";
	if (threedi_read_memory(a.data(), a.size(), &fa) == 0 && threedi_read_memory(b.data(), b.size(), &fb) == 0) {
		if (fa.version != fb.version) where = "the version word";
		else where = first_difference(*fa.root, *fb.root, "");
		if (where.empty()) where = "bytes outside the chunks";
	}
	threedi_free_file(&fa);
	threedi_free_file(&fb);
	return where;
}

// Read, write; "" and the written bytes, or what failed.
std::string rewrite(const std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
	Threedi3di3 model = {};
	if (threedi_3di3_read_memory(in.data(), in.size(), &model) != 0) return "does not read";
	const int rc = threedi_3di3_write_memory(&model, out);
	threedi_3di3_free(&model);
	return rc == 0 ? std::string() : std::string("does not write");
}

} // namespace

int main() {
	const std::string root = retail::assets();
	if (!retail::dir_exists(root)) return retail::skip("OPENNOVA_JO_ASSETS (the retail models at its root)");

	std::vector<std::filesystem::path> models;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(root, ec))
		if (entry.is_regular_file(ec) && retail::lower_ascii(entry.path().extension().string()) == ".3di")
			models.push_back(entry.path());
	if (models.empty()) return retail::skip("OPENNOVA_JO_ASSETS with the retail .3di models at its root");

	int failures = 0;
	for (const auto &path : models) {
		const std::string name = path.filename().string();
		const std::vector<uint8_t> original = test_io::read_file(path.string());
		std::vector<uint8_t> first, second;
		std::string problem = rewrite(original, first);
		if (problem.empty() && first != original) problem = "rewrites " + describe(original, first);
		if (problem.empty()) problem = rewrite(first, second);
		if (problem.empty() && second != first) problem = "is not a fixed point at " + describe(first, second);
		if (!problem.empty()) {
			std::printf("FAIL %s: %s\n", name.c_str(), problem.c_str());
			++failures;
		}
	}
	std::printf("%zu retail models, %d rewritten differently\n", models.size(), failures);
	return failures == 0 ? 0 : 1;
}
