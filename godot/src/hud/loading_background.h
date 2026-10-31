#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class ResourceRoot;

// The loading screen's background pick, read-only: the image LoadingScreen
// loads and whether it is the mission's own sidecar (the flag the session
// band's right edge reads, HudPos.composite_loading_session_text); plus the SP
// start-mission splash gate that reads the same flag (engine
// hud::loading_background / start_mission_splash_due,
// runtime/hud/loading_background.h carries the witnesses). The probe is the root's UI image lookup, loose files first.
class LoadingBackground : public RefCounted {
	GDCLASS(LoadingBackground, RefCounted)

	String name_;
	bool custom_ = false;

protected:
	static void _bind_methods();

public:
	// A null root finds no sidecar.
	static Ref<LoadingBackground> resolve(const Ref<ResourceRoot> &p_root, const String &p_mission_file);
	static bool start_mission_splash_due(bool p_in_session, bool p_custom_background);

	String get_name() const { return name_; }
	bool get_custom() const { return custom_; }
};

} // namespace godot
