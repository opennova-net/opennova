#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <net/npwire/net_ports.h>

#include "mission/mission_object_placer.h"
#include "network/host_session_options.h"
#include "network/join_target.h"
#include "object/character_join_profile.h"
#include "object/item_database.h"
#include "resource_index/resource_root.h"
#include "simulation/simulation.h"
#include "terrain/terrain_data.h"

namespace godot {

// Typed record for MissionRoot.setup() (ADR 0017/0042): every input the
// composition hands the mission root, replacing the old options Dictionary.
// GameWorld's runtime stage builds one per load and
// SessionDrive::stage_runtime_options stamps the staged net-session request
// onto the same record; isolated tests fill only the fields they exercise. An
// unset reference reads null/empty and means "not provided", exactly as the
// old absent keys did. Ported from mission_setup_options.gd (ADR 0043 slice
// G5) minus the two fields nothing produced (infantry_adm, listen_server).
class MissionSetupOptions : public RefCounted {
	GDCLASS(MissionSetupOptions, RefCounted)

public:
	// The already-connected simulation to adopt: a remote join owns the live
	// socket + NP session before the wire-header world load (SessionDrive
	// surrenders it here). Null = setup creates a fresh Simulation. MissionRoot
	// is the one adopter either way (ADR 0011/0012).
	MusicDirector *get_music_director() const;
	void set_music_director(MusicDirector *director);
	Ref<Simulation> get_simulation() const { return simulation_; }
	void set_simulation(const Ref<Simulation> &p_value) { simulation_ = p_value; }
	// Mission identity for diagnostics and the host session advertisement.
	// setup() normalizes mission_file to a portable basename for
	// get_mission_file(); the host leg advertises the raw value.
	String get_mission_file() const { return mission_file_; }
	void set_mission_file(const String &p_value) { mission_file_ = p_value; }
	String get_mission_name() const { return mission_name_; }
	void set_mission_name(const String &p_value) { mission_name_ = p_value; }
	// Serve-and-play/SP hosts spawn their own player at bring-up; false for
	// diagnostic previews and a DEDICATED serve (ADR 0015 serve mode).
	bool get_playable() const { return playable_; }
	void set_playable(bool p_value) { playable_ = p_value; }

	// --- The typed net-session request (ADR 0017): at most one is non-null
	//     per load. ---
	// Co-op LAN HOST request — the listen server binds a real UDP socket. The
	// host screen's HostSessionConfig projects to this sim-shaped record
	// (HostSessionConfig.to_session_options) at staging; the root resolves
	// game_type_auto and stamps the mission identity on top.
	Ref<HostSessionOptions> get_host_session() const { return host_session_; }
	void set_host_session(const Ref<HostSessionOptions> &p_value) { host_session_ = p_value; }
	// Co-op LAN JOINER dial target — the non-authority client.
	Ref<JoinTarget> get_join_target() const { return join_target_; }
	void set_join_target(const Ref<JoinTarget> &p_value) { join_target_ = p_value; }
	// The host's two per-side character selections projected to the wire
	// (Simulation.set_local_character_profile). Null = keep the sim's shipped
	// defaults.
	Ref<CharacterJoinProfile> get_local_character_profile() const {
		return local_character_profile_;
	}
	void set_local_character_profile(const Ref<CharacterJoinProfile> &p_value) {
		local_character_profile_ = p_value;
	}
	// The joiner's profile-to-wire projection (Simulation.set_join_character_profile).
	// Null = none staged.
	Ref<CharacterJoinProfile> get_join_character_profile() const {
		return join_character_profile_;
	}
	void set_join_character_profile(const Ref<CharacterJoinProfile> &p_value) {
		join_character_profile_ = p_value;
	}
	// Host-session spawn-name list; empty falls back to [mission_name].
	PackedStringArray get_spawn_names() const { return spawn_names_; }
	void set_spawn_names(const PackedStringArray &p_value) { spawn_names_ = p_value; }

