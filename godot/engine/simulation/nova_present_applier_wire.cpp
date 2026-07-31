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

// Wire-walk dispatch StringNames (function-local static: safe after Godot init).
struct WireDispatchNames {
	StringName set_right_hand_collapsed = StringName("set_right_hand_collapsed");
	StringName set_aim_overlay = StringName("set_aim_overlay");
	StringName begin_ctrl_update = StringName("begin_ctrl_update");
	StringName end_ctrl_update = StringName("end_ctrl_update");
	StringName set_part_phase = StringName("set_part_phase");
	StringName clear_part_phase = StringName("clear_part_phase");
	StringName reset_remote_body_state = StringName("reset_remote_body_state");
	StringName apply_remote_body_state = StringName("apply_remote_body_state");
	StringName advance_remote_body_blend_tick =
			StringName("advance_remote_body_blend_tick");
	StringName play_body_clip = StringName("play_body_clip");
	StringName play_body_clip_at = StringName("play_body_clip_at");
	StringName play_body_blend_at = StringName("play_body_blend_at");
	StringName play_body_anim = StringName("play_body_anim");
	StringName play_body_anim_at = StringName("play_body_anim_at");
	StringName set_weapon_channel = StringName("set_weapon_channel");
};

const WireDispatchNames &wnames() {
	static WireDispatchNames n;
	return n;
}

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
	cache.insert(state, NovaSimulation::infantry_anim_key(state));
	return *cache.getptr(state);
}

} // namespace

int NovaPresentApplier::wire_node_caps(Object *node, int visual_ctrl_caps) {
	// The wire pass's capability superset, resolved once per plan append —
	// the per-frame has_method probes this replaces were a measured hot cost.
	const WireDispatchNames &n = wnames();
	int caps = 0;
	if (node->has_method(n.set_aim_overlay)) {
		caps |= WIRE_CAP_AIM;
	}
	if ((visual_ctrl_caps & (VISUAL_CTRL_OWNED | VISUAL_CTRL_LEGACY)) != 0) {
		caps |= WIRE_CAP_CTRL;
	}
	if ((visual_ctrl_caps & VISUAL_PART_PHASE) != 0) {
		caps |= WIRE_CAP_PART;
	}
	if ((visual_ctrl_caps & VISUAL_PART_CLEAR) != 0) {
		caps |= WIRE_CAP_PART_CLEAR;
	}
	if ((visual_ctrl_caps & VISUAL_CTRL_BATCH) != 0 &&
			(caps & (WIRE_CAP_CTRL | WIRE_CAP_PART)) != 0) {
		caps |= WIRE_CAP_CTRL_BATCH;
	}
	if (node->has_method(n.apply_remote_body_state)) {
		caps |= WIRE_CAP_REMOTE_BODY;
	}
	if (node->has_method(n.play_body_clip)) {
		caps |= WIRE_CAP_BODY_CLIP;
	}
	if (node->has_method(n.play_body_clip_at)) {
		caps |= WIRE_CAP_BODY_CLIP_AT;
	}
	if (node->has_method(n.play_body_anim_at)) {
		caps |= WIRE_CAP_BODY_SLOT_AT;
	}
	if (node->has_method(n.play_body_anim)) {
		caps |= WIRE_CAP_BODY_SLOT;
	}
	if (node->has_method(n.set_right_hand_collapsed)) {
		caps |= WIRE_CAP_RHC;
	}
	if (node->has_method(n.set_weapon_channel)) {
		caps |= WIRE_CAP_WPN;
	}
	if (node->has_method(n.play_body_blend_at)) {
		caps |= WIRE_CAP_BODY_BLEND_AT;
	}
	if (node->has_method(n.advance_remote_body_blend_tick)) {
		caps |= WIRE_CAP_REMOTE_BLEND_TICK;
	}
	if (node->has_method(n.reset_remote_body_state)) {
		caps |= WIRE_CAP_RESET_REMOTE;
	}
	return caps;
}

