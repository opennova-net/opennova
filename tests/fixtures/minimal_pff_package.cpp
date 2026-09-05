// The minimal set's packaging tool: bundle the COMMITTED authored tree (assets/) into the
// three boot-table archives, or assemble a runnable loose install from it. The boot gate
// counts only archives opened from the fixed name table (language.pff / localres.pff /
// resource.pff) [orig: PFF_OpenAllArchives @ 0x4a4310, name table @ 0x829f90] — an
// arbitrary-named .pff never mounts (D-VFS-2), so a single mnml.pff dies with
// ShowEarlyError(3) "missing CD?" (validated on retail 2026-07-05). Each file lands in the
// archive retail uses for its kind (witnessed against the JOTAC JO install): text bins →
// language, menus/defs/missions/fonts/music scripts → localres, terrain/env/art + the
// .fx shader set → resource.
//
// Nothing is generated here any more: every byte, the baked mnml.cpt included, is authored
// in ONED and committed. The tool reads assets/ and only writes for `--write-pff` or
// `--install <dir>`; `--check` validates the shared manifest read-only in CTest. It once
// wrote generated music banks over the committed ones. See assets/README.md.
#include <formats/pff/pff.h>

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
#include "common/retail_paths.h"

#include <cctype>
#include <cstring>
#include <ctime>

using namespace opennova::pff;

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

using FileList = std::vector<std::pair<std::string, fs::path>>;

// assets/.gitignore is deliberately an explicit allowlist: assets/ also doubles as a retail
// run directory, so enumerating the directory would package logs, saves, generated PFFs, or
// other ignored retail bytes. Read the authored names from that allowlist, keep shaders in
// resource.pff, and stage every other authored file loose. This is the single manifest used
// by both --check and --install.
bool collect_authored_files(const fs::path &root, FileList &loose, FileList &shaders) {
	std::ifstream manifest(root / ".gitignore");
	if (!manifest) {
		std::fprintf(stderr, "FAIL: cannot read authored asset allowlist: %s\n",
		             (root / ".gitignore").string().c_str());
		return false;
	}

	std::vector<std::string> seen;
	std::string line;
	while (std::getline(manifest, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.rfind("!/", 0) != 0) continue;

		const std::string name = line.substr(2);
		if (name == ".gitignore" || name == "README.md") continue;
		const fs::path relative(name);
		if (name.empty() || relative.is_absolute() || relative.has_parent_path()) {
			std::fprintf(stderr, "FAIL: unsupported authored asset allowlist entry: %s\n",
			             line.c_str());
			return false;
		}
		if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
			std::fprintf(stderr, "FAIL: duplicate authored asset allowlist entry: %s\n",
			             name.c_str());
			return false;
		}

		std::vector<uint8_t> bytes;
		if (!read_bytes(root / relative, bytes)) {
			std::fprintf(stderr, "FAIL: committed source missing: %s\n", name.c_str());
			return false;
		}
		if (bytes.empty()) {
			std::fprintf(stderr, "FAIL: committed source is empty: %s\n", name.c_str());
			return false;
		}
		if (is_lfs_pointer(bytes)) {
			std::fprintf(stderr, "FAIL: %s is an unpulled LFS pointer (git lfs pull)\n",
			             name.c_str());
			return false;
		}

		std::string ext = relative.extension().string();
		for (char &ch : ext)
			ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
		(ext == ".fx" ? shaders : loose).emplace_back(name, root / relative);
		seen.push_back(name);
	}

	std::sort(loose.begin(), loose.end());
	std::sort(shaders.begin(), shaders.end());
	return true;
}

bool manifest_has(const FileList &files, const char *name) {
	return std::any_of(files.begin(), files.end(), [&](const auto &entry) {
		return entry.first == name;
	});
}

bool manifest_has_all(const FileList &files, std::initializer_list<const char *const *> lists,
                      std::initializer_list<size_t> counts) {
	bool ok = true;
	auto list_it = lists.begin();
	auto count_it = counts.begin();
	for (; list_it != lists.end(); ++list_it, ++count_it) {
		for (size_t i = 0; i < *count_it; ++i) {
			const char *name = (*list_it)[i];
			if (!manifest_has(files, name)) {
				std::fprintf(stderr, "FAIL: required source is not in assets/.gitignore: %s\n",
				             name);
				ok = false;
			}
		}
	}
	return ok;
}

