#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include "object/weapon_def.h"

namespace godot {

// The weapon.def slice the HUD's weapon-coupled elements read (the former
// player_hud_weapon_def.gd, ADR 0043 slice G10), decoded from the WeaponDef
// record `WeaponDatabase.get_weapon()` returns. Mirrors the original's
// per-frame HUD info struct holding the equipped weapon-def pointer
// [orig: HUD_BuildEntityInfo @0x4b8561 -> hudInfo+552].
//
// `error_deg` is the 6-row dispersion table in DEGREES -- rows hip
// prone/crouch/stand then scoped prone/crouch/stand; the crosshair spread
// reads row stance + 3*scoped [orig: parse @0x543b21 (16.16); HUD_DrawCrosshair
// @0x592b84]. The clip graphic (HUDCLIPGFX [orig: parse @0x54427f]) and the
// per-round row (HUDRNDGFX [orig: parse @0x5442fc -> weapon +644..+656,
// divisor byte +727]) drive the clip indicator.
class PlayerHudWeaponDef : public RefCounted {
	GDCLASS(PlayerHudWeaponDef, RefCounted)

public:
	// Decode one WeaponDef; null for no weapon (the HUD then draws no weapon
	// cluster).
	static Ref<PlayerHudWeaponDef> from_weapon_def(const Ref<WeaponDef> &p_def);
	// The HUDRNDGFX divisor as the original reads it: stored as a byte
	// (weapon+727), so an out-of-range file value wraps mod 256.
	// [orig: HUDRNDGFX parse @0x5442fc; read @0x599bb1]
	static int rounds_per_icon_from_layout(const Vector3i &p_layout);

	// The dispersion row in degrees; 0.0 outside the parsed table.
	float error_row_deg(int p_row) const;

	String get_weapon_name() const { return weapon_name_; }
	void set_weapon_name(const String &p_value) { weapon_name_ = p_value; }
	// the fired round's ammo.def name (the clip-flash key is the engine
	// weapon view's ammo_bucket / ammo_class_id pair, D-HUD-5)
	String get_round_type() const { return round_type_; }
	void set_round_type(const String &p_value) { round_type_ = p_value; }
	// magazine capacity; -1 = infinite (clip reads -1)
	int get_clipsize() const { return clipsize_; }
	void set_clipsize(int p_value) { clipsize_ = p_value; }
	PackedFloat32Array get_error_deg() const { return error_deg_; }
	void set_error_deg(const PackedFloat32Array &p_value) { error_deg_ = p_value; }
	String get_clipgfx_texture() const { return clipgfx_texture_; }
	void set_clipgfx_texture(const String &p_value) { clipgfx_texture_ = p_value; }
	Vector2i get_clipgfx_offset() const { return clipgfx_offset_; }
	void set_clipgfx_offset(const Vector2i &p_value) { clipgfx_offset_ = p_value; }
	String get_rndgfx_texture() const { return rndgfx_texture_; }
	void set_rndgfx_texture(const String &p_value) { rndgfx_texture_ = p_value; }
	// first round icon, relative to the HUDCLIP anchor
	Vector2i get_rndgfx_offset() const { return rndgfx_offset_; }
	void set_rndgfx_offset(const Vector2i &p_value) { rndgfx_offset_ = p_value; }
	// per-round icon step
	Vector2i get_rndgfx_step() const { return rndgfx_step_; }
	void set_rndgfx_step(const Vector2i &p_value) { rndgfx_step_ = p_value; }
	// >1 draws one icon per N rounds, rounded up
	int get_rounds_per_icon() const { return rounds_per_icon_; }
	void set_rounds_per_icon(int p_value) { rounds_per_icon_ = p_value; }
	// Standard SIGHTS-card contents. The simulation owns the dynamic card
	// selector; this record only preserves every authored row (WeaponSightRow)
	// in draw order and virtual 1024x768 space.
	// [orig: HUD_DrawWeaponSightOverlays @0x4dce00]
	TypedArray<WeaponSightRow> get_sights() const { return sights_; }
	void set_sights(const TypedArray<WeaponSightRow> &p_value) { sights_ = p_value; }
	// The card's `slide` multiplier at the def's default zero
	// (WeaponDef::get_sight_slide_multiplier; the engine evaluator carries
	// the witness). 0 for a def without a scope-zero default.
	int get_sight_slide_multiplier() const { return sight_slide_multiplier_; }
	void set_sight_slide_multiplier(int p_value) { sight_slide_multiplier_ = p_value; }
	// The two card selectors' weapon.def halves, resolved by the engine
	// (runtime/hud/scope_circle_mask.h, which carries the witnesses):
	// `scoped_selector` = Scoped without Inset -- the arm that draws the
	// scoped-view circle mask after the card; `sighted_selector` = the Sighted
	// bit, which pre-empts it. The drawer's third Sighted term (the MountSlot
	// action is not SWITCHFROM) is not a def field and is not modelled here;
	// it only matters for a def that sets BOTH bits, and none ships.
	bool get_scoped_selector() const { return scoped_selector_; }
	void set_scoped_selector(bool p_value) { scoped_selector_ = p_value; }
	bool get_sighted_selector() const { return sighted_selector_; }
	void set_sighted_selector(bool p_value) { sighted_selector_ = p_value; }

protected:
	static void _bind_methods();

private:
	String weapon_name_;
	String round_type_;
	int clipsize_ = 0;
	PackedFloat32Array error_deg_;
	String clipgfx_texture_;
	Vector2i clipgfx_offset_;
	String rndgfx_texture_;
	Vector2i rndgfx_offset_;
	Vector2i rndgfx_step_;
	int rounds_per_icon_ = 0;
	TypedArray<WeaponSightRow> sights_;
	int sight_slide_multiplier_ = 0;
	bool scoped_selector_ = false;
	bool sighted_selector_ = false;
};

} // namespace godot
