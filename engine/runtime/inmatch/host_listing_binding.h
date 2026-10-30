#pragma once

// THE NOVAWORLD HOST LISTING'S MATCH SIDE (ADR 0051 d6, ADR 0077): what a hosting NovaWorld
// session's host role (NwuHostRole) and the in-match host it lists (its server context) hand each
// other while the session hosts the match. Both NovaWorld hosts bind through it: the game's listen
// host (godot SessionDrive) and opennova-serve's live listing source (apps/serve ServeListing).
//
// The binding owns the match half: the context's hooks into the session (the server-info update,
// a LAN-only host's joiner address, the join-phase watchdog's ClientPlayerEnterRequest, the
// METPROTOCOL gate reports), each role's Host-list column read off the context, the registration
// the context carries (the GSID, the AppIds, the login PCID, the join-ticket arms), the NWU
// session's facts the authority's NovaWorld exit reads, and the PlayerList rows. The embedder keeps
// the session's driver (the game's NovaWorld menu session; opennova-serve's Lister: the gate, the
// login and HOSTKEY, the host request, the pump, when columns and roster go out), the
// ServerCommands' cfg block and save, the mission start's publish and a listen host's own
// PlayerList slot.
//
// A second listing (opennova-serve's --also-master-host, none of retail's: a stock host lists on
// one service; D-NET-1385) is listing 1: its role reads the same columns and runs the same
// server-info update, a joiner its service sent goes to its session, and its AppId and ticket arm
// ride the context beside the first's. Everything else of the match's NovaWorld facts stays the
// first listing's.

#include <net/novaworld/lobby_vars.h>        // HostRegistration, HostPlayerSlot
#include <net/novaworld/nwu_lobby_session.h> // NwuLobbySession::MatchFacts
#include <runtime/hud/game_text_lookup.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace opennova {
class NwuHostRole;
}

namespace opennova::inmatch {

struct NapiNPServerCtx;

class HostListingBinding {
public:
	// The hosted match's server context as it stands at the call; null while there is none (a map
	// change's load, which the next mission's bind follows). The embedder resolves it the way its
	// match lives: opennova-serve's kept context, the game's world's simulation.
	using MatchContext = std::function<NapiNPServerCtx *()>;

	HostListingBinding() = default;
	// Touches neither the match nor the roles (either may be gone already): what the binding
	// installed and an unbind did not take back only does nothing from here on. The embedder
	// unbinds while its match and its roles stand.
	~HostListingBinding();
	HostListingBinding(const HostListingBinding &) = delete;
	HostListingBinding &operator=(const HostListingBinding &) = delete;

	// The hosting session `first` meets its match: the context's hooks reach the sessions, and
	// each role reads its columns off the context (read_columns) at every server-info update. A
	// bound binding rebinds (a map change: the context kept, the columns the next map's).
	// `gametext` resolves the GameType column's and the gate reports' GateTypeAbbrev text. `first`
	// stands until the embedder unbinds.
	void bind(MatchContext match, NwuHostRole &first, hud::GameTextLookup gametext);
	// The second listing's role (null: none), kept across bind and unbind: while bound its role
	// reads the match's columns beside the first's and runs the same server-info update, and each
	// sync_registration carries its AppId and arm. The embedder clears it before its role goes.
	void set_second(NwuHostRole *second);
	// The session leaves the match: the context's hooks and registration go (on the context the
	// match resolves to now; a context it no longer reaches keeps hooks that do nothing), and the
	// roles' column reads with them.
	void unbind();
	bool bound() const { return ends_->first != nullptr; }

	// The Host list's columns the session does not own, read off the bound match into `r` (the
	// server name and message, the game type, the mission, the caps, the join lock, the rules, the
	// round clock, the cookie keys, the GCC: read_session_listing_columns). False, `r` untouched,
	// with no match: a map change's load, whose last read stands.
	// [orig: Lobby_UpdateServerInfo @0x4FE8C0 - the Host list SetOrCreates @0x4FE911..0x4FF3FF read
	//  the session's globals at each run]
	bool read_columns(HostRegistration &r) const;

	// The registration onto `ctx`, once an update while the session hosts: the first listing's GSID
	// (the 0x81's SUS1, empty while its connection is down, a re-host's fresh one), its AppId and
	// join-ticket arm (the code-9 leg and the join-phase watchdog), the second listing's AppId and
	// arm while it hosts (none once it goes, so a joiner still carrying its AppId is refused 9, as
	// retail refuses one whose .joi predates a fresh host request), and a listen host's own
	// player's PCID (`login_pcid`, its login cookie; "" for a host with no loopback connection).
	// [orig: CNapiGameSession_HandleHostVerifyResponse @0x4D59D0 - the GSID @0x4D5BD6..0x4D5C0F;
	//  CNapiNetwork_CheckPlayerTimeouts @0x4C8AD0 - the arm @0x4C8B88]
	void sync_registration(NapiNPServerCtx &ctx, const std::string &login_pcid) const;

private:
	// What the installed hooks reach, shared with them so a hook outliving the binding (left on a
	// context unbind no longer reached) finds no role.
	struct Ends {
		MatchContext match;
		NwuHostRole *first = nullptr;
		NwuHostRole *second = nullptr;
		hud::GameTextLookup gametext;
		// The role of the listing a joiner came through: 1 the second's, any other the first's.
		NwuHostRole *listing(uint8_t index) const { return index == 1 ? second : first; }
	};

	static void install_column_source(const std::shared_ptr<Ends> &ends, NwuHostRole &role);
	static bool read_match_columns(const Ends &ends, HostRegistration &r);

	std::shared_ptr<Ends> ends_ = std::make_shared<Ends>();
	NwuHostRole *second_ = nullptr; // set_second's, which a bind hands its ends
};

// The NWU session's facts onto the authority's context, once an update: its in-use word, its
// hosting / playing word and its own exit store, which the 62-frame NovaWorld exit reads, and its
// flags, whose bits the connection indicators' N icon steps on (a Serve Only host's status page,
// D-NET-971). A zero exit store leaves the context's.
// [orig: Game_ProcessMainFrame's NovaWorld exit @0x52654F..0x52657C (dword_B5FD2C, dword_B60108)]
void set_nwu_match_facts(NapiNPServerCtx &ctx, const NwuLobbySession::MatchFacts &facts);

// The PlayerList the session publishes for the match: one slot per player the match added
// (host_player_list_rows), the listen host's own connection as `own` (its name, the login PCID)
// with the PlayerTeam / PlayerType the match keeps for it; a host with no own slot (`own` null: a
// Serve Only host builds no local connection, or a listen host before its own slot is set) skips
// that row.
// [orig: Server_PlayerAdd @0x51CBC0 - the five vars @0x51D441..0x51D4AA;
//  CNapiGameSession_CreateSession @0x4C9B73]
std::vector<HostPlayerSlot> host_listing_roster(
		const NapiNPServerCtx &ctx, const HostPlayerSlot *own);

} // namespace opennova::inmatch
