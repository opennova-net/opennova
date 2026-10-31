#include <runtime/mission/mission_load.h>

#include <utility>

namespace opennova::mission {

namespace {

// A leg the embedder supplied, run; an empty one is a stage it has nothing to do at.
template <class Leg, class... Args> void run_leg(const Leg &leg, Args &&...args) {
	if (leg)
		leg(std::forward<Args>(args)...);
}

void present(const MissionLoadLegs &legs, int percent) {
	run_leg(legs.device.present, percent);
}

// A stage's start: the loading screen at the stage's checkpoint, presented when the stage begins
// (mission_load_plan.h), then the session's periodic update that follows each present
// (inmatch::run_loading_step; JoinerRole::pump_mission_start_periodic_update carries the witness,
// D-NET-1209).
void begin_stage(const MissionLoadLegs &legs, MissionLoadStage stage) {
	present(legs, mission_load_progress_percent(stage));
	run_leg(legs.session.loading_step);
}

void span(const MissionLoadLegs &legs, const char *name) {
	run_leg(legs.device.span, name);
}

void end_span(const MissionLoadLegs &legs) {
	run_leg(legs.device.end_span);
}

// One spanned leg (an instrument): the span opened, the leg, the span closed.
template <class Leg, class... Args>
void spanned(const MissionLoadLegs &legs, const char *name, const Leg &leg, Args &&...args) {
	span(legs, name);
	run_leg(leg, std::forward<Args>(args)...);
	end_span(legs);
}

// A leg that can stop the load: its error, the timeline finished with it.
int checked(const MissionLoadLegs &legs, const std::function<int()> &leg) {
	if (!leg)
		return 0;
	const int error = leg();
	if (error != 0)
		run_leg(legs.device.finish_timeline);
	return error;
}

// The mission start's head through its terrain, past the start's copies of the options: the
// terrain's gate, the head's opening and the tile info, the environment (the mission's overrides,
// its clock, the weather owner) and the terrain's connect leg; each stage presents its checkpoint
// when it starts, no periodic update between them. The local-asset gate holds for a wire-header
// join too: there is no world without terrain, and a retail joiner resolves it from its own
// install by the name the wire supplies (net-re section 5.28); the environment gates nothing (a
// .env that is not there is skipped and the mission starts on the engine's defaults). The clock and
// the weather owner's reset stand before the runtime is constructed, so the authority's T0 sample
// precedes its first network tick.
// [orig: Game_StartMission @0x524B26..0x5251CD -> Game_LoadTerrainDuringConnect @0x520710 ->
//  Terrain_LoadEnvironmentConfig @0x52073B, Terrain_Init @0x520765]
int run_head(const MissionLoadLegs &legs) {
	const MissionLoadDeviceLegs &device = legs.device;
	if (const int error = checked(legs, device.terrain_gate); error != 0)
		return error;
	run_leg(device.begin_timeline);
	run_leg(device.head_begin);
	present(legs, mission_load_progress_percent(MissionLoadStage::kEnvironment));
	spanned(legs, "environment", device.environment);
	present(legs, mission_load_progress_percent(MissionLoadStage::kTerrain));
	span(legs, "terrain");
	if (const int error = checked(legs, device.terrain); error != 0)
		return error;
	end_span(legs);
	return 0;
}

} // namespace

int run_mission_start(const MissionLoadLegs &legs) {
	// Phase B of the one host boot (inmatch/host_boot.h start_host_mission): the head a deferred
	// boot left (the render arm's last pump, the PreMission pass, the round init,
	// Player_InitPlayer), the water plane the vehicle init grounds hulls against, the environment
	// boundary and the session's load end.
	if (legs.session.mission_start_boundary) {
		if (const int error = legs.session.mission_start_boundary(); error != 0)
			return error;
	}
	// The player profile's saved weapon kits, loaded before ANY kit is applied or submitted: in a
	// net session the spawn kit is a page of this file, selected by the very class byte it also
	// puts on the wire [orig: Game_StartMission @ 0x525767-0x525836].
	run_leg(legs.session.weapon_profile);
	run_leg(legs.device.spawn_loadout);
	// The game starts running past the session's load end, which leaves a single player's session
	// paused.
	run_leg(legs.session.play);
	return 0;
}

int run_join_head_load(const MissionLoadLegs &legs) {
	// The S2C 0x11's load (inmatch/joiner_connection.h set_sync_tail_loading carries the
	// witness): the mission start's copies of the options, then the head with the session's
	// periodic update ahead of the terrain and past it.
	run_leg(legs.device.copy_start_options);
	run_leg(legs.session.loading_step);
	if (const int error = run_head(legs); error != 0)
		return error;
	run_leg(legs.session.loading_step);
	// Then the avatar combos' models, at the header's climate: their data loads into the placer
	// the S2C 0x1A's load keeps, and that load's freeze resolves their materials with the rest of
	// the list (inmatch/loaded_model_list.h avatar_combo_models, D-NET-845, D-NET-1157).
	spanned(legs, "avatars", legs.device.avatar_models);
	run_leg(legs.device.finish_timeline);
	return 0;
}

int run_mission_load(const MissionLoadArm &arm, const MissionLoadLegs &legs) {
	const MissionLoadSessionLegs &session = legs.session;
	const MissionLoadDeviceLegs &device = legs.device;
	// A join ran the head at its S2C 0x11 (run_join_head_load): this load, at its S2C 0x1A, goes
	// on from the objects (inmatch/joiner_connection.h set_sync_tail_loading carries the witness).
	const bool head_loaded = arm.wire_header_join && arm.head_loaded;
	if (head_loaded) {
		run_leg(device.begin_timeline);
	} else {
		// A fresh host's or single player's session stands before the mission start's every load:
		// its simulation staged (the host's bound socket) and its session created here, ahead of
		// the head's terrain and the objects (inmatch/host_boot.h create_host_session carries the
		// witness, D-NET-1254).
		if (arm.fresh_authority) {
			if (const int error = checked(legs, session.create_session); error != 0)
				return error;
		}
		// A host's mission start runs its head ahead of its terrain, a fresh session's past its
		// create and a map change's on the kept session, and pumps the session through the load's
		// window and stages, as retail's load does (inmatch/host_role.h begin_mission_load /
		// begin_next_mission_load / pump_load_stage, D-NET-116).
		run_leg(session.begin_host_load);
		// A NovaWorld host's mission-start server-info update, past its session's create or the
		// map change's head and ahead of the effect system and every load (inmatch/server_info.h
		// apply_mission_start_columns carries the witness, D-NET-1254).
		run_leg(session.mission_start_update);
		run_leg(device.begin_timeline);
		run_leg(device.copy_start_options);
		// Then the mission start's effect system at the copies' words, past the head's per-slot
		// reset and ahead of the window's first pumps and the terrain, on every load but a
		// dedicated host's: retail's skips it in session on the authority with no client of its
		// own (D-NET-1055). A join's mission-data load built one already (its final S2C 0x64,
		// D-NET-1054); a single player's restart, whose effect system retail keeps from its first
		// load, builds one here too, its unload having freed the last.
		// [orig: Game_StartMission @0x524360 - the dedicated test @0x524958..0x524979,
		//  CEffectSystem_Init @0x524980, CEffectSystem_BuildTextureAtlases @0x5249B9]
		if (!arm.dedicated && !arm.effects_preloaded)
			spanned(legs, "effect_system", device.head_effect_system);
		run_leg(session.open_host_load_window);
		if (const int error = run_head(legs); error != 0) {
			run_leg(device.drop_created_session); // the created session goes with its staging
			return error;
		}
		// The first stage pump, after the terrain [orig: Game_StartMission -
		//  Game_LoadTerrainDuringConnect @0x524F7C, Server_PumpNetworkTransport @0x524F8C].
		run_leg(session.pump_host_load_stage);
	}

	begin_stage(legs, MissionLoadStage::kObjects);
	span(legs, "objects");
	// A join whose S2C 0x11 ran the head keeps that load's models, the avatars' among them.
	run_leg(device.mission_root, head_loaded);
	// A host's load: the stages after the mission's document and after the avatars and the clock,
	// ahead of the objects [orig: Game_StartMission - Mission_LoadBMSFile @0x524FFA,
	//  Server_PumpNetworkTransport @0x525256, Avatars_LoadComboModels @0x525267,
	//  Server_PumpNetworkTransport @0x5253EA].
	if (!head_loaded) {
		run_leg(session.pump_host_load_stage);
		run_leg(session.pump_host_load_stage);
	}
	run_leg(device.place_objects);
	end_span(legs);

	begin_stage(legs, MissionLoadStage::kRuntime);
	span(legs, "runtime");
	// GAME music opens and seeds for a session with a client of its own, a single player's
	// included; a dedicated host closes the context. M# resolves this context while WAC compiles,
	// so the open precedes the runtime's boot (docs/audio/mus-sbf-re.md, D-MUS-SPGATE;
	// docs/world/world-wac-ai-re.md section 33.15a) [orig: Game_StartMission - @0x525581..0x52561b
	//  ahead of the WAC init @0x525CB3].
	run_leg(device.music_context, !arm.dedicated);
	if (session.start_runtime) {
		if (const int error = session.start_runtime(); error != 0) {
			end_span(legs);
			run_leg(device.finish_timeline);
			// A load abort leaves the mission as the player's own quit does.
			run_leg(device.unload_failed_runtime);
			return error;
		}
	}
	const MissionLoadRole role = session.role ? session.role() : MissionLoadRole::None;
	const bool has_session = role != MissionLoadRole::None;
	const bool start_deferred = session.mission_start_deferred && session.mission_start_deferred();
	// A boot that left the device stages no mission-start head (a joiner's, a world with no host)
	// runs the mission start with the runtime's start.
	if (has_session && !start_deferred) {
		if (const int error = run_mission_start(legs); error != 0) {
			end_span(legs);
			run_leg(device.finish_timeline);
			return error;
		}
	}
	run_leg(device.runtime_started);
	end_span(legs);
	// The C2S 0x3D page freezes once here (the load plan's freeze witness): the mission start's
	// model-def list computed from the mount and the loaded tables (inmatch/loaded_model_list.h,
	// D-NET-527); late network spawns must not change it. A wire-header joiner's freezes past its
	// streamed world, below.
	if (has_session && !arm.wire_header_join) {
		if (role == MissionLoadRole::Joiner)
			run_leg(device.warm_model_definitions);
		run_leg(session.freeze_model_list);
	}

	begin_stage(legs, MissionLoadStage::kAudio);
	spanned(legs, "audio", device.mission_audio);
	const bool joiner_start = role == MissionLoadRole::Joiner;
	// A joiner's mission start initializes every streamed row from its model, then queues its
	// first 0x2F: its sound banks, the entity models' reload and the rows' init all run ahead of
	// the first submission, and the model freeze after it (inmatch::JoinerRole::MissionStartLegs
	// carries the witness, D-NET-1053). The reload's pool-0 leg loads and registers each person
	// row's animation map here, in the load (JoinerRole::reload_pool0_row_channels, D-NET-1252).
	if (joiner_start)
		spanned(legs, "pool0_channels", session.reload_pool0_rows);
	bool wire_world_placed = false;
	if (joiner_start && arm.wire_header_join) {
		span(legs, "streamed_world");
		wire_world_placed = device.place_streamed_world && device.place_streamed_world();
		end_span(legs);
	}
	const bool dedicated_host = role == MissionLoadRole::DedicatedHost;
	// The celestial bodies' models past the rows' init, ahead of the HUD's overlay init, on every
	// arm (a joiner's inside its S2C 0x1A load, not its 0x11 head); their draw objects on the full
	// arm alone (environment/celestial_frame.h builds_celestial_draws carries the witness,
	// D-NET-1187).
	spanned(legs, "celestial", device.celestial_models, dedicated_host);
	// The HUD's overlay init, on every mission start and every role: past the rows' init and
	// ahead of a joiner's first 0x2F (mission_load_plan.h carries the witness, D-NET-1167).
	spanned(legs, "hud_init", device.hud_overlay_init);
	if (joiner_start) {
		// The definitions and the world are in: the mission start's first leg queues the first
		// 0x2F and runs the pumps ahead of the effect builds and the freeze
		// (ClientRuntime::push_mission_start_first_loadout, D-NET-861).
		spanned(legs, "first_leg", session.first_loadout);
		// A wire-header joiner's C2S 0x3D page freezes here, its streamed world in, after the first
		// leg as the freeze follows the first 0x2F; its stream's types take their model-derived
		// legs first, and the mission textures stage's freeze below resolves its models'
		// materials, as a host's does (D-NET-527, D-NET-1053, D-NET-1157).
		if (wire_world_placed) {
			span(legs, "model_list");
			run_leg(device.warm_model_definitions);
			run_leg(session.settle_streamed_types);
			run_leg(session.freeze_model_list);
			end_span(legs);
		}
	}

	begin_stage(legs, MissionLoadStage::kEffects);
	// A join's mission-data load built and warmed the effect world ahead of its 0x09; the rest
	// binds it to this mission.
	spanned(legs, "effects", device.start_effect_world);
	// A host's render arm: its load window's pumps ahead of the mission's textures
	// (HostRole::render_arm_pumps carries the witness).
	if (has_session)
		run_leg(session.pump_render_arm);

	// The effect catalog warms while the loading screen still covers the frame, as retail pays it
	// at load (the load plan's effect-system witness).
	begin_stage(legs, MissionLoadStage::kEffectsWarm);
	// The mission textures stage opens with the terrain's, on every arm, a dedicated host's
	// reduced one too (terrain/terrain_load_legs.h carries the witness, D-NET-1098), then the sky
	// maps, the full arm's (environment/sky_frame.h loads_mission_sky_maps, D-NET-1170).
	run_leg(device.terrain_textures, dedicated_host);
	spanned(legs, "sky_maps", device.sky_maps, dedicated_host);
	// Then the freeze, on every arm: the listed models' materials resolve and no model builds, a
	// wire-header joiner's list too. The full arm's freeze is the reduced arm's, with the render
	// arm's device inits after it (inmatch/loaded_model_list.h LoadedModelList::model_files
	// carries the witness, D-NET-1078, D-NET-1157).
	span(legs, "model_materials");
	if (has_session)
		run_leg(device.resolve_model_materials);
	end_span(legs);
	// The HUD's textures and fonts past the freeze: the full arm's textures and materials, every
	// arm's fonts and stdbox frame (mission_load_plan.h carries the witness, D-NET-1167).
	spanned(legs, "hud_textures", device.hud_textures, !dedicated_host);
	// The view effects', the tracer pool's and FrameFX's device objects past the HUD's textures:
	// the full arm's, none on the reduced arm (D-NET-1172, D-NET-1231).
	spanned(legs, "frame_fx_textures", device.frame_fx_textures, dedicated_host);
	// The render arm's device inits past the freeze: the effect catalog and the presenters' fire
	// pipelines warm in the forced frames; a dedicated host's reduced arm runs none.
	if (!dedicated_host)
		spanned(legs, "effects_warm", device.effects_warm, !arm.effects_preloaded);
	// A host's mission start past its device stages: phase B opens with the render arm's last
	// pump, then the PreMission pass, the round init and Player_InitPlayer, as retail's start
	// runs them past its mission textures (inmatch/host_boot.h start_host_mission carries the
	// witness).
	if (has_session && start_deferred) {
		span(legs, "mission_start");
		const int error = run_mission_start(legs);
		end_span(legs);
		if (error != 0) {
			run_leg(device.finish_timeline);
			return error;
		}
	}
	// The start's WAC/BMS descriptors, past the warm-scene reset.
	spanned(legs, "script_effects", device.script_effects);
	// The load end past the mission start: a host's S2C 0x7B and the spawn gate's clear
	// (inmatch::end_host_mission_load).
	span(legs, "load_end");
	if (has_session)
		run_leg(session.end_host_load);
	end_span(legs);
	// The tail's legs are spans too (an instrument): the world-ready edges and their listeners run
	// inside the load's call.
	span(legs, "finish");
	begin_stage(legs, MissionLoadStage::kFinish);
	end_span(legs);
	run_leg(device.world_ready);
	// The water's world rendering opens at the load's end; its minimap mask was made with the
	// terrain's textures.
	spanned(legs, "water", device.water_enabled);
	span(legs, "world_loaded");
	present(legs, kMissionLoadProgressWorldReady);
	run_leg(device.world_loaded);
	end_span(legs);
	run_leg(device.finish_timeline);
	// The first frames past the load, timed on a timeline of their own (an instrument): a host's
	// S2C 0x7B leaves at the first open send boundary of their updates (HostRole::
	// finish_mission_load), and the mission start's first frames bank no render time
	// (inmatch::Session::complete_load).
	run_leg(device.post_load_frames);
	return 0;
}

} // namespace opennova::mission
