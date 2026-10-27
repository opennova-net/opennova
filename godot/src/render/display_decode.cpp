#include "render/display_decode.h"
#include "render/world_environment_lookup.h"

#include <cstdint>
#include <map>

#include <godot_cpp/classes/compositor.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/world_environment.hpp>

using namespace godot;

namespace {

// The DisplayDecode whose compositor each scenario renders with, by the
// scenario's RID (DisplayDecode::scenario_compositor; Godot binds no getter for
// a scenario's compositor). Object ids, never references: nothing here holds a
// resource past the servers at exit.
std::map<std::int64_t, ObjectID> &scenario_decodes() {
	static std::map<std::int64_t, ObjectID> decodes;
	return decodes;
}

} // namespace

void DisplayDecode::_bind_methods() {}

void DisplayDecode::install() {
	if (effect_.is_null())
		effect_.instantiate();
	Node *scope = get_parent() != nullptr ? get_parent() : this;
	WorldEnvironment *world_environment = find_world_environment(scope);
	if (world_environment != nullptr) {
		world_environment_id_ = ObjectID(world_environment->get_instance_id());
		previous_compositor_ = world_environment->get_compositor();
		installed_compositor_ = compositor_with_terminal(previous_compositor_,
				effect_);
		world_environment->set_compositor(installed_compositor_);
		return;
	}
	// No WorldEnvironment in scope: attach the compositor to the viewport's
	// scenario directly. A view without a WorldEnvironment has no compositor
	// of its own, so there is nothing to merge or restore beyond clearing it.
	Viewport *viewport = get_viewport();
	world_ = viewport != nullptr ? viewport->find_world_3d() : Ref<World3D>();
	RenderingServer *server = RenderingServer::get_singleton();
	if (world_.is_null() || server == nullptr)
		return;
	installed_compositor_ = compositor_with_terminal(Ref<Compositor>(), effect_);
	server->scenario_set_compositor(world_->get_scenario(),
			installed_compositor_->get_rid());
	scenario_decodes()[world_->get_scenario().get_id()] = ObjectID(get_instance_id());
}

Ref<Compositor> DisplayDecode::scenario_compositor(const Ref<World3D> &p_world) {
	if (p_world.is_null())
		return Ref<Compositor>();
	const auto found = scenario_decodes().find(p_world->get_scenario().get_id());
	if (found == scenario_decodes().end())
		return Ref<Compositor>();
	const DisplayDecode *decode = Object::cast_to<DisplayDecode>(
			ObjectDB::get_instance(static_cast<std::uint64_t>(found->second)));
	return decode != nullptr ? decode->installed_compositor_ : Ref<Compositor>();
}

void DisplayDecode::uninstall() {
	Ref<FrameFxCompositorEffect> effect = effect_;
	if (effect.is_valid())
		effect->set_enabled(false);
	WorldEnvironment *world_environment =
			world_environment_from_id(world_environment_id_);
	if (world_environment != nullptr &&
			world_environment->get_compositor() == installed_compositor_)
		world_environment->set_compositor(previous_compositor_);
	RenderingServer *server = RenderingServer::get_singleton();
	if (world_.is_valid()) {
		const auto found = scenario_decodes().find(world_->get_scenario().get_id());
		if (found != scenario_decodes().end() && found->second == ObjectID(get_instance_id()))
			scenario_decodes().erase(found);
	}
	if (world_.is_valid() && server != nullptr)
		server->scenario_set_compositor(world_->get_scenario(), RID());
	world_environment_id_ = ObjectID();
	world_.unref();
	installed_compositor_.unref();
	previous_compositor_.unref();
	if (effect.is_valid() && server != nullptr &&
			server->get_rendering_device() != nullptr)
		server->force_sync();
	if (effect.is_valid())
		effect->release_device_resources();
	effect_.unref();
	effect.unref();
}

void DisplayDecode::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		install();
	} else if (p_what == NOTIFICATION_EXIT_TREE) {
		uninstall();
	}
}
