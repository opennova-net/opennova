#include "common/retail_mission_files.h"

#include <base/io/log.h>
#include <base/gameprofile/game_type.h>
#include <runtime/terrain_query/terrain_field_build.h>

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

bool RetailMissionRig::load_terrain(std::string &error) {
	// The same field the game builds (Simulation::set_terrain_height_field):
	// the engine's one owning cpt/trn(+charmap) loader (ADR 0042 d4), filled
	// into the kernel's own store.
	return terrain::terrain_field_store_load(terrain_store, index, mission.get_terrain(), error);
}

bool RetailMissionRig::boot(const BootOptions &options, std::string &error) {
	listen_server = options.listen_server;
	std::string terrain_error;
	if (options.terrain && !load_terrain(terrain_error))
		std::printf("rig: terrain not loaded (%s) - the ground solve will not run\n",
				terrain_error.c_str());
	mission::KernelBootOptions kernel_options;
	kernel_options.playable = options.playable;
	kernel_options.wac = options.wac;
	kernel_options.collision = options.collision;
	kernel_options.seat_specs = options.seat_specs;
	kernel_options.infantry_adm = options.infantry_adm;
	kernel_options.game_type = game_type::for_mission_attribs(mission.header.attrib_flags);
	if (listen_server)
		kernel_options.bringup_net_session = [this] { host_role.bring_up_singleplayer(); };
	return mission::MissionKernel::boot(kernel_options, error);
}

void RetailMissionRig::tick() {
	role().run_tick(inmatch::TickInput{});
}

void RetailMissionRig::tick(int count) {
	for (int i = 0; i < count; ++i) tick();
}



} // namespace opennova::testrig
