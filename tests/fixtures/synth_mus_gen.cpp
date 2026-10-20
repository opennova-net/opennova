// Generator + guard for the synthetic MUS programs fixtures/mus/synth_gamemus.bin,
// fixtures/mus/synth_menumus.bin, fixtures/mus/synth_forms.bin and the decompile
// goldens fixtures/mus/golden_synth_gamemus.mus.txt and
// fixtures/mus/golden_synth_forms.mus.txt: three interactive-music scripts
// authored in the MDEdit source dialect, compiled by mus_compile and written by
// mus_encode_file (the SCR0/MU01 container with the canonical eleven intrinsic
// names and the editor debug export table, so section names and the source
// path reload as authored). The compiler writes MDEdit's layout (every shipped
// script, decompiled and compiled again, is its own bytes: mus_encode_idempotence
// proved it over the install's four), so the minted programs hold MDEdit's
// bytecode for every construct they use. No retail program is carried.
//
//  * gamescript has the shipped gamemus SHAPE: eight sections in the same
//    order (Begin, Missionnull, Missionwin, Win000, Missionlose, Lose000,
//    Testmission, Multiplayerstart), the master-volume call, the Var01-gated
//    if/else that routes a fresh start into Multiplayerstart and an active
//    mission into the silent Missionnull self-loop, an on-switch, a method
//    call, and play runs over sound_0..sound_6 (inside the thirteen-entry
//    synth_gamemus.sbf); and its MessageHandler function, the chunk's +0x40
//    entry, between Begin and Missionnull where the shipped one stands.
//  * menuscript is a Var02-dispatched attract/idle/browse/ambient state
//    machine: every branch loops through setstate, so the VM never runs off
//    the end.
//  * the forms program is a menuscript in the form the shipped menu programs
//    take (forms_source below): the constructs theirs and the pair's not.
//
// Default: rebuild the programs in memory, prove they reload with their
// names, that the gamescript golden is the decompiler's own output and a
// compile fixed point and the forms program decompiles to its own source, and
// byte-compare the five committed files. `--write` (re)writes them.
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

using namespace opennova::mus;

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
	"handler MessageHandler(msgtype, source)\n"
	"{\n"
	"  FB()\n"
	"  on (msgtype) enter Missionnull Missionwin Missionlose\n"
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

