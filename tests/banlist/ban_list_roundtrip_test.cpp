// The two ban files (formats/banlist): the minted fixtures read back to their models and
// re-written byte-exact, and each loader's witnessed rules over authored inline text.
// Fixtures: fixtures/banlist/synth_banlist.txt and synth_banned.txt, minted by
// tests/fixtures/minimal_banlist_gen.cpp from tests/banlist/banlist_synth.h.
// [orig: BanList_LoadFromFile @0x4DB290; BanList_SaveToFileWithHeader @0x4DB590;
//  BanList_ParseIPEntry @0x4FD520; BanList_SaveToFile @0x4FDD70]

#include "common/test_paths.h"
#include "common/file_io.h"

#include "banlist/banlist_synth.h"

#include <formats/banlist/address_ban_list.h>
#include <formats/banlist/pcid_ban_list.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace opennova::banlist;

namespace {

int g_failures = 0;

void expect(bool cond, const char *what) {
	if (!cond) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		++g_failures;
	}
}

std::string read_fixture(const char *name) {
	std::vector<uint8_t> bytes;
	const std::string path = std::string(test_paths_repo_root(__FILE__)) + "/fixtures/banlist/" + name;
	if (!test_io::read_file(path, bytes)) return {};
	return std::string(bytes.begin(), bytes.end());
}

bool same(const PcidBanList &a, const PcidBanList &b) {
	if (a.entries.size() != b.entries.size()) return false;
	for (size_t i = 0; i < a.entries.size(); ++i)
		if (a.entries[i].pcid != b.entries[i].pcid || a.entries[i].name != b.entries[i].name) return false;
	return true;
}

bool same(const AddressBanList &a, const AddressBanList &b) {
	if (a.entries.size() != b.entries.size()) return false;
	for (size_t i = 0; i < a.entries.size(); ++i)
		if (a.entries[i].address != b.entries[i].address || a.entries[i].name != b.entries[i].name) return false;
	return true;
}

uint32_t ip(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
	return a + (b << 8) + (c << 16) + (d << 24);
}

} // namespace

