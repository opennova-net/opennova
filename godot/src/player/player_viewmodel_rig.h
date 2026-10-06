#pragma once

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include "object/object_model.h"
#include "simulation/player_local_view.h"
#include "simulation/player_weapon_view.h"

namespace godot {

class LocalPlayerPresenter;
class Simulation;

// The local player's first-person viewmodel presentation (the former
// player_viewmodel_rig.gd, ADR 0043 slice G8), owned by LocalPlayerPresenter:
// the FP projection feed (the renderfov focal ratio + depth band the object
// shaders apply to flagged instances inside the beauty pass), the viewmodel
// node + parts lifetime, the weapon.def placement (pos/tpos ADS lerp,
// rotation bias, axis map), and the first-person control-register writers.
// The presenter keeps the player camera, the avatar/held-weapon presentation,
// and the third-person flag; per-frame inputs (the view snapshot, the weapon
// view, the camera mode, the debug force flag) arrive as update_viewmodel
// arguments. Registered and reachable through the presenter's viewmodel_rig()
// because the vm_bone_dump probe sweeps the five placement tunables live.
//
// First-person weapon viewmodel placement, witnessed from weapon.def `pos`
// (hip) / `tpos` (ADS). The original adds the equipped weapon's view-bias
// offset to the eye in view-local space, rotated by the view orientation,
// then draws the gun (gfx1) + character arms at that view root [orig:
// Player_UpdateFirstPersonCamera @0x4dd380 -> g_ViewEulerTranslationOut;
// Player_RenderFirstPersonViewModel @0x4ded60]. The weapon.def parser stores
// the pos/tpos POSITION as `atof(str) * 256.0` (a 16.16 fixed-point world
// coord; scale flt_7D1D70 @0x544770) and the ROTATION as degrees -> 32-bit
// BAM (`* 0x0B60B60` = 2^32/360) [orig: weapon.def 'tpos' handler
// @0x54471f]. The camera ftol's the stored float and adds it straight onto
// g_view_pos (16.16), so the net WORLD offset is simply `file_value / 256`
// -- see place_viewmodel_at_camera for the axis map and derivation. The Sighted/ADS
// path eases `pos` -> `tpos` (the scope interp runs from the hip copy at
// WeaponDef+0x10C to the tpos at +0x124 and publishes the difference as the
// view bias [orig: Player_StepFpViewBiasInterp @0x4ddf53..0x4ddfc3]),
// rotation carried by the sim's authored pose interpolator; position still
// uses its scalar fraction (D-WPN-39). (The `Flags & 2` leg of the
// camera is the DEAD/round-end camera, not ADS.) The view fields flow from
// the mounted root's weapon.def (apply_viewmodel_def <-
// LocalPlayerVisuals.local_player_viewmodel_def, the equipped weapon's row);
// the initial values are the witnessed JOX WPN_AK47AUTO line, which nothing
// draws with until an equipped def replaces them. (The pre-def
// constant (10, 0, -201) turned out to be the REVX-era WPN_AK47AUTO `pos` --
// that SKU's def drives the AKM_1st viewmodel.) The witnessed scale +
// fallback placement values live at engine renderer/fp_viewmodel_spec.h,
// re-exported through Simulation statics. Tunable (properties, not
// constants) so debug drivers can sweep placements live; the values are the
// witnessed WPN_AK47AUTO def line + the current best facing.
class PlayerViewmodelRig : public RefCounted {
	GDCLASS(PlayerViewmodelRig, RefCounted)

public:
	PlayerViewmodelRig();

