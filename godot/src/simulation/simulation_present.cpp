// Simulation — presentation reads: entity/pose getters, the present-effect
// pose cache, the packed present snapshots (AI pool + client replicas), HUD views,
// and the drains (effects, fire, destruction, round impacts, tracers).
#include "simulation/simulation_internal.h"

#include <runtime/world/nvg_laser.h>

#include "simulation/fire_presenter.h" // NvgLaserSource
#include "simulation/hud_view_records.h"
#include "simulation/destruction_events.h"

#include "simulation/entity_card.h" // the typed per-entity debug card (ADR 0042 d5)
#include "simulation/entity_row.h"  // one typed entity-directory row
#include "rtxt/rtxt_string_file.h" // the mission text table the objectives fill resolves through

#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/inmatch/client_replica_card.h> // the joiner's decoded replica section
#include <runtime/inmatch/minimap_markers.h> // the retained marker rows (bank walk + local restore)
#include <runtime/inmatch/minimap_overlays.h> // the non-bank map legs' feed
#include <runtime/inmatch/napi_np_server_ctx.h> // the authority's location table
#include <runtime/hud/hud_frame.h>  // HudObjectiveRow
#include <runtime/hud/hud_minimap_feed.h>  // the feed layout the snapshot carries
#include <runtime/inmatch/role_feeds.h>
#include <runtime/inmatch/present_rows.h> // the PF_* row collectors, both roles (ADR 0043 G3)

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <runtime/replication/client_state.h> // minimap_team_argb (the ONE palette home)
#include <runtime/replication/entity_wire_bridge.h> // entity_class_of / player_wire_net_id (the host's own rows)
#include <runtime/world/zone_chain.h> // zone_chain_zone_info_byte
#include <net/npwire/ingame_decode.h> // kRoundEventFlag* (the fire-mode byte)
#include <runtime/world/minimap_footprint.h> // the OOBJ occlusion ground-slice footprint mesh
#include <runtime/world/minimap_overlay.h>   // classifier + the blip draw policy
#include <runtime/renderer/tracer_frame.h> // the styled tracer-ribbon compile
#include <runtime/world/entity.h> // kEntityFlag* (the wire state_flags byte IS entity+36 low)
#include <runtime/world/present_drains.h> // the present-pass row fills (ADR 0040 ladder E0)
#include <runtime/world/objectives_feed.h> // the SP objectives panel's row walk (ADR 0040 ladder E0)

using namespace sim_internal;


void Simulation::advance_facial_presentation(const Vector3 &p_camera) {
    if (!kernel_ || !p_camera.is_finite()) return;
    std::vector<opennova::world::FacialDraw> rows;
    kernel_->world.facials.compile_draws(kernel_->world,
            static_cast<int32_t>(p_camera.x * 65536.0),
            static_cast<int32_t>(-p_camera.z * 65536.0),
            kernel_->local.view.camera_mode == 0, rows);
    // JO renders these targets then writes two unused globals. The complete
    // .text/.rdata/.data audit found no texture sink; model materials retain
    // their normal textures. Keep the frame counter/priority state live.
    // [orig: sub_580360 @0x580360; sub_580030 @0x580030;
    //  write-only globals @0x272137C/@0x2721380, world-wac-ai-re §33.30]
}

void Simulation::fill_minefield_draw_rows(
        std::vector<opennova::world::MinefieldDraw> &r_rows) const {
    r_rows.clear();
    if (kernel_) kernel_->world.minefields.compile_draws(kernel_->world, r_rows);
}

void Simulation::fill_throwable_visual_rows(
		std::vector<opennova::world::ThrowableVisualRow> &r_rows) const {
	r_rows.clear();
	if (!kernel_) return;
	opennova::world::fill_throwable_visual_rows(kernel_->world, r_rows);
}

TypedArray<ThrowableVisualRow> Simulation::get_throwable_visuals() const {
	TypedArray<ThrowableVisualRow> out;
	std::vector<opennova::world::ThrowableVisualRow> rows;
	fill_throwable_visual_rows(rows);
	for (const opennova::world::ThrowableVisualRow &row : rows) {
		Ref<ThrowableVisualRow> d;
		d.instantiate();
		d->assign(row);
		out.push_back(d);
	}
	return out;
}

const opennova::particle::ParticleForceField *Simulation::particle_force_field() const {
	return kernel_ ? &kernel_->world.rotor_wash : nullptr;
}

void Simulation::fill_vehicle_trail_visual_rows(
		std::vector<opennova::world::VehicleTrailVisualRow> &r_rows) const {
	r_rows.clear();
	if (!kernel_)
		return;
	opennova::world::fill_vehicle_trail_visual_rows(kernel_->world, r_rows);
}

Ref<WaypointHudView> Simulation::get_waypoint_hud_view() const {
	// The record converts the entry's fixed 16.16 mission (x,y,z) to Godot
	// (x, z, -y), like every entity read; no track (no kernel) reads as the
	// default view.
	Ref<WaypointHudView> out;
	out.instantiate();
	out->assign(kernel_ ? opennova::world::waypoint_hud_view(kernel_->world.script.waypoints,
								  &kernel_->world.registry)
					   : opennova::world::WaypointHudView{});
	return out;
}

