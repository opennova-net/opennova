// Simulation — the net roles (ADR 0009/0011/0012): per-load listen-host
// bring-up + host pump, the LAN joiner pump family + wire proxies/events, the
// host session config FFI, and the joiner preload/session API.
#include "simulation/simulation_internal.h"
#include "network/join_screen_status.h"
#include "network/connection_error.h"
#include <runtime/inmatch/host_config.h> // run_host_server_command
#include <runtime/inmatch/host_settings.h>
#include <runtime/inmatch/mission_exit.h> // kExitMissionSoundset (input action 3's tone)
#include <runtime/inmatch/net_debug_report.h> // the F3 Net window + the joiner diagnostics
#include <runtime/inmatch/loaded_model_list.h> // the C2S 0x3D page (D-NET-527)
#include <runtime/mission/runtime_boot.h>      // boot_files_from_index
#include "network/udp_pump_datagram_socket.h"
#include "simulation/hud_view_records.h"
#include "simulation/deploy_rows.h" // the DEATH screen's zone / list rows
#include "simulation/joiner_net_trace_row.h" // the joiner net readout's rows
#include "simulation/model_list_node_row.h" // the model-list readout's rows
#include "object/character_join_profile.h" // the two-side character selection record
#include "network/host_session_options.h" // the hosted-session request record
#include "network/join_target.h" // the join request the discovered session comes from
#include "network/lan_session.h" // drain_process_enumerator (the session reset's drain)
#include "util/string_convert.h"
#include "resource_index/resource_root.h" // session_title (the launch game a bare session speaks)
#include <net/title/title_identity.h> // TitleIdentity::game_id (the session's rules title)

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <runtime/inmatch/server_admin_command.h> // Server_ExecuteServerCommand (the NovaWorld ServerCommand verbs)
#include <runtime/inmatch/server_ban_lists.h> // Server_HandleBanPuntCommand (the console's authority arm)
#include <runtime/inmatch/server_console.h> // Server_HandleAdminNetlogCommand, entity_usage_text (SAVENFO)
#include <runtime/controls/binding_set.h> // keylist_text (SAVEKEYS)
#include <runtime/inmatch/server_info.h> // Server_ToggleJoinLock (the lockgame action)
#include <runtime/inmatch/server_spawn.h> // Server_SetPlayerSpectator
#include <runtime/inmatch/session_status.h>
#include <formats/threedi/threedi_panm_pose.h> // the native PANM liveness gate (S3, ADR 0028)
#include <net/npwire/ingame_decode.h> // kRoundEventFlag* (the fire-mode byte)
#include <runtime/hud/feed_format.h> // the witnessed feed line/color policy
#include <runtime/replication/client_scoreboard_view.h> // the Tab board's draw-time projection
#include <runtime/hud/hud_frame.h> // HudScoreboardState (the Tab board feed)
#include <runtime/inmatch/role_feeds.h> // scoreboard_feed
#include <runtime/world/wire_body_sound.h> // the wire-fed remote body's footstep/foley consume
#include <net/npwire/net_ports.h> // lan_host_bind_ports (the D-NET-210 bind scan)
#include <net/novaworld/lobby_vars.h> // resolve_address_without_dns (the join's row address)
#include <base/vfs/vfs.h> // vfs_expansion_version_checksum (the D-NET-166 JOIN CRC)
#include <net/npwire/wire_handle.h>  // pool()/kPoolItem/kInvalid (the wire handle home)
#include <runtime/world/angle.h>         // placed_heading_bam / mission_yaw_deg_from_bam_heading
#include <runtime/world/infantry.h>      // kAnimStanceFlag* (the witnessed stance bits)
#include <runtime/world/spawn_select.h>  // kDeployPickNone/AutoTeam (C2S 0x2C sentinels)
#include <runtime/world/vehicle_motor.h> // carrier_pose_fixed (the deck-ride pose reader)
#include <runtime/world/destruction.h>  // destruction_notify_item_damage (S2C 0x13 net kill)
#include <runtime/world/entity_spawn.h> // entity_reset_to_spawn_state (redeploy release)
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace sim_internal;


// The host's session record (the faithful §5.0 mode-3 in-process listen
// server (engine: runtime/inmatch/host_role.cpp)): the UI host config for a
// LAN host, the SP listen server's own otherwise. The host's own player
// AUTO-spawns through the real pipeline (Server_ProcessPendingPlayerSpawns ->
// resolve_player_spawn_pose marker chain), and its own loopback client renders
// the per-frame 0x0A. Standalone SP is ALWAYS serve-and-play (it renders the
// host's own player); a LAN host honors the UI server type
// (net_.host_serve_and_play, from configure_host_session). A dedicated host
// (serve_and_play=false) skips the own-player spawn + the local view and lets
// host_session_pump discard the host loopback (step 5), mirroring
// start_host_session's gating. The host ctx's mission points straight at the
// kernel's adopted document, which outlives the match; a reload's bring-up
// rebuilds the loopback and a fresh host owner drops stale owner peers.
namespace {

// The title a session speaks, the listen host's (HostConfig::title) and the joiner's
// (JoinerRole::set_title): the one the shell named (Simulation::set_title, the mounted
// game's), else, for a session no shell named one for, the game the process was launched
// as (ResourceRoot::session_title with no root). Null when that title's identity is not
// witnessed in full; a joiner then does not dial and a host admits no joiner.
const opennova::TitleIdentity *session_title(const opennova::TitleIdentity *named) {
	if (named != nullptr) return named;
	std::string error;
	return ResourceRoot::session_title(Ref<ResourceRoot>(), error);
}

} // namespace

opennova::inmatch::HostConfig Simulation::host_session_cfg(uint32_t p_game_type,
		const std::string &p_sp_mission_file) {
	namespace inmatch = opennova::inmatch;
	// The record is the engine's (inmatch/host_config.h shell_host_config); the
	// binding passes the session's values.
	inmatch::ShellHostSource source;
	if (is_host_listening()) {
		source.lan_config = &net_.host_session_config; // the host screen's (configure_host_session)
		source.lan_max_players = net_.host_max_players;
		source.lan_serve_and_play = net_.host_serve_and_play;
		source.lan_network_type = net_.host_network_type;
	}
	source.sp_game_type = p_game_type;
	source.sp_mission_file = p_sp_mission_file;
	source.profile = player_.profile_record_set ? &player_.profile_record : nullptr;
	source.cfg_block = net_.game_config.is_valid() ? &net_.game_config->native().cfg() : nullptr;
	inmatch::HostConfig cfg = inmatch::shell_host_config(source);
	cfg.title = session_title(net_.title);
	// The process's log devices: the switches retail's command line arms reach every host the
	// process runs, a listen host's as a dedicated one's (base/resource_index boot_policy.h
	// LogSwitches carries the witness).
	if (net_.log_devices != nullptr) cfg.logs = net_.log_devices->logs();
	// The listen socket's bind address and bound port, which HOST STARTED / STOPPED print
	// (the NovaWorld session's when the host rides it); a socketless host prints the
	// placeholder (engine: inmatch::inout_host_line, JoinerRole::set_local_address).
	if (is_host_listening() && net_.pump.is_valid() && net_.pump->is_open()) {
		cfg.local_address = net_.pump->local_address();
		cfg.local_address_known = true;
	}
	// The mounted Avatars.def's combos the player add validates against (D-NET-137).
	cfg.characters = assets_.character_registry;
	// banned.txt and banlist.txt by bare name in the same working directory, as retail opens
	// them; every host's round init loads them (the witness: engine server_ban_lists.h).
	if (net_.log_devices != nullptr) cfg.ban_directory = net_.log_devices->files().dir();
	// The process clock the send-interval legs and the receive reap measure (GetTickCount's):
	// the mission load runs no tick, its send pumps still fire a due leg (D-NET-256).
	cfg.clock_ms = inmatch::host_monotonic_milliseconds;
	// The process's 0x4C report count runs on across sessions (ClientProcessState).
	cfg.client_process = net_.client_process;
	// The loose _NSTMOUT.TXT reap/pool override under the install root
	// (engine session_timeout_config.h; the CNapiNetwork_Init read). Empty
	// keeps the 120000 ms / 1200-record template.
	cfg.game_root = host_game_root_;
	if (net_.local_character_vars_set) cfg.local_character_vars = net_.local_character_vars;
	return cfg;
}

// The in-memory loads' bring-up record (load_from_mission_data, the demo):
// the session record over the adopted document's game type, the retained .til
// (S2C 0x45 source, empty => skipped, §5.37) and mission text, and the
// mounted gametext's Server strings. The file boot's phase A stages its own.
opennova::inmatch::HostBringup Simulation::host_bringup() {
	namespace inmatch = opennova::inmatch;
	inmatch::HostBringup bringup;
	bringup.host_cfg = host_session_cfg(
			opennova::game_type::for_mission_attribs(kernel_->mission.header.attrib_flags));
	bringup.terrain_til_data = net_.terrain_til_data;
	bringup.mission_text = net_.mission_text;
	if (assets_.root.is_valid())
		bringup.server_text = inmatch::read_host_server_text(
				opennova::mission::boot_files_from_index(assets_.root->native_index()),
				net_.mission_text.override_table.get());
	return bringup;
}


