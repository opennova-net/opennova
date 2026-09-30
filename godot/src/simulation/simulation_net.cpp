// Simulation — the net roles (ADR 0009/0011/0012): per-load listen-host
// bring-up + host pump, the LAN joiner pump family + wire proxies/events, the
// host session config FFI, and the joiner preload/session API.
#include "simulation/simulation_internal.h"
#include <runtime/inmatch/host_settings.h>
#include <runtime/inmatch/net_debug_report.h> // the F3 Net window + the joiner diagnostics
#include "network/udp_pump_datagram_socket.h"
#include "simulation/hud_view_records.h"
#include "simulation/deploy_rows.h" // the DEATH screen's zone / list rows
#include "object/character_join_profile.h" // the two-side character selection record
#include "network/host_session_options.h" // the hosted-session request record
#include "util/string_convert.h"

#include <cmath>
#include <cstring>

#include <runtime/inmatch/server_admin_command.h> // Server_ExecuteServerCommand (the NovaWorld ServerCommand verbs)
#include <runtime/inmatch/server_spawn.h> // Server_SetPlayerSpectator
#include <runtime/inmatch/session_status.h>
#include <runtime/terrain_query/surface_tiles.h> // surface_tiles_from_til_bytes (D-SND-15)
#include <formats/threedi/threedi_panm_pose.h> // the native PANM liveness gate (S3, ADR 0028)
#include <net/npwire/ingame_decode.h> // kRoundEventFlag* (the fire-mode byte)
#include <runtime/hud/feed_format.h> // the witnessed feed line/color policy
#include <runtime/replication/client_scoreboard_view.h> // the Tab board's draw-time projection
#include <runtime/hud/hud_frame.h> // HudScoreboardState (the Tab board feed)
#include <runtime/inmatch/role_feeds.h> // scoreboard_feed
#include <runtime/world/wire_body_sound.h> // the wire-fed remote body's footstep/foley consume
#include <net/npwire/net_ports.h> // lan_host_bind_ports (the D-NET-210 bind scan)
#include <base/vfs/vfs.h> // vfs_expansion_version_checksum (the D-NET-166 JOIN CRC)
#include <net/npwire/wire_handle.h>  // pool()/kPoolItem/kInvalid (the wire handle home)
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


