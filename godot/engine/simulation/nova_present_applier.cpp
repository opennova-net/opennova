#include "simulation/nova_present_applier.h"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include "simulation/nova_simulation.h"

using namespace godot;

namespace {

// Dispatch-surface StringNames built once (function-local statics: safe after
// Godot init, shared across appliers).
struct DispatchNames {
	StringName resolve = StringName("resolve");
	StringName get_generation = StringName("get_generation");
	StringName set_part_phase = StringName("set_part_phase");
	StringName play_body_clip_at = StringName("play_body_clip_at");
	StringName play_body_anim_at = StringName("play_body_anim_at");
	StringName play_body_anim = StringName("play_body_anim");
	StringName set_aim_overlay = StringName("set_aim_overlay");
	StringName set_right_hand_collapsed = StringName("set_right_hand_collapsed");
	StringName set_ctrl_value = StringName("set_ctrl_value");
	StringName clear_ctrl_value = StringName("clear_ctrl_value");
	StringName has_muzzle = StringName("has_muzzle");
	StringName get_muzzle_world_position = StringName("get_muzzle_world_position");
	StringName set_ai_muzzle_world = StringName("set_ai_muzzle_world");
	StringName basis = StringName("basis");
	String eweap_gunyaw = String("EWEAP_GUNYAW");
	String eweap_gunpitch = String("EWEAP_GUNPITCH");
};

const DispatchNames &names() {
	static DispatchNames n;
	return n;
}

// Per-row node capabilities, resolved once at plan rebuild (has_method is a
// per-frame string lookup otherwise).
enum RowCaps {
	CAP_BODY_CLIP = 1,
	CAP_BODY_SLOT = 2,
	CAP_BODY_PLAY = 4,
	CAP_AIM = 8,
	CAP_RHC = 16,
	CAP_CTRL = 32,
	CAP_MUZZLE = 64,
};

inline int32_t field_i(const float *p, int base, int field) {
	return static_cast<int32_t>(p[base + field]);
}

} // namespace

void NovaPresentApplier::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "sim", "index"),
			&NovaPresentApplier::setup);
	ClassDB::bind_method(D_METHOD("set_output_channels", "channels"),
			&NovaPresentApplier::set_output_channels);
	ClassDB::bind_method(D_METHOD("get_output_channels"),
			&NovaPresentApplier::get_output_channels);
	ClassDB::bind_method(
			D_METHOD("set_shared_visibility_maps", "occlusion_hidden_ids",
					"present_visibility"),
			&NovaPresentApplier::set_shared_visibility_maps);
	ClassDB::bind_method(
			D_METHOD("present_snapshot", "snap", "stride", "layout_revision"),
			&NovaPresentApplier::present_snapshot);
	ClassDB::bind_method(D_METHOD("get_stats"), &NovaPresentApplier::get_stats);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("bms_to_godot_basis", "rot_deg"),
			&NovaPresentApplier::bms_to_godot_basis);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("aim_root_basis", "snap", "base", "fallback"),
			&NovaPresentApplier::aim_root_basis);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("aim_apply", "node", "snap", "base", "drive_root_basis"),
			&NovaPresentApplier::aim_apply);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("aim_apply_valid", "node", "snap", "base", "drive_root_basis"),
			&NovaPresentApplier::aim_apply_valid);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("emplaced_apply", "node", "snap", "base", "clear_when_invalid"),
			&NovaPresentApplier::emplaced_apply);
	ClassDB::bind_static_method("NovaPresentApplier",
			D_METHOD("emplaced_clear", "node"), &NovaPresentApplier::emplaced_clear);
	BIND_ENUM_CONSTANT(OUTPUT_TRANSFORM);
	BIND_ENUM_CONSTANT(OUTPUT_PART_ANIM);
	BIND_ENUM_CONSTANT(OUTPUT_VISIBILITY);
	BIND_ENUM_CONSTANT(OUTPUT_BODY_ANIM);
	BIND_ENUM_CONSTANT(OUTPUT_ALL);
}

