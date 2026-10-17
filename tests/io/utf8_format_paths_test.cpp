// The formats' fstream opens under a UTF-8 path whose name no ANSI code page spells: an RTXT
// table, a CPT, a mission .bms written and read again, a menu and a particle file read. A
// std::ifstream / std::ofstream given the UTF-8 std::string took it in the system's ANSI code page
// inside a process with no UTF-8 manifest (the Godot editor, the OpenNova Editor's build), so such
// a path failed to open, as the narrow fopen did before io::fopen_utf8; they open through
// io::os_path now (engine/CLAUDE.md's io rule).
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <formats/cpt/cpt_io.h>
#include <formats/mission/bms.h>
#include <formats/mnu/mnu.h>
#include <formats/particle/parser.h>
#include <formats/rtxt/rtxt.h>

#include "common/temp_dir.h"
#include "common/test_paths.h"

using namespace opennova;

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
	if (condition) return;
	std::fprintf(stderr, "FAIL: %s\n", message);
	++failures;
}

bool exists(const std::string &path) {
	std::error_code ec;
	return std::filesystem::exists(io::os_path(path), ec);
}

} // namespace

int main() {
	test_temp::TempDir temp("utf8_format_paths");
	// "jörg Ωμέγα": Latin-1, Greek; no single-byte ANSI page spells both.
	const std::string dir = temp.file("j\xC3\xB6rg \xCE\xA9\xCE\xBC\xCE\xAD\xCE\xB3\xCE\xB1");
	std::string error;
	expect(io::ensure_directory(dir, error), "the UTF-8 directory is made");

	// RTXT: rtxt::write_file, then rtxt::parse_file.
	{
		rtxt::File table;
		table.sections.push_back({"menu", 1});
		table.entries.push_back({"OK", "Okay", {0, 0}, 0});
		const std::string path = io::utf8_join(dir, "t.bin");
		expect(rtxt::write_file(table, path, error), "rtxt::write_file opens a UTF-8 path");
		expect(exists(path), "the table lands under its UTF-8 name");
		rtxt::File back;
		expect(rtxt::parse_file(path, back, error), "rtxt::parse_file opens a UTF-8 path");
		expect(back.entries.size() == 1 && back.entries[0].key == "OK", "the table read back is the table written");
	}

	// CPT: CptFile::write, then CptFile::read.
	{
		CptFile cpt;
		cpt.depth_format = DepthFormat::DPTH;
		cpt.depth_buffer.assign(1024u * 1024u, 7);
		const std::string path = io::utf8_join(dir, "t.cpt");
		try {
			cpt.write(path);
			expect(exists(path), "the CPT lands under its UTF-8 name");
			const CptFile back = CptFile::read(path);
			expect(back.depth_buffer.size() == cpt.depth_buffer.size() && back.depth_buffer[5] == 7,
			       "the CPT read back is the CPT written");
		} catch (const std::exception &e) {
			std::fprintf(stderr, "FAIL: CptFile through a UTF-8 path threw: %s\n", e.what());
			++failures;
		}
	}

	// A mission .bms (the minted synth_logic.bms read in): bms::write_file, then bms::parse_file.
	{
		bms::File mission;
		expect(bms::parse_file(test_paths_fixture("bms/synth_logic.bms"), mission, error), "the fixture reads");
		const std::string path = io::utf8_join(dir, "t.bms");
		expect(bms::write_file(mission, path, error), "bms::write_file opens a UTF-8 path");
		expect(exists(path), "the mission lands under its UTF-8 name");
		bms::File back;
		expect(bms::parse_file(path, back, error), "bms::parse_file reads it back");
	}

	// A menu: mnu::parse_file.
	{
		const std::string text = "<SCREEN><NAME>S</NAME></SCREEN>";
		const std::string path = io::utf8_join(dir, "t.mnu");
		expect(io::write_file_atomic(path, text, error), "the menu is written");
		mnu::Document menu;
		expect(mnu::parse_file(path, menu, error), "mnu::parse_file opens a UTF-8 path");
		expect(menu.screens.size() == 1, "the menu read is the menu written");
	}

	// A particle file: particle::load_particles_from_file.
	{
		const std::string text = "[effectdef]\n{\n\tid = fx;\n\tpdefs = p1;\n}\n\n";
		const std::string path = io::utf8_join(dir, "t.ptl");
		expect(io::write_file_atomic(path, text, error), "the particle file is written");
		particle::ParticleFile effects;
		particle::ParseError parse_error;
		expect(particle::load_particles_from_file(path, effects, parse_error),
		       "particle::load_particles_from_file opens a UTF-8 path");
		expect(effects.effects.size() == 1, "the effects read are the effects written");
	}

	if (failures != 0) return 1;
	std::puts("utf8_format_paths_test: OK");
	return 0;
}
