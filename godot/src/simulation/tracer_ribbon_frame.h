#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>

namespace godot {

// One tracer family's triangle-strip vertex run (the channels joined by
// degenerate pairs), ready for verbatim ImmediateMesh upload: parallel
// positions and colors.
class TracerRibbonStrip : public RefCounted {
	GDCLASS(TracerRibbonStrip, RefCounted)

public:
	PackedVector3Array get_positions() const { return positions_; }
	void set_positions(const PackedVector3Array &p_positions) { positions_ = p_positions; }
	PackedColorArray get_colors() const { return colors_; }
	void set_colors(const PackedColorArray &p_colors) { colors_ = p_colors; }

protected:
	static void _bind_methods();

private:
	PackedVector3Array positions_;
	PackedColorArray colors_;
};

// The compiled tracer frame (Simulation.compile_tracer_ribbons; engine:
// runtime/renderer/tracer_frame.cpp): the additive and alpha (smoke)
// families and the channel count the present pass peaks.
class TracerRibbonFrame : public RefCounted {
	GDCLASS(TracerRibbonFrame, RefCounted)

public:
	Ref<TracerRibbonStrip> get_additive() const { return additive_; }
	void set_additive(const Ref<TracerRibbonStrip> &p_strip) { additive_ = p_strip; }
	Ref<TracerRibbonStrip> get_alpha() const { return alpha_; }
	void set_alpha(const Ref<TracerRibbonStrip> &p_strip) { alpha_ = p_strip; }
	int get_channels() const { return channels_; }
	void set_channels(int p_channels) { channels_ = p_channels; }

protected:
	static void _bind_methods();

private:
	Ref<TracerRibbonStrip> additive_;
	Ref<TracerRibbonStrip> alpha_;
	int channels_ = 0;
};

} // namespace godot
