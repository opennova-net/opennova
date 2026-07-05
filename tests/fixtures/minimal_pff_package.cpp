// The minimal set's packaging tool: generate the terrain, then bundle the whole
// authored set into ONE PFF so the fatal not-all-archives-missing gate clears
// [orig: PFF_OpenAllArchives @ 0x4a4310]. This is the generate-at-package step
// (build_terrain is ~35 s and emits ~10 MB / 685 files) — NOT committed, NOT run
// per build: it SKIPS clean unless OPENNOVA_BUILD_MINIMAL_PFF=1. When run it
// writes <fixtures/minimal>/mnml.pff (gitignored) and verifies it opens with the
// fatal set present. See fixtures/minimal/README.md.
#include <pff/pff.h>
#include <terrain/builder.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// The committed source files that go into the PFF verbatim.
const char *kCommitted[] = {
    "gametext.bin", "vmacros.bin", "keyhelp.bin", "items.def",  "weapon.def", "ammo.def",
    "main.mnu",     "mp.mnu",      "mnml.env",    "mnml.bms",    "mnml.trn",   "mnml_c.tga",
    "mnml_dm.tga",  "mnml_dc1.tga", "mnml_t.tga", "mnml_m.pcx",  "mnml_f.pcx"};

} // namespace

int main() {
	if (!std::getenv("OPENNOVA_BUILD_MINIMAL_PFF")) {
		std::printf("[skip] set OPENNOVA_BUILD_MINIMAL_PFF=1 to build the ~10 MB terrain + PFF\n");
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

	// 2) Build one (pff_name, filepath) list: committed set + generated terrain
	//    (excluding the depthmap build input).
	std::vector<std::pair<std::string, fs::path>> files;
	for (const char *n : kCommitted) files.emplace_back(n, root / n);
	for (const fs::directory_entry &de : fs::directory_iterator(work)) {
		if (!de.is_regular_file()) continue;
		const std::string fn = de.path().filename().string();
		if (fn == "mnml_depth.raw") continue;
		files.emplace_back(fn, de.path());
	}

	// 3) Read every payload (stable storage) + build entries with stable name/data.
	std::vector<std::vector<uint8_t>> storage(files.size());
	std::vector<PffWriteEntry> entries;
	entries.reserve(files.size());
	for (size_t i = 0; i < files.size(); ++i) {
		if (files[i].first.size() > 16) { // PFF_NAME_SIZE
			std::fprintf(stderr, "FAIL: name too long for PFF: %s\n", files[i].first.c_str());
			return 1;
		}
		if (!read_bytes(files[i].second, storage[i])) {
			std::fprintf(stderr, "FAIL: missing %s\n", files[i].second.string().c_str());
			return 1;
		}
		PffWriteEntry e{};
		e.name = files[i].first.c_str();
		e.data = storage[i].data();
		e.size = static_cast<uint32_t>(storage[i].size());
		entries.push_back(e);
	}

	// 4) Write the PFF.
	const std::string pff = (root / "mnml.pff").string();
	const int rc = pff_write_archive(pff.c_str(), PFF_FORMAT_PFF3, entries.data(),
	                                 static_cast<uint32_t>(entries.size()));
	if (rc != PFF_WRITE_OK) {
		std::fprintf(stderr, "FAIL: pff_write_archive rc=%d\n", rc);
		return 1;
	}
	std::printf("wrote %s (%zu entries)\n", pff.c_str(), entries.size());

	// 5) Verify it opens and the fatal + map entries are present.
	PffArchive ar{};
	if (pff_open(&ar, pff.c_str()) != 0) {
		std::fprintf(stderr, "FAIL: pff_open on the written archive\n");
		return 1;
	}
	int missing = 0;
	for (const char *n : {"gametext.bin", "items.def", "mnml.bms", "mnml.trn", "mnml.cpt"}) {
		if (!pff_find(&ar, n)) {
			std::fprintf(stderr, "FAIL: fatal/map entry absent from PFF: %s\n", n);
			++missing;
		}
	}
	pff_close(&ar);
	if (missing) return 1;

	std::printf("OK: mnml.pff bundles the minimal set (fatal + map + terrain), opens clean\n");
	return 0;
}
