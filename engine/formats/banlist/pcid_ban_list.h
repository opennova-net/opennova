// banlist.txt — the PCIDs a NovaWorld game server refuses.
//
// A plain-text file in the game's working directory that only an authority in a
// session on the NovaWorld network type reads: every round init frees the list
// and reloads the file; a missing file, or one whose VERSION is not 1, yields no
// list, so a fresh empty one is created and saved over the file
// [orig: Server_InitNewRoundState @0x51C92F -> sub_436EB0 -> BanList_InitFromMission
// @0x5098F0]. The console's BAN / UNBAN and the admin PLAYER BAN rewrite it at
// once. A joiner whose decrypted `<local>PCID` cookie is on the list is refused
// with punt code 29 [orig: Server_ValidatePlayerJoinRequest @0x512737..0x512759].
// The record is docs/net/novaworld-net-re.md §6.9 ("The ban lists").
//
// THE FILE. The loader reads the whole file, cuts it into lines at LF, CR LF or a
// lone CR (2047 characters at most; a longer line continues as the next), cuts
// each line at its first `//`, quotes or not, and tokenizes it on white space
// with `"` toggling a quoted run (the quotes dropped). A first pass takes every
// `VERSION <n>` line (the last wins); when n is 1 a second pass appends every
// `BAN <pcid> [<name>]` line whose PCID is not empty, in file order and with no
// duplicate check. Keywords compare case-insensitively.
// [orig: BanList_LoadFromFile @0x4DB290; Text_ReadLineFromBuffer @0x4DAF60;
//  String_TokenizeQuoted @0x4DB000; String_CompareCaseInsensitive_0 @0x4DB0C0;
//  LinkedList_AddEntry @0x4DAE00]
//
// The writer is BanList_SaveToFileWithHeader: a fixed comment header, `VERSION 1`,
// the usage block, then one `BAN "<pcid>" "<name>"` line per entry in list order
// (`BAN "<pcid>"` for an empty name), CR LF throughout, built from the model.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::banlist {

// The file's name in the working directory [orig: "banlist.txt" @0x7C0ED8].
inline constexpr const char *kPcidFileName = "banlist.txt";

// The one version the loader accepts and the writer prints
// [orig: BanList_LoadFromFile `cmp esi, 1` @0x4DB46E; "VERSION %ld" @0x7CD084].
inline constexpr long kPcidBanListVersion = 1;

// One node {pcid, name} [orig: LinkedList_AddEntry @0x4DAE00 — the key and the value
// copied inline after a {list, prev, next, key, value} header].
struct PcidBan {
	std::string pcid;
	std::string name;
};

// g_BannedPcidList @0xC867A0: the nodes in list order (appended at the tail).
struct PcidBanList {
	std::vector<PcidBan> entries;
};

// String_TokenizeQuoted: white space (the C locale's isspace) separates tokens outside
// quotes, a `"` toggles a quoted run and is dropped, a backslash copies as itself.
// [orig: String_TokenizeQuoted @0x4DB000]
std::vector<std::string> tokenize_quoted(std::string_view line);

// LinkedList_AddEntry: appends at the tail; false (nothing added) for an empty PCID.
// [orig: LinkedList_AddEntry @0x4DAE00 — `!key || !*key` @0x4DAE12]
bool add(PcidBanList &list, std::string_view pcid, std::string_view name);
// LinkedList_FindByName: the first node whose PCID equals `pcid` case-insensitively.
// [orig: LinkedList_FindByName @0x4DBBD0 -> String_CompareCaseInsensitive_0 @0x4DB0C0]
const PcidBan *find(const PcidBanList &list, std::string_view pcid);
// The UNBAN's find and unlink: false when the PCID is not listed.
// [orig: Server_HandleBanPuntCommand @0x50B380 -> LinkedList_FindByName @0x4DBBD0,
//  LinkedList_RemoveAndFreeNode @0x4DAF10]
bool remove(PcidBanList &list, std::string_view pcid);

// BanList_LoadFromFile over the file's bytes: no list when VERSION is not 1.
std::optional<PcidBanList> parse_pcid_list(const char *text, size_t size);
// parse_pcid_list() over the file at `path`; no list when it does not open or read whole.
// [orig: BanList_LoadFromFile @0x4DB290 — _lopen, _llseek, _hread; a short read fails]
std::optional<PcidBanList> load_pcid_list(const std::string &path);

// BanList_SaveToFileWithHeader's text. [orig: @0x4DB590]
std::string write_pcid_list(const PcidBanList &list);
// write_pcid_list() to `path`, replacing it. False when it cannot open.
bool save_pcid_list(const std::string &path, const PcidBanList &list);

} // namespace opennova::banlist
