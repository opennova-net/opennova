// The minimal set's packaging tool: generate the terrain, then bundle the
// authored set into the THREE boot-table archives. The boot gate counts only
// archives opened from the fixed name table (language.pff / localres.pff /
// resource.pff) [orig: PFF_OpenAllArchives @ 0x4a4310, name table @ 0x829f90]
// — an arbitrary-named .pff never mounts (D-VFS-2), so a single mnml.pff dies
// with ShowEarlyError(3) "missing CD?" (validated on retail 2026-07-05). Each
// file lands in the archive retail uses for its kind (witnessed against the
// JOTAC JO install): text bins → language, menus/defs/missions → localres,
// terrain/env/art → resource. This is the generate-at-package step
// (build_terrain is ~35 s and emits ~10 MB / 685 files) — NOT committed, NOT
// run per build: it SKIPS clean unless OPENNOVA_BUILD_MINIMAL_PFF=1. See
// fixtures/minimal/README.md.
#include "minimal_fnt_builder.h"
#include "minimal_mus_builder.h"

#include <fnt/fnt.h>
#include <pff/pff.h>
#include <terrain/builder.h>

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

std::string dir() { return MINIMAL_FIXTURE_DIR; }

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

// Committed sources (fixtures/minimal/resources/) per boot-table archive,
// mirroring retail's placement: gameerr/gametext/vmacros/keyhelp ship in
// retail language.pff; every .mnu/.def/.bms in retail localres.pff;
// .env/.trn/terrain art in retail resource.pff.
// The mission-family companions follow the witnessed retail placement:
// <stem>.bin/.pcx/.lwf in language.pff; <stem>.dbf (like .bms) in
// localres.pff. (.til/.wac stay unauthored: the .til loader runs in the
// render loop only and .wac is a witnessed silent skip.)
const char *kLanguage[] = {"gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin",
                           "menutxt.bin", "mnml.bin",     "mnml.pcx",    "mnml.lwf"};
const char *kLocalres[] = {"items.def", "weapon.def", "ammo.def",
                           "main.mnu",  "mp.mnu",     "mnml.bms",
                           "menu_style.mns", "newarow1.tga", "mnml.dbf"};

// The boot font set is HARDCODED by name [orig: HUD_InitAllFonts @ 0x51ee20:
// width breakpoints 640/800/1024 pick Arial12b/14n, 14b/14n, or 16b/16n;
// Impac22b/38b always] plus the menu_style.mns DEF_FONTNAME_* names. Every
// name gets the same generated glyph set (minimal_fnt_builder.h); retail
// ships its .fnt files in localres.pff.
const char *kFonts[] = {"Arial12b.fnt", "Arial14n.fnt", "Arial14b.fnt", "Arial16n.fnt",
                        "Arial16b.fnt", "Impac22b.fnt", "Impac38b.fnt"};
const char *kResource[] = {"mnml.env",   "mnml.trn",   "mnml_c.tga", "mnml_dm.tga",
                           "mnml_dc1.tga", "mnml_t.tga", "mnml_m.pcx", "mnml_f.pcx"};

// Bundle (pff_name, filepath) pairs into <fixtures/minimal>/<archive>, then
// re-open it and require every `expect` entry to resolve.
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

} // namespace

