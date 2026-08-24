// The minimal set's packaging tool: bundle the COMMITTED authored tree (assets/) into the
// three boot-table archives, or assemble a runnable loose install from it. The boot gate
// counts only archives opened from the fixed name table (language.pff / localres.pff /
// resource.pff) [orig: PFF_OpenAllArchives @ 0x4a4310, name table @ 0x829f90] — an
// arbitrary-named .pff never mounts (D-VFS-2), so a single mnml.pff dies with
// ShowEarlyError(3) "missing CD?" (validated on retail 2026-07-05). Each file lands in the
// archive retail uses for its kind (witnessed against the JOTAC JO install): text bins →
// language, menus/defs/missions/fonts/music scripts → localres, terrain/env/art → resource.
//
// Nothing is generated here any more: every byte, the baked mnml.cpt included, is authored
// in ONED and committed. The tool READS assets/ and never writes into it — it once wrote its
// generated music banks over the committed ones. It SKIPS clean unless asked, because it
// writes files: OPENNOVA_BUILD_MINIMAL_PFF=1 or OPENNOVA_MINIMAL_INSTALL=<dir>. See
// assets/README.md.
#include <pff/pff.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string dir() { return GAME_ASSETS_DIR; }

bool read_bytes(const fs::path &p, std::vector<uint8_t> &b) {
	std::ifstream f(p, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff sz = f.tellg();
	if (sz < 0) return false;
	f.seekg(0);
	b.resize(static_cast<size_t>(sz));
	if (!b.empty()) f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(b.size()));
	return true;
}

bool is_lfs_pointer(const std::vector<uint8_t> &b) {
	static const char kSentinel[] = "version https://git-lfs";
	const size_t n = sizeof(kSentinel) - 1;
	return b.size() >= n && std::equal(kSentinel, kSentinel + n, b.begin());
}

// Committed sources (assets/, flat) per boot-table archive, mirroring retail's placement:
// gameerr/gametext/vmacros/keyhelp ship in retail language.pff; every .mnu/.def/.bms/.fnt and
// the music scripts in retail localres.pff; .env/.trn/.cpt/terrain art in retail resource.pff.
// The mission-family companions follow the witnessed retail placement: <stem>.bin/.pcx/.lwf
// in language.pff; <stem>.dbf (like .bms) in localres.pff. (.til/.wac stay unauthored: the
// .til loader runs in the render loop only and .wac is a witnessed silent skip.)
const char *const kLanguage[] = {"gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin",
                           "menutxt.bin", "mnml.bin",     "mnml.pcx",    "mnml.lwf"};
// The boot font set is HARDCODED by name [orig: HUD_InitAllFonts @ 0x51ee20: width
// breakpoints 640/800/1024 pick Arial12b/14n, 14b/14n, or 16b/16n; Impac22b/38b always] plus
// the menu_style.mns DEF_FONTNAME_* names; retail ships its .fnt files in localres.pff. The
// music scripts are the hardcoded base-game pair [orig: Expansion_LoadAssets @ 0x4a4730:
// MENUMUS.* / GAMEMUS.*].
const char *const kLocalres[] = {"items.def",      "weapon.def",   "ammo.def",     "main.mnu",
                           "mp.mnu",         "sp.mnu",       "mnml.bms",     "menu_style.mns",
                           "newarow1.tga",   "mnml.dbf",     "Arial12b.fnt", "Arial14n.fnt",
                           "Arial14b.fnt",   "Arial16n.fnt", "Arial16b.fnt", "Impac22b.fnt",
                           "Impac38b.fnt",   "menumus.bin",  "gamemus.bin"};
// Every image the committed mnml.trn names (minimal_trn_gen pins the list) plus the baked
// polydata. A name missing here is a texture retail asks for and never finds.
const char *const kResource[] = {"mnml.env",     "mnml.trn",     "mnml.cpt",     "mnml_c.tga",
                           "mnml_dm.tga",  "mnml_dc1.tga", "mnml_dc2.tga", "mnml_dc3.tga",
                           "mnml_dmd.tga", "mnml_d1.tga",  "mnml_t.tga",   "mnml_m.pcx",
                           "mnml_f.pcx"};
