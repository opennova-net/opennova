#pragma once

// opennova-serve's NovaWorld listing source (ADR 0051 d6): the live in-process
// match behind the lister's NovaWorld session. It does what the game's
// SessionDrive does for its hosted match (godot/src/world/session_drive.cpp,
// godot/src/simulation/simulation_net.cpp): the columns from the live session
// (inmatch::read_session_listing_columns, the round clock's TimeLeft among
// them), the PlayerList from the host's connections (a Serve Only host has no
// slot 0), the GSID / AppId / cookie keys and the join-ticket arm on the
// server context, the NWU session's facts the authority's NovaWorld exit
// reads, and the service's ServerCommands and ServerPlayerEnterResults run on
// the match; a changed name, message or mpreset reaches the cfg block and
// game.cfg. A ServerCommand that arrives before the match binds (the hosting
// wait) runs on a session-less context, where only SetMPReset passes its gate
// (on_command).
//
// A server listed on a second service as well (ServeOptions::also_master_host,
// none of retail's: a stock host lists on one; D-NET-1385, ADR 0068) lists the same match there through
// SecondListing: the second lister's host role reads the same columns and runs
// the same server-info refresh (set_second), and its ServerCommands run on the
// match. The match's own NovaWorld facts stay the first listing's: its GSID is
// the 0x81's SUS1, its join-ticket arm and enter requests the match's, and its
// session word the one the match's NovaWorld exit reads, so the second service
// stopping its listing never ends the match.

#include "listing_source.h"

#include <formats/gamecfg/game_cfg.h>
#include <net/novaworld/lobby_vars.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/mission/mission_kernel.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace opennova::serve {

class Lister;

class ServeListing final : public ListingSource {
public:
	// The server's game.cfg, which a ServerCommand's SetServerName /
	// SetServerMsg / SetMPReset writes and saves (the admin console's seam).
	struct Seams {
		gamecfg::GameCfg *config_block = nullptr;
		std::function<void()> save_config;
		// The mounted gametext, which the GameType column's GateTypeAbbrev text reads.
		hud::GameTextLookup gametext;
	};

	// `base`: the columns the host file and the mount give before the mission
	// boots (the name, the message, the caps, the rules, the starting map).
	ServeListing(HostRegistration base, Seams seams) : base_(std::move(base)), seams_(std::move(seams)) {}
	// A map change's columns: the next map's game type and title.
	void set_base(HostRegistration base) { base_ = std::move(base); }

	// The booted match: from here the columns read the session config, the
	// roster the host's connections, and the session's facts reach the match.
	// A map change binds again: the kernel is fresh, the server context kept.
	void bind(inmatch::HostRole &role, mission::MissionKernel &kernel, Lister &lister);
	void unbind();
	// The second listing's lister (null: none), bound or not: its host role reads the match's
	// columns and runs the match's server-info refresh beside the first's. It outlives the
	// setting: the embedder clears it before the lister goes.
	void set_second(Lister *second);
	// Feed the NWU session's facts to the authority (its 62-frame NovaWorld
	// exit reads them) and the session's keys to the server context. The
	// server calls it once a frame, also after the lister finished.
	void sync_session();

	bool start(int &exit_code) override;
	bool refresh(uint32_t now_ms, bool force) override;
	HostRegistration registration() const override;
	std::vector<HostPlayerSlot> wanted_roster(const std::map<int, HostPlayerSlot> &current) const override;
	void on_command(const ServerCommand &command) override;
	void on_player_enter_result(const ClientSession::PlayerEnterResult &result) override;
	// A ServerCommand from `issuer`'s service, run on the match; a stop it asks for (PuntPlayer at
	// the server's own slot) leaves `issuer`'s hosting.
	void run_command(const ServerCommand &command, Lister *issuer);

private:
	// `lister`'s host role reads the match's columns at each server-info update.
	void install_column_source(Lister &lister);

	HostRegistration base_;
	Seams seams_;
	inmatch::HostRole *role_ = nullptr;
	mission::MissionKernel *kernel_ = nullptr;
	Lister *lister_ = nullptr;
	Lister *second_ = nullptr;
};

// The second listing's source (ServeOptions::also_master_host): the first's live match, read the
// same way (its columns and roster), its ServerCommands run on it (ServeListing::run_command), a
// stop it asks for leaving only its own hosting. The match's join tickets stay the first
// listing's, so it announces no joiner and takes no ServerPlayerEnterResult.
class SecondListing final : public ListingSource {
public:
	explicit SecondListing(ServeListing &first) : first_(first) {}
	// The lister this source lists for, a stop's target (null: none). It outlives the setting.
	void set_lister(Lister *lister) { lister_ = lister; }

	bool start(int &exit_code) override {
		(void)exit_code;
		return true;
	}
	bool refresh(uint32_t now_ms, bool force) override { return first_.refresh(now_ms, force); }
	HostRegistration registration() const override { return first_.registration(); }
	std::vector<HostPlayerSlot> wanted_roster(const std::map<int, HostPlayerSlot> &current) const override {
		return first_.wanted_roster(current);
	}
	void on_command(const ServerCommand &command) override { first_.run_command(command, lister_); }

private:
	ServeListing &first_;
	Lister *lister_ = nullptr;
};

} // namespace opennova::serve
