// banlist.txt (pcid_ban_list.h).
#include <formats/banlist/pcid_ban_list.h>

#include <base/io/cp1252.h> // cp1252_isspace
#include <base/io/crt_ftol.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include <cstdio>

namespace opennova::banlist {

namespace {

// The CRT isspace under the game's ".ACP" LC_CTYPE, pinned to cp1252: the six C-locale spaces
// plus 0xA0 (docs/net/novaworld-net-re.md D-NET-381, D-NET-382). This tokenizer passes the
// byte SIGN-extended, and the CRT's ctype table mirrors its 128..254 rows below index 0 for
// that, so 0xA0 (-96) is a space here too; 0xFF (-1) reads the EOF row, which no set marks.
// [orig: String_TokenizeQuoted @0x4DB000 — `movsx` @0x4DB042, isspace @0x4DB046 ->
//  @0x76B964; CRT_init_ctype's mirror copy @0x784C59]
bool is_space(char c) {
	return cp1252_isspace(static_cast<uint8_t>(c));
}

// String_CompareCaseInsensitive_0: equal under toupper, both ends together.
// [orig: @0x4DB0C0]
bool keyword_equals(std::string_view a, std::string_view b) {
	return strutil::iequals(a, b);
}

// Text_ReadLineFromBuffer over `text` from `at`: the line (2047 characters at most)
// and the next start; false at the end (a NUL or the buffer's end). LF, CR LF and a
// lone CR end a line; a line that fills the buffer ends without its terminator, so
// the rest follows as the next line.
// [orig: Text_ReadLineFromBuffer @0x4DAF60 — `*srcBuffer == 0` returns -2, the
//  `remaining > 1` copy over a 2048-byte buffer, LF / CR LF / CR consumed]
bool read_line(std::string_view text, size_t &at, std::string &line) {
	line.clear();
	if (at >= text.size() || text[at] == '\0') return false;
	size_t remaining = 2048;
	while (remaining > 1 && at < text.size()) {
		const char c = text[at];
		if (c == '\0') break;
		if (c == '\n') {
			++at;
			break;
		}
		if (c == '\r') {
			++at;
			if (at < text.size() && text[at] == '\n') ++at;
			break;
		}
		line.push_back(c);
		++at;
		--remaining;
	}
	return true;
}

// The line with its first `//` cut, quotes or not [orig: strstr(line, "//") @0x4DB3ED /
// @0x4DB4CD, the NUL stored there].
std::string_view uncommented(const std::string &line) {
	const size_t cut = line.find("//");
	return std::string_view(line).substr(0, cut == std::string::npos ? line.size() : cut);
}

} // namespace

std::vector<std::string> tokenize_quoted(std::string_view line) {
	std::vector<std::string> out;
	bool in_token = false;
	bool quoted = false;
	for (const char c : line) {
		if (c == '\0') break;
		if (!is_space(c) || quoted) {
			if (!in_token) {
				in_token = true;
				out.emplace_back();
			}
			if (c == '"')
				quoted = !quoted;
			else
				out.back().push_back(c);
		} else {
			in_token = false;
		}
	}
	return out;
}

bool add(PcidBanList &list, std::string_view pcid, std::string_view name) {
	if (pcid.empty()) return false;
	list.entries.push_back(PcidBan{std::string(pcid), std::string(name)});
	return true;
}

const PcidBan *find(const PcidBanList &list, std::string_view pcid) {
	for (const PcidBan &ban : list.entries)
		if (keyword_equals(ban.pcid, pcid)) return &ban;
	return nullptr;
}

bool remove(PcidBanList &list, std::string_view pcid) {
	for (auto it = list.entries.begin(); it != list.entries.end(); ++it) {
		if (!keyword_equals(it->pcid, pcid)) continue;
		list.entries.erase(it);
		return true;
	}
	return false;
}

// [orig: BanList_LoadFromFile @0x4DB290 — the whole file read @0x4DB355 and checked
//  against its size @0x4DB39C; the VERSION pass @0x4DB3C4..0x4DB45E (two or more tokens,
//  `atol` of token 1 @0x4DB43D); `version == 1` @0x4DB46E; the BAN pass @0x4DB4A5..0x4DB553 (two or more tokens, the
//  name token 2 when there are three or more @0x4DB51A, a nonempty PCID @0x4DB52C, then
//  LinkedList_AddEntry @0x4DB534)]
std::optional<PcidBanList> parse_pcid_list(const char *text, size_t size) {
	const std::string_view file(text != nullptr ? text : "", text != nullptr ? size : 0);
	long version = 0;
	size_t at = 0;
	std::string line;
	while (read_line(file, at, line)) {
		const std::vector<std::string> tokens = tokenize_quoted(uncommented(line));
		if (tokens.size() >= 2 && keyword_equals(tokens[0], "VERSION"))
			version = io::retail_atol(tokens[1].c_str());
	}
	if (version != kPcidBanListVersion) return std::nullopt;
	PcidBanList list;
	at = 0;
	while (read_line(file, at, line)) {
		const std::vector<std::string> tokens = tokenize_quoted(uncommented(line));
		if (tokens.size() < 2 || !keyword_equals(tokens[0], "BAN")) continue;
		if (!tokens[1].empty()) add(list, tokens[1], tokens.size() > 2 ? tokens[2] : std::string());
	}
	return list;
}

std::optional<PcidBanList> load_pcid_list(const std::string &path) {
	std::FILE *f = io::fopen_utf8(path.c_str(), "rb");
	if (f == nullptr) return std::nullopt;
	std::string text;
	char chunk[4096];
	size_t got = 0;
	while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, got);
	const bool read_whole = std::ferror(f) == 0;
	std::fclose(f);
	if (!read_whole) return std::nullopt;
	return parse_pcid_list(text.data(), text.size());
}

