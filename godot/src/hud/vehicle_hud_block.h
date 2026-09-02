#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <formats/def/def.h>

namespace godot {

// One hudpos.def VEHICLE_HUD block (formats/def/def.h DefVehicleHudBlock), the
// typed handoff from HudPos::get_vehicle_hud to HudOverlay::set_vehicle_panel.
// The record retains the def block itself, so the overlay feeds the engine
// the parsed rows with no re-parse; the setters exist so a test authors a
// variant (they write the block's fixed-width fields with the block's caps).
class VehicleHudBlock : public RefCounted {
	GDCLASS(VehicleHudBlock, RefCounted)

public:
	void assign(const DefVehicleHudBlock &p_block) { block_ = p_block; }
	const DefVehicleHudBlock &native() const { return block_; }

	String get_sid() const;
	void set_sid(const String &p_value);
	String get_icon() const;
	void set_icon(const String &p_value);
	String get_interface_texture() const;
	void set_interface_texture(const String &p_value);
	String get_static_texture() const;
	void set_static_texture(const String &p_value);
	Vector2i get_driver() const;
	void set_driver(const Vector2i &p_value);
	int get_emplace_count() const { return block_.emplace_count; }
	Vector2i get_emplace_point(int p_index) const;
	// Appends one authored pair; retail caps the list at 4 (a fifth is dropped).
	void add_emplace_point(const Vector2i &p_point);
	int get_seat_count() const { return block_.seat_count; }
	Vector2i get_seat_point(int p_index) const;
	// Appends one authored pair; retail caps the list at 8 (a ninth is dropped).
	void add_seat_point(const Vector2i &p_point);

protected:
	static void _bind_methods();

private:
	DefVehicleHudBlock block_{};
};

} // namespace godot
