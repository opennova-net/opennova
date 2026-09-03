// Simulation — the net roles (ADR 0009/0011/0012): per-load listen-host
// bring-up + host pump, the LAN joiner pump family + wire proxies/events, the
// host session config FFI, and the joiner preload/session API.
#include "simulation/simulation_internal.h"
#include "network/udp_pump_datagram_socket.h"
#include "simulation/hud_view_records.h"
#include "simulation/deploy_rows.h" // the DEATH screen's zone / list rows
#include "hud/feed_row.h" // the typed message-feed row (ADR 0040 B3)
#include "object/character_join_profile.h" // the two-side character selection record
#include "network/host_session_options.h" // the hosted-session request record

#include <cmath>
#include <cstring>

#include <runtime/inmatch/server_initial_state.h> // install_mission_location_names
#include <runtime/inmatch/server_spawn.h> // Server_SetPlayerSpectator
#include <runtime/inmatch/session_status.h>
#include <runtime/terrain_query/surface_tiles.h> // surface_tiles_from_til_bytes (D-SND-15)
#include <formats/threedi/threedi_panm_pose.h> // the native PANM liveness gate (S3, ADR 0028)
#include <net/npwire/ingame_decode.h> // kRoundEventFlag* (the fire-mode byte)
#include <net/npwire/ingame_message_id.h>
#include <runtime/hud/feed_format.h> // the witnessed feed line/color policy
#include <runtime/replication/client_scoreboard_view.h> // the Tab board's draw-time projection
#include <runtime/world/wire_body_sound.h> // the wire-fed remote body's footstep/foley consume
#include <net/npwire/net_ports.h> // lan_host_bind_ports (the D-NET-210 bind scan)
#include <base/vfs/vfs.h> // vfs_expansion_version_checksum (the D-NET-166 JOIN CRC)
#include <net/npwire/wire_handle.h>  // pool()/kPoolItem/kInvalid (the wire handle home)
#include <runtime/world/infantry.h>      // kAnimStanceFlag* (the witnessed stance bits)
#include <runtime/world/spawn_select.h>  // kDeployPickNone/AutoTeam (C2S 0x2C sentinels)
#include <runtime/world/vehicle_motor.h> // carrier_pose_fixed (the deck-ride pose reader)
#include <formats/rtxt/rtxt.h>
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


// P7: per-load host bring-up — the faithful §5.0 mode-3 in-process listen server
// [orig: SinglePlayer_StartMission @0x561af0], mirroring apps/nw_server/main.cpp. The host's own
// player AUTO-spawns through the real pipeline (Server_ProcessPendingPlayerSpawns ->
// resolve_player_spawn_pose marker chain), and its own loopback client renders the per-frame 0x0A.
void Simulation::bringup_host_runtime() {
	namespace inmatch = opennova::inmatch;
	// ctx_.mission (read by the §5.1 0x0B BMS-header burst for LAN joiners)
	// points straight at the kernel's adopted document, which outlives the match.
	host_loop_.clear();
	// Reload: a fresh host_owner_ drops stale owner peers, while create_session owns the protocol
	// connection-table reset at this new-match boundary. No live configuration mutator can clear
	// admitted peers mid-match. serve_and_play: host_session_pump must NOT discard the host's own loopback 0x0A — we
	// fold it into ClientState (runtime_) to render the host's own view.
	// Serve-and-play (default) vs dedicated. Standalone SP is ALWAYS serve-and-play (it renders the
	// host's own player); isolated test/tooling MissionRoot instantiations keep that default too.
	// ONED has no live editor-preview branch: MainGame/GameWorld is its sole live mission runtime
	// (ADR 0025). A LAN host honors the UI server-type (host_serve_and_play_, from
	// configure_host_session). A dedicated host (serve_and_play=false) skips the own-player spawn +
	// the local view below and lets host_session_pump discard the host loopback (step 5) — mirroring
	// start_host_session's gating [orig: SinglePlayer_StartMission @0x561af0].
	const bool serve_and_play = host_listen_ ? host_serve_and_play_ : true;
	inmatch::GameConfig host_config;
	if (host_listen_) {
		host_config = host_session_config_; // mission/player/spawn + game_type/mp_attributes from the UI
		if (host_config.server_name.empty()) host_config.server_name = "OpenNova LAN Host";
		host_config.max_players = host_max_players_; // the UI player cap (configure_host_session clamped 1..65)
	} else {
		host_config.server_name = "SINGLEPLAYERGAME";
		host_config.max_players = 1;
		host_config.game_type = mission_game_type();
	}
	inmatch::HostBringup bringup;
	bringup.host_cfg.config = host_config;
	bringup.host_cfg.socket_mode = host_listen_ ? inmatch::SocketMode::Lan : inmatch::SocketMode::Socketless;
	bringup.host_cfg.serve_and_play = serve_and_play;
	if (local_character_vars_set_) {
		bringup.host_cfg.local_character_vars = local_character_vars_;
	}
	bringup.terrain_til_data = terrain_til_data_; // S2C 0x45 terrain-tile load source (empty => skipped, §5.37)
	bringup.mission_text_loaded = mission_text_loaded_;
	bringup.mission_briefing3 = mission_briefing3_;
	bringup.mission_briefing2 = mission_briefing2_;
	bringup.mission_location_texts = mission_location_texts_;
	host_role_.bring_up(bringup);
	runtime_ = host_role_.client_runtime();
}


