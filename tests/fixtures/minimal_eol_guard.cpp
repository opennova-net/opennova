// Every hand-authored TEXT resource in the minimal set must use CRLF line
// endings. Retail's text parsers are not LF-tolerant, and the failure mode is
// silent: the file "loads", the parse stops early, and the game hangs or draws
// nothing rather than reporting an error.
//
// Witnessed twice, in two different formats:
//   * .mns — a LF-only menu_style.mns parses far enough to be logged twice by
//     /FRISK and then dead-ends the menu shell before main.mnu is ever reached
//     (retail Jointops.exe, 2026-08-23). Converting the SAME file to CRLF takes
//     the boot from 18 logged loads to 28, through main.mnu, the fonts and the
//     cursor. Retail's own menu_style.mns ships CRLF, and its header documents a
//     line-oriented parser with backslash continuations.
//   * .def — "a bare LF between blocks stopped retail's parser after block 0"
//     (apps/retail_minimal.py, which preserves each def's existing line ending
//     for exactly this reason).
//
// The writer-produced members of the set (.trn via save_trn, .env via save_env)
// already emit CRLF from the engine libraries; this guard covers the committed
// hand-authored files, which no writer owns. fixtures/** is `-text -eol` in
// .gitattributes, so git will not normalize these either way — an editor that
// saves LF is all it takes to silently break the boot again.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

int fail = 0;

// The hand-authored text resources. Writer-produced .trn/.env are deliberately
// absent: their CRLF is the engine writers' contract, pinned by their own tests.
const char *kAuthoredText[] = {
    "items.def", "weapon.def", "ammo.def", "main.mnu", "mp.mnu", "sp.mnu",
    "menu_style.mns",
};

std::string path(const char *name) {
	return std::string(GAME_ASSETS_DIR) + "/" + name;
}

} // namespace

int main() {
	for (const char *name : kAuthoredText) {
		const std::string p = path(name);
		std::ifstream f(p, std::ios::binary);
		if (!f) {
			std::fprintf(stderr, "FAIL: %s missing\n", name);
			++fail;
			continue;
		}
		const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (bytes.rfind("version https://git-lfs", 0) == 0) {
			std::printf("[skip] %s is an unpulled LFS pointer\n", name);
			continue;
		}

		size_t crlf = 0, bare_lf = 0;
		for (size_t i = 0; i < bytes.size(); ++i) {
			if (bytes[i] != '\n') continue;
			if (i > 0 && bytes[i - 1] == '\r') ++crlf;
			else ++bare_lf;
		}

		if (bare_lf != 0) {
			std::fprintf(stderr,
			             "FAIL: %s has %zu bare-LF line ending(s) - retail's text parsers "
			             "need CRLF and fail silently without it\n",
			             name, bare_lf);
			++fail;
		} else if (crlf == 0) {
			std::fprintf(stderr, "FAIL: %s has no line endings at all\n", name);
			++fail;
		}
	}

	if (fail == 0) std::printf("OK: authored text resources are CRLF\n");
	return fail == 0 ? 0 : 1;
}
