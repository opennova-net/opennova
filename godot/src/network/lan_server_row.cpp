#include "network/lan_server_row.h"

using namespace godot;

namespace {

template <typename T>
constexpr Variant::Type variant_type_of();
template <>
constexpr Variant::Type variant_type_of<int>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<int64_t>() { return Variant::INT; }
template <>
constexpr Variant::Type variant_type_of<String>() { return Variant::STRING; }

} // namespace

void LanServerRow::_bind_methods() {
#define LAN_SERVER_ROW_BIND(m_type, m_name, m_default)                                              \
	ClassDB::bind_method(D_METHOD("get_" #m_name), &LanServerRow::get_##m_name);                    \
	ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &LanServerRow::set_##m_name);           \
	ADD_PROPERTY(PropertyInfo(variant_type_of<m_type>(), #m_name), "set_" #m_name, "get_" #m_name);
	LAN_SERVER_ROW_FIELDS(LAN_SERVER_ROW_BIND)
#undef LAN_SERVER_ROW_BIND
	ClassDB::bind_static_method("LanServerRow", D_METHOD("make", "server_name", "host_ip", "port"),
			&LanServerRow::make);
}

Ref<LanServerRow> LanServerRow::make(const String &p_server_name, const String &p_host_ip,
		int p_port) {
	Ref<LanServerRow> out;
	out.instantiate();
	out->set_server_name(p_server_name);
	out->set_host_ip(p_host_ip);
	out->set_port(p_port);
	return out;
}