// P7/A5: the per-frame host owner loop is now a THIN delegation to the shared core host_session_pump
// (engine/runtime/inmatch) — the SAME loop apps/nw_server runs, so the headless server and the Godot binding can no
// longer drift. Simulation supplies the socket (a UdpPump adapter; SP passes a null pump and the
// loop's socket legs go inert) and folds the host's own loopback 0x0A into ClientState for the present
// pass (serve_and_play: host_session_pump skips the loopback discard so we can read it here).
// The host frame's dispatch_event + admit_peer were promoted into engine/runtime/inmatch (inmatch::dispatch_event /
// inmatch::admit_peer over host_owner_, driven by host_session_pump) — the SAME code apps/nw_server runs, so
// the Godot binding and the headless server can no longer drift.

Array Simulation::get_streamed_placement_records() const {
	Array out;
	if (!joiner_ || !kernel_) return out;
	// The placed identity and the record-space fields read straight off the
	// registry rows the materializer stamped (a BMS entity record's shape, the
	// same batched/placed path the host's own statics take).
	for (const opennova::world::Entity *row :
			joiner_role_.materializer().placed_rows(kernel_->world)) {
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
	if (!joiner_) return out;
	for (const int32_t id : joiner_role_.materializer().take_retired_placement_ids())
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
	if (!is_joiner_network_diagnostics_enabled() || !runtime_) return;
	String local_state = " L=none";
	if (joiner_role_.local_spawned() && kernel_) {
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
			joiner_role_.freeze_suspected()
					? vformat(" *** REPLICATION FROZEN %ds ***",
							  static_cast<int64_t>(joiner_role_.flat_seconds()))
					: String()));
}

void Simulation::enable_listen_server(bool p_enable) {
	listen_server_ = p_enable;
	// P7: the SP listen server now rides the npruntime in-match runtime (ctx_ / host_loop_ /
	// runtime_), stood up per-load in bringup_host_runtime — there is no net ISystem and
	// no legacy loopback seam here. (The LAN host still builds the legacy seam in enable_host_listen
	// until A3; a sim is SP listen XOR LAN host XOR joiner.)
}

void Simulation::set_terrain_til_data(const PackedByteArray &p_til_bytes) {
	terrain_til_data_.assign(p_til_bytes.ptr(), p_til_bytes.ptr() + p_til_bytes.size());
	// The same bytes feed the sim's placed-tile surface array (D-SND-15);
	// the fold and its witness live engine-side (terrain_query
	// surface_tiles_from_til_bytes).
	surface_tiles_ =
			opennova::terrain::surface_tiles_from_til_bytes(terrain_til_data_);
	apply_terrain_to_ai();
}

void Simulation::set_mission_text_data(const PackedByteArray &p_rtxt_bytes) {
	mission_text_loaded_ = false;
	mission_briefing3_.clear();
	mission_briefing2_.clear();
	mission_location_texts_.clear();
	mission_people_names_.clear();
	if (p_rtxt_bytes.is_empty()) return;

	opennova::rtxt::File table;
	std::string error;
	if (!opennova::rtxt::parse(p_rtxt_bytes.ptr(),
	                           static_cast<std::size_t>(p_rtxt_bytes.size()),
	                           table, error)) {
		UtilityFunctions::push_warning(String("Simulation: mission text RTXT rejected: ") +
		                               String::utf8(error.c_str()));
		return;
	}

	// Preserve the table's raw cp1252 bytes. Retail uses an empty briefing2 as
	// the signal to fall back to briefing [orig: @0x506649..0x506660].
	if (const opennova::rtxt::Entry *e = table.find_in_section("info", "briefing3"))
		mission_briefing3_ = e->text;
	if (const opennova::rtxt::Entry *e = table.find_in_section("info", "briefing2"))
		mission_briefing2_ = e->text;
	if (mission_briefing2_.empty()) {
		if (const opennova::rtxt::Entry *e = table.find_in_section("info", "briefing"))
			mission_briefing2_ = e->text;
	}

	// The numeric-key section harvests, raw cp1252 values with only the ASCII
	// section/key interpreted: [Locations] LOCATION%03i (type-2044 markers by
	// one-based spawn order, the S2C 0x0F deploy-map labels) and [PeopleNames]
	// STRNAME%03i (the D-HUD-20 authored entity display names promote resolves
	// from each record's name_index — the witnessed resolve is cited at the
	// promote.cpp port site).
	const auto fold = [](char c) {
		return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	};
	const auto harvest_indexed = [&](const char *section_lc,
			std::size_t section_len, const char *prefix_lc,
			std::size_t prefix_len,
			std::unordered_map<int32_t, std::string> &out_map) {
		for (std::size_t section_index = 0;
		     section_index < table.sections.size(); ++section_index) {
			const std::string &section_name = table.sections[section_index].name;
			if (section_name.size() != section_len) continue;
			bool is_match = true;
			for (std::size_t i = 0; i < section_len; ++i) {
				if (fold(section_name[i]) != section_lc[i]) {
					is_match = false;
					break;
				}
			}
			if (!is_match) continue;

			for (const opennova::rtxt::Entry *entry :
			     table.get_section_entries(static_cast<uint32_t>(section_index))) {
				if (entry == nullptr || entry->key.size() <= prefix_len) continue;
				bool valid = true;
				for (std::size_t i = 0; i < prefix_len; ++i) {
					if (fold(entry->key[i]) != prefix_lc[i]) {
						valid = false;
						break;
					}
				}
				int32_t index = 0;
				for (std::size_t i = prefix_len; valid && i < entry->key.size();
				     ++i) {
					const char digit = entry->key[i];
					if (digit < '0' || digit > '9' || index > 214748364) {
						valid = false;
						break;
					}
					index = index * 10 + (digit - '0');
				}
				if (valid) out_map.emplace(index, entry->text);
			}
			break;
		}
	};
	harvest_indexed("locations", 9, "location", 8, mission_location_texts_);
	harvest_indexed("peoplenames", 11, "strname", 7, mission_people_names_);
	mission_text_loaded_ = true;
}