void NovaPresentApplier::setup(Object *sim, Object *index) {
	sim_id_ = sim != nullptr ? sim->get_instance_id() : ObjectID();
	index_id_ = index != nullptr ? index->get_instance_id() : ObjectID();
	index_has_generation_ =
			index != nullptr && index->has_method(names().get_generation);
	plan_revision_ = -1; // force a rebuild against the new wiring
	rows_.clear();
}

void NovaPresentApplier::set_output_channels(int channels) {
	output_channels_ = channels & OUTPUT_ALL;
}

void NovaPresentApplier::set_shared_visibility_maps(
		const Dictionary &occlusion_hidden_ids, const Dictionary &present_visibility) {
	occlusion_hidden_ids_ = occlusion_hidden_ids;
	present_visibility_ = present_visibility;
}

Dictionary NovaPresentApplier::get_stats() const {
	Dictionary d;
	d["moved"] = stat_moved_;
	d["posed"] = stat_posed_;
	d["hidden"] = stat_hidden_;
	d["muzzles"] = stat_muzzles_;
	d["plan_rebuilds"] = stat_plan_rebuilds_;
	d["transform_builds"] = stat_transform_builds_;
	d["aim_dispatches"] = stat_aim_dispatches_;
	d["rhc_dispatches"] = stat_rhc_dispatches_;
	d["part_dispatches"] = stat_part_dispatches_;
	d["control_dispatches"] = stat_control_dispatches_;
	d["body_dispatches"] = stat_body_dispatches_;
	d["muzzle_queries"] = stat_muzzle_queries_;
	return d;
}

Basis NovaPresentApplier::bms_to_godot_basis(const Vector3 &rot_deg) {
	// R_godot = RotY(90 - yaw) * RotZ(pitch) * RotX(roll) * RotY(90) — the one
	// placement convention (see mission_object_placer.gd's [orig] derivation);
	// the GDScript twin is pinned equivalent by the basis parity GUT case.
	const double pitch = Math::deg_to_rad(static_cast<double>(rot_deg.x));
	const double yaw = Math::deg_to_rad(static_cast<double>(rot_deg.y));
	const double roll = Math::deg_to_rad(static_cast<double>(rot_deg.z));
	return Basis(Vector3(0, 1, 0), Math::deg_to_rad(90.0) - yaw) *
			Basis(Vector3(0, 0, 1), pitch) * Basis(Vector3(1, 0, 0), roll) *
			Basis(Vector3(0, 1, 0), Math::deg_to_rad(90.0));
}

Basis NovaPresentApplier::aim_root_basis(const PackedFloat32Array &snap, int base,
		const Basis &fallback) {
	const float *p = snap.ptr();
	if (field_i(p, base, NovaSimulation::PF_AIM_OVERLAY_VALID) == 0) {
		return fallback;
	}
	return bms_to_godot_basis(
			Vector3(p[base + NovaSimulation::PF_AIM_BODY_PITCH_DEG],
					p[base + NovaSimulation::PF_AIM_BODY_YAW_DEG],
					p[base + NovaSimulation::PF_AIM_BODY_ROLL_DEG]));
}

void NovaPresentApplier::aim_apply(Object *node, const PackedFloat32Array &snap,
		int base, bool drive_root_basis) {
	if (node == nullptr) {
		return;
	}
	const float *p = snap.ptr();
	// The mounted-seat selector's final skeletal verdict applies to placed and
	// wire models alike [orig: BN17 special row @ 0x4b1290].
	if (node->has_method(names().set_right_hand_collapsed)) {
		node->call(names().set_right_hand_collapsed,
				field_i(p, base, NovaSimulation::PF_RIGHT_HAND_COLLAPSED) != 0);
	}
	if (!node->has_method(names().set_aim_overlay)) {
		return;
	}
	if (field_i(p, base, NovaSimulation::PF_AIM_OVERLAY_VALID) == 0) {
		node->call(names().set_aim_overlay, Array());
		return;
	}
	aim_apply_valid(node, snap, base, drive_root_basis);
}