Ref<HudMapGridOrigin> Simulation::get_hud_map_grid_origin() const {
	// The authority's promoted marker or the joiner's decoded pool-3 row
	// (inmatch/role_feeds.h).
	Ref<HudMapGridOrigin> out;
	out.instantiate();
	out->assign(opennova::inmatch::hud_map_grid_origin(role_view()));
	return out;
}

PackedInt32Array Simulation::get_hud_minimap_snapshot() const {
	const opennova::world::Entity *local_player = kernel_ != nullptr
			? kernel_->world.registry.get(kernel_->world.cached.local_player)
			: nullptr;
	const uint16_t local_marker_handle = local_player != nullptr
			? static_cast<uint16_t>(get_local_player_wire_handle())
			: opennova::world::EntityHandle::kInvalid;
	// Between 62 Hz logic ticks every input is unchanged (the retained banks
	// bump ClientMinimapState::revision; entity resolves, policies, and the
	// local row advance only with the tick), so display frames reuse the
	// built array. The baseline restore invalidates across epochs.
	const uint64_t revision =
			runtime_ ? runtime_->state().minimap.revision : 0;
	const uint64_t tick = kernel_ != nullptr
			? static_cast<uint64_t>(kernel_->world.logic_tick) : 0;
	if (present_.minimap_snapshot_valid && revision == present_.minimap_snapshot_revision &&
			tick == present_.minimap_snapshot_tick &&
			local_marker_handle == present_.minimap_snapshot_local_handle) {
		return present_.minimap_snapshot_cache;
	}
	// The rows (the bank walk, the policy resolve, the restored local row) and
	// the feed layout are the engine's; this leg only packs the array.
	opennova::inmatch::MinimapMarkerInputs in;
	in.map = runtime_ ? &runtime_->state().minimap : nullptr;
	in.world = &kernel_->world;
	in.local_marker_handle = local_marker_handle;
	in.local_heading_bam = static_cast<int32_t>(get_local_player_heading_bam());
	// The zone-timer list the rows' v7 entry reads (the joiner's own, the
	// authority's HostClient loopback; none on the bare local role).
	in.zone_timers = runtime_ ? &runtime_->zone_states() : nullptr;
	std::vector<opennova::hud::HudMinimapMarker> markers;
	opennova::inmatch::build_minimap_markers(in, markers);
	std::vector<int32_t> feed;
	opennova::hud::minimap_feed_encode(markers, feed);
	PackedInt32Array out;
	out.resize(static_cast<int64_t>(feed.size()));
	std::copy(feed.begin(), feed.end(), out.ptrw());
	present_.minimap_snapshot_cache = out;
	present_.minimap_snapshot_revision = revision;
	present_.minimap_snapshot_tick = tick;
	present_.minimap_snapshot_local_handle = local_marker_handle;
	present_.minimap_snapshot_valid = true;
	return out;
}

Ref<HudMapOverlays> Simulation::get_hud_minimap_overlays(
		const Ref<RtxtStringFile> &p_gametext) const {
	// The feed's gather and selection rules are the engine's
	// (inmatch/minimap_overlays.h); this leg only binds the role's state.
	// Between logic ticks every input is unchanged, so display frames reuse
	// the gathered record.
	const uint64_t revision = runtime_ ? runtime_->state().minimap.revision : 0;
	const uint64_t tick = kernel_ ? static_cast<uint64_t>(kernel_->world.logic_tick) : 0;
	const uint64_t gametext_id = p_gametext.is_valid() ? p_gametext->get_instance_id() : 0;
	if (present_.minimap_overlays_valid && present_.minimap_overlays_cache.is_valid() &&
			revision == present_.minimap_overlays_revision &&
			tick == present_.minimap_overlays_tick &&
			gametext_id == present_.minimap_overlays_gametext)
		return present_.minimap_overlays_cache;
	Ref<HudMapOverlays> out;
	out.instantiate();
	if (!kernel_) return out;
	const opennova::inmatch::RoleView view = role_view();
	opennova::inmatch::MinimapOverlayInputs in;
	in.client = runtime_ ? &runtime_->state() : nullptr;
	in.world = &kernel_->world;
	in.game_type = runtime_ ? static_cast<int32_t>(runtime_->game_type()) : 0;
	in.rules_word = (view.joiner && runtime_) ? runtime_->view().mp_attributes()
			: view.staged_mp_attributes;
	in.authority_location_names =
			view.host != nullptr ? &view.host->mission_location_names : nullptr;
	in.gametext = game_text_lookup(p_gametext);
	if (runtime_ && runtime_->has_self_handle()) in.self_handle = runtime_->self_handle();
	opennova::hud::HudMinimapOverlays value;
	opennova::inmatch::build_minimap_overlays(in, value);
	out->assign(std::move(value));
	present_.minimap_overlays_cache = out;
	present_.minimap_overlays_revision = revision;
	present_.minimap_overlays_tick = tick;
	present_.minimap_overlays_gametext = gametext_id;
	present_.minimap_overlays_valid = true;
	return out;
}