bool Simulation::set_score_config_data(const PackedByteArray &p_score_ini_bytes) {
	if (p_score_ini_bytes.is_empty()) return false;
	const std::string text(
			reinterpret_cast<const char *>(p_score_ini_bytes.ptr()),
			static_cast<std::size_t>(p_score_ini_bytes.size()));
	return opennova::inmatch::load_session_score_config(host_session_config_, text);
}

bool Simulation::enable_host_listen(int p_port) {
	listen_server_ = true;
	if (pump_.is_null()) pump_.instantiate();
	pump_->set_capture_path(capture_pcap_path_);
	// The LAN host scans the retail port range from the requested port
	// (D-NET-210; the sequence policy is npwire net_ports.h
	// lan_host_bind_ports). Port 0 keeps the OS-assigned bind — the dev/test
	// seam retail has no analog for.
	bool bound = false;
	if (p_port <= 0) {
		bound = pump_->bind_listen(0) == 0;
	} else {
		for (const uint16_t port : opennova::lan_host_bind_ports(
					 static_cast<uint32_t>(p_port))) {
			if (pump_->bind_listen(port) == 0) {
				bound = true;
				break;
			}
		}
	}
	if (!bound) {
		host_listen_ = false;
		host_role_.set_socket(nullptr);
		pump_socket_.reset();
		return false;
	}
	host_listen_ = true;
	// The host role reads and writes the bound pump through the adapter from
	// now on (the SP/test host never installs one and stays socketless).
	pump_socket_ = std::make_unique<UdpPumpDatagramSocket>(pump_.ptr());
	host_role_.set_socket(pump_socket_.get());
	if (kernel_) {
		kernel_->world.rules.projectile_authority = true;
		kernel_->world.rules.mp_session = true;
	}
	// P7: the LAN host rides the npruntime runtime (ctx_ over a real UDP socket), stood up per-load in
	// bringup_host_runtime with SocketMode::Lan. UdpPump owns the socket; all protocol/crypto/
	// framing stays in libs (ADR 0010). host_session_config_ keeps the GDScript-facing session options
	// (the Dictionary getter + the §5.1 reactive-reply config consumed by create_session).
	// The port actually bound (the scan may have stepped past the requested
	// one) is what the session advertises.
	host_bind_port_ = static_cast<uint16_t>(pump_->local_port());
	return true;
}

int Simulation::get_host_listen_port() const {
	return (host_listen_ && pump_.is_valid()) ? pump_->local_port() : 0;
}

int Simulation::get_host_peer_count() const {
	// Count the type-1 (remote-joiner) connections in the npruntime table. The host's own type-2
	// loopback is excluded; a pre-Hello garbage datagram registers no node (handle_server_datagram
	// drops bad envelopes), so it stays 0 until a real JointOperations peer handshakes.
	int n = 0;
	for (const opennova::inmatch::NapiNPConnection &c : ctx_.np_protocol.connection_list) {
		if (c.type == 1) ++n;
	}
	return n;
}

