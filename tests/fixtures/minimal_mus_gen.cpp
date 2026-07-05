// Guard for the minimal set's generated music (minimal_mus_builder.h): the
// silent SBF bank and the minimal MUS script the packaging step emits under
// the hardcoded boot music names [orig: Expansion_LoadAssets @ 0x4a4730:
// MENUMUS.SBF/BIN + GAMEMUS.SBF/BIN]. Always-on; nothing is committed.
#include "minimal_mus_builder.h"

#include <mus/mus.h>
#include <sbf/sbf.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

} // namespace

int main() {
	int failures = 0;

	// The silent bank parses back with its one named entry.
	std::vector<uint8_t> sbf;
	failures += !expect(minimal_mus::build_silent_sbf(&sbf) == 0, "build_silent_sbf");
	failures += !expect(sbf_validate(sbf.data(), sbf.size()) == 0, "sbf_validate");
	SbfArchive arc = {};
	failures += !expect(sbf_open_memory(&arc, sbf.data(), sbf.size()) == 0, "sbf_open_memory");
	failures += !expect(arc.header.entry_count == 1, "one bank entry");
	failures += !expect(std::strncmp(arc.entries[0].name, "silence", 7) == 0, "entry name");
	sbf_close(&arc);

	// Deterministic output (byte-stable across rebuilds).
	std::vector<uint8_t> sbf2;
	failures += !expect(minimal_mus::build_silent_sbf(&sbf2) == 0 && sbf2 == sbf,
	                    "sbf output is deterministic");

	// Both scripts compile, encode, and re-parse with their boot names.
	for (const char *name : {"menumus", "gamemus"}) {
		std::vector<uint8_t> bin;
		char msg[64];
		std::snprintf(msg, sizeof(msg), "build_minimal_mus(%s)", name);
		if (!expect(minimal_mus::build_minimal_mus(name, &bin) == 0, msg)) {
			++failures;
			continue;
		}
		MusFile mf = {};
		std::snprintf(msg, sizeof(msg), "mus_open_memory(%s)", name);
		if (!expect(mus_open_memory(&mf, bin.data(), bin.size()) == 0, msg)) {
			++failures;
			continue;
		}
		failures += !expect(mf.header.magic == MUS_MAGIC_SCR0, "SCR0 magic");
		failures += !expect(mf.header.chunk_count == 1, "one script chunk");
		failures += !expect(mf.scripts != nullptr &&
		                        std::strncmp(mf.scripts[0].name, name, std::strlen(name)) == 0,
		                    "script name preserved");
		mus_close(&mf);
	}

	if (failures == 0) std::printf("OK: minimal music bank + scripts valid + reproducible\n");
	return failures == 0 ? 0 : 1;
}