// P7/A5: the per-frame host owner loop is now a THIN delegation to the shared core host_session_pump
// (engine/runtime/inmatch). Simulation supplies the socket (a UdpPump adapter; SP passes a null pump and the
// loop's socket legs go inert) and folds the host's own loopback 0x0A into ClientState for the present
// pass (serve_and_play: host_session_pump skips the loopback discard so we can read it here).
// The host frame's dispatch_event + admit_peer were promoted into engine/runtime/inmatch (inmatch::dispatch_event /
// inmatch::admit_peer over host_owner, driven by host_session_pump).

Array Simulation::get_streamed_placement_records() const {
	Array out;
	if (joiner_role_ == nullptr || !kernel_) return out;
	// The placed identity and the record-space fields read straight off the
	// registry rows the materializer stamped (a BMS entity record's shape, the
	// same batched/placed path the host's own statics take).
	for (const opennova::world::Entity *row :
			joiner_role_->materializer().placed_rows(kernel_->world)) {
		Dictionary d;
		d["kind"] = opennova::world::spawn_origin_kind(row->spawn_origin);
		d["index"] = opennova::world::spawn_origin_index(row->spawn_origin);
		d["bms_id"] = row->bms_id;
		// The placer resolves graphics by items.def id: the wire type plus the
		// catalog offset (pools 1..3 never carry the runtime player type).
		d["item_id"] = opennova::mission::visual_item_id_for_runtime_type(row->item_id);
		d["position"] = Vector3(row->position.x, row->position.y, row->position.z);
		d["rotation_deg"] = Vector3(
				static_cast<float>(opennova::world::degrees_from_angle_bam(row->pitch_bam)),
				static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(row->heading)),
				static_cast<float>(opennova::world::degrees_from_angle_bam(row->roll_bam)));
		d["team"] = row->team;
		d["group"] = row->group_id;
		d["ai_flags"] = static_cast<int64_t>(
				opennova::world::bms_attributes_from_entity_flags(row->engine_flags));
		out.push_back(d);
	}
	return out;
}

PackedInt32Array Simulation::take_retired_placement_ids() {
	PackedInt32Array out;
	if (joiner_role_ == nullptr) return out;
	for (const int32_t id : joiner_role_->materializer().take_retired_placement_ids())
		out.push_back(id);
	return out;
}

// The ~1 Hz frozen-session diagnostic print (the bridge computes the sampled
// state; this shell leg owns the env-gated print_verbose channel). L's LIVE
// pose is the exact source build_player_uplink transmits: a pose that never
// changes while the player is moving on screen means the local motor is not
// driving L (so the host renders us frozen at spawn and eventually stops
// streaming records around a stale reference position).
void Simulation::print_joiner_net_diagnostic_sample() {
	if (!is_joiner_network_diagnostics_enabled() || !runtime_ || joiner_role_ == nullptr) return;
	String local_state = " L=none";
	if (joiner_role_->local_spawned() && kernel_) {
		const opennova::world::AiEntity *lae =
				kernel_->world.ai.for_handle(kernel_->world.cached.local_player);
		const opennova::world::Entity *le =
				kernel_->world.registry.get(kernel_->world.cached.local_player);
		if (lae != nullptr)
			local_state = vformat(
					" L=(%.1f,%.1f,%.1f) hdg=%d input=0x%02x",
					lae->self->position.x.q16 / 65536.0f,
					lae->self->position.y.q16 / 65536.0f,
					lae->self->position.z.q16 / 65536.0f,
					static_cast<int64_t>(lae->heading >> 16),
					static_cast<int64_t>(le ? le->net_move_input : 0));
	}
	UtilityFunctions::print_verbose(vformat(
			"joiner net: in=%d out=%d rec=%d gap=%d retained=%d match=%d deployed=%d%s%s",
			static_cast<int64_t>(runtime_->inbound_frontier_seq()),
			static_cast<int64_t>(runtime_->outbound_seq()),
			static_cast<int64_t>(runtime_->state().compact_records_applied),
			static_cast<int64_t>(runtime_->inbound_gap_depth()),
			static_cast<int64_t>(runtime_->retained_outbound_depth()),
			runtime_->in_match() ? 1 : 0,
			runtime_->is_deployed() ? 1 : 0, local_state,
			joiner_role_->freeze_suspected()
					? vformat(" *** REPLICATION FROZEN %ds ***",
							  static_cast<int64_t>(joiner_role_->flat_seconds()))
					: String()));
}

void Simulation::enable_listen_server(bool p_enable) {
	listen_server_ = p_enable;
	// P7: the SP listen server rides the inmatch in-match runtime on the host role (its
	// ListenHostState), stood up per-load by the role's bring-up — there is no net ISystem and
	// no legacy loopback seam here. The role installs now when the session can switch, else at
	// the next load (a sim is SP listen XOR LAN host XOR joiner).
	ensure_session_role();
}

void Simulation::set_terrain_til_data(const PackedByteArray &p_til_bytes) {
	net_.terrain_til_data.assign(p_til_bytes.ptr(), p_til_bytes.ptr() + p_til_bytes.size());
	// The same bytes feed the terrain store's placed-tile overlay (D-SND-15;
	// the fold and its witness live engine-side, terrain_field_build.h).
	kernel_->set_placed_tiles(net_.terrain_til_data);
	apply_terrain_to_ai();
}

bool Simulation::enable_host_listen(int p_port) {
	using RoleKind = opennova::inmatch::RoleKind;
	listen_server_ = true;
	// The LAN host role, kinded by the UI server type (configure_host_session).
	// A live mission cannot switch roles: the pump is still bound now and the
	// role is installed at the next load (net_.lan_host_pending, consumed by
	// ensure_session_role) -- the contract the armory's MP open rides.
	const bool installed = install_role(std::make_unique<opennova::inmatch::HostRole>(
			net_.host_serve_and_play ? RoleKind::ListenHost : RoleKind::DedicatedHost,
			assets_.item_replication_catalog));
	net_.lan_host_pending = !installed;
	if (net_.pump.is_null()) net_.pump.instantiate();
	net_.pump->set_capture_path(net_.capture_pcap_path);
	// The LAN host scans from the requested port (the request's first port is
	// the cfg block's mplanserverportmin) through the block's
	// mplanserverportmax by its mplanserverportdelta, random 0 (D-NET-210; the
	// witness is engine inmatch::transport_bind_ports). Port 0 keeps the
	// OS-assigned bind — the dev/test seam retail has no analog for.
	// Each bind is on the block's mpipaddressstring (engine: inmatch::transport_bind_address).
	const uint32_t address = net_.game_config.is_valid()
			? opennova::inmatch::transport_bind_address(net_.game_config->native().cfg())
			: 0u;
	bool bound = false;
	if (p_port <= 0) {
		bound = net_.pump->bind_listen_at(0, address) == 0;
	} else {
		const std::vector<uint16_t> ports = net_.game_config.is_valid()
				? opennova::inmatch::transport_bind_ports(net_.game_config->native().cfg(),
						  opennova::inmatch::NetworkType::Lan, /*authority=*/true,
						  static_cast<uint32_t>(p_port))
				: opennova::lan_host_bind_ports(static_cast<uint32_t>(p_port));
		for (const uint16_t port : ports) {
			if (net_.pump->bind_listen_at(port, address) == 0) {
				bound = true;
				break;
			}
		}
	}
	// The socket open's connect-log lines, the setup over the bound socket or the failure over the
	// block's quad (engine: inmatch::write_transport_socket_open_lines).
	if (net_.log_devices != nullptr) {
		const opennova::PeerAddr local = net_.pump->local_address();
		const opennova::gamecfg::GameCfg &block = net_.game_config.is_valid()
				? net_.game_config->native().cfg()
				: opennova::gamecfg::defaults();
		opennova::inmatch::write_transport_socket_open_lines(net_.log_devices->connect(),
				bound ? &local : nullptr, address,
				opennova::inmatch::transport_port_quad(block, opennova::inmatch::NetworkType::Lan,
						/*authority=*/true,
						p_port > 0 ? std::optional<uint32_t>(static_cast<uint32_t>(p_port))
								   : std::nullopt));
	}
	if (!bound) {
		// Not a LAN host: the socketless SP listen server is what remains.
		net_.pump_socket.reset();
		net_.lan_host_pending = false;
		if (installed) (void)install_offline_role();
		return false;
	}
	// The host role reads and writes the bound pump through the adapter from
	// now on (the SP/test host never installs one and stays socketless). The
	// kind-derived world rules (the authority, the mp session) applied with the
	// role install; a pending role takes the socket when it is installed.
	net_.pump_socket = std::make_unique<opennova::CountingDatagramSocket>(
			std::make_unique<UdpPumpDatagramSocket>(net_.pump.ptr()));
	if (installed) host_role_->set_socket(net_.pump_socket.get());
	// P7: the LAN host rides the inmatch runtime (ctx over a real UDP socket), stood up per-load in
	// bringup_host_runtime with SocketMode::Lan. UdpPump owns the socket; all protocol/crypto/
	// framing stays in libs (ADR 0010). net_.host_session_config keeps the GDScript-facing session options
	// (the Dictionary getter + the §5.1 reactive-reply config consumed by create_session).
	// The port actually bound (the scan may have stepped past the requested
	// one) is what the session advertises.
	net_.host_bind_port = static_cast<uint16_t>(net_.pump->local_port());
	return true;
}

