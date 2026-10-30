#include "mnu/vehicle_menu_controller.h"

#include "mnu/mnu_document.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <runtime/world/occlusion.h> // render_matrix_from_pose

#include <godot_cpp/core/object.hpp>

namespace godot {

class VehicleMenuController::Host : public opennova::inmatch::VehicleMenuController::Host {
	VehicleMenuController *owner_;

public:
	explicit Host(VehicleMenuController *p_owner) : owner_(p_owner) {}
	opennova::inmatch::VehicleMenuSession session() override {
		Simulation *sim = owner_->sim_();
		return sim != nullptr ? sim->vehicle_menu_session()
							  : opennova::inmatch::VehicleMenuSession();
	}
	void close() override { owner_->emit_signal("close_requested"); }
};

VehicleMenuController::VehicleMenuController() : host_(std::make_unique<Host>(this)) {
	controller_.set_host(host_.get());
}

VehicleMenuController::~VehicleMenuController() {
	controller_.set_host(nullptr);
}

MenuDriver *VehicleMenuController::driver_() const {
	return driver_id_.is_null() ? nullptr
								: Object::cast_to<MenuDriver>(ObjectDB::get_instance(driver_id_));
}

Simulation *VehicleMenuController::sim_() const {
	return sim_id_.is_null() ? nullptr
							 : Object::cast_to<Simulation>(ObjectDB::get_instance(sim_id_));
}

void VehicleMenuController::setup(const Ref<MenuDriver> &p_driver, const Ref<Simulation> &p_sim) {
	driver_id_ = p_driver.is_valid() ? ObjectID(p_driver->get_instance_id()) : ObjectID();
	sim_id_ = p_sim.is_valid() ? ObjectID(p_sim->get_instance_id()) : ObjectID();
}

void VehicleMenuController::refresh() {
	if (MenuDriver *driver = driver_())
		controller_.refresh(driver->runtime());
}

void VehicleMenuController::click(bool p_cancel) {
	controller_.click(p_cancel);
}

String VehicleMenuController::preview_graphic() {
	// The graphic names are items.def bytes (the machine's code page).
	return opennova::cp1252_to_gd(controller_.preview_graphic());
}

namespace {
// A Godot length as 16.16, truncated (the engine's box words).
int32_t preview_q16(real_t p_value) {
	return static_cast<int32_t>(static_cast<double>(p_value) * 65536.0);
}
} // namespace

Vector3 VehicleMenuController::preview_eye(const AABB &p_bounds) {
	const Vector3 lo = p_bounds.position;
	const Vector3 hi = p_bounds.position + p_bounds.size;
	const int32_t box_min[3] = { preview_q16(lo.x), preview_q16(lo.y), preview_q16(lo.z) };
	const int32_t box_max[3] = { preview_q16(hi.x), preview_q16(hi.y), preview_q16(hi.z) };
	const opennova::inmatch::VehiclePreviewView view =
			opennova::inmatch::vehicle_preview_view(box_min, box_max, 0);
	// Render float (x south, y up, z east) to Godot (x east, y up, z south).
	return Vector3(view.eye[2], view.eye[1], view.eye[0]);
}

Basis VehicleMenuController::preview_model_basis(int64_t p_tick_ms) {
	const int32_t zero[3] = { 0, 0, 0 };
	const opennova::inmatch::VehiclePreviewView view =
			opennova::inmatch::vehicle_preview_view(zero, zero, static_cast<uint32_t>(p_tick_ms));
	const opennova::world::RenderMatrix pose = opennova::world::render_matrix_from_pose(
			zero, static_cast<int32_t>(view.yaw_bam), 0, 0);
	// The pose is row-major for row vectors (v' = v M), so its column-vector basis is M's
	// transpose; the render-to-Godot axis map swaps x and z on both sides.
	const int swap[3] = { 2, 1, 0 };
	Basis basis;
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			basis.rows[i][j] = pose.m[4 * swap[j] + swap[i]];
	return basis;
}

float VehicleMenuController::preview_fov() {
	return opennova::inmatch::VehiclePreviewView().fov_deg;
}

Color VehicleMenuController::preview_clear_color() {
	const uint32_t argb = opennova::inmatch::kVehiclePreviewClear;
	return Color(static_cast<float>((argb >> 16) & 0xFFu) / 255.0f,
			static_cast<float>((argb >> 8) & 0xFFu) / 255.0f,
			static_cast<float>(argb & 0xFFu) / 255.0f);
}

void VehicleMenuController::_bind_methods() {
	ClassDB::bind_method(D_METHOD("setup", "driver", "sim"), &VehicleMenuController::setup);
	ClassDB::bind_method(D_METHOD("refresh"), &VehicleMenuController::refresh);
	ClassDB::bind_method(D_METHOD("click", "cancel"), &VehicleMenuController::click);
	ClassDB::bind_method(D_METHOD("preview_graphic"), &VehicleMenuController::preview_graphic);
	ClassDB::bind_static_method("VehicleMenuController", D_METHOD("preview_eye", "bounds"),
			&VehicleMenuController::preview_eye);
	ClassDB::bind_static_method("VehicleMenuController", D_METHOD("preview_model_basis", "tick_ms"),
			&VehicleMenuController::preview_model_basis);
	ClassDB::bind_static_method(
			"VehicleMenuController", D_METHOD("preview_fov"), &VehicleMenuController::preview_fov);
	ClassDB::bind_static_method("VehicleMenuController", D_METHOD("preview_clear_color"),
			&VehicleMenuController::preview_clear_color);
	// ACCEPT, CANCEL or the Escape close clicked: the screen's frame closes.
	ADD_SIGNAL(MethodInfo("close_requested"));
}

} // namespace godot