// Loose in every layout: the .sbf banks stream by path and never resolve through the
// archives [orig: AudioVM_InitMenuMusicStreaming @ 0x56aa60]; earlyerr.txt is the
// pre-archive error text, read before any mount [orig: Game_ShowEarlyError @ 0x4a68a0].
const char *const kLoose[] = {"menumus.sbf", "gamemus.sbf", "earlyerr.txt"};

// Every source must be a real committed file, not an unpulled LFS pointer: packing pointers
// produces archives retail opens and then fails inside, which reads as an authoring bug.
bool sources_present(const fs::path &root, std::initializer_list<const char *const *> lists,
                     std::initializer_list<size_t> counts) {
	bool ok = true;
	auto list_it = lists.begin();
	auto count_it = counts.begin();
	for (; list_it != lists.end(); ++list_it, ++count_it) {
		for (size_t i = 0; i < *count_it; ++i) {
			const char *n = (*list_it)[i];
			std::vector<uint8_t> b;
			if (!read_bytes(root / n, b)) {
				std::fprintf(stderr, "FAIL: committed source missing: %s\n", n);
				ok = false;
			} else if (b.empty()) {
				std::fprintf(stderr, "FAIL: committed source is empty: %s\n", n);
				ok = false;
			} else if (is_lfs_pointer(b)) {
				std::fprintf(stderr, "FAIL: %s is an unpulled LFS pointer (git lfs pull)\n", n);
				ok = false;
			}
		}
	}
	return ok;
}

// Bundle (pff_name, filepath) pairs into <root>/<archive>, then re-open it and require every
// `expect` entry to resolve.
bool write_and_verify(const fs::path &root, const char *archive,
                      const std::vector<std::pair<std::string, fs::path>> &files,
                      std::initializer_list<const char *> expect) {
	std::vector<std::vector<uint8_t>> storage(files.size());
	std::vector<PffWriteEntry> entries;
	entries.reserve(files.size());
	for (size_t i = 0; i < files.size(); ++i) {
		if (files[i].first.size() > 16) { // PFF_NAME_SIZE
			std::fprintf(stderr, "FAIL: name too long for PFF: %s\n", files[i].first.c_str());
			return false;
		}
		if (!read_bytes(files[i].second, storage[i])) {
			std::fprintf(stderr, "FAIL: missing %s\n", files[i].second.string().c_str());
			return false;
		}
		PffWriteEntry e{};
		e.name = files[i].first.c_str();
		e.data = storage[i].data();
		e.size = static_cast<uint32_t>(storage[i].size());
		entries.push_back(e);
	}

	const std::string pff = (root / archive).string();
	const int rc = pff_write_archive(pff.c_str(), PFF_FORMAT_PFF3, entries.data(),
	                                 static_cast<uint32_t>(entries.size()));
	if (rc != PFF_WRITE_OK) {
		std::fprintf(stderr, "FAIL: pff_write_archive(%s) rc=%d\n", archive, rc);
		return false;
	}
	std::printf("wrote %s (%zu entries)\n", pff.c_str(), entries.size());

	PffArchive ar{};
	if (pff_open(&ar, pff.c_str()) != 0) {
		std::fprintf(stderr, "FAIL: pff_open on the written %s\n", archive);
		return false;
	}
	int missing = 0;
	for (const char *n : expect) {
		if (!pff_find(&ar, n)) {
			std::fprintf(stderr, "FAIL: expected entry absent from %s: %s\n", archive, n);
			++missing;
		}
	}
	pff_close(&ar);
	return missing == 0;
}