void NovaPresentApplier::setup_wire(const Callable &rebuild_held_weapon) {
	wire_rebuild_held_weapon_ = rebuild_held_weapon;
	wire_plan_dirty_ = true;
	wire_rows_.clear();
	wire_deferred_ids_.clear();
}

void NovaPresentApplier::begin_wire_plan(int64_t layout_revision, int stride,
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

void NovaPresentApplier::append_wire_row(Object *node, int base, int handle,
		bool spawned_now) {
	WireRow row;
	row.base = base;
	row.handle = handle;
	row.node_id = node != nullptr ? ObjectID(node->get_instance_id()) : ObjectID();
	row.visual_ctrl_caps = get_visual_control_capabilities(node);
	row.caps = node != nullptr ? wire_node_caps(node, row.visual_ctrl_caps) : 0;
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

void NovaPresentApplier::append_wire_deferred(Object *node) {
	if (node != nullptr) {
		wire_deferred_ids_.push_back(ObjectID(node->get_instance_id()));
	}
}

bool NovaPresentApplier::wire_plan_is_current(int64_t snapshot_size, int stride,
		int64_t layout_revision, int64_t index_generation, int local_handle) {
	// Without a source revision, preserve compatibility by taking the cold path.
	// The revision keys on exactly the per-row identity quintet
	// (wire_handle/type_id/bms_id/kind/index — nova_simulation_present.cpp), so
	// revision equality replaces the GDScript plan's per-row identity re-reads;
	// node swaps mark the plan dirty through release_wire_handle.
	if (wire_plan_dirty_ || layout_revision < 0 ||
			wire_plan_revision_ != layout_revision ||
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

void NovaPresentApplier::release_wire_handle(int handle) {
	wire_remote_body_.erase(handle);
	wire_respawn_revisions_.erase(handle);
	wire_held_weapon_adm_.erase(handle);
	wire_held_weapon_ids_.erase(handle);
	wire_plan_dirty_ = true;
}

void NovaPresentApplier::reset_wire_runtime_state() {
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

void NovaPresentApplier::present_wire_rows(const PackedFloat32Array &snap,
		int stride, int tick_delta) {
	const int64_t size = snap.size();
	for (WireRow &row : wire_rows_) {
		if (row.base < 0 || row.base + stride > size) {
			continue;
		}
		Object *node_obj = ObjectDB::get_instance(row.node_id);
		Node3D *node = Object::cast_to<Node3D>(node_obj);
		if (node == nullptr) {
			continue;
		}
		present_one_wire_row(row, node, snap, tick_delta);
		row.spawned_now = false;
	}
}

void NovaPresentApplier::present_one_wire_row(WireRow &row, Node3D *node,
		const PackedFloat32Array &snap, int tick_delta) {
	const WireDispatchNames &n = wnames();
	const float *p = snap.ptr();
	const int base = row.base;
	const int32_t respawn_revision =
			wfield_i(p, base, NovaSimulation::PF_RESPAWN_REVISION);
	const int32_t *seen_revision = wire_respawn_revisions_.getptr(row.handle);
	const bool respawned_since_present = !row.spawned_now &&
			seen_revision != nullptr && *seen_revision != respawn_revision;
	const Vector3 pos(p[base + NovaSimulation::PF_POS_X],
			p[base + NovaSimulation::PF_POS_Y],
			p[base + NovaSimulation::PF_POS_Z]);
	const Vector3 rot(p[base + NovaSimulation::PF_PITCH_DEG],
			p[base + NovaSimulation::PF_YAW_DEG],
			p[base + NovaSimulation::PF_ROLL_DEG]);
	const Basis entity_basis = bms_to_godot_basis(rot);
	const Basis root_basis = (row.caps & WIRE_CAP_AIM) != 0
			? aim_root_basis(snap, base, entity_basis)
			: entity_basis;
	const Transform3D next_transform(root_basis, pos);
	if (node->get_transform() != next_transform) {
		node->set_transform(next_transform);
	}
	// The mounted right-hand collapse rides its own packed field; edge-gated to
	// the value change like MissionPresentPass.
	if ((row.caps & WIRE_CAP_RHC) != 0) {
		const int32_t rhc =
				wfield_i(p, base, NovaSimulation::PF_RIGHT_HAND_COLLAPSED);
		if (rhc != row.rhc) {
			node->call(n.set_right_hand_collapsed, rhc != 0);
		}
		row.rhc = rhc;
	}
	// Aim overlay: apply while valid, clear only on the valid->invalid edge.
	if ((row.caps & WIRE_CAP_AIM) != 0) {
		const int32_t aim_valid =
				wfield_i(p, base, NovaSimulation::PF_AIM_OVERLAY_VALID);
		if (aim_valid != 0) {
			aim_apply_valid(node, snap, base, false);
		} else if (row.aim_valid != 0) {
			node->call(n.set_aim_overlay, Array());
		}
		row.aim_valid = aim_valid;
	}
	const bool ctrl_batch = (row.caps & WIRE_CAP_CTRL_BATCH) != 0;
	if (ctrl_batch) {
		node->call(n.begin_ctrl_update);
	}
	if ((row.caps & WIRE_CAP_CTRL) != 0) {
		if ((row.caps & WIRE_CAP_PART) != 0) {
			apply_wire_procedural_part(row, node, snap);
		}
		// All four semantic CTRL writers through the cached dispatch mode:
		// compact joiner rows release absent authoritative fields, while direct
		// host/synthetic rows publish vehicle, zone and attachment heat values.
		// [orig: Entity_CacheVehicleHUDStats @ 0x4929B0;
		//  BoneCallback_gnrc_World @ 0x4E288B..0x4E28FB;
		//  parent UseGun attachment @ 0x546518 -> cache @ 0x440930]
		wire_controls_apply_with_capabilities(node, snap, base,
				row.visual_ctrl_caps);
	} else if ((row.caps & WIRE_CAP_PART) != 0) {
		apply_wire_procedural_part(row, node, snap);
	}
	if (ctrl_batch) {
		node->call(n.end_ctrl_update);
	}
	if (respawned_since_present && (row.caps & WIRE_CAP_RESET_REMOTE) != 0) {
		node->call(n.reset_remote_body_state);
		row.anim_state = -2; // force the next body-anim dispatch through
		row.remote_body_tick = 0;
	}
	apply_wire_body_anim(row, node, snap, tick_delta);
	// The upper-body weapon channel: the hold pose this player's held weapon and
	// scope state select. -1 means no channel this frame, which clears any pose
	// left over from the weapon it was holding before.
	// [orig: the selection Entity_UpdateInfantryPlayerBody @ 0x4b5dad, which
	//  retail runs for every player body it draws, not just the local one]
	if ((row.caps & WIRE_CAP_WPN) != 0) {
		const int32_t wpn_state =
				wfield_i(p, base, NovaSimulation::PF_WPN_ANIM_STATE);
		node->call(n.set_weapon_channel,
				wpn_state >= 0 ? infantry_key_cached(wpn_state) : String(),
				wfield_i(p, base, NovaSimulation::PF_WPN_PHASE_TICKS));
	}
	const bool next_visible =
			wfield_i(p, base, NovaSimulation::PF_HIDDEN) == 0 &&
			wfield_i(p, base, NovaSimulation::PF_LOCAL_VIEW_SUPPRESSED) == 0;
	update_wire_held_weapon(row, node, snap, next_visible);
	wire_respawn_revisions_.insert(row.handle, respawn_revision);
	if (node->is_visible() != next_visible) {
		node->set_visible(next_visible);
	}
}

// Keep dynamically materialized items on the same PANM path as placed mission
// objects. The ACTIVE fields are publication ownership, so an owned zero phase
// must still be written and a suppressed channel must release its prior value.
void NovaPresentApplier::apply_wire_procedural_part(const WireRow &row,
		Node3D *node, const PackedFloat32Array &snap) {
	const WireDispatchNames &n = wnames();
	const float *p = snap.ptr();
	const int base = row.base;
	if (wfield_i(p, base, NovaSimulation::PF_ACTIVE1) > 0) {
		node->call(n.set_part_phase, 1,
				NovaSimulation::decode_present_part_anim_phase(snap, base, 1));
	} else if ((row.caps & WIRE_CAP_PART_CLEAR) != 0) {
		node->call(n.clear_part_phase, 1);
	}
	if (wfield_i(p, base, NovaSimulation::PF_ACTIVE2) > 0) {
		node->call(n.set_part_phase, 2,
				NovaSimulation::decode_present_part_anim_phase(snap, base, 2));
	} else if ((row.caps & WIRE_CAP_PART_CLEAR) != 0) {
		node->call(n.clear_part_phase, 2);
	}
}

// The wire-driven skeletal primary pose, on the same projection path as the
// mission walk. REMOTE-request rows skip re-dispatch entirely while the wire
// state is unchanged; host-loopback rows (remote_request 0) keep per-tick
// dispatch — their playhead rides play_body_clip_at's phase.
void NovaPresentApplier::apply_wire_body_anim(WireRow &row, Node3D *node,
		const PackedFloat32Array &snap, int tick_delta) {
	const WireDispatchNames &n = wnames();
	const float *p = snap.ptr();
	const int base = row.base;
	const int32_t anim_state = wfield_i(p, base, NovaSimulation::PF_ANIM_STATE);
	const int32_t remote_request_i =
			wfield_i(p, base, NovaSimulation::PF_ANIM_REMOTE_REQUEST);
	const int32_t anim_pulse =
			wfield_i(p, base, NovaSimulation::PF_ANIM_STATE_PULSE);
	// Accepted/queued remote transitions advance exactly once per simulation
	// tick. The row latch is set only when the model reports live transition
	// work, so steady-state rows never dispatch.
	if (row.remote_body_tick != 0 && remote_request_i != 0 && anim_pulse < 0 &&
			anim_state == row.anim_state && remote_request_i == row.anim_request &&
			(row.caps & WIRE_CAP_REMOTE_BLEND_TICK) != 0) {
		int ticks_left = tick_delta;
		while (ticks_left > 0 && row.remote_body_tick != 0) {
			row.remote_body_tick =
					bool(node->call(n.advance_remote_body_blend_tick, anim_state))
					? 1
					: 0;
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
			wfield_i(p, base, NovaSimulation::PF_ANIM_PHASE_TICKS);
	const bool remote_request = remote_request_i != 0;
	bool remote_needs_tick = false;
	// A transition state that arrived and was overwritten within one decode fold
	// dispatches FIRST so the model's arbitration sees retail's per-record
	// order. [orig: per-record remote anim apply @ 0x4c1153]
	if (remote_request && anim_pulse >= 0 &&
			(row.caps & WIRE_CAP_REMOTE_BODY) != 0) {
		const String &pulse_key = infantry_key_cached(anim_pulse);
		if (!pulse_key.is_empty()) {
			remote_needs_tick = bool(node->call(n.apply_remote_body_state,
					anim_pulse, pulse_key,
					NovaSimulation::infantry_anim_flags(anim_pulse),
					wfield_i(p, base, NovaSimulation::PF_ANIM_PULSE_TICKS)));
		}
	}
	if (anim_state >= 0) {
		const String &key = infantry_key_cached(anim_state);
		if (!key.is_empty()) {
			// NovaObjectModel owns current/pending acceptance because it also
			// owns clip time and completion. Forward every raw wire request.
			// [orig: @ 0x4c0859 / @ 0x4c11a6]
			if (remote_request && (row.caps & WIRE_CAP_REMOTE_BODY) != 0) {
				remote_needs_tick = bool(node->call(n.apply_remote_body_state,
						anim_state, key,
						NovaSimulation::infantry_anim_flags(anim_state),
						anim_phase));
				row.remote_body_tick = remote_needs_tick ? 1 : 0;
				store_wire_remote_body_cache(row);
				return;
			}
			if (remote_request && (row.caps & WIRE_CAP_BODY_CLIP) != 0) {
				node->call(n.play_body_clip, key);
				return;
			}
			// Host-loopback rows expose the authority's already-accepted
			// CURRENT state and playhead: pose it directly.
			if (!remote_request && anim_phase >= 0 &&
					(row.caps & WIRE_CAP_BODY_CLIP_AT) != 0) {
				const int32_t source_state =
						wfield_i(p, base, NovaSimulation::PF_ANIM_SOURCE_STATE);
				const String source_key = source_state >= 0
						? infantry_key_cached(source_state)
						: String();
				const float blend_weight =
						p[base + NovaSimulation::PF_ANIM_BLEND_WEIGHT];
				if (!source_key.is_empty() && blend_weight < 1.0f &&
						(row.caps & WIRE_CAP_BODY_BLEND_AT) != 0) {
					node->call(n.play_body_blend_at, source_key,
							wfield_i(p, base,
									NovaSimulation::PF_ANIM_SOURCE_PHASE_TICKS),
							key, anim_phase, blend_weight);
				} else {
					node->call(n.play_body_clip_at, key, anim_phase);
				}
				return;
			}
			if (!remote_request && (row.caps & WIRE_CAP_BODY_CLIP) != 0) {
				node->call(n.play_body_clip, key);
				return;
			}
		}
	}
	if (!remote_request && (row.caps & WIRE_CAP_BODY_CLIP_AT) != 0) {
		const int32_t source_state =
				wfield_i(p, base, NovaSimulation::PF_ANIM_SOURCE_STATE);
		if (source_state >= 0) {
			const String &source_key = infantry_key_cached(source_state);
			if (!source_key.is_empty()) {
				node->call(n.play_body_clip_at, source_key,
						wfield_i(p, base,
								NovaSimulation::PF_ANIM_SOURCE_PHASE_TICKS));
				return;
			}
		}
	}
	const int32_t body_anim_slot =
			wfield_i(p, base, NovaSimulation::PF_BODY_ANIM_SLOT);
	if (body_anim_slot < 0) {
		return;
	}
	if (anim_phase >= 0 && (row.caps & WIRE_CAP_BODY_SLOT_AT) != 0) {
		node->call(n.play_body_anim_at, body_anim_slot, anim_phase);
		return;
	}
	if ((row.caps & WIRE_CAP_BODY_SLOT) != 0) {
		node->call(n.play_body_anim, body_anim_slot);
	}
}

void NovaPresentApplier::store_wire_remote_body_cache(const WireRow &row) {
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
void NovaPresentApplier::update_wire_held_weapon(WireRow &row, Node3D *node,
		const PackedFloat32Array &snap, bool body_visible) {
	const float *p = snap.ptr();
	const int base = row.base;
	const int32_t adm =
			wfield_i(p, base, NovaSimulation::PF_HELD_WEAPON_ADM);
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
			Vector3(p[base + NovaSimulation::PF_HELD_WEAPON_PITCH_DEG],
					p[base + NovaSimulation::PF_HELD_WEAPON_YAW_DEG],
					p[base + NovaSimulation::PF_HELD_WEAPON_ROLL_DEG]),
			p[base + NovaSimulation::PF_HELD_WEAPON_HAND_FRAME] != 0.0f);
	if (attach.get_type() != Variant::TRANSFORM3D) {
		weapon->set_visible(false);
		return;
	}
	weapon->set_global_transform(attach);
	weapon->set_visible(true);
}

Object *NovaPresentApplier::find_wire_skeleton(Node *root) {
	// The recursive Skeleton3D walk the GDScript reference ran per call, native
	// (NovaObjectModel.rebuild() frees children, so caching the result by
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