void Simulation::set_hud_radar_gates(int p_gates) {
	session_.set_hud_radar_gates(static_cast<uint32_t>(p_gates));
}

PackedInt32Array Simulation::get_hud_radar() const {
	// The frame's contact update, lock tone, snapshot, missile-count clear and
	// the map banks' aging ran on the session's frame (inmatch/session.h, the
	// radar step); this leg only packs its snapshot.
	std::vector<int32_t> feed;
	opennova::hud::radar_feed_encode(session_.hud_radar(), feed);
	PackedInt32Array out;
	out.resize(static_cast<int64_t>(feed.size()));
	std::copy(feed.begin(), feed.end(), out.ptrw());
	return out;
}

PackedInt32Array Simulation::get_hud_minimap_footprints() const {
	// Static footprint polygons for every visible footprint-class entity:
	// the OOBJ occlusion ground-slice mesh transformed by the entity pose, in
	// the witnessed fill colors and the overlay ctx alpha (witness at
	// world::minimap_footprint_fill_argb) with the occlusion ground-slice
	// mesh placed by the entity pose (world::minimap_footprint_place).
	PackedInt32Array out;
	out.push_back(1); // feed version
	out.push_back(0); // row count, patched below
	if (!kernel_) return out;
	int count = 0;
	std::unordered_map<int32_t, opennova::world::MinimapFootprintMesh> meshes;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &entity) {
		if (!opennova::world::minimap_overlay_entity_enabled(entity)) return;
		const opennova::world::MinimapOverlayClassification row =
				opennova::world::classify_minimap_overlay(entity, &kernel_->world);
		if (!row.visible) return;
		const opennova::world::MinimapBlipDrawPolicy policy =
				opennova::world::minimap_blip_draw_policy(entity, row.icon);
		if (!policy.footprint) return;
		const int32_t model_id = kernel_->occlusion.instance_model_id(
				entity.handle);
		if (model_id < 0) return;
		auto mesh_it = meshes.find(model_id);
		if (mesh_it == meshes.end()) {
			const opennova::world::OcclusionModel *model =
					kernel_->occlusion.model(model_id);
			if (model == nullptr) return;
			mesh_it = meshes.emplace(model_id,
					opennova::world::minimap_footprint_from_occlusion(
							*model)).first;
		}
		const opennova::world::MinimapFootprintMesh &mesh = mesh_it->second;
		if (mesh.empty()) return;
		// Color + placement live engine-side (world::minimap_footprint_*):
		// the same yaw-degree -> BAM placement matrix the collision instance
		// uses, so footprint, model, and shell agree.
		std::vector<int32_t> fill_xy;
		std::vector<int32_t> edge_xy;
		opennova::world::minimap_footprint_place(mesh, entity, fill_xy,
				edge_xy);
		out.push_back(entity.handle.packed);
		out.push_back(static_cast<int32_t>(
				opennova::world::minimap_footprint_fill_argb(entity)));
		out.push_back(static_cast<int32_t>(fill_xy.size()));
		for (const int32_t value : fill_xy) out.push_back(value);
		out.push_back(static_cast<int32_t>(edge_xy.size()));
		for (const int32_t value : edge_xy) out.push_back(value);
		++count;
	});
	out.set(1, count);
	return out;
}

void Simulation::fill_objectives(const Ref<RtxtStringFile> &p_mission_text,
		std::vector<opennova::hud::HudObjectiveRow> &r_rows) const {
	r_rows.clear();
	if (!kernel_) return;
	opennova::world::fill_objective_rows(kernel_->world, game_text_lookup(p_mission_text), r_rows);
}
// The round-impact drain (world/present_drains.h drain_round_impact_rows).
void Simulation::drain_round_impact_rows(
		std::vector<opennova::world::RoundImpactPresentation> &r_rows) {
	r_rows.clear();
	if (!kernel_) return;
	opennova::world::drain_round_impact_rows(kernel_->world, r_rows);
}

TypedArray<RoundImpactRow> Simulation::drain_round_impacts() {
	TypedArray<RoundImpactRow> out;
	std::vector<opennova::world::RoundImpactPresentation> rows;
	drain_round_impact_rows(rows);
	for (const opennova::world::RoundImpactPresentation &row : rows) {
		Ref<RoundImpactRow> d;
		d.instantiate();
		d->assign(row);
		out.push_back(d);
	}
	return out;
}

void Simulation::drain_terrain_scorches(
		std::vector<opennova::world::TerrainScorchEvent> &r_events,
		std::vector<opennova::world::TerrainPageInvalidationEvent> &r_invalidations) {
	r_events.clear();
	r_invalidations.clear();
	if (!kernel_) return;
	// The events carry the mission 16.16 bounds; the terrain consumer folds
	// mission (x,y,z) -> Godot (x,z,-y) as it inserts them.
	r_events = kernel_->world.out.terrain_scorches.pending();
	r_invalidations = kernel_->world.out.terrain_scorches.pending_page_invalidations();
	kernel_->world.out.terrain_scorches.clear_pending();
}

