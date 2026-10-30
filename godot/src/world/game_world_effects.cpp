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
#include "util/string_convert.h"

using namespace godot;

// Fire mission audio for presentation. A BMS PlayWavList ("dialog" effect)
// has already registered its dialog in the world's dialog table, whose lines
// the mission audio plays on the dialog ticks (on_runtime_fixed_tick); the
// effect stays a presentation log. Typed WAC/BMS particle descriptors spawn in
// the engine (open_script_effects). Every kind is still emitted via
// mission_effects for downstream consumers (HUD, etc.).
void GameWorld::route_mission_effects(const Array &p_effects) {
	MissionAudio *audio = get_mission_audio();
	for (int64_t i = 0; i < p_effects.size(); ++i) {
		const Ref<MissionEffect> eff = p_effects[i];
		if (eff.is_null()) {
			continue;
		}
		const String kind = eff->get_kind();
		if (kind == "dialog_wav") {
			// WAC wave/pwave: a scripted voice .wav by filename on its own channel.
			if (audio != nullptr) {
				audio->follow_script_voice();
			}
		}
	}
}

// A co-op host's dialog lines this peer plays (engine: client_effects' typed order,
// resolve_dialog_line), each at once on a voice of its own.
void GameWorld::route_dialog_lines() {
	const Ref<Simulation> sim = get_sim();
	MissionAudio *audio = get_mission_audio();
	if (sim.is_null()) return;
	std::vector<opennova::world::DialogLineOrder> lines;
	sim->drain_dialog_lines(lines);
	if (audio == nullptr) return;
	for (const opennova::world::DialogLineOrder &line : lines)
		audio->play_dialog_line(opennova::to_gd(line.dialog_name), line.line, line.player_class);
}

// The world's script effects and +0x1CC positioned emitters spawn in the engine where their event
// runs, in the tick's order with every other producer of an entity's slot (engine
// world::ItemEmitterSystem::open_producers, D-PTL-26). The shell opens them onto its effect world
// past a load's warm reset and past a restore's scene reset: the queued ones (a mission start's
// WAC/BMS descriptors, a restore's replay) spawn there, in order. The open interns the script
// catalog's names first, so a BMS by-name lookup sees the stock aliases its compile registered.
void GameWorld::open_script_effects() {
    const Ref<Simulation> sim = get_sim();
    EffectWorld *effects = get_effect_world();
    if (sim.is_null() || effects == nullptr) return;
    sim->open_effect_producers();
    // The spawns went straight into the native scene: the render snapshot reads them.
    effects->invalidate_scene_snapshot();
}

// A dedicated host has no effect world (start_effect_world), so its producers never open: its
// script descriptors drain unshown, each tick.
void GameWorld::drain_unshown_script_effects() {
    const Ref<Simulation> sim = get_sim();
    if (sim.is_null() || get_effect_world() != nullptr ||
            sim->session_role() != Simulation::ROLE_DEDICATED_HOST)
        return;
    std::vector<opennova::world::ScriptEffectEvent> events;
    sim->drain_script_effects(events);
}

// Drain the flight sim's resolved round impacts and present their effect and
// light legs. Impact particles are generic Always transients in the world
// domain; their production tick/order and catch-up age survive a multi-tick
// render frame. The impact's sound is not played here: the engine plays it
// where the impact is produced, through the fire-sound queue's distance delay
// (world::play_round_impact_sound), and it reaches the audio bank through the
// fire presenter's drain.
// (engine: runtime/world/round_sim.h)
void GameWorld::route_round_impacts() {
	Ref<Simulation> sim = get_sim();
	if (sim.is_null()) {
		return;
	}
	EffectWorld *effect_world = get_effect_world();
	std::vector<opennova::world::RoundImpactPresentation> rows;
	sim->drain_round_impact_rows(rows);
	for (const opennova::world::RoundImpactPresentation &row : rows) {
		// mission (x,y,z) -> Godot (x, z, -y), the get_local_player_position convention.
		const Vector3 pos = mission_to_godot(row.position);
		if (effect_world != nullptr && !row.effect.empty()) {
			effect_world->spawn_effect_transient(opennova::to_gd(row.effect), pos,
					mission_to_godot(row.direction), static_cast<int64_t>(row.age_ticks),
					EffectScene::RENDER_DOMAIN_WORLD, static_cast<int64_t>(row.source_tick),
					static_cast<int64_t>(row.source_order), row.section_tagged);
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
	std::vector<opennova::world::TerrainPageInvalidationEvent> invalidations;
	sim->drain_terrain_scorches(events, invalidations);
	for (const opennova::world::TerrainPageInvalidationEvent &region : invalidations) {
		// mission (x,y) -> Godot (x,-y), as for the scorch bounds below.
		terrain_->invalidate_tile_cache_region(
				static_cast<int64_t>(region.minimum_x_q16),
				-static_cast<int64_t>(region.maximum_y_q16),
				static_cast<int64_t>(region.maximum_x_q16),
				-static_cast<int64_t>(region.minimum_y_q16));
	}
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

// Route the dialog-line and script-voice effects to mission audio, then expose the
// downstream effects to HUD consumers. The vehicle_control_* lifecycle edges are the
// engine's own (its item emitters run their slot-A leg where the claim edges run,
// world::ItemEmitterSystem::control_started): no HUD consumer reads them.
void GameWorld::on_runtime_effects(const Array &p_effects) {
	Array routed;
	for (int64_t i = 0; i < p_effects.size(); ++i) {
		const Ref<MissionEffect> effect = p_effects[i];
		if (effect.is_valid() && (effect->get_kind() == "vehicle_control_started" ||
										 effect->get_kind() == "vehicle_control_stopped")) {
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
	// The effect groups' section gate reads this simulation's blink volumes
	// and building masks (particle::EffectSectionGate).
	if (EffectWorld *gate_world = get_effect_world()) {
		gate_world->set_section_source(get_sim().ptr());
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
    drain_unshown_script_effects();
	route_dialog_lines();
	// The dialog channel's playback tick, once a logic tick, after this tick's
	// PlayWavList dialogs registered in the world's dialog table: its slots
	// load their lines on their timers (runtime/audio/dialog_queue).
	if (MissionAudio *audio = get_mission_audio()) {
		audio->advance_dialog_tick();
	}
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

// One wire-spawn router, subscribed to the entity presenter's spawn signal in
// start_effect_world: the engine's slot-A attach for the row the node draws (by its
// wire handle; world::ItemEmitterSystem::attach_entity_effects keeps the walk's gates and
// attaches a lifetime once), and the light director's.
void GameWorld::on_wire_node_spawned(ObjectModel *p_node, int p_kind, int p_item_id) {
	const Ref<EntityRef> ref = p_node != nullptr ? p_node->get_entity_ref() : Ref<EntityRef>();
	const Ref<Simulation> sim = get_sim();
	if (ref.is_valid() && sim.is_valid()) {
		sim->attach_item_effects(ref->get_wire_handle());
	}
	if (light_director_.is_valid()) {
		light_director_->on_wire_node_spawned(p_node, p_kind, p_item_id);
	}
}