	// --- the five placement tunables (bound properties, the probe's sweep) ---
	// PLAYER_VIEWMODEL_POS_UNITS: the weapon.def `pos` view offset, raw units.
	Vector3 get_player_viewmodel_pos_units() const { return pos_units_; }
	void set_player_viewmodel_pos_units(const Vector3 &p_value) { pos_units_ = p_value; }
	// PLAYER_VIEWMODEL_TPOS_UNITS: the ADS/sighted view offset (weapon.def
	// `tpos` -> WeaponDef.CamOffsetTpos @0x124), blended in by the sim's scope
	// fraction; JOX AK47AUTO = (-62.33, 29.19, -152.56).
	Vector3 get_player_viewmodel_tpos_units() const { return tpos_units_; }
	void set_player_viewmodel_tpos_units(const Vector3 &p_value) { tpos_units_ = p_value; }
	// PLAYER_VIEWMODEL_ROT: the FP rig's model->camera AXIS MAP, euler DEGREES
	// in CAMERA space. The FP rig is a T-posed character skeleton (BN01 Pelvis
	// at the origin) that the wpn clips POSE into the hold facing downrange;
	// the rig renders through the standard skeletal pipeline (import-flipped
	// meshes + the bind-rotation rests, positions reconstructed from the model
	// table), and the yaw-180 turns the rig's authored forward onto Godot's -Z
	// camera forward -- the structural equivalent of the original drawing its
	// composed render-frame bone matrices with the raw view matrix [orig:
	// Player_RenderFirstPersonViewModel @0x4ded60 root = view transform; the
	// S*A^T*S copy loops @0x40c4d8..0x40c57c realize the model->render map
	// inside the composition]. Sign pinned against retail: the stock/grip
	// anchor bottom-RIGHT at the hip idle (the pre-train build renders
	// identically and was retail-confirmed).
	Vector3 get_player_viewmodel_rot() const { return rot_; }
	void set_player_viewmodel_rot(const Vector3 &p_value) { rot_ = p_value; }
	// PLAYER_VIEWMODEL_ROT_BIAS_DEF: the witnessed per-weapon view-rotation
	// bias: weapon.def `pos` rotation columns, DEGREES (yaw, pitch, roll)
	// ADDED to the view angles -- the weapon cant. AK47AUTO = 5.0 / 3.75 /
	// 353.0. [orig: Player_UpdateFirstPersonCamera @0x4dd444: rot = view_rot +
	// Def.Bone.rot; parser stores degrees -> BAM @0x54471f.] Sign map to Godot
	// camera axes verified visually.
	Vector3 get_player_viewmodel_rot_bias_def() const { return rot_bias_def_; }
	void set_player_viewmodel_rot_bias_def(const Vector3 &p_value) { rot_bias_def_ = p_value; }
	// PLAYER_VIEWMODEL_RENDERFOV_H_DEG: the FP render pass -- the original
	// draws the viewmodel through its OWN projection: the weapon's `renderfov`
	// (HORIZONTAL degrees; every JO weapon.def omits the key, so all use the
	// record default 80.0) converted to vertical via the aspect, with the near
	// plane swapped 0.2 -> 0.05 and the viewport depth range remapped so the
	// world never overdraws it, then its own flush [orig:
	// Player_RenderFirstPersonViewModel @0x4ded60:
	// Render_SwapProjectionNearZ(0.05) @0x4dee29 / restore 0.2 @0x4df0aa, fov =
	// WeaponDef+0x148 @0x4dee71 -> h->v conversion in
	// Render_SetViewAndProjectionMatrices @0x58d900, depth remap
	// Render_SetViewportDepth01 @0x58a7b0; default 80.0 = flt_7D1898 stored by
	// AdmDef_InitEntryDefaults @0x53ff31; parser key 'renderfov' @0x54482a].
	// Ported as a fold into the beauty pass: the viewmodel parts ride their own
	// visual layer (the beauty camera and the focused Q3 adapter admit it, the
	// rigid glow strips copying under the world projection; the mirror/capture
	// cameras exclude it) and every flagged instance applies the renderfov focal ratio plus
	// the near depth band in the object shaders
	// (shaders/viewmodel_pass.gdshaderinc, fed by update_viewmodel_projection)
	// -- the depth-remap's visible equivalent, drawn into the same frame as the
	// world. (near-z lives engine-side: Simulation.viewmodel_pass_near_z())
	float get_player_viewmodel_renderfov_h_deg() const { return renderfov_h_deg_; }
	void set_player_viewmodel_renderfov_h_deg(float p_value) { renderfov_h_deg_ = p_value; }

	// The showhud bit-0 FP-gun gate (GameHudPresenter cycles the flags and
	// pushes the bit through LocalPlayerPresenter.set_fp_gun_visible). Default
	// on = the boot flags value 3. [orig: g_FpWeaponViewFlags bit 0, tested by
	// Player_RenderFirstPersonViewModel @0x4DEDEA before the FP submit]
	void set_fp_gun_visible(bool p_visible);

	// The world serves the viewmodel builder + def; the presenter serves the
	// weapon effects (play-serial resync on rebuild).
	void setup(Node *p_world, LocalPlayerPresenter *p_presenter, Camera3D *p_camera);
	void teardown();

