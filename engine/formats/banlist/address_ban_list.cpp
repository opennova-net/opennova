// banned.txt (address_ban_list.h).
#include <formats/banlist/address_ban_list.h>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/os_path.h>

#include <cstdio>
#include <cstring>

namespace opennova::banlist {

// [orig: BanList_ParseIPEntry @0x4FD520 — the three '.' scans @0x4FD538..0x4FD588 (each cut
//  in place, a segment with no dot left running to the token's end), the four `atol`s
//  @0x4FD596..0x4FD5AE summed under the shifts, the one-token line's `?` @0x4FD5D4, a name of
//  16 or more characters `?` too @0x4FD60E, the count stepped with no capacity check]
void parse_entry(AddressBanList &list, int count, const char *token0, const char *token1) {
	const std::string text = token0 != nullptr ? token0 : "";
	size_t starts[4] = {0, text.size(), text.size(), text.size()};
	size_t ends[4] = {text.size(), text.size(), text.size(), text.size()};
	for (int octet = 1; octet < 4; ++octet) {
		const size_t from = starts[octet - 1];
		const size_t dot = text.find('.', from);
		if (dot == std::string::npos) break;
		ends[octet - 1] = dot;
		starts[octet] = dot + 1;
		ends[octet] = text.size();
	}
	uint32_t octets[4] = {};
	for (int i = 0; i < 4; ++i) {
		const std::string segment = text.substr(starts[i], ends[i] - starts[i]);
		octets[i] = static_cast<uint32_t>(io::retail_atol(segment.c_str()));
	}
	AddressBan ban;
	ban.address = octets[0] + ((octets[1] + ((octets[2] + (octets[3] << 8)) << 8)) << 8);
	const char *name = token1 != nullptr ? token1 : "";
	ban.name = (count == 1 || std::strlen(name) >= 16) ? std::string("?") : std::string(name);
	list.entries.push_back(std::move(ban));
}

AddressBanList parse_address_list(const char *text, size_t size) {
	AddressBanList list;
	io::for_each_config_file_line(text, size, [&](io::ConfigTokens &tokens) {
		parse_entry(list, tokens.count, tokens.token(0), tokens.token(1));
	});
	return list;
}

bool load_address_list(const std::string &path, AddressBanList &out) {
	out = AddressBanList{};
	std::FILE *f = io::fopen_utf8(path.c_str(), "rb");
	if (f == nullptr) return false;
	std::string text;
	char chunk[4096];
	size_t got = 0;
	while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, got);
	std::fclose(f);
	out = parse_address_list(text.data(), text.size());
	return true;
}

// [orig: BanList_SaveToFile @0x4FDD70 — fopen("banned.txt", "w") @0x4FDDA3, then per entry
//  sprintf("%i.%i.%i.%i") of the address's four bytes @0x4FDDF5 and fprintf("%20s   \"%s\"\n")
//  @0x4FDE06; the text-mode stream writes each "\n" as CR LF]
std::string write_address_list(const AddressBanList &list) {
	std::string out;
	for (const AddressBan &ban : list.entries) {
		char address[64];
		std::snprintf(address, sizeof(address), "%i.%i.%i.%i", static_cast<int>(ban.address & 0xFFu),
				static_cast<int>((ban.address >> 8) & 0xFFu), static_cast<int>((ban.address >> 16) & 0xFFu),
				static_cast<int>(ban.address >> 24));
		char line[128];
		std::snprintf(line, sizeof(line), "%20s   \"", address);
		out += line;
		out += ban.name;
		out += "\"\r\n";
	}
	return out;
}

bool save_address_list(const std::string &path, const AddressBanList &list) {
	std::FILE *f = io::fopen_utf8(path.c_str(), "wb");
	if (f == nullptr) return false;
	const std::string text = write_address_list(list);
	const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
	return std::fclose(f) == 0 && ok;
}

} // namespace opennova::banlist
