// The Simulation's net-session shell inputs (ADR 0043 d3/d9): the sockets the
// engine roles pump, the hosted-session request and the host bring-up inputs
// the shell feeds before load, the joiner's retained join inputs (re-installed
// on every ClientRuntime the role rebuilds) and the per-session cursors. The
// session itself, its role and the live ClientRuntime pointer stay on
// Simulation; this is plain data with no behavior.
#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>

#include <net/npwire/idatagram_socket.h>
#include <runtime/inmatch/charattr_challenge.h>  // CharAttrChallengeTable
#include <runtime/inmatch/game_config.h>          // GameConfig
#include <runtime/inmatch/napi_np_connection.h>   // CharacterJoinVars
#include <runtime/world/spawn_select.h>           // SpawnZoneRegistry

#include "network/udp_pump.h"

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
	// every datagram dropped, the role's socket legs inert).
	std::unique_ptr<opennova::IDatagramSocket> pump_socket;
	// The pcap this session's datagrams are recorded to (`--capture-pcap`).
	String capture_pcap_path;

	// --- the hosted session ---------------------------------------------------
	// The ONE consolidated server-state config (ADR 0013): the GDScript-facing
	// session options (the record getter + the §5.1 reactive-reply config
	// consumed by create_session).
	opennova::inmatch::GameConfig host_session_config;
	uint16_t host_bind_port = 64220; // the lobby-advertised bind port (UI only)
	// UI server-type: serve-and-play (default true) spawns + renders the host's
	// own player and folds the host loopback into the live runtime; a DEDICATED
	// host (false) runs the listen server with NO local player and lets
	// host_session_pump discard the loopback (step 5). Mirrors
	// HostConfig.serve_and_play / start_host_session's gating (engine:
	// runtime/inmatch/host_role.cpp).
	bool host_serve_and_play = true;
	// A LAN host requested by enable_host_listen while a mission was LIVE: the
	// session cannot switch roles mid-mission, so the pump is bound at once and
	// the LAN HostRole is installed by ensure_session_role at the next load
	// (the pre-G13 host_listen_ latch; is_host_listening() reads it meanwhile).
	bool lan_host_pending = false;
	uint32_t host_max_players = 16; // the lobby-advertised player cap; clamped host-side to the witnessed 1..65 [orig +0xC0]
	// The mission's raw terrain-tile (.til) file bytes, fed from the Godot shell
	// (which owns the resource root) before load; copied into the host ctx's
	// terrain_til_data at bring-up so the initial-state burst streams the S2C
	// 0x45 terrain-tile load (phase 5). Empty => 0x45 faithfully skipped. [§5.37]
	std::vector<uint8_t> terrain_til_data;
	// MissionText briefing values parsed from the mounted <mission>.bin (or the
	// medmssn.bin fallback) before load. These remain the RTXT's original cp1252
	// bytes so the S2C 0x7E payload is byte-exact for localized text.
	bool mission_text_loaded = false;
	std::string mission_briefing3;
	std::string mission_briefing2;
	// Numeric suffix -> original cp1252 MissionText [Locations] value. The host
	// bring-up resolves these against type-2044 BMS markers in spawn order for
	// the S2C 0x0F deploy-map label block.
	std::unordered_map<int32_t, std::string> mission_location_texts;
	// Numeric suffix -> [PeopleNames] STRNAME%03i value; promote resolves an
	// entity's authored display name (D-HUD-20) from its BMS name_index here
	// (engine: runtime/mission/promote.cpp).
	std::unordered_map<int32_t, std::string> mission_people_names;

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
	// Explicit resource-corpus identity for the retail anti-cheat 0x30/0x31
	// sources. Empty keeps safe silence. Retained across direct-load runtime
	// rebuilds just like the character and charattr profile data.
	std::string join_integrity_profile_id;
	// The install root the joiner's JOIN VERSIONCRCSTRING checksum reads its
	// loose expansion/<name>/version.txt from (D-NET-166). Empty keeps the
	// golden "0". Retained across direct-load runtime rebuilds.
	std::string join_expansion_version_root;
	// The game-session APPID join token (decoded .joi CK) a NovaWorld host
	// validates (reject code 9). "0" is the LAN default. Retained across rebuilds.
	std::string app_id = "0";
	// The CD identity cookie (packed PUB* blob) for the 0x00 JOIN (codes 23/24/25).
	// Empty for LAN. Retained across direct-load runtime rebuilds.
	std::vector<uint8_t> join_cd_cookie;
	// Retail loads this process-scoped table from charattr.def before joining.
	// The byte image stays outside ClientRuntime so a direct mission load can
	// reinstall it when a load rebuilds an as-yet-unstarted joiner.
	opennova::inmatch::CharAttrChallengeTable charattr_challenge_table{};
	bool charattr_challenge_loaded = false;

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
	// The deploy/spawn-zone registry (letters/pick-index space), built lazily
	// per load (engine: runtime/hud/hud_lfp_panel.h), and the joiner role's
	// world-sync serial it was last derived at (a streamed topology change
	// moves it; the registry re-derives).
	opennova::world::SpawnZoneRegistry deploy_zone_registry;
	bool deploy_zone_registry_built = false;
	uint64_t deploy_zone_registry_sync_serial = 0;
};

} // namespace godot
