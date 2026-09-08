// GameWorld's effect fan-out: the WAC/BMS effect routing (dialog audio,
// fx2ssn emitters), the per-source-tick impact/scorch drains, the runtime
// signal targets, the wire-spawn router for both directors and the weather
// resync after a sim restore. The former world_effect_router.gd (slice G10):
// the mission_effects signal stays declared on GameWorld, the shared
// presentation sinks (the mission audio, the effect world, the two
// directors) and the probe switches are members.

#include "world/game_world.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "util/axes.h"
#include "util/color_convert.h"
#include "particle/effect_scene.h"

using namespace godot;

// Fire mission audio + particle effects for presentation. PlayWavList actions
// surface as "dialog" effects carrying the dialog/wav id in `a`; route them to
// the mission audio (which resolves the id through the co-named .DBF and plays
// the LWF set). WAC fx commands surface with the effect name in `str`; route
// them to the effect world. Other kinds are still emitted via mission_effects
// for downstream consumers (HUD, etc.).
void GameWorld::route_mission_effects(const Array &p_effects) {
	MissionAudio *audio = get_mission_audio();
	EffectWorld *effect_world = get_effect_world();
	MissionRoot *runtime = get_runtime();
	for (int64_t i = 0; i < p_effects.size(); ++i) {
		const Ref<MissionEffect> eff = p_effects[i];
		if (eff.is_null()) {
			continue;
		}
		const String kind = eff->get_kind();
		if (kind == "dialog") {
			// BMS PlayWavList: dialog id resolved through the co-named .DBF (queued).
			if (audio != nullptr) {
				audio->play_dialog(eff->get_a());
			}
		} else if (kind == "dialog_wav") {
			// WAC wave/pwave: a scripted voice .wav by filename on its own channel.
			if (audio != nullptr) {
				audio->play_wac_wave(eff->get_text());
			}
		} else if (kind == "fx2ssn") {
			// WAC fx2ssn: spawn the named effect at the SSN entity's position with
			// the emitter handle owned per entity — a scripted re-trigger detaches
			// the previous group (spawn_effect_owned), so loops/respawns never stack
			// emitters and FOREVEREMIT effects never accumulate
			// [orig: WacScript_SpawnEffectAtSsnEntity @ 0x4f23a0 — renamed from the
			// kong "sound" misnomer, it spawns a particle emitter]. The original
			// orients the emitter to the terrain surface normal at the entity's
			// grid cell, using the same recovered normal-map kernel as terrain.
			// [orig: WacScript_SpawnEffectAtSsnEntity @0x4f23a0 reads
			// outMillis/off_849934 after resolving the entity grid cell.]
			if (effect_world != nullptr && runtime != nullptr) {
				const int ssn = eff->get_b();
				const Variant pos = runtime->entity_position_for_ssn(ssn);
				if (pos.get_type() != Variant::NIL) {
					const Vector3 position = pos;
					Vector3 orientation(0, 1, 0);
					if (terrain_data_.is_valid()) {
						orientation = terrain_data_->get_surface_normal_world(position);
					}
					effect_world->spawn_effect_owned(ssn, eff->get_text(), position, orientation);
				}
			}
		}
		// fx2tgt (spawn at a placed type-6088 target marker
		// [orig: WacScript_SpawnEffectAtTargetMarker @ 0x4f7fd0 — same misnomer
		// family]) stays unrouted: which .bms record field carries the 1..99
		// target number is unwitnessed — ptl-format-re.md §8.
	}
}

// Drain the flight sim's resolved round impacts and present both descriptor legs.
// Impact particles are generic Always transients in the world domain; their
// production tick/order and catch-up age survive a multi-tick render frame.
// [orig: Projectile_UpdatePhysics @ 0x4e9d70 -> the type-specific impact
//  handler -> Projectile_SpawnImpactEffect @ 0x4e9b80]
void GameWorld::route_round_impacts() {
	Ref<Simulation> sim = get_sim();
	if (sim.is_null()) {
		return;
	}
	EffectWorld *effect_world = get_effect_world();
	MissionAudio *audio = get_mission_audio();
	std::vector<opennova::world::RoundImpactPresentation> rows;
	sim->drain_round_impact_rows(rows);
	for (const opennova::world::RoundImpactPresentation &row : rows) {
		// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position convention.
		const Vector3 pos = mission_to_godot(row.position);
		if (effect_world != nullptr && !row.effect.empty()) {
			effect_world->spawn_effect_transient(String::utf8(row.effect.c_str()), pos,
					mission_to_godot(row.direction), static_cast<int64_t>(row.age_ticks),
					EffectScene::RENDER_DOMAIN_WORLD, static_cast<int64_t>(row.source_tick),
					static_cast<int64_t>(row.source_order));
		}
		if (audio != nullptr && !row.sound.empty()) {
			audio->fire_soundset(String::utf8(row.sound.c_str()), pos);
		}
		// The light_impact flash rides the effect leg's own gate (the row only
		// carries light fields when the ammo authors it and the effect presents)
		// [orig: AmmoDef_ProcessImpactEffect @ 0x40a2b3].
		if (light_director_.is_valid() && row.has_light) {
			light_director_->on_impact_light(pos, row.light_radius,
					opennova::color_from_rgb24(row.light_color_rgb24), row.light_ticks);
		}
	}
}