// P7: the per-load host bring-up record — the faithful §5.0 mode-3 in-process listen server
// [orig: SinglePlayer_StartMission @0x561af0]. The host's own
// player AUTO-spawns through the real pipeline (Server_ProcessPendingPlayerSpawns ->
// resolve_player_spawn_pose marker chain), and its own loopback client renders the per-frame 0x0A.
// Staged on the host role right before the kernel boots; its boot-hook bring_up consumes it.
opennova::inmatch::HostBringup Simulation::host_bringup() {
	namespace inmatch = opennova::inmatch;
	// The host ctx's mission (read by the §5.1 0x0B BMS-header burst for LAN joiners)
	// points straight at the kernel's adopted document, which outlives the match.
	// Reload: the role's bring-up rebuilds the loopback and a fresh host owner drops stale
	// owner peers, while create_session owns the protocol
	// connection-table reset at this new-match boundary. No live configuration mutator can clear
	// admitted peers mid-match. serve_and_play: host_session_pump must NOT discard the host's own loopback 0x0A — we
	// fold it into ClientState (runtime_) to render the host's own view.
	// Serve-and-play (default) vs dedicated. Standalone SP is ALWAYS serve-and-play (it renders the
	// host's own player); isolated test/tooling MissionRoot instantiations keep that default too.
	// There is no live editor-preview branch: MainGame/GameWorld is the sole live mission runtime
	// (ADR 0025). A LAN host honors the UI server-type (net_.host_serve_and_play, from
	// configure_host_session). A dedicated host (serve_and_play=false) skips the own-player spawn +
	// the local view below and lets host_session_pump discard the host loopback (step 5) — mirroring
	// start_host_session's gating [orig: SinglePlayer_StartMission @0x561af0].
	const bool serve_and_play = is_host_listening() ? net_.host_serve_and_play : true;
	inmatch::GameConfig host_config;
	if (is_host_listening()) {
		host_config = net_.host_session_config; // mission/player/spawn + game_type/mp_attributes from the UI
		if (host_config.server_name.empty()) host_config.server_name = "OpenNova LAN Host";
		host_config.max_players = net_.host_max_players; // the UI player cap as configure_host_session published it (host_player_slot_limit)
	} else {
		// The SP listen server's config (docs/net/novaworld-net-re.md §5.0).
		host_config = inmatch::singleplayer_game_config(mission_game_type());
	}
	inmatch::HostBringup bringup;
	bringup.host_cfg.config = host_config;
	bringup.host_cfg.socket_mode = is_host_listening() ? inmatch::SocketMode::Lan : inmatch::SocketMode::Socketless;
	bringup.host_cfg.serve_and_play = serve_and_play;
	// The loose _NSTMOUT.TXT reap/pool override under the install root
	// (engine session_timeout_config.h; the CNapiNetwork_Init read). Empty
	// keeps the 120000 ms / 1200-record template.
	bringup.host_cfg.game_root = host_game_root_;
	if (net_.local_character_vars_set) {
		bringup.host_cfg.local_character_vars = net_.local_character_vars;
	}
	bringup.terrain_til_data = net_.terrain_til_data; // S2C 0x45 terrain-tile load source (empty => skipped, §5.37)
	bringup.mission_text = net_.mission_text;
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
		d["item_id"] = opennova::mission::visual_item_id_for_runtime_type(
				row->item_id, false);
		d["position"] = Vector3(row->position.x, row->position.y, row->position.z);
		d["rotation_deg"] = Vector3(static_cast<float>(row->pitch),
				static_cast<float>(row->yaw), static_cast<float>(row->roll));
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
					lae->pos[0] / 65536.0f, lae->pos[1] / 65536.0f,
					lae->pos[2] / 65536.0f,
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
	// The same bytes feed the sim's placed-tile surface array (D-SND-15);
	// the fold and its witness live engine-side (terrain_query
	// surface_tiles_from_til_bytes).
	assets_.surface_tiles =
			opennova::terrain::surface_tiles_from_til_bytes(net_.terrain_til_data);
	apply_terrain_to_ai();
}

void Simulation::set_mission_text_data(const PackedByteArray &p_rtxt_bytes) {
	// Empty bytes (no text file) leave the table unloaded without a warning.
	net_.mission_text = opennova::mission::MissionText();
	if (p_rtxt_bytes.is_empty()) return;
	std::string error;
	if (!opennova::mission::parse_mission_text(p_rtxt_bytes.ptr(),
				static_cast<std::size_t>(p_rtxt_bytes.size()), net_.mission_text, error)) {
		UtilityFunctions::push_warning(String("Simulation: mission text RTXT rejected: ") +
		                               opennova::to_gd(error));
	}
}

