#include "world/resource_root_resolver.h"

using namespace godot;

String ResourceRootResolver::resource_dir() {
	String out;
	if (GDVIRTUAL_CALL(_resource_dir, out)) {
		return out;
	}
	return String();
}

String ResourceRootResolver::expansion() {
	String out;
	if (GDVIRTUAL_CALL(_expansion, out)) {
		return out;
	}
	return String();
}

String ResourceRootResolver::game() {
	String out;
	if (GDVIRTUAL_CALL(_game, out)) {
		return out;
	}
	return "jo";
}

void ResourceRootResolver::_bind_methods() {
	GDVIRTUAL_BIND(_resource_dir);
	GDVIRTUAL_BIND(_expansion);
	GDVIRTUAL_BIND(_game);
	ClassDB::bind_method(D_METHOD("resource_dir"), &ResourceRootResolver::resource_dir);
	ClassDB::bind_method(D_METHOD("expansion"), &ResourceRootResolver::expansion);
	ClassDB::bind_method(D_METHOD("game"), &ResourceRootResolver::game);
}
