// The Simulation's net-session shell inputs (ADR 0043 d3/d9): the sockets the
// engine roles pump, the hosted-session request and the host bring-up inputs
// the shell feeds before load, the joiner's retained join inputs (re-installed
// on every ClientRuntime the role rebuilds) and the per-session cursors. The
// session itself, its role and the live ClientRuntime pointer stay on
// Simulation; this is plain data with no behavior.
#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>

#include <net/npwire/counting_datagram_socket.h>
#include <net/npwire/idatagram_socket.h>
#include <runtime/inmatch/charattr_table.h>  // CharAttrTable
#include <runtime/inmatch/game_config.h>          // GameConfig
#include <runtime/inmatch/joiner_connection.h>    // JoinerConnection::DiscoveredSession
#include <runtime/inmatch/mission_rotation.h>     // HostRotation (D-NET-331)
#include <runtime/inmatch/napi_np_connection.h>   // CharacterJoinVars
#include <runtime/inmatch/napi_np_server_ctx.h>   // NetworkType
#include <runtime/mission/mission_text.h>          // MissionText
#include <runtime/world/spawn_select.h>           // SpawnZoneRegistry

#include "network/udp_pump.h"
#include "player/game_config_block.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace godot {

struct SimulationNetState {
	// --- the sockets --------------------------------------------------------
	// The co-op LAN socket (UdpPump): enable_host_listen binds it and installs
	// the LAN host role, enable_join dials it and installs the joiner role. The
	// host role drives the owner loop over it (dispatch_event/admit_peer admit
	// joiners + stream the named dcb-bearing 0x0C); sockets live here, the
	// protocol/crypto in libs (ADR 0010).
	Ref<UdpPump> pump;
	// The pump as the active role's opennova::IDatagramSocket (installed by
	// enable_host_listen / enable_join, so the SP/test host stays socketless:
	// every datagram dropped, the role's socket legs inert), wrapped in the
	// counter the F3 Net window reads (the datagrams pass through unchanged).
	std::unique_ptr<opennova::CountingDatagramSocket> pump_socket;
	// The pcap this session's datagrams are recorded to (`--capture-pcap`).
	String capture_pcap_path;

	// --- the hosted session ---------------------------------------------------
	// The ONE consolidated server-state config (ADR 0013): the GDScript-facing
	// session options (the record getter + the §5.1 reactive-reply config
	// consumed by create_session).
	opennova::inmatch::GameConfig host_session_config;
	// The process's cfg block the shell keeps (game.cfg): every session create
	// takes its mpreset word, single player's and a host's alike, and a
	// ServerCommand's config change lands in it. Null in tools and tests that
	// install none: the session config's own word stands.
	Ref<GameConfigBlock> game_config;
	uint16_t host_bind_port = 64220; // the lobby-advertised bind port (UI only)
	// UI server-type: serve-and-play (default true) spawns + renders the host's
	// own player and folds the host loopback into the live runtime; a DEDICATED
	// host (false) runs the listen server with NO local player and lets
	// host_session_pump discard the loopback (step 5). Mirrors
	// HostConfig.serve_and_play / start_host_session's gating (engine:
	// runtime/inmatch/host_role.cpp).
	bool host_serve_and_play = true;
	// The host's network type (HostSessionOptions::network_type): NovaWorld only
	// for a host registered with the NovaWorld gate; HostConfig::network_type.
	opennova::inmatch::NetworkType host_network_type = opennova::inmatch::NetworkType::Lan;
	// A LAN host requested by enable_host_listen while a mission was LIVE: the
	// session cannot switch roles mid-mission, so the pump is bound at once and
	// the LAN HostRole is installed by ensure_session_role at the next load
	// (the pre-G13 host_listen_ latch; is_host_listening() reads it meanwhile).
	bool lan_host_pending = false;
	uint32_t host_max_players = 16; // the session's player cap after session_player_cap (1..65) [orig game_settings +0xC0]; host_player_slot_limit adds a dedicated host's own slot
	// The mission's raw terrain-tile (.til) file bytes, fed from the Godot shell
	// (which owns the resource root) before load; copied into the host ctx's
	// terrain_til_data at bring-up so the initial-state burst streams the S2C
	// 0x45 terrain-tile load (phase 5). Empty => 0x45 faithfully skipped. [§5.37]
	std::vector<uint8_t> terrain_til_data;
	// The map rotation (D-NET-331, inmatch/mission_rotation.h): the host
	// screen's rows and Switch cells (configure_host_session), the mounted
	// catalog the first host boot seeds the list over, the session-level
	// words the map change reads, and the latch that makes the next boot the
	// map change's (begin_host_map_change -> boot_mission). The words live
	// with this session's simulation: a new session's start fresh, where
	// retail's process globals carry the half toggle and the SETNEXT latch
	// into the next session (D-NET-353).
	std::vector<std::string> rotation_missions;
	std::vector<int32_t> rotation_launch_options;
	std::vector<opennova::mission_catalog::Row> rotation_catalog;
	opennova::inmatch::HostRotation rotation;
	bool map_change_pending = false;
	// The mounted <mission>.bin (or the medmssn.bin fallback) RTXT the shell
	// handed over before load, harvested engine-side
	// (runtime/mission/mission_text.h): the briefing pages the S2C 0x7E payload
	// streams byte-exact, the [Locations] deploy-map labels the host bring-up
	// resolves, the [PeopleNames] display names promote resolves (D-HUD-20).
	opennova::mission::MissionText mission_text;

