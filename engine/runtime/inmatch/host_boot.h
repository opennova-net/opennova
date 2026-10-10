#pragma once

// The one mission boot every host embedder runs (ADR 0051 d4): the game's
// Simulation (single player, the listen host, Serve Only, and a joiner, which
// rides the same order with no HostRole) and opennova-serve call the same two
// phases, so the composition of the boot exists once. Before it the order was
// written twice, in godot/src (the mission root, the simulation, the world's
// load) and in apps/serve.
//
// The phases bracket the embedder's device stages: the game places the
// mission objects and builds its render terrain before phase A, and binds its
// occlusion, audio and effects between the two; a headless host runs them
// back to back. Everything an embedder still owns is the config (the host
// screen or the host file), the mount, the socket, the clock and its devices.
//
//   boot_host_mission (A): the loose score.ini into the session config, the
//     mission .til, the mission text, the gametext "Server" strings,
//     Session::begin_load, the fresh kernel and its open, the terrain field
//     with the placed tiles and the .TSD table, the environment and the water
//     plane (both before the PreMission pass), the HostRole's staged
//     bring-up, MissionKernel::boot with the bringup_net_session hook, the
//     item catalog and traits, and charattr.def.
//   start_host_mission (B): the water plane, the mission-start environment
//     boundary (the weather seed, the embedder's render bind, the kernel's
//     complete_mission_start: the eager WAC, the 255-tick settle, the
//     vehicles, the baseline), the authority's load end (the S2C 0x7B to
//     every slot), then Session::complete_load.
//
// A map change (inmatch/map_change.h) boots the next map through the same
// two phases with HostBootRequest::next_mission: the role's bring-up
// continues the kept session instead of starting one.
//
// Retail's whole sequence is Game_StartMission @0x524360 inside the Game Loop
// mode's initialize, which the loop's clock never banks (session.cpp carries
// that witness); the per-step witnesses sit on the steps in host_boot.cpp.

#include <formats/env/env.h>
#include <formats/mission/bms.h>
#include <runtime/environment/environment_state.h>
#include <runtime/environment/weather_runtime.h>
#include <runtime/inmatch/charattr_table.h>
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/session.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/mission/mission_text.h>
#include <runtime/mission/runtime_boot.h>
#include <runtime/replication/item_replication_catalog.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace opennova::inmatch {

struct HostBootRequest {
	// The mission document as the embedder read it (the server's catalog
	// row through the archive or loose policy, the shell's MissionData) and
	// its file basename (the .til / .bin / .dbf / .wac stem).
	bms::File mission;
	std::string mission_basename;
	// The mounted reader; its read_loose_first serves the reads retail makes
	// off the disk (score.ini, the .til).
	mission::BootFileSource files;
	// The shared native assets (null = none) and the embedder's parsed
	// items.def rows (null = the kernel parses items.def off `files`); both
	// outlive the kernel.
	const assets::AssetStore *assets = nullptr;
	const def::DefItemsFile *items = nullptr;
	// The items.def replication catalog the item traits and the host's
	// HostClient view take; null = built from the items table at the step.
	std::shared_ptr<const replication::ItemReplicationCatalog> item_catalog;

	// The session and the role it runs. `host` is the role as a HostRole (the
	// SP listen server, the listen host, the dedicated host); null for a
	// joiner, which skips the authority's legs (score.ini, the staged
	// bring-up, the Server strings, charattr.def: a joiner loads its own
	// charattr.def before it connects).
	Session *session = nullptr;
	Role *role = nullptr;
	HostRole *host = nullptr;
	// The host's bring-up record: the session config the host screen or the
	// host file produced, the socket mode, the network type, serve-and-play
	// and the install root. The boot adds the score.ini rows, the .til, the
	// mission text and the Server strings before it stages it.
	HostConfig host_cfg;
	// Overlay the loose score.ini onto host_cfg.config (an in-session host:
	// start_host_session copies the score table into world.match).
	bool session_score_ini = false;
	// The map change's boot (inmatch/map_change.h): the host continues its
	// kept session on the new kernel instead of starting one, and phase B's
	// load end pumps the socket for the reloading joiners.
	bool next_mission = false;
	// The .til bytes when the embedder has them (a joiner's S2C 0x45 stream,
	// which must never fall back to a same-named local file); unset = the
	// boot reads <basename>.til loose-first.
	std::optional<std::vector<uint8_t>> terrain_til;
	// The S2C 0x0B header-only world (the joiner's wire materialization).
	bool wire_header_world = false;