// [orig: BanList_SaveToFileWithHeader @0x4DB590 — the truncate (open 0x201) @0x4DB5DE and
//  the _lopen / _lcreat @0x4DB5FB..0x4DB60D; the dash line off_82CE38 (@0x7CD268) and the
//  header writes @0x4DB65C..0x4DB933, "VERSION %ld" with 1 @0x4DB936..0x4DB941, the usage
//  block @0x4DB962..0x4DBAF1, then the node walk: `BAN "%s" "%s"` (@0x7CCFE8) for a nonempty
//  name @0x4DBB11, else `BAN "%s"` (@0x7CCFDC) @0x4DBB29, each written @0x4DBB51]
std::string write_pcid_list(const PcidBanList &list) {
	static constexpr const char *kDashes = "//---------------------------------------------------\r\n";
	std::string out;
	out += kDashes;
	out += "// NovaLogic Ban List File\r\n";
	out += kDashes;
	out += "\r\n";
	out += "// This file contains a list of PCID's (Public Customer ID's)\r\n";
	out += "// that you, as a game server host, chose to exclude from\r\n";
	out += "// joining your game server.\r\n";
	out += "//\r\n";
	out += "// This file loads into memory when the game is run, and\r\n";
	out += "// is modified by the game server when people are BANNED or UNBANNED using\r\n";
	out += "// the BAN and UNBAN commands on the ~ command line in the game.\r\n";
	out += "//\r\n";
	out += "// Do not modify this file while the game is running!\r\n";
	out += "//\r\n";
	out += "\r\n";
	out += "\r\n";
	out += "VERSION " + std::to_string(kPcidBanListVersion) + "\r\n";
	out += "\r\n";
	out += "\r\n";
	out += "// Usage:  BAN \"<pcid goes here>\" \"<optional player name>\"\r\n";
	out += "//\r\n";
	out += "// Example:\r\n";
	out += "// BAN \"A-MM-WXXIB2\" \"Knightmare\"\r\n";
	out += "// BAN \"A-WC-WXXIAD\"\r\n";
	out += "\r\n";
	for (const PcidBan &ban : list.entries) {
		if (!ban.name.empty())
			out += "BAN \"" + ban.pcid + "\" \"" + ban.name + "\"\r\n";
		else
			out += "BAN \"" + ban.pcid + "\"\r\n";
	}
	return out;
}

bool save_pcid_list(const std::string &path, const PcidBanList &list) {
	std::FILE *f = io::fopen_utf8(path.c_str(), "wb");
	if (f == nullptr) return false;
	const std::string text = write_pcid_list(list);
	const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	return std::fclose(f) == 0 && ok;
}

} // namespace opennova::banlist
