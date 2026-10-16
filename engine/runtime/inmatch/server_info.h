#pragma once

// THE SERVER-INFO UPDATE'S SESSION SIDE: the Host list's columns as the
// update reads them off the live session at each run (the server name and
// message, the game type, the mission's title and climate, the published
// cap, the join lock, the skins and tracers rules, the password, the country
// and the round clock), so a NovaWorld listing follows its match: a map change, a
// rename or a lock reaches the next update with no re-registration. The
// embedders (the game's SessionDrive, opennova-serve's ServeListing) hand the
// result to their NovaWorld host role; the columns the session does not own
// (LobbyName, HostKey, the caps the host leg clamps, the machine locale, the
// role's own Players, PCIDKey and Age) stay the registration's. And the join
// lock's toggle, which runs the update at once.
// [orig: Lobby_UpdateServerInfo @0x4FE8C0 — the Host list SetOrCreates
//  @0x4FE911..0x4FF3FF; Server_ToggleJoinLock @0x50D750]

#include <net/novaworld/lobby_vars.h> // HostRegistration, HostPlayerSlot
#include <runtime/hud/game_text_lookup.h>

#include <vector>

namespace opennova::inmatch {

struct NapiNPServerCtx;

// Overwrite `r`'s session-owned columns from the host context (its config, its
// join lock, its world's round clock and its mission). `gametext` resolves the
// GateTypeAbbrev text of the session's game type (an empty lookup or a missing
// key: "").
void read_session_listing_columns(
		HostRegistration &r, const NapiNPServerCtx &ctx, const hud::GameTextLookup &gametext);

// The lockgame action on an authority in a session: the join lock flips (the join validate refuses
// every joiner while it is set, the Locked column reads it), then the server-info update runs at
// once through the context's hook, a NovaWorld embedder's (none: nothing runs, as retail's update
// returns outside a NovaWorld session). Returns the new lock.
// [orig: Server_ToggleJoinLock @0x50D750 — the flip of g_ServerJoinLocked @0x50D752..0x50D75D,
//  Lobby_UpdateServerInfo(0) @0x50D762, the new value returned @0x50D767; its one caller,
//  Input_HandleActionBinding's case 119 @0x49BC36]
bool Server_ToggleJoinLock(NapiNPServerCtx &ctx);

// One PlayerList row per player the match added, the roster both NovaWorld embedders sync
// (opennova-serve's ServeListing, the game's SessionDrive): every connection past its
// player add, at its slot, with the add's connection id, its joiner name bytes, its UDP source
// as "a.b.c.d:port", its cookie's PCID and the PlayerTeam / PlayerType the match keeps for it
// (NapiNPConnection::player_list_team / player_list_type). `own` marks the listen host's own
// client-side connection, whose identity the embedder's NovaWorld login supplies; a mode-1
// host has none.
// [orig: Server_PlayerAdd @0x51cbc0 - PlayerIpAndPort @0x51d3d1..0x51d3f2 (Napi_FormatAddress
//  @0x62ded0), the five vars @0x51d441..0x51d4aa]
struct HostPlayerListRow {
	HostPlayerSlot slot;
	bool own = false;
};
std::vector<HostPlayerListRow> host_player_list_rows(const NapiNPServerCtx &ctx);

} // namespace opennova::inmatch