void Simulation::configure_host_session(const Ref<HostSessionOptions> &p_options) {
	if (p_options.is_null()) return;
	const opennova::inmatch::GameConfig &in = p_options->config();
	// Start from the live config so the sim-owned fields (mission header blob,
	// score tables, PCID) survive; every user-facing field lands from the record.
	opennova::inmatch::GameConfig config = host_session_config_;
	host_bind_port_ = static_cast<uint16_t>(std::clamp(p_options->get_bind_port(), 0, 0xFFFF));
	config.server_name = in.server_name;
	config.mission_name = in.mission_name;
	config.mission_file = in.mission_file;
	config.custom_text = in.custom_text;
	config.player_name = in.player_name;
	config.expansion = in.expansion;
	config.spectator_password = in.spectator_password;
	config.spectator_slots = std::max(in.spectator_slots, -1);
	// D-NET-166: the host's g_expansion_checksum analog. When the caller names
	// its install root, compute the CRC of the loose
	// expansion/<name>/version.txt so the join gate can run retail's compare
	// (the witnessed producer/gate live in vfs_expansion_version_checksum and
	// validates_join_request).
	if (!p_options->get_game_root().is_empty()) {
		config.expansion_version_checksum =
				opennova::vfs_expansion_version_checksum(
						std::string(p_options->get_game_root().utf8().get_data()),
						config.expansion);
	}
	{
		const String requested = p_options->get_integrity_profile().strip_edges();
		const std::string id(requested.utf8().get_data());
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
	config.respawn_time = in.respawn_time;
	config.time_limit_minutes = in.time_limit_minutes;
	config.replay_enabled = in.replay_enabled;
	config.max_team_lives = in.max_team_lives;
	config.score_limit = in.score_limit;
	config.max_score = in.max_score;
	config.koth_delta = in.koth_delta;
	config.flag_return_ticks = in.flag_return_ticks;
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
	// [orig: g_entity_send_budget @0xC8FC50].
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
	config.spawn_x = in.spawn_x;
	config.spawn_y = in.spawn_y;
	config.spawn_z = in.spawn_z;
	config.spawn_names = in.spawn_names;
	// Server type + player cap (UI host config): serve_and_play gates the host's own-player spawn +
	// loopback fold at bring-up; max_players is the lobby-advertised cap, clamped to the witnessed 1..65.
	host_serve_and_play_ = p_options->get_serve_and_play();
	host_max_players_ = static_cast<uint32_t>(std::clamp(p_options->get_max_players(), 1,
			static_cast<int>(opennova::inmatch::kMaxPlayersCap)));
	host_session_config_ = std::move(config);
	if (kernel_ && host_listen_) {
		kernel_->world.rules.fat_bullets = host_session_config_.fat_bullets;
		kernel_->world.rules.one_shot_kill = host_session_config_.one_shot_kill;
	}
}

Ref<HostSessionOptions> Simulation::get_host_session_config() const {
	Ref<HostSessionOptions> out;
	out.instantiate();
	out->assign_config(host_session_config_);
	out->set_bind_port(host_bind_port_);
	out->set_max_players(static_cast<int>(host_max_players_));
	out->set_serve_and_play(host_serve_and_play_);
	return out;
}

int Simulation::get_mission_header_size() const {
	return static_cast<int>(host_session_config_.mission_header_blob.size());
}

void Simulation::set_join_character_profile(const Ref<CharacterJoinProfile> &p_profile) {
	if (p_profile.is_null()) return;
	join_character_vars_ = opennova::npruntime::character_join_vars(p_profile->value());
	join_character_vars_set_ = true;
	install_character_join_vars();
}

void Simulation::set_local_character_profile(const Ref<CharacterJoinProfile> &p_profile) {
	if (p_profile.is_null()) return;
	local_character_vars_ = opennova::npruntime::character_join_vars(p_profile->value());
	local_character_vars_set_ = true;
}

bool Simulation::set_join_integrity_profile(const String &p_profile_id) {
	const std::string id = std::string(p_profile_id.strip_edges().utf8().get_data());
	if (id.empty()) {
		join_integrity_profile_id_.clear();
		install_join_integrity_profile();
		return true;
	}
	if (opennova::inmatch::find_integrity_challenge_profile(id) == nullptr) {
		join_integrity_profile_id_.clear();
		install_join_integrity_profile();
		return false;
	}
	join_integrity_profile_id_ = id;
	install_join_integrity_profile();
	return true;
}

void Simulation::set_app_id(const String &p_token) {
	// The .joi-recovered game-session BT join token a NovaWorld host validates
	// (reject code 9). Empty/"0" is the LAN default. Applied to the live joiner
	// runtime immediately and re-applied on each (re)load via install_app_id.
	app_id_ = std::string(p_token.strip_edges().utf8().get_data());
	if (app_id_.empty()) app_id_ = "0";
	install_app_id();
}

void Simulation::set_join_cd_cookie(const PackedByteArray &p_cookie) {
	// The CD identity cookie (packed PUB* blob) for the C2S 0x00 JOIN. Retained
	// and re-applied to the joiner runtime on each (re)load via install_join_cd_cookie.
	join_cd_cookie_.assign(p_cookie.ptr(), p_cookie.ptr() + p_cookie.size());
	install_join_cd_cookie();
}

void Simulation::set_join_expansion_version_root(const String &p_game_root) {
	// D-NET-166: the JOIN VERSIONCRCSTRING checksum source. The runtime CRCs
	// the loose expansion/<SUS2>/version.txt under this root at JOIN-build
	// time (see JoinerConnection::set_expansion_version_root).
	join_expansion_version_root_ = std::string(p_game_root.utf8().get_data());
	install_expansion_version_root();
}

// ---- co-op LAN joiner (D.2) -------------------------------------------------

bool Simulation::enable_join(const String &p_host_ip, int p_port,
		const String &p_player_name, int p_join_role,
		const String &p_spectator_password) {
	// P7: the joiner is a non-authority inmatch::ClientRuntime (Joiner role) built per-load by the boot's role hook;
	// it owns the connect-leg state machine + the S2C->ClientState fold internally. Here we only dial
	// the socket + store the player name (the ClientAuth.NA the host echoes for the name-match). Leave
	// listen_server_ false (the present gate adds || joiner_); a sim is host XOR joiner.
	// Validate the lifecycle transition before opening a socket. Re-dialing a
	// live mission is rejected without partially replacing its transport.
	const opennova::inmatch::TransitionResult role = session_.configure_role(joiner_role_);
	if (role.code != opennova::inmatch::TransitionCode::Applied &&
			role.code != opennova::inmatch::TransitionCode::NoOp) {
		return false;
	}
	if (pump_.is_null()) pump_.instantiate();
	pump_->set_capture_path(capture_pcap_path_);
	if (pump_->dial(p_host_ip, p_port) != 0) {
		joiner_ = false;
		return false;
	}
	// The joiner role reads and writes the dialed pump through the adapter
	// from now on.
	pump_socket_ = std::make_unique<UdpPumpDatagramSocket>(pump_.ptr());
	joiner_role_.set_socket(pump_socket_.get(), pump_->dialed_host());
	joiner_player_name_ = std::string(p_player_name.utf8().get_data());
	join_role_ = p_join_role == static_cast<int>(
			opennova::inmatch::JoinRole::Spectator)
			? opennova::inmatch::JoinRole::Spectator
			: opennova::inmatch::JoinRole::Player;
	join_spectator_password_ =
			std::string(p_spectator_password.utf8().get_data());
	// Build the Joiner runtime now so get_joiner_phase reads Idle before the first load (the contract
	// the legacy joiner_session_ held); each (re)load's role hook rebuilds it fresh.
	runtime_ = &joiner_role_.create_runtime(joiner_player_name_, join_role_, join_spectator_password_);
	install_charattr_challenge_table();
	install_character_join_vars();
	install_join_integrity_profile();
	install_expansion_version_root();
	install_app_id();
	install_join_cd_cookie();
	install_item_class_resolver();
	joiner_ = true;
	if (kernel_) {
		kernel_->world.rules.projectile_authority = false;
		kernel_->world.rules.mp_session = true;
	}
	joiner_role_.reset_for_join();
	joiner_applied_loadout_revision_ = 0;
	if (!session_.begin_connect().applied()) {
		joiner_ = false;
		return false;
	}
	return true;
}

bool Simulation::is_local_spectator() const {
	if (joiner_) return runtime_ != nullptr && runtime_->is_spectator();
	for (const opennova::inmatch::NapiNPConnection &connection :
			ctx_.np_protocol.connection_list) {
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
	if (joiner_ || kernel_ == nullptr || !ctx_.is_authority) return false;
	for (opennova::inmatch::NapiNPConnection &connection :
			ctx_.np_protocol.connection_list) {
		if (connection.type !=
				opennova::inmatch::NapiNPConnection::kTypeClientSide) {
			continue;
		}
		if (!opennova::inmatch::Server_SetPlayerSpectator(
					ctx_, connection, kernel_->world, p_spectator)) {
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
	charattr_challenge_table_ = {};
	charattr_challenge_loaded_ = false;
	if (p_resource_root.is_valid() &&
	    p_resource_root->has_file("charattr.def")) {
		const PackedByteArray bytes =
				p_resource_root->read_file("charattr.def");
		if (!bytes.is_empty()) {
			charattr_challenge_loaded_ =
					opennova::inmatch::parse_charattr_challenge_table(
							bytes.ptr(),
							static_cast<std::size_t>(bytes.size()),
							charattr_challenge_table_);
		}
	}
	install_charattr_challenge_table();
	return charattr_challenge_loaded_;
}

void Simulation::set_join_world_ready(bool p_ready) {
	if (joiner_ && runtime_) runtime_->set_world_ready(p_ready);
}

void Simulation::finalize_loaded_model_challenge_snapshot() {
	if (!joiner_ || !runtime_) return;
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
	// contributes one zero dword. Keep the npruntime seam value-based so a future
	// witnessed writer can supply its actual row without changing paging.
	runtime_->set_loaded_model_challenge_snapshot(
			std::vector<uint32_t>(static_cast<std::size_t>(count), 0u));
}

bool Simulation::poll_join_preload() {
	if (!joiner_ || !runtime_) return false;
	// The pre-mission subset of the joiner role's frame: the same UDP socket and
	// ClientRuntime advance the retail connect exchange, but no World exists yet
	// to tick and the runtime's world-ready gate suppresses the load/spawn drive.
	joiner_role_.poll_preload();
	return true;
}

bool Simulation::is_join_preload_ready() const {
	return joiner_ && runtime_ && runtime_->preload_ready();
}

String Simulation::get_join_admission_stage() const {
	if (!joiner_ || !runtime_) {
		return String();
	}
	return String(runtime_->admission_stage_name());
}

bool Simulation::has_join_mission() const {
	return joiner_ && runtime_ && runtime_->mission_known();
}

// String::utf8, not the Latin-1 const char* constructor: the LAN browser rows
// decode the same wire fields as UTF-8, and both presentations of one host's
// metadata must agree byte-for-byte.
String Simulation::get_join_server_name() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->server_name().c_str()) : String();
}

String Simulation::get_join_mission_name() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->mission_name().c_str()) : String();
}