// The forms program, fixtures/mus/synth_forms.bin: a menuscript of eleven sections in the form the
// shipped menu programs take (menumus.bin and jox01's MJox01.bin, both MDEdit's), each form theirs and
// the minted pair's not: a global of the script's own past MDEdit's Var00..Var15 (`global INT`, the
// globals 0x44 bytes), assignments to a Var and to it (pop_g, 0x08), `==` (equal, 0x20) and `&&`
// (l_and, 0x15) in an if's condition, an if inside an if's body, an on-switch naming one section
// three times, and plays past sound 255 through the word-wide play (0x3D [orig: AudioVM_Op_PlayWait @
// 0x672C90]; MJox01.bin plays 61 of its 884 so, its bank 317 streams). Its text is the decompiler's own
// layout, each statement on the line MDEdit's line table names with no `#line` (the shipped layout,
// whose source binds every sound and declares every section ahead of the code), so the program
// decompiles to this text exactly.
std::string forms_source() {
	std::string s =
		"// Script: menuscript\n"
		"// Original source: C:\\opennova\\fixtures\\mus\\synth_forms.mus\n"
		"//\n"
		"// IMPORTANT: Load MUSIC.LAN before opening this file in MDEdit\n"
		"// The following methods are used: GEcho, GGRnd, GSV, GSDV, GFB, F.Set, F.Clear, F.IsSet, F.IsClear, "
		"T.Start, T.Stop\n"
		"\n"
		"script menuscript\n"
		"\n"
		"// Sound bind declarations (play requires bind identifiers)\n";
	for (int i = 0; i <= 316; ++i) s += "bind sound_" + std::to_string(i) + " \"sound_" + std::to_string(i) + "\"\n";
	s += "\n"
	     "// Global variables\n"
	     "// Used global offsets from original: [0, 8, 56, 60, 64]\n"
	     "// Original globals_area = 68 bytes\n"
	     "// NOTE: MDEdit pre-defines Var00-Var15 at offsets 0-60 (64 bytes)\n"
	     "global INT counter\n"
	     "\n"
	     "// Section forward declarations\n";
	const char *const sections[] = {"Begin", "Poll",    "Title", "Cycle", "Route", "Fault",
	                                "Setup", "Profile", "Solo",  "Lobby", "Server"};
	for (const char *section : sections) s += std::string("declsection ") + section + "\n";
	s += "\n"
	     "section Begin\n"
	     "{\n"
	     "Var14 = 0\n"
	     "counter = 0\n"
	     "SV(250)\n"
	     "enter Poll\n"
	     "}\n"
	     "\n"
	     "section Poll\n"
	     "{\n"
	     "on (Var00) enter Title Title Title\n"
	     "enter Title\n"
	     "}\n"
	     "\n"
	     "section Title\n"
	     "{\n"
	     "SV(250)\n"
	     "if ((Var14 == 0))\n"
	     "{\n"
	     "    Var14 = 1\n"
	     "    play sound_1\n"
	     "}\n"
	     "else\n"
	     "{\n"
	     "    enter Cycle\n"
	     "}\n"
	     "enter Poll\n"
	     "}\n"
	     "\n"
	     "section Cycle\n"
	     "{\n"
	     "SV(250)\n"
	     "play sound_2\n"
	     "if ((Var02 != 1))\n"
	     "{\n"
	     "    enter Route\n"
	     "}\n"
	     "play sound_3\n"
	     "if ((Var02 != 1))\n"
	     "{\n"
	     "    enter Route\n"
	     "}\n"
	     "play sound_256\n"
	     "if ((Var02 != 1))\n"
	     "{\n"
	     "    enter Route\n"
	     "}\n"
	     "enter Cycle\n"
	     "}\n"
	     "\n"
	     "section Route\n"
	     "{\n"
	     "if ((Var02 == 0))\n"
	     "{\n"
	     "    enter Fault\n"
	     "}\n"
	     "if ((Var02 == 9))\n"
	     "{\n"
	     "    play sound_12\n"
	     "    enter Setup\n"
	     "}\n"
	     "if ((Var02 == 10))\n"
	     "{\n"
	     "    enter Profile\n"
	     "}\n"
	     "if ((Var02 == 11))\n"
	     "{\n"
	     "    enter Solo\n"
	     "}\n"
	     "if ((Var02 == 5))\n"
	     "{\n"
	     "    enter Lobby\n"
	     "}\n"
	     "enter Server\n"
	     "}\n"
	     "\n"
	     "section Fault\n"
	     "{\n"
	     "play sound_4\n"
	     "enter Poll\n"
	     "}\n"
	     "\n"
	     "section Setup\n"
	     "{\n"
	     "SV(250)\n"
	     "play sound_5\n"
	     "if ((Var02 != 9))\n"
	     "{\n"
	     "    play sound_6\n"
	     "    enter Route\n"
	     "}\n"
	     "enter Setup\n"
	     "}\n"
	     "\n"
	     "section Profile\n"
	     "{\n"
	     "play sound_300\n"
	     "if ((Var02 != 10))\n"
	     "{\n"
	     "    enter Route\n"
	     "}\n"
	     "enter Profile\n"
	     "}\n"
	     "\n"
	     "section Solo\n"
	     "{\n"
	     "play sound_7\n"
	     "if ((Var02 != 11))\n"
	     "{\n"
	     "    play sound_8\n"
	     "    enter Route\n"
	     "}\n"
	     "enter Solo\n"
	     "}\n"
	     "\n"
	     "section Lobby\n"
	     "{\n"
	     "SV(250)\n"
	     "if ((Var15 == 0))\n"
	     "{\n"
	     "    Var15 = 1\n"
	     "    play sound_9\n"
	     "    if (((Var02 != 5) && (Var02 != 6)))\n"
	     "    {\n"
	     "        play sound_10\n"
	     "        enter Route\n"
	     "    }\n"
	     "    play sound_316\n"
	     "}\n"
	     "counter = 1\n"
	     "enter Lobby\n"
	     "}\n"
	     "\n"
	     "section Server\n"
	     "{\n"
	     "play sound_11\n"
	     "if (((Var02 != 5) && (Var02 != 6)))\n"
	     "{\n"
	     "    enter Route\n"
	     "}\n"
	     "enter Server\n"
	     "}\n";
	return s;
}

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
	const bool shape_ok = back.header.chunk_count == 1 && back.intrinsic_count == 11 &&
	                      std::strncmp(s.name, p.name, MUS_NAME_SIZE) == 0 && s.section_count == p.section_count &&
	                      s.entry_section_index == 0 && std::strcmp(s.sections[0].name, p.entry_name) == 0 &&
	                      std::strcmp(s.sections[p.section_count - 1].name, p.last_name) == 0 &&
	                      std::strstr(s.source_path, "synth_") != nullptr &&
	                      std::strcmp(back.intrinsic_names[0], "GEcho") == 0 &&
	                      std::strcmp(back.intrinsic_names[10], "TStop") == 0;
	// MDEdit's layout: the leading nop the main entry and (no MessageHandler function) the +0x40 entry point at; the
	// gamescript's MessageHandler, its two parameters and its frame setup; a line per statement.
	const bool handler = std::strcmp(p.name, "gamescript") == 0;
	const bool layout_ok = s.code_size % 4 == 0 && s.code[0] == 0x00 && s.has_message_handler && s.line_count > 0 &&
	                       (handler ? s.function_count == 1 && s.local_count == 2 &&
	                                          std::strcmp(s.functions[0].name, "MessageHandler") == 0 &&
	                                          s.message_handler_offset == s.functions[0].start &&
	                                          s.code[s.functions[0].start] == MUS_OP_ENTER && s.locals_size == 0x28
	                                : s.function_count == 0 && s.message_handler_offset == 0 && s.locals_size == 0);
	mus_close(&back);
	if (!layout_ok) {
		err = std::string(p.file) + ": the program is not in MDEdit's layout";
		return false;
	}
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

