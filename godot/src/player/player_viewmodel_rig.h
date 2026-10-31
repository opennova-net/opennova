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

// The local player's first-person viewmodel device (the former
// player_viewmodel_rig.gd, ADR 0043 slice G8), owned by LocalPlayerPresenter:
// the viewmodel node and parts lifetime, the root's stamp at the camera, the
// FP projection shader global and the first-person CTRL register writes. What
// it draws is the engine's: the root (renderer::fp_viewmodel_root over the
// sim's blended view bias), the projection (world::viewmodel_projection_feed),
// the submission gates (world::fp_viewmodel_retail_submit) and the writers
// (world::fp_ctrl_register_writes), each carrying its witnesses. The
// presenter keeps the player camera, the avatar and the third-person flag;
// per-frame inputs arrive as update_viewmodel arguments. Registered and
// reachable through the presenter's viewmodel_rig() because the vm_bone_dump
// probe sweeps the five placement tunables live: they start at the engine's
// witnessed WPN_AK47AUTO line (renderer::kFallbackPosUnits and siblings) until
// an equipped def's rows replace them.
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
	// in CAMERA space: the yaw-180 of renderer::kViewmodelRigYawDeg, which
	// carries the witness. Sign pinned against retail: the stock/grip anchor
	// bottom-RIGHT at the hip idle.
	Vector3 get_player_viewmodel_rot() const { return rot_; }
	void set_player_viewmodel_rot(const Vector3 &p_value) { rot_ = p_value; }
	// PLAYER_VIEWMODEL_ROT_BIAS_DEF: the witnessed per-weapon view-rotation
	// bias: weapon.def `pos` rotation columns, DEGREES (yaw, pitch, roll)
	// ADDED to the view angles -- the weapon cant. AK47AUTO = 5.0 / 3.75 /
	// 353.0. (engine: runtime/renderer/fp_viewmodel_spec.h) Sign map to Godot
	// camera axes verified visually.
	Vector3 get_player_viewmodel_rot_bias_def() const { return rot_bias_def_; }
	void set_player_viewmodel_rot_bias_def(const Vector3 &p_value) { rot_bias_def_ = p_value; }
	// PLAYER_VIEWMODEL_RENDERFOV_H_DEG: the weapon `renderfov`, HORIZONTAL
	// degrees, the FP pass's own projection (world::viewmodel_projection_feed
	// carries the witness). Ported as a fold into the beauty pass: the parts
	// ride their own visual layer (the beauty camera and the focused Q3 adapter
	// admit it; the mirror/capture cameras exclude it) and every flagged
	// instance applies the feed in the object shaders
	// (shaders/viewmodel_pass.gdshaderinc, fed by update_viewmodel_projection).
	float get_player_viewmodel_renderfov_h_deg() const { return renderfov_h_deg_; }
	void set_player_viewmodel_renderfov_h_deg(float p_value) { renderfov_h_deg_ = p_value; }

	// The CTRL override owners the first-person writers store under
	// (ObjectModel::set_ctrl_override): one per writer, so a writer that does
	// not execute clears only its own registers.
	static constexpr const char *kCtrlOwnerFpHeat = "first_person:heat";
	static constexpr const char *kCtrlOwnerFpEmplaced = "first_person:emplaced";
	static constexpr const char *kCtrlOwnerFpTeam = "first_person:team";
	static constexpr const char *kCtrlOwnerFpArmsCamo = "first_person:arms_camo";
	// The TEX_TEAM writer on one first-person part, inside the part's
	// begin/end_ctrl_update: `p_team_byte` (renderer::viewmodel_team_byte)
	// when the writer executes (`p_write`: world::fp_ctrl_register_writes'
	// team, which carries the witness), else its register cleared. The rig's
	// per-frame writers run it on every part, and the editor's first-person
	// previews on theirs.
	static void write_fp_team(ObjectModel &p_part, bool p_write, int p_team_byte);

	// The showhud bit-0 FP-gun gate (GameHudPresenter cycles the flags and
	// pushes the bit through LocalPlayerPresenter.set_fp_gun_visible). Default
	// on = the boot flags value 3. (engine: runtime/hud/hud_config_tokens.h)
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
	// The live parts one by one, for the per-frame native readers (no array built).
	template <typename F>
	void for_each_vm_part(F &&p_fn) const {
		for (const ObjectID &id : vm_parts_)
			if (ObjectModel *part = Object::cast_to<ObjectModel>(ObjectDB::get_instance(id)))
				p_fn(part);
	}
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
	// from the (changed) equipped weapon (the armory ACCEPT re-mount, a committed
	// switch). Retires the old draw and parts immediately; the next
	// presentation pass rebuilds and poses the replacement before rendering.
	void refresh_viewmodel();
	// How many times the viewmodel was dropped for a rebuild (a read seam: the
	// armory pins count the equip/unequip refreshes).
	int viewmodel_generation() const { return generation_; }
	// The viewmodel leg of the presenter's clear-models path.
	void clear_viewmodel();
	// First-person weapon viewmodel: stamp the engine's root against the camera
	// (camera.global_transform == the engine view transform here), shown in
	// first person only (in 3P the body avatar shows instead). Per-frame state
	// arrives as arguments from the presenter's camera pass: the sim view
	// snapshot, this tick's weapon view, the camera mode, the debug force flag.
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