bool Simulation::set_score_config_data(const PackedByteArray &p_score_ini_bytes) {
	if (p_score_ini_bytes.is_empty()) return false;
	const std::string text(
			reinterpret_cast<const char *>(p_score_ini_bytes.ptr()),
			static_cast<std::size_t>(p_score_ini_bytes.size()));
	return opennova::inmatch::load_session_score_config(net_.host_session_config, text);
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
	// The LAN host scans the retail port range from the requested port
	// (D-NET-210; the sequence policy is npwire net_ports.h
	// lan_host_bind_ports). Port 0 keeps the OS-assigned bind — the dev/test
	// seam retail has no analog for.
	bool bound = false;
	if (p_port <= 0) {
		bound = net_.pump->bind_listen(0) == 0;
	} else {
		for (const uint16_t port : opennova::lan_host_bind_ports(
					 static_cast<uint32_t>(p_port))) {
			if (net_.pump->bind_listen(port) == 0) {
				bound = true;
				break;
			}
		}
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

void Simulation::set_novaworld_gsid(const String &p_gsid) {
	if (opennova::inmatch::NapiNPServerCtx *ctx = host_ctx()) {
		ctx->novaworld_gsid = opennova::to_std(p_gsid);
	}
}

std::vector<Simulation::HostPeerSlot> Simulation::host_peer_slots() const {
	std::vector<HostPeerSlot> out;
	const opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx == nullptr) return out;
	for (const opennova::inmatch::NapiNPConnection &c : ctx->np_protocol.connection_list) {
		if (c.type != opennova::inmatch::NapiNPConnection::kTypeServerSide ||
				c.phase < opennova::inmatch::ConnectionPhase::PlayerAdded) {
			continue;
		}
		HostPeerSlot s;
		s.slot = c.reply.player_slot;
		s.player_name = opennova::to_gd(c.player_name);
		s.ip_and_port = opennova::to_gd(opennova::peer_addr_to_string(c.peer));
		if (c.assigned_team_valid) s.team = String::num_int64(c.assigned_team);
		out.push_back(std::move(s));
	}
	return out;
}

Simulation::ServerCommandResult Simulation::execute_server_command(const String &p_verb,
		const String &p_target, const PackedStringArray &p_args) {
	ServerCommandResult out;
	opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx == nullptr) return out;
	std::vector<std::string> args;
	args.reserve(static_cast<size_t>(p_args.size()));
	for (int i = 0; i < p_args.size(); ++i) args.push_back(opennova::to_std(p_args[i]));
	const opennova::inmatch::ServerCommandOutcome outcome =
			opennova::inmatch::Server_ExecuteServerCommand(*ctx,
					kernel_ != nullptr ? &kernel_->world : nullptr, opennova::to_std(p_verb),
					opennova::to_std(p_target), args);
	out.handled = outcome.handled;
	out.stop_hosting = outcome.stop_hosting;
	out.config_changed = outcome.config_changed;
	if (outcome.config_changed) {
		// The sim's host session record is what the next session build reads.
		net_.host_session_config.server_name = ctx->config.server_name;
		net_.host_session_config.custom_text = ctx->config.custom_text;
		net_.host_session_config.multiplayer_reset = ctx->config.multiplayer_reset;
	}
	out.server_name = opennova::to_gd(ctx->config.server_name);
	out.server_message = opennova::to_gd(ctx->config.custom_text);
	return out;
}

void Simulation::set_novaworld_join_tickets(bool p_armed, PlayerEnterRequestHook p_hook) {
	opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
	if (ctx == nullptr) return;
	ctx->novaworld_join_tickets_armed = p_armed;
	// The prefix of the JOINTICKET key the watchdog looks up in the joiner's CD
	// identity pairs (the engine owns the witnessed constant).
	ctx->host_local_address =
			p_armed ? opennova::inmatch::NapiNPServerCtx::kNovaWorldLocalAddress : "";
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
			p_msg_code);
}