String Simulation::get_join_mission_file() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->map_file().c_str()) : String();
}

PackedByteArray Simulation::get_join_mission_header() const {
	PackedByteArray out;
	if (!joiner_ || !runtime_ || !runtime_->has_mission_header()) return out;
	const std::vector<uint8_t> &bytes = runtime_->mission_header_bytes();
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	return out;
}

int64_t Simulation::get_join_terrain_til_state() const {
	if (!joiner_ || !runtime_) return JOIN_TERRAIN_TIL_ABSENT;
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
	if (!joiner_ || !runtime_ || !runtime_->has_terrain_til()) return out;
	const std::vector<uint8_t> &bytes = runtime_->terrain_til_bytes();
	out.resize(static_cast<int64_t>(bytes.size()));
	if (!bytes.empty()) std::memcpy(out.ptrw(), bytes.data(), bytes.size());
	return out;
}

String Simulation::get_join_expansion() const {
	return (joiner_ && runtime_) ? String::utf8(runtime_->expansion().c_str()) : String();
}

int64_t Simulation::get_join_game_type() const {
	return (joiner_ && runtime_) ? static_cast<int64_t>(runtime_->game_type()) : -1;
}

String Simulation::get_join_error() const {
	return (joiner_ && runtime_) ? String(runtime_->last_error().c_str()) : String();
}

