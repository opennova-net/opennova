#include "simulation/nova_present_applier.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "simulation/nova_simulation.h"

// The WIRE (joiner/MP) per-row hot walk — the native twin of the plan walk
// wire_present_pass.gd carried before this TU (that facade keeps the COLD path:
// spawn/defer/unresolved bookkeeping, the liveness prune, spawn callbacks and
// stats; per-leg behavioral semantics and their [orig] witnesses moved here
// with the code). The walk deliberately TRANSLITERATES the GDScript's dispatch
// pattern — the same legs fire in the same order under the same conditions —
// so the pass's behavioral contract (pinned by wire_present_pass_test.gd's 33
// cases) is preserved by construction; further edge-gating of the
// every-frame legs (aim payload, ctrl publish, weapon channel) is a measured
// follow-up, not part of this move.

using namespace godot;

namespace {

inline int32_t wfield_i(const float *p, int base, int field) {
	return static_cast<int32_t>(p[base + field]);
}

// state id -> "anim_<name>" String, memoized once per process (the same cache
// the GDScript pass carried: the native key call allocates a fresh String per
// invocation, which on a joiner ran twice per row per frame).
const String &infantry_key_cached(int state) {
	static HashMap<int, String> cache;
	String *found = cache.getptr(state);
	if (found != nullptr) {
		return *found;
	}
	cache.insert(state, Simulation::infantry_anim_key(state));
	return *cache.getptr(state);
}

// The exact PF fields each edge-gated leg consumes. A new field read inside a
// gated leg MUST join its table, or the gate holds stale node output. Gating
// is presentation memoization only: identical inputs re-dispatched to the same
// node produce the same node state, so skipping the redundant dispatch cannot
// change what renders — it removes the measured per-frame Variant/String/Array
// churn (2026-08-04 joiner profile: ~21us/row over 876 mostly-static rows).
// Mirrors of WireRow::kCtrlCacheCount / kAimCacheCount (the struct is class-
// private); static_asserts in present_one_wire_row pin the mirror.
constexpr int kCtrlLegFieldCount = 18;
constexpr int kAimLegFieldCount = 30;

constexpr int kCtrlLegFields[kCtrlLegFieldCount] = {
	Simulation::PF_EMPLACED_CONTROLS_VALID,
	Simulation::PF_EWEAP_GUNYAW,
	Simulation::PF_EWEAP_GUNPITCH,
	Simulation::PF_VEHICLE_MOTION_VALID,
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
};

// The aim-overlay leg's inputs: the body triple plus all nine overlay-class
// triples (aim_apply_valid's exact reads).
struct AimLegFields {
	int fields[kAimLegFieldCount];
	AimLegFields() {
		fields[0] = Simulation::PF_AIM_BODY_PITCH_DEG;
		fields[1] = Simulation::PF_AIM_BODY_YAW_DEG;
		fields[2] = Simulation::PF_AIM_BODY_ROLL_DEG;
		int write = 3;
		for (int overlay_class = 0; overlay_class < 9; ++overlay_class) {
			const int offset = Simulation::PF_AIM_ANGLES +
					overlay_class * Simulation::PF_AIM_CLASS_STRIDE;
			fields[write++] = offset;
			fields[write++] = offset + 1;
			fields[write++] = offset + 2;
		}
	}
};

const int *aim_leg_fields() {
	static AimLegFields table;
	return table.fields;
}

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

void PresentApplier::setup_wire(const Callable &rebuild_held_weapon) {
	wire_rebuild_held_weapon_ = rebuild_held_weapon;
	wire_plan_dirty_ = true;
	wire_rows_.clear();
	wire_deferred_ids_.clear();
}

void PresentApplier::begin_wire_plan(int64_t layout_revision, int stride,
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

void PresentApplier::append_wire_row(Object *node, int base, int handle,
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

void PresentApplier::append_wire_deferred(Object *node) {
	if (node != nullptr) {
		wire_deferred_ids_.push_back(ObjectID(node->get_instance_id()));
	}
}

bool PresentApplier::wire_plan_is_current(int64_t snapshot_size, int stride,
		int64_t layout_revision, int64_t index_generation, int local_handle) {
	// The revision keys on exactly the per-row identity quintet
	// (wire_handle/type_id/bms_id/kind/index — nova_simulation_present.cpp), so
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

void PresentApplier::release_wire_handle(int handle) {
	wire_remote_body_.erase(handle);
	wire_respawn_revisions_.erase(handle);
	wire_held_weapon_adm_.erase(handle);
	wire_held_weapon_ids_.erase(handle);
	wire_plan_dirty_ = true;
}

void PresentApplier::reset_wire_runtime_state() {
	wire_rows_.clear();
	wire_deferred_ids_.clear();
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

void PresentApplier::present_wire_rows(const PackedFloat32Array &snap,
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

void PresentApplier::present_one_wire_row(WireRow &row, ObjectModel *model,
		const PackedFloat32Array &snap, int tick_delta) {
	static_assert(kCtrlLegFieldCount == WireRow::kCtrlCacheCount,
			"ctrl leg field table must match the row cache size");
	static_assert(kAimLegFieldCount == WireRow::kAimCacheCount,
			"aim leg field table must match the row cache size");
	const float *p = snap.ptr();
	const int base = row.base;
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
	const Basis root_basis = aim_root_basis(snap, base, entity_basis);
	const Transform3D next_transform(root_basis, pos);
	if (model->get_transform() != next_transform) {
		model->set_transform(next_transform);
	}
	// The mounted right-hand collapse rides its own packed field; edge-gated to
	// the value change like MissionPresentPass.
	{
		const int32_t rhc =
				wfield_i(p, base, Simulation::PF_RIGHT_HAND_COLLAPSED);
		if (rhc != row.rhc) {
			model->set_right_hand_collapsed(rhc != 0);
		}
		row.rhc = rhc;
	}
	// Aim overlay: apply while valid, clear only on the valid->invalid edge.
	// The apply itself is input-gated: identical overlay angles re-dispatch
	// the identical delta set, so only changed inputs build the 9-basis Array.
	// The clear edge invalidates the cache so a later re-valid always applies.
	{
		const int32_t aim_valid =
				wfield_i(p, base, Simulation::PF_AIM_OVERLAY_VALID);
		if (aim_valid != 0) {
			if (!leg_inputs_unchanged(p, base, aim_leg_fields(),
					kAimLegFieldCount, row.aim_cache,
					row.aim_cache_valid)) {
				aim_apply_valid(model, snap, base, false);
			}
		} else if (row.aim_valid != 0) {
			model->set_aim_overlay(Array());
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
		row.wpn_phase = INT32_MIN;
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
		if (wpn_state != row.wpn_state || wpn_phase != row.wpn_phase) {
			model->set_weapon_channel(
					wpn_state >= 0 ? infantry_key_cached(wpn_state) : String(),
					wpn_phase);
			row.wpn_state = wpn_state;
			row.wpn_phase = wpn_phase;
		}
	}
	const bool next_visible =
			wfield_i(p, base, Simulation::PF_HIDDEN) == 0 &&
			wfield_i(p, base, Simulation::PF_LOCAL_VIEW_SUPPRESSED) == 0;
	update_wire_held_weapon(row, model, snap, next_visible);
	wire_respawn_revisions_.insert(row.handle, respawn_revision);
	if (model->is_visible() != next_visible) {
		model->set_visible(next_visible);
	}
}

// Keep dynamically materialized items on the same PANM path as placed mission
// objects. The ACTIVE fields are publication ownership, so an owned zero phase
// must still be written and a suppressed channel must release its prior value.
void PresentApplier::apply_wire_procedural_part(const WireRow &row,
		ObjectModel *model, const PackedFloat32Array &snap) {
	const float *p = snap.ptr();
	const int base = row.base;
	if (wfield_i(p, base, Simulation::PF_ACTIVE1) > 0) {
		model->set_part_phase(1,
				Simulation::decode_present_part_anim_phase(snap, base, 1));
	} else {
		model->clear_part_phase(1);
	}
	if (wfield_i(p, base, Simulation::PF_ACTIVE2) > 0) {
		model->set_part_phase(2,
				Simulation::decode_present_part_anim_phase(snap, base, 2));
	} else {
		model->clear_part_phase(2);
	}
}

// The wire-driven skeletal primary pose, on the same projection path as the
// mission walk. REMOTE-request rows skip re-dispatch entirely while the wire
// state is unchanged; host-loopback rows (remote_request 0) keep per-tick
// dispatch — their playhead rides play_body_clip_at's phase.
void PresentApplier::apply_wire_body_anim(WireRow &row, ObjectModel *model,
		const PackedFloat32Array &snap, int tick_delta) {
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
		const String &pulse_key = infantry_key_cached(anim_pulse);
		if (!pulse_key.is_empty()) {
			remote_needs_tick = model->apply_remote_body_state(
					anim_pulse, pulse_key,
					Simulation::infantry_anim_flags(anim_pulse),
					wfield_i(p, base, Simulation::PF_ANIM_PULSE_TICKS));
		}
	}
	if (anim_state >= 0) {
		const String &key = infantry_key_cached(anim_state);
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
						? infantry_key_cached(source_state)
						: String();
				const float blend_weight =
						p[base + Simulation::PF_ANIM_BLEND_WEIGHT];
				if (!source_key.is_empty() && blend_weight < 1.0f) {
					model->play_body_blend_at(source_key,
							wfield_i(p, base,
									Simulation::PF_ANIM_SOURCE_PHASE_TICKS),
							key, anim_phase, blend_weight);
				} else {
					model->play_body_clip_at(key, anim_phase);
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
			const String &source_key = infantry_key_cached(source_state);
			if (!source_key.is_empty()) {
				model->play_body_clip_at(source_key,
						wfield_i(p, base,
								Simulation::PF_ANIM_SOURCE_PHASE_TICKS));
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

void PresentApplier::store_wire_remote_body_cache(const WireRow &row) {
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
// placer build, naming, the _weapon_nodes/_weapon_graphics maps consumers poke
// — stays on the facade behind the rebuild Callable.
// [orig: BoneCallback_org0_World draw 5 @ 0x4e3c87..0x4e3d99; matrix
//  @ 0x4b2180..0x4b22f8; gate Entity_CanFireWeapon @ 0x4dcb10]
void PresentApplier::update_wire_held_weapon(WireRow &row, Node3D *node,
		const PackedFloat32Array &snap, bool body_visible) {
	const float *p = snap.ptr();
	const int base = row.base;
	const int32_t adm =
			wfield_i(p, base, Simulation::PF_HELD_WEAPON_ADM);
	const int32_t *last_adm = wire_held_weapon_adm_.getptr(row.handle);
	if (last_adm == nullptr || *last_adm != adm) {
		wire_held_weapon_adm_.insert(row.handle, adm);
		Object *weapon_obj = nullptr;
		if (wire_rebuild_held_weapon_.is_valid()) {
			weapon_obj = Object::cast_to<Object>(
					wire_rebuild_held_weapon_.call(row.handle, adm));
		}
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
			find_wire_skeleton(node),
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

Object *PresentApplier::find_wire_skeleton(Node *root) {
	// The recursive Skeleton3D walk the GDScript reference ran per call, native
	// (ObjectModel.rebuild() frees children, so caching the result by
	// ObjectID would go stale mid-play; the walk itself is now cheap).
	if (root == nullptr) {
		return nullptr;
	}
	if (Object::cast_to<Skeleton3D>(root) != nullptr) {
		return root;
	}
	for (int i = 0; i < root->get_child_count(); ++i) {
		Object *found = find_wire_skeleton(root->get_child(i));
		if (found != nullptr) {
			return found;
		}
	}
	return nullptr;
}
