#include "env/env_keyframe.h"
#include "util/color_convert.h"

#include <algorithm>

using namespace godot;

namespace {

} // namespace

EnvKeyframe::EnvKeyframe() {}

void EnvKeyframe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_time", "time"), &EnvKeyframe::set_time);
	ClassDB::bind_method(D_METHOD("get_time"), &EnvKeyframe::get_time);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "time", PROPERTY_HINT_RANGE, "0,2359,1"), "set_time", "get_time");

#define BIND_COLOR(prop, setter, getter) \
	ClassDB::bind_method(D_METHOD(#setter, "color"), &EnvKeyframe::setter); \
	ClassDB::bind_method(D_METHOD(#getter), &EnvKeyframe::getter); \
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

void EnvKeyframe::copy_from_native(const opennova::env::Keyframe &keyframe) {
	time = keyframe.time;
	sun_color = opennova::color_from_env_rgb(keyframe.sun);
	ground_color = opennova::color_from_env_rgb(keyframe.ground);
	fog_color = opennova::color_from_env_rgb(keyframe.fog);
	sky_color = opennova::color_from_env_rgb(keyframe.sky);
	moon_color = opennova::color_from_env_rgb(keyframe.moon);
	skyfog_color = opennova::color_from_env_rgb(keyframe.skyfog);
	skybase_color = opennova::color_from_env_rgb(keyframe.skybase);
	skybright_color = opennova::color_from_env_rgb(keyframe.skybright);
	skyhighlight_color = opennova::color_from_env_rgb(keyframe.skyhighlight);
	cloudbase_color = opennova::color_from_env_rgb(keyframe.cloudbase);
	cloudhighlight_color = opennova::color_from_env_rgb(keyframe.cloudhighlight);
	cloudedge_color = opennova::color_from_env_rgb(keyframe.cloudedge);
	emit_changed();
}

opennova::env::Keyframe EnvKeyframe::to_native() const {
	opennova::env::Keyframe keyframe;
	keyframe.time = opennova::env::clamp_tod_time(time);
	keyframe.sun = opennova::env_rgb_from_color(sun_color);
	keyframe.ground = opennova::env_rgb_from_color(ground_color);
	keyframe.fog = opennova::env_rgb_from_color(fog_color);
	keyframe.sky = opennova::env_rgb_from_color(sky_color);
	keyframe.moon = opennova::env_rgb_from_color(moon_color);
	keyframe.skyfog = opennova::env_rgb_from_color(skyfog_color);
	keyframe.skybase = opennova::env_rgb_from_color(skybase_color);
	keyframe.skybright = opennova::env_rgb_from_color(skybright_color);
	keyframe.skyhighlight = opennova::env_rgb_from_color(skyhighlight_color);
	keyframe.cloudbase = opennova::env_rgb_from_color(cloudbase_color);
	keyframe.cloudhighlight = opennova::env_rgb_from_color(cloudhighlight_color);
	keyframe.cloudedge = opennova::env_rgb_from_color(cloudedge_color);
	return keyframe;
}

void EnvKeyframe::set_time(int p_time) { time = opennova::env::clamp_tod_time(p_time); emit_changed(); }
int EnvKeyframe::get_time() const { return time; }

#define IMPL_COLOR(field, setter, getter) \
	void EnvKeyframe::setter(const Color &p_color) { field = p_color; emit_changed(); } \
	Color EnvKeyframe::getter() const { return field; }

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