// A NovaWorld host's match rides the NovaWorld session's socket: the pump the
// session bound from the mpnovaworld range, read through its demux's game view
// (UdpPumpGameSocket), so the service's observed NWU source is the endpoint its
// joiners reach (D-NET-346; engine: net/npwire/datagram_demux.h). Nothing binds
// here; the role and the counter are enable_host_listen's.
bool Simulation::enable_host_listen_on(const Ref<UdpPump> &p_pump) {
	using RoleKind = opennova::inmatch::RoleKind;
	if (p_pump.is_null() || !p_pump->is_open()) return false;
	listen_server_ = true;
	const bool installed = install_role(std::make_unique<opennova::inmatch::HostRole>(
			net_.host_serve_and_play ? RoleKind::ListenHost : RoleKind::DedicatedHost,
			assets_.item_replication_catalog));
	net_.lan_host_pending = !installed;
	net_.pump = p_pump;
	net_.pump_socket = std::make_unique<opennova::CountingDatagramSocket>(
			std::make_unique<UdpPumpGameSocket>(p_pump));
	if (installed) host_role_->set_socket(net_.pump_socket.get());
	net_.host_bind_port = static_cast<uint16_t>(p_pump->local_port());
	return true;
}

bool Simulation::is_host_listening() const {
	const opennova::inmatch::RoleKind kind = session_.kind();
	return kind == opennova::inmatch::RoleKind::ListenHost ||
			kind == opennova::inmatch::RoleKind::DedicatedHost ||
			net_.lan_host_pending;
}

int Simulation::get_host_listen_port() const {
	return (is_host_listening() && net_.pump.is_valid()) ? net_.pump->local_port() : 0;
}

int Simulation::get_host_peer_count() const {
	// Count the type-1 (remote-joiner) connections in the inmatch table. The host's own type-2
	// loopback is excluded; a pre-Hello garbage datagram registers no node (handle_server_datagram
	// drops bad envelopes), so it stays 0 until a real JointOperations peer handshakes.
	int n = 0;
	const opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx == nullptr) return 0;
	for (const opennova::inmatch::NapiNPConnection &c : ctx->np_protocol.connection_list) {
		if (c.type == 1) ++n;
	}
	return n;
}

void Simulation::set_novaworld_registration(const String &p_gsid, int p_app_id,
		const String &p_login_pcid) {
	if (opennova::inmatch::NapiNPServerCtx *ctx = host_ctx()) {
		ctx->novaworld_gsid = opennova::to_std(p_gsid);
		ctx->novaworld_app_id = static_cast<uint32_t>(p_app_id);
		opennova::inmatch::set_novaworld_account_facts(*ctx, opennova::to_std(p_login_pcid));
	}
}

// The NovaWorld link both roles read: the network type is the role's own (the
// authority's transport_mode, the joiner's join record), the NWU session facts
// the shell's NovaWorld session node's. The authority's 62-frame block reads
// them off its context, the joiner's off its client runtime; either one's NWU
// exit store (a stop-playing or a punt) lands beside them.
void Simulation::set_nwu_session(bool p_in_use, uint32_t p_flags, int32_t p_role,
		int32_t p_exit_reason) {
	opennova::hud::NovaWorldLinkFacts facts;
	facts.nwu_in_use = p_in_use;
	facts.nwu_session_flags = static_cast<uint8_t>(p_flags & 0xFFu);
	facts.nwu_session_role = p_role;
	opennova::inmatch::NapiNPServerCtx *ctx = is_joiner() ? nullptr : host_ctx();
	if (ctx != nullptr) {
		facts.novaworld = ctx->transport_mode == opennova::inmatch::NetworkType::NovaWorld;
		ctx->nwu_in_use = p_in_use;
		ctx->nwu_session_role = p_role;
		ctx->nwu_session_flags = facts.nwu_session_flags; // a Serve Only host's N icon (D-NET-NEW-L8-24)
		if (p_exit_reason != 0) ctx->mission_exit_reason = p_exit_reason;
	} else {
		facts.novaworld = net_.join_network_type == opennova::inmatch::NetworkType::NovaWorld;
		if (runtime_ != nullptr && p_exit_reason != 0) runtime_->set_mission_exit_reason(p_exit_reason);
	}
	if (runtime_ != nullptr) runtime_->set_novaworld_link(facts);
}

std::vector<opennova::inmatch::HostPlayerListRow> Simulation::host_player_list_rows() const {
	const opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx == nullptr) return {};
	return opennova::inmatch::host_player_list_rows(*ctx);
}

Simulation::ServerCommandResult Simulation::execute_server_command(const String &p_verb,
		const String &p_target, const PackedStringArray &p_args) {
	ServerCommandResult out;
	opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	std::vector<std::string> args;
	args.reserve(static_cast<size_t>(p_args.size()));
	for (int i = 0; i < p_args.size(); ++i) args.push_back(opennova::to_std(p_args[i]));
	// No host context yet: the session-less one, in the listen host's own mode
	// (engine: inmatch/host_config.h run_host_server_command).
	const opennova::inmatch::ServerCommandOutcome outcome =
			opennova::inmatch::run_host_server_command(ctx,
					kernel_ != nullptr ? &kernel_->world : nullptr,
					opennova::inmatch::host_connection_mode(net_.host_serve_and_play),
					net_.game_config.is_valid() ? &net_.game_config->native().cfg() : nullptr,
					opennova::to_std(p_verb), opennova::to_std(p_target), args);
	out.handled = outcome.handled;
	out.stop_hosting = outcome.stop_hosting;
	out.config_changed = outcome.config_changed;
	if (ctx != nullptr && outcome.config_changed) {
		// The sim's host session record is what the next session build reads; the NovaWorld
		// listing reads the live context's (inmatch::read_session_listing_columns).
		net_.host_session_config.server_name = ctx->config.server_name;
		net_.host_session_config.custom_text = ctx->config.custom_text;
		net_.host_session_config.multiplayer_reset = ctx->config.multiplayer_reset;
	}
	return out;
}

opennova::inmatch::AdminConsole::Seams Simulation::remote_admin_seams(
		opennova::gamecfg::GameCfg *p_block, std::function<void()> p_save_config) {
	namespace inmatch = opennova::inmatch;
	inmatch::AdminConsole::Seams seams;
	seams.config_block = p_block;
	seams.save_config = std::move(p_save_config);
	if (net_.process_rotation != nullptr) {
		if (net_.rotation_admin == nullptr)
			net_.rotation_admin = std::make_unique<inmatch::HostRotationAdmin>(
					*net_.process_rotation, net_.rotation_catalog);
		seams.rotation = net_.rotation_admin.get();
	}
	seams.characters = &assets_.character_registry;
	seams.items_table = [this]() -> const opennova::def::DefItemsFile * {
		return kernel_ ? kernel_->items_table() : nullptr;
	};
	// The local command's WAC overlay toggle, the authority's (D-NET-371).
	seams.toggle_overlay = [this](const std::string &p_line) {
		return kernel_ && kernel_->wac.toggle_cheat(p_line, !is_joiner(), kernel_->world);
	};
	// The status report's gametext is the process's: GameWorld::remote_admin_seams sets it for
	// every console (engine inmatch/remote_admin.h process_game_text).
	const opennova::TitleIdentity *title = session_title(net_.title);
	if (title != nullptr) {
		const opennova::inmatch::ConnectLogHeader header = opennova::inmatch::connect_log_header(title);
		seams.version_text = header.version;
		seams.title_text = header.title;
	}
	return seams;
}