// Sentinels for the retail bring-up path the minimal set currently promises. The complete
// allowlist is staged, while these make an accidental removal of the AK viewmodel or the
// player locomotion chain fail the read-only package check with a useful message.
const char *const kRequiredBringup[] = {
    "AKM_1st.3di", "AKM_1ST.adm", "ArmsG.3di",     "RAKM_1f.bad",
    "RAKM_1f2.bad", "RAKM_1i.bad", "RAKM_1i2.bad", "RAKM_1r.bad",
    "rAKM_RST.bad", "rpk74_6.dds", "E_STAND.adm",  "US01.3di",
    "US01.ADM",     "DT1runF.bad",  "FAILSAFE.BAD", "AVATARS.DEF",
    "SndProf.def"};
const char *const kRequiredShaders[] = {"_ffp.fx", "_baseinc.fx", "phongt.fx",
                                        "skbasic.fx"};

uint32_t crc32_ieee(const uint8_t *p, size_t n) {
	uint32_t c = 0xFFFFFFFFu;
	for (size_t i = 0; i < n; ++i) {
		c ^= p[i];
		for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
	}
	return ~c;
}

// Bundle (pff_name, filepath) pairs into <root>/<archive>, then re-open it and require every
// `expect` entry to resolve. Every entry is STAMPED: the boot shader-precompile PFF walk
// SKIPS archives whose entries carry zero timestamp/checksum (witnessed 2026-08-31 on this
// set: a zero-stamped resource.pff left retail in fixed-function fallback with no FP
// viewmodel; generic nonzero stamps -- pack-time time() + CRC32 -- drew it on the same day).
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
		e.timestamp = static_cast<uint32_t>(time(nullptr));
		e.checksum = crc32_ieee(storage[i].data(), storage[i].size());
		if (e.checksum == 0) e.checksum = 1;
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
// plus a resource.pff carrying the committed .fx shader set (see below). Retail is archive-only by default, so the install runs
// with `/d` (loose-first); the token archive exists only to clear the boot gate, which counts
// archives OPENED rather than entries [orig: PFF_OpenAllArchives @ 0x4a4310; fatal check
// @ 0x4a6f44 — witnessed with a 20-byte archive on retail 2026-08-23,
// docs/vfs/vfs-pff-mount-re.md]. The loose `FindFirstFile *.bms` walk is what lists the
// mission in this layout, so the token's name does not matter to the mission list the way
// the packed archive's does (ONED's packer: localres.pff).
//
// The retail models name .tga textures while the bring-up set carries their witnessed .dds
// substitutes. Under /d, Texture_LoadByNameWithChannel @ 0x58b470 (its loose .dds probe
// @ 0x58b52c) performs that substitution for loose files, so those .dds files must be copied
// too. Only shaders stay archived because
// the boot precompiler discovers .fx exclusively through PFF directory walks.
bool emit_loose_install(const fs::path &out, const fs::path &root, const FileList &loose,
                        const FileList &shaders) {
	std::error_code ec;
	fs::create_directories(out, ec);

	size_t n = 0;
	for (const auto &entry : loose) {
		fs::copy_file(entry.second, out / entry.first, fs::copy_options::overwrite_existing, ec);
		if (ec) {
			std::fprintf(stderr, "FAIL: copy %s -> %s: %s\n", entry.second.string().c_str(),
			             entry.first.c_str(), ec.message().c_str());
			return false;
		}
		++n;
	}

	// resource.pff clears the boot gate (an OPENED archive counts, D-VFS-2) and CARRIES
	// the committed .fx shader set: the shader precompile only ENUMERATES shaders through
	// PFF directory walks, so loose .fx are invisible to it -- 47 of them staged flat left
	// retail probing only _ffp.fx by name and falling back to fixed-function, which
	// silently kills the FP viewmodel and every skinned draw (witnessed 2026-08-31).
	if (!write_and_verify(out, "resource.pff", shaders, {"_ffp.fx", "_baseinc.fx"}))
		return false;

	// The retail runtime the authored set is validated against. NOT part of the
	// authored set and never committed - OPENNOVA_JO_DIR points at the user's own
	// install (docs/asset-gated-tests.md). Without it the install is complete but
	// has no exe to run.
	//   * binkw32_.dll is the real Bink; a JOTAC install's binkw32.dll is an
	//     unrelated hook shim, so prefer the underscored one when present.
	//   * game.cfg matters: on a FIRST launch with no config, retail's video
	//     enumeration hangs before the menu (reproduced 2026-08-23). Seeding the
	//     install's config skips that. It is machine state, not game content.
	if (const std::string jo = retail::install(); !jo.empty()) {
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

	std::printf("wrote loose install: %zu files + the shader resource.pff -> %s\n",
	            n, out.string().c_str());
	std::printf("  run: Jointops.exe /w /d   (add /FRISK to log loads)\n");
	return true;
}

template <size_t N>
constexpr size_t count_of(const char *const (&)[N]) { return N; }

} // namespace

