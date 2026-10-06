// Simulation — the map rotation (D-NET-331): the host screen's START seed over
// the mounted mission catalog, the host's round-end map change and the
// joiner's reload in place. The rotation, the router, the teardown's arms and
// the reload legs are the engine's (inmatch/mission_rotation.h,
// inmatch/map_change.h, JoinerConnection::begin_mission_reload); this TU only
// feeds them the session's catalog and keeps the session across the world's
// reload.
#include "simulation/simulation_internal.h"

#include <base/io/strutil.h>
#include <runtime/inmatch/joiner_role.h>
#include <runtime/inmatch/map_change.h>

#include "resource_index/resource_root.h"
#include "util/string_convert.h"

using namespace sim_internal;

namespace sim_internal {

// The host screen's START over the mounted catalog: each SELECTED_MISSIONS
// row names a catalog row (the first one whose file matches), its Switch cell
// that row's launch option. A START with no table (the NovaWorld panel, the
// --lan-host flag, a tool) is its one mission. A file the catalog does not
// list cannot be a table row: it is left out, and a session with no listed
// mission has no rotation (its first round end ends it). The seed itself is
// the engine's (inmatch::seed_rotation_from_host_screen carries the witness).
void seed_host_rotation(SimulationNetState &p_net, const Ref<ResourceRoot> &p_root) {
	namespace inmatch = opennova::inmatch;
	p_net.rotation.list = inmatch::MissionRotation{};
	p_net.rotation_catalog.clear();
	if (p_root.is_null()) return;
	p_net.rotation_catalog = opennova::mission_catalog::build(p_root->native_index());
	const std::vector<opennova::mission_catalog::Row> &catalog = p_net.rotation_catalog;
	std::vector<std::string> files = p_net.rotation_missions;
	std::vector<int32_t> switches = p_net.rotation_launch_options;
	if (files.empty() && !p_net.host_session_config.mission_file.empty()) {
		files.push_back(p_net.host_session_config.mission_file);
		switches.clear();
	}
	std::vector<int32_t> rows;
	std::vector<int32_t> launch_options(catalog.size(), 0);
	for (size_t i = 0; i < files.size(); ++i) {
		for (size_t row = 0; row < catalog.size(); ++row) {
			if (!opennova::strutil::iequals(catalog[row].file, files[i])) continue;
			rows.push_back(static_cast<int32_t>(row));
			launch_options[row] = i < switches.size() ? switches[i] : 0;
			break;
		}
	}
	if (rows.empty()) return;
	inmatch::seed_rotation_from_host_screen(p_net.rotation, catalog, rows, launch_options);
}

} // namespace sim_internal

namespace {

// The mission loop the round end left: a frame's stored exit failed the
// session; a caller from a running frame ends it the same way first.
void end_mission_loop(opennova::inmatch::Session &p_session) {
	using State = opennova::inmatch::State;
	if (p_session.state() == State::Running || p_session.state() == State::Paused)
		(void)p_session.fail({opennova::inmatch::SessionErrorCode::SessionLost, "map change"});
}

} // namespace

String Simulation::begin_host_map_change() {
	if (host_role_ == nullptr || !is_host_listening()) return String();
	end_mission_loop(session_);
	if (opennova::inmatch::begin_host_map_change(*host_role_, net_.rotation_catalog) ==
			opennova::inmatch::MapChangeStep::RotationEnded)
		return String();
	// The next boot continues this session (boot_mission reads the latch).
	net_.map_change_pending = true;
	return opennova::to_gd(net_.rotation.list.map_file);
}

bool Simulation::begin_joiner_reload() {
	if (joiner_role_ == nullptr || !joiner_role_->begin_mission_reload()) return false;
	end_mission_loop(session_);
	// The failed frame's session waits in the connect state the join preload
	// polls, which a load keeps (prepare_session_load) with its socket.
	return session_.begin_connect().applied();
}