void Simulation::install_console_seams() {
	namespace inmatch = opennova::inmatch;
	if (runtime_ == nullptr) return;
	inmatch::ClientLocalCommandSeams seams;
	const opennova::TitleIdentity *title = session_title(net_.title);
	if (title != nullptr) {
		const inmatch::ConnectLogHeader header = inmatch::connect_log_header(title);
		seams.version_text = header.version;
		seams.title_text = header.title;
	}
	// SAVENFO: debug.nfo in text mode, each '\n' a CR LF on the disk (engine
	// inmatch/client_console.h ClientLocalCommandSeams::save_nfo carries the witness).
	seams.save_nfo = [this]() {
		if (!kernel_ || net_.log_devices == nullptr) return;
		const char *name = "debug.nfo";
		const std::string file = inmatch::entity_usage_file_name(name);
		if (file.empty()) return;
		std::string text;
		for (const char c : inmatch::entity_usage_text(kernel_->world, kernel_->items_table(), name)) {
			if (c == '\n') text += '\r';
			text += c;
		}
		(void)net_.log_devices->files().write(file, text, /*append=*/false);
	};
	// SAVEKEYS: keylist.key over the live binding table, written whole (engine
	// inmatch/client_console.h ClientLocalCommandSeams::save_keys carries the witness).
	seams.save_keys = [this]() {
		if (controls_model_.is_null() || net_.log_devices == nullptr) return;
		(void)net_.log_devices->files().write("keylist.key",
				opennova::controls::keylist_text(controls_model_->native_bindings()),
				/*append=*/false);
	};
	// The WAC overlay toggle, the authority's alone: a joiner's runtime never flips one
	// (engine wac/wac_system.h WacSystem::toggle_cheat carries the witness; D-NET-371).
	seams.toggle_overlay = [this](const std::string &p_line) {
		return kernel_ && kernel_->wac.toggle_cheat(p_line, !is_joiner(), kernel_->world);
	};
	runtime_->set_local_command_seams(std::move(seams));
	// The console's queued input actions whose arms are the game's, at their input frame
	// (the role keeps the Respawn row's 55, the medic call's 217 and the stance selects):
	// the exit, the scope, the deploy pick, the punt by slot and the seat selects, the
	// scope's and the seats' behind their rows' dispatcher head (engine
	// inmatch/client_runtime.h set_console_action_sink carries the witness).
	runtime_->set_console_action_sink([this](int32_t p_code, int32_t p_param, int32_t, int32_t) {
		switch (p_code) {
		case 3:
			// The action's Exit Mission tone, then its exit: the tone rides the world's
			// interface sound channel, which the session leg's terminal frame still plays
			// (GameWorld::leg_session; engine inmatch/mission_exit.h kExitMissionSoundset
			// carries the witness).
			if (kernel_) {
				opennova::world::ScriptSoundEvent tone;
				tone.kind = opennova::world::ScriptSoundEvent::Kind::Interface;
				tone.name = opennova::inmatch::kExitMissionSoundset;
				kernel_->world.out.script_sounds.push_back(std::move(tone));
			}
			input_action_exit();
			break;
		case 6:
			if (active_role().action_head_admits(p_code))
				(void)request_local_player_scope_toggle();
			break;
		case 12:
			(void)send_deployment_pick(p_param);
			break;
		case 34:
			if (inmatch::NapiNPServerCtx *ctx = host_ctx())
				inmatch::validated_punt(*ctx, p_param, 33, "ComIdPunt");
			break;
		default:
			if (p_code >= 182 && p_code <= 191 && active_role().action_head_admits(p_code))
				(void)local_player_select_seat(p_code - 182);
			break;
		}
	});
	// The authority's arms, NETLOG then BAN / PUNT with no sender (engine
	// inmatch/client_runtime.h set_authority_console carries the witness).
	runtime_->set_authority_console([this](const std::string &p_line) {
		inmatch::NapiNPServerCtx *ctx = host_ctx();
		if (ctx == nullptr) return false;
		std::vector<opennova::ProtocolMessage> no_slot_replies;
		if (inmatch::Server_HandleAdminNetlogCommand(*ctx, p_line, nullptr, no_slot_replies))
			return true;
		return inmatch::Server_HandleBanPuntCommand(*ctx, p_line);
	});
}

opennova::inmatch::RemoteAdminBinding Simulation::remote_admin_binding() {
	opennova::inmatch::RemoteAdminBinding binding;
	binding.host = host_ctx();
	binding.client = is_joiner() ? runtime_ : host_client_runtime();
	binding.joiner = is_joiner() ? joiner_role_ : nullptr;
	binding.world = kernel_ ? &kernel_->world : nullptr;
	binding.text_override = net_.mission_text.override_table.get();
	binding.session = static_cast<uint64_t>(get_instance_id());
	return binding;
}

opennova::io::CrtRand *Simulation::process_crt_rand() {
	return kernel_ ? &kernel_->world.crt_rand : nullptr;
}

opennova::inmatch::ClientRuntime *Simulation::host_client_runtime() {
	return host_role_ != nullptr ? host_role_->state.client_runtime.get() : nullptr;
}

void Simulation::set_novaworld_join_tickets(bool p_armed, PlayerEnterRequestHook p_hook) {
	opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx == nullptr) return;
	ctx->novaworld_join_tickets_armed = p_armed;
	if (!p_hook) {
		ctx->on_player_enter_request = nullptr;
		return;
	}
	ctx->on_player_enter_request =
			[hook = std::move(p_hook)](
					const opennova::inmatch::NapiNPServerCtx::PlayerEnterRequest &p_request) {
				hook(p_request.connection_id, p_request.peer.ip, p_request.peer.port,
						opennova::to_gd(p_request.join_ticket));
			};
}

bool Simulation::apply_player_enter_result(uint32_t p_connection_id, bool p_success,
		int32_t p_msg_code) {
	opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx == nullptr) return false;
	return opennova::inmatch::Server_ApplyPlayerEnterResult(*ctx, p_connection_id, p_success,
			p_msg_code, host_role_ != nullptr ? host_role_->state.host_owner.now_tick : 0u);
}

// The lockgame action at the authority (HudToggles' EVENT_JOIN_LOCK_TOGGLED, in a session on the
// authority): the join lock flips and a hosting NovaWorld session's server-info update runs at once
// through the context's hook (SessionDrive::bind_nw_host).
int Simulation::toggle_join_lock() {
	opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx == nullptr)
		return -1;
	return opennova::inmatch::Server_ToggleJoinLock(*ctx) ? 1 : 0;
}

void Simulation::configure_host_session(const Ref<HostSessionOptions> &p_options) {
	if (p_options.is_null()) return;
	// The WHOLE request config: every GameConfig field a host screen, the host
	// file or a tool sets lands as given (identity, rules, the hit feedback,
	// the vote and autobalance policies, the ping checks, ...). Only the
	// sim-owned mission header blob survives from the live config; the score
	// tables come from score.ini at each boot (inmatch/host_boot.h).
	opennova::inmatch::GameConfig config = p_options->config();
	config.mission_header_blob = std::move(net_.host_session_config.mission_header_blob);
	// The rules the session runs are the title's it hosts as (session_title); none, Jointops.exe's.
	const opennova::TitleIdentity *title = session_title(net_.title);
	config.title = title != nullptr ? title->game_id : opennova::gameprofile::GAME_JO;
	net_.host_bind_port = static_cast<uint16_t>(std::clamp(p_options->get_bind_port(), 0, 0xFFFF));
	net_.host_network_type = p_options->network_type();
	config.spectator_slots = std::max(config.spectator_slots, -1);
	// D-NET-166: the host's g_ExpansionChecksum analog. When the caller names
	// its install root, compute the CRC of the loose
	// expansion/<name>/version.txt so the join gate can run retail's compare
	// (the witnessed producer/gate live in vfs_expansion_version_checksum and
	// validates_join_request).
	if (!p_options->get_game_root().is_empty()) {
		config.expansion_version_checksum =
				opennova::vfs_expansion_version_checksum(
						opennova::to_std(p_options->get_game_root()),
						config.expansion);
	}
	// The same install root feeds the host bring-up's loose _NSTMOUT.TXT
	// session-timeout override (HostConfig::game_root).
	host_game_root_ = opennova::to_std(p_options->get_game_root());
	// The host screen's rotation: the rows and their Switch cells, which the
	// first host boot seeds the list from (seed_host_rotation).
	net_.rotation_missions.clear();
	net_.rotation_launch_options.clear();
	const PackedStringArray rotation_missions = p_options->get_rotation_missions();
	const PackedInt32Array rotation_options = p_options->get_rotation_launch_options();
	for (int64_t i = 0; i < rotation_missions.size(); ++i) {
		net_.rotation_missions.push_back(opennova::to_std(rotation_missions[i]));
		net_.rotation_launch_options.push_back(i < rotation_options.size() ? rotation_options[i] : 0);
	}
	// Server type + player cap (UI host config): serve_and_play gates the host's own-player spawn +
	// loopback fold at bring-up; max_players is the session's advertised and admitted cap: the
	// session create's 1..65 clamp, the title's (this host is in session), stored back into the
	// process block as the create stores it (engine: inmatch::store_session_player_cap). The
	// dedicated word makes the engine's slot limit (GameConfig::player_slot_limit) carry the host's
	// own slot on top.
	net_.host_serve_and_play = p_options->get_serve_and_play();
	net_.host_max_players = static_cast<uint32_t>(opennova::inmatch::store_session_player_cap(
			net_.game_config.is_valid() ? &net_.game_config->native().cfg() : nullptr,
			p_options->get_max_players(), config.title));
	config.max_players = net_.host_max_players;
	config.dedicated_server = !net_.host_serve_and_play;
	net_.host_session_config = std::move(config);
	if (kernel_ && is_host_listening()) {
		kernel_->world.rules.fat_bullets = net_.host_session_config.fat_bullets != 0;
		kernel_->world.rules.one_shot_kill = net_.host_session_config.one_shot_kill != 0;
		kernel_->world.rules.vehicle_respawns = net_.host_session_config.unlimited_vehicles != 0;
	}
	// A LAN host role follows the server type it was just given.
	ensure_session_role();
}

Ref<HostSessionOptions> Simulation::get_host_session_config() const {
	Ref<HostSessionOptions> out;
	out.instantiate();
	out->assign_config(net_.host_session_config);
	out->set_bind_port(net_.host_bind_port);
	// Return a request that can be applied again: the session's cap, which
	// player_slot_limit adds the reserved dedicated slot to.
	out->set_max_players(static_cast<int>(net_.host_max_players));
	out->set_serve_and_play(net_.host_serve_and_play);
	return out;
}

int Simulation::get_mission_header_size() const {
	return static_cast<int>(net_.host_session_config.mission_header_blob.size());
}

void Simulation::set_join_character_profile(const Ref<CharacterJoinProfile> &p_profile) {
	if (p_profile.is_null()) return;
	net_.join_character_vars = opennova::inmatch::character_join_vars(p_profile->value());
	net_.join_character_vars_set = true;
	install_character_join_vars();
}

void Simulation::set_local_character_profile(const Ref<CharacterJoinProfile> &p_profile) {
	if (p_profile.is_null()) return;
	net_.local_character_vars = opennova::inmatch::character_join_vars(p_profile->value());
	net_.local_character_vars_set = true;
}

