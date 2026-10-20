// Every committed .wac script against the original compiler. For each source,
// fixtures/wac_corpus_vectors.inc holds the SHA-256 of the listing
// Script_Compile @0x4F31F0 writes of it with every catalog empty (effects,
// sound sets, ammo, mission text; the seven default groups), written by
// scripts/oracles/wac_parity.py; the port compiles the same file the same way
// and must write the same listing. Only hashes are committed. The scripts are
// the tree's own: the base game (assets/), its expansions (expansions/) and the
// test scripts (fixtures/wac/), among them fixtures/wac/forms.wac, authored to
// hold every lexical and statement form the 48 shipped scripts use (surveyed
// 2026-10-19, when this row last read them: both comment starts, CR LF lines,
// tabs and trailing blanks, mixed case, no final line end, AND / OR / NOT,
// ELSE, ELSEIF on its own line, an `Else if`, DOSEQ and DORND with NEXT,
// assignment and comparison operators, a stray '/', a command with no
// parentheses and a quoted string, the operand prefixes). Directories come from
// argv, else the tree's own.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <base/io/sha256.h>
#include <runtime/wac/compiler.h>
#include "common/file_io.h"
#include "common/test_paths.h"
#include "wac_listing.h"

namespace fs = std::filesystem;
using namespace opennova::wac;

namespace {

struct CorpusVector {
	const char *source_sha256;
	const char *listing_sha256;
};
const CorpusVector kCorpus[] = {
#include "fixtures/wac_corpus_vectors.inc"
};

std::string upper(std::string text) {
	for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return text;
}

std::string sha256(const std::string &bytes) {
	return opennova::io::sha256_hex(bytes.data(), bytes.size());
}

} // namespace

int main(int argc, char **argv) {
	std::vector<std::string> dirs;
	for (int i = 1; i < argc; ++i) dirs.push_back(argv[i]);
	if (dirs.empty()) {
		const std::string root = test_paths_repo_root(__FILE__);
		for (const char *sub : {"/assets", "/expansions", "/fixtures/wac"}) dirs.push_back(root + sub);
	}

	std::map<std::string, std::string> scripts; // upper-case file name -> path, for RUN
	for (const std::string &dir : dirs) {
		std::error_code ec;
		if (!fs::is_directory(dir, ec)) continue;
		for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator();
				it.increment(ec)) {
			if (ec) break;
			if (!it->is_regular_file(ec) || upper(it->path().extension().string()) != ".WAC") continue;
			scripts.emplace(upper(it->path().filename().string()), it->path().string());
		}
	}
	if (scripts.empty()) {
		std::printf("FAIL: no .wac scripts under the corpus directories\n");
		return 1;
	}

	int compared = 0;
	int unwitnessed = 0;
	int failed = 0;
	for (const auto &[name, path] : scripts) {
		const std::string source = test_io::read_file_text(path);
		const std::string source_hash = sha256(source);
		const auto vector = std::find_if(std::begin(kCorpus), std::end(kCorpus),
				[&](const CorpusVector &v) { return source_hash == v.source_sha256; });
		if (vector == std::end(kCorpus)) {
			std::printf("  no original listing for %s (%s)\n", name.c_str(), source_hash.c_str());
			++unwitnessed;
			continue;
		}
		CompileEnv env;
		env.source_names = {"script.wac"};
		env.load_source = [&scripts](const std::string &file, std::string &text) {
			const auto found = scripts.find(upper(file));
			if (found == scripts.end()) return false;
			text = test_io::read_file_text(found->second);
			return true;
		};
		const Program program = compile_source(source, env);
		++compared;
		if (sha256(wac_listing::document(program)) != vector->listing_sha256) {
			std::printf("FAIL %s: the listing differs from the original compiler's\n", name.c_str());
			++failed;
		}
	}
	std::printf("corpus: %d scripts compared, %d without an original listing\n", compared, unwitnessed);
	if (compared == 0) {
		std::printf("FAIL: no corpus script has an original listing\n");
		return 1;
	}
	if (failed != 0) {
		std::printf("CORPUS TEST FAILED (%d)\n", failed);
		return 1;
	}
	std::printf("corpus test passed\n");
	return 0;
}