void Simulation::configure_host_session(const Ref<HostSessionOptions> &p_options) {
	if (p_options.is_null()) return;
	const opennova::inmatch::GameConfig &in = p_options->config();
	// Start from the live config so the sim-owned fields (mission header blob,
	// score tables, PCID) survive; every user-facing field lands from the record.
	opennova::inmatch::GameConfig config = net_.host_session_config;
	net_.host_bind_port = static_cast<uint16_t>(std::clamp(p_options->get_bind_port(), 0, 0xFFFF));
	config.server_name = in.server_name;
	config.mission_name = in.mission_name;
	config.mission_file = in.mission_file;
	config.custom_text = in.custom_text;
	config.player_name = in.player_name;
	config.expansion = in.expansion;
	config.server_password = in.server_password;
	config.side_a_password = in.side_a_password;
	config.side_b_password = in.side_b_password;
	config.country = in.country;
	config.server_punkbuster = in.server_punkbuster;
	config.server_lan_only = in.server_lan_only;
	config.connection_speed = in.connection_speed;
	config.max_friendly_kills = in.max_friendly_kills;
	config.allow_ai = in.allow_ai;
	config.time_of_day_continuity = in.time_of_day_continuity;
	config.spectator_password = in.spectator_password;
	config.spectator_slots = std::max(in.spectator_slots, -1);
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
	{
		const String requested = p_options->get_integrity_profile().strip_edges();
		const std::string id = opennova::to_std(requested);
		if (id.empty() ||
				opennova::inmatch::find_integrity_challenge_profile(id) != nullptr) {
			config.integrity_profile = id;
		} else {
			UtilityFunctions::push_warning(
					String("Unknown authority integrity profile: ") + requested);
			config.integrity_profile.clear();
		}
	}
	config.game_type = in.game_type;
	config.mp_attributes = in.mp_attributes;
	config.class_allow_mask = in.class_allow_mask;
	// The armory-reuse cooldown an accepted C2S 0x2F seeds (cfg `armory_reuse_time`,
	// admin `ArmoryTimer`; the engine default 30 is retail's).
	config.armory_reuse_time = in.armory_reuse_time;
	config.respawn_time = in.respawn_time;
	config.time_limit_minutes = in.time_limit_minutes;
	config.replay_enabled = in.replay_enabled;
	config.max_team_lives = in.max_team_lives;
	config.score_limit = in.score_limit;
	config.max_score = in.max_score;
	config.koth_delta = in.koth_delta;
	config.flag_return_ticks = in.flag_return_ticks;
	// The flag CARRY limit Server_CheckPlayerViolations enforces (cfg
	// `flag_reset_seconds`; the engine default 420 is retail's).
	config.flag_reset_seconds = in.flag_reset_seconds;
	config.capture_duration_seconds = in.capture_duration_seconds;
	config.capture_speed_setting = in.capture_speed_setting;
	config.spawn_wave_time_base = in.spawn_wave_time_base;
	config.spawn_wave_time_zone = in.spawn_wave_time_zone;
	config.default_spawn_requires_no_team_zone = in.default_spawn_requires_no_team_zone;
	config.num_teams = in.num_teams;
	config.respawn_timeout = in.respawn_timeout;
	config.start_delay = in.start_delay;
	config.destroy_buildings = in.destroy_buildings;
	config.death_messages = in.death_messages;
	// The witnessed BANDWIDTH server command (100-1600, clamped at apply):
	// lowers the per-frame 0x0A byte cap so entity records rotate across frames
	// [orig: g_EntitySendBudget @0xC8FC50].
	config.entity_send_budget = in.entity_send_budget;
	// Retail selects its default send divider from the session family, then
	// from g_LanMode for an authority LAN host. Explicit test/tool overrides
	// remain available through send_holdoff_ticks.
	// [orig: NapiNPServer_GetSendHoldoffTicks @0x4c4ab0]
	config.session_channel = in.session_channel;
	config.lan_mode = in.lan_mode;
	config.send_holdoff_ticks = in.send_holdoff_ticks;
	config.fat_bullets = in.fat_bullets;
	config.one_shot_kill = in.one_shot_kill;
	config.unlimited_vehicles = in.unlimited_vehicles;
	config.spawn_x = in.spawn_x;
	config.spawn_y = in.spawn_y;
	config.spawn_z = in.spawn_z;
	config.spawn_names = in.spawn_names;
	// Server type + player cap (UI host config): serve_and_play gates the host's own-player spawn +
	// loopback fold at bring-up; max_players is the session-list-advertised cap as the engine's
	// host_player_slot_limit publishes it (the dedicated slot added, the 65 ceiling, no lower clamp).
	net_.host_serve_and_play = p_options->get_serve_and_play();
	net_.host_max_players = opennova::inmatch::host_player_slot_limit(
			p_options->get_max_players(), net_.host_serve_and_play);
	config.max_players = net_.host_max_players;
	net_.host_session_config = std::move(config);
	if (kernel_ && is_host_listening()) {
		kernel_->world.rules.fat_bullets = net_.host_session_config.fat_bullets;
		kernel_->world.rules.one_shot_kill = net_.host_session_config.one_shot_kill;
		kernel_->world.rules.vehicle_respawns = net_.host_session_config.unlimited_vehicles;
	}
	// A LAN host role follows the server type it was just given.
	ensure_session_role();
}