	// W4-2-style justified accessors: PlayerWeaponEffects resolves action
	// userpoints against the FP viewmodel parts, and the presenter stamps
	// lighting context onto them -- both read through the presenter's
	// vm_parts()/viewmodel() delegates, which land here (the rig OWNS the
	// parts and the node). The parts array keeps its entries until a rebuild
	// re-collects them; the read hands back the LIVE ones.
	TypedArray<ObjectModel> vm_parts() const;
	Node3D *viewmodel() const;
	// The gameplay camera the rig was set up with (probes read the FP
	// placement relative to it).
	Camera3D *camera() const;

	// A world-only render capture hides the gun for its duration. Applied at
	// once: the fixture flow freezes the shell before it captures, so no
	// per-frame update runs between the latch and the readback.
	void set_capture_hidden(bool p_hidden);
	bool is_capture_hidden() const { return capture_hidden_; }

	// The FP projection feed last pushed to `opennova_viewmodel_projection`
	// (x = renderfov focal ratio, y = near, z = far).
	Vector4 projection_feed() const { return projection_feed_; }

	// The viewmodel branch of the presenter's model lifetime: (re)build the FP
	// model when missing, apply its weapon.def view record, and re-collect the
	// parts.
	void ensure_viewmodel();
	// Drop the built FP viewmodel so the next update pass rebuilds gun/arms/FSM
	// from the (changed) equipped weapon -- the armory ACCEPT re-mount [orig:
	// WeaponLoadout_ApplyFromBuffer @0x565cd0 tail -> Player_MountWeaponSlot
	// @0x4dfa40]. Retires the old draw and parts immediately; the next
	// presentation pass rebuilds and poses the replacement before rendering.
	void refresh_viewmodel();
	// How many times the viewmodel was dropped for a rebuild (a read seam: the
	// armory pins count the equip/unequip refreshes).
	int viewmodel_generation() const { return generation_; }
	// The viewmodel leg of the presenter's clear-models path.
	void clear_viewmodel();
	// First-person weapon viewmodel: sit it in front of the eye, tracking the
	// camera 1:1, shown in first person only (in 3P the body avatar shows
	// instead). The original biases the CAMERA by the weapon's `pos`/`tpos`
	// view offset and draws the model at the view root [orig:
	// Player_UpdateFirstPersonCamera @0x4dd380]; placing it in camera space is
	// the faithful structural equivalent (camera.global_transform == the engine
	// view transform here). Per-frame state arrives as arguments from the
	// presenter's camera pass: the sim view snapshot, this tick's weapon view,
	// the camera mode, the debug force flag.
	void update_viewmodel(const Ref<PlayerLocalView> &p_view, const Ref<PlayerWeaponView> &p_weapon_view,
			bool p_third_person, bool p_force_visible);
	// Re-place the gun and re-push its projection feed against the camera's
	// CURRENT pose without advancing any sim state: a frozen-shell fixture
	// moves the beauty camera after the per-frame pass stopped, and the fold
	// draws the gun where the root last sat.
	void restamp_at_camera();

protected:
	static void _bind_methods();

private:
	Node *world() const;
	LocalPlayerPresenter *presenter() const;
	Ref<Simulation> sim() const;
	void update_viewmodel_projection();
	void place_viewmodel_at_camera();
	void apply_viewmodel_control_registers(bool p_submit_viewmodel,
			const Ref<PlayerWeaponView> &p_weapon_view);
	void apply_viewmodel_def();

	Vector3 pos_units_;
	Vector3 tpos_units_;
	Vector3 rot_;
	Vector3 rot_bias_def_;
	float renderfov_h_deg_ = 80.0f;
	// The world pass's horizontal fov of the last view snapshot (the policy
	// fov the feed's focal ratio is taken against; the base 80 until one lands).
	float world_fov_h_deg_ = 80.0f;

	ObjectID world_id_;
	ObjectID presenter_id_;
	ObjectID camera_id_;
	ObjectID viewmodel_id_;
	int generation_ = 0; // refresh_viewmodel count
	Vector<ObjectID> vm_parts_; // the builder's typed viewmodel models
	// A world-only render capture hides the gun for its duration (the shell's
	// capture session latches this; the per-frame submission gate ANDs it in).
	bool capture_hidden_ = false;
	// The last per-frame submission verdict, so the capture latch can re-apply
	// visibility without a frame.
	bool submit_viewmodel_ = false;
	// The last FP projection feed pushed to the shader global (k, near, far).
	Vector4 projection_feed_;
	bool fp_gun_visible_ = true;
};

} // namespace godot