// Assemble a complete runnable install at `out`: every authored file flat next to the exe,
// plus a ZERO-ENTRY resource.pff. Retail is archive-only by default, so the install runs
// with `/d` (loose-first); the token archive exists only to clear the boot gate, which counts
// archives OPENED rather than entries [orig: PFF_OpenAllArchives @ 0x4a4310; fatal check
// @ 0x4a6f44 — witnessed with a 20-byte archive on retail 2026-08-23,
// docs/vfs/vfs-pff-mount-re.md]. The loose `FindFirstFile *.bms` walk is what lists the
// mission in this layout, so the token's name does not matter to the mission list the way
// the packed archive's does (ONED's packer: localres.pff).
//
// The hard rule for this layout is NO .dds anywhere: under /d,
// Texture_LoadByNameWithChannel @ 0x58b470 truncates a model's texture name at the first
// extension and probes it loose, and a loose hit routes to the TGA/MDT/PCX branch, which
// returns 0 for a .dds name (checkerboard). Every authored texture here is .tga/.pcx, so the
// archive stays empty.
bool emit_loose_install(const fs::path &out, const fs::path &root,
                        const std::vector<std::pair<std::string, fs::path>> &language,
                        const std::vector<std::pair<std::string, fs::path>> &localres,
                        const std::vector<std::pair<std::string, fs::path>> &resource) {
	std::error_code ec;
	fs::create_directories(out, ec);

	size_t n = 0;
	for (const auto *list : {&language, &localres, &resource}) {
		for (const auto &entry : *list) {
			const std::string &name = entry.first;
			const std::string ext = fs::path(name).extension().string();
			if (ext == ".dds" || ext == ".DDS") {
				std::fprintf(stderr, "FAIL: %s is .dds - unloadable loose under /d\n", name.c_str());
				return false;
			}
			fs::copy_file(entry.second, out / name, fs::copy_options::overwrite_existing, ec);
			if (ec) {
				std::fprintf(stderr, "FAIL: copy %s -> %s: %s\n", entry.second.string().c_str(),
				             name.c_str(), ec.message().c_str());
				return false;
			}
			++n;
		}
	}

	for (const char *b : kLoose) {
		fs::copy_file(root / b, out / b, fs::copy_options::overwrite_existing, ec);
		if (ec) {
			std::fprintf(stderr, "FAIL: copy %s: %s\n", b, ec.message().c_str());
			return false;
		}
		++n;
	}

	// The boot token: zero entries, 20 bytes, under a name the fixed table probes.
	const std::string token = (out / "resource.pff").string();
	const int rc = pff_write_archive(token.c_str(), PFF_FORMAT_PFF3, nullptr, 0);
	if (rc != PFF_WRITE_OK) {
		std::fprintf(stderr, "FAIL: zero-entry resource.pff rc=%d\n", rc);
		return false;
	}

	// The retail runtime the authored set is validated against. NOT part of the
	// authored set and never committed - OPENNOVA_JO_DIR points at the user's own
	// install (docs/asset-gated-tests.md). Without it the install is complete but
	// has no exe to run.
	//   * binkw32_.dll is the real Bink; a JOTAC install's binkw32.dll is an
	//     unrelated hook shim, so prefer the underscored one when present.
	//   * game.cfg matters: on a FIRST launch with no config, retail's video
	//     enumeration hangs before the menu (reproduced 2026-08-23). Seeding the
	//     install's config skips that. It is machine state, not game content.
	if (const char *jo = std::getenv("OPENNOVA_JO_DIR")) {
		const fs::path src(jo);
		auto copy_one = [&](const fs::path &from, const char *to) {
			std::error_code e;
			if (!fs::exists(from)) return false;
			fs::copy_file(from, out / to, fs::copy_options::overwrite_existing, e);
			return !e;
		};
		const bool exe = copy_one(src / "Jointops.exe", "Jointops.exe");
		const bool bink = copy_one(src / "binkw32_.dll", "binkw32.dll") ||
		                  copy_one(src / "binkw32.dll", "binkw32.dll");
		const bool cfg = copy_one(src / "game.cfg", "game.cfg");
		std::printf("  retail runtime from OPENNOVA_JO_DIR: exe=%s bink=%s game.cfg=%s\n",
		            exe ? "yes" : "NO", bink ? "yes" : "NO", cfg ? "yes" : "NO");
	} else {
		std::printf("  (set OPENNOVA_JO_DIR to also stage Jointops.exe + binkw32.dll + game.cfg)\n");
	}

	std::printf("wrote loose install: %zu files + zero-entry resource.pff -> %s\n",
	            n, out.string().c_str());
	std::printf("  run: Jointops.exe /w /d   (add /FRISK to log loads)\n");
	return true;
}

