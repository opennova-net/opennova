// Generator + guard for the synthetic MUS programs fixtures/mus/synth_gamemus.bin,
// fixtures/mus/synth_menumus.bin and the decompile golden
// fixtures/mus/golden_synth_gamemus.mus.txt: two interactive-music scripts
// authored in the MDEdit source dialect, compiled by mus_compile and written by
// mus_encode_file (the SCR0/MU01 container with the canonical eleven intrinsic
// names and the editor debug export table, so section names and the source
// path reload as authored). No retail program is carried: the shipped
// jo_gamemus.bin / jo_menumus.bin and their decompile golden are the
// reference-tree legs of the mus ctests (docs/asset-gated-tests.md).
//
//  * gamescript has the shipped gamemus SHAPE: eight sections in the same
//    order (Begin, Missionnull, Missionwin, Win000, Missionlose, Lose000,
//    Testmission, Multiplayerstart), the master-volume call, the Var01-gated
//    if/else that routes a fresh start into Multiplayerstart and an active
//    mission into the silent Missionnull self-loop, an on-switch, a method
//    call, and play runs over sound_0..sound_6 (inside the thirteen-entry
//    synth_gamemus.sbf).
//  * menuscript is a Var02-dispatched attract/idle/browse/ambient state
//    machine: every branch loops through setstate, so the VM never runs off
//    the end.
//
// Default: rebuild both programs in memory, prove they reload with their
// names, that the gamescript golden is the decompiler's own output and a
// compile fixed point, and byte-compare the three committed files. `--write`
// (re)writes them.
#include "common/test_paths.h"

#include <formats/mus/mus.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

const char *const kGameSource =
	"// Script: gamescript\n"
	"// Original source: C:\\opennova\\fixtures\\mus\\synth_gamemus.mus\n"
	"script gamescript\n"
	"section Begin\n"
	"{\n"
	"  SV(200)\n"
	"  enter Testmission\n"
	"}\n"
	"section Missionnull\n"
	"{\n"
	"  enter Missionnull\n"
	"}\n"
	"section Missionwin\n"
	"{\n"
	"  on (Var02) enter Win000 Lose000 Missionnull\n"
	"}\n"
	"section Win000\n"
	"{\n"
	"  play sound_2\n"
	"  play sound_3\n"
	"  play sound_4\n"
	"  enter Missionnull\n"
	"}\n"
	"section Missionlose\n"
	"{\n"
	"  enter Lose000\n"
	"}\n"
	"section Lose000\n"
	"{\n"
	"  play sound_5\n"
	"  play sound_6\n"
	"  FB()\n"
	"  enter Missionnull\n"
	"}\n"
	"section Testmission\n"
	"{\n"
	"  if ((Var01 != 0))\n"
	"  {\n"
	"    enter Missionnull\n"
	"  }\n"
	"  else\n"
	"  {\n"
	"    enter Multiplayerstart\n"
	"  }\n"
	"}\n"
	"section Multiplayerstart\n"
	"{\n"
	"  play sound_0\n"
	"  play sound_0\n"
	"  play sound_0\n"
	"  play sound_1\n"
	"  enter Missionnull\n"
	"}\n";

const char *const kMenuSource =
	"// Script: menuscript\n"
	"// Original source: C:\\opennova\\fixtures\\mus\\synth_menumus.mus\n"
	"script menuscript\n"
	"section Begin\n"
	"{\n"
	"  SV(200)\n"
	"  enter Attract\n"
	"}\n"
	"section Attract\n"
	"{\n"
	"  play sound_1\n"
	"  on (Var02) enter Idle Browse Ambient\n"
	"}\n"
	"section Idle\n"
	"{\n"
	"  play sound_2\n"
	"  play sound_0\n"
	"  enter Idle\n"
	"}\n"
	"section Browse\n"
	"{\n"
	"  play sound_2\n"
	"  play sound_3\n"
	"  play sound_4\n"
	"  enter Browse\n"
	"}\n"
	"section Ambient\n"
	"{\n"
	"  SV(200)\n"
	"  play sound_2\n"
	"  enter Ambient\n"
	"}\n";

struct Program {
	const char *name;
	const char *source;
	const char *file;
	uint32_t section_count;
	const char *entry_name;
	const char *last_name;
};

const Program kPrograms[] = {
	{"gamescript", kGameSource, "synth_gamemus.bin", 8, "Begin", "Multiplayerstart"},
	{"menuscript", kMenuSource, "synth_menumus.bin", 5, "Begin", "Ambient"},
};

bool compile(const char *source, MusScript &out, std::string &err) {
	std::memset(&out, 0, sizeof(out));
	int line = 0, col = 0;
	const char *msg = nullptr;
	if (mus_compile(source, &out, &line, &col, &msg) != 0) {
		err = std::string("compile error at ") + std::to_string(line) + ":" + std::to_string(col) + ": " +
		      (msg ? msg : "?");
		return false;
	}
	return true;
}

bool decompile(const MusScript &script, std::string &text, std::string &err) {
	const int needed = mus_decompile(&script, nullptr, 0);
	if (needed <= 0) {
		err = "decompile size query failed";
		return false;
	}
	std::vector<char> buf(static_cast<size_t>(needed) + 1);
	if (mus_decompile(&script, buf.data(), buf.size()) != needed) {
		err = "decompile write failed";
		return false;
	}
	text.assign(buf.data(), static_cast<size_t>(needed));
	return true;
}