void Simulation::set_app_id(const String &p_token) {
	// The .joi-recovered game-session BT join token a NovaWorld host validates
	// (reject code 9). Empty/"0" is the LAN default. Applied to the live joiner
	// runtime immediately and re-applied on each (re)load via install_app_id.
	net_.app_id = opennova::to_std(p_token.strip_edges());
	if (net_.app_id.empty()) net_.app_id = "0";
	install_app_id();
}

void Simulation::set_join_network_type(int p_type) {
	net_.join_network_type =
			p_type == static_cast<int>(opennova::inmatch::NetworkType::NovaWorld)
			? opennova::inmatch::NetworkType::NovaWorld
			: opennova::inmatch::NetworkType::Lan;
	// A NovaWorld join's session destroy keeps its transport (engine:
	// inmatch::ClientRuntime::tear_down_transport).
	if (joiner_role_ != nullptr)
		joiner_role_->set_join_novaworld(
				net_.join_network_type == opennova::inmatch::NetworkType::NovaWorld);
}

void Simulation::set_join_cd_cookie(const PackedByteArray &p_cookie) {
	// The CD identity cookie (packed PUB* blob) for the C2S 0x00 JOIN. Retained
	// and re-applied to the joiner runtime on each (re)load via install_join_cd_cookie.
	net_.join_cd_cookie.assign(p_cookie.ptr(), p_cookie.ptr() + p_cookie.size());
	install_join_cd_cookie();
}

void Simulation::set_join_glsvss_request(const PackedByteArray &p_request) {
	net_.join_glsvss_request.assign(p_request.ptr(), p_request.ptr() + p_request.size());
	install_join_glsvss_request();
}

void Simulation::set_join_discovered_session(const Ref<JoinTarget> &p_target) {
	opennova::inmatch::JoinerConnection::DiscoveredSession session;
	if (p_target.is_valid()) {
		session.present = p_target->get_discovered_session();
		session.host_key = static_cast<uint32_t>(p_target->get_session_host_key());
		session.password_required = p_target->get_session_password_required();
		session.expansion = opennova::to_std(p_target->get_expansion());
		// The browse's CI, which the join goes on with (D-NET-521).
		session.client_index = static_cast<uint32_t>(p_target->get_session_client_index());
	}
	net_.join_discovered_session = std::move(session);
	// ... and the browse's socket (D-NET-NEW-R3-3).
	net_.join_transport = p_target.is_valid() ? p_target->get_transport() : Ref<UdpPump>();
	// Applied to a live joiner runtime now and on each (re)load in enable_join.
	if (runtime_) runtime_->set_discovered_session(net_.join_discovered_session);
}

void Simulation::set_join_expansion_version_root(const String &p_game_root) {
	// D-NET-166: the JOIN VERSIONCRCSTRING checksum source. The runtime CRCs
	// the loose expansion/<SUS2>/version.txt under this root at JOIN-build
	// time (see JoinerConnection::set_expansion_version_root).
	net_.join_expansion_version_root = opennova::to_std(p_game_root);
	install_expansion_version_root();
}

// ---- co-op LAN joiner (D.2) -------------------------------------------------

bool Simulation::enable_join(const String &p_host_ip, int p_port,
		const String &p_player_name, int p_join_role,
		const String &p_spectator_password, const String &p_server_password,
		const String &p_join_password) {
	// P7: the joiner is a non-authority inmatch::ClientRuntime (Joiner role) built per-load by the boot's role hook;
	// it owns the connect-leg state machine + the S2C->ClientState fold internally. Here we only dial
	// the socket + store the player name (the ClientAuth.NA display callsign). Leave
	// listen_server_ false (the session kind is the flag); a sim is host XOR joiner.
	// The joiner role, constructed with the loadout profile seams it keeps; the
	// session's role switch is the gate: re-dialing a live mission is rejected
	// without partially replacing its transport. A session with no title identity
	// (session_title) has nothing to join with.
	const opennova::TitleIdentity *title = session_title(net_.title);
	if (title == nullptr) return false;
	if (!install_role(std::make_unique<opennova::inmatch::JoinerRole>(joiner_kit_seams())))
		return false;
	joiner_role_->set_title(title);
	// The process's 0x4C report count runs on across joins (ClientProcessState).
	joiner_role_->use_process_state(net_.client_process);
	// The process's log devices: a session destroy's stop of a held server writes its
	// _inout.txt lines (engine: inmatch::JoinerRole::destroy_session).
	if (net_.log_devices != nullptr) joiner_role_->set_server_logs(net_.log_devices->logs());
	if (net_.join_transport.is_valid() && net_.join_transport->is_open()) {
		// The join goes on with the browse's socket, the one the row was found on:
		// retail's client has one transport socket per network type, and the join
		// continues on the browse's connection (on NovaWorld the lobby session's
		// socket, its demux keeping the two protocols apart). The connection reads
		// its peer's datagrams alone (engine: net/npwire/lan_discovery.h, the
		// browser's transport; net/npwire/datagram_demux.h, the per-source lookup).
		// The row's address is the 0x81's source, a dotted quad.
		const opennova::PeerAddr host{
				opennova::resolve_address_without_dns(opennova::to_std(p_host_ip)),
				static_cast<uint16_t>(p_port)};
		if (host.ip == 0) {
			(void)install_offline_role();
			return false;
		}
		net_.pump = net_.join_transport;
		// A node of the process's held server reads its joiner's datagrams there too
		// (engine: inmatch::JoinerRole::deposit_inbound, D-NET-NEW-L1-26).
		auto join_socket = std::make_unique<UdpPumpGameSocket>(net_.pump, host);
		if (net_.client_process != nullptr)
			join_socket->set_held_server(&net_.client_process->unstopped_server);
		net_.pump_socket = std::make_unique<opennova::CountingDatagramSocket>(std::move(join_socket));
		joiner_role_->set_socket(net_.pump_socket.get(), host);
	} else {
		if (net_.pump.is_null()) net_.pump.instantiate();
		net_.pump->set_capture_path(net_.capture_pcap_path);
		// The dial's socket open is the client arm's: the block's mplanclient quad
		// with its random word, on its mpipaddressstring (engine:
		// inmatch::transport_bind_ports(block, Lan, false), D-NET-NEW-L8-10).
		if (net_.game_config.is_valid())
			net_.pump->set_client_bind_scan(
					opennova::inmatch::transport_bind_ports(net_.game_config->native().cfg(),
							opennova::inmatch::NetworkType::Lan, /*authority=*/false),
					opennova::inmatch::transport_bind_address(net_.game_config->native().cfg()));
		const int dialed = net_.pump->dial(p_host_ip, p_port);
		// The dial's socket open writes its connect-log lines; a name that resolves to nothing
		// opens none (engine: inmatch::write_transport_socket_open_lines).
		if (net_.log_devices != nullptr && dialed != static_cast<int>(ERR_CANT_RESOLVE)) {
			const opennova::PeerAddr local = net_.pump->local_address();
			const opennova::gamecfg::GameCfg &block = net_.game_config.is_valid()
					? net_.game_config->native().cfg()
					: opennova::gamecfg::defaults();
			opennova::inmatch::write_transport_socket_open_lines(net_.log_devices->connect(),
					net_.pump->is_open() ? &local : nullptr,
					opennova::inmatch::transport_bind_address(block),
					opennova::inmatch::transport_port_quad(block,
							opennova::inmatch::NetworkType::Lan, /*authority=*/false));
		}
		if (dialed != 0) {
			(void)install_offline_role();
			return false;
		}
		// The joiner role reads and writes the dialed pump through the adapter
		// from now on.
		net_.pump_socket = std::make_unique<opennova::CountingDatagramSocket>(
				std::make_unique<UdpPumpDatagramSocket>(net_.pump.ptr()));
		joiner_role_->set_socket(net_.pump_socket.get(), net_.pump->dialed_host());
	}
	// The join socket's bound address, which a held server's HOST STOPPED prints (engine:
	// inmatch::JoinerRole::set_local_address).
	joiner_role_->set_local_address(net_.pump->local_address());
	// Build the Joiner runtime now so get_joiner_phase reads Idle before the first load (the contract
	// the legacy joiner_session_ held); the role retains the request for a load that rebuilds it.
	// The callsign goes out as the profile name's code-page bytes, the NA TLV and the host's
	// roster carrying them (engine: inmatch/joiner_role.h create_runtime; D-NET-661).
	runtime_ = &joiner_role_->create_runtime(
			opennova::utf8_to_retail_text(opennova::to_std(p_player_name)),
			p_join_role == static_cast<int>(opennova::inmatch::JoinRole::Spectator)
					? opennova::inmatch::JoinRole::Spectator
					: opennova::inmatch::JoinRole::Player,
			opennova::to_std(p_spectator_password),
			opennova::to_std(p_server_password),
			opennova::to_std(p_join_password));
	// The process's _connectlog.txt, which the session key's handler closes.
	runtime_->set_connect_log(net_.log_devices != nullptr ? net_.log_devices->logs().connect
	                                                     : nullptr);
	// The join's network type: a NovaWorld join's session destroy keeps its transport.
	joiner_role_->set_join_novaworld(
			net_.join_network_type == opennova::inmatch::NetworkType::NovaWorld);
	// The session reset's connection destroy drains the process enumerator's records, the
	// browse the join went on from (engine: ClientRuntime::drain_enumerator, D-NET-NEW-L8-8).
	joiner_role_->set_enumerator_drain([] { LanSession::drain_process_enumerator(); });
	install_console_seams();
	install_charattr_table();
	install_character_join_vars();
	install_auto_medic_preference();
	install_expansion_version_root();
	install_app_id();
	install_join_cd_cookie();
	install_join_glsvss_request();
	install_join_mission_start();
	runtime_->set_discovered_session(net_.join_discovered_session); // the browse row's 0x81
	install_item_catalog();
	install_character_registry();
	// The kind-derived world rules (no authority, an mp session) applied with
	// the role install.
	joiner_role_->reset_for_join();
	net_.joiner_applied_loadout_revision = 0;
	if (!session_.begin_connect().applied()) {
		(void)install_offline_role();
		return false;
	}
	return true;
}

