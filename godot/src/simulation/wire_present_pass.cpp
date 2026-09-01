#include "simulation/wire_present_pass.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <net/npruntime/wire_present.h>
#include <runtime/world/present_rows.h>
#include <runtime/world/tick_accumulator.h>

#include "simulation/simulation.h"
#include "object/object_data.h"

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

} // namespace

Simulation *WirePresentPass::sim() const {
	return sim_id_.is_valid()
			? Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_))
			: nullptr;
}

Node3D *WirePresentPass::container() const {
	return container_id_.is_valid()
			? Object::cast_to<Node3D>(ObjectDB::get_instance(container_id_))
			: nullptr;
}

void WirePresentPass::setup(Object *p_sim,
		const Ref<MissionObjectPlacer> &p_placer, Node3D *p_container,
		const Ref<EntityIndex> &p_defer_index) {
	sim_id_ = p_sim != nullptr ? p_sim->get_instance_id() : ObjectID();
	placer_ = p_placer;
	container_id_ =
			p_container != nullptr ? p_container->get_instance_id() : ObjectID();
	defer_index_ = p_defer_index;
	pending_spawn_count_ = 0;
	camera_framed_ = false;
	last_present_logic_tick_ = -1;
	applier_.instantiate();
	applier_->setup_wire(
			callable_mp(this, &WirePresentPass::rebuild_held_weapon));
}

void WirePresentPass::set_synthetic_origin_only(bool p_enabled) {
	synthetic_origin_only_ = p_enabled;
}

void WirePresentPass::set_render_culled(int p_handle, bool p_culled) {
	if (applier_.is_valid()) applier_->set_wire_render_culled(p_handle, p_culled);
}

void WirePresentPass::clear_render_culled() {
	if (applier_.is_valid()) applier_->clear_wire_render_culled();
}

void WirePresentPass::set_cold_spawn_budget(int p_budget) {
	cold_spawn_budget_ = MAX(1, p_budget);
}

void WirePresentPass::set_spectator_camera(Camera3D *p_camera) {
	camera_id_ = p_camera != nullptr ? p_camera->get_instance_id() : ObjectID();
	camera_framed_ = false;
}

Ref<WirePresentStats> WirePresentPass::get_stats_record() const {
	return WirePresentStats::create(stat_live_, stat_spawned_,
			stat_unresolved_, pending_spawn_count_);
}

ObjectModel *WirePresentPass::resolve_wire_handle(int p_handle) const {
	const ObjectID *id = nodes_.getptr(p_handle);
	return id != nullptr ? model_for_id(*id) : nullptr;
}

ObjectModel *WirePresentPass::held_weapon_node(int p_handle) const {
	const ObjectID *id = weapon_nodes_.getptr(p_handle);
	return id != nullptr ? model_for_id(*id) : nullptr;
}

void WirePresentPass::set_entity_lighting_context(int p_handle,
		float p_effect_scale, bool p_interior_lerp, float p_light_transfer) {
	LightingContext context;
	context.effect_scale = CLAMP(p_effect_scale, 0.0f, 1.0f);
	context.interior_lerp = p_interior_lerp;
	context.light_transfer = CLAMP(p_light_transfer, 0.0f, 1.0f);
	lighting_contexts_[p_handle] = context;
	apply_lighting_context(p_handle);
}

void WirePresentPass::apply_lighting_context(int p_handle) {
	const LightingContext *context = lighting_contexts_.getptr(p_handle);
	if (context == nullptr) return;
	if (ObjectModel *body = resolve_wire_handle(p_handle)) {
		body->set_entity_lighting_context(context->effect_scale,
				context->interior_lerp, context->light_transfer);
	}
	if (ObjectModel *weapon = held_weapon_node(p_handle)) {
		weapon->set_entity_lighting_context(context->effect_scale,
				context->interior_lerp, context->light_transfer);
	}
}

void WirePresentPass::free_wire_node(int p_handle) {
	if (ObjectModel *node = resolve_wire_handle(p_handle)) {
		node->queue_free();
	}
	nodes_.erase(p_handle);
	free_held_weapon(p_handle);
	if (applier_.is_valid()) {
		applier_->release_wire_handle(p_handle);
	}
	lighting_contexts_.erase(p_handle);
}

void WirePresentPass::free_held_weapon(int p_handle) {
	if (ObjectModel *weapon = held_weapon_node(p_handle)) {
		weapon->queue_free();
	}
	weapon_nodes_.erase(p_handle);
	weapon_graphics_.erase(p_handle);
}