void Simulation::bind_item_effect_scene(std::shared_ptr<opennova::particle::EffectScene> scene) {
    kernel_->world.item_emitters.bind_scene(std::move(scene));
}

std::vector<std::string> Simulation::script_effect_names() const {
    return world_installed_ ? kernel_->script_effect_catalog.interned_names() : std::vector<std::string>{};
}

void Simulation::drain_script_effects(std::vector<opennova::world::ScriptEffectEvent> &events) {
    events.clear();
    if (!world_installed_) return;
    events.swap(kernel_->world.out.script_effects);
}

TypedArray<MissionEffect> Simulation::drain_effects() {
	TypedArray<MissionEffect> out;
	if (!world_installed_) return out;
	for (const opennova::world::Effect &e : kernel_->world.out.effects.entries()) {
		// The vehicle_control_* lifecycle edges carry the wire handle in d;
		// every other kind leaves the alias absent.
		const bool control_edge = e.kind == "vehicle_control_started" ||
				e.kind == "vehicle_control_stopped";
		Ref<MissionEffect> d;
		d.instantiate();
		d->assign(e, control_edge ? e.d : -1);
		out.push_back(d);
	}
	kernel_->world.out.effects.clear();
	return out;
}

// The fire-presentation drain (world/present_drains.h drain_fire_presentation_rows);
// the rows cross in the mission frame and the consumer axis-maps mission -> godot.
void Simulation::drain_fire_presentation_rows(
		std::vector<opennova::world::FirePresentationRow> &r_rows) {
	r_rows.clear();
	if (!world_installed_) return;
	opennova::world::drain_fire_presentation_rows(kernel_->world, r_rows);
}

TypedArray<FirePresentationEvent> Simulation::drain_fire_presentation_events() {
	TypedArray<FirePresentationEvent> out;
	std::vector<opennova::world::FirePresentationRow> rows;
	drain_fire_presentation_rows(rows);
	for (const opennova::world::FirePresentationRow &row : rows) {
		Ref<FirePresentationEvent> d;
		d.instantiate();
		d->assign(row);
		out.push_back(d);
	}
	return out;
}

// world/fire_sound.h mirrors the npwire arm bits so the sim's seed can split
// the arms without linking the net stack — pin the pairing here, where both
// headers are visible (the S7a weapon_flag pattern).
static_assert(opennova::world::round_event_flag::kAltFire ==
				opennova::kRoundEventFlagAltFire,
		"round_event_flag::kAltFire must match the npwire decoder bit");
static_assert(opennova::world::round_event_flag::kAdmIndexed ==
				opennova::kRoundEventFlagAdmIndexed,
		"round_event_flag::kAdmIndexed must match the npwire decoder bit");

uint8_t Simulation::sound_listener_view_flags() const {
	return kernel_->world.cached.sound_listener_view_flags;
}

// The presenting shell's listener stamp — the camera position, once per frame
// before the tick batch, feeding the sim's fire-sound distance gate
// [orig: listener_pos @ 0x24D6630; world/fire_sound.h]. Never called on a
// dedicated host, which is the witnessed peer gate.
void Simulation::set_sound_listener(const Vector3 &p_listener_godot) {
	if (!world_installed_) return;
	kernel_->world.out.fire_sounds.set_listener(opennova::world::Vec3{
			p_listener_godot.x, -p_listener_godot.z, p_listener_godot.y});
}

// The ready fire-sound drain: immediate near shots, the adm-arm action-row
// sets, and expired propagation-delayed slots, in play order on the logic
// clock. The shell plays each row positionally; the set's max-range cull
// stays at play time in the audio bank (D-AI-8).
// [orig: Entity_PlaySound3D_FullVolume @ 0x528e20 / the pending drain
//  Sound_TickPendingSlots @ 0x529310]
void Simulation::drain_fire_sounds(std::vector<opennova::world::ReadyFireSound> &r_sounds) {
	r_sounds.clear();
	if (!world_installed_) return;
	// Mission-space rows; the fire pass axis-maps (x, z, -y) as it plays them.
	r_sounds = kernel_->world.out.fire_sounds.drain();
}

// The destruction presentation drain (world/destruction.h; §24): one call per
// present, handing the sim's events to the C++ pass whole (the counters ride
// along: `crackles` is the wreck-fire crackle rolls fired, S12b). The events
// cross in mission space; the pass axis-maps mission (x, y, z-up) -> Godot
// (x, z, -y), the drain_fire_presentation_rows rule.
void Simulation::drain_vehicle_effects(std::vector<opennova::world::VehicleEffectEvent> &r_events) {
	r_events.clear();
	if (world_installed_)
		r_events.swap(kernel_->world.out.vehicle_effects);
}

void Simulation::drain_destruction_events(opennova::world::DestructionEvents &r_events) {
	r_events.clear();
	if (!world_installed_) return;
	opennova::world::DestructionEvents &ev = kernel_->world.out.destruction;
	r_events.effects.swap(ev.effects);
	r_events.sounds.swap(ev.sounds);
	r_events.husk_swaps.swap(ev.husk_swaps);
	r_events.death_lights.swap(ev.death_lights);
	r_events.explosions_processed = ev.explosions_processed;
	r_events.items_destroyed = ev.items_destroyed;
	r_events.crackles = ev.crackles;
	r_events.debris_triangles = ev.debris_triangles;
	r_events.glass_points = ev.glass_points;
	ev.clear();
}

