#include <runtime/inmatch/joiner_boot.h>

#include <runtime/inmatch/joiner_role.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/item_replication_catalog.h>
#include <runtime/world/present_rows.h> // PF_STRIDE, PF_TYPE_ID

#include <algorithm>
#include <cstddef>

namespace opennova::inmatch {

ClientRuntime &boot_joiner(JoinerRole &role, const JoinerBootRequest &request) {
	role.set_protocol(request.protocol);
	// The rules the joiner's World and its replica run, the mounted game's.
	if (request.rules != nullptr)
		role.set_rules(*request.rules);
	// The title whose fresh profile a kitless 0x2F sends (ClientRuntime::set_title).
	if (request.title != nullptr)
		role.set_title(*request.title);
	// The process's 0x4C report count runs on across joins (ClientProcessState).
	role.use_process_state(request.process);
	// A session destroy's stop of a held server writes the process's _inout.txt lines
	// (JoinerRole::destroy_session).
	if (request.server_logs != nullptr)
		role.set_server_logs(*request.server_logs);
	// The join goes on with the socket the embedder opened: a browse row's, the one the row was
	// found on (retail's client has one transport socket per network type and the join continues
	// on the browse's connection), else its dial's over the client port range.
	// [orig: CNapiNetwork_StartClientConnection @0x4CA160 - the enumerator's connection
	//  ctx+0xE60 @0x4CA1DD..0x4CA1EE]
	role.set_socket(request.socket, request.host);
	// The join socket's bound address, which a held server's HOST STOPPED prints.
	role.set_local_address(request.local_address);
	// The runtime, so the joiner phase reads Idle before the first load; the role retains the
	// request for a load that rebuilds it. The callsign and the passwords go out as the profile's
	// code-page bytes (create_runtime; D-NET-661, D-HUD-57).
	ClientRuntime &runtime = role.create_runtime(request.player_name, request.join_role,
			request.spectator_password, request.server_password, request.join_password);
	role.set_join_novaworld(request.settings.network_type == NetworkType::NovaWorld);
	if (request.enumerator_drain)
		role.set_enumerator_drain(request.enumerator_drain);
	install_joiner_runtime(role, request.settings, request.tables);
	// The kind-derived world rules (no authority, an mp session) applied with the role install;
	// the role's latches start the join fresh.
	role.reset_for_join();
	return runtime;
}

void install_joiner_runtime(
		JoinerRole &role, const JoinerSettings &settings, const JoinerTables &tables) {
	if (!role.runtime)
		return;
	ClientRuntime &runtime = *role.runtime;
	// The process's _connectlog.txt, which the session key's handler closes.
	runtime.set_connect_log(tables.connect_log);
	// The process-scoped charattr.def table retail loads before joining, and the World's
	// per-class words from it (install_class_attribute_flags).
	if (tables.charattr != nullptr) {
		runtime.set_charattr_table(*tables.charattr);
		if (mission::MissionKernel *kernel = role.kernel())
			install_class_attribute_flags(
					kernel->world, runtime.charattr_table(), *tables.charattr);
	}
	if (settings.character_vars)
		runtime.set_character_join_vars(*settings.character_vars);
	// The record's +1660, raw (JoinerConnection::set_auto_medic_preference).
	if (tables.auto_medic_off)
		runtime.set_auto_medic_preference(*tables.auto_medic_off);
	runtime.set_expansion_version_root(settings.expansion_version_root);
	runtime.set_app_id(settings.app_id);
	runtime.set_join_cd_cookie(settings.cd_cookie);
	runtime.set_cd_key_ids(settings.cd_key_ids);
	runtime.set_glsvss_request(settings.glsvss_request);
	runtime.set_sync_tail_loading(settings.sync_tail_loading);
	runtime.set_mission_data_loading(settings.mission_data_loading);
	runtime.set_discovered_session(settings.discovered_session); // the browse row's 0x81
	if (tables.item_catalog)
		runtime.view().set_item_catalog(tables.item_catalog);
	// A mounted table installs even when empty: its fallback packs 0, as retail's empty table
	// does (ClientReplicaPipeline::resolve_player_net_id carries the witness).
	runtime.view().set_character_registry(tables.character_registry);
}

std::vector<int32_t> streamed_type_order(const std::vector<float> &rows) {
	std::vector<int32_t> order;
	const std::size_t stride = static_cast<std::size_t>(world::PF_STRIDE);
	for (std::size_t row = 0; row + stride <= rows.size(); row += stride) {
		const int32_t type_id = static_cast<int32_t>(rows[row + world::PF_TYPE_ID]);
		if (type_id == 0 || std::find(order.begin(), order.end(), type_id) != order.end())
			continue;
		order.push_back(type_id);
	}
	return order;
}

void settle_streamed_types(JoinerRole &role, mission::MissionKernel &kernel,
		const std::vector<int32_t> &type_order, const StreamedTypeLegs &legs) {
	if (legs.seat_specs)
		legs.seat_specs(type_order);
	if (legs.item_traits)
		legs.item_traits();
	// The rows' class init slot leg, now that their weapon names and models are in.
	role.run_streamed_weapon_slot_class_inits();
	if (legs.collision)
		legs.collision();
	kernel.occlusion_init_mission();
}

} // namespace opennova::inmatch