using test_io::is_lfs_pointer;
using test_io::read_file;

bool write_file(const std::string &path, const void *data, size_t size) {
	std::ofstream o(path, std::ios::binary);
	if (!o) return false;
	o.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
	return static_cast<bool>(o);
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

// The forms program: built as the minted pair are, its shipped forms in its bytes (the globals 0x44,
// the opcodes pop_g, equal, l_and and the wide play among its code's), and it decompiles to its own
// text, no `#line` in it.
bool build_forms(const std::string &source, std::vector<uint8_t> &bytes, std::string &err) {
	const Program forms = {"menuscript", source.c_str(), "synth_forms.bin", 11, "Begin", "Server"};
	if (!build(forms, bytes, err)) return false;
	MusFile file;
	if (mus_open_memory(&file, bytes.data(), bytes.size()) != 0) {
		err = "synth_forms.bin does not open";
		return false;
	}
	const MusScript &s = file.scripts[0];
	bool ops[256] = {};
	// The opcodes as the decoder's widths walk them (the 1-, 2- and 4-byte operands, the table's own).
	for (uint32_t pc = 0; pc < s.code_size;) {
		const uint8_t op = s.code[pc++];
		ops[op] = true;
		if (op == MUS_OP_TABLEXEC && pc + 4 <= s.code_size) pc += 4 + uint32_t(s.code[pc]) * s.code[pc + 2];
		else if (op == 0x02 || (op >= 0x30 && op <= 0x34 && op != 0x33)) pc += 4;
		else if (op == 0x05 || op == 0x06 || op == 0x0A || op == 0x0B || op == MUS_OP_PLAYW) pc += 2;
		else if (op == 0x01 || op == 0x03 || op == 0x04 || op == 0x07 || op == 0x08 || op == 0x09 || op == 0x33 ||
		         (op >= 0x28 && op <= 0x2B) || op == MUS_OP_ENTER || op == MUS_OP_SETSTATE || op == MUS_OP_PLAY ||
		         op == 0x40)
			pc += 1;
	}
	std::string text;
	const bool decompiled = decompile(s, text, err);
	const bool forms_ok = s.globals_size == 0x44 && ops[0x08] && ops[0x15] && ops[0x20] && ops[MUS_OP_PLAYW] &&
	                      ops[MUS_OP_PLAY] && ops[MUS_OP_TABLEXEC] && s.variable_count >= 1;
	mus_close(&file);
	if (!decompiled) return false;
	if (!forms_ok) {
		err = "the program lacks a shipped menu program's form";
		return false;
	}
	if (text != source || text.find("#line") != std::string::npos) {
		size_t at = 0;
		while (at < text.size() && at < source.size() && text[at] == source[at]) ++at;
		err = "the program does not decompile to its own text (at " + std::to_string(at) + ": '" +
		      text.substr(at, 60) + "')";
		return false;
	}
	return true;
}

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string dir = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/mus/";

	std::vector<uint8_t> game, menu, forms;
	std::string err;
	if (!expect(build(kPrograms[0], game, err), ("synth_gamemus.bin: " + err).c_str())) return 1;
	if (!expect(build(kPrograms[1], menu, err), ("synth_menumus.bin: " + err).c_str())) return 1;
	const std::string forms_text = forms_source();
	if (!expect(build_forms(forms_text, forms, err), ("synth_forms.bin: " + err).c_str())) return 1;
	const std::vector<uint8_t> forms_text_bytes(forms_text.begin(), forms_text.end());
	const std::string forms_path = dir + "synth_forms.bin";
	const std::string forms_golden_path = dir + "golden_synth_forms.mus.txt";
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
		if (!expect(write_file(forms_path, forms.data(), forms.size()), ("cannot write " + forms_path).c_str())) return 1;
		if (!expect(write_file(forms_golden_path, forms_text.data(), forms_text.size()),
		            ("cannot write " + forms_golden_path).c_str()))
			return 1;
		std::printf("wrote %s (%zu bytes), %s (%zu bytes), %s (%zu bytes), %s (%zu bytes), %s (%zu bytes)\n",
		            game_path.c_str(), game.size(), menu_path.c_str(), menu.size(), golden_path.c_str(), golden.size(),
		            forms_path.c_str(), forms.size(), forms_golden_path.c_str(), forms_text.size());
		return 0;
	}
	bool ok = guard(game_path, game);
	ok = guard(menu_path, menu) && ok;
	ok = guard(golden_path, golden_bytes) && ok;
	ok = guard(forms_path, forms) && ok;
	ok = guard(forms_golden_path, forms_text_bytes) && ok;
	if (!ok) return 1;
	std::printf("OK: fixtures/mus synth programs + goldens byte-reproducible (%zu + %zu + %zu + %zu + %zu bytes)\n",
	            game.size(), menu.size(), golden.size(), forms.size(), forms_text.size());
	return 0;
}
