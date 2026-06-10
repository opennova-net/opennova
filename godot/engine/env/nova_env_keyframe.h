#pragma once

#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>

#include <env/env.h>

namespace godot {

// Godot Resource adapter for opennova::env::Keyframe.
// Engine equivalent: one 52-byte TOD entry populated by [orig: TimeOfDay_ParseProperty @ 0x57c590]
// and consumed by [orig: Environment_ComputeTimeOfDayColors @ 0x57de40].
class NovaEnvKeyframe : public Resource {
	GDCLASS(NovaEnvKeyframe, Resource)

private:
	int time = 0;
	Color sun_color;
	Color ground_color;
	Color fog_color;
	Color sky_color;
	Color moon_color;
	Color skyfog_color;
	Color skybase_color;
	Color skybright_color;
	Color skyhighlight_color;
	Color cloudbase_color;
	Color cloudhighlight_color;
	Color cloudedge_color;

protected:
	static void _bind_methods();

public:
	NovaEnvKeyframe();

	void copy_from_native(const opennova::env::Keyframe &keyframe);
	opennova::env::Keyframe to_native() const;

	void set_time(int p_time);
	int get_time() const;

	void set_sun_color(const Color &p_color);
	Color get_sun_color() const;
	void set_ground_color(const Color &p_color);
	Color get_ground_color() const;
	void set_fog_color(const Color &p_color);
	Color get_fog_color() const;
	void set_sky_color(const Color &p_color);
	Color get_sky_color() const;
	void set_moon_color(const Color &p_color);
	Color get_moon_color() const;
	void set_skyfog_color(const Color &p_color);
	Color get_skyfog_color() const;
	void set_skybase_color(const Color &p_color);
	Color get_skybase_color() const;
	void set_skybright_color(const Color &p_color);
	Color get_skybright_color() const;
	void set_skyhighlight_color(const Color &p_color);
	Color get_skyhighlight_color() const;
	void set_cloudbase_color(const Color &p_color);
	Color get_cloudbase_color() const;
	void set_cloudhighlight_color(const Color &p_color);
	Color get_cloudhighlight_color() const;
	void set_cloudedge_color(const Color &p_color);
	Color get_cloudedge_color() const;
};

} // namespace godot
