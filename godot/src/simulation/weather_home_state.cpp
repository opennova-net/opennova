#include "simulation/weather_home_state.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type weather_variant_type();
template <>
constexpr Variant::Type weather_variant_type<bool>() { return Variant::BOOL; }
template <>
constexpr Variant::Type weather_variant_type<int>() { return Variant::INT; }
template <>
constexpr Variant::Type weather_variant_type<int64_t>() { return Variant::INT; }

} // namespace

void WeatherHomeState::_bind_methods() {
#define WEATHER_HOME_BIND(m_type, m_name)                                                          \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &WeatherHomeState::get_##m_name);              \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &WeatherHomeState::set_##m_name);     \
	ADD_PROPERTY(PropertyInfo(weather_variant_type<m_type>(), #m_name), "set_" #m_name,           \
			"get_" #m_name);
	WEATHER_HOME_STATE_FIELDS(WEATHER_HOME_BIND)
#undef WEATHER_HOME_BIND
}