int main() {
	// banlist.txt: the fixture round-trip.
	{
		const std::string text = read_fixture("synth_banlist.txt");
		expect(!text.empty(), "synth_banlist.txt present");
		const auto list = parse_pcid_list(text.data(), text.size());
		expect(list.has_value() && same(*list, banlist_synth::pcids()), "the fixture reads back to its model");
		expect(list.has_value() && write_pcid_list(*list) == text, "and re-writes byte-exact");
		expect(text.find("VERSION 1\r\n") != std::string::npos &&
		               text.find("BAN \"A-SY-NTH002\"\r\n") != std::string::npos &&
		               text.find("BAN \"A-SY-NTH001\" \"Spammer\"\r\n") != std::string::npos,
		       "CR LF, VERSION 1, a nameless entry without its second quotes");
	}
	// banlist.txt: the loader's rules.
	{
		expect(!parse_pcid_list("BAN \"X\"\r\n", 9).has_value(), "no VERSION line: no list");
		const std::string v2 = "VERSION 2\nBAN X\n";
		expect(!parse_pcid_list(v2.data(), v2.size()).has_value(), "VERSION 2: no list");
		const std::string last = "version 2\r\nVERSION 1\rban a\nBAN \"b c\" \"d e\" extra\nBAN\nBAN \"\" x\n"
		                         "BAN f // g\nBAN \"h//i\"\n";
		const auto list = parse_pcid_list(last.data(), last.size());
		expect(list.has_value(), "the last VERSION wins; a lone CR ends a line");
		if (list.has_value()) {
			expect(list->entries.size() == 4, "BAN lines need a PCID token, and a nonempty one");
			if (list->entries.size() == 4) {
				expect(list->entries[0].pcid == "a" && list->entries[0].name.empty(), "keywords case-insensitive");
				expect(list->entries[1].pcid == "b c" && list->entries[1].name == "d e", "quoted runs, a third token ignored");
				expect(list->entries[2].pcid == "f" && list->entries[2].name.empty(), "// cuts the line");
				expect(list->entries[3].pcid == "h", "// cuts even inside quotes");
			}
			PcidBanList edit = *list;
			expect(find(edit, "A") != nullptr && find(edit, "zz") == nullptr, "the find is case-insensitive");
			expect(!add(edit, "", "n"), "an empty PCID is never added");
			expect(remove(edit, "B C") && edit.entries.size() == 3, "the UNBAN's unlink");
		}
	}
	// banlist.txt: String_TokenizeQuoted's white space is the CRT isspace under the game's ".ACP"
	// LC_CTYPE, pinned to cp1252: the six C-locale spaces split, and so does 0xA0 (the no-break
	// space), which a quoted run keeps (docs/net/novaworld-net-re.md D-NET-381).
	// [orig: String_TokenizeQuoted @0x4DB000 — isspace @0x4DB046; System_InitTimerAndLocale
	//  @0x762A6E]
	{
		for (const char space : std::string(" \t\n\v\f\r")) {
			const std::vector<std::string> tokens = tokenize_quoted(std::string("a") + space + "b");
			expect(tokens.size() == 2 && tokens[0] == "a" && tokens[1] == "b", "each C-locale space splits");
		}
		const std::vector<std::string> nbsp = tokenize_quoted("a\xA0" "b");
		expect(nbsp.size() == 2 && nbsp[0] == "a" && nbsp[1] == "b", "0xA0 splits a token");
		const std::vector<std::string> quoted = tokenize_quoted("\"a\xA0" "b\"");
		expect(quoted.size() == 1 && quoted[0] == "a\xA0" "b", "a quoted 0xA0 stays in its token");
		const std::vector<std::string> high = tokenize_quoted("a\x85\xFF" "b");
		expect(high.size() == 1 && high[0] == "a\x85\xFF" "b", "no other high byte splits");
		const std::string text = "VERSION\xA0" "1\nBAN\xA0" "A-1\xA0" "Bob\nBAN \"A-2\xA0" "x\" \"N\xA0" "M\"\n";
		const auto list = parse_pcid_list(text.data(), text.size());
		expect(list.has_value() && list->entries.size() == 2, "0xA0 separates the keywords and their tokens");
		if (list.has_value() && list->entries.size() == 2) {
			expect(list->entries[0].pcid == "A-1" && list->entries[0].name == "Bob", "an unquoted 0xA0 splits");
			expect(list->entries[1].pcid == "A-2\xA0" "x" && list->entries[1].name == "N\xA0" "M",
			       "a quoted 0xA0 is kept");
		}
	}
	// banned.txt: the fixture round-trip.
	{
		const std::string text = read_fixture("synth_banned.txt");
		expect(!text.empty(), "synth_banned.txt present");
		const AddressBanList list = parse_address_list(text.data(), text.size());
		expect(same(list, banlist_synth::addresses()), "the fixture reads back to its model");
		expect(write_address_list(list) == text, "and re-writes byte-exact");
		expect(text.rfind("            10.0.0.7   \"?\"\r\n", 0) == 0, "%20s, three spaces, the quoted name, CR LF");
	}
	// banned.txt: BanList_ParseIPEntry's rules.
	{
		const std::string text = "1.2.3\r\n1.2.3.4.5 x\r\n300.1.1.1 \"a b\"\r\n9.9.9.9 SixteenCharsName\r\n8.8.8.8\r\n";
		const AddressBanList list = parse_address_list(text.data(), text.size());
		expect(list.entries.size() == 5, "one entry per line");
		if (list.entries.size() == 5) {
			expect(list.entries[0].address == ip(1, 2, 3, 0) && list.entries[0].name == "?",
			       "a missing octet reads 0, a one-token line names ?");
			expect(list.entries[1].address == ip(1, 2, 3, 4) && list.entries[1].name == "x",
			       "the fourth octet runs to the token's end (atol stops at the dot)");
			expect(list.entries[2].address == ip(300, 1, 1, 1) && list.entries[2].name == "a b",
			       "octets pack by addition, so 300 carries into the next byte");
			expect(list.entries[3].name == "?", "a name of 16 or more characters is ?");
			expect(list.entries[4].name == "?", "a bare address");
		}
	}
	if (g_failures == 0) {
		std::printf("ban_list_roundtrip: OK\n");
		return 0;
	}
	std::fprintf(stderr, "%d assertion(s) failed\n", g_failures);
	return 1;
}