// The in-match analog of get_join_error: the host closed the session on its own terms
// (the punt record), or an established session went silent past the witnessed connection
// reap window. Either way the disconnect event maps a reason code onto g_mission_exit_reason
// — retail EXITS THE MISSION with a reason rather than raising an in-world dialog, so the
// shell's analog is return-to-menu with the reason surfaced.
// [orig: CNapiNetwork_Init @0x4ca4a0 (timeout stores @0x4caa81/@0x4cab54) and
//  CNapiNPConnection_HandleDescriptionPacket @0x621ae0, both ->
//  CNapiNetwork_OnDisconnectedFromServer @0x4c63d0]
String Simulation::get_session_loss_reason() const {
	if (!joiner_ || !runtime_) return String();
	return String::utf8(runtime_->session_loss_reason().c_str());
}

bool Simulation::is_session_lost() const {
	return joiner_ && runtime_ && runtime_->session_lost();
}

bool Simulation::is_joined_in_match() const {
	return joiner_ && runtime_ && runtime_->in_match();
}

bool Simulation::is_join_initial_admission_complete() const {
	return joiner_ && runtime_ && runtime_->initial_admission_complete();
}

bool Simulation::is_joiner_network_diagnostics_enabled() const {
	return joiner_net_diagnostics_;
}

void Simulation::set_joiner_network_diagnostics_enabled(bool p_enabled) {
	joiner_net_diagnostics_ = p_enabled;
}

void Simulation::set_capture_pcap_path(const String &p_path) {
	capture_pcap_path_ = p_path;
}

Dictionary Simulation::get_joiner_network_diagnostics() const {
	Dictionary out;
	out["enabled"] = is_joiner_network_diagnostics_enabled();
	out["frontier_seq"] = static_cast<int64_t>(
			runtime_ ? runtime_->inbound_frontier_seq() : 0u);
	out["outbound_seq"] = static_cast<int64_t>(
			runtime_ ? runtime_->outbound_seq() : 0u);
	out["records_applied"] = static_cast<int64_t>(
			runtime_ ? runtime_->state().compact_records_applied : 0u);
	out["gap_depth"] = static_cast<int64_t>(
			runtime_ ? runtime_->inbound_gap_depth() : 0u);
	out["retained_outbound"] = static_cast<int64_t>(
			runtime_ ? runtime_->retained_outbound_depth() : 0u);
	out["flat_seconds"] = joiner_role_.flat_seconds();
	out["freeze_suspected"] = joiner_role_.freeze_suspected();
	out["in_match"] = runtime_ && runtime_->in_match();
	out["deployed"] = runtime_ && runtime_->is_deployed();
	if (runtime_) {
		out["stage"] = String(runtime_->admission_stage_name());
		const opennova::inmatch::JoinerConnection::ChallengeDiagnostics challenges =
				runtime_->challenge_diagnostics();
		Dictionary crc;
		crc["entity_checksum_seen"] = static_cast<int64_t>(challenges.entity_checksum_seen);
		crc["entity_checksum_answered"] =
				static_cast<int64_t>(challenges.entity_checksum_answered);
		crc["loadout_crc_seen"] = static_cast<int64_t>(challenges.loadout_crc_seen);
		crc["loadout_crc_answered"] = static_cast<int64_t>(challenges.loadout_crc_answered);
		crc["charattr_seen"] = static_cast<int64_t>(challenges.charattr_seen);
		crc["charattr_row_missing"] = static_cast<int64_t>(challenges.charattr_row_missing);
		crc["property_clears"] = static_cast<int64_t>(challenges.property_clears);
		out["challenges"] = crc;
		const opennova::inmatch::JoinerConnection::JoinRejectRecord reject =
				runtime_->last_join_reject();
		if (reject.set) {
			Dictionary r;
			r["jfc"] = static_cast<int64_t>(reject.jfc);
			r["jfp"] = static_cast<int64_t>(reject.jfp);
			r["jfs"] = String::utf8(reject.jfs.c_str());
			out["last_reject"] = r;
		}
		if (runtime_->has_disconnect_event()) {
			const opennova::DisconnectEvent event = runtime_->last_disconnect_event();
			Dictionary d;
			d["dc"] = static_cast<int64_t>(event.dc);
			d["dpc"] = static_cast<int64_t>(event.dpc);
			d["ddstr"] = String::utf8(event.ddstr.c_str());
			d["dstr"] = String::utf8(event.dstr.c_str());
			out["last_disconnect"] = d;
		}
	}
	return out;
}

bool Simulation::is_join_deploy_pick_pending() const {
	return joiner_ && runtime_ && runtime_->deployment_pick_pending();
}

