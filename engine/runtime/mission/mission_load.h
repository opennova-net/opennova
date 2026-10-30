#pragma once

// The mission load: the executor that drives the load plan (mission_load_plan.h) through the
// embedder's legs. The stage order, the arm each role takes and the session's points inside the
// load are this executor's; an embedder supplies legs alone: its session's (the authority's head,
// window and stage pumps, the runtime's start with the host boot's phase A, the model freeze, a
// joiner's mission-start legs, phase B) and its device's (the loading screen's presents, the
// scene's stage loads, the instrument's spans). A leg left empty is a stage the embedder has
// nothing to do at: opennova-serve's headless Serve Only host fills the session's legs alone and
// the game's GameWorld every leg, so the server and the game run one load order (ADR 0077 R2;
// before it the order lived in the Godot binding and the server ran the boot's phases back to
// back with its freeze past phase B).
//
// Retail's whole sequence is Game_StartMission @0x524360; mission_load_plan.h carries the stage
// witnesses, inmatch/host_boot.h and inmatch/host_role.h the session's, and each step below
// cites what it orders.

#include <runtime/mission/mission_load_plan.h>

#include <cstdint>
#include <functional>

namespace opennova::mission {

// The role the load's session runs once its runtime stands (MissionLoadSessionLegs::role).
enum class MissionLoadRole : uint8_t {
	None, // the runtime's start built no session
	Authority, // a single player's or a listen host's: the authority with a client of its own
	DedicatedHost, // the authority with no client of its own (Serve Only)
	Joiner,
};

// The load in hand, as the embedder knows it at the load's start.
struct MissionLoadArm {
	// The document is the host's S2C 0x0B header alone (a wire join, D-NET-194).
	bool wire_header_join = false;
	// The join's S2C 0x11 ran the mission start's head and its terrain (run_join_head_load):
	// this load, at its S2C 0x1A, goes on from the objects. Read with wire_header_join alone.
	bool head_loaded = false;
	// A fresh host's or single player's session, created ahead of the mission start's every load
	// (MissionLoadSessionLegs::create_session, D-NET-1254).
	bool fresh_authority = false;
	// The load's session request is a dedicated host's (the authority with no client of its own):
	// no effect system at the head (D-NET-1055) and the game music context closed.
	bool dedicated = false;
	// A join's mission-data load built and warmed the effect world already (its final S2C 0x64,
	// D-NET-1054): no effect system at the head, and the warm spawns no catalog.
	bool effects_preloaded = false;
};

// The session's legs. Each returning int answers 0 to go on; any other value stops the load with
// it, the leg having reported why.
struct MissionLoadSessionLegs {
	// A fresh authority's session create ahead of the mission start's every load
	// (inmatch::create_host_session carries the witness).
	std::function<int()> create_session;
	// The authority's load head (HostRole::begin_mission_load, a map change's
	// inmatch::begin_next_host_mission_load), its window past the effect system
	// (HostRole::open_mission_load_window) and one of its stage pumps (HostRole::pump_load_stage),
	// for an embedder whose loads run ahead of the boot's phase A. Empty: phase A runs them.
	std::function<void()> begin_host_load;
	std::function<void()> open_host_load_window;
	std::function<void()> pump_host_load_stage;
	// The mission start's server-info update past the head (inmatch::HostBootRequest::
	// mission_start_update carries the witness). Empty: phase A runs it, or none is listed.
	std::function<void()> mission_start_update;
	// The session's periodic update past each loading present (inmatch::run_loading_step).
	std::function<void()> loading_step;
	// The mission runtime's start over the loaded world: the host boot's phase A
	// (inmatch::boot_host_mission), the session's role brought up inside it.
	std::function<int()> start_runtime;
	// The role the started runtime runs (None: no session).
	std::function<MissionLoadRole()> role;
	// The boot left the mission start's head to phase B, past the device stages
	// (inmatch::HostBoot::mission_start_head_owed).
	std::function<bool()> mission_start_deferred;
	// The mission start's model-def list frozen: the C2S 0x3D page a joiner answers from and a
	// host's S2C 0x68 cursor wrap (inmatch/loaded_model_list.h).
	std::function<void()> freeze_model_list;
	// A joiner's model reload, its pool-0 leg (inmatch::JoinerRole::reload_pool0_row_channels),
	// and its mission start's first leg (JoinerRole::push_mission_start_first_loadout).
	std::function<void()> reload_pool0_rows;
	std::function<void()> first_loadout;
	// A header-only joiner's model-derived legs once its stream's types are in, ahead of the
	// freeze (inmatch/joiner_boot.h settle_streamed_types).
	std::function<void()> settle_streamed_types;
	// The render arm's pumps ahead of the mission's textures
	// (HostRole::pump_render_arm_ahead_of_textures).
	std::function<void()> pump_render_arm;
	// Phase B: the mission-start environment boundary over the embedder's weather device
	// (inmatch::start_host_mission).
	std::function<int()> mission_start_boundary;
	// The player profile's saved weapon kits onto the session, ahead of any kit's apply.
	std::function<void()> weapon_profile;
	// The game starts running past the session's load end.
	std::function<void()> play;
	// The load end phase B left (inmatch::end_host_mission_load).
	std::function<void()> end_host_load;
};

// The device's legs: the backend's work at each stage, in the executor's order.
struct MissionLoadDeviceLegs {
	// The loading screen's present at `percent` (mission_load_plan.h's checkpoints).
	std::function<void(int percent)> present;
	// The load's timeline (an instrument): begun when none is, a named span opened and the
	// innermost closed, and finished with every span still open.
	std::function<void()> begin_timeline;
	std::function<void(const char *name)> span;
	std::function<void()> end_span;
	std::function<void()> finish_timeline;
	// The mission start's copies of the options (the object detail, the texture words, the
	// particle density, the ragdoll bit, the view's aspect), ahead of its effect system and head.
	std::function<void()> copy_start_options;
	// The effect system at the head: every mounted .ptl and its texture atlases.
	std::function<void()> head_effect_system;
	// The head: the terrain's gate (nonzero: the load's failure, reported), its opening (the
	// weather owner, the water dormant, the mount, the tile info), the environment (the .env, the
	// mission's overrides, the clock, the weather owner's reset) and the terrain's connect leg
	// (nonzero: failed, reported).
	std::function<int()> terrain_gate;
	std::function<void()> head_begin;
	std::function<void()> environment;
	std::function<int()> terrain;
	// A join's avatar combos' models at the header's climate, its S2C 0x11 load's tail.
	std::function<void()> avatar_models;
	// The mission's per-load root (`keep_head_models`: the S2C 0x11 head's models stand) and the
	// placed objects.
	std::function<void(bool keep_head_models)> mission_root;
	std::function<void()> place_objects;
	// The game music context opened and seeded (`open`) or closed.
	std::function<void(bool open)> music_context;
	// The runtime's start's device tail, past phase A (and past the mission start when phase A
	// left none to the device stages).
	std::function<void()> runtime_started;
	// The failures' teardown: a created session dropped with its staging; a failed runtime's world
	// unloaded.
	std::function<void()> drop_created_session;
	std::function<void()> unload_failed_runtime;
	// The loaded-model list's definitions warmed ahead of its freeze.
	std::function<void()> warm_model_definitions;
	std::function<void()> mission_audio;
	// A wire-header joiner's streamed world placed (false: failed, latched for the settle).
	std::function<bool()> place_streamed_world;
	// The celestial bodies' models; their draw objects on the full arm alone.
	std::function<void(bool dedicated)> celestial_models;
	std::function<void()> hud_overlay_init;
	std::function<void()> start_effect_world;
	// The mission textures stage, each leg by the arm (`dedicated`: the reduced arm).
	std::function<void(bool dedicated)> terrain_textures;
	std::function<void(bool dedicated)> sky_maps;
	std::function<void()> resolve_model_materials;
	std::function<void(bool full_arm)> hud_textures;
	std::function<void(bool dedicated)> frame_fx_textures;
	// The full arm's device warm (`spawn_effects`: the catalog still cold).
	std::function<void(bool spawn_effects)> effects_warm;
	// The local player's staged spawn kit applied.
	std::function<void()> spawn_loadout;
	std::function<void()> script_effects;
	// The world ready, the water's world rendering on, the world loaded, and the first frames
	// past the load timed.
	std::function<void()> world_ready;
	std::function<void()> water_enabled;
	std::function<void()> world_loaded;
	std::function<void()> post_load_frames;
};

struct MissionLoadLegs {
	MissionLoadSessionLegs session;
	MissionLoadDeviceLegs device;
};

// A mission start's load: a single player's, a host's (a fresh session's or a map change's) and a
// joiner's (its S2C 0x1A load, or a document join's whole one). 0, or the error a leg stopped it
// with.
int run_mission_load(const MissionLoadArm &arm, const MissionLoadLegs &legs);

// A join's S2C 0x11 load: the mission start's head and its terrain off the host's 0x0B header,
// then the avatar combos' models, ahead of the game-start wait's 0x0A; its S2C 0x1A load goes on
// from the objects (MissionLoadArm::head_loaded). 0, or the error a leg stopped it with.
int run_join_head_load(const MissionLoadLegs &legs);

// The mission start past phase A, where its arm runs it: the environment boundary (phase B), the
// player profile's weapon kits, the spawn kit, then play. 0, or phase B's error.
int run_mission_start(const MissionLoadLegs &legs);

} // namespace opennova::mission