bool Simulation::is_local_spectator() const {
	if (is_joiner()) return runtime_ != nullptr && runtime_->is_spectator();
	const opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx == nullptr) return false;
	for (const opennova::inmatch::NapiNPConnection &connection :
			ctx->np_protocol.connection_list) {
		if (connection.type ==
				opennova::inmatch::NapiNPConnection::kTypeClientSide) {
			return connection.link.spectator;
		}
	}
	return false;
}

bool Simulation::set_local_spectator(bool p_spectator) {
	// A joiner is non-authoritative: its S2C 0x75 state is intentionally
	// read-only. F3 mutates only the in-process SP/listen-host player, one way:
	// retail has no spectator leave (Server_SetPlayerSpectator refuses false).
	opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (is_joiner() || kernel_ == nullptr || ctx == nullptr || !ctx->is_authority) return false;
	for (opennova::inmatch::NapiNPConnection &connection :
			ctx->np_protocol.connection_list) {
		if (connection.type !=
				opennova::inmatch::NapiNPConnection::kTypeClientSide) {
			continue;
		}
		if (!opennova::inmatch::Server_SetPlayerSpectator(
					*ctx, connection, kernel_->world, p_spectator)) {
			return false;
		}
		kernel_->local.reset_local_player_input_to_player_facing();
		set_local_player_weapon_input(false, false, false);
		return true;
	}
	return false;
}

bool Simulation::load_charattr(
		const Ref<ResourceRoot> &p_resource_root) {
	// Game_Run clears all 0x7C0 bytes before attempting the boot-soft load.
	// Preserve that failure result: missing/empty input is not replaced with a
	// synthetic row, and joining continues with the checksum's inactive zero
	// (runtime/inmatch/charattr_table.h charattr_load).
	net_.charattr_table = {};
	PackedByteArray bytes;
	if (p_resource_root.is_valid() && p_resource_root->has_file("charattr.def"))
		bytes = p_resource_root->read_file("charattr.def");
	opennova::inmatch::charattr_load(net_.charattr_table, bytes.ptr(), static_cast<std::size_t>(bytes.size()));
	install_charattr_table();
	return net_.charattr_table.loaded;
}

void Simulation::finalize_loaded_model_challenge_snapshot() {
	// The page the C2S 0x3D answers from is the model-def list the mission start's loads
	// built, computed from the mount and the tables as retail's loads run, not from what
	// this shell happened to render (D-NET-527; engine: inmatch/loaded_model_list.h). A
	// joiner loads no mission body, so it has no special event, and its pool 0 is its
	// replica's; a Serve Only host loads no foliage.
	namespace inmatch = opennova::inmatch;
	std::vector<uint32_t> page;
	net_.mission_start_model_files.clear();
	net_.mission_start_model_entries.clear();
	if (kernel_ && assets_.root.is_valid()) {
		const opennova::mission::BootFileSource files =
				opennova::mission::boot_files_from_index(assets_.root->native_index());
		const inmatch::LoadedModelList list = inmatch::mission_start_model_list(*kernel_, files,
				&net_.charattr_table, !is_joiner(),
				/*dedicated=*/!is_joiner() && !net_.host_serve_and_play,
				is_joiner() && runtime_ != nullptr ? &runtime_->state() : nullptr);
		page = list.freeze();
		net_.mission_start_model_files = list.model_files();
		net_.mission_start_model_entries = inmatch::loaded_model_list_entries(list);
	}
	// The same frozen count is a host's S2C 0x68 cursor wrap (the role reads
	// it every tick) [orig: Server_SendLoadedModelPageRequest @0x511375 / @0x511391].
	net_.loaded_model_list_count = static_cast<uint32_t>(page.size());
	if (!is_joiner() || !runtime_) return;
	runtime_->set_loaded_model_challenge_snapshot(std::move(page));
}

TypedArray<ModelListNodeRow> Simulation::debug_mission_start_model_nodes() const {
	TypedArray<ModelListNodeRow> rows;
	for (const opennova::inmatch::LoadedModelListEntry &entry : net_.mission_start_model_entries) {
		Ref<ModelListNodeRow> row;
		row.instantiate();
		row->assign(entry);
		rows.push_back(row);
	}
	return rows;
}

int64_t Simulation::debug_loaded_model_list_count() const {
	return static_cast<int64_t>(net_.loaded_model_list_count);
}

TypedArray<JoinerNetTraceRow> Simulation::debug_joiner_net_trace(int64_t p_from_seq) const {
	TypedArray<JoinerNetTraceRow> rows;
	if (runtime_ == nullptr) return rows;
	const uint32_t from = static_cast<uint32_t>(
			std::clamp<int64_t>(p_from_seq, 0, static_cast<int64_t>(UINT32_MAX)));
	for (const opennova::inmatch::JoinerNetTraceRecord &record :
			runtime_->net_trace().since(from)) {
		Ref<JoinerNetTraceRow> row;
		row.instantiate();
		row->assign(record);
		rows.push_back(row);
	}
	return rows;
}

int64_t Simulation::debug_joiner_net_trace_next_seq() const {
	return runtime_ != nullptr ? static_cast<int64_t>(runtime_->net_trace().next_seq()) : 0;
}

PackedStringArray Simulation::get_mission_start_model_files() const {
	PackedStringArray files;
	for (const std::string &file : net_.mission_start_model_files) {
		files.push_back(opennova::to_gd(file));
	}
	return files;
}

void Simulation::set_join_mission_start_loading(bool p_loading) {
	net_.join_mission_start_loading = p_loading;
	install_join_mission_start();
}

void Simulation::set_join_sync_tail_loading(bool p_loading) {
	net_.join_sync_tail_loading = p_loading;
	install_join_mission_start();
}

bool Simulation::is_join_mission_start_released() const {
	return is_joiner() && runtime_ != nullptr && runtime_->mission_start_released();
}

void Simulation::finish_join_mission_start() {
	if (joiner_role_ != nullptr) joiner_role_->push_mission_start_end();
}

bool Simulation::poll_join_preload() {
	if (joiner_role_ == nullptr || !runtime_) return false;
	// The pre-mission subset of the joiner role's frame: the same UDP socket and
	// ClientRuntime advance the retail connect exchange; no World exists yet to tick,
	// and the load waits for the S2C 0x1A's mission start (D-NET-NEW-L2-19).
	joiner_role_->poll_preload();
	return true;
}

void Simulation::cancel_join() {
	if (joiner_role_ != nullptr) joiner_role_->cancel_join();
}

bool Simulation::cancel_join_final_wait() {
	return is_joiner() && runtime_ && runtime_->cancel_final_wait();
}

bool Simulation::is_join_game_loop() const {
	return is_joiner() && runtime_ && runtime_->in_game_loop();
}

bool Simulation::is_join_mission_start_aborted() const {
	return is_joiner() && runtime_ && runtime_->mission_start_aborted();
}

bool Simulation::is_join_preload_ready() const {
	return is_joiner() && runtime_ && runtime_->preload_ready();
}

String Simulation::get_join_admission_stage() const {
	if (!is_joiner() || !runtime_) {
		return String();
	}
	return String(runtime_->admission_stage_name());
}

bool Simulation::has_join_mission() const {
	return is_joiner() && runtime_ && runtime_->mission_known();
}

// String::utf8, not the Latin-1 const char* constructor: the LAN browser rows
// decode the same wire fields as UTF-8, and both presentations of one host's
// metadata must agree byte-for-byte.
String Simulation::get_join_server_name() const {
	return (is_joiner() && runtime_) ? opennova::to_gd(runtime_->server_name()) : String();
}

String Simulation::get_join_mission_name() const {
	return (is_joiner() && runtime_) ? opennova::to_gd(runtime_->mission_name()) : String();
}

String Simulation::get_join_mission_file() const {
	return (is_joiner() && runtime_) ? opennova::to_gd(runtime_->map_file()) : String();
}

PackedByteArray Simulation::get_join_mission_header() const {
	PackedByteArray out;
	if (!is_joiner() || !runtime_ || !runtime_->has_mission_header()) return out;
	const std::vector<uint8_t> &bytes = runtime_->mission_header_bytes();
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	return out;
}

int64_t Simulation::get_join_terrain_til_state() const {
	if (!is_joiner() || !runtime_) return JOIN_TERRAIN_TIL_ABSENT;
	switch (runtime_->terrain_til_state()) {
	case opennova::inmatch::TerrainTilState::Absent:
		return JOIN_TERRAIN_TIL_ABSENT;
	case opennova::inmatch::TerrainTilState::Receiving:
		return JOIN_TERRAIN_TIL_RECEIVING;
	case opennova::inmatch::TerrainTilState::Complete:
		return JOIN_TERRAIN_TIL_COMPLETE;
	case opennova::inmatch::TerrainTilState::Invalid:
		return JOIN_TERRAIN_TIL_INVALID;
	}
	return JOIN_TERRAIN_TIL_INVALID;
}