bool Simulation::is_join_deploy_overlay_active() const {
	// The deploy-map overlay (retail g_deploy_screen_active): armed by the S2C
	// 0x0F game_flags bit0, then host-maintained per frame from the 0x0A flags1
	// bit1. A UI signal only — it never gates the spawn. [orig: the folds
	// @0x42e2f8/@0x42ff82]
	return joiner_ && runtime_ && runtime_->state().deploy_overlay_active;
}

bool Simulation::take_join_deploy_overlay_open() {
	// The open latch and its clear are ClientState's (client_state.h
	// deploy_overlay_open_latch); this is the typed seam for the shell's frame.
	return joiner_ && runtime_ && runtime_->state().take_deploy_overlay_open();
}

int Simulation::get_join_assigned_team() const {
	return joiner_ && runtime_ ? runtime_->assigned_team() : 0;
}

int Simulation::get_class_allow_mask() const {
	if (joiner_ && runtime_) return runtime_->class_allow_mask();
	if (ctx_.is_authority != 0) return ctx_.config.class_allow_mask;
	return host_session_config_.class_allow_mask;
}

const opennova::world::SpawnZoneRegistry &Simulation::deploy_zone_registry() {
	// Built once per load; the zone set is authored (the world stream upserts state,
	// not membership). The joiner role hook resets the flag. [orig: Entity_BuildSpawnZoneList
	// @0x43EAE0 — rebuilt at mission start]
	if (kernel_ && (!deploy_zone_registry_built_ ||
			deploy_zone_registry_sync_serial_ != joiner_role_.world_sync_serial())) {
		deploy_zone_registry_ = kernel_->world.zones.build_spawn_zone_list();
		deploy_zone_registry_built_ = true;
		deploy_zone_registry_sync_serial_ = joiner_role_.world_sync_serial();
	}
	return deploy_zone_registry_;
}

std::vector<opennova::world::DeployZoneRow> Simulation::deploy_zone_rows() {
	// The DEATH screen's zone rows [orig: UI_UpdateDeathScreenContent @0x5536a0 —
	// def present, team match, SECURED (a numbered zone lists only at full control:
	// the zone-timer EntryById[9] >= [10] gate), attrib 0x40000; letter = 'A' +
	// registry index, name = WPNames/STRWPNAME%03d(index+1)]. The local BMS owns
	// membership/letter identity; live S2C 0x6F/0x53 owns team + control. Every
	// TEAM zone is emitted with its `secured` verdict: the second (occupant)
	// loop of the populate has no secured gate, so the engine builder decides
	// which rows list and where the occupants land.
	std::vector<opennova::world::DeployZoneRow> rows;
	if (!kernel_ || !joiner_ || !runtime_) return rows;
	const opennova::world::SpawnZoneRegistry &reg = deploy_zone_registry();
	const uint8_t team = runtime_->assigned_team();
	const opennova::replication::ClientState &cs = runtime_->state();
	const uint16_t self_handle = runtime_->has_self_handle() ? runtime_->self_handle() : 0xFFFFu;
	for (size_t i = 0; i < reg.entries.size(); ++i) {
		const opennova::world::Entity *e = kernel_->world.registry.get(reg.entries[i]);
		if (e == nullptr || !e->has_item_def || !e->is_spawn_point) continue;
		uint8_t effective_team = e->team;
		int32_t effective_control = e->zone_control;
		int32_t effective_limit = 0x10000;
		const auto live = runtime_->zone_states().find(e->handle.packed);
		if (live != runtime_->zone_states().end()) {
			// The DEATH list reads the value entry's team and exact value >= limit
			// gate. 0x53 is the separate timed-capture window; retaining an old
			// window after a later 0x6F must not overwrite this ownership channel.
			// [orig: UI_UpdateDeathScreenContent @0x5536a0; §5.49/§5.61]
			if (live->second.has_value) {
				effective_team = live->second.value.mode;
				effective_control = live->second.value.value_s;
				effective_limit = live->second.value.limit_s;
			}
		}
		if (effective_team != team) continue;
		opennova::world::DeployZoneRow row;
		row.index = static_cast<int>(i);
		row.letter = static_cast<char>('A' + static_cast<int>(i));
		row.name_key = vformat("STRWPNAME%03d", static_cast<int>(i) + 1).utf8().get_data();
		row.secured = !(e->zone_number != 0 && effective_control < effective_limit);
		// The 0x6E wave group on this zone: its countdown (entity+548) and the
		// queued members, named through the roster the way retail reads the
		// member entity's Name (the player entity's name IS the roster name)
		// [orig: dword_A85BC4[idx] / unk_A85CC4 @0x553cd0..0x553d8b, see world/deploy_screen_feed.h].
		if (cs.spawn_waves.known) {
			for (const opennova::SpawnWaveGroup &g : cs.spawn_waves.value.groups) {
				if (g.zone_handle != e->handle.packed) continue;
				row.wave_countdown = static_cast<uint16_t>(g.wave_countdown);
				for (uint16_t member : g.members) {
					opennova::world::DeployOccupant o;
					o.handle = member;
					std::string name;
					const opennova::world::EntityHandle mh{member};
					for (const opennova::replication::ClientRosterSlot &slot : cs.roster) {
						if (slot.bound && slot.entity_slot == mh.slot() && mh.pool() == 0) {
							name = slot.name;
							break;
						}
					}
					if (name.empty()) {
						if (const opennova::replication::ClientEntityState *row_state = cs.find(member))
							name = row_state->name;
					}
					o.name = name;
					o.self = member == self_handle;
					row.occupants.push_back(o);
				}
			}
		}
		rows.push_back(row);
	}
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
	if (!joiner_ || !runtime_) return false;
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
	return (joiner_ && runtime_) ? static_cast<int>(runtime_->phase()) : -1;
}

