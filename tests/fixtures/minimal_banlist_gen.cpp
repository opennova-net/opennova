// Generator + guard for fixtures/banlist/synth_banlist.txt and fixtures/banlist/synth_banned.txt:
// the two ban files written by banlist::write_pcid_list and banlist::write_address_list from the
// models in tests/banlist/banlist_synth.h (invented PCIDs, names and addresses). No retail file
// stands behind them: a server writes both on its operator's machine.
// [orig: BanList_SaveToFileWithHeader @0x4DB590; BanList_SaveToFile @0x4FDD70]
//
// Default: rebuild in memory and byte-compare the committed files. `--write` (re)writes them.
#include "common/test_paths.h"

#include "banlist/banlist_synth.h"

#include <formats/banlist/address_ban_list.h>
#include <formats/banlist/pcid_ban_list.h>

#include <cstdint>
#include <cstdio>
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

bool check_or_write(const std::string &path, const std::string &text, bool write_mode) {
	if (write_mode) {
		std::ofstream o(path, std::ios::binary);
		o.write(text.data(), static_cast<std::streamsize>(text.size()));
		std::printf("wrote %s (%zu bytes)\n", path.c_str(), text.size());
		return true;
	}
	std::vector<uint8_t> committed;
	if (!expect(test_io::read_file(path, committed), ("committed file missing; run with --write: " + path).c_str()))
		return false;
	return expect(std::string(committed.begin(), committed.end()) == text,
	              ("differs from the generator output; regenerate with --write: " + path).c_str());
}

} // namespace

int main(int argc, char **argv) {
	using namespace opennova::banlist;
	bool write_mode = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--write") == 0) write_mode = true;
	const std::string root = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/banlist/";

	const std::string pcid_text = write_pcid_list(banlist_synth::pcids());
	const auto pcid_back = parse_pcid_list(pcid_text.data(), pcid_text.size());
	if (!expect(pcid_back.has_value() && write_pcid_list(*pcid_back) == pcid_text,
	            "write(parse(write(banlist.txt))) is not byte-stable"))
		return 1;
	const std::string address_text = write_address_list(banlist_synth::addresses());
	if (!expect(write_address_list(parse_address_list(address_text.data(), address_text.size())) == address_text,
	            "write(parse(write(banned.txt))) is not byte-stable"))
		return 1;

	if (!check_or_write(root + "synth_banlist.txt", pcid_text, write_mode)) return 1;
	if (!check_or_write(root + "synth_banned.txt", address_text, write_mode)) return 1;
	if (!write_mode) std::printf("OK: fixtures/banlist/synth_banlist.txt and synth_banned.txt byte-reproducible\n");
	return 0;
}
