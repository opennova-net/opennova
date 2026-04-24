#include "env/nova_env_keyframe.h"

#include <algorithm>

using namespace godot;

namespace {

Color to_color(const opennova::env::Rgb &rgb) {
	return Color(rgb.r, rgb.g, rgb.b);
}

opennova::env::Rgb to_rgb(const Color &color) {
	return {
		static_cast<float>(color.r),
		static_cast<float>(color.g),
		static_cast<float>(color.b),
	};
}

int clamp_time(int time) {
	return std::max(0, std::min(2359, time));
}

} // namespace

NovaEnvKeyframe::NovaEnvKeyframe() {}

void NovaEnvKeyframe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_time", "time"), &NovaEnvKeyframe::set_time);
	ClassDB::bind_method(D_METHOD("get_time"), &NovaEnvKeyframe::get_time);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "time", PROPERTY_HINT_RANGE, "0,2359,1"), "set_time", "get_time");

#define BIND_COLOR(prop, setter, getter) \
	ClassDB::bind_method(D_METHOD(#setter, "color"), &NovaEnvKeyframe::setter); \
	ClassDB::bind_method(D_METHOD(#getter), &NovaEnvKeyframe::getter); \
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, #prop), #setter, #getter);

	ADD_GROUP("Lighting", "");
	BIND_COLOR(sun_color, set_sun_color, get_sun_color)
	BIND_COLOR(ground_color, set_ground_color, get_ground_color)
	BIND_COLOR(moon_color, set_moon_color, get_moon_color)
	ADD_GROUP("Fog", "");
	BIND_COLOR(fog_color, set_fog_color, get_fog_color)
	BIND_COLOR(skyfog_color, set_skyfog_color, get_skyfog_color)
	ADD_GROUP("Sky", "sky");
	BIND_COLOR(sky_color, set_sky_color, get_sky_color)
	BIND_COLOR(skybase_color, set_skybase_color, get_skybase_color)
	BIND_COLOR(skybright_color, set_skybright_color, get_skybright_color)
	BIND_COLOR(skyhighlight_color, set_skyhighlight_color, get_skyhighlight_color)
	ADD_GROUP("Clouds", "cloud");
	BIND_COLOR(cloudbase_color, set_cloudbase_color, get_cloudbase_color)
	BIND_COLOR(cloudhighlight_color, set_cloudhighlight_color, get_cloudhighlight_color)
	BIND_COLOR(cloudedge_color, set_cloudedge_color, get_cloudedge_color)

#undef BIND_COLOR
}

void NovaEnvKeyframe::copy_from_native(const opennova::env::Keyframe &keyframe) {
	time = keyframe.time;
	sun_color = to_color(keyframe.sun);
	ground_color = to_color(keyframe.ground);
	fog_color = to_color(keyframe.fog);
	sky_color = to_color(keyframe.sky);
	moon_color = to_color(keyframe.moon);
	skyfog_color = to_color(keyframe.skyfog);
	skybase_color = to_color(keyframe.skybase);
	skybright_color = to_color(keyframe.skybright);
	skyhighlight_color = to_color(keyframe.skyhighlight);
	cloudbase_color = to_color(keyframe.cloudbase);
	cloudhighlight_color = to_color(keyframe.cloudhighlight);
	cloudedge_color = to_color(keyframe.cloudedge);
	emit_changed();
}

opennova::env::Keyframe NovaEnvKeyframe::to_native() const {
	opennova::env::Keyframe keyframe;
	keyframe.time = clamp_time(time);
	keyframe.sun = to_rgb(sun_color);
	keyframe.ground = to_rgb(ground_color);
	keyframe.fog = to_rgb(fog_color);
	keyframe.sky = to_rgb(sky_color);
	keyframe.moon = to_rgb(moon_color);
	keyframe.skyfog = to_rgb(skyfog_color);
	keyframe.skybase = to_rgb(skybase_color);
	keyframe.skybright = to_rgb(skybright_color);
	keyframe.skyhighlight = to_rgb(skyhighlight_color);
	keyframe.cloudbase = to_rgb(cloudbase_color);
	keyframe.cloudhighlight = to_rgb(cloudhighlight_color);
	keyframe.cloudedge = to_rgb(cloudedge_color);
	return keyframe;
}

void NovaEnvKeyframe::set_time(int p_time) { time = clamp_time(p_time); emit_changed(); }
int NovaEnvKeyframe::get_time() const { return time; }

#define IMPL_COLOR(field, setter, getter) \
	void NovaEnvKeyframe::setter(const Color &p_color) { field = p_color; emit_changed(); } \
	Color NovaEnvKeyframe::getter() const { return field; }

IMPL_COLOR(sun_color, set_sun_color, get_sun_color)
IMPL_COLOR(ground_color, set_ground_color, get_ground_color)
IMPL_COLOR(fog_color, set_fog_color, get_fog_color)
IMPL_COLOR(sky_color, set_sky_color, get_sky_color)
IMPL_COLOR(moon_color, set_moon_color, get_moon_color)
IMPL_COLOR(skyfog_color, set_skyfog_color, get_skyfog_color)
IMPL_COLOR(skybase_color, set_skybase_color, get_skybase_color)
IMPL_COLOR(skybright_color, set_skybright_color, get_skybright_color)
IMPL_COLOR(skyhighlight_color, set_skyhighlight_color, get_skyhighlight_color)
IMPL_COLOR(cloudbase_color, set_cloudbase_color, get_cloudbase_color)
IMPL_COLOR(cloudhighlight_color, set_cloudhighlight_color, get_cloudhighlight_color)
IMPL_COLOR(cloudedge_color, set_cloudedge_color, get_cloudedge_color)

#undef IMPL_COLOR