int Simulation::get_joiner_self_handle() const {
	return joiner_ ? static_cast<int>(joiner_role_.self_wire_handle()) : 0;
}

// [orig: CNapiNPConnection_TeardownActiveConnection @0x6253c0 — the leave sends a burst of
// 0x46 disconnect packets (SendDisconnectPacket @0x61f2a0) before the key material clears; the
// host's non-timeout teardown fires only on that opcode (Nwu_HandleClientGoodbye @0x624250)]


// (P7 A4: joiner_net_poll / joiner_net_flush deleted — the joiner now runs through the joiner role
//  over an inmatch::ClientRuntime; the legacy JoinerSession path is retired here.)

bool Simulation::admit_test_remote_peer(Vector3 p_position, float p_yaw_deg, int p_team) {
	if (!host_listen_) return false;
	opennova::world::PlayerSpawn spawn;
	// Godot (x,y,z) -> mission (x,-z,y), the inverse of the present remap (same as spawn_local_player).
	spawn.position = {static_cast<float>(p_position.x), static_cast<float>(-p_position.z),
	                  static_cast<float>(p_position.y)};
	spawn.yaw = static_cast<int16_t>(p_yaw_deg);
	spawn.team = static_cast<uint8_t>(p_team);
	// A synthetic loopback peer; distinct port per call so repeated admits don't alias. Own a transport
	// so the connection is well-formed. The synthetic admit (no handshake) mirrors the post-PeerSpawned
	// state; with the host already at net_id 0xFFF0 the joiner allocates 0xFFF1.
	const opennova::PeerAddr peer{0x0100007Fu, static_cast<uint16_t>(40000 + host_owner_.peers.size())};
	opennova::inmatch::PeerLink &link = host_owner_.peers[peer];
	if (!link.transport) {
		link.transport = std::make_unique<opennova::replication::UdpSessionTransport>(
				opennova::replication::UdpSessionTransport::Role::Host);
	}
	const opennova::world::EntityHandle h =
			opennova::inmatch::admit_synthetic_peer(
					ctx_, kernel_->world, peer, spawn, link.transport.get());
	kernel_->resolve_new_infantry_adm_ids();
	return h.valid();
}


// Drain this frame's folded S2C 0x1E game events into typed feed rows. The
// fold is the engine's (runtime/hud/feed_format.h feed_event_rows); this seam
// only resolves actor names against the decoded roster (a pool-0 INDEX on the
// wire becomes the handle (0<<12)|index) and packs the rows.
TypedArray<FeedRow> Simulation::drain_feed_events() {
	TypedArray<FeedRow> out;
	if (!runtime_) return out;
	opennova::replication::ClientState &cs = runtime_->state();
	const uint16_t self_handle =
			runtime_->has_self_handle() ? runtime_->self_handle() : 0xFFFF;
	const auto name_of = [&cs](uint8_t index) -> std::string {
		const opennova::replication::ClientEntityState *e =
				cs.find(static_cast<uint16_t>(index));
		return e != nullptr ? e->name : std::string();
	};
	std::vector<opennova::hud::FeedEventInput> inputs;
	for (const opennova::replication::ClientGameEvent &ev : runtime_->drain_game_events()) {
		inputs.push_back({ ev.event_type, ev.attacker_index, ev.victim_index,
				ev.aux_index, ev.kind });
	}
	std::vector<opennova::hud::FeedRow> rows;
	opennova::hud::feed_event_rows(inputs.data(), inputs.size(), self_handle,
			opennova::hud::kMpVerboseDefault, name_of, rows);
	for (const opennova::hud::FeedRow &row : rows) {
		Ref<FeedRow> r;
		r.instantiate();
		r->assign(row);
		out.push_back(r);
	}
	return out;
}

String Simulation::format_feed_line(const String &p_template, const String &p_attacker,
		const String &p_victim, const String &p_extra,
		const String &p_bonus_template) const {
	return String::utf8(opennova::hud::feed_format_line(
			p_template.utf8().get_data(), p_attacker.utf8().get_data(),
			p_victim.utf8().get_data(), p_extra.utf8().get_data(),
			p_bonus_template.utf8().get_data())
			                    .c_str());
}

String Simulation::format_feed_camp_line(const String &p_template,
		const String &p_wpname) const {
	return String::utf8(opennova::hud::feed_format_camp_line(
			p_template.utf8().get_data(), p_wpname.utf8().get_data())
			                    .c_str());
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
// fill_scoreboard_rows, and the counts here come from the same netsim
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

bool Simulation::fill_scoreboard_rows(
		std::vector<opennova::hud::ScoreboardEntry> &r_rows) const {
	if (!runtime_) {
		r_rows.clear();
		return false;
	}
	opennova::replication::project_scoreboard(runtime_->state(), r_rows);
	return true;
}

int Simulation::scoreboard_team_count() const {
	if (!runtime_) return 0;
	return static_cast<int>(runtime_->state().scoreboard.team_count);
}