	// --- the joiner's retained join inputs -----------------------------------
	// The per-second joiner trace switch (`net_joiner_diagnostics`).
	bool joiner_net_diagnostics = false;
	// Retail authenticates with one packed Avatars.def selection for each side.
	// GameWorld resolves the active profile before enable_join; retained here
	// because a direct-loaded join rebuilds ClientRuntime at load.
	opennova::inmatch::CharacterJoinVars join_character_vars{};
	bool join_character_vars_set = false;
	// The listen host's own type-2 connection consumes the same profile shape,
	// installed before its authoritative player spawn.
	opennova::inmatch::CharacterJoinVars local_character_vars{};
	bool local_character_vars_set = false;
	// The install root the joiner's JOIN VERSIONCRCSTRING checksum reads its
	// loose expansion/<name>/version.txt from (D-NET-166). Empty keeps the
	// golden "0". Retained across direct-load runtime rebuilds.
	std::string join_expansion_version_root;
	// The title the session speaks (set_title); null until the shell names one.
	const opennova::TitleIdentity *title = nullptr;
	// The game-session APPID join token (decoded .joi CK) a NovaWorld host
	// validates (reject code 9). "0" is the LAN default. Retained across rebuilds.
	std::string app_id = "0";
	// The joiner's mission-start seam (JoinerConnection::set_mission_start_loading): the
	// shell's load runs after the S2C 0x1A. Retained across rebuilds.
	bool join_mission_start_loading = false;
	// The count of the loaded-model list frozen at this mission's load
	// (finalize_loaded_model_challenge_snapshot): a host's S2C 0x68 cursor wrap.
	uint32_t loaded_model_list_count = 0;
	// The joiner's network type (JoinTarget::network_type; retail's
	// g_NapiNPCtx.transport_mode on the client). Retained across rebuilds.
	opennova::inmatch::NetworkType join_network_type = opennova::inmatch::NetworkType::Lan;
	// The CD identity cookie (packed PUB* blob) for the 0x00 JOIN (codes 23/24/25).
	// Empty for LAN. Retained across direct-load runtime rebuilds.
	std::vector<uint8_t> join_cd_cookie;
	// The browse row's 0x81 record a join from it connects with (no 0x41).
	// Retained across direct-load runtime rebuilds.
	opennova::inmatch::JoinerConnection::DiscoveredSession join_discovered_session;
	// The transport the join goes on with (JoinTarget.transport, D-NET-NEW-R3-3); null
	// = enable_join binds its own over the client port range.
	Ref<UdpPump> join_transport;
	// Retail loads this process-scoped table from charattr.def before joining
	// (a host's is its boot's, kept with its restriction latches). It stays
	// outside ClientRuntime so a direct mission load can reinstall it when a
	// load rebuilds an as-yet-unstarted joiner.
	opennova::inmatch::CharAttrTable charattr_table{};

	// --- per-session cursors ---------------------------------------------------
	// The live environment owner consumes each decoded phase-2 edge once. The
	// ClientState revision is monotonic for one ClientRuntime; fresh runtimes
	// reset this cursor with their other receive-side cursors.
	// The 0x81 score-feedback edge cursor (ClientScoreFeedback::updates).
	uint32_t score_feedback_updates_seen = 0;
	// Last authoritative S2C 0x5A grant installed into the local slot pool.
	// Requests may rebuild optimistically, but only a newer host grant becomes
	// the durable spawn/respawn kit.
	uint64_t joiner_applied_loadout_revision = 0;
	// Last S2C 0x0F ammo-pool image copied into the local inventory (after the
	// grant above rebuilt the slots; engine: world/weapon_inventory.h
	// weapon_inventory_apply_authority_pools carries the witness).
	uint64_t joiner_applied_ammo_pools_revision = 0;
	// The deploy/spawn-zone registry (letters/pick-index space) and its
	// lifetime (engine world::SpawnZoneRegistryCache).
	opennova::world::SpawnZoneRegistryCache deploy_zone_registry;
};

} // namespace godot
