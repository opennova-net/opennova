#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>

// The collision ray-trace debug channel for the F3 rays view
// (Simulation::get_ray_debug): `events` is the stride-12 float run — per
// event [category, age_ticks, result, sx,sy,sz, ex,ey,ez, hx,hy,hz] in Godot
// space (result 0 clear / 1 hit at h / 2 blocked with h == e), already
// filtered by the engine-held category mask and TTL, oldest first per
// category; `counts` lists every category unfiltered. Read-write so the view
// test authors a report.

namespace godot {

class RayDebugCount : public RefCounted {
	GDCLASS(RayDebugCount, RefCounted)

public:
	String get_name() const { return name_; }
	void set_name(const String &p_value) { name_ = p_value; }
	int64_t get_held() const { return held_; }
	void set_held(int64_t p_value) { held_ = p_value; }
	int64_t get_total() const { return total_; }
	void set_total(int64_t p_value) { total_ = p_value; }
	static Ref<RayDebugCount> make(const String &p_name, int64_t p_held, int64_t p_total);

protected:
	static void _bind_methods();

private:
	String name_;
	int64_t held_ = 0;
	int64_t total_ = 0;
};

class RayDebugReport : public RefCounted {
	GDCLASS(RayDebugReport, RefCounted)

public:
	static constexpr int kStride = 12;

	int get_stride() const { return kStride; }
	PackedFloat32Array get_events() const { return events_; }
	void set_events(const PackedFloat32Array &p_value) { events_ = p_value; }
	TypedArray<RayDebugCount> get_counts() const { return counts_; }
	void set_counts(const TypedArray<RayDebugCount> &p_value) { counts_ = p_value; }
	void add_count(const Ref<RayDebugCount> &p_count) { counts_.push_back(p_count); }
	int64_t get_mask() const { return mask_; }
	void set_mask(int64_t p_value) { mask_ = p_value; }
	int64_t get_ttl() const { return ttl_; }
	void set_ttl(int64_t p_value) { ttl_ = p_value; }
	bool get_recording() const { return recording_; }
	void set_recording(bool p_value) { recording_ = p_value; }
	int64_t get_tick() const { return tick_; }
	void set_tick(int64_t p_value) { tick_ = p_value; }

protected:
	static void _bind_methods();

private:
	PackedFloat32Array events_;
	TypedArray<RayDebugCount> counts_;
	int64_t mask_ = 0;
	int64_t ttl_ = 93;
	bool recording_ = false;
	int64_t tick_ = 0;
};

} // namespace godot
