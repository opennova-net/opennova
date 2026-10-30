#include "local_mission.h"

#include <base/gameprofile/game_type.h>
#include <base/io/log.h>
#include <base/title/title.h>
#include <runtime/mission/mission_load.h> // the load plan's executor (the game's order)
#include <runtime/world/title_rules.h>

namespace opennova::game {

LocalMission::LocalMission() : role_(inmatch::RoleKind::SinglePlayer), session_(role_) {
	role_.bind(kernel_);
}

bool LocalMission::has_player() const {
	return kernel_.world.cached.local_player.valid();
}

bool LocalMission::load(const MissionRequest &request, std::string &error) {
	const Title *title = title_by_code(request.game);
	if (title == nullptr) {
		error = "no title is named '" + request.game + "'";
		return false;
	}
	if (!session_.begin_load().applied()) {
		error = "the session cannot load";
		return false;
	}
	// The load plan's order, the game's (mission/mission_load.h). A headless embedder fills the
	// session's legs alone, as opennova-serve does: the runtime's start mounts the folder, reads
	// the mission and boots it, the single player's session coming up inside the boot (its
	// bring-up hook, SinglePlayer_StartMission's create and round init, inmatch/host_role.h), and
	// the play leg ends the load. The device stages (the loading screen, the scene's loads, the
	// music context, the HUD) are R5's and R3's; their legs stay empty.
	mission::MissionLoadLegs legs;
	legs.device.present = [](int percent) {
		io::logf(io::LogLevel::kDebug, "opennova-game: loading %d%%", percent);
	};
	legs.session.start_runtime = [this, &request, &error, title]() -> int {
		if (!kernel_.open(request.root, request.mission, error, request.expansion)) return 1;
		game_type_ = game_type::for_mission_attribs(kernel_.mission.header.attrib_flags);
		mission::KernelBootOptions options;
		// The World runs the title's rules from its load (the session's bring-up stamps the same).
		options.title_rules = &world::title_rules(title->code);
		options.playable = true;
		options.wac = true;
		options.terrain = true;
		options.game_type = game_type_;
		options.bringup_net_session = [this, title] { role_.bring_up_singleplayer(*title); };
		return kernel_.boot(options, error) ? 0 : 1;
	};
	legs.session.role = [] { return mission::MissionLoadRole::Authority; };
	// The game runs past the load's end: the session's mission start, whose first frames bank no
	// load time (inmatch::Session::complete_load).
	legs.session.play = [this] { (void)session_.complete_load(); };
	const mission::MissionLoadArm arm;
	if (mission::run_mission_load(arm, legs) != 0 ||
			session_.state() != inmatch::State::Running) {
		if (error.empty()) error = "the mission did not start";
		(void)session_.fail({inmatch::SessionErrorCode::LoadFailed, error});
		return false;
	}
	io::logf(io::LogLevel::kInfo, "opennova-game: '%s' up: %d entities, terrain %s, WAC %s",
			request.mission.c_str(), kernel_.promo.spawned,
			kernel_.terrain_store.valid() ? "loaded" : "absent",
			kernel_.wac_loaded ? "loaded" : "absent");
	return true;
}

} // namespace opennova::game