// The live death-piece pool snapshot (world/present_drains.h fill_death_pieces).
void Simulation::fill_death_pieces(std::vector<opennova::world::DeathPieceRow> &r_pieces) const {
	r_pieces.clear();
	if (!world_installed_) return;
	opennova::world::fill_death_pieces(kernel_->world, r_pieces);
}

const std::vector<opennova::world::DeathPieceDraw> &Simulation::death_piece_draws() const {
	return present_.death_piece_draws;
}

// The visible-person list retail walks holds every collected person; the rows
// here are every decoded person, and the beam leg keeps the drawn bodies. A
// row's entity+0x298 is its equipped AdmDef entry (the client's player-record
// store; 0xFF names none) [retail NetPacket_SerializePlayerState @ 0x4c11f2 /
// @ 0x4c120d].
void Simulation::nvg_laser_sources(std::vector<NvgLaserSource> &r_sources) const {
	r_sources.clear();
	if (!world_installed_ || runtime_ == nullptr) return;
	const uint16_t self_handle = runtime_->has_self_handle()
			? runtime_->self_handle()
			: opennova::world::EntityHandle::kInvalid;
	for (const opennova::replication::ClientEntityState &es : runtime_->state().entities) {
		if (es.cls != opennova::EntityClass::Player && es.cls != opennova::EntityClass::Infantry)
			continue;
		const opennova::world::WeaponTableEntry *def =
				kernel_->world.tables.weapons.by_index(es.equipped_adm_index);
		NvgLaserSource source;
		source.handle = es.handle;
		source.gate.attach_bone = es.mount_bone;
		source.gate.has_weapon_def = def != nullptr;
		source.gate.weapon_flags = def != nullptr ? def->flags : 0;
		source.gate.local_player = es.handle == self_handle;
		source.launch_userpoint = def != nullptr ? def->launch_userpoint : 0;
		r_sources.push_back(source);
	}
}

// A beam's ray clip through the static then pool-1 walks, Q16 (world/nvg_laser.h).
int32_t Simulation::nvg_laser_clip_distance(int p_handle, const int32_t p_origin[3],
		const int32_t p_dir[3]) const {
	if (!world_installed_) return opennova::world::kNvgLaserRangeQ16;
	return opennova::world::nvg_laser_clip_distance(kernel_->collision, kernel_->world,
			opennova::world::EntityHandle{static_cast<uint16_t>(p_handle)}, p_origin, p_dir);
}

// Whether the collision world holds an instance for the placed entity: the
// one destruction-gate fact the GUT collision cases read by bms_id.
bool Simulation::has_collision_instance(int p_bms_id) const {
	if (!kernel_) return false;
	const opennova::world::Entity *found = nullptr;
	kernel_->world.registry.for_each([&](const opennova::world::Entity &e) {
		if (found == nullptr && e.bms_id == p_bms_id) found = &e;
	});
	if (found == nullptr) return false;
	return kernel_->collision.has_instance(kernel_->world, found->handle);
}

// The live tracer trail channels for the ribbon layer — see the header note.
// Mission -> godot axis map (x, z, -y), matching the other presentation drains.
PackedFloat32Array Simulation::get_tracer_trails() const {
	PackedFloat32Array out;
	if (!world_installed_) return out;
	for (const opennova::world::TracerTrailChannel &c : kernel_->world.round_sim.trails.channels) {
		if (!c.active || c.count <= 0) continue;
		const int64_t base = out.size();
		out.resize(base + 3 + static_cast<int64_t>(c.count) * 4);
		float *w = out.ptrw() + base;
		w[0] = static_cast<float>(c.style_id);
		w[1] = static_cast<float>(c.age);
		w[2] = static_cast<float>(c.count);
		float *pw = w + 3;
		for (int i = 0; i < c.count; ++i, pw += 4) {
			const opennova::world::TracerTrailPoint &p = c.pts[static_cast<size_t>(i)];
			pw[0] = p.pos.x;
			pw[1] = p.pos.z;
			pw[2] = -p.pos.y;
			pw[3] = p.w;
		}
	}
	return out;
}

// The in-flight round glows — see the header note. One row per active round
// whose ammo authors `light_move`; the id is the round's presentation
// generation so pool-slot reuse never teleports a glow.
void Simulation::fill_round_glows(std::vector<opennova::world::RoundGlowRow> &r_rows) const {
	r_rows.clear();
	if (!world_installed_) return;
	opennova::world::fill_round_glows(kernel_->world, r_rows);
}