template <size_t N>
constexpr size_t count_of(const char *const (&)[N]) { return N; }

} // namespace

int main() {
	// Two layouts over one committed tree: OPENNOVA_BUILD_MINIMAL_PFF=1 writes the three
	// boot-table archives into assets/ (the retail-validated packed shape; the .pffs are
	// gitignored there); OPENNOVA_MINIMAL_INSTALL=<dir> assembles a runnable loose install.
	const char *install_env = std::getenv("OPENNOVA_MINIMAL_INSTALL");
	const bool want_pff = std::getenv("OPENNOVA_BUILD_MINIMAL_PFF") != nullptr;
	if (!want_pff && install_env == nullptr) {
		std::printf("[skip] set OPENNOVA_BUILD_MINIMAL_PFF=1 to write the boot-table archives into "
		            "assets/, or OPENNOVA_MINIMAL_INSTALL=<dir> for a runnable loose install\n");
		return 0; // writes files: opt-in, not run per build
	}

	const fs::path root(dir());
	if (!sources_present(root, {kLanguage, kLocalres, kResource, kLoose},
	                     {count_of(kLanguage), count_of(kLocalres), count_of(kResource),
	                      count_of(kLoose)}))
		return 1;

	// Per-archive file lists: committed sources by retail placement.
	std::vector<std::pair<std::string, fs::path>> language, localres, resource;
	for (const char *n : kLanguage) language.emplace_back(n, root / n);
	for (const char *n : kLocalres) localres.emplace_back(n, root / n);
	for (const char *n : kResource) resource.emplace_back(n, root / n);

	// The loose layout: everything flat plus the zero-entry boot token.
	if (install_env != nullptr &&
	    !emit_loose_install(fs::path(install_env), root, language, localres, resource))
		return 1;

	if (!want_pff) {
		std::printf("OK: loose install assembled (set OPENNOVA_BUILD_MINIMAL_PFF=1 "
		            "to also write the boot-table archives)\n");
		return 0;
	}

	// Write + verify each boot-table archive: the boot bins in language, the
	// mission/menu/font/music set in localres, the map polydata in resource.
	if (!write_and_verify(root, "language.pff", language,
	                      {"gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin",
	                       "menutxt.bin", "mnml.bin", "mnml.pcx", "mnml.lwf"}))
		return 1;
	if (!write_and_verify(root, "localres.pff", localres,
	                      {"items.def", "main.mnu", "mp.mnu", "sp.mnu", "mnml.bms", "menu_style.mns",
	                       "Arial16n.fnt", "Impac38b.fnt", "menumus.bin", "gamemus.bin",
	                       "newarow1.tga", "mnml.dbf"}))
		return 1;
	if (!write_and_verify(root, "resource.pff", resource,
	                      {"mnml.env", "mnml.trn", "mnml.cpt", "mnml_c.tga", "mnml_dc2.tga",
	                       "mnml_dc3.tga", "mnml_dmd.tga", "mnml_d1.tga"}))
		return 1;

	std::printf("OK: language/localres/resource.pff bundle the committed minimal set "
	            "(fatal + map + terrain) under the witnessed boot-table names\n");
	return 0;
}
