#include "network/novaworld_service_target.h"

#include "resource_index/launch_flags.h"
#include "util/string_convert.h"

#include <godot_cpp/classes/os.hpp>

#include <net/novaworld/service_target.h>

namespace godot {

namespace {

opennova::NovaWorldTarget engine_target(int target) {
	return target == NovaWorldServiceTarget::TARGET_ORIGINAL ? opennova::NovaWorldTarget::Original
															 : opennova::NovaWorldTarget::OpenNova;
}

} // namespace

String NovaWorldServiceTarget::opennova_gate_host() {
	OS *os = OS::get_singleton();
	return opennova_gate_host_for(LaunchFlags::novaworld_host(),
			os != nullptr && os->has_feature("template"), os != nullptr && os->is_debug_build());
}

String NovaWorldServiceTarget::opennova_gate_host_for(
		const String &flag_host, bool exported, bool debug) {
	return opennova::to_gd(
			opennova::opennova_gate_host(opennova::to_std(flag_host), exported, debug));
}

String NovaWorldServiceTarget::target_gate_host(int target) {
	return opennova::to_gd(opennova::target_gate_host(
			engine_target(target), opennova::to_std(opennova_gate_host())));
}

String NovaWorldServiceTarget::web_base() {
	return web_base_for(opennova_gate_host());
}

String NovaWorldServiceTarget::web_base_for(const String &gate_host) {
	return opennova::to_gd(opennova::service_web_base(opennova::to_std(gate_host)));
}

bool NovaWorldServiceTarget::is_novalogic_host(const String &host) {
	return opennova::is_novalogic_host(opennova::to_std(host));
}

int NovaWorldServiceTarget::stored_target(const Variant &stored) {
	const std::optional<int64_t> value = stored.get_type() == Variant::INT
			? std::optional<int64_t>(static_cast<int64_t>(stored))
			: std::nullopt;
	return static_cast<int>(opennova::stored_novaworld_target(value));
}

int NovaWorldServiceTarget::launch_target(const String &flag_host, bool joining, int stored) {
	return static_cast<int>(opennova::launch_novaworld_target(
			opennova::to_std(flag_host), joining, engine_target(stored)));
}

void NovaWorldServiceTarget::_bind_methods() {
	const char *name = "NovaWorldServiceTarget";
	ClassDB::bind_static_method(
			name, D_METHOD("opennova_gate_host"), &NovaWorldServiceTarget::opennova_gate_host);
	ClassDB::bind_static_method(name,
			D_METHOD("opennova_gate_host_for", "flag_host", "exported", "debug"),
			&NovaWorldServiceTarget::opennova_gate_host_for);
	ClassDB::bind_static_method(name, D_METHOD("target_gate_host", "target"),
			&NovaWorldServiceTarget::target_gate_host);
	ClassDB::bind_static_method(name, D_METHOD("web_base"), &NovaWorldServiceTarget::web_base);
	ClassDB::bind_static_method(
			name, D_METHOD("web_base_for", "gate_host"), &NovaWorldServiceTarget::web_base_for);
	ClassDB::bind_static_method(name, D_METHOD("is_novalogic_host", "host"),
			&NovaWorldServiceTarget::is_novalogic_host);
	ClassDB::bind_static_method(
			name, D_METHOD("stored_target", "stored"), &NovaWorldServiceTarget::stored_target);
	ClassDB::bind_static_method(name, D_METHOD("launch_target", "flag_host", "joining", "stored"),
			&NovaWorldServiceTarget::launch_target);
	BIND_ENUM_CONSTANT(TARGET_OPENNOVA);
	BIND_ENUM_CONSTANT(TARGET_ORIGINAL);
}

} // namespace godot