// The typed entity inspection API (ADR 0042 d5): the directory join and the
// per-entity card are engine facts (world/inspect.h); this binding forwards
// and converts into the typed records. The joiner's decoded replica section
// is the inmatch card (runtime/inmatch/client_replica_card.h).
TypedArray<EntityRow> Simulation::entity_directory() const {
	TypedArray<EntityRow> out;
	if (!kernel_) return out;
	const std::vector<opennova::world::inspect::EntityRow> rows = native_entity_directory();
	for (const opennova::world::inspect::EntityRow &row : rows) {
		Ref<EntityRow> typed;
		typed.instantiate();
		typed->assign(row);
		out.push_back(typed);
	}
	return out;
}

std::vector<opennova::world::inspect::EntityRow> Simulation::native_entity_directory() const {
	if (!kernel_) return {};
	// A joiner never mixes its non-authoritative tooling AI pool into the
	// decoded view; the host joins registry rows to their AI cards.
	return opennova::world::inspect::entity_directory(kernel_->world, /*with_brains=*/!is_joiner());
}

opennova::world::inspect::EntityCard Simulation::native_entity_card(int p_handle) const {
	if (!kernel_ || p_handle < 0 || p_handle > 0xFFFF) return {};
	// The directory's rule: a joiner's non-authoritative tooling AI pool never
	// joins the decoded view, so the F3 card shows no AI half for a row the
	// list beside it calls brainless.
	return opennova::world::inspect::build_entity_card(
			kernel_->world, /*with_brains=*/!is_joiner(),
			opennova::world::EntityHandle{static_cast<uint16_t>(p_handle)},
			[this](int32_t adm_id) { return kernel_->root_motion.adm_name(adm_id); });
}

Ref<EntityCard> Simulation::entity_card(int p_handle) const {
	if (!kernel_ || p_handle < 0 || p_handle > 0xFFFF) return Ref<EntityCard>();
	const opennova::world::EntityHandle handle{static_cast<uint16_t>(p_handle)};
	Ref<EntityCard> card;
	card.instantiate();
	// The MCP/test card keeps both halves on every role (its joiner readers
	// diff the tooling pool against the replica section).
	card->assign(opennova::world::inspect::build_entity_card(
			kernel_->world, /*with_brains=*/true, handle,
			[this](int32_t adm_id) { return kernel_->root_motion.adm_name(adm_id); }));
	// The joiner's decoded replica row for the same handle, when one exists.
	if (is_joiner() && runtime_ != nullptr) {
		card->assign_replica(opennova::inmatch::client_replica_card(
				runtime_->state(), handle.packed));
	}
	return card->native_valid() ? card : Ref<EntityCard>();
}

Ref<EntityCard> Simulation::entity_card_by_ai_index(int p_index) const {
	if (!kernel_) return Ref<EntityCard>();
	const AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return Ref<EntityCard>();
	return entity_card(static_cast<int>(e->handle.packed));
}

Ref<EntityCard> Simulation::entity_card_by_net_id(int p_net_id) const {
	if (!kernel_ || p_net_id <= 0 || p_net_id > 0xFFFF) return Ref<EntityCard>();
	const opennova::world::EntityHandle h = kernel_->world.registry.find_by_net_id(
			static_cast<uint16_t>(p_net_id));
	if (!h.valid()) return Ref<EntityCard>();
	return entity_card(static_cast<int>(h.packed));
}

String Simulation::ai_state_name(int p_state) {
	return String(opennova::world::ai_state_name(p_state));
}

String Simulation::infantry_anim_key(int p_state) {
	return String(opennova::world::infantry_anim_key(p_state).c_str());
}

int64_t Simulation::infantry_anim_flags(int p_state) {
	if (p_state < 0 || p_state >= opennova::world::kInfantryAnimStateCount) return 0;
	return static_cast<int64_t>(opennova::world::kInfantryAnimFlags[p_state]);
}

int Simulation::get_entity_count() const {
	return kernel_ ? kernel_->world.ai.count() : 0;
}

int Simulation::get_entity_kind(int p_index) const {
	if (!kernel_) return -1;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return -1;
	const opennova::world::Entity *ent = kernel_->world.registry.get(e->handle);
	if (!ent) return -1;
	return opennova::world::spawn_origin_kind(ent->spawn_origin); // [orig promote: (kind<<24)|index]
}

Vector3 Simulation::get_entity_position(int p_index) const {
	if (!kernel_) return Vector3();
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return Vector3();
	// mission (x, y, z) 16.16 -> Godot (x, z, -y) world units. [orig render remap: (x, z, -y).]
	return Vector3(static_cast<float>(e->pos[0] / kFixed16),
	               static_cast<float>(e->pos[2] / kFixed16),
	               static_cast<float>(-e->pos[1] / kFixed16));
}

float Simulation::get_entity_yaw_deg(int p_index) const {
	if (!kernel_) return 0.0f;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return 0.0f;
	return static_cast<float>(opennova::world::mission_yaw_deg_from_bam_heading(e->heading));
}

int Simulation::get_entity_state(int p_index) const {
	if (!kernel_) return 0;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kCurState];
}

int Simulation::get_entity_net_id(int p_index) const {
	if (!kernel_) return 0;
	AiEntity *e = kernel_->world.ai.at(p_index);
	return e ? e->net_id : 0;
}

