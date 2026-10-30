#pragma once

#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>

#include <cstdint>
#include <string>

// The single player's mission opennova-game runs until the engine frontend (R3's runtime/frontend)
// takes the game flow over: one mission named on the command line, booted through the engine's
// mission load executor (runtime/mission/mission_load.h) with headless legs, the way
// opennova-serve boots its maps (no device stage loads: R5 brings the render world), and run by
// the engine's session over the standalone single player's role, the in-process listen server
// (inmatch::HostRole of kind SinglePlayer, SinglePlayer_StartMission's bring-up), as the Godot
// game runs a standalone mission.
namespace opennova::game {

struct MissionRequest {
	// The game folder (its PFF set, or a loose tree) and the mission file in it.
	std::string root;
	std::string mission;
	// The expansion to mount (/exp), "" for none.
	std::string expansion;
	// The title the mission runs as (a title code: "jo", "dfx2", "acidgambit").
	std::string game;
};

class LocalMission {
public:
	LocalMission();
	LocalMission(const LocalMission &) = delete;
	LocalMission &operator=(const LocalMission &) = delete;

	// The mission mounted, booted and its session running. False with `error` when any stage
	// failed; the session is Failed then.
	bool load(const MissionRequest &request, std::string &error);

	inmatch::Session &session() { return session_; }
	inmatch::HostRole &role() { return role_; }
	mission::MissionKernel &kernel() { return kernel_; }
	// The local player stands (the host's own body, which the round init spawned).
	bool has_player() const;
	uint32_t game_type() const { return game_type_; }

private:
	mission::MissionKernel kernel_;
	inmatch::HostRole role_;
	inmatch::Session session_;
	uint32_t game_type_ = 0;
};

} // namespace opennova::game