int main(int argc, char **argv) {
	// Two layouts over one committed tree: `--write-pff` writes the three
	// boot-table archives into assets/ (the retail-validated packed shape; the
	// .pffs are gitignored there); `--install <dir>` assembles a runnable loose
	// install. Both write modes remain opt-in; CTest passes --check to exercise
	// the same authored manifest without mutating the tree.
	const char *install_env = nullptr;
	bool want_pff = false;
	bool want_check = false;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--write-pff") == 0) want_pff = true;
		else if (std::strcmp(argv[i], "--check") == 0) want_check = true;
		else if (std::strcmp(argv[i], "--install") == 0 && i + 1 < argc) install_env = argv[++i];
	}
	if (!want_pff && !want_check && install_env == nullptr)
		return retail::skip("--check, --write-pff (boot-table archives into assets/), or --install <dir> (a runnable loose install)");

	const fs::path root(dir());
	FileList authored_loose, shaders;
	if (!collect_authored_files(root, authored_loose, shaders))
		return 1;
	if (!manifest_has_all(authored_loose,
	                      {kLanguage, kLocalres, kResource, kLoose, kRequiredBringup},
	                      {count_of(kLanguage), count_of(kLocalres), count_of(kResource),
	                       count_of(kLoose), count_of(kRequiredBringup)}) ||
	    !manifest_has_all(shaders, {kRequiredShaders}, {count_of(kRequiredShaders)}))
		return 1;

	// Per-archive file lists: committed sources by retail placement.
	FileList language, localres, resource;
	for (const char *n : kLanguage) language.emplace_back(n, root / n);
	for (const char *n : kLocalres) localres.emplace_back(n, root / n);
	for (const char *n : kResource) resource.emplace_back(n, root / n);

	// The committed .fx shader set rides resource.pff in BOTH layouts (retail's own
	// placement). It comes from the same explicit authored allowlist as the loose files.
	if (shaders.empty()) {
		std::fprintf(stderr, "FAIL: no .fx shaders in %s - the FP/skinned pass dies without them\n",
		             root.string().c_str());
		return 1;
	}

	// The loose layout: everything flat plus the shader-bearing resource.pff.
	if (install_env != nullptr &&
	    !emit_loose_install(fs::path(install_env), root, authored_loose, shaders))
		return 1;

	if (!want_pff && install_env != nullptr) {
		std::printf("OK: loose install assembled (add --write-pff to also write the "
		            "boot-table archives)\n");
		return 0;
	}
	if (!want_pff) {
		std::printf("OK: authored package manifest has %zu loose files and %zu shaders; "
		            "AK viewmodel + player locomotion sentinels are present\n",
		            authored_loose.size(), shaders.size());
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
	FileList resource_all = resource;
	resource_all.insert(resource_all.end(), shaders.begin(), shaders.end());
	if (!write_and_verify(root, "resource.pff", resource_all,
	                      {"mnml.env", "mnml.trn", "mnml.cpt", "mnml_c.tga", "mnml_dc2.tga",
	                       "mnml_dc3.tga", "mnml_dmd.tga", "mnml_d1.tga", "_ffp.fx"}))
		return 1;

	std::printf("OK: language/localres/resource.pff bundle the committed minimal set "
	            "(fatal + map + terrain) under the witnessed boot-table names\n");
	return 0;
}