	// --- Boot feeds (Simulation.boot_mission, S9 / ADR 0028). ---
	Ref<ResourceRoot> get_resource_root() const { return resource_root_; }
	void set_resource_root(const Ref<ResourceRoot> &p_value) { resource_root_ = p_value; }
	Ref<ItemDatabase> get_item_db() const { return item_db_; }
	void set_item_db(const Ref<ItemDatabase> &p_value) { item_db_ = p_value; }
	Ref<TerrainData> get_terrain() const { return terrain_; }
	void set_terrain(const Ref<TerrainData> &p_value) { terrain_ = p_value; }
	// Terrain-tile (.til) bytes for the S2C 0x45 stream a listen host serves.
	PackedByteArray get_terrain_til() const { return terrain_til_; }
	void set_terrain_til(const PackedByteArray &p_value) { terrain_til_ = p_value; }
	String get_wac_basename() const { return wac_basename_; }
	void set_wac_basename(const String &p_value) { wac_basename_ = p_value; }

	// --- Presentation composition. The present passes' other collaborators
	//     (the mission audio, the effect world, the light director, the
	//     environment node, the owner-anchor registry) are typed objects that
	//     exist only after the runtime stage: the load hands them to
	//     MissionRoot.setup_passes once they do. ---
	Ref<MissionObjectPlacer> get_placer() const { return placer_; }
	void set_placer(const Ref<MissionObjectPlacer> &p_value) { placer_ = p_value; }

	// --- Net staging read DOWNSTREAM of setup(): GameWorld's LAN bind-failure
	//     report and SessionDrive's NovaWorld gate registration. ---
	// "lan" for a staged host, "lan-join" for a staged joiner, "" for a local
	// start.
	String get_net_transport() const { return net_transport_; }
	void set_net_transport(const String &p_value) { net_transport_ = p_value; }
	int get_bind_port() const { return bind_port_; }
	void set_bind_port(int p_value) { bind_port_ = p_value; }
	String get_server_name() const { return server_name_; }
	void set_server_name(const String &p_value) { server_name_ = p_value; }
	// The host's own callsign advertised on the gate row.
	String get_player_name() const { return player_name_; }
	void set_player_name(const String &p_value) { player_name_ = p_value; }
	int get_max_players() const { return max_players_; }
	void set_max_players(int p_value) { max_players_ = p_value; }
	String get_channel() const { return channel_; }
	void set_channel(const String &p_value) { channel_ = p_value; }
	// NovaWorld gate registration (HostSessionConfig.CHANNEL_NOVAWORLD): an
	// empty gate host means pure LAN — nothing is registered.
	String get_nw_gate_host() const { return nw_gate_host_; }
	void set_nw_gate_host(const String &p_value) { nw_gate_host_ = p_value; }
	int get_nw_gate_port() const { return nw_gate_port_; }
	void set_nw_gate_port(int p_value) { nw_gate_port_ = p_value; }
	String get_region() const { return region_; }
	void set_region(const String &p_value) { region_ = p_value; }
	// Explicit advertised-IP override for the gate row; empty keeps the socket's.
	String get_advertise() const { return advertise_; }
	void set_advertise(const String &p_value) { advertise_ = p_value; }

protected:
	static void _bind_methods();

private:
	ObjectID music_director_id_;
	// The default lobby player cap when no host UI supplied one
	// (HostSessionConfig.DEFAULT_MAX_PLAYERS; the host-side clamp to the
	// witnessed 1..65 applies either way).
	static constexpr int kDefaultMaxPlayers = 32;

	Ref<Simulation> simulation_;
	String mission_file_;
	String mission_name_;
	bool playable_ = false;
	Ref<HostSessionOptions> host_session_;
	Ref<JoinTarget> join_target_;
	Ref<CharacterJoinProfile> local_character_profile_;
	Ref<CharacterJoinProfile> join_character_profile_;
	PackedStringArray spawn_names_;
	Ref<ResourceRoot> resource_root_;
	Ref<ItemDatabase> item_db_;
	Ref<TerrainData> terrain_;
	PackedByteArray terrain_til_;
	String wac_basename_;
	Ref<MissionObjectPlacer> placer_;
	String net_transport_;
	int bind_port_ = opennova::kRetailLanPortMin;
	String server_name_;
	String player_name_;
	int max_players_ = kDefaultMaxPlayers;
	// HostSessionConfig.CHANNEL_LAN: the LAN channel never reads or
	// manufactures NovaWorld service configuration.
	String channel_ = "LAN";
	String nw_gate_host_;
	int nw_gate_port_ = opennova::kNovaWorldGatePort;
	String region_ = "us";
	String advertise_;
};

} // namespace godot
