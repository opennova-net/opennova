#include "common/retail_mission_files.h"

#include <base/io/log.h>
#include <base/gameprofile/game_type.h>

#include <cstdio>
#include <string>
#include <vector>

namespace opennova::testrig {

RetailMissionRig::RetailMissionRig() {
	local_role.bind(*this);
	host_role.bind(*this);
	host_role.set_kind(inmatch::RoleKind::ListenHost);
	// The kernel logs its boot/tick diagnostics through io/log.h and stays
	// silent unless the embedder installs a sink; the ctest binaries want the
	// warnings on stdout, exactly as the pre-promotion rig printed them.
	if (io::log_sink_slot() == nullptr)
		io::set_log_sink([](io::LogLevel level, const char *message) {
			if (level >= io::LogLevel::kWarn) std::printf("%s\n", message);
		});
}

bool RetailMissionRig::boot(const BootOptions &options, std::string &error) {
	listen_server = options.listen_server;
	mission::KernelBootOptions kernel_options;
	kernel_options.playable = options.playable;
	kernel_options.wac = options.wac;
	// The terrain field is the kernel boot's own load through the mounted
	// root (the same field the game builds from its parsed documents).
	kernel_options.terrain = options.terrain;
	kernel_options.collision = options.collision;
	kernel_options.seat_specs = options.seat_specs;
	kernel_options.infantry_adm = options.infantry_adm;
	kernel_options.restart = options.restart;
	kernel_options.game_type = game_type::for_mission_attribs(mission.header.attrib_flags);
	if (listen_server)
		kernel_options.bringup_net_session = [this] { host_role.bring_up_singleplayer(); };
	return mission::MissionKernel::boot(kernel_options, error);
}

void RetailMissionRig::tick() {
	role().run_tick(inmatch::TickInput{});
	// The frame's render follows its tick: the SP end-of-round cine's render
	// pass (inmatch::Session runs it after each frame's drain).
	world.epilog.render_pass();
}

void RetailMissionRig::tick(int count) {
	for (int i = 0; i < count; ++i) tick();
}



} // namespace opennova::testrig