PackedByteArray Simulation::get_join_terrain_til() const {
	PackedByteArray out;
	if (!is_joiner() || !runtime_ || !runtime_->has_terrain_til()) return out;
	const std::vector<uint8_t> &bytes = runtime_->terrain_til_bytes();
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	return out;
}

String Simulation::get_join_expansion() const {
	return (is_joiner() && runtime_) ? opennova::to_gd(runtime_->expansion()) : String();
}

int64_t Simulation::get_join_game_type() const {
	return (is_joiner() && runtime_) ? static_cast<int64_t>(runtime_->game_type()) : -1;
}

String Simulation::get_join_error() const {
	return (is_joiner() && runtime_) ? String(runtime_->last_error().c_str()) : String();
}

// The record a failed join or a lost session leaves on the joiner connection; the
// shell builds retail's reason text from it through its gameerr table
// (engine: inmatch/disconnect_reason.h).
Ref<ConnectionError> Simulation::get_connection_error() const {
	// A host's reason text reads its own client connection's record, none on a host with no
	// own client (engine: HostRole::own_client_error_record carries the witness).
	if (!is_joiner() && host_role_ != nullptr) {
		const std::optional<opennova::inmatch::ConnectionErrorRecord> own =
				host_role_->own_client_error_record();
		return own ? ConnectionError::make(*own) : ConnectionError::make_absent();
	}
	return ConnectionError::make((is_joiner() && runtime_)
					? runtime_->connection_error_record()
					: opennova::inmatch::ConnectionErrorRecord{});
}

// Where the join stands for the join screen (engine: inmatch/pre_game_menu.h).
Ref<JoinScreenStatus> Simulation::get_join_screen_status() const {
	if (!is_joiner() || !runtime_) return Ref<JoinScreenStatus>();
	return JoinScreenStatus::make(runtime_->join_screen_stage(), runtime_->join_queue(),
			runtime_->join_clock_ms());
}

// The in-match analog of get_join_error: the host closed the session on its own terms
// (the punt record), or an established session went silent past the witnessed connection
// reap window. Either way the disconnect event maps a reason code onto g_MissionExitReason
// — retail EXITS THE MISSION with a reason rather than raising an in-world dialog, so the
// shell's analog is return-to-menu with the reason surfaced.
// (engine: runtime/inmatch/joiner_connection.h)
String Simulation::get_session_loss_reason() const {
	if (!is_joiner() || !runtime_) return String();
	return opennova::to_gd(runtime_->session_loss_reason());
}

bool Simulation::is_session_lost() const {
	return is_joiner() && runtime_ && runtime_->session_lost();
}

int Simulation::get_mission_exit_reason() const {
	if (is_joiner()) {
		return runtime_ ? runtime_->mission_exit_reason() : 0;
	}
	// The net writers' store first, then the world-side writers' (the roles'
	// session_lost reads the same pair).
	const opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx != nullptr && ctx->mission_exit_reason != 0) return ctx->mission_exit_reason;
	return kernel_ != nullptr ? kernel_->world.mission_exit_reason : 0;
}

int Simulation::round_over_key(int p_vk, int p_restart_vk) {
	if (!kernel_) return 0;
	return static_cast<int>(
			opennova::inmatch::round_over_key(role_view(), active_role(), p_vk, p_restart_vk));
}

bool Simulation::ingame_restart() {
	return kernel_ != nullptr && kernel_->world.ingame_restart_command();
}


bool Simulation::is_joined_in_match() const {
	return is_joiner() && runtime_ && runtime_->in_match();
}

bool Simulation::escape_input_reset() {
	// The Escape key's reset (engine inmatch/session.h Role::reset_local_input carries
	// the witness); true when the device owes the Scroll Lock turn-off.
	return role_ != nullptr && kernel_ && active_role().reset_local_input();
}

void Simulation::input_action_exit() {
	// Input action 3 over the running role (engine inmatch/remote_admin.h
	// input_action_exit carries the witness).
	if (kernel_)
		opennova::inmatch::input_action_exit(
				remote_admin_binding(), opennova::inmatch::ExitSite::Input);
}

void Simulation::reset_all_input_state() {
	if (kernel_) active_role().reset_all_input_state();
}

bool Simulation::take_input_reset() {
	// The joiner's mission start owes one, and a role action (the deploy action's
	// head, D-NET-685) another; both drain.
	const bool mission_start = is_joiner() && runtime_ && runtime_->take_input_reset();
	const bool action = active_role().take_input_reset();
	return mission_start || action;
}

bool Simulation::is_join_initial_admission_complete() const {
	return is_joiner() && runtime_ && runtime_->initial_admission_complete();
}

bool Simulation::is_joiner_network_diagnostics_enabled() const {
	return net_.joiner_net_diagnostics;
}

void Simulation::set_joiner_network_diagnostics_enabled(bool p_enabled) {
	net_.joiner_net_diagnostics = p_enabled;
}

void Simulation::set_capture_pcap_path(const String &p_path) {
	net_.capture_pcap_path = p_path;
}

// The MCP/GUT form of the engine's joiner diagnostics
// (inmatch::joiner_network_diagnostics): a conversion, keys unchanged.
Dictionary Simulation::get_joiner_network_diagnostics() const {
	const opennova::inmatch::JoinerNetworkDiagnostics d =
			opennova::inmatch::joiner_network_diagnostics(runtime_, joiner_role_);
	Dictionary out;
	out["enabled"] = is_joiner_network_diagnostics_enabled();
	out["frontier_seq"] = static_cast<int64_t>(d.frontier_seq);
	out["outbound_seq"] = static_cast<int64_t>(d.outbound_seq);
	out["records_applied"] = static_cast<int64_t>(d.records_applied);
	out["gap_depth"] = static_cast<int64_t>(d.gap_depth);
	out["retained_outbound"] = static_cast<int64_t>(d.retained_outbound);
	out["flat_seconds"] = d.flat_seconds;
	out["freeze_suspected"] = d.freeze_suspected;
	out["in_match"] = d.in_match;
	out["deployed"] = d.deployed;
	out["client_index"] = static_cast<int64_t>(d.client_index);
	if (d.present) {
		out["stage"] = opennova::to_gd(d.stage);
		Dictionary crc;
		crc["entity_checksum_seen"] = static_cast<int64_t>(d.entity_checksum_seen);
		crc["entity_checksum_answered"] = static_cast<int64_t>(d.entity_checksum_answered);
		crc["loadout_crc_seen"] = static_cast<int64_t>(d.loadout_crc_seen);
		crc["loadout_crc_answered"] = static_cast<int64_t>(d.loadout_crc_answered);
		crc["charattr_seen"] = static_cast<int64_t>(d.charattr_seen);
		crc["charattr_row_missing"] = static_cast<int64_t>(d.charattr_row_missing);
		crc["property_clears"] = static_cast<int64_t>(d.property_clears);
		out["challenges"] = crc;
		if (d.reject_set) {
			Dictionary r;
			r["jfc"] = d.reject_jfc;
			r["jfp"] = d.reject_jfp;
			r["jfs"] = opennova::to_gd(d.reject_jfs);
			out["last_reject"] = r;
		}
		if (d.disconnect_set) {
			Dictionary dc;
			dc["dc"] = d.disconnect_dc;
			dc["dpc"] = d.disconnect_dpc;
			dc["ddstr"] = opennova::to_gd(d.disconnect_ddstr);
			dc["dstr"] = opennova::to_gd(d.disconnect_dstr);
			out["last_disconnect"] = dc;
		}
	}
	return out;
}

bool Simulation::native_net_report(opennova::inmatch::NetDebugReport &r_out) const {
	r_out = opennova::inmatch::net_debug_report(session_, host_ctx(), runtime_, joiner_role_,
			net_.pump_socket.get());
	return true;
}

bool Simulation::is_join_deploy_pick_pending() const {
	return is_joiner() && runtime_ && runtime_->deployment_pick_pending();
}

bool Simulation::is_join_deploy_hold_ready() const {
	return opennova::inmatch::joiner_deploy_hold_ready(role_view());
}

bool Simulation::is_join_in_match_ready(bool p_auto_deploy) const {
	return opennova::inmatch::joiner_in_match_ready(role_view(), p_auto_deploy);
}

bool Simulation::is_deploy_overlay_active() const {
	// The deploy-map overlay (retail g_DeployScreenActive): armed by the S2C
	// 0x0F game_flags bit0, then host-maintained per frame from the 0x0A flags1
	// bit1, on the listen host's own loopback client too. A UI signal only —
	// it never gates the spawn. (engine: runtime/replication/client_state.h)
	return runtime_ && runtime_->state().deploy_overlay_active;
}

bool Simulation::take_death_menu_open(bool p_menu_open) {
	// The triggers are the engine's (inmatch::death_menu_triggered) and the
	// open latch with its clear is ClientState's (client_state.h
	// death_menu_open_latch); this is the typed seam for the shell's frame.
	return runtime_ && runtime_->state().take_death_menu_open(
			opennova::inmatch::death_menu_triggered(role_view()), p_menu_open);
}

bool Simulation::is_death_menu_held() const {
	return opennova::inmatch::death_menu_triggered(role_view()) || is_join_deploy_pick_pending();
}

