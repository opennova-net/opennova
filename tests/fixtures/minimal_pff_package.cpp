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

// Committed sources per boot-table archive, mirroring retail's placement:
// gametext/vmacros/keyhelp ship in retail language.pff; every .mnu/.def/.bms in
// retail localres.pff; .env/.trn/terrain art in retail resource.pff.
const char *kLanguage[] = {"gametext.bin", "vmacros.bin", "keyhelp.bin"};
const char *kLocalres[] = {"items.def", "weapon.def", "ammo.def", "main.mnu", "mp.mnu", "mnml.bms"};
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

	// 2) Per-archive file lists: committed sources by retail placement; the
	//    generated terrain rides resource.pff (excluding the depthmap build input).
	std::vector<std::pair<std::string, fs::path>> language, localres, resource;
	for (const char *n : kLanguage) language.emplace_back(n, root / n);
	for (const char *n : kLocalres) localres.emplace_back(n, root / n);
	for (const char *n : kResource) resource.emplace_back(n, root / n);
	for (const fs::directory_entry &de : fs::directory_iterator(work)) {
		if (!de.is_regular_file()) continue;
		const std::string fn = de.path().filename().string();
		if (fn == "mnml_depth.raw") continue;
		resource.emplace_back(fn, de.path());
	}

	// 3) Write + verify each boot-table archive: the fatal bins in language, the
	//    mission set in localres, the map polydata in resource.
	if (!write_and_verify(root, "language.pff", language, {"gametext.bin", "vmacros.bin", "keyhelp.bin"}))
		return 1;
	if (!write_and_verify(root, "localres.pff", localres, {"items.def", "main.mnu", "mp.mnu", "mnml.bms"}))
		return 1;
	if (!write_and_verify(root, "resource.pff", resource, {"mnml.env", "mnml.trn", "mnml.cpt"}))
		return 1;

	std::printf("OK: language/localres/resource.pff bundle the minimal set "
	            "(fatal + map + terrain) under the witnessed boot-table names\n");
	return 0;
}
