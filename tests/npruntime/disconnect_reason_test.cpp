// Retail's player-facing reason for a failed join or a lost session: the
// connection's error record picks one gameerr.bin entry from four code tables
// (or an "MP Errors" fallback), its "[[$]]" replaced by the record's text.
// [orig: CNapiNetwork_GetDisconnectReasonString @0x4c7000; the tables
//  g_MPNetConnectCodes @0x82ba58 / g_MPGameConnectCodes @0x82bad8 /
//  g_MPNetDisconnectCodes @0x82bb08 / g_MPGameDisconnectCodes @0x82bb60]

#include <runtime/inmatch/disconnect_reason.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace opennova;
using inmatch::ConnectionErrorRecord;

namespace {

bool expect(bool cond, const char *what) {
	if (!cond) std::fprintf(stderr, "FAIL: %s\n", what);
	return cond;
}

// A gameerr-shaped table: each (section, key, text) row in its own section.
rtxt::File make_table(const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> &rows) {
	rtxt::File file;
	for (const auto &row : rows) {
		uint32_t section = 0;
		bool found = false;
		for (uint32_t i = 0; i < file.sections.size(); ++i) {
			if (file.sections[i].name == row.first) {
				section = i;
				found = true;
			}
		}
		if (!found) {
			section = static_cast<uint32_t>(file.sections.size());
			file.sections.push_back(rtxt::Section{row.first, 0});
		}
		++file.sections[section].string_count;
		rtxt::Entry entry;
		entry.key = row.second.first;
		entry.text = row.second.second;
		entry.section_index = section;
		file.entries.push_back(entry);
	}
	return file;
}

const rtxt::File &gameerr() {
	static const rtxt::File file = make_table({
			{"MP Errors", {"ERR1", "An unknown error has occurred.  (ERR1)"}},
			{"MP Errors", {"ERR2", "An unknown error has occurred.  (ERR2)"}},
			{"MP Errors", {"ERR3", "An unknown error has occurred.  (ERR3)"}},
			{"MP Errors", {"ERR4", "An unknown error has occurred.  (ERR4)"}},
			{"MP Errors", {"ERR5", "An unknown error has occurred.  (ERR5)"}},
			{"MP Errors", {"UNSPECIFIED", "An unspecified error has occurred: "}},
			{"MPNetConnectCodes", {"NCC002", "Your connection has timed out. (Code NCC002)"}},
			{"MPNetConnectCodes", {"NCC007", "Your game is incompatible with this server. (NCC007)"}},
			{"MPGameConnectCodes", {"GCC004", "The server is currently full. (GCC004)"}},
			{"MPNetDisconnectCodes", {"NDC009", "The server has been shut down.  (NDC009)"}},
			{"MPGameDisconnectCodes", {"GDC032", "[[$]] (GDC032)"}},
			{"MPGameDisconnectCodes", {"GDC047", "An expansion pack/mod type mismatch has occurred. (GDC047)"}},
	});
	return file;
}

std::string reason(const ConnectionErrorRecord &record, const std::string &nw = {}) {
	return inmatch::disconnect_reason_string(&record, nullptr, &gameerr(), nw);
}

bool run_join_failures_read_the_connect_tables() {
	ConnectionErrorRecord pv2;
	pv2.connect_error = 7;
	if (!expect(reason(pv2) == "Your game is incompatible with this server. (NCC007)",
			"JFC 7 reads MPNetConnectCodes NCC007"))
		return false;
	ConnectionErrorRecord full;
	full.connect_error = 14;
	full.connect_param = 4;
	if (!expect(reason(full) == "The server is currently full. (GCC004)",
			"JFC 14 reads MPGameConnectCodes by JFP"))
		return false;
	ConnectionErrorRecord odd_validate;
	odd_validate.connect_error = 14;
	odd_validate.connect_param = 9;
	if (!expect(reason(odd_validate) == "An unknown error has occurred.  (ERR3)",
			"a JFP the game-connect table lacks falls back to ERR3"))
		return false;
	ConnectionErrorRecord unknown;
	unknown.connect_error = 17;
	if (!expect(reason(unknown) == "An unknown error has occurred.  (ERR2)",
			"a JFC the net-connect table lacks falls back to ERR2"))
		return false;
	ConnectionErrorRecord timeout;
	timeout.connect_error = 2;
	timeout.disconnect_code = 9; // a join failure outranks a latched disconnect
	return expect(reason(timeout) == "Your connection has timed out. (Code NCC002)",
			"the connect error is read ahead of the disconnect record");
}

bool run_disconnects_read_the_disconnect_tables() {
	ConnectionErrorRecord expansion;
	expansion.disconnect_code = 2;
	expansion.disconnect_param = 47;
	if (!expect(reason(expansion) ==
					"An expansion pack/mod type mismatch has occurred. (GDC047)",
			"DC 2 reads MPGameDisconnectCodes by DPC"))
		return false;
	ConnectionErrorRecord text;
	text.disconnect_code = 2;
	text.disconnect_param = 32;
	text.disconnect_text = "Kicked by admin";
	if (!expect(reason(text) == "Kicked by admin (GDC032)",
			"the record's DSTR replaces [[$]]"))
		return false;
	ConnectionErrorRecord unlisted;
	unlisted.disconnect_code = 2;
	unlisted.disconnect_param = 49;
	if (!expect(reason(unlisted) == "??MPGameDisconnectCodes:GDC049??",
			"GDC049 is in the code table but not gameerr.bin: the lookup-miss marker"))
		return false;
	ConnectionErrorRecord past_table;
	past_table.disconnect_code = 2;
	past_table.disconnect_param = 50;
	if (!expect(reason(past_table) == "An unknown error has occurred.  (ERR5)",
			"a DPC past the game-disconnect table falls back to ERR5"))
		return false;
	ConnectionErrorRecord shutdown;
	shutdown.disconnect_code = 9;
	if (!expect(reason(shutdown) == "The server has been shut down.  (NDC009)",
			"any other DC reads MPNetDisconnectCodes by DC"))
		return false;
	ConnectionErrorRecord gap;
	gap.disconnect_code = 12;
	return expect(reason(gap) == "An unknown error has occurred.  (ERR4)",
			"a DC the net-disconnect table lacks falls back to ERR4");
}

bool run_no_record_and_the_novaworld_message() {
	if (!expect(inmatch::disconnect_reason_string(nullptr, nullptr, &gameerr()) ==
					"An unknown error has occurred.  (ERR1)",
			"no connection object reads ERR1"))
		return false;
	const ConnectionErrorRecord clean;
	if (!expect(reason(clean).empty(), "a record with neither code builds no reason"))
		return false;
	if (!expect(reason(clean, "NWUSERVERMSGCODE_7") ==
					"An unspecified error has occurred: NWUSERVERMSGCODE_7",
			"a NovaWorld server message on an empty reason follows UNSPECIFIED"))
		return false;
	ConnectionErrorRecord shutdown;
	shutdown.disconnect_code = 9;
	if (!expect(reason(shutdown, "X") == "The server has been shut down.  (NDC009)X",
			"a NovaWorld server message is appended to a reason"))
		return false;
	const rtxt::File override_table = make_table(
			{{"MPNetDisconnectCodes", {"NDC009", "Expansion text (NDC009)"}}});
	return expect(inmatch::disconnect_reason_string(&shutdown, &override_table, &gameerr()) ==
					"Expansion text (NDC009)",
			"the expansion's override table wins");
}

} // namespace

int main() {
	bool ok = true;
	ok = run_join_failures_read_the_connect_tables() && ok;
	ok = run_disconnects_read_the_disconnect_tables() && ok;
	ok = run_no_record_and_the_novaworld_message() && ok;
	if (ok) std::printf("disconnect_reason_test: OK\n");
	return ok ? 0 : 1;
}