int Simulation::get_join_assigned_team() const {
	return is_joiner() && runtime_ ? runtime_->assigned_team() : 0;
}

int Simulation::get_class_allow_mask() const {
	if (is_joiner() && runtime_) return runtime_->class_allow_mask();
	if (const opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
			ctx != nullptr && ctx->is_authority != 0)
		return ctx->config.class_allow_mask;
	// No session yet: the mask the configured class words build at the session's
	// first mission (the host dialog writes words, never a mask).
	return opennova::inmatch::class_allow_mask_from_words(
			net_.host_session_config.class_availability, /*has_mission_entry=*/true);
}

const opennova::world::SpawnZoneRegistry &Simulation::deploy_zone_registry() {
	// The cache's lifetime is the engine's (world::SpawnZoneRegistryCache); a
	// fresh world invalidates it.
	if (!kernel_) return net_.deploy_zone_registry.registry;
	return net_.deploy_zone_registry.get(kernel_->world,
			joiner_role_ != nullptr ? joiner_role_->world_sync_serial() : 0);
}

int Simulation::deploy_key_pick(int p_vk) {
	if (!kernel_) return -1;
	return opennova::inmatch::deploy_key_pick(role_view(), deploy_zone_registry(), p_vk);
}

std::vector<opennova::world::DeployZoneRow> Simulation::deploy_zone_rows() {
	// The DEATH screen's zone rows (inmatch/role_feeds.h carries the witnesses).
	std::vector<opennova::world::DeployZoneRow> rows;
	if (!kernel_) return rows;
	opennova::inmatch::deploy_zone_rows(role_view(), deploy_zone_registry(), rows);
	return rows;
}

TypedArray<DeployZoneRow> Simulation::get_deploy_spawn_zones() {
	TypedArray<DeployZoneRow> out;
	for (const opennova::world::DeployZoneRow &row : deploy_zone_rows()) {
		Ref<DeployZoneRow> record;
		record.instantiate();
		record->assign(row);
		out.push_back(record);
	}
	return out;
}

bool Simulation::send_deployment_pick(int p_param) {
	// [orig: Input_HandleActionBinding case 12 @0x49b0c5-0x49b17b — param 0 -> 0xFFFF,
	// 65534 -> 0xFFFE, else SpawnZoneList_GetByIndex(param-1) -> the entity handle;
	// an index that resolves no entity falls through to 0xFFFF @0x49b17b LABEL_70]
	// The listen host's own client sends the same pick over its loopback; the
	// role's case-12 gate turns a dead permanent-death press into the 0x51
	// convert or nothing (Role::request_deployment_pick).
	if (!runtime_ || !kernel_) return false;
	return active_role().request_deployment_pick(
			opennova::world::deploy_pick_wire_value(p_param, deploy_zone_registry()));
}

int Simulation::get_joiner_phase() const {
	return (is_joiner() && runtime_) ? static_cast<int>(runtime_->phase()) : -1;
}

int Simulation::get_joiner_self_handle() const {
	// 0 means unbound at this seam (the GDScript consumers poll `> 0`); the role itself
	// reports EntityHandle::kInvalid while the identity is not bound.
	return joiner_role_ != nullptr && joiner_role_->has_self_wire_handle()
			? static_cast<int>(joiner_role_->self_wire_handle()) : 0;
}

bool Simulation::admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team) {
	opennova::inmatch::ListenHostState *host = host_state();
	if (!is_host_listening() || host == nullptr) return false;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x,-z,y), the inverse of the present remap (same as spawn_local_player).
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.heading = opennova::world::placed_heading_bam(static_cast<int32_t>(p_yaw_deg));
	spawn.team = static_cast<uint8_t>(p_team);
	// A synthetic loopback peer; distinct port per call so repeated admits don't alias. Own a transport
	// so the connection is well-formed. The synthetic admit (no handshake) mirrors the post-PeerSpawned
	// state; with the host already at net_id 0xFFF0 the joiner allocates 0xFFF1.
	const opennova::PeerAddr peer{0x0100007Fu, static_cast<uint16_t>(40000 + host->host_owner.peers.size())};
	opennova::inmatch::PeerLink &link = host->host_owner.peers[peer];
	if (!link.transport) {
		link.transport = std::make_unique<opennova::replication::UdpSessionTransport>(
				opennova::replication::UdpSessionTransport::Role::Host);
	}
	const opennova::world::EntityHandle h =
			opennova::inmatch::admit_synthetic_peer(
					host->host_owner.ctx, kernel_->world, peer, spawn, link.transport.get());
	kernel_->resolve_new_infantry_adm_ids();
	return h.valid();
}


void Simulation::retain_feed_announcement(const String &text, int64_t tick) {
	if (runtime_) runtime_->state().kill_announcement.record(
			opennova::to_std(text), static_cast<uint32_t>(tick));
}
String Simulation::get_kill_announcement_text() const {
	return runtime_ ? opennova::to_gd(runtime_->state().kill_announcement.text) : String();
}
int64_t Simulation::get_kill_announcement_tick(int64_t now) {
	if (!runtime_) return 0;
	auto &announcement = runtime_->state().kill_announcement;
	announcement.expire(static_cast<uint32_t>(now));
	return announcement.tick;
}

String Simulation::format_feed_line(const String &p_template, const String &p_attacker,
		const String &p_victim, const String &p_extra,
		const String &p_bonus_template) const {
	return opennova::to_gd(opennova::hud::feed_format_line(
			opennova::to_std(p_template), opennova::to_std(p_attacker),
			opennova::to_std(p_victim), opennova::to_std(p_extra),
			opennova::to_std(p_bonus_template)));
}

String Simulation::format_feed_camp_line(const String &p_template,
		const String &p_wpname) const {
	return opennova::to_gd(opennova::hud::feed_format_camp_line(
			opennova::to_std(p_template), opennova::to_std(p_wpname)));
}


// One wire row's REMOTE-body sounds for this frame: resolve the row's clip,
// scan the trigger words its wire-driven playhead crossed, and hand the
// witnessed consume to the portable engine leg
// (world::wire_body_slot_sounds; the witness map lives in
// world/wire_body_sound.h and docs/audio/lwf-dbf-sound-re.md).
//
// Exactly one sound source per drawn body: this consume runs only for rows
// the wire pass renders (a joiner's remote rows; a listen host's admitted
// players — authored rows defer to the authority presenter and never enter
// the wire plan), and the authority tick's sound pass never reaches
// net-snapped peers (tick_infantry returns at the net-snap gate before the
// consume, the gate AiSystem::tick_infantry cites; each machine instead
// consumes from the body updater of every body it draws; see
// docs/audio/lwf-dbf-sound-re.md).
void Simulation::present_wire_body_sounds(int p_type_id, int p_character_id,
		int p_wire_handle, int p_carrier_handle, int p_anim_state,
		int p_from_phase, int p_to_phase, const Vector3 &p_pos) {
	if (!world_installed_ || !kernel_ || p_anim_state < 0) return;
	if (p_to_phase <= p_from_phase) return;
	const int adm_id = kernel_->cached_adm_id_for_runtime_type(static_cast<uint16_t>(p_type_id));
	if (adm_id < 0) return;

	uint32_t words[16] = {};
	const int n = kernel_->root_motion.scan_triggers(adm_id, p_anim_state,
			p_from_phase, p_to_phase, words, 16, /*variant=*/0);
	if (n <= 0) return;
	// The dip belongs to the frame the playhead ended on (a multi-frame
	// catch-up dips all its words by the final frame's bottom — the scan
	// carries no per-word phases).
	const int32_t capsule_bottom =
			kernel_->root_motion.capsule_bottom_at(adm_id, p_anim_state, p_to_phase,
					/*variant=*/0);
	// Godot -> mission 16.16 (util/axes.h).
	int32_t body[3];
	fixed3_from_godot(p_pos, body);
	opennova::world::wire_body_slot_sounds(kernel_->world, words, n, capsule_bottom,
			p_type_id, static_cast<uint16_t>(p_character_id),
			static_cast<uint16_t>(p_wire_handle), p_carrier_handle >= 0, body);
}

// The Tab board's header as the shell needs it. Row data no longer rides a
// script Dictionary: HudOverlay pulls the drawn rows natively through
// fill_scoreboard, and the counts here come from the same replication
// projection (replication::scoreboard_header — the accepted-rows-minus-spectators
// players count is the witnessed header arithmetic, retail @0x4231dd).
Ref<ScoreboardHeader> Simulation::get_scoreboard() const {
	Ref<ScoreboardHeader> out;
	out.instantiate();
	if (!runtime_) return out;
	opennova::replication::ClientScoreboardSession v;
	v.header = opennova::replication::scoreboard_header(runtime_->state());
	// The drawer branches on the session game type (retail reads g_GameType
	// @0x423acb); the header's session strings are the role's session
	// variables (inmatch/role_feeds.h scoreboard_session_vars).
	v.game_type = runtime_->game_type();
	const opennova::SessionVars vars = opennova::inmatch::scoreboard_session_vars(role_view());
	v.server_name = vars.server_name;
	v.mission_name = vars.mission_name;
	out->assign(v);
	return out;
}

bool Simulation::fill_scoreboard(opennova::hud::HudScoreboardState &r_state) const {
	opennova::inmatch::scoreboard_feed(role_view(), r_state);
	return runtime_ != nullptr;
}