int main() {
	if (!std::getenv("OPENNOVA_BUILD_MINIMAL_PFF")) {
		std::printf("[skip] set OPENNOVA_BUILD_MINIMAL_PFF=1 to build the ~10 MB terrain + PFFs\n");
		return 0; // generate-at-package: not run per build
	}

	const fs::path root(dir());
	const fs::path work = root / "_pff_build"; // gitignored scratch
	fs::create_directories(work);

	// 1) Flat 1024x1024 depthmap -> build_terrain -> mnml.cpt + tiles into work/.
	{
		std::vector<uint8_t> flat(1024 * 1024, 32);
		std::ofstream f(work / "mnml_depth.raw", std::ios::binary);
		f.write(reinterpret_cast<const char *>(flat.data()), static_cast<std::streamsize>(flat.size()));
	}
	opennova::TpjProject proj;
	proj.terrain_name = "mnml";
	proj.depthmap = (work / "mnml_depth.raw").string();
	proj.output = "mnml";
	std::printf("building terrain (this takes ~35 s)...\n");
	try {
		opennova::build_terrain(proj, work.string());
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: build_terrain: %s\n", e.what());
		return 1;
	}

	// 2) Generate the boot font set: one authored glyph page written under every
	//    hardcoded font name (minimal_fnt_builder.h). Generated, never committed.
	const fs::path fonts_dir = work / "fonts";
	fs::create_directories(fonts_dir);
	{
		fnt_font_t font{};
		if (minimal_fnt::build_font(&font) != FNT_OK) {
			std::fprintf(stderr, "FAIL: minimal_fnt build_font\n");
			return 1;
		}
		const size_t font_size = fnt_calculate_file_size(font.num_pages);
		std::vector<uint8_t> font_bytes(font_size);
		size_t written = 0;
		if (fnt_write(&font, font_bytes.data(), font_bytes.size(), &written) != FNT_OK ||
		    written != font_size) {
			std::fprintf(stderr, "FAIL: fnt_write\n");
			fnt_free(&font);
			return 1;
		}
		fnt_free(&font);
		for (const char *n : kFonts) {
			std::ofstream f(fonts_dir / n, std::ios::binary);
			f.write(reinterpret_cast<const char *>(font_bytes.data()),
			        static_cast<std::streamsize>(font_bytes.size()));
		}
		std::printf("generated %zu-byte font x%zu names\n", font_size,
		            sizeof(kFonts) / sizeof(kFonts[0]));
	}

	// 3) Generate the boot music set: the silent bank under both hardcoded
	//    .sbf names LOOSE at the root (retail ships the banks loose in the
	//    game dir) and the minimal scripts for localres.pff
	//    [orig: Expansion_LoadAssets @ 0x4a4730: MENUMUS.* / GAMEMUS.*].
	const fs::path mus_dir = work / "mus";
	fs::create_directories(mus_dir);
	{
		std::vector<uint8_t> bank;
		if (minimal_mus::build_silent_sbf(&bank) != 0) {
			std::fprintf(stderr, "FAIL: build_silent_sbf\n");
			return 1;
		}
		for (const char *n : {"menumus.sbf", "gamemus.sbf"}) {
			std::ofstream f(root / n, std::ios::binary);
			f.write(reinterpret_cast<const char *>(bank.data()),
			        static_cast<std::streamsize>(bank.size()));
		}
		for (const char *n : {"menumus", "gamemus"}) {
			std::vector<uint8_t> bin;
			if (minimal_mus::build_minimal_mus(n, &bin) != 0) {
				std::fprintf(stderr, "FAIL: build_minimal_mus(%s)\n", n);
				return 1;
			}
			std::ofstream f(mus_dir / (std::string(n) + ".bin"), std::ios::binary);
			f.write(reinterpret_cast<const char *>(bin.data()),
			        static_cast<std::streamsize>(bin.size()));
		}
		std::printf("generated menumus/gamemus .sbf (loose, %zu bytes) + .bin scripts\n",
		            bank.size());
	}

	// 4) Per-archive file lists: committed sources (resources/) by retail
	//    placement; the generated fonts + music scripts ride localres.pff and
	//    the generated terrain rides resource.pff (excluding the depthmap
	//    build input).
	const fs::path sources = root / "resources";
	std::vector<std::pair<std::string, fs::path>> language, localres, resource;
	for (const char *n : kLanguage) language.emplace_back(n, sources / n);
	for (const char *n : kLocalres) localres.emplace_back(n, sources / n);
	for (const char *n : kFonts) localres.emplace_back(n, fonts_dir / n);
	for (const char *n : {"menumus.bin", "gamemus.bin"}) localres.emplace_back(n, mus_dir / n);
	for (const char *n : kResource) resource.emplace_back(n, sources / n);
	for (const fs::directory_entry &de : fs::directory_iterator(work)) {
		if (!de.is_regular_file()) continue;
		const std::string fn = de.path().filename().string();
		if (fn == "mnml_depth.raw") continue;
		resource.emplace_back(fn, de.path());
	}

	// 5) Write + verify each boot-table archive: the boot bins in language, the
	//    mission/menu/font/music set in localres, the map polydata in resource.
	if (!write_and_verify(root, "language.pff", language,
	                      {"gameerr.bin", "gametext.bin", "vmacros.bin", "keyhelp.bin",
	                       "menutxt.bin", "mnml.bin", "mnml.pcx", "mnml.lwf"}))
		return 1;
	if (!write_and_verify(root, "localres.pff", localres,
	                      {"items.def", "main.mnu", "mp.mnu", "mnml.bms", "menu_style.mns",
	                       "Arial16n.fnt", "Impac38b.fnt", "menumus.bin", "gamemus.bin",
	                       "newarow1.tga", "mnml.dbf"}))
		return 1;
	if (!write_and_verify(root, "resource.pff", resource, {"mnml.env", "mnml.trn", "mnml.cpt"}))
		return 1;

	std::printf("OK: language/localres/resource.pff bundle the minimal set "
	            "(fatal + map + terrain) under the witnessed boot-table names\n");
	return 0;
}
