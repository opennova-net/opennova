#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

// The NovaWorld service the menu dials (engine net/novaworld/service_target.h): which gate each
// target reaches, the OpenNova target's web site, and the stored and launch choices read back.
// The build facts (an export, a debug build) and the launch's --novaworld-host are sampled here;
// every rule is the engine's.
class NovaWorldServiceTarget : public RefCounted {
	GDCLASS(NovaWorldServiceTarget, RefCounted)

protected:
	static void _bind_methods();

public:
	enum Target {
		TARGET_OPENNOVA = 0,
		TARGET_ORIGINAL = 1,
	};

	// The OpenNova target's gate for this build and launch (opennova_gate_host).
	static String opennova_gate_host();
	static String opennova_gate_host_for(const String &flag_host, bool exported, bool debug);
	// The gate `target` dials.
	static String target_gate_host(int target);
	// The OpenNova target's web site, for this build and launch or for a gate.
	static String web_base();
	static String web_base_for(const String &gate_host);
	static bool is_novalogic_host(const String &host);
	// The stored choice (any Variant: what the settings file held) read back.
	static int stored_target(const Variant &stored);
	// The target the launch's NovaWorld menu opens on, given the stored one.
	static int launch_target(const String &flag_host, bool joining, int stored);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::NovaWorldServiceTarget::Target);
