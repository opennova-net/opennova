// banned.txt — the addresses (and names) every authority refuses.
//
// A plain-text file in the game's working directory. Every round init zeroes the list
// and its dirty word and re-reads the file through the game.cfg walk
// [orig: Server_InitNewRoundState @0x51CB25..0x51CB3B -> File_ParseASCIIFileWithCallback
// @0x53D980 with BanList_ParseIPEntry @0x4FD520]. The host's in-game ban appends the slot's
// connection address and name and sets the dirty word [orig: Server_NotifyPlayerKicked
// @0x5082D0, a misnomer]; the shutdown rewrites the file only when the list and the dirty
// word are both nonzero [orig: Game_ShutdownSubsystems @0x4A539D -> j_BanList_SaveToFile
// @0x508E20 -> BanList_SaveToFile @0x4FDD70]. The consumers are the protocol's address check
// at the join (family 14 reason 3) and, in a session, the exact case-sensitive name check of
// the game-layer join (punt code 31). The record is docs/net/novaworld-net-re.md §6.9 ("The
// ban lists").
//
// A LINE. Token 0 is `a.b.c.d`, each octet the `atol` of the text up to the next dot (so
// `1.2.3` reads d = 0, and a fourth octet runs to the end of the token), packed by addition
// as `a + (b << 8) + (c << 16) + (d << 24)`; token 1, when the line has one and it is shorter
// than 16 characters, is the name, else `?`. [orig: BanList_ParseIPEntry @0x4FD520]
//
// The writer prints one `%20s   "%s"\n` line per entry, the address as `%i.%i.%i.%i` of its
// four bytes, right-justified in 20, through a text-mode "w" stream (CR LF on the game's
// host), built from the model. [orig: BanList_SaveToFile @0x4FDD70]
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::banlist {

// The file's name in the working directory [orig: "banned.txt" @0x7CF1D8].
inline constexpr const char *kAddressFileName = "banned.txt";

// One entry: g_BannedIdList's packed address (the raw in_addr dword, the PeerAddr::ip
// packing) and g_BannedNameList's 32-byte name.
struct AddressBan {
	uint32_t address = 0;
	std::string name;
};

// g_BannedNameCount @0xC8FF20 entries of g_BannedIdList @0xC8FF28 and g_BannedNameList
// @0xC90728, in order. Retail's arrays hold 512 and the parse has no capacity check; this
// list grows (D-NET-368).
struct AddressBanList {
	std::vector<AddressBan> entries;
};

// One line's tokens onto the list [orig: BanList_ParseIPEntry @0x4FD520]. `count` is the
// line's token count (a game.cfg-walk line has at least one).
void parse_entry(AddressBanList &list, int count, const char *token0, const char *token1);

// The file's lines through the game.cfg walk.
AddressBanList parse_address_list(const char *text, size_t size);
// parse_address_list() over the file at `path`; false (an empty list) when it does not open.
bool load_address_list(const std::string &path, AddressBanList &out);

// BanList_SaveToFile's text, CR LF line ends.
std::string write_address_list(const AddressBanList &list);
// write_address_list() to `path`, replacing it. False when it cannot open.
bool save_address_list(const std::string &path, const AddressBanList &list);

} // namespace opennova::banlist
