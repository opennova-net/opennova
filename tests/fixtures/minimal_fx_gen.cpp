// Generator + guard for the authored .fx shader set: the plaintext HLSL
// sources under tests/fixtures/fx/ wrap into the SCR containers retail's
// effect loader consumes (assets/*.fx). Retail rejects a bare-text .fx —
// ScriptFile_LoadAndDecrypt @ 0x5AE060 requires the exact 'SCR',0x01 header
// and decrypts with the shader key (0xA55B1EED, key at 0x5AE0C0) — so the
// committed artifacts are the wrapped form and this test pins the pair:
// wrap(source) must byte-equal the committed artifact, and assets/ must hold
// exactly this set (a stray .fx in the game dir is a policy violation, not
// data).
//
// Default: rebuild in memory and byte-compare the committed files. `--write`
// (re)writes them.
#include <formats/scr/scr.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// The authored set: 8 effects (skinned body/arms/head families, the rigid
// phong the rifle rides, the fixed-function fallback) + 6 shared includes.
// The include names are underscore-prefixed like retail's so the loose
// override walk skips them [orig: HLSLEffect_InitAndLoadAll @ 0x5b0080].
const char *const kFxNames[] = {
    "_baseinc.fx",  "_vsshared.fx", "_vsskshared.fx", "_psshared.fx",
    "_tdepth.fx",   "_tskin.fx",    "_ffp.fx",        "phongt.fx",
    "skbasic.fx",   "skbdifft2.fx", "skbdiffo.fx",    "skbdiffo2.fx",
    "skbphongt.fx", "skbphongo.fx",
};
constexpr size_t kFxCount = sizeof(kFxNames) / sizeof(kFxNames[0]);

bool read_bytes(const fs::path &p, std::vector<uint8_t> &b) {
	std::ifstream f(p, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff sz = f.tellg();
	if (sz < 0) return false;
	f.seekg(0);
	b.resize(static_cast<size_t>(sz));
	if (!b.empty()) f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size()));
	return f.good() || b.empty();
}

std::vector<uint8_t> wrap(const std::vector<uint8_t> &plain) {
	std::vector<uint8_t> out(plain.size() + SCR_HEADER_SIZE);
	size_t out_size = out.size();
	if (scr_encrypt_buf(plain.data(), plain.size(), out.data(), &out_size, SCR_KEY_SHADERS, 1) != 0)
		out.clear();
	else
		out.resize(out_size);
	return out;
}

std::string lower(std::string s) {
	for (char &c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
	return s;
}

} // namespace

int main(int argc, char **argv) {
	const bool write = argc > 1 && std::strcmp(argv[1], "--write") == 0;
	const fs::path src_dir(FX_SOURCE_DIR);
	const fs::path out_dir(GAME_ASSETS_DIR);
	int failures = 0;

	for (size_t i = 0; i < kFxCount; ++i) {
		const char *name = kFxNames[i];
		std::vector<uint8_t> plain;
		if (!read_bytes(src_dir / name, plain) || plain.empty()) {
			std::fprintf(stderr, "FAIL: fx source missing or empty: %s\n", name);
			++failures;
			continue;
		}
		const std::vector<uint8_t> wrapped = wrap(plain);
		if (wrapped.empty()) {
			std::fprintf(stderr, "FAIL: scr_encrypt_buf failed for %s\n", name);
			++failures;
			continue;
		}

		// Self-check the pair through the read side before trusting it.
		std::vector<uint8_t> round(wrapped.size());
		size_t round_size = round.size();
		if (scr_decrypt_buf(wrapped.data(), wrapped.size(), round.data(), &round_size,
		                    SCR_KEY_SHADERS) != 0 ||
		    round_size != plain.size() ||
		    std::memcmp(round.data(), plain.data(), plain.size()) != 0) {
			std::fprintf(stderr, "FAIL: SCR wrap did not round-trip for %s\n", name);
			++failures;
			continue;
		}

		const fs::path out = out_dir / name;
		if (write) {
			std::ofstream f(out, std::ios::binary | std::ios::trunc);
			f.write(reinterpret_cast<const char *>(wrapped.data()),
			        static_cast<std::streamsize>(wrapped.size()));
			if (!f) {
				std::fprintf(stderr, "FAIL: cannot write %s\n", out.string().c_str());
				++failures;
			} else {
				std::printf("wrote %s (%zu bytes)\n", out.string().c_str(), wrapped.size());
			}
			continue;
		}

		std::vector<uint8_t> committed;
		if (!read_bytes(out, committed)) {
			std::fprintf(stderr, "FAIL: committed artifact missing: %s (run with --write)\n",
			             out.string().c_str());
			++failures;
			continue;
		}
		if (committed != wrapped) {
			std::fprintf(stderr,
			             "FAIL: %s does not match wrap(source) (%zu vs %zu bytes) - "
			             "edit the source under tests/fixtures/fx/ and rerun with --write\n",
			             name, committed.size(), wrapped.size());
			++failures;
		}
	}

	// The game dir holds exactly the authored set: nothing but these names may
	// carry the extension the shader walk enumerates.
	std::vector<std::string> extra;
	for (const auto &de : fs::directory_iterator(out_dir)) {
		if (!de.is_regular_file()) continue;
		if (lower(de.path().extension().string()) != ".fx") continue;
		const std::string base = de.path().filename().string();
		bool known = false;
		for (size_t i = 0; i < kFxCount; ++i) {
			if (lower(base) == lower(kFxNames[i])) { known = true; break; }
		}
		if (!known) extra.push_back(base);
	}
	std::sort(extra.begin(), extra.end());
	for (const std::string &e : extra) {
		std::fprintf(stderr, "FAIL: unexpected .fx in assets/: %s (not in the authored set)\n",
		             e.c_str());
		++failures;
	}

	if (failures) return 1;
	std::printf("OK: %zu authored .fx wrap to the committed SCR artifacts%s\n", kFxCount,
	            write ? " (written)" : "");
	return 0;
}
