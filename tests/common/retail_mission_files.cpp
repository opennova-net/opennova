#include "common/retail_mission_files.h"

#include <base/io/log.h>
#include <formats/cpt/cpt_io.h>
#include <formats/pcx/pcx_io.h>
#include <formats/trn/trn_io.h>
#include <net/netsim/idatagram_socket.h>
#include <net/npwire/game_type.h>
#include <runtime/terrain_query/terrain_field_build.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace opennova::testrig {

namespace {

// The socketless SP host: no datagrams in or out (the loopback carries the
// host's own client).
class NullDatagramSocket final : public netsim::IDatagramSocket {
public:
	int recv_from(uint8_t *, std::size_t, PeerAddr &) override { return 0; }
	void send_to(const PeerAddr &, const uint8_t *, std::size_t) override {}
};

} // namespace

RetailMissionRig::RetailMissionRig() {
	// The kernel logs its boot/tick diagnostics through io/log.h and stays
	// silent unless the embedder installs a sink; the ctest binaries want the
	// warnings on stdout, exactly as the pre-promotion rig printed them.
	if (io::log_sink_slot() == nullptr)
		io::set_log_sink([](io::LogLevel level, const char *message) {
			if (level >= io::LogLevel::kWarn) std::printf("%s\n", message);
		});
}

bool RetailMissionRig::load_terrain(std::string &error) {
	const std::string tname = mission.get_terrain();
	if (tname.empty()) {
		error = "the mission names no terrain";
		return false;
	}
	std::vector<uint8_t> cpt_bytes, trn_bytes;
	if (!index.read_file(tname + ".cpt", cpt_bytes) || !index.read_file(tname + ".trn", trn_bytes)) {
		error = tname + ".cpt/.trn are not under the mount";
		return false;
	}
	std::string terr_err;
	if (!load_cpt(cpt_bytes.data(), cpt_bytes.size(), cpt, terr_err)) {
		error = tname + ".cpt: " + terr_err;
		return false;
	}
	std::string raw(reinterpret_cast<const char *>(trn_bytes.data()), trn_bytes.size());
	std::istringstream ts(raw);
	if (!load_trn(ts, trn, terr_err) || cpt.depth_buffer.empty()) {
		error = tname + ".trn: " + terr_err;
		return false;
	}
	// The charmap surface raster for the footstep surface pick, decoded from
	// the .trn-named PCX like the shell does (TerrainData's charmap slot);
	// absent or undecodable = no surface map, the sampler's "no charmap ->
	// surface 1" leg.
	IndexedImage8 charmap;
	std::vector<uint8_t> charmap_bytes;
	if (!trn.charmap.empty()) {
		if (!index.read_file(trn.charmap, charmap_bytes)) {
			std::printf("rig: charmap %s is not under the mount - no surface map\n",
					trn.charmap.c_str());
		} else {
			std::string charmap_err;
			if (!decode_pcx_indexed(charmap_bytes.data(), charmap_bytes.size(), charmap,
						charmap_err)) {
				charmap = IndexedImage8{};
				std::printf("rig: charmap %s did not decode (%s) - no surface map\n",
						trn.charmap.c_str(), charmap_err.c_str());
			}
		}
	}
	// The same field the game builds (Simulation::set_terrain_height_field):
	// the engine's one owning cpt/trn(+charmap) builder (ADR 0042 d4), filled
	// into the kernel's own store.
	terrain::terrain_field_store_build(terrain_store, cpt, trn,
			charmap.empty() ? nullptr : charmap.indices.data(),
			charmap.width, charmap.height);
	return terrain_store.valid();
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
	kernel_options.game_type = game_type::for_mission_mode(bms::selected_game_mode(
			static_cast<bms::AttribFlags>(mission.header.attrib_flags)));
	if (listen_server)
		kernel_options.bringup_net_session = [this] { inmatch::listen_host::bringup(*this, host); };
	return mission::MissionKernel::boot(kernel_options, error);
}

void RetailMissionRig::tick() {
	if (listen_server) {
		NullDatagramSocket sock;
		inmatch::listen_host::frame(*this, host, sock, /*viewport_height=*/0, /*perf=*/nullptr);
		return;
	}
	tick_no_net();
}

void RetailMissionRig::tick(int count) {
	for (int i = 0; i < count; ++i) tick();
}

// --- inmatch::TickTarget ----------------------------------------------------

inmatch::TickOutcome RetailMissionRig::advance_mission_tick(const inmatch::TickInput &in) {
	input = in.player.movement;
	if (in.player.look_delta_x != 0.0f || in.player.look_delta_y != 0.0f)
		look(in.player.look_delta_x, in.player.look_delta_y);
	tick();
	inmatch::TickOutcome out;
	out.status = inmatch::TickStatus::Ran;
	out.logic_tick = static_cast<int32_t>(world.logic_tick);
	return out;
}

bool RetailMissionRig::reset_mission_to_baseline(inmatch::SessionError &error) {
	if (restore_baseline()) return true;
	error = {inmatch::SessionErrorCode::LoadFailed, "no baseline"};
	return false;
}

void RetailMissionRig::close_mission() {}

} // namespace opennova::testrig