void NovaPresentApplier::aim_apply_valid(Object *node,
		const PackedFloat32Array &snap, int base, bool drive_root_basis) {
	if (node == nullptr) {
		return;
	}
	Node3D *n3 = Object::cast_to<Node3D>(node);
	const Basis current_basis = n3 != nullptr
			? n3->get_basis()
			: static_cast<Basis>(node->get(names().basis));
	const Basis body_basis = aim_root_basis(snap, base, current_basis);
	if (drive_root_basis && current_basis != body_basis) {
		if (n3 != nullptr) {
			n3->set_basis(body_basis);
		} else {
			node->set(names().basis, body_basis);
		}
	}
	const Basis inverse_body = body_basis.inverse();
	const float *p = snap.ptr();
	Array deltas;
	deltas.resize(9);
	for (int overlay_class = 0; overlay_class < 9; ++overlay_class) {
		const int offset = base + NovaSimulation::PF_AIM_ANGLES +
				overlay_class * NovaSimulation::PF_AIM_CLASS_STRIDE;
		deltas[overlay_class] = inverse_body *
				bms_to_godot_basis(
						Vector3(p[offset], p[offset + 1], p[offset + 2]));
	}
	node->call(names().set_aim_overlay, deltas);
}

int NovaPresentApplier::emplaced_apply(Object *node,
		const PackedFloat32Array &snap, int base, bool clear_when_invalid) {
	if (node == nullptr || !node->has_method(names().set_ctrl_value)) {
		return 0;
	}
	const float *p = snap.ptr();
	if (field_i(p, base, NovaSimulation::PF_EMPLACED_CONTROLS_VALID) == 1) {
		node->call(names().set_ctrl_value, names().eweap_gunyaw,
				field_i(p, base, NovaSimulation::PF_EWEAP_GUNYAW));
		node->call(names().set_ctrl_value, names().eweap_gunpitch,
				field_i(p, base, NovaSimulation::PF_EWEAP_GUNPITCH));
		return 2;
	}
	// Nodes persist across dismount/death: remove only the two controls this
	// presenter owns — clear_ctrl_values() would also erase live WAC channels.
	if (clear_when_invalid) {
		emplaced_clear(node);
	}
	return 0;
}

void NovaPresentApplier::emplaced_clear(Object *node) {
	if (node == nullptr || !node->has_method(names().clear_ctrl_value)) {
		return;
	}
	node->call(names().clear_ctrl_value, names().eweap_gunyaw);
	node->call(names().clear_ctrl_value, names().eweap_gunpitch);
}

int64_t NovaPresentApplier::current_index_generation() {
	if (!index_has_generation_) {
		return 0;
	}
	Object *index = ObjectDB::get_instance(index_id_);
	if (index == nullptr) {
		return 0;
	}
	return static_cast<int64_t>(index->call(names().get_generation));
}

bool NovaPresentApplier::row_plan_is_current(const float *p, int64_t size,
		int stride, int64_t layout_revision) {
	// No revision means a compatible fake/custom source: preserve the original
	// full-resolution behavior rather than trusting an unverifiable row order.
	if (layout_revision < 0 || plan_revision_ != layout_revision ||
			plan_stride_ != stride || plan_snapshot_size_ != size ||
			plan_index_generation_ != current_index_generation()) {
		return false;
	}
	for (const Row &row : rows_) {
		if (row.base < 0 || row.base + stride > size) {
			return false;
		}
		if (ObjectDB::get_instance(row.node_id) == nullptr) {
			return false;
		}
		if (field_i(p, row.base, NovaSimulation::PF_WIRE_HANDLE) != row.handle ||
				field_i(p, row.base, NovaSimulation::PF_TYPE_ID) != row.type_id ||
				field_i(p, row.base, NovaSimulation::PF_BMS_ID) != row.bms_id ||
				field_i(p, row.base, NovaSimulation::PF_KIND) != row.kind ||
				field_i(p, row.base, NovaSimulation::PF_INDEX) != row.index) {
			return false;
		}
	}
	return true;
}

