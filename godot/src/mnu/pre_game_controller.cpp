#include "mnu/pre_game_controller.h"

#include "mnu/menu_text.h"
#include "util/string_convert.h"

#include <godot_cpp/core/object.hpp>

namespace godot {

class PreGameController::Host : public opennova::inmatch::PreGameController::Host {
	PreGameController *owner_;

public:
	explicit Host(PreGameController *p_owner) : owner_(p_owner) {}
	void cancelled() override { owner_->emit_signal("cancel_requested"); }
	void committed(const std::string &p_game_password, bool p_spectate,
			const std::string &p_team_password) override {
		const MenuDriver *driver = owner_->driver_();
		owner_->emit_signal("choice_committed", menu_text_to_gd(driver, p_game_password),
				p_spectate, menu_text_to_gd(driver, p_team_password));
	}
};

PreGameController::PreGameController() : host_(std::make_unique<Host>(this)) {
	controller_.set_host(host_.get());
}

PreGameController::~PreGameController() {
	controller_.set_host(nullptr);
}

MenuDriver *PreGameController::driver_() const {
	return driver_id_.is_null() ? nullptr
								: Object::cast_to<MenuDriver>(ObjectDB::get_instance(driver_id_));
}

void PreGameController::setup(const Ref<MenuDriver> &p_driver) {
	driver_id_ = p_driver.is_valid() ? ObjectID(p_driver->get_instance_id()) : ObjectID();
}

void PreGameController::show_progress(const String &p_text) {
	if (MenuDriver *driver = driver_())
		controller_.show_progress(driver->runtime(), opennova::gd_to_retail_text(p_text));
}

void PreGameController::show_failure(const String &p_text) {
	if (MenuDriver *driver = driver_())
		controller_.show_failure(driver->runtime(), opennova::gd_to_retail_text(p_text));
}

bool PreGameController::show_choice(int p_server_flags) {
	MenuDriver *driver = driver_();
	if (driver == nullptr || p_server_flags < 0)
		return false;
	return controller_.show_choice(driver->runtime(), static_cast<uint32_t>(p_server_flags));
}

void PreGameController::accept() {
	if (MenuDriver *driver = driver_())
		controller_.accept(driver->runtime());
}

int PreGameController::follow_frame(bool p_has_status, int p_stage, const String &p_text) {
	MenuDriver *driver = driver_();
	if (driver == nullptr)
		return FOLLOW_IDLE;
	return static_cast<int>(controller_.follow_frame(driver->runtime(), p_has_status,
			static_cast<opennova::inmatch::JoinScreenStage>(p_stage),
			opennova::gd_to_retail_text(p_text)));
}

void PreGameController::_bind_methods() {
	BIND_ENUM_CONSTANT(FOLLOW_IDLE);
	BIND_ENUM_CONSTANT(FOLLOW_SHOWN);
	BIND_ENUM_CONSTANT(FOLLOW_HAND_OFF);
	ClassDB::bind_method(D_METHOD("setup", "driver"), &PreGameController::setup);
	ClassDB::bind_method(D_METHOD("show_progress", "text"), &PreGameController::show_progress);
	ClassDB::bind_method(D_METHOD("show_failure", "text"), &PreGameController::show_failure);
	ClassDB::bind_method(D_METHOD("show_choice", "server_flags"), &PreGameController::show_choice);
	ClassDB::bind_method(D_METHOD("accept"), &PreGameController::accept);
	ClassDB::bind_method(D_METHOD("follow", "following"), &PreGameController::follow);
	ClassDB::bind_method(D_METHOD("is_following"), &PreGameController::is_following);
	ClassDB::bind_method(D_METHOD("follow_frame", "has_status", "stage", "text"),
			&PreGameController::follow_frame);
	ClassDB::bind_method(D_METHOD("panel"), &PreGameController::panel);
	// BACK, ABORT_CANCEL or ACCEPT's abort: the join is abandoned.
	ADD_SIGNAL(MethodInfo("cancel_requested"));
	// The choice's last ACCEPT: the three controls as the commit reads them.
	ADD_SIGNAL(MethodInfo("choice_committed", PropertyInfo(Variant::STRING, "game_password"),
			PropertyInfo(Variant::BOOL, "spectate"),
			PropertyInfo(Variant::STRING, "team_password")));
}

} // namespace godot
