#pragma once

// opennova-serve's NovaWorld listing source (ADR 0051 d6): the live in-process
// match behind the lister's NovaWorld session. It does what the game's
// SessionDrive does for its hosted match (godot/src/world/session_drive.cpp,
// godot/src/simulation/simulation_net.cpp): the columns from the session
// config, the PlayerList from the host's connections (a Serve Only host has no
// slot 0), the GSID / AppId / cookie keys and the join-ticket arm on the
// server context, the NWU session's facts the authority's NovaWorld exit
// reads, the round clock the TimeLeft column re-reads, and the service's
// ServerCommands and ServerPlayerEnterResults run on the match; a changed
// name, message or mpreset reaches the cfg block and game.cfg.

#include "listing_source.h"

#include <formats/gamecfg/game_cfg.h>
#include <net/novaworld/lobby_vars.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/mission/mission_kernel.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace opennova::nw_lister {
class Lister;
}

namespace opennova::serve {

class ServeListing final : public nw_lister::ListingSource {
public:
	// The server's game.cfg, which a ServerCommand's SetServerName /
	// SetServerMsg / SetMPReset writes and saves (the admin console's seam).
	struct Seams {
		gamecfg::GameCfg *config_block = nullptr;
		std::function<void()> save_config;
	};

	// `base`: the columns the host file and the mount give before the mission
	// boots (the name, the message, the caps, the rules, the starting map).
	ServeListing(HostRegistration base, Seams seams) : base_(std::move(base)), seams_(std::move(seams)) {}
	// A map change's columns: the next map's game type and title.
	void set_base(HostRegistration base) { base_ = std::move(base); }

	// The booted match: from here the columns read the session config, the
	// roster the host's connections, and the session's facts reach the match.
	// A map change binds again: the kernel is fresh, the server context kept.
	void bind(inmatch::HostRole &role, mission::MissionKernel &kernel, nw_lister::Lister &lister);
	void unbind();
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

private:
	HostRegistration base_;
	Seams seams_;
	inmatch::HostRole *role_ = nullptr;
	mission::MissionKernel *kernel_ = nullptr;
	nw_lister::Lister *lister_ = nullptr;
};

} // namespace opennova::serve