void NovaPresentApplier::rebuild_row_plan(const float *p, int64_t size, int stride,
		int64_t layout_revision) {
	++stat_plan_rebuilds_;
	rows_.clear();
	present_visibility_.clear();
	plan_revision_ = layout_revision;
	plan_stride_ = stride;
	plan_snapshot_size_ = size;
	plan_index_generation_ = current_index_generation();
	Object *index = ObjectDB::get_instance(index_id_);
	if (index == nullptr) {
		return;
	}
	const int64_t count = size / stride;
	rows_.reserve(static_cast<size_t>(count));
	for (int64_t r = 0; r < count; ++r) {
		const int base = static_cast<int>(r * stride);
		const int32_t bms_id = field_i(p, base, NovaSimulation::PF_BMS_ID);
		const int32_t kind = field_i(p, base, NovaSimulation::PF_KIND);
		const int32_t idx = field_i(p, base, NovaSimulation::PF_INDEX);
		Object *node = index->call(names().resolve, bms_id, kind, idx);
		if (node == nullptr) {
			continue;
		}
		Row row;
		row.base = base;
		row.node_id = node->get_instance_id();
		row.handle = field_i(p, base, NovaSimulation::PF_WIRE_HANDLE);
		row.type_id = field_i(p, base, NovaSimulation::PF_TYPE_ID);
		row.bms_id = bms_id;
		row.kind = kind;
		row.index = idx;
		// Capability lookups are stable per node script: resolve them once per
		// topology change so the per-frame loop never pays has_method() again.
		int caps = 0;
		if (node->has_method(names().play_body_clip_at)) {
			caps |= CAP_BODY_CLIP;
		}
		if (node->has_method(names().play_body_anim_at)) {
			caps |= CAP_BODY_SLOT;
		}
		if (node->has_method(names().play_body_anim)) {
			caps |= CAP_BODY_PLAY;
		}
		if (node->has_method(names().set_aim_overlay)) {
			caps |= CAP_AIM;
		}
		if (node->has_method(names().set_right_hand_collapsed)) {
			caps |= CAP_RHC;
		}
		if (node->has_method(names().set_ctrl_value) &&
				node->has_method(names().clear_ctrl_value)) {
			caps |= CAP_CTRL;
		}
		if (node->has_method(names().has_muzzle) &&
				node->has_method(names().get_muzzle_world_position) &&
				static_cast<bool>(node->call(names().has_muzzle))) {
			caps |= CAP_MUZZLE;
		}
		row.caps = caps;
		rows_.push_back(row);
	}
}

const String &NovaPresentApplier::infantry_key(int state) {
	auto it = infantry_keys_.find(state);
	if (it == infantry_keys_.end()) {
		it = infantry_keys_.emplace(state, NovaSimulation::infantry_anim_key(state))
					 .first;
	}
	return it->second;
}