void WirePresentPass::reset_runtime_state() {
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
	if (applier_.is_valid()) {
		applier_->reset_wire_runtime_state();
	}
	pending_spawn_count_ = 0;
	last_present_logic_tick_ = -1;
	camera_framed_ = false;
	stat_live_ = 0;
}

void WirePresentPass::register_wire_node(int p_handle, ObjectModel *p_node) {
	if (p_node == nullptr) {
		nodes_.erase(p_handle);
		lighting_contexts_.erase(p_handle);
		return;
	}
	nodes_[p_handle] = ObjectID(p_node->get_instance_id());
	apply_lighting_context(p_handle);
}

void WirePresentPass::set_node_spawned_callback(const Callable &p_callback) {
	node_spawned_callback_ = p_callback;
	if (!node_spawned_callback_.is_valid()) {
		return;
	}
	// Replay existing nodes so registration is safe after the first present.
	for (const KeyValue<int32_t, ObjectID> &kv : nodes_) {
		ObjectModel *node = model_for_id(kv.value);
		if (node == nullptr) {
			continue;
		}
		Dictionary ref = node->get_meta("entity_ref", Dictionary());
		node_spawned_callback_.call(node, int(ref.get("kind", -1)),
				int(ref.get("item_id", 0)));
	}
}

void WirePresentPass::present() {
	Simulation *s = sim();
	if (s == nullptr || placer_.is_null() || container() == nullptr) {
		return;
	}
	const int stride = s->get_present_stride();
	if (stride < PF_STRIDE) {
		return;
	}
	present_snapshot(s->get_present_snapshot(), stride,
			s->get_present_layout_revision());
}

