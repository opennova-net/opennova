#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// The shell's persisted resource settings as the world sees them (ADR 0043
// slice G10: the ex `_resolve_root` Callable the net-session drive borrowed).
// A world load without an injected root mounts the persisted resource
// directory with the persisted expansion and game code; the settings store
// itself (ResourceDirSettings) stays shell-side, so the shell implements the
// three hooks and installs the resolver
// (GameWorld.set_resource_root_resolver). Without one the world reads the
// unset defaults: no directory, the base game, the "jo" decode key.
class ResourceRootResolver : public RefCounted {
	GDCLASS(ResourceRootResolver, RefCounted)

protected:
	static void _bind_methods();

	// The persisted resource directory ("" = unset).
	GDVIRTUAL0R(String, _resource_dir)
	// The persisted expansion name ("" = the base game).
	GDVIRTUAL0R(String, _expansion)
	// The persisted game code ("jo" when unset).
	GDVIRTUAL0R(String, _game)

public:
	String resource_dir();
	String expansion();
	String game();
};

} // namespace godot
