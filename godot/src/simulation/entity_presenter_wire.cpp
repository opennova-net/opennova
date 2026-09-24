#include <runtime/renderer/render_order.h>
#include "simulation/entity_presenter.h"
#include "object/model_user_point.h"
#include "util/axes.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <runtime/inmatch/wire_present.h>
#include <runtime/world/present_rows.h>
#include <runtime/world/tick_accumulator.h>

#include "simulation/simulation.h"
#include "object/object_data.h"
#include "world/scar_presenter.h"

// The WIRE (joiner/MP) walk: the COLD path (spawn/defer/unresolved
// bookkeeping, the liveness prune, held-weapon builds, the spawn signal and
// stats — the former WirePresentPass) followed by the per-row hot walk (the
// native twin of the plan walk the former wire_present_pass.gd carried;
// per-leg behavioral semantics and their [orig] witnesses moved here with
// the code). The walk deliberately TRANSLITERATES the GDScript's dispatch
// pattern — the same legs fire in the same order under the same conditions —
// so the pass's behavioral contract (pinned by wire_present_pass_test.gd) is
// preserved by construction; further edge-gating of the every-frame legs
// (ctrl publish, weapon channel) is a measured follow-up, not part of this
// move.

namespace godot {

using opennova::world::PF_BMS_ID;
using opennova::world::PF_CHARACTER_ID;
using opennova::world::PF_INDEX;
using opennova::world::PF_KIND;
using opennova::world::PF_STRIDE;
using opennova::world::PF_TYPE_ID;
using opennova::world::PF_WIRE_HANDLE;

namespace {

ObjectModel *model_for_id(ObjectID p_id) {
	return p_id.is_valid()
			? Object::cast_to<ObjectModel>(ObjectDB::get_instance(p_id))
			: nullptr;
}

inline int32_t wfield_i(const float *p, int base, int field) {
	return static_cast<int32_t>(p[base + field]);
}

// The exact PF fields the edge-gated CTRL leg consumes. A new field read
// inside the gated leg MUST join its table, or the gate holds stale node
// output. Gating is presentation memoization only: identical inputs
// re-dispatched to the same node produce the same node state, so skipping the
// redundant dispatch cannot change what renders — it removes the measured
// per-frame Variant/String/Array churn (2026-08-04 joiner profile: ~21us/row
// over 876 mostly-static rows). Mirrors WireRow::kCtrlCacheCount (the struct
// is class-private); the static_assert in present_one_wire_row pins the
// mirror.
constexpr int kCtrlLegFieldCount = 42;

constexpr int kCtrlLegFields[kCtrlLegFieldCount] = {
	Simulation::PF_EMPLACED_CONTROLS_VALID,
	Simulation::PF_EWEAP_GUNYAW,
	Simulation::PF_EWEAP_GUNPITCH,
	Simulation::PF_WEAP_SPIN,
	Simulation::PF_VEHICLE_MOTION_VALID,
	Simulation::PF_VEHICLE_CTRL_MASK,
	Simulation::PF_VEHICLE_TRACK_LEFT,
	Simulation::PF_VEHICLE_TRACK_RIGHT,
	Simulation::PF_VEHICLE_GUN_YAW,
	Simulation::PF_VEHICLE_GUN_PITCH,

	Simulation::PF_VEHICLE_STEERING,
	Simulation::PF_VEHICLE_SPEED,
	Simulation::PF_TEX_TEAM_VALID,
	Simulation::PF_TEX_TEAM,
	Simulation::PF_ZONE_CTRL_VALID,
	Simulation::PF_TEAMSWING,
	Simulation::PF_LFP_CAMPPERCENT_VALID,
	Simulation::PF_LFP_CAMPPERCENT,
	Simulation::PF_WORLD_HEAT_GLOW_VALID,
	Simulation::PF_WORLD_HEAT_GLOW,
	Simulation::PF_ACTIVE1,
	Simulation::PF_PHASE1,
	Simulation::PF_ACTIVE2,
	Simulation::PF_PHASE2,
	// The part-animation words the vehicle-motion leg now stores (appended so
	// the cache layout of every earlier field is unchanged).
	Simulation::PF_VEHICLE_ROTOR,
	Simulation::PF_VEHICLE_TAIL_ROTOR,
	Simulation::PF_VEHICLE_WHEELS,
	Simulation::PF_VEHICLE_TIRE00,
	Simulation::PF_VEHICLE_TIRE01,
	Simulation::PF_VEHICLE_TIRE02,
	Simulation::PF_VEHICLE_TIRE03,
	Simulation::PF_VEHICLE_TIRE04,
	Simulation::PF_VEHICLE_TIRE05,
	Simulation::PF_VEHICLE_TIRE06,
	Simulation::PF_VEHICLE_TIRE07,
	Simulation::PF_VEHICLE_TIRE08,
	Simulation::PF_VEHICLE_TIRE09,
	Simulation::PF_VEHICLE_TIRE10,
	Simulation::PF_VEHICLE_TIRE11,
	Simulation::PF_VEHICLE_TIRE12,
	Simulation::PF_VEHICLE_TIRE13,
	Simulation::PF_VEHICLE_GEAR,
};

// Compare-and-refresh one leg's input cache. Returns true when every field is
// bit-identical to the last applied set (skip the leg); otherwise refreshes
// the cache and returns false (apply). A NaN field never compares equal, so a
// poisoned row degrades to per-frame application, never to a stale hold.
bool leg_inputs_unchanged(const float *p, int base, const int *fields,
		int count, float *cache, bool &cache_valid) {
	if (cache_valid) {
		bool same = true;
		for (int i = 0; i < count; ++i) {
			if (p[base + fields[i]] != cache[i]) {
				same = false;
				break;
			}
		}
		if (same) {
			return true;
		}
	}
	for (int i = 0; i < count; ++i) {
		cache[i] = p[base + fields[i]];
	}
	cache_valid = true;
	return false;
}

} // namespace

// --- The cold path + registry ------------------------------------------------

Node3D *EntityPresenter::container() const {
	return container_id_.is_valid()
			? Object::cast_to<Node3D>(ObjectDB::get_instance(container_id_))
			: nullptr;
}

void EntityPresenter::setup_wire(Object *p_sim,
		const Ref<MissionObjectPlacer> &p_placer, Node3D *p_container,
		const Ref<EntityIndex> &p_defer_index) {
	Simulation *native_sim = Object::cast_to<Simulation>(p_sim);
	sim_id_ = native_sim != nullptr ? native_sim->get_instance_id() : ObjectID();
	placer_ = p_placer;
	container_id_ =
			p_container != nullptr ? p_container->get_instance_id() : ObjectID();
	defer_index_ = p_defer_index;
	pending_spawn_count_ = 0;
	camera_framed_ = false;
	last_present_logic_tick_ = -1;
	wire_plan_dirty_ = true;
	wire_rows_.clear();
	wire_deferred_ids_.clear();
}

void EntityPresenter::set_synthetic_origin_only(bool p_enabled) {
	synthetic_origin_only_ = p_enabled;
}

void EntityPresenter::set_render_culled(int p_handle, bool p_culled) {
	if (p_culled) {
		wire_render_culled_[p_handle] = true;
	} else {
		wire_render_culled_.erase(p_handle);
	}
}

void EntityPresenter::set_cold_spawn_budget(int p_budget) {
	cold_spawn_budget_ = MAX(1, p_budget);
}

void EntityPresenter::set_spectator_camera(Camera3D *p_camera) {
	camera_id_ = p_camera != nullptr ? p_camera->get_instance_id() : ObjectID();
	camera_framed_ = false;
}

Ref<WirePresentStats> EntityPresenter::get_wire_stats_record() const {
	return WirePresentStats::create(stat_live_, stat_spawned_,
			stat_unresolved_, pending_spawn_count_);
}

ObjectModel *EntityPresenter::resolve_wire_handle(int p_handle) const {
	const ObjectID *id = nodes_.getptr(p_handle);
	return id != nullptr ? model_for_id(*id) : nullptr;
}

ObjectModel *EntityPresenter::held_weapon_node(int p_handle) const {
	const ObjectID *id = weapon_nodes_.getptr(p_handle);
	return id != nullptr ? model_for_id(*id) : nullptr;
}

TypedArray<ObjectModel> EntityPresenter::wire_nodes() const {
	TypedArray<ObjectModel> live;
	for (const KeyValue<int32_t, ObjectID> &kv : nodes_) {
		ObjectModel *node = model_for_id(kv.value);
		if (node != nullptr) {
			live.push_back(node);
		}
	}
	return live;
}

void EntityPresenter::set_entity_lighting_context(int p_handle,
		float p_effect_scale, bool p_interior_lerp, float p_light_transfer,
		int p_interior_bms, int p_interior_section) {
	LightingContext context;
	context.effect_scale = CLAMP(p_effect_scale, 0.0f, 1.0f);
	context.interior_lerp = p_interior_lerp;
	context.light_transfer = CLAMP(p_light_transfer, 0.0f, 1.0f);
	context.interior_bms = p_interior_bms;
	context.interior_section = p_interior_section;
	lighting_contexts_[p_handle] = context;
	apply_lighting_context(p_handle);
}

void EntityPresenter::apply_lighting_context(int p_handle) {
	const LightingContext *context = lighting_contexts_.getptr(p_handle);
	if (context == nullptr) return;
	if (ObjectModel *body = resolve_wire_handle(p_handle)) {
		body->set_entity_lighting_context(context->effect_scale,
				context->interior_lerp, context->light_transfer);
		body->set_interior_light_group(context->interior_bms, context->interior_section);
	}
	if (ObjectModel *weapon = held_weapon_node(p_handle)) {
		const ObjectModel *body = resolve_wire_handle(p_handle);
		weapon->set_thermal_entity_wave(body != nullptr && body->get_thermal_entity_wave());
		weapon->set_entity_lighting_context(context->effect_scale,
				context->interior_lerp, context->light_transfer);
		weapon->set_interior_light_group(context->interior_bms, context->interior_section);
	}
}

void EntityPresenter::free_wire_node(int p_handle) {
	if (ObjectModel *node = resolve_wire_handle(p_handle)) {
		node->queue_free();
	}
	nodes_.erase(p_handle);
	free_held_weapon(p_handle);
	release_wire_handle(p_handle);
	lighting_contexts_.erase(p_handle);
}

void EntityPresenter::free_held_weapon(int p_handle) {
	if (ObjectModel *weapon = held_weapon_node(p_handle)) {
		weapon->queue_free();
	}
	weapon_nodes_.erase(p_handle);
	weapon_graphics_.erase(p_handle);
}

void EntityPresenter::reset_wire_runtime_state() {
    reset_minefields();
	reset_virtual_display();
	Vector<int32_t> handles;
	for (const KeyValue<int32_t, ObjectID> &kv : nodes_) {
		handles.push_back(kv.key);
	}
	for (int32_t handle : handles) {
		free_wire_node(handle);
	}
	handles.clear();
	for (const KeyValue<int32_t, ObjectID> &kv : weapon_nodes_) {
		handles.push_back(kv.key);
	}
	for (int32_t handle : handles) {
		free_held_weapon(handle);
	}
	weapon_nodes_.clear();
	weapon_graphics_.clear();
	lighting_contexts_.clear();
	nodes_.clear();
	unresolved_.clear();
	reset_wire_plan_state();
	pending_spawn_count_ = 0;
	last_present_logic_tick_ = -1;
	camera_framed_ = false;
	stat_live_ = 0;
	// The present passes' Stop -> Play boundary, in the drive order the
	// restart signal fanned in: destruction (restore intact visuals, drop the
	// husk grafts, retire the wreck/piece anchors), throwable (free the models,
	// stop the move groups), scars (every scar mesh goes).
	destruction_->reset_runtime_state();
	throwable_->reset_runtime_state();
	vehicle_trail_->reset_runtime_state();
	if (ScarPresenter *scars_node = scars()) {
		scars_node->reset_runtime_state();
	}
}

void EntityPresenter::register_wire_node(int p_handle, ObjectModel *p_node) {
	if (p_node == nullptr) {
		nodes_.erase(p_handle);
		lighting_contexts_.erase(p_handle);
		return;
	}
	nodes_[p_handle] = ObjectID(p_node->get_instance_id());
	apply_lighting_context(p_handle);
}

void EntityPresenter::register_wire_held_weapon(int p_handle, ObjectModel *p_node) {
	if (p_node == nullptr) {
		weapon_nodes_.erase(p_handle);
		return;
	}
	weapon_nodes_[p_handle] = ObjectID(p_node->get_instance_id());
	apply_lighting_context(p_handle);
}

void EntityPresenter::present_wire_snapshot(const PackedFloat32Array &p_snap,
		int p_stride, int64_t p_layout_revision) {
	present_wire_rows_view({p_snap.ptr(), p_snap.size()}, p_stride, p_layout_revision);
}

void EntityPresenter::present_wire_rows_view(PresentRowsView p_snap,
		int p_stride, int64_t p_layout_revision) {
	Simulation *s = sim();
	Node3D *parent = container();
	if (s == nullptr || placer_.is_null() || parent == nullptr ||
			p_stride < PF_STRIDE) {
		return;
	}
	const int tick_delta = consume_present_logic_tick_delta();
	// Packed handle zero is a valid pool-0 identity, so the numeric getter
	// cannot also carry presence: fold the sim's validity seam into a -1
	// sentinel so the row-plan key distinguishes "no local player yet" from a
	// genuine slot-0 local handle.
	const int local_handle =
			s->has_local_player() ? s->get_local_player_wire_handle() : -1;
	const int64_t index_generation =
			defer_index_.is_valid() ? defer_index_->get_generation() : 0;
	if (pending_spawn_count_ == 0 &&
			wire_plan_is_current(p_snap.size(), p_stride,
					p_layout_revision, index_generation, local_handle)) {
		present_wire_rows(p_snap, p_stride, tick_delta);
		frame_spectator_camera();
		return;
	}
	begin_wire_plan(p_layout_revision, p_stride, p_snap.size(),
			index_generation, local_handle);
	const float *snap = p_snap.ptr();
	const int count = int(p_snap.size() / p_stride);
	HashMap<int32_t, bool> live;
	// [node, runtime_kind, visual_item_id] per spawn; the signal fires after
	// the production transform is applied, exactly as the inline cold walk
	// ordered it.
	LocalVector<ObjectID> spawned_nodes;
	LocalVector<int32_t> spawned_kinds;
	LocalVector<int32_t> spawned_items;
	int spawn_attempts = 0;
	int pending_spawns = 0;
	for (int i = 0; i < count; ++i) {
		const int base = i * p_stride;
		const int type_id = int(snap[base + PF_TYPE_ID]);
		const int handle = int(snap[base + PF_WIRE_HANDLE]);
		const int character_id = int(snap[base + PF_CHARACTER_ID]) & 0xffff;
		const int32_t visual_identity = int32_t(
				(uint32_t(type_id) << 16) | uint32_t(character_id));
		// A zero type row is the joiner's self-filtered echo (H) or an
		// unresolved record; the local player handle is drawn by
		// LocalPlayerPresenter.
		if (type_id == 0 || handle == local_handle) {
			continue;
		}
		// A wire-only row with no local BMS record carries the decoded halves
		// of kSpawnOriginNone (world/entity.h names both sentinels).
		if (synthetic_origin_only_ &&
				!(int(snap[base + PF_KIND]) ==
								opennova::world::kSpawnOriginKindNone &&
						int(snap[base + PF_INDEX]) ==
								opennova::world::kSpawnOriginIndexNone)) {
			continue;
		}
		const int runtime_kind = opennova::inmatch::
				mission_kind_for_wire_handle(uint16_t(handle));
		const int visual_item_id =
				placer_->resolve_player_visual_item_id(type_id);
		// Defer any row that carries a PLACED .bms identity: the placed
		// representation — an individual node or a static MultiMesh batch
		// instance (deliberately node-less) — owns the rendering. The node
		// resolve is bookkeeping for row-plan validity, not the defer
		// condition; a batched static resolves to null and still defers.
		// A row whose index is not installed yet (a joiner's streamed statics
		// between the world-stream fence and the settle that places them)
		// defers the same way: no wire node, ever.
		{
			const int d_kind = int(snap[base + PF_KIND]);
			const int d_index = int(snap[base + PF_INDEX]);
			if (d_kind >= 0 && d_kind <= 3 && d_index >= 0 &&
					d_index != opennova::world::kSpawnOriginIndexNone) {
				ObjectModel *placed = defer_index_.is_valid()
						? defer_index_->resolve(
								int(snap[base + PF_BMS_ID]), d_kind, d_index)
						: nullptr;
				if (placed != nullptr) {
					append_wire_deferred(placed);
				}
				continue;
			}
		}
		live[handle] = true;
		if (const int32_t *failed_identity = unresolved_.getptr(handle)) {
			if (*failed_identity == visual_identity) {
				continue;
			}
			unresolved_.erase(handle);
			lighting_contexts_.erase(handle);
		}
		ObjectModel *node = resolve_wire_handle(handle);
		if (node != nullptr &&
				!wire_node_matches_row(node, p_snap, base, type_id)) {
			free_wire_node(handle);
			node = nullptr;
		}
		bool spawned_now = false;
		if (node == nullptr) {
			// Continue the cheap scan after exhausting the budget: later live
			// nodes still need this frame's transform, and mismatched/retired
			// nodes still need prompt teardown. The omitted rows force
			// another cold plan below.
			if (spawn_attempts >= cold_spawn_budget_) {
				++pending_spawns;
				continue;
			}
			++spawn_attempts;
			// build_player_animated_model maps the player runtime type to its
			// visual item and passes other organics through — the SAME chain
			// the host uses for the local avatar and placed NPCs.
			node = placer_->build_player_animated_model(
					type_id, parent, character_id);
			if (node == nullptr) {
				unresolved_[handle] = visual_identity;
				++stat_unresolved_;
				continue;
			}
			node->set_name(vformat("Wire_%04x", handle));
			Ref<EntityRef> ref;
			ref.instantiate();
			ref->set_kind(runtime_kind);
			ref->set_origin_kind(int(snap[base + PF_KIND]));
			ref->set_index(int(snap[base + PF_INDEX]));
			ref->set_bms_id(int(snap[base + PF_BMS_ID]));
			ref->set_wire_handle(handle);
			ref->set_item_id(visual_item_id);
			ref->set_runtime_type_id(type_id);
			ref->set_character_id(character_id);
			node->set_entity_ref(ref);
			const Ref<ItemDatabase> item_db = placer_->get_item_db();
			node->set_thermal_entity_wave(item_db.is_valid() &&
					opennova::renderer::entity_uses_thermal_wave(item_db->get_item_type(visual_item_id)));
			nodes_[handle] = node->get_instance_id();
			apply_lighting_context(handle);
			++stat_spawned_;
			spawned_now = true;
		}
		append_wire_row(node, base, handle, spawned_now);
		if (spawned_now) {
			spawned_nodes.push_back(ObjectID(node->get_instance_id()));
			spawned_kinds.push_back(runtime_kind);
			spawned_items.push_back(visual_item_id);
		}
	}
	present_wire_rows(p_snap, p_stride, tick_delta);
	pending_spawn_count_ = pending_spawns;
	for (uint32_t i = 0; i < spawned_nodes.size(); ++i) {
		if (ObjectModel *node = model_for_id(spawned_nodes[i])) {
			emit_signal("wire_node_spawned", node, spawned_kinds[i],
					spawned_items[i]);
		}
	}
	stat_live_ = int64_t(live.size());
	Vector<int32_t> retired;
	for (const KeyValue<int32_t, ObjectID> &kv : nodes_) {
		if (!live.has(kv.key)) {
			retired.push_back(kv.key);
		}
	}
	for (int32_t handle : retired) {
		free_wire_node(handle);
	}
	retired.clear();
	for (const KeyValue<int32_t, int32_t> &kv : unresolved_) {
		if (!live.has(kv.key)) {
			retired.push_back(kv.key);
		}
	}
	for (int32_t handle : retired) {
		unresolved_.erase(handle);
		lighting_contexts_.erase(handle);
	}
	frame_spectator_camera();
}

// Spectator-only one-shot overview.
void EntityPresenter::frame_spectator_camera() {
	Camera3D *camera = camera_id_.is_valid()
			? Object::cast_to<Camera3D>(ObjectDB::get_instance(camera_id_))
			: nullptr;
	if (camera == nullptr || camera_framed_ || nodes_.is_empty() ||
			pending_spawn_count_ > 0) {
		return;
	}
	Vector3 centroid;
	int count = 0;
	for (const KeyValue<int32_t, ObjectID> &kv : nodes_) {
		ObjectModel *node = model_for_id(kv.value);
		if (node == nullptr) {
			continue;
		}
		centroid += node->get_global_position();
		++count;
	}
	if (count == 0) {
		return;
	}
	camera_framed_ = true;
	centroid /= real_t(count);
	camera->set_global_position(centroid + Vector3(0.0f, 90.0f, 110.0f));
	camera->look_at(centroid, Vector3(0, 1, 0));
}

// Remote primary-channel blends are fixed-tick state, while this walk also
// runs on zero-tick render frames and once after a multi-tick catch-up batch.
// Consume the sim clock once per presented snapshot so every row advances by
// the exact logic-tick delta rather than by the number of render submissions.
int EntityPresenter::consume_present_logic_tick_delta() {
	Simulation *s = sim();
	const int64_t now = s != nullptr ? s->get_logic_tick() : 0;
	if (last_present_logic_tick_ < 0) {
		last_present_logic_tick_ = now;
		return 0;
	}
	if (now <= last_present_logic_tick_) {
		// Equal means a render-only re-present; lower means the world was
		// restarted under the same presenter. Rebase without fabricating
		// ticks.
		last_present_logic_tick_ = now;
		return 0;
	}
	const int64_t delta = now - last_present_logic_tick_;
	last_present_logic_tick_ = now;
	return int(MIN(delta,
			int64_t(opennova::world::TickAccumulator::kMaxCatchupTicks)));
}

bool EntityPresenter::wire_node_matches_row(ObjectModel *p_node,
		PresentRowsView p_snap, int p_base, int p_type_id) const {
	const float *snap = p_snap.ptr();
	const Ref<EntityRef> ref = p_node->get_entity_ref();
	if (ref.is_null()) {
		return false;
	}
	return ref->get_runtime_type_id() == p_type_id &&
			ref->get_character_id() ==
					(int(snap[p_base + PF_CHARACTER_ID]) & 0xffff) &&
			ref->get_origin_kind() == int(snap[p_base + PF_KIND]) &&
			ref->get_index() == int(snap[p_base + PF_INDEX]) &&
			ref->get_bms_id() == int(snap[p_base + PF_BMS_ID]);
}

Vector3 EntityPresenter::muzzle_world_for(int p_handle,
		const String &p_userpoint) const {
	ObjectModel *body = resolve_wire_handle(p_handle);
	const Vector3 body_origin = body != nullptr
			? body->get_global_transform().origin
			: Vector3(INFINITY, INFINITY, INFINITY);
	if (p_userpoint.is_empty()) {
		return body_origin;
	}
	ObjectModel *weapon = held_weapon_node(p_handle);
	if (weapon == nullptr || !weapon->is_visible()) {
		return body_origin;
	}
	Ref<ObjectData> data = weapon->get_object_data();
	if (data.is_null()) {
		return body_origin;
	}
	const int point_count = data->get_user_point_count();
	for (int i = 0; i < point_count; ++i) {
		const Ref<ModelUserPoint> info = data->get_user_point_info(i);
		if (info.is_valid() && info->get_name().nocasecmp_to(p_userpoint) == 0) {
			return weapon->get_global_transform().xform(info->get_position());
		}
	}
	return body_origin;
}

// Build (or free) this wire body's third-person gun model when its ADM
// changes — the hot walk detects the edge and calls here so the placer build,
// node naming, and the maps muzzle_world_for/tests consume stay on the cold
// path; the per-frame rigid attach lives in the hot walk. Kept beside nodes_
// rather than parented under the body: ObjectModel rebuild() frees all of its
// children, so a child weapon would vanish on any body rebuild. (Witness:
// runtime/inmatch/wire_present.h ledger — the model resolves off the equipped ADM;
// the sim folds the draw gate in.)
Node3D *EntityPresenter::rebuild_held_weapon(int p_handle, int p_adm) {
	Simulation *s = sim();
	String graphic;
	if (p_adm > 0 && s != nullptr) {
		graphic = s->get_weapon_third_person_model(p_adm);
	}
	const String *current = weapon_graphics_.getptr(p_handle);
	if (current != nullptr && *current == graphic) {
		return held_weapon_node(p_handle);
	}
	free_held_weapon(p_handle);
	Node3D *parent = container();
	if (!graphic.is_empty() && placer_.is_valid() && parent != nullptr) {
		ObjectModel *built = placer_->build_model_from_graphic(graphic,
				String(), parent, String(), String(), true);
		if (built != nullptr) {
			built->set_name(vformat("WireWeapon_%04x", p_handle));
			built->set_shadow_caster_enabled(true);
			// The 3P gun silhouettes inside the body's render slot, like
			// retail's child walk [orig:
			// RenderSlot_RenderEntityAndChildren @0x5d78ef, see
			// docs/render/render-lighting-re.md] — never a slot of its own.
			built->set_slot_shadow_capture_with(resolve_wire_handle(p_handle));
			built->set_entity_light_owner(resolve_wire_handle(p_handle));
			// The held weapon draws at its owner's selected RLOD clamped to its
			// own LOD count and never walks its own thresholds
			// (renderer::attachment_lod_index). Retail draws it inside the
			// HEAD submit of a composed avatar (the flagged body submit skips
			// it), so the head part owns the level when there is one. The
			// weapon never outlives its body (free_wire_node frees both), so
			// one stamp at build suffices.
			ObjectModel *body = resolve_wire_handle(p_handle);
			ObjectModel *head = MissionObjectPlacer::avatar_head_part(body);
			built->set_authored_lod_owner(head != nullptr ? head : body);
			weapon_nodes_[p_handle] = built->get_instance_id();
			apply_lighting_context(p_handle);
		}
	}
	weapon_graphics_[p_handle] = graphic;
	return held_weapon_node(p_handle);
}

// --- The plan + the per-row hot walk -----------------------------------------

void EntityPresenter::begin_wire_plan(int64_t layout_revision, int stride,
		int64_t snapshot_size, int64_t index_generation, int local_handle) {
	wire_rows_.clear();
	wire_deferred_ids_.clear();
	wire_plan_revision_ = layout_revision;
	wire_plan_stride_ = stride;
	wire_plan_snapshot_size_ = snapshot_size;
	wire_plan_index_generation_ = index_generation;
	wire_plan_local_handle_ = local_handle;
	wire_plan_dirty_ = false;
}

void EntityPresenter::append_wire_row(Object *node, int base, int handle,
		bool spawned_now) {
	WireRow row;
	row.base = base;
	row.handle = handle;
	row.node_id = node != nullptr ? ObjectID(node->get_instance_id()) : ObjectID();
	row.spawned_now = spawned_now;
	// Last-applied edge state (-1 = unknown, first hot frame always applies);
	// the body-transition scalars re-seed from the stable per-handle cache so a
	// revisionless source's cold plan every call cannot strand an active
	// receive-side blend at weight zero.
	const RemoteBodyCache *cached = wire_remote_body_.getptr(handle);
	if (cached != nullptr) {
		row.anim_state = cached->state;
		row.anim_request = cached->request;
		row.remote_body_tick = cached->latch;
	}
	wire_rows_.push_back(row);
}

void EntityPresenter::append_wire_deferred(Object *node) {
	if (node != nullptr) {
		wire_deferred_ids_.push_back(ObjectID(node->get_instance_id()));
	}
}

bool EntityPresenter::wire_plan_is_current(int64_t snapshot_size, int stride,
		int64_t layout_revision, int64_t index_generation, int local_handle) {
	// The revision keys on exactly the per-row identity quintet
	// (wire_handle/type_id/bms_id/kind/index — simulation_present.cpp), so
	// revision equality replaces the GDScript plan's per-row identity re-reads;
	// node swaps mark the plan dirty through release_wire_handle.
	if (wire_plan_dirty_ || wire_plan_revision_ != layout_revision ||
			wire_plan_stride_ != stride ||
			wire_plan_snapshot_size_ != snapshot_size ||
			wire_plan_index_generation_ != index_generation ||
			wire_plan_local_handle_ != local_handle) {
		return false;
	}
	for (const WireRow &row : wire_rows_) {
		if (row.base < 0 || row.base + stride > snapshot_size) {
			return false;
		}
		if (ObjectDB::get_instance(row.node_id) == nullptr) {
			return false;
		}
	}
	for (const ObjectID &deferred : wire_deferred_ids_) {
		if (ObjectDB::get_instance(deferred) == nullptr) {
			return false;
		}
	}
	return true;
}

// The render-gate verdict is keyed by the sim's wire handle, not by the
// node: a node swap (release + rebuild) keeps it, so the replacement body
// draws gated exactly like the one it replaced. Only the baseline reset
// (clear_render_culled) forgets verdicts.
void EntityPresenter::release_wire_handle(int handle) {
	wire_remote_body_.erase(handle);
	wire_respawn_revisions_.erase(handle);
	wire_held_weapon_adm_.erase(handle);
	wire_held_weapon_ids_.erase(handle);
	wire_plan_dirty_ = true;
}

void EntityPresenter::reset_wire_plan_state() {
	wire_rows_.clear();
	wire_deferred_ids_.clear();
	wire_render_culled_.clear();
	wire_remote_body_.clear();
	wire_respawn_revisions_.clear();
	wire_held_weapon_adm_.clear();
	wire_held_weapon_ids_.clear();
	wire_plan_revision_ = -1;
	wire_plan_stride_ = 0;
	wire_plan_snapshot_size_ = -1;
	wire_plan_index_generation_ = -1;
	wire_plan_local_handle_ = -1;
	wire_plan_dirty_ = true;
}

void EntityPresenter::present_wire_rows(PresentRowsView snap,
		int stride, int tick_delta) {
	const int64_t size = snap.size();
	for (WireRow &row : wire_rows_) {
		if (row.base < 0 || row.base + stride > size) {
			continue;
		}
		ObjectModel *model =
				Object::cast_to<ObjectModel>(ObjectDB::get_instance(row.node_id));
		if (model == nullptr) {
			continue;
		}
		present_one_wire_row(row, model, snap, tick_delta);
		row.spawned_now = false;
	}
}

void EntityPresenter::present_one_wire_row(WireRow &row, ObjectModel *model,
		PresentRowsView snap, int tick_delta) {
	static_assert(kCtrlLegFieldCount == WireRow::kCtrlCacheCount,
			"ctrl leg field table must match the row cache size");
	const float *p = snap.ptr();
	const int base = row.base;
	// A row the occlusion frame's collector gate culled is not drawn: no
	// presentation leg runs for it (the sector walk only visits collected
	// entities; OcclusionWorld::sphere_render_visible carries the witness). Its body
	// sounds still walk the wire playhead (retail triggers them from the entity
	// update, not the draw), and the respawn revision stays unconsumed so the
	// reset lands on the first drawn frame. The compare-gated legs re-assert
	// exactly what changed when the gate releases the row.
	if (wire_render_culled_.has(row.handle)) {
		if (model->is_visible()) {
			model->set_visible(false);
		}
		// The held weapon is a sibling node under the pass container, so it
		// hides through its own leg (retail draws the third-person gun inside
		// the body's submit; a culled body never draws its gun
		// [see RenderSlot_RenderEntityAndChildren in the engine's witness map]).
		update_wire_held_weapon(row, model, snap, false);
		// A pending remote body blend is entity-update work, not draw work
		// (retail advances it in AnimMap_UpdateEntity): keep consuming the
		// tick delta so the blend finishes on schedule while occluded. Only a
		// row with a blend in flight pays this; clip dispatch itself is draw
		// work and re-asserts on the first drawn frame.
		if (row.remote_body_tick != 0) {
			apply_wire_body_anim(row, model, snap, tick_delta);
		}
		present_wire_row_body_sounds(row, snap);
		return;
	}
    if (wfield_i(p, base, Simulation::PF_HUSK) != 0) {
        opennova::world::HuskSwapEvent husk;
        husk.net_id = wfield_i(p, base, Simulation::PF_NET_ID);
        husk.wire_handle = uint16_t(row.handle);
        husk.bms_id = wfield_i(p, base, Simulation::PF_BMS_ID);
        husk.spawn_origin = opennova::world::spawn_origin_pack(
                wfield_i(p, base, Simulation::PF_KIND), wfield_i(p, base, Simulation::PF_INDEX));
        husk.item_id = wfield_i(p, base, Simulation::PF_TYPE_ID);
        destruction_->apply_husk_swap(husk);
    }
	model->set_parachute_deployed(wfield_i(p, base, Simulation::PF_PARACHUTE_DEPLOYED) != 0);
	stamp_match_terrain(model, p, base);
        stamp_destroy_phases(model, p, base);
	const int32_t respawn_revision =
			wfield_i(p, base, Simulation::PF_RESPAWN_REVISION);
	const int32_t *seen_revision = wire_respawn_revisions_.getptr(row.handle);
	const bool respawned_since_present = !row.spawned_now &&
			seen_revision != nullptr && *seen_revision != respawn_revision;
	const Vector3 pos(p[base + Simulation::PF_POS_X],
			p[base + Simulation::PF_POS_Y],
			p[base + Simulation::PF_POS_Z]);
	const Vector3 rot(p[base + Simulation::PF_PITCH_DEG],
			p[base + Simulation::PF_YAW_DEG],
			p[base + Simulation::PF_ROLL_DEG]);
	const Basis entity_basis = bms_to_godot_basis(rot);
	// aim_root_basis is data-gated internally (PF_AIM_OVERLAY_VALID falls back
	// to the entity rotation).
	const Basis root_basis = aim_root_basis_native(snap, base, entity_basis);
	const Transform3D next_transform =
			model->compose_entity_transform(root_basis, pos);
	if (model->get_transform() != next_transform) {
		model->set_transform(next_transform);
	}
	// The mounted right-hand collapse rides its own packed field; edge-gated to
	// the value change like the placed walk.
	stamp_right_hand_collapsed(model, p, base, row.rhc);
	// Aim overlay: apply while valid, clear only on the valid->invalid edge.
	// The apply itself is input-gated: identical overlay angles re-dispatch
	// the identical delta set, so only changed inputs build the 9-basis Array.
	// The clear edge invalidates the cache so a later re-valid always applies.
	{
		const int32_t aim_valid =
				wfield_i(p, base, Simulation::PF_AIM_OVERLAY_VALID);
		if (aim_valid != 0) {
			if (aim_payload_changed(p, base, row.aim_cache,
					row.aim_cache_valid)) {
				aim_apply_valid(model, snap, base, false);
			}
		} else if (row.aim_valid != 0) {
			model->clear_aim_overlay();
			row.aim_cache_valid = false;
		}
		row.aim_valid = aim_valid;
	}
	// The CTRL/PART block is input-gated as one unit: every field all four
	// semantic writers and both procedural part channels consume sits in
	// kCtrlLegFields, so an unchanged set means the node's presenter-owned
	// controls and part phases are already exactly this state (the release
	// legs included — one release is as absent as a re-released one).
	if (!leg_inputs_unchanged(p, base, kCtrlLegFields,
			kCtrlLegFieldCount, row.ctrl_cache, row.ctrl_cache_valid)) {
		model->begin_ctrl_update();
		apply_wire_procedural_part(row, model, snap);
		// All four semantic CTRL writers: compact joiner rows release absent
		// authoritative fields, while direct host/synthetic rows publish
		// vehicle, zone and attachment heat values.
		// [orig: Entity_CacheVehicleHUDStats @ 0x4929B0;
		//  BoneCallback_gnrc_World @ 0x4E288B..0x4E28FB;
		//  parent UseGun attachment @ 0x546518 -> cache @ 0x440930]
		wire_controls_apply(model, snap, base);
		model->end_ctrl_update();
	}
	if (respawned_since_present) {
		model->reset_remote_body_state();
		row.anim_state = -2; // force the next body-anim dispatch through
		row.remote_body_tick = 0;
		// The reset wipes model-side state the gated legs may have applied;
		// their caches must not claim it is still applied.
		row.ctrl_cache_valid = false;
		row.aim_cache_valid = false;
		row.wpn_state = INT32_MIN;
		row.wpn_src_state = INT32_MIN;
		row.wpn_phase = INT32_MIN;
		// The fresh life's clip may reuse the old state id with a restarted
		// playhead; a held foot cursor would compare against the old
		// monotonic phase and silence the feet until it caught up.
		row.foot_state = -2;
		row.foot_phase = -1;
	}
	apply_wire_body_anim(row, model, snap, tick_delta);
	// The upper-body weapon channel: the hold pose this player's held weapon and
	// scope state select. -1 means no channel this frame, which clears any pose
	// left over from the weapon it was holding before. Dispatch is gated on the
	// (state, phase) pair — a playing channel advances its phase every tick and
	// still dispatches per tick; idle -1 rows and render-only frames skip.
	// [orig: the selection Entity_UpdateInfantryPlayerBody @ 0x4b5dad, which
	//  retail runs for every player body it draws, not just the local one]
	{
		const int32_t wpn_state =
				wfield_i(p, base, Simulation::PF_WPN_ANIM_STATE);
		const int32_t wpn_phase =
				wfield_i(p, base, Simulation::PF_WPN_PHASE_TICKS);
		const int32_t wpn_src_state =
				wfield_i(p, base, Simulation::PF_WPN_SOURCE_STATE);
		const int32_t wpn_src_phase =
				wfield_i(p, base, Simulation::PF_WPN_SOURCE_PHASE_TICKS);
		const float wpn_weight = p[base + Simulation::PF_WPN_BLEND_WEIGHT];
		const int32_t wpn_variant = wfield_i(p, base, Simulation::PF_WPN_VARIANT);
		const int32_t wpn_src_variant =
				wfield_i(p, base, Simulation::PF_WPN_SOURCE_VARIANT);
		if (wpn_state != row.wpn_state || wpn_phase != row.wpn_phase ||
				wpn_src_state != row.wpn_src_state || wpn_src_phase != row.wpn_src_phase ||
				wpn_weight != row.wpn_weight || wpn_variant != row.wpn_variant ||
				wpn_src_variant != row.wpn_src_variant) {
			model->set_weapon_channel(
					wpn_state >= 0 ? infantry_key(wpn_state) : String(),
					wpn_phase,
					wpn_src_state >= 0 ? infantry_key(wpn_src_state) : String(),
					wpn_src_phase, wpn_weight, wpn_variant, wpn_src_variant);
			row.wpn_state = wpn_state;
			row.wpn_phase = wpn_phase;
			row.wpn_src_state = wpn_src_state;
			row.wpn_src_phase = wpn_src_phase;
			row.wpn_weight = wpn_weight;
			row.wpn_variant = wpn_variant;
			row.wpn_src_variant = wpn_src_variant;
		}
	}
	present_wire_row_body_sounds(row, snap);
	const bool next_visible =
			wfield_i(p, base, Simulation::PF_HIDDEN) == 0 &&
			wfield_i(p, base, Simulation::PF_LOCAL_VIEW_SUPPRESSED) == 0;
	// Owned-channel contract as in the placed walk: only a row publishing
	// PF_SECTION_MASK_VALID drives the model's section mask; a VALID -> clear
	// transition releases once, and never-publishing rows leave the channel to
	// its other writer (the occlusion frame pass on buildings).
	stamp_section_mask(model, p, base, row.destroyed_section_mask);
	update_wire_held_weapon(row, model, snap, next_visible);
	wire_respawn_revisions_.insert(row.handle, respawn_revision);
	if (model->is_visible() != next_visible) {
		model->set_visible(next_visible);
	}
}

// REMOTE-BODY SOUNDS: walk the authored trigger words this row's clip
// playhead just crossed and queue the witnessed footstep/foley slots. The
// wire playhead is the authority here (closer to retail than a free-running
// clip). A clip change seeds the consume cursor through the scan
// contract's two forms (adm_root_motion.h): observed within its first
// ticks it is a fresh start — scan from -1 so frame 0 fires; observed
// deeper in (an enter-range attach, a ratio-seeded wire retarget, a plan
// rebuild) it is a mid-clip landing — seed at the playhead so the crossed
// prefix never back-fires as a burst. Runs for culled rows too: the sounds
// are entity-update work, not draw work.
void EntityPresenter::present_wire_row_body_sounds(WireRow &row,
		PresentRowsView snap) {
	const float *p = snap.ptr();
	const int base = row.base;
	// The replication seeds a retargeted clip at phase 0 and advances once per
	// tick, so a fresh clip's first observed playhead is a few ticks in at
	// most; anything past this is a mid-clip landing.
	constexpr int32_t kFreshClipTicks = 3;
	Simulation *s = sim();
	const int32_t foot_state = wfield_i(p, base, Simulation::PF_ANIM_STATE);
	const int32_t foot_phase =
			wfield_i(p, base, Simulation::PF_ANIM_PHASE_TICKS);
	// Armed tuple rows only (PF_ANIM_REMOTE_REQUEST == 0): the request
	// path publishes a model-FSM ratio in the phase field, not the
	// simulation playhead this consume walks.
	const bool foot_armed =
			wfield_i(p, base, Simulation::PF_ANIM_REMOTE_REQUEST) == 0;
	if (s != nullptr && foot_armed && foot_state >= 0 && foot_phase >= 0) {
		if (foot_state != row.foot_state) {
			row.foot_state = foot_state;
			row.foot_phase =
					foot_phase <= kFreshClipTicks ? -1 : foot_phase;
		}
		if (foot_phase > row.foot_phase) {
			s->present_wire_body_sounds(
					wfield_i(p, base, Simulation::PF_TYPE_ID),
					wfield_i(p, base, Simulation::PF_CHARACTER_ID),
					wfield_i(p, base, Simulation::PF_WIRE_HANDLE),
					wfield_i(p, base, Simulation::PF_CARRIER_HANDLE),
					foot_state, row.foot_phase, foot_phase,
					Vector3(p[base + Simulation::PF_POS_X],
							p[base + Simulation::PF_POS_Y],
							p[base + Simulation::PF_POS_Z]));
		}
		row.foot_phase = foot_phase;
	} else {
		// No clip this frame: drop the cursor so a re-armed clip re-seeds
		// instead of comparing against a stale monotonic playhead.
		row.foot_state = -2;
		row.foot_phase = -1;
	}
}

// Keep dynamically materialized items on the same PANM path as placed mission
// objects. The ACTIVE fields are publication ownership, so an owned zero phase
// must still be written and a suppressed channel must release its prior value.
void EntityPresenter::apply_wire_procedural_part(const WireRow &row,
		ObjectModel *model, PresentRowsView snap) {
	const float *p = snap.ptr();
	const int base = row.base;
	if (wfield_i(p, base, Simulation::PF_ACTIVE1) > 0) {
		model->set_part_phase(1,
				opennova::world::decode_present_part_anim_phase(snap, base, 1));
	} else {
		model->clear_part_phase(1);
	}
	if (wfield_i(p, base, Simulation::PF_ACTIVE2) > 0) {
		model->set_part_phase(2,
				opennova::world::decode_present_part_anim_phase(snap, base, 2));
	} else {
		model->clear_part_phase(2);
	}
}

// The wire-driven skeletal primary pose, on the same projection path as the
// placed walk. REMOTE-request rows skip re-dispatch entirely while the wire
// state is unchanged; host-loopback rows (remote_request 0) keep per-tick
// dispatch — their playhead rides play_body_clip_at's phase.
void EntityPresenter::apply_wire_body_anim(WireRow &row, ObjectModel *model,
		PresentRowsView snap, int tick_delta) {
	const float *p = snap.ptr();
	const int base = row.base;
	const int32_t anim_state = wfield_i(p, base, Simulation::PF_ANIM_STATE);
	const int32_t remote_request_i =
			wfield_i(p, base, Simulation::PF_ANIM_REMOTE_REQUEST);
	const int32_t anim_pulse =
			wfield_i(p, base, Simulation::PF_ANIM_STATE_PULSE);
	// Accepted/queued remote transitions advance exactly once per simulation
	// tick. The row latch is set only when the model reports live transition
	// work, so steady-state rows never dispatch.
	if (row.remote_body_tick != 0 && remote_request_i != 0 && anim_pulse < 0 &&
			anim_state == row.anim_state && remote_request_i == row.anim_request) {
		int ticks_left = tick_delta;
		while (ticks_left > 0 && row.remote_body_tick != 0) {
			row.remote_body_tick =
					model->advance_remote_body_blend_tick(anim_state) ? 1 : 0;
			--ticks_left;
		}
		row.anim_state = anim_state;
		row.anim_request = remote_request_i;
		store_wire_remote_body_cache(row);
		return;
	}
	if (remote_request_i != 0 && anim_pulse < 0 && anim_state == row.anim_state &&
			remote_request_i == row.anim_request) {
		return;
	}
	row.anim_state = anim_state;
	row.anim_request = remote_request_i;
	store_wire_remote_body_cache(row);
	const int32_t anim_phase =
			wfield_i(p, base, Simulation::PF_ANIM_PHASE_TICKS);
	const bool remote_request = remote_request_i != 0;
	bool remote_needs_tick = false;
	// A transition state that arrived and was overwritten within one decode fold
	// dispatches FIRST so the model's arbitration sees retail's per-record
	// order. [orig: per-record remote anim apply @ 0x4c1153]
	if (remote_request && anim_pulse >= 0) {
		const String &pulse_key = infantry_key(anim_pulse);
		if (!pulse_key.is_empty()) {
			remote_needs_tick = model->apply_remote_body_state(
					anim_pulse, pulse_key,
					Simulation::infantry_anim_flags(anim_pulse),
					wfield_i(p, base, Simulation::PF_ANIM_PULSE_TICKS));
		}
	}
	if (anim_state >= 0) {
		const String &key = infantry_key(anim_state);
		if (!key.is_empty()) {
			// ObjectModel owns current/pending acceptance because it also
			// owns clip time and completion. Forward every raw wire request.
			// [orig: @ 0x4c0859 / @ 0x4c11a6]
			if (remote_request) {
				remote_needs_tick = model->apply_remote_body_state(
						anim_state, key,
						Simulation::infantry_anim_flags(anim_state),
						anim_phase);
				row.remote_body_tick = remote_needs_tick ? 1 : 0;
				store_wire_remote_body_cache(row);
				return;
			}
			// Host-loopback rows expose the authority's already-accepted
			// CURRENT state and playhead: pose it directly.
			if (anim_phase >= 0) {
				const int32_t source_state =
						wfield_i(p, base, Simulation::PF_ANIM_SOURCE_STATE);
				const String source_key = source_state >= 0
						? infantry_key(source_state)
						: String();
				const float blend_weight =
						p[base + Simulation::PF_ANIM_BLEND_WEIGHT];
				// Each channel poses its served ring entry (PF_ANIM_VARIANT /
				// PF_ANIM_SOURCE_VARIANT).
				const int32_t variant =
						wfield_i(p, base, Simulation::PF_ANIM_VARIANT);
				if (!source_key.is_empty() && blend_weight < 1.0f) {
					model->play_body_blend_at(source_key,
							wfield_i(p, base,
									Simulation::PF_ANIM_SOURCE_PHASE_TICKS),
							key, anim_phase, blend_weight,
							wfield_i(p, base, Simulation::PF_ANIM_SOURCE_VARIANT),
							variant);
				} else {
					model->play_body_clip_at(key, anim_phase, variant);
				}
				return;
			}
			model->play_body_clip(key);
			return;
		}
	}
	if (!remote_request) {
		const int32_t source_state =
				wfield_i(p, base, Simulation::PF_ANIM_SOURCE_STATE);
		if (source_state >= 0) {
			const String &source_key = infantry_key(source_state);
			if (!source_key.is_empty()) {
				model->play_body_clip_at(source_key,
						wfield_i(p, base,
								Simulation::PF_ANIM_SOURCE_PHASE_TICKS),
						wfield_i(p, base, Simulation::PF_ANIM_SOURCE_VARIANT));
				return;
			}
		}
	}
	const int32_t body_anim_slot =
			wfield_i(p, base, Simulation::PF_BODY_ANIM_SLOT);
	if (body_anim_slot < 0) {
		return;
	}
	if (anim_phase >= 0) {
		model->play_body_anim_at(body_anim_slot, anim_phase);
		return;
	}
	model->play_body_anim(body_anim_slot);
}

void EntityPresenter::store_wire_remote_body_cache(const WireRow &row) {
	RemoteBodyCache cache;
	cache.state = row.anim_state;
	cache.request = row.anim_request;
	cache.latch = row.remote_body_tick;
	wire_remote_body_.insert(row.handle, cache);
}

// This body's third-person gun — retail's draw 5, for a remote player. The sim
// already folded the draw gate in (ADM 0 = unarmed/hidden). Drawn RIGID: posed
// entirely by bone 16's joint plus the weapon's own attach basis. The graphic
// resolve is edged on the ADM (get_weapon_third_person_model is a pure table
// lookup, so an unchanged ADM cannot change the graphic); the rebuild itself —
// placer build, naming, the weapon-node maps consumers poke — is the cold
// path's rebuild_held_weapon.
// [orig: BoneCallback_org0_World draw 5 @ 0x4e3c87..0x4e3d99; matrix
//  @ 0x4b2180..0x4b22f8; gate Entity_CanFireWeapon @ 0x4dcb10]
void EntityPresenter::update_wire_held_weapon(WireRow &row, Node3D *node,
		PresentRowsView snap, bool body_visible) {
	const float *p = snap.ptr();
	const int base = row.base;
	const int32_t adm =
			wfield_i(p, base, Simulation::PF_HELD_WEAPON_ADM);
	const int32_t *last_adm = wire_held_weapon_adm_.getptr(row.handle);
	if (last_adm == nullptr || *last_adm != adm) {
		wire_held_weapon_adm_.insert(row.handle, adm);
		Node3D *weapon_obj = rebuild_held_weapon(row.handle, adm);
		wire_held_weapon_ids_.insert(row.handle,
				weapon_obj != nullptr ? ObjectID(weapon_obj->get_instance_id())
									  : ObjectID());
	}
	const ObjectID *weapon_id = wire_held_weapon_ids_.getptr(row.handle);
	if (weapon_id == nullptr) {
		return;
	}
	Node3D *weapon =
			Object::cast_to<Node3D>(ObjectDB::get_instance(*weapon_id));
	if (weapon == nullptr) {
		return;
	}
	if (adm <= 0 || !body_visible) {
		weapon->set_visible(false);
		return;
	}
	const Variant attach = held_weapon_attach_transform(
			find_skeleton(node),
			Vector3(p[base + Simulation::PF_HELD_WEAPON_PITCH_DEG],
					p[base + Simulation::PF_HELD_WEAPON_YAW_DEG],
					p[base + Simulation::PF_HELD_WEAPON_ROLL_DEG]),
			p[base + Simulation::PF_HELD_WEAPON_HAND_FRAME] != 0.0f);
	if (attach.get_type() != Variant::TRANSFORM3D) {
		weapon->set_visible(false);
		return;
	}
	weapon->set_global_transform(attach);
	weapon->set_visible(true);
}

} // namespace godot