// The distant MODEL/depth-mask foliage tier's anchors of the last occlusion
// frame (run_occlusion_frame carries the witness).
PackedVector3Array Simulation::get_foliage_mask_anchor_positions() const {
	return present_.foliage_mask_anchors;
}

PackedVector3Array Simulation::get_entity_effect_state_for_ssn(int p_ssn) const {
	PackedVector3Array out;
	if (kernel_ == nullptr || p_ssn <= 0 ||
			p_ssn > static_cast<int>(std::numeric_limits<std::uint16_t>::max())) {
		return out;
	}
	const opennova::world::Entity *entity = kernel_->world.registry.get(
			kernel_->world.registry.find_by_net_id(static_cast<std::uint16_t>(p_ssn)));
	if (entity == nullptr) return out;

	out.resize(EFFECT_STATE_COUNT);
	out.set(EFFECT_STATE_POSITION, Vector3(
			entity->position.x, entity->position.z, -entity->position.y));
	out.set(EFFECT_STATE_ROTATION_DEG, Vector3(
			static_cast<float>(entity->pitch),
			static_cast<float>(entity->yaw),
			static_cast<float>(entity->roll)));
	return out;
}

void Simulation::invalidate_present_effect_pose_cache() const {
	present_.effect_poses.invalidate();
}

namespace {

// The engine pose (mission units, degrees) as the presenter's state pair:
// the Godot-space position and the (pitch, yaw, roll) rotation.
PackedVector3Array effect_state_of(const opennova::inmatch::EffectPose *p_pose) {
	PackedVector3Array out;
	if (p_pose == nullptr) return out;
	out.resize(Simulation::EFFECT_STATE_COUNT);
	out.set(Simulation::EFFECT_STATE_POSITION, Vector3(p_pose->x, p_pose->z, -p_pose->y));
	out.set(Simulation::EFFECT_STATE_ROTATION_DEG,
			Vector3(p_pose->pitch_deg, p_pose->yaw_deg, p_pose->roll_deg));
	return out;
}

} // namespace

// The lazily indexed pose an attached effect follows, by every identity an
// attachment names (inmatch/effect_pose_index.h carries the rules: the
// joiner self-filter, the wire-only identity on a joiner, the per-epoch
// misses).
PackedVector3Array Simulation::present_effect_state_for_handle(uint16_t p_handle) const {
	return effect_state_of(present_.effect_poses.for_handle(role_view(), p_handle));
}

PackedVector3Array Simulation::get_present_effect_state_for_ssn(int p_ssn) const {
	return effect_state_of(present_.effect_poses.for_ssn(role_view(), p_ssn));
}

PackedVector3Array Simulation::get_present_effect_state_for_wire_handle(
		int p_wire_handle) const {
	if (p_wire_handle < 0 ||
			p_wire_handle > static_cast<int>(std::numeric_limits<uint16_t>::max())) {
		return PackedVector3Array();
	}
	return present_effect_state_for_handle(static_cast<uint16_t>(p_wire_handle));
}

PackedVector3Array Simulation::get_present_effect_state_for_bms_id(int p_bms_id) const {
	return effect_state_of(
			present_.effect_poses.for_bms_id(role_view(), p_bms_id, present_.bms_handles));
}

PackedVector3Array Simulation::get_present_effect_state_for_origin(
		int p_kind, int p_index) const {
	return effect_state_of(present_.effect_poses.for_origin(role_view(), p_kind, p_index));
}

// [D-NET-112] entity+0x78 ownerConnectionId (the connection/dcb that owns this entity). A networked
// PLAYER is identified by this + its handle, NOT by an SSN (players carry net_id 0). 0 = unowned (AI /
// mission entity / the host's dedicated reservation).
int Simulation::get_entity_owner_connection_id(int p_index) const {
	if (!kernel_) return 0;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return 0;
	const opennova::world::Entity *ent = kernel_->world.registry.get(e->handle);
	return ent ? static_cast<int>(ent->owner_connection_id) : 0;
}

// The entity's wire handle (pool<<12|slot) — the per-entity identity carried on the 0x0A/0x0C wire and
// the decoded present's PF_WIRE_HANDLE. Unique per entity (unlike a player's net_id, which is now 0).
int Simulation::get_entity_wire_handle(int p_index) const {
	if (!kernel_) return 0;
	AiEntity *e = kernel_->world.ai.at(p_index);
	return e ? static_cast<int>(e->handle.packed) : 0;
}

int32_t Simulation::decode_present_part_anim_phase(
		const PackedFloat32Array &p_snapshot, int p_base, int p_channel) {
	return opennova::world::decode_present_part_anim_phase(
			{p_snapshot.ptr(), p_snapshot.size()}, p_base, p_channel);
}

int Simulation::get_entity_part_anim_phase(int p_index, int channel) const {
	if (!kernel_ || channel < 1 || channel > 2) return 0;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return 0;
	return e->brain.f[AiBrain::kPartAnimPhase0 + (channel - 1)];
}