void WirePresentPass::present_snapshot(const PackedFloat32Array &p_snap,
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
			applier_->wire_plan_is_current(p_snap.size(), p_stride,
					p_layout_revision, index_generation, local_handle)) {
		applier_->present_wire_rows(p_snap, p_stride, tick_delta);
		frame_spectator_camera();
		return;
	}
	applier_->begin_wire_plan(p_layout_revision, p_stride, p_snap.size(),
			index_generation, local_handle);
	const float *snap = p_snap.ptr();
	const int count = int(p_snap.size() / p_stride);
	HashMap<int32_t, bool> live;
	// [node, runtime_kind, visual_item_id] per spawn; registration runs after
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
		const int runtime_kind = opennova::npruntime::
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
					applier_->append_wire_deferred(placed);
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
			Dictionary ref;
			ref["kind"] = runtime_kind;
			ref["origin_kind"] = int(snap[base + PF_KIND]);
			ref["index"] = int(snap[base + PF_INDEX]);
			ref["bms_id"] = int(snap[base + PF_BMS_ID]);
			ref["wire_handle"] = handle;
			ref["item_id"] = visual_item_id;
			ref["runtime_type_id"] = type_id;
			ref["character_id"] = character_id;
			node->set_meta("entity_ref", ref);
			nodes_[handle] = node->get_instance_id();
			apply_lighting_context(handle);
			++stat_spawned_;
			spawned_now = true;
		}
		applier_->append_wire_row(node, base, handle, spawned_now);
		if (spawned_now) {
			spawned_nodes.push_back(ObjectID(node->get_instance_id()));
			spawned_kinds.push_back(runtime_kind);
			spawned_items.push_back(visual_item_id);
		}
	}
	applier_->present_wire_rows(p_snap, p_stride, tick_delta);
	pending_spawn_count_ = pending_spawns;
	if (node_spawned_callback_.is_valid()) {
		for (uint32_t i = 0; i < spawned_nodes.size(); ++i) {
			if (ObjectModel *node = model_for_id(spawned_nodes[i])) {
				node_spawned_callback_.call(node, spawned_kinds[i],
						spawned_items[i]);
			}
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
void WirePresentPass::frame_spectator_camera() {
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

// Remote primary-channel blends are fixed-tick state, while this pass also
// runs on zero-tick render frames and once after a multi-tick catch-up batch.
// Consume the sim clock once per presented snapshot so every row advances by
// the exact logic-tick delta rather than by the number of render submissions.
int WirePresentPass::consume_present_logic_tick_delta() {
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

bool WirePresentPass::wire_node_matches_row(ObjectModel *p_node,
		const PackedFloat32Array &p_snap, int p_base, int p_type_id) const {
	const float *snap = p_snap.ptr();
	Dictionary ref = p_node->get_meta("entity_ref", Dictionary());
	return int(ref.get("runtime_type_id", 0)) == p_type_id &&
			int(ref.get("character_id", 0)) ==
					(int(snap[p_base + PF_CHARACTER_ID]) & 0xffff) &&
			int(ref.get("origin_kind", -1)) == int(snap[p_base + PF_KIND]) &&
			int(ref.get("index", -1)) == int(snap[p_base + PF_INDEX]) &&
			int(ref.get("bms_id", 0)) == int(snap[p_base + PF_BMS_ID]);
}

Vector3 WirePresentPass::muzzle_world_for(int p_handle,
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
		Dictionary info = data->get_user_point_info(i);
		if (String(info.get("name", String())).nocasecmp_to(p_userpoint) ==
				0) {
			return weapon->get_global_transform().xform(
					Vector3(info.get("position", Vector3())));
		}
	}
	return body_origin;
}

// Build (or free) this wire body's third-person gun model when its ADM
// changes — the applier's wire walk detects the edge and calls back here so
// the placer build, node naming, and the maps muzzle_world_for/tests consume
// stay on the cold path; the per-frame rigid attach lives in the native walk.
// Kept beside nodes_ rather than parented under the body: ObjectModel
// rebuild() frees all of its children, so a child weapon would vanish on any
// body rebuild. (Witness: npruntime/wire_present.h ledger — the model
// resolves off the equipped ADM; the sim folds the draw gate in.)
Node3D *WirePresentPass::rebuild_held_weapon(int p_handle, int p_adm) {
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

void WirePresentPass::_bind_methods() {
	ClassDB::bind_method(
			D_METHOD("setup", "sim", "placer", "container", "defer_index"),
			&WirePresentPass::setup, DEFVAL(Ref<EntityIndex>()));
	ClassDB::bind_method(D_METHOD("set_synthetic_origin_only", "enabled"),
			&WirePresentPass::set_synthetic_origin_only);
	ClassDB::bind_method(D_METHOD("set_render_culled", "wire_handle", "culled"),
			&WirePresentPass::set_render_culled);
	ClassDB::bind_method(D_METHOD("clear_render_culled"),
			&WirePresentPass::clear_render_culled);
	ClassDB::bind_method(D_METHOD("set_cold_spawn_budget", "budget"),
			&WirePresentPass::set_cold_spawn_budget);
	ClassDB::bind_method(D_METHOD("set_spectator_camera", "camera"),
			&WirePresentPass::set_spectator_camera);
	ClassDB::bind_method(D_METHOD("present"), &WirePresentPass::present);
	ClassDB::bind_method(
			D_METHOD("present_snapshot", "snap", "stride", "layout_revision"),
			&WirePresentPass::present_snapshot);
	ClassDB::bind_method(D_METHOD("pending_spawn_count"),
			&WirePresentPass::pending_spawn_count);
	ClassDB::bind_method(D_METHOD("get_stats_record"),
			&WirePresentPass::get_stats_record);
	ClassDB::bind_method(D_METHOD("resolve_wire_handle", "wire_handle"),
			&WirePresentPass::resolve_wire_handle);
	ClassDB::bind_method(D_METHOD("held_weapon_node", "wire_handle"),
			&WirePresentPass::held_weapon_node);
	ClassDB::bind_method(D_METHOD("set_entity_lighting_context", "wire_handle",
			"effect_scale", "interior_lerp", "light_transfer"),
			&WirePresentPass::set_entity_lighting_context);
	ClassDB::bind_method(D_METHOD("muzzle_world_for", "handle", "userpoint"),
			&WirePresentPass::muzzle_world_for);
	ClassDB::bind_method(D_METHOD("entity_count"),
			&WirePresentPass::entity_count);
	ClassDB::bind_method(D_METHOD("set_node_spawned_callback", "callback"),
			&WirePresentPass::set_node_spawned_callback);
	ClassDB::bind_method(D_METHOD("register_wire_node", "handle", "node"),
			&WirePresentPass::register_wire_node);
	ClassDB::bind_method(D_METHOD("reset_runtime_state"),
			&WirePresentPass::reset_runtime_state);
	ClassDB::bind_method(D_METHOD("teardown"), &WirePresentPass::teardown);
	BIND_CONSTANT(DEFAULT_COLD_SPAWN_BUDGET);
}

} // namespace godot