	// The kernel boot's embedder flags: playable, joiner, mp_session, terrain
	// (false when the embedder builds the store from its own parsed
	// documents), wac, wac_basename, infantry_adm, music_globals, game_type,
	// player_limit, team_count, seat_specs, collision. The boot sets
	// defer_mission_start (phase B completes the start), the people-name
	// resolver (the boot's mission text) and the bring-up hook.
	mission::KernelBootOptions boot_options;

	// The embedder's seams, run in the boot's order:
	//   fresh_kernel   -- the load's fresh kernel, the previous one's carry
	//                     taken (MissionKernel::carry_across_load_from) and
	//                     the role bound; the boot then installs the assets
	//                     and opens the document. Required.
	//   before_terrain -- after the open: the embedder that holds parsed
	//                     terrain documents builds the store
	//                     (terrain_field_store_build) and re-layers its
	//                     device-fed tables here. Optional.
	//   after_bringup  -- the tail of the kernel's bringup_net_session hook,
	//                     after the role's bring_up(), given its result (a
	//                     joiner's fresh ClientRuntime). Optional.
	std::function<mission::MissionKernel &()> fresh_kernel;
	std::function<void(mission::MissionKernel &)> before_terrain;
	std::function<void(bool fresh_runtime)> after_bringup;
};

// What phase A leaves for phase B and for the embedder to retain. Not
// copyable or movable: the environment state points into the boot's own
// config.
struct HostBoot {
	HostBoot() = default;
	HostBoot(const HostBoot &) = delete;
	HostBoot &operator=(const HostBoot &) = delete;

	mission::MissionKernel *kernel = nullptr;
	Session *session = nullptr;
	// The authority role the boot ran (null for a joiner): phase B ends its
	// load (HostRole::finish_mission_load).
	HostRole *host = nullptr;
	// The bring-up's session create (server_session.h). ProcessExit: the
	// session config's `mpreset` word was set, so the create refused and
	// phase A returned false; the embedder ends its process with exit code 0,
	// as retail's create does, and runs no phase B. A joiner's boot and a map
	// change's create no session and keep Created.
	// [orig: CNapiGameSession_CreateSession @0x4C97E7..0x4C97F0 -> crt_exit(0)]
	CreateSessionResult session_create = CreateSessionResult::Created;
	// The tables the boot read, for the embedder to retain.
	mission::MissionText mission_text;
	std::vector<uint8_t> terrain_til;
	ServerTextTable server_text;
	// The process's character-attribute table (charattr_table.h), kept from
	// boot to boot as retail's g_CharAttr outlives every mission: the first
	// authority boot loads charattr.def into it (`charattr_read`), and every
	// authority boot applies the session's restrictions to it, whose zeroed
	// properties and raised latches stay for the next.
	CharAttrTable charattr;
	bool charattr_read = false;
	std::shared_ptr<const replication::ItemReplicationCatalog> item_catalog;
	// The mission environment the authority reads: the .env with the BMS
	// override layer (env::load_mission_env's skip rule: a missing file
	// leaves the engine defaults), and the water plane it resolved, 16.16.
	env::Config env_config;
	env::EnvironmentState environment;
	int32_t water_z_q16 = 0;
	// The headless weather owner phase B runs when the embedder brings none.
	std::unique_ptr<env::WeatherRuntime> weather;
};

// The gametext "Server" strings the host's handlers format a player's name
// into, from the mounted gametext.bin; an absent key is GameText_GetString's
// "" miss, which the handler sends nothing for [orig: Game_InitSubsystems
// @0x4A6CD0 loads gametext.bin]. Phase A reads them; an embedder staging its
// own bring-up (the game's in-memory loads) reads them here.
ServerTextTable read_host_server_text(const mission::BootFileSource &files);

// Phase A. False with `error` when the session cannot enter its load, the
// kernel boot fails, or the host's session create asks for the process exit
// (`boot.session_create`); the session is left Loading for the embedder to
// fail.
bool boot_host_mission(HostBootRequest request, HostBoot &boot, std::string &error);

// The embedder's weather device for phase B: its WeatherRuntime and the
// environment it renders (the game's Weather node and MissionEnvironment),
// and the render bind the boundary runs after the seed. Default = headless:
// the boot's own environment and a WeatherRuntime of its own, no bind.
struct HostStartDevice {
	env::WeatherRuntime *weather = nullptr;
	env::EnvironmentState *environment = nullptr;
	std::function<void()> bind_render;
};

// Phase B. False with `error` when the session cannot leave its load.
bool start_host_mission(HostBoot &boot, const HostStartDevice &device, std::string &error);

} // namespace opennova::inmatch