// Compile + encode one program, reload the bytes and pin the author-facing
// shape (script name, section count and names, the source path, the eleven
// intrinsic names).
bool build(const Program &p, std::vector<uint8_t> &bytes, std::string &err) {
	MusScript script;
	if (!compile(p.source, script, err)) return false;
	const MusScript *arr[1] = {&script};
	uint8_t *buf = nullptr;
	size_t size = 0;
	const bool encoded = mus_encode_file(arr, 1, &buf, &size) == 0;
	mus_script_free(&script);
	if (!encoded) {
		err = "mus_encode_file failed";
		return false;
	}
	bytes.assign(buf, buf + size);
	mus_free(buf);

	MusFile back;
	if (mus_open_memory(&back, bytes.data(), bytes.size()) != 0) {
		err = "the written program does not open";
		return false;
	}
	const MusScript &s = back.scripts[0];
	const bool shape_ok = back.header.chunk_count == 1 && back.intrinsic_count == MUS_INTRINSIC_NAMES &&
	                      std::strncmp(s.name, p.name, MUS_NAME_SIZE) == 0 && s.section_count == p.section_count &&
	                      s.entry_section_index == 0 && std::strcmp(s.sections[0].name, p.entry_name) == 0 &&
	                      std::strcmp(s.sections[p.section_count - 1].name, p.last_name) == 0 &&
	                      std::strstr(s.source_path, "synth_") != nullptr &&
	                      std::strcmp(back.intrinsic_names[0], "GEcho") == 0 &&
	                      std::strcmp(back.intrinsic_names[10], "TStop") == 0;
	mus_close(&back);
	if (!shape_ok) {
		err = std::string(p.file) + ": the reloaded program lost an authored name";
		return false;
	}
	return true;
}

// The gamescript golden: the decompiler's text for the minted program, and a
// compile fixed point (decompile(compile(golden)) == golden), so mus_decompile
// and mus_roundtrip pin the emitter over it.
bool build_golden(const std::vector<uint8_t> &game_bytes, std::string &golden, std::string &err) {
	MusFile file;
	if (mus_open_memory(&file, game_bytes.data(), game_bytes.size()) != 0) {
		err = "synth_gamemus.bin does not open";
		return false;
	}
	const bool ok = decompile(file.scripts[0], golden, err);
	mus_close(&file);
	if (!ok) return false;
	MusScript again;
	if (!compile(golden.c_str(), again, err)) {
		err = "the golden does not recompile: " + err;
		return false;
	}
	std::string text2;
	const bool ok2 = decompile(again, text2, err);
	mus_script_free(&again);
	if (!ok2) return false;
	if (text2 != golden) {
		err = "decompile(compile(golden)) differs from the golden";
		return false;
	}
	return true;
}

using test_io::read_file;

bool write_file(const std::string &path, const void *data, size_t size) {
	std::ofstream o(path, std::ios::binary);
	if (!o) return false;
	o.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
	return static_cast<bool>(o);
}

bool is_lfs_pointer(const std::vector<uint8_t> &bytes) {
	static const char kLfsSentinel[] = "version https://git-lfs";
	return bytes.size() >= sizeof(kLfsSentinel) - 1 &&
	       std::memcmp(bytes.data(), kLfsSentinel, sizeof(kLfsSentinel) - 1) == 0;
}

// Byte-compare one committed file against its generator output; an unpulled
// LFS pointer is reported and passes.
bool guard(const std::string &path, const std::vector<uint8_t> &bytes) {
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed), ("committed file missing; run with --write: " + path).c_str()))
		return false;
	if (is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return true;
	}
	return expect(committed == bytes, ("differs from the generator output; regenerate with --write: " + path).c_str());
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/mus/";

	std::vector<uint8_t> game, menu;
	std::string err;
	if (!expect(build(kPrograms[0], game, err), ("synth_gamemus.bin: " + err).c_str())) return 1;
	if (!expect(build(kPrograms[1], menu, err), ("synth_menumus.bin: " + err).c_str())) return 1;
	std::string golden;
	if (!expect(build_golden(game, golden, err), ("golden_synth_gamemus.mus.txt: " + err).c_str())) return 1;
	const std::vector<uint8_t> golden_bytes(golden.begin(), golden.end());

	const std::string game_path = dir + kPrograms[0].file;
	const std::string menu_path = dir + kPrograms[1].file;
	const std::string golden_path = dir + "golden_synth_gamemus.mus.txt";
	if (write_mode) {
		if (!expect(write_file(game_path, game.data(), game.size()), ("cannot write " + game_path).c_str())) return 1;
		if (!expect(write_file(menu_path, menu.data(), menu.size()), ("cannot write " + menu_path).c_str())) return 1;
		if (!expect(write_file(golden_path, golden.data(), golden.size()), ("cannot write " + golden_path).c_str()))
			return 1;
		std::printf("wrote %s (%zu bytes), %s (%zu bytes), %s (%zu bytes)\n", game_path.c_str(), game.size(),
		            menu_path.c_str(), menu.size(), golden_path.c_str(), golden.size());
		return 0;
	}
	bool ok = guard(game_path, game);
	ok = guard(menu_path, menu) && ok;
	ok = guard(golden_path, golden_bytes) && ok;
	if (!ok) return 1;
	std::printf("OK: fixtures/mus synth programs + golden byte-reproducible (%zu + %zu + %zu bytes)\n", game.size(),
	            menu.size(), golden.size());
	return 0;
}