void NovaPresentApplier::present_snapshot(const PackedFloat32Array &snap,
		int stride, int64_t layout_revision) {
	if (stride <= 0 || index_id_ == ObjectID()) {
		return;
	}
	const float *p = snap.ptr();
	const int64_t size = snap.size();
	if (!row_plan_is_current(p, size, stride, layout_revision)) {
		rebuild_row_plan(p, size, stride, layout_revision);
	}
	Object *sim = ObjectDB::get_instance(sim_id_);
	NovaSimulation *native_sim = Object::cast_to<NovaSimulation>(sim);
	const DispatchNames &n = names();
	for (Row &row : rows_) {
		Object *node = ObjectDB::get_instance(row.node_id);
		if (node == nullptr) {
			// A freed cached node cannot be written through; the next call's
			// validation rejects the plan and rebuilds.
			continue;
		}
		Node3D *n3 = Object::cast_to<Node3D>(node);
		const int base = row.base;
		const int caps = row.caps;
		if ((output_channels_ & OUTPUT_TRANSFORM) != 0 && n3 != nullptr) {
			// Position is already Godot-space (x, z, -y); rotation is
			// mission-space degrees, built through the ONE placement convention
			// so a sim-driven entity sits exactly where placement would put it.
			const Vector3 pos(p[base + NovaSimulation::PF_POS_X],
					p[base + NovaSimulation::PF_POS_Y],
					p[base + NovaSimulation::PF_POS_Z]);
			const Basis entity_basis = bms_to_godot_basis(
					Vector3(p[base + NovaSimulation::PF_PITCH_DEG],
							p[base + NovaSimulation::PF_YAW_DEG],
							p[base + NovaSimulation::PF_ROLL_DEG]));
			const Basis root_basis = (caps & CAP_AIM) != 0
					? aim_root_basis(snap, base, entity_basis)
					: entity_basis;
			const Transform3D next(root_basis, pos);
			++stat_transform_builds_;
			// The pass owns these transforms: compare against the last APPLIED
			// value instead of reading the node property back per row (the aim
			// leg below runs with drive_root_basis=false, so nothing else
			// rewrites the root between frames). On a cache miss (fresh plan —
			// including the every-call rebuild of revisionless fake sources)
			// fall back to one live read so an unchanged row never re-dirties
			// the node's tree, exactly like the GDScript live compare did.
			if (!row.has_last_transform && n3->get_transform() == next) {
				row.has_last_transform = true;
				row.last_transform = next;
			} else if (!row.has_last_transform || row.last_transform != next) {
				n3->set_transform(next);
				row.has_last_transform = true;
				row.last_transform = next;
				++stat_moved_;
			}
		}
		// Aim overlay with the capability lookups hoisted into the row plan and
		// the no-overlay clear gated to the valid->invalid edge (the node-side
		// setters no-op on repeats; these gates skip the dispatch itself).
		if ((caps & CAP_RHC) != 0) {
			const int32_t rhc =
					field_i(p, base, NovaSimulation::PF_RIGHT_HAND_COLLAPSED);
			if (rhc != row.rhc) {
				node->call(n.set_right_hand_collapsed, rhc != 0);
				++stat_rhc_dispatches_;
				row.rhc = rhc;
			}
		}
		if ((caps & CAP_AIM) != 0) {
			const int32_t aim_valid =
					field_i(p, base, NovaSimulation::PF_AIM_OVERLAY_VALID);
			if (aim_valid != 0) {
				aim_apply_valid(node, snap, base, false);
				++stat_aim_dispatches_;
			} else if (row.aim_valid != 0) {
				node->call(n.set_aim_overlay, Array());
				++stat_aim_dispatches_;
			}
			row.aim_valid = aim_valid;
		}
		if ((output_channels_ & OUTPUT_PART_ANIM) != 0) {
			if ((caps & CAP_CTRL) != 0) {
				// Remove last tick's semantic mount ownership before generic
				// model-order channels run: a generic PLAYPARTANIM can itself
				// address EWEAP_*; it must survive dismount, while live gunner
				// aim still overlays it last.
				emplaced_clear(node);
				stat_control_dispatches_ += 2;
			}
			// PANM: the engine integrates each channel's phase
			// [orig: Entity_ApplyCommand @ 0x43ab60 case 0x22]; the host only
			// poses commanded channels.
			if (node->has_method(n.set_part_phase)) {
				if (field_i(p, base, NovaSimulation::PF_ACTIVE1) == 1) {
					node->call(n.set_part_phase, 1,
							field_i(p, base, NovaSimulation::PF_PHASE1));
					++stat_posed_;
					++stat_part_dispatches_;
				}
				if (field_i(p, base, NovaSimulation::PF_ACTIVE2) == 1) {
					node->call(n.set_part_phase, 2,
							field_i(p, base, NovaSimulation::PF_PHASE2));
					++stat_posed_;
					++stat_part_dispatches_;
				}
			}
			if ((caps & CAP_CTRL) != 0) {
				const int applied = emplaced_apply(node, snap, base, false);
				stat_posed_ += applied;
				stat_control_dispatches_ += applied;
			}
		}
		const bool present_visible =
				field_i(p, base, NovaSimulation::PF_HIDDEN) == 0 &&
				field_i(p, base, NovaSimulation::PF_LOCAL_VIEW_SUPPRESSED) == 0;
		const int32_t present_visible_int = present_visible ? 1 : 0;
		if (present_visible_int != row.present_visible) {
			present_visibility_[row.bms_id] = present_visible;
			row.present_visible = present_visible_int;
		}
		if ((output_channels_ & OUTPUT_VISIBILITY) != 0 && n3 != nullptr) {
			// Death is not disappearance (corpses and husks keep rendering until
			// the sim despawns via PF_HIDDEN); the local first-person UseGun
			// parent's own world model is presentation-suppressed. Semantics and
			// witnesses recorded at the GDScript origin (mission_present_pass.gd)
			// [orig: Entity_RenderVehicleModel @ 0x4407d0 cull/submit;
			// Flags&4 husk pick @ 0x413086].
			if (n3->is_visible() != present_visible) {
				// Two-bit visibility ownership: while the render-occlusion frame
				// claims this node (the shared hidden set), a sim-wants-visible
				// node stays hidden — occlusion releases through the same set.
				if (!(present_visible &&
							occlusion_hidden_ids_.has(row.bms_id))) {
					n3->set_visible(present_visible);
				}
			}
			if (!present_visible) {
				++stat_hidden_;
			}
		}
		const int32_t net_id = field_i(p, base, NovaSimulation::PF_NET_ID);
		// Hidden models skip skeletal writes unless they own the authoritative
		// posed-muzzle feedback seam (AI fire origins survive; hidden non-weapon
		// actors take the cheap path).
		if ((output_channels_ & OUTPUT_BODY_ANIM) != 0 &&
				(present_visible || (net_id > 0 && (caps & CAP_MUZZLE) != 0))) {
			// Main-body skeletal clip: infantry poses to the exact anim-state
			// phase that produced root motion; PF_BODY_ANIM_SLOT is the coarse
			// fallback for compatible non-infantry nodes.
			const int32_t anim_state =
					field_i(p, base, NovaSimulation::PF_ANIM_STATE);
			bool dispatched = false;
			if (anim_state >= 0 && (caps & CAP_BODY_CLIP) != 0) {
				const String &key = infantry_key(anim_state);
				if (!key.is_empty()) {
					node->call(n.play_body_clip_at, key,
							field_i(p, base, NovaSimulation::PF_ANIM_PHASE_TICKS));
					++stat_body_dispatches_;
					dispatched = true;
				}
			}
			if (!dispatched) {
				const int32_t body_anim_slot =
						field_i(p, base, NovaSimulation::PF_BODY_ANIM_SLOT);
				if (body_anim_slot >= 0) {
					if ((caps & CAP_BODY_SLOT) != 0) {
						node->call(n.play_body_anim_at, body_anim_slot,
								field_i(p, base,
										NovaSimulation::PF_ANIM_PHASE_TICKS));
						++stat_body_dispatches_;
					} else if ((caps & CAP_BODY_PLAY) != 0) {
						node->call(n.play_body_anim, body_anim_slot);
						++stat_body_dispatches_;
					}
				}
			}
		}
		if (net_id > 0 && (caps & CAP_MUZZLE) != 0) {
			// The D-AI-6 muzzle seam: feed the posed gun-flash userpoint back to
			// the sim so AI rounds leave the GUN (one-frame staleness, ledgered)
			// [orig: Entity_GetAttachmentWorldPosition @ 0x4b2670 — our sim has
			// no skeletal pose, so the present layer pushes it back].
			++stat_muzzle_queries_;
			if (static_cast<bool>(node->call(n.has_muzzle))) {
				const Vector3 muzzle = node->call(n.get_muzzle_world_position);
				if (native_sim != nullptr) {
					native_sim->set_ai_muzzle_world(net_id, muzzle);
					++stat_muzzles_;
				} else if (sim != nullptr &&
						sim->has_method(n.set_ai_muzzle_world)) {
					sim->call(n.set_ai_muzzle_world, net_id, muzzle);
					++stat_muzzles_;
				}
			}
		}
	}
}