bool Simulation::get_entity_part_anim_active(int p_index, int channel) const {
	if (!kernel_ || channel < 1 || channel > 2) return false;
	AiEntity *e = kernel_->world.ai.at(p_index);
	if (!e) return false;
	if (channel == 1 && kernel_ != nullptr) {
		const opennova::world::Entity *entity =
				kernel_->world.registry.get(e->handle);
		if (entity != nullptr && (entity->item_attrib & 0x1000u) != 0)
			return false;
	}
	return true;
}

std::shared_ptr<const SimulationPresentSnapshot> Simulation::build_present_snapshot() const {
	const uint64_t start_us =
			runtime_profiling_enabled_ ? opennova::io::perf_now_us() : 0;
	// ADR 0011 Decision 1 (as amended, D-NET-140 closed): every authoritative live mission is an
	// in-process listen server in standalone MainGame/GameWorld, and the listen host presents
	// from its OWN pools — retail's local client reads process memory and its loopback 0x0A is
	// header-only [orig: NetPacket_SerializeEntityStatesToPacket @0x50f07e;
	// Terrain_CollectVisibleEntitiesForTerrain @0x5c8c60]. A joiner renders the host's stream
	// wire-direct from the state its ClientReplicaPipeline decoded (ClientState).
	// Empty when no runtime is active (a bare sim) — scalar getters (get_entity_*) read the
	// AI pool for tooling.
	if (!present_.snapshot || present_.snapshot.use_count() != 1)
		present_.snapshot = std::make_shared<SimulationPresentSnapshot>();
	auto &frame = *present_.snapshot;
	frame.rows.clear();
	frame.door_phases.clear();
	if (runtime_ && kernel_) {
		const opennova::inmatch::PresentRowsContext context{*kernel_, runtime_, is_joiner(),
				host_ctx()};
		if (is_joiner()) {
			opennova::inmatch::build_client_replica_present_rows(
					context, present_.pool_lifecycle, frame.rows, frame.door_phases);
			// Consume-once: each transition pulse dispatches exactly one presented
			// frame (the rows copied any live pulse into PF_ANIM_STATE_PULSE).
			runtime_->state().clear_anim_pulses();
		} else {
			opennova::inmatch::build_world_present_rows(context, present_.pool_lifecycle,
					frame.rows, frame.door_phases);
		}
	}
	present_.last_entity_count = static_cast<int>(frame.rows.size() / PF_STRIDE);
	// Reuse the identity storage and compare exact fields, never a hash.
	bool changed = present_.layout.size() != static_cast<size_t>(present_.last_entity_count);
	present_.layout.resize(static_cast<size_t>(present_.last_entity_count));
	for (int i = 0; i < present_.last_entity_count; ++i) {
		const float *row = frame.rows.data() + static_cast<int64_t>(i) * PF_STRIDE;
		const PresentRowIdentity identity{
				static_cast<int32_t>(row[PF_WIRE_HANDLE]),
				static_cast<int32_t>(row[PF_TYPE_ID]),
				static_cast<int32_t>(row[PF_BMS_ID]),
				static_cast<int32_t>(row[PF_KIND]),
				static_cast<int32_t>(row[PF_INDEX])};
		if (!(present_.layout[static_cast<size_t>(i)] == identity)) {
			present_.layout[static_cast<size_t>(i)] = identity;
			changed = true;
		}
	}
	if (changed) ++present_.layout_revision;
	frame.layout_revision = present_.layout_revision;
	if (runtime_profiling_enabled_)
		present_.last_snapshot_us = opennova::io::perf_now_us() - start_us;
	return present_.snapshot;
}

bool Simulation::local_player_person_overlays(opennova::world::PersonOverlays &r_out) const {
	r_out = opennova::world::PersonOverlays{};
	if (!kernel_) return false;
	const opennova::inmatch::PresentRowsContext context{*kernel_, runtime_, is_joiner()};
	return opennova::inmatch::local_player_person_overlays(context, r_out);
}

PackedFloat32Array Simulation::get_present_snapshot() const {
	const auto frame = build_present_snapshot();
	PackedFloat32Array out;
	out.resize(static_cast<int64_t>(frame->rows.size()));
	if (!frame->rows.empty())
		std::memcpy(out.ptrw(), frame->rows.data(), frame->rows.size() * sizeof(float));
	return out;
}

PackedInt32Array Simulation::get_present_door_phases() const {
	PackedInt32Array out;
	if (!present_.snapshot) return out;
	const auto &table = present_.snapshot->door_phases;
	out.resize(static_cast<int64_t>(table.size()));
	if (!table.empty())
		std::memcpy(out.ptrw(), table.data(), table.size() * sizeof(int32_t));
	return out;
}

void Simulation::fill_water_wake_frame(
		const Vector3 &camera, opennova::renderer::WaterWakeFrame &frame) const {
	frame.clear();
	if (!kernel_)
		return;
	const int32_t position[3] = { opennova::world::to_fixed(camera.x),
		opennova::world::to_fixed(-camera.z), opennova::world::to_fixed(camera.y) };
	const auto &world = kernel_->world;
	opennova::renderer::compile_water_wakes(
			world.rotor_wash.water_wakes(), world.env.water_z, world.entity_update_counter,
			position, frame);
}
