// Generator + guard for the minimal set's RTXT string tables
// (fixtures/minimal/gametext.bin, vmacros.bin, keyhelp.bin) — the fatal-set
// strings the engine loads at boot [orig: Game_InitSubsystems @ 0x4A6CD0:
// gametext @ 0x4a6fed, vmacros @ 0x4a702f, keyhelp @ 0x4a7072]. Each fatal
// file needs only to LOAD as a valid RTXT (missing keys fall back to literals
// in the engine), so these are minimal-but-valid tables authored from scratch
// by our own writer — no retail asset. See fixtures/minimal/README.md.
//
// Default: regenerate each table in memory, assert it byte-matches the
// committed file and round-trips (parse->write is byte-stable). Run with
// OPENNOVA_WRITE_MINIMAL_FIXTURES=1 to (re)write the committed files.
#include <rtxt/rtxt.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace opennova::rtxt;

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

// A minimal-but-valid table: one section, its keys grouped contiguously. The
// content is a small placeholder set — it grows only when retail validation
// shows a fallback literal that reads wrong (documented in the README).
File make_table(const std::string &section, const std::vector<std::pair<std::string, std::string>> &kv) {
	File f;
	f.sections.push_back({section, static_cast<uint32_t>(kv.size())});
	for (const auto &pair : kv) {
		Entry e;
		e.key = pair.first;
		e.text = pair.second;
		e.section_index = 0;
		f.entries.push_back(e);
	}
	return f;
}

// The three fatal tables. gametext carries a couple of the boot/host keys the
// menu path references (the rest fall back to literals); vmacros and keyhelp
// are valid-but-empty (voice macros / keyboard help are non-essential to boot).
File gametext_table() {
	return make_table("Server", {
	                                {"STRSRV_MEDREQ", "Medic!"},
	                                {"STR_HOST", "Host Game"},
	                                {"STR_JOIN", "Join Game"},
	                            });
}
File vmacros_table() { return File{}; }
File keyhelp_table() { return File{}; }

bool read_file(const std::string &path, std::vector<uint8_t> &out) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) return false;
	const std::streamoff sz = f.tellg();
	if (sz < 0) return false;
	f.seekg(0);
	out.resize(static_cast<size_t>(sz));
	if (!out.empty()) f.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
	return true;
}

struct Target {
	const char *name;
	File (*build)();
};

int run(const std::string &dir, bool write_mode) {
	const Target targets[] = {
	    {"gametext.bin", gametext_table},
	    {"vmacros.bin", vmacros_table},
	    {"keyhelp.bin", keyhelp_table},
	};
	int failures = 0;
	for (const Target &t : targets) {
		const std::string path = dir + "/" + t.name;
		File f = t.build();
		std::vector<uint8_t> bytes;
		std::string err;
		if (!expect(write(f, bytes, err), (std::string("write ") + t.name + ": " + err).c_str())) {
			++failures;
			continue;
		}
		// Round-trip: the emitted bytes parse and re-write identically.
		File reparsed;
		if (!expect(parse(bytes.data(), bytes.size(), reparsed, err),
		            (std::string("parse ") + t.name + ": " + err).c_str())) {
			++failures;
			continue;
		}
		std::vector<uint8_t> rewritten;
		if (!expect(write(reparsed, rewritten, err) && rewritten == bytes,
		            (std::string(t.name) + " is not byte-stable across a round-trip").c_str())) {
			++failures;
			continue;
		}
		if (write_mode) {
			std::ofstream o(path, std::ios::binary);
			o.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			std::printf("wrote %s (%zu bytes)\n", path.c_str(), bytes.size());
		} else {
			std::vector<uint8_t> committed;
			if (!expect(read_file(path, committed),
			            (std::string("committed ") + t.name + " missing — run with "
			                                                  "OPENNOVA_WRITE_MINIMAL_FIXTURES=1")
			                .c_str())) {
				++failures;
				continue;
			}
			if (!expect(committed == bytes,
			            (std::string(t.name) + " committed bytes differ from the writer output — "
			                                    "regenerate with OPENNOVA_WRITE_MINIMAL_FIXTURES=1")
			                .c_str()))
				++failures;
		}
	}
	return failures;
}

} // namespace

int main() {
#ifndef MINIMAL_FIXTURE_DIR
#define MINIMAL_FIXTURE_DIR "."
#endif
	const bool write_mode = std::getenv("OPENNOVA_WRITE_MINIMAL_FIXTURES") != nullptr;
	const int failures = run(MINIMAL_FIXTURE_DIR, write_mode);
	if (failures == 0) std::printf("OK: minimal RTXT fatal-set tables valid + byte-reproducible\n");
	return failures == 0 ? 0 : 1;
}
