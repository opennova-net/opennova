#pragma once

// The authority's two ban lists and their files (docs/net/novaworld-net-re.md §6.9, "The ban
// lists"): banlist.txt's PCIDs, which only an authority in a NovaWorld session keeps, and
// banned.txt's addresses and names, which every authority keeps. The lists live on the
// server context; the file timing is retail's: both reload at the round init, banlist.txt is
// rewritten at once by every edit, banned.txt only at the process's shutdown when an in-game
// ban dirtied it. The console's BAN / UNBAN / PUNT / BANDWIDTH line is here too.

#include <formats/banlist/address_ban_list.h>
#include <formats/banlist/pcid_ban_list.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace opennova::inmatch {

struct NapiNPServerCtx;
struct NapiNPConnection;

struct ServerBanLists {
	// The directory both files live in: retail's working directory, which the embedder names
	// (opennova-serve its own). Unset, the lists still load (empty) and work, and no file is
	// read or written: the game's hosts and the tests (D-NET-295's residual).
	std::optional<std::string> directory;
	// g_BannedPcidList @0xC867A0: absent off a NovaWorld authority session, where BAN and
	// UNBAN do nothing and a PLAYER BAN is a punt.
	std::optional<banlist::PcidBanList> pcids;
	// g_BannedNameCount / g_BannedIdList / g_BannedNameList @0xC8FF20..0xC90728.
	banlist::AddressBanList addresses;
	// dword_C8FF24: an in-game ban added an entry; the shutdown's save needs it.
	bool addresses_dirty = false;
};

// The round init's PCID leg: the list freed, then, only for an authority in a session on the
// NovaWorld network type, banlist.txt loaded; a missing file or a VERSION other than 1 yields
// a fresh empty list, saved over the file.
// [orig: Server_InitNewRoundState @0x51C92F -> sub_436EB0 @0x436EB0 -> BanList_InitFromMission
//  @0x5098F0]
void Server_ReloadPcidBanList(NapiNPServerCtx &ctx);

// The round init's address leg: the count and the dirty word zeroed, banned.txt re-read.
// [orig: Server_InitNewRoundState @0x51CB25..0x51CB3B]
void Server_ReloadAddressBanList(NapiNPServerCtx &ctx);

// BanList_SaveToFileWithHeader over the PCID list (when it and a directory exist).
// [orig: BanList_SaveToFileWithHeader @0x4DB590]
void Server_SavePcidBanList(const NapiNPServerCtx &ctx);

// The shutdown's banned.txt save: only when the list and the dirty word are both nonzero.
// [orig: Game_ShutdownSubsystems @0x4A539D -> j_BanList_SaveToFile @0x508E20 ->
//  BanList_SaveToFile @0x4FDD70, the gates @0x4FDD7E..0x4FDD92]
void Server_SaveAddressBanList(const NapiNPServerCtx &ctx);

// A joiner's PCID on the list, the game-layer join's code-29 refusal (an empty PCID matches
// nothing: the loader and LinkedList_AddEntry keep no empty one).
// [orig: Server_ValidatePlayerJoinRequest @0x512737..0x512759]
bool Server_PcidBanned(const NapiNPServerCtx &ctx, std::string_view pcid);
// A joiner's name on banned.txt, compared exactly (strcmp), the code-31 refusal.
// [orig: Server_ValidatePlayerJoinRequest @0x512989..0x5129CB, the refusal @0x512A6C]
bool Server_NameBanned(const NapiNPServerCtx &ctx, std::string_view name);
// The game-layer join's ban and duplicate legs, past the cookie's NAMEINFO / PCID decrypt
// legs (codes 23 / 24 / 25, not ported): on a NovaWorld session (the host has a local
// address), a joiner whose decrypted PCID is listed is refused 29; then, with game.cfg's
// mphostpuntplayerswithsamepcids set and the joiner's PCID decrypted (`pcid_decrypted`, the
// cookie load's answer: retail's walk reaches the leg only past its own decrypt), a PCID
// that more than one of the session's connections holds (case-insensitively, the joiner
// counted) flags every OTHER remote connection holding it for the next 0x79 boundary's kick
// (Server_KickDuplicatePcids), writes the _inout.txt REJECTING line for every holder, and
// refuses the joiner 30; in a session, a listed name is refused 31. 0 admits.
// [orig: Server_ValidatePlayerJoinRequest — the local-address block @0x5125F2..0x512956,
//  LinkedList_FindByName @0x512737 -> code 29 @0x512751; the duplicate leg: the cfg word
//  @0x512771..0x512778, NapiSession_CountPlayersByName(ctx, 1, pcid) @0x51278D (> 1
//  @0x512795), the walk over ctx+0x24 @0x51279E..0x51287C (Napi_StrCaseEqual @0x5127CF, not
//  the joiner @0x5127DF, a connection that is not the local one @0x5127E3..0x5127F1, the flag
//  @0x5127F3, the /INOUT line @0x5127F7..0x512865), code 30 @0x512890; the session test
//  @0x51297D, the name walk @0x512989..0x5129CB -> code 31 @0x512A6C]
uint32_t Server_ValidateJoinBans(NapiNPServerCtx &ctx, NapiNPConnection &conn, bool pcid_decrypted);
// The S2C 0x79 boundary's kick of the connections the join validator flagged (nothing off the
// boundary, `quality_boundary` false): each flagged remote connection latches {DS its role, DC 2,
// 0, 0, "N.C:SPTWNTBP[<its PCID>]", DPC 0, ""} when no record is latched yet and is marked for the
// next protocol pump's teardown. A misnomer in the IDB: no spawn or ban is involved. [orig:
// CNapiServer_DisconnectPendingSpawnBans @0x4C9290 — the walk over ctx+0x24
//  @0x4C92CB..0x4C93F3, the flag @0x4C92F5, the connection and not-local tests
//  @0x4C92FE..0x4C930F, sprintf("N.C:SPTWNTBP[%s]", node+0x250) @0x4C9333..0x4C9340, the role
//  @0x4C9355..0x4C9375, DC 2 @0x4C938B, the latch-if-!valid @0x4C93BB..0x4C93D5,
//  CNapiNPConnection_RequestDisconnect @0x4C93DD; called by Server_TickUpdate @0x51E20C]
void Server_KickDuplicatePcids(NapiNPServerCtx &ctx, bool quality_boundary);
// A source address on banned.txt, the protocol join's family-14 reason-3 refusal.
// [orig: CNapiNetwork_ValidateJoinRequest @0x4C6203..0x4C6217]
bool Server_AddressBanned(const NapiNPServerCtx &ctx, uint32_t address);

// The authority console's ban and punt line with no sender (authorized, and no chat back):
// `BAN <#>` and `UNBAN <#>` over the PCID list (a connected slot's PCID; the host's own slot
// refused), `PUNT <#|ALL>` (reason 33, `S.C:SP#` / `S.C:SPA`), `BANDWIDTH [n]` (the entity
// send budget n / 5 clamped to 100..1600, 600 with no value). True when the line was one of
// them (the caller's chain stops); a BAN or UNBAN with no list is still handled.
// [orig: Server_HandleBanPuntCommand(line, 0) @0x50B380, from Client_ProcessConsoleCommand
//  @0x520E80]
bool Server_HandleBanPuntCommand(NapiNPServerCtx &ctx, std::string_view line);

} // namespace opennova::inmatch