// Install simulation-resolved permanent scorch records into the terrain page
// compiler before this source tick's ordinary impact presentation. Bounds are
// exact 16.16 terrain x/z; Terrain owns selective page invalidation.
void GameWorld::route_terrain_scorches() {
	Ref<Simulation> sim = get_sim();
	if (sim.is_null() || terrain_ == nullptr) {
		return;
	}
	std::vector<opennova::world::TerrainScorchEvent> events;
	sim->drain_terrain_scorches(events);
	for (const opennova::world::TerrainScorchEvent &event : events) {
		const opennova::terrain::TerrainScorchEntry &mission = event.mission_bounds;
		// mission (x,y,z) -> Godot (x,z,-y): negation swaps the ordered
		// extrema on the second ground-plane axis.
		terrain_->append_terrain_scorch(static_cast<int64_t>(mission.texture_index),
				static_cast<int64_t>(mission.minimum_x_q16),
				-static_cast<int64_t>(mission.maximum_z_q16),
				static_cast<int64_t>(mission.maximum_x_q16),
				-static_cast<int64_t>(mission.minimum_z_q16));
	}
}

// Consume render-internal lifecycle effects first, route "dialog" actions to
// mission audio (resolved through the co-named .DBF + LWF set), then expose only
// the remaining downstream effects to HUD consumers.
void GameWorld::on_runtime_effects(const Array &p_effects) {
	Array routed;
	for (int64_t i = 0; i < p_effects.size(); ++i) {
		const Ref<MissionEffect> effect = p_effects[i];
		if (effect.is_valid() && item_fx_->consume_control_effect(effect)) {
			continue;
		}
		routed.push_back(p_effects[i]);
	}
	if (routed.is_empty()) {
		return;
	}
	route_mission_effects(routed);
	emit_signal("mission_effects", routed);
}

void GameWorld::on_runtime_fixed_tick(int p_logic_tick) {
	const bool probe_enabled = perf_probe_enabled_;
	const bool skip_fixed_handlers = probe_enabled && perf_probe_skip_fixed_handlers_;
	if (skip_fixed_handlers) {
		return;
	}
	// Retail executes local weapon actions and physical impacts before the same
	// frame's global particle update. Consume each source tick synchronously so
	// admission slots, first emission, and catch-up chronology are exact; only
	// mission render Nodes remain batched until the session frame returns.
	LocalPlayerPresenter *presenter = local_view_presenter();
	if (presenter != nullptr) {
		presenter->present_fixed_weapon_tick(drain_local_player_weapon_events());
	}
	route_terrain_scorches();
	route_round_impacts();
	// The light-pool lifecycle decay + the light_move round-glow follow, on
	// the witnessed 62 Hz cadence [orig: EffectWorld_TickInstancesAndLightScale
	// @ 0x5aa170 from Game_ProcessMainFrame; the round follow @ 0x4eaa9f].
	if (light_director_.is_valid()) {
		light_director_->advance_fixed_tick();
		Ref<Simulation> glow_sim = get_sim();
		if (glow_sim.is_valid()) {
			std::vector<opennova::world::RoundGlowRow> glows;
			glow_sim->fill_round_glows(glows);
			light_director_->sync_round_glows(glows);
		}
	}
	const bool skip_effect_tick = probe_enabled && perf_probe_skip_effect_tick_;
	EffectWorld *effect_world = get_effect_world();
	if (effect_world != nullptr && !skip_effect_tick) {
		if (frame_stats_.is_valid() && frame_stats_->is_capture_active()) {
			const int64_t fx_start = static_cast<int64_t>(Time::get_singleton()->get_ticks_usec());
			effect_world->advance_simulation_tick(Simulation::tick_dt(),
				get_sim().is_valid() ? get_sim()->particle_force_field() : nullptr);
			frame_stats_->add(FrameStats::EFFECTS_TICK,
					static_cast<int64_t>(Time::get_singleton()->get_ticks_usec()) - fx_start);
		} else {
			effect_world->advance_simulation_tick(Simulation::tick_dt(),
				get_sim().is_valid() ? get_sim()->particle_force_field() : nullptr);
		}
	}
}

void GameWorld::on_runtime_simulation_restarted() {
	// A Stop/restart can restore the saved personal slot while the presenter still
	// owns an emplaced model. Consume that control event synchronously; no fixed
	// tick runs while stopped.
	LocalPlayerPresenter *presenter = local_view_presenter();
	if (presenter != nullptr) {
		presenter->present_fixed_weapon_tick(drain_local_player_weapon_events());
	}
	if (terrain_ != nullptr) {
		terrain_->clear_terrain_scorches();
	}
	EffectWorld *effect_world = get_effect_world();
	if (effect_world == nullptr) {
		resync_weather_after_restore();
		return;
	}
	effect_world->reset_runtime_state();
	// Persistent item effects belong to the restored entity set, not the scene
	// that was just discarded. Re-register their admission and owner identities;
	// restore emits fresh controller-start lifecycle events for occupied baselines.
	item_fx_->reattach();
	if (light_director_.is_valid()) {
		light_director_->reattach();
	}
	resync_weather_after_restore();
}

// The restored baseline rewound the World's weather home; the render owner
// snaps its color blocks back onto the restored targets.
void GameWorld::resync_weather_after_restore() {
	Weather *weather = weather_;
	if (weather != nullptr) {
		weather->resync_colors_now();
	}
}

// One wire-spawn router for both directors, subscribed to the entity
// presenter's spawn signal in start_effect_world.
void GameWorld::on_wire_node_spawned(ObjectModel *p_node, int p_kind, int p_item_id) {
	item_fx_->on_wire_node_spawned(p_node, p_kind, p_item_id);
	if (light_director_.is_valid()) {
		light_director_->on_wire_node_spawned(p_node, p_kind, p_item_id);
	}
}
