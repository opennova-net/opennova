// Generator + guard for fixtures/update/synth_update.sfs: a packed update (the 1SFS single-file
// update, docs/update/updater-re.md §9) written by sfs_write from synthetic files, the one form
// the update service takes (ADR 0061 d4). The base product's section carries its command file
// (a loose version.txt, a TARGET of resource.pff, a COPY into it and a REMOVE), each payload a
// 1ZLN file; a PACKAGEBREAK section carries the jox01 expansion product's. No retail byte is in
// it: every name and text is invented, in retail's shapes.
//
// Default: rebuild in memory and byte-compare the committed file. `--write` (re)writes it.
#include "common/test_paths.h"

#include <formats/update/command_file.h>
#include <formats/update/nlz.h>
#include <formats/update/sfs.h>
#include <formats/update/update_crc.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/file_io.h"

using namespace opennova::update;

namespace {

bool expect(bool cond, const char *msg) {
	if (!cond)
		std::fprintf(stderr, "FAIL: %s\n", msg);
	return cond;
}

std::vector<uint8_t> bytes(std::string_view text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

// One product's section: its command file, then each row's file as a 1ZLN blob.
SfsSection product_section(const std::string &expansion, const std::string &cmd_name,
		const std::string &product, const std::string &version_text, const std::string &copy_name,
		const std::string &copy_text) {
	const std::vector<uint8_t> version = bytes(version_text);
	const std::vector<uint8_t> copy = bytes(copy_text);
	CommandFile cmd;
	cmd.product = product;
	cmd.version = "1.0.0.1";
	cmd.rows.push_back({ "UPD", CommandType::Loose, "version.txt", 1700000000,
			int32_t(version.size()), crc_as_signed(file_crc(version.data(), version.size())) });
	cmd.rows.push_back({ "TARGET", CommandType::Target, "resource.pff", 0, 0, std::nullopt });
	cmd.rows.push_back({ "COPY", CommandType::Copy, copy_name, 1700000100, int32_t(copy.size()),
			crc_as_signed(file_crc(copy.data(), copy.size())) });
	cmd.rows.push_back({ "REMOVE", CommandType::Remove, "retired.def", 0, 0, std::nullopt });
	SfsSection section;
	section.expansion = expansion;
	section.entries.push_back({ cmd_name, bytes(write_command_file(cmd)) });
	section.entries.push_back({ "version.txt", nlz_encode(version.data(), version.size()) });
	section.entries.push_back({ copy_name, nlz_encode(copy.data(), copy.size()) });
	return section;
}

SfsPackage make_package() {
	SfsPackage package;
	package.product = "Synthetic Product";
	package.sections.push_back(product_section("", "SYNPTR.CMD", "Synthetic Product",
			"Synthetic Product, V1.0.0.1; English", "synth.def",
			"[weapon]\r\nname = synthetic rifle\r\nrange = 600\r\n"));
	package.sections.push_back(product_section("jox01", "SYNEPTR.CMD", "Synthetic Expansion",
			"Synthetic Expansion, V1.0.0.1; English", "synthx.def", "[item]\r\nname = gadget\r\n"));
	return package;
}

using test_io::read_file;

} // namespace

int main(int argc, char **argv) {
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0)
			write_mode = true;
	const std::string path =
			std::string(test_paths_repo_root(__FILE__)) + "/fixtures/update/synth_update.sfs";

	const std::vector<uint8_t> written = sfs_write(make_package());
	if (!expect(!written.empty(), "sfs_write refused the package"))
		return 1;
	// The package reads back to the same bytes and keeps its pinned shape.
	SfsPackage back;
	std::string error;
	if (!expect(sfs_read(written.data(), written.size(), back, error), error.c_str()))
		return 1;
	if (!expect(sfs_write(back) == written, "write(read(write(package))) is not byte-stable"))
		return 1;
	if (!expect(back.sections.size() == 2 && back.sections[1].expansion == "jox01" &&
						back.sections[0].entries.size() == 3,
				"the re-read package lost a section or an entry"))
		return 1;
	const std::string cmd_text(
			back.sections[0].entries[0].data.begin(), back.sections[0].entries[0].data.end());
	const std::optional<CommandFile> cmd = read_command_file(cmd_text);
	if (!expect(cmd && cmd->rows.size() == 4 && cmd->rows[2].name == "synth.def",
				"the base command file does not read back"))
		return 1;
	std::vector<uint8_t> inflated;
	if (!expect(nlz_decode(back.sections[0].entries[2].data.data(),
						back.sections[0].entries[2].data.size(), inflated) == NlzDecode::Inflated &&
						int32_t(inflated.size()) == cmd->rows[2].size,
				"the COPY row's blob does not inflate to its size"))
		return 1;

	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(reinterpret_cast<const char *>(written.data()),
				static_cast<std::streamsize>(written.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), written.size());
		return 0;
	}
	std::vector<uint8_t> committed;
	if (!expect(read_file(path, committed),
				("committed file missing; run with --write: " + path).c_str()))
		return 1;
	if (test_io::is_lfs_pointer(committed)) {
		std::printf("[skip] %s is an unpulled LFS pointer\n", path.c_str());
		return 0;
	}
	if (!expect(committed == written,
				("differs from the generator output; regenerate with --write: " + path).c_str()))
		return 1;
	std::printf("OK: fixtures/update/synth_update.sfs byte-reproducible\n");
	return 0;
}