Ref<HostSessionOptions> Simulation::get_host_session_config() const {
	Ref<HostSessionOptions> out;
	out.instantiate();
	out->assign_config(net_.host_session_config);
	out->set_bind_port(net_.host_bind_port);
	// Return a request that can be applied again without adding the reserved
	// dedicated slot a second time. player_slot_limit exposes the live limit.
	out->set_max_players(static_cast<int>(net_.host_max_players) -
			(net_.host_serve_and_play ? 0 : 1));
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

bool Simulation::set_join_integrity_profile(const String &p_profile_id) {
	const std::string id = opennova::to_std(p_profile_id.strip_edges());
	if (id.empty()) {
		net_.join_integrity_profile_id.clear();
		install_join_integrity_profile();
		return true;
	}
	if (opennova::inmatch::find_integrity_challenge_profile(id) == nullptr) {
		net_.join_integrity_profile_id.clear();
		install_join_integrity_profile();
		return false;
	}
	net_.join_integrity_profile_id = id;
	install_join_integrity_profile();
	return true;
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
}

void Simulation::set_join_cd_cookie(const PackedByteArray &p_cookie) {
	// The CD identity cookie (packed PUB* blob) for the C2S 0x00 JOIN. Retained
	// and re-applied to the joiner runtime on each (re)load via install_join_cd_cookie.
	net_.join_cd_cookie.assign(p_cookie.ptr(), p_cookie.ptr() + p_cookie.size());
	install_join_cd_cookie();
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
	// without partially replacing its transport.
	if (!install_role(std::make_unique<opennova::inmatch::JoinerRole>(joiner_kit_seams())))
		return false;
	if (net_.pump.is_null()) net_.pump.instantiate();
	net_.pump->set_capture_path(net_.capture_pcap_path);
	if (net_.pump->dial(p_host_ip, p_port) != 0) {
		(void)install_offline_role();
		return false;
	}
	// The joiner role reads and writes the dialed pump through the adapter
	// from now on.
	net_.pump_socket = std::make_unique<opennova::CountingDatagramSocket>(
			std::make_unique<UdpPumpDatagramSocket>(net_.pump.ptr()));
	joiner_role_->set_socket(net_.pump_socket.get(), net_.pump->dialed_host());
	// Build the Joiner runtime now so get_joiner_phase reads Idle before the first load (the contract
	// the legacy joiner_session_ held); the role retains the request for a load that rebuilds it.
	runtime_ = &joiner_role_->create_runtime(opennova::to_std(p_player_name),
			p_join_role == static_cast<int>(opennova::inmatch::JoinRole::Spectator)
					? opennova::inmatch::JoinRole::Spectator
					: opennova::inmatch::JoinRole::Player,
			opennova::to_std(p_spectator_password),
			opennova::to_std(p_server_password),
			opennova::to_std(p_join_password));
	install_charattr_challenge_table();
	install_character_join_vars();
	install_join_integrity_profile();
	install_expansion_version_root();
	install_app_id();
	install_join_cd_cookie();
	install_item_catalog();
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

namespace {

// Split an "a.b.c.d:port" endpoint at its last colon. False when there is no
// colon, no host or a port outside 1..65535.
bool split_endpoint(const String &p_endpoint, std::string &r_ip, uint32_t &r_port) {
	const int colon = p_endpoint.rfind(":");
	if (colon <= 0) return false;
	const String port_text = p_endpoint.substr(colon + 1);
	if (!port_text.is_valid_int()) return false;
	const int64_t port = port_text.to_int();
	if (port <= 0 || port > 0xFFFF) return false;
	r_ip = opennova::to_std(p_endpoint.substr(0, colon));
	r_port = static_cast<uint32_t>(port);
	return true;
}

} // namespace

void Simulation::set_join_proxy(const String &p_node, const String &p_relay, int64_t p_cookie) {
	if (joiner_role_ == nullptr) return;
	opennova::inmatch::JoinerRole::JoinProxyOptions options;
	// A malformed part leaves its fields zero, which the role's all-six gate
	// reads as "no proxy" -- the same outcome as a .joi without NI/NP/BK.
	if (split_endpoint(p_node, options.proxy_node_ip, options.proxy_node_port) &&
			split_endpoint(p_relay, options.proxy_relay_ip, options.proxy_relay_port) &&
			p_cookie > 0 && p_cookie <= 0xFFFFFFFFLL) {
		options.proxy_cookie = static_cast<uint32_t>(p_cookie);
	} else {
		options = opennova::inmatch::JoinerRole::JoinProxyOptions{};
	}
	joiner_role_->set_join_proxy(options);
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
	// read-only. F3 mutates only the in-process SP/listen-host player.
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
		if (!p_spectator) respawn_local_player_loadout();
		return true;
	}
	return false;
}

bool Simulation::load_charattr_challenge(
		const Ref<ResourceRoot> &p_resource_root) {
	// Game_Run clears all 0x7C0 bytes before attempting the boot-soft load.
	// Preserve that failure result: missing/empty input is not replaced with a
	// synthetic row, and joining continues with the checksum's inactive zero.
	net_.charattr_challenge_table = {};
	net_.charattr_challenge_loaded = false;
	if (p_resource_root.is_valid() &&
	    p_resource_root->has_file("charattr.def")) {
		const PackedByteArray bytes =
				p_resource_root->read_file("charattr.def");
		if (!bytes.is_empty()) {
			net_.charattr_challenge_loaded =
					opennova::inmatch::parse_charattr_challenge_table(
							bytes.ptr(),
							static_cast<std::size_t>(bytes.size()),
							net_.charattr_challenge_table);
		}
	}
	install_charattr_challenge_table();
	return net_.charattr_challenge_loaded;
}

void Simulation::set_join_world_ready(bool p_ready) {
	if (is_joiner() && runtime_) runtime_->set_world_ready(p_ready);
}

void Simulation::finalize_loaded_model_challenge_snapshot() {
	if (!is_joiner() || !runtime_) return;
	const int64_t count = ObjectData::network_challenge_model_count();
	if (count <= 0) {
		runtime_->set_loaded_model_challenge_snapshot({});
		return;
	}
	// NetPacket_WriteEntityIndexList writes ONE dword per frozen model-def row —
	// a 5x-unrolled loop capped at 50 entries after the [u32 startIndex] header
	// [orig: @0x42d950; the per-entry store @0x42d9f0, the 0x32 cap @0x42da7d].
	// The complete writer/xref audit for this binary found no surviving writer to
	// the source field after model resolution, so each included definition
	// contributes one zero dword. Keep the inmatch seam value-based so a future
	// witnessed writer can supply its actual row without changing paging.
	runtime_->set_loaded_model_challenge_snapshot(
			std::vector<uint32_t>(static_cast<std::size_t>(count), 0u));
}

bool Simulation::poll_join_preload() {
	if (joiner_role_ == nullptr || !runtime_) return false;
	// The pre-mission subset of the joiner role's frame: the same UDP socket and
	// ClientRuntime advance the retail connect exchange, but no World exists yet
	// to tick and the runtime's world-ready gate suppresses the load/spawn drive.
	joiner_role_->poll_preload();
	return true;
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

// The in-match analog of get_join_error: the host closed the session on its own terms
// (the punt record), or an established session went silent past the witnessed connection
// reap window. Either way the disconnect event maps a reason code onto g_MissionExitReason
// — retail EXITS THE MISSION with a reason rather than raising an in-world dialog, so the
// shell's analog is return-to-menu with the reason surfaced.
// [orig: CNapiNetwork_Init @0x4ca4a0 (timeout stores @0x4caa81/@0x4cab54) and
//  CNapiNPConnection_HandleDescriptionPacket @0x621ae0, both ->
//  CNapiNetwork_OnDisconnectedFromServer @0x4c63d0]
String Simulation::get_session_loss_reason() const {
	if (!is_joiner() || !runtime_) return String();
	return opennova::to_gd(runtime_->session_loss_reason());
}

bool Simulation::is_session_lost() const {
	return is_joiner() && runtime_ && runtime_->session_lost();
}

bool Simulation::is_joined_in_match() const {
	return is_joiner() && runtime_ && runtime_->in_match();
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

bool Simulation::is_join_deploy_overlay_active() const {
	// The deploy-map overlay (retail g_DeployScreenActive): armed by the S2C
	// 0x0F game_flags bit0, then host-maintained per frame from the 0x0A flags1
	// bit1. A UI signal only — it never gates the spawn. [orig: the folds
	// @0x42e2f8/@0x42ff82]
	return is_joiner() && runtime_ && runtime_->state().deploy_overlay_active;
}

bool Simulation::take_join_deploy_overlay_open() {
	// The open latch and its clear are ClientState's (client_state.h
	// deploy_overlay_open_latch); this is the typed seam for the shell's frame.
	return is_joiner() && runtime_ && runtime_->state().take_deploy_overlay_open();
}

int Simulation::get_join_assigned_team() const {
	return is_joiner() && runtime_ ? runtime_->assigned_team() : 0;
}

int Simulation::get_class_allow_mask() const {
	if (is_joiner() && runtime_) return runtime_->class_allow_mask();
	if (const opennova::inmatch::NapiNPServerCtx *ctx = host_ctx();
			ctx != nullptr && ctx->is_authority != 0)
		return ctx->config.class_allow_mask;
	return net_.host_session_config.class_allow_mask;
}

const opennova::world::SpawnZoneRegistry &Simulation::deploy_zone_registry() {
	// Built once per load; the zone set is authored (the world stream upserts state,
	// not membership). The joiner role hook resets the flag. [orig: Entity_BuildSpawnZoneList
	// @0x43EAE0 — rebuilt at mission start]
	const uint64_t sync_serial = joiner_role_ != nullptr ? joiner_role_->world_sync_serial() : 0;
	if (kernel_ && (!net_.deploy_zone_registry_built ||
			net_.deploy_zone_registry_sync_serial != sync_serial)) {
		net_.deploy_zone_registry = kernel_->world.zones.build_spawn_zone_list();
		net_.deploy_zone_registry_built = true;
		net_.deploy_zone_registry_sync_serial = sync_serial;
	}
	return net_.deploy_zone_registry;
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
	if (!is_joiner() || !runtime_) return false;
	uint16_t wire = opennova::world::kDeployPickNone;
	if (p_param == 65534) {
		wire = opennova::world::kDeployPickAutoTeam;
	} else if (p_param > 0) {
		const opennova::world::SpawnZoneRegistry &reg = deploy_zone_registry();
		const size_t idx = static_cast<size_t>(p_param - 1);
		if (idx < reg.entries.size()) wire = reg.entries[idx].packed;
	}
	return runtime_->queue_deployment_pick(wire);
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

// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0 — the leave sends a burst of
// 0x46 disconnect packets (SendDisconnectPacket @0x61f2a0) before the key material clears; the
// host's non-timeout teardown fires only on that opcode (Nwu_HandleClientGoodbye @0x624250)]


// (P7 A4: joiner_net_poll / joiner_net_flush deleted — the joiner now runs through the joiner role
//  over an inmatch::ClientRuntime; the legacy JoinerSession path is retired here.)

bool Simulation::admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team) {
	opennova::inmatch::ListenHostState *host = host_state();
	if (!is_host_listening() || host == nullptr) return false;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x,-z,y), the inverse of the present remap (same as spawn_local_player).
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.yaw = static_cast<int16_t>(p_yaw_deg);
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
	// Godot (x, y, z) -> mission (x, -z, y) 16.16, the drain's own convention.
	const int32_t body[3] = { static_cast<int32_t>(p_pos.x * 65536.0f),
		                      static_cast<int32_t>(-p_pos.z * 65536.0f),
		                      static_cast<int32_t>(p_pos.y * 65536.0f) };
	opennova::world::wire_body_slot_sounds(kernel_->world, words, n, capsule_bottom,
			p_type_id, static_cast<uint16_t>(p_character_id),
			static_cast<uint16_t>(p_wire_handle), p_carrier_handle >= 0, body);
}

// The Tab board's header as the shell needs it. Row data no longer rides a
// script Dictionary: HudOverlay pulls the drawn rows natively through
// fill_scoreboard_rows, and the counts here come from the same replication
// projection (replication::scoreboard_header — the accepted-rows-minus-spectators
// players count is the witnessed header arithmetic, retail @0x4231dd).
Ref<ScoreboardHeader> Simulation::get_scoreboard() const {
	Ref<ScoreboardHeader> out;
	out.instantiate();
	if (!runtime_) return out;
	opennova::replication::ClientScoreboardSession v;
	v.header = opennova::replication::scoreboard_header(runtime_->state());
	// The drawer branches on the session game type (retail reads g_GameType
	// @0x423acb); the header's session strings ride along — joiner-decoded,
	// empty on a host until the host sessionvars are plumbed (D-HUD-24).
	v.game_type = runtime_->game_type();
	v.server_name = runtime_->server_name();
	v.mission_name = runtime_->mission_name();
	out->assign(v);
	return out;
}

bool Simulation::fill_scoreboard(opennova::hud::HudScoreboardState &r_state) const {
	opennova::inmatch::scoreboard_feed(role_view(), r_state);
	return runtime_ != nullptr;
}
