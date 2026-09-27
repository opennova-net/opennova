#include "render/target_projection_xr_interface.h"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/sub_viewport.hpp>
#include <godot_cpp/classes/xr_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

namespace {

// The live interface: the XRServer owns its references (the interface list
// and the primary slot), so the extension keeps only the ObjectID.
ObjectID g_live_interface;
bool g_warned_foreign_primary = false;

} // namespace

void TargetProjectionXrInterface::_bind_methods() {
	ClassDB::bind_static_method("TargetProjectionXrInterface",
			D_METHOD("serve", "target", "transform", "projection"),
			&TargetProjectionXrInterface::serve);
	ClassDB::bind_static_method("TargetProjectionXrInterface", D_METHOD("release", "target"),
			&TargetProjectionXrInterface::release);
	ClassDB::bind_static_method("TargetProjectionXrInterface", D_METHOD("is_serving", "target"),
			&TargetProjectionXrInterface::is_serving);
	ClassDB::bind_static_method("TargetProjectionXrInterface",
			D_METHOD("served_projection", "target"),
			&TargetProjectionXrInterface::served_projection);
	ClassDB::bind_static_method("TargetProjectionXrInterface",
			D_METHOD("served_transform", "target"),
			&TargetProjectionXrInterface::served_transform);
	ClassDB::bind_integer_constant(get_class_static(), "", "TARGET_SIDE", kTargetSide);
}

TargetProjectionXrInterface *TargetProjectionXrInterface::live() {
	return Object::cast_to<TargetProjectionXrInterface>(ObjectDB::get_instance(g_live_interface));
}

TargetProjectionXrInterface *TargetProjectionXrInterface::acquire() {
	XRServer *server = XRServer::get_singleton();
	if (server == nullptr) {
		return nullptr;
	}
	const Ref<XRInterface> primary = server->get_primary_interface();
	TargetProjectionXrInterface *existing = live();
	if (existing != nullptr && (primary.is_null() || primary.ptr() == existing)) {
		if (primary.is_null()) {
			server->set_primary_interface(Ref<XRInterface>(existing));
		}
		return existing;
	}
	if (primary.is_valid()) {
		// A real XR runtime owns the use_xr viewports; the targets draw
		// through their own cameras instead.
		if (!g_warned_foreign_primary) {
			g_warned_foreign_primary = true;
			UtilityFunctions::push_warning(
					"TargetProjectionXrInterface: another XR interface is primary; the "
					"512-square targets draw through their cameras' square frusta");
		}
		return nullptr;
	}
	Ref<TargetProjectionXrInterface> created;
	created.instantiate();
	server->add_interface(created);
	created->initialize();
	server->set_primary_interface(created);
	g_live_interface = ObjectID(created->get_instance_id());
	return created.ptr();
}

void TargetProjectionXrInterface::retire(TargetProjectionXrInterface *p_interface) {
	// One reference across the removal: the server's two are the others.
	const Ref<TargetProjectionXrInterface> hold(p_interface);
	g_live_interface = ObjectID();
	XRServer *server = XRServer::get_singleton();
	if (server != nullptr) {
		if (server->get_primary_interface().ptr() == p_interface) {
			server->set_primary_interface(Ref<XRInterface>());
		}
		for (int32_t i = 0; i < server->get_interface_count(); ++i) {
			if (server->get_interface(i).ptr() == p_interface) {
				server->remove_interface(hold);
				break;
			}
		}
	}
	p_interface->uninitialize();
}

void TargetProjectionXrInterface::prune_dead_entries() {
	for (size_t i = entries_.size(); i-- > 0;) {
		if (ObjectDB::get_instance(ObjectID(entries_[i].target_id)) == nullptr) {
			entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
		}
	}
}

const TargetProjectionXrInterface::Entry *TargetProjectionXrInterface::find_by_viewport(
		const RID &p_viewport) const {
	for (const Entry &entry : entries_) {
		if (entry.viewport == p_viewport) {
			return &entry;
		}
	}
	return nullptr;
}

bool TargetProjectionXrInterface::serve(SubViewport *p_target, const Transform3D &p_transform,
		const Projection &p_projection) {
	if (p_target == nullptr) {
		return false;
	}
	TargetProjectionXrInterface *xr = acquire();
	if (xr == nullptr) {
		return false;
	}
	const RID viewport = p_target->get_viewport_rid();
	// The renderer names the target by its render target in _pre_draw_viewport.
	const RID render_target = RenderingServer::get_singleton()->viewport_get_render_target(viewport);
	{
		std::lock_guard<std::mutex> lock(xr->mutex_);
		xr->prune_dead_entries();
		Entry *entry = const_cast<Entry *>(xr->find_by_viewport(viewport));
		if (entry == nullptr) {
			xr->entries_.emplace_back();
			entry = &xr->entries_.back();
		}
		entry->target_id = static_cast<uint64_t>(p_target->get_instance_id());
		entry->viewport = viewport;
		entry->render_target = render_target;
		entry->transform = p_transform;
		entry->projection = p_projection;
	}
	if (!p_target->is_using_xr()) {
		p_target->set_use_xr(true);
	}
	return true;
}

