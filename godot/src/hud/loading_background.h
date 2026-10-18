#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class ResourceRoot;

// The loading screen's background pick, read-only: the image LoadingScreen
// loads, whether it is the mission's own sidecar, and the session text band's
// right edge over it; plus the SP start-mission splash gate that reads the same
// flag (engine hud::loading_background / loading_band_right /
// start_mission_splash_due, runtime/hud/loading_background.h carries the
// witnesses). The probe is the root's UI image lookup, loose files first.
class LoadingBackground : public RefCounted {
	GDCLASS(LoadingBackground, RefCounted)

	String name_;
	bool custom_ = false;
	int band_right_ = 0;

protected:
	static void _bind_methods();

public:
	// A null root finds no sidecar.
	static Ref<LoadingBackground> resolve(const Ref<ResourceRoot> &p_root, const String &p_mission_file);
	static bool start_mission_splash_due(bool p_in_session, bool p_custom_background);

	String get_name() const { return name_; }
	bool get_custom() const { return custom_; }
	int get_band_right() const { return band_right_; }
};

} // namespace godot
