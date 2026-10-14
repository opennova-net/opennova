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

#include <net/novaworld/lobby_vars.h> // HostRegistration
#include <runtime/hud/game_text_lookup.h>

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

} // namespace opennova::inmatch