void TargetProjectionXrInterface::release(SubViewport *p_target) {
	if (p_target == nullptr) {
		return;
	}
	TargetProjectionXrInterface *xr = live();
	if (xr == nullptr) {
		return;
	}
	bool served = false;
	bool empty = false;
	{
		std::lock_guard<std::mutex> lock(xr->mutex_);
		const RID viewport = p_target->get_viewport_rid();
		for (size_t i = 0; i < xr->entries_.size(); ++i) {
			if (xr->entries_[i].viewport == viewport) {
				xr->entries_.erase(xr->entries_.begin() + static_cast<std::ptrdiff_t>(i));
				served = true;
				break;
			}
		}
		xr->prune_dead_entries();
		empty = xr->entries_.empty();
	}
	if (served && p_target->is_using_xr()) {
		p_target->set_use_xr(false);
	}
	if (empty) {
		retire(xr);
	}
}

bool TargetProjectionXrInterface::is_serving(SubViewport *p_target) {
	TargetProjectionXrInterface *xr = live();
	if (xr == nullptr || p_target == nullptr) {
		return false;
	}
	std::lock_guard<std::mutex> lock(xr->mutex_);
	return xr->find_by_viewport(p_target->get_viewport_rid()) != nullptr;
}

Projection TargetProjectionXrInterface::served_projection(SubViewport *p_target) {
	TargetProjectionXrInterface *xr = live();
	if (xr == nullptr || p_target == nullptr) {
		return Projection();
	}
	std::lock_guard<std::mutex> lock(xr->mutex_);
	const Entry *entry = xr->find_by_viewport(p_target->get_viewport_rid());
	return entry != nullptr ? entry->projection : Projection();
}

Transform3D TargetProjectionXrInterface::served_transform(SubViewport *p_target) {
	TargetProjectionXrInterface *xr = live();
	if (xr == nullptr || p_target == nullptr) {
		return Transform3D();
	}
	std::lock_guard<std::mutex> lock(xr->mutex_);
	const Entry *entry = xr->find_by_viewport(p_target->get_viewport_rid());
	return entry != nullptr ? entry->transform : Transform3D();
}

void TargetProjectionXrInterface::cleanup_statics() {
	if (TargetProjectionXrInterface *xr = live()) {
		{
			std::lock_guard<std::mutex> lock(xr->mutex_);
			xr->entries_.clear();
		}
		retire(xr);
	}
	g_live_interface = ObjectID();
	g_warned_foreign_primary = false;
}

StringName TargetProjectionXrInterface::_get_name() const {
	return StringName("OpenNovaTargetProjection");
}

uint32_t TargetProjectionXrInterface::_get_capabilities() const {
	return XRInterface::XR_MONO;
}

bool TargetProjectionXrInterface::_is_initialized() const {
	return initialized_;
}

bool TargetProjectionXrInterface::_initialize() {
	initialized_ = true;
	return true;
}

void TargetProjectionXrInterface::_uninitialize() {
	initialized_ = false;
}

Vector2 TargetProjectionXrInterface::_get_render_target_size() {
	return Vector2(kTargetSide, kTargetSide);
}

uint32_t TargetProjectionXrInterface::_get_view_count() {
	return 1;
}

// No tracked head: nothing locks the camera to an XR origin.
Transform3D TargetProjectionXrInterface::_get_camera_transform() {
	return Transform3D();
}

Transform3D TargetProjectionXrInterface::_get_transform_for_view(uint32_t,
		const Transform3D &) {
	std::lock_guard<std::mutex> lock(mutex_);
	return drawing_transform_;
}

PackedFloat64Array TargetProjectionXrInterface::_get_projection_for_view(uint32_t, double,
		double, double) {
	std::lock_guard<std::mutex> lock(mutex_);
	PackedFloat64Array columns;
	columns.resize(16);
	double *write = columns.ptrw();
	for (int column = 0; column < 4; ++column) {
		for (int row = 0; row < 4; ++row) {
			write[column * 4 + row] = static_cast<double>(drawing_projection_.columns[column][row]);
		}
	}
	return columns;
}

// The renderer names the target it is about to draw; an unserved one skips
// its draw (there is no projection to give it).
bool TargetProjectionXrInterface::_pre_draw_viewport(const RID &p_render_target) {
	std::lock_guard<std::mutex> lock(mutex_);
	for (const Entry &entry : entries_) {
		if (entry.render_target == p_render_target) {
			drawing_transform_ = entry.transform;
			drawing_projection_ = entry.projection;
			return true;
		}
	}
	return false;
}

} // namespace godot
