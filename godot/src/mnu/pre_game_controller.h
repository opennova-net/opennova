#pragma once

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/inmatch/pre_game_controller.h>

#include <memory>

#include "mnu/menu_driver.h"

namespace godot {

// The join screen's controller (engine inmatch::PreGameController, pre.mnu PRE_GAME_MENU): the
// presenter claims it on its driver at the open (MenuDriver.claim_screen), hands it the join's
// progress, failure and choice and, once a frame while it follows a join, the join's status; the
// controller's abort and commit come back as cancel_requested and choice_committed.
class PreGameController : public MenuScreenBinding {
	GDCLASS(PreGameController, MenuScreenBinding)

	class Host;
	friend class Host;

protected:
	static void _bind_methods();

public:
	// PreGameController::FollowStep.
	enum FollowStep {
		FOLLOW_IDLE = static_cast<int>(opennova::inmatch::PreGameController::FollowStep::Idle),
		FOLLOW_SHOWN = static_cast<int>(opennova::inmatch::PreGameController::FollowStep::Shown),
		FOLLOW_HAND_OFF =
				static_cast<int>(opennova::inmatch::PreGameController::FollowStep::HandOff),
	};

	PreGameController();
	~PreGameController() override;

	opennova::menu::MenuScreenController *screen_controller() override { return &controller_; }

	void setup(const Ref<MenuDriver> &p_driver);
	void show_progress(const String &p_text);
	void show_failure(const String &p_text);
	bool show_choice(int p_server_flags);
	void accept();
	void follow(bool p_following) { controller_.follow(p_following); }
	bool is_following() const { return controller_.following(); }
	// One frame of the join it follows: `p_has_status` with the join's stage (JoinScreenStatus
	// STAGE_*) and its status line; FOLLOW_HAND_OFF when the loading screen takes over.
	int follow_frame(bool p_has_status, int p_stage, const String &p_text);
	// The panel showing (JoinScreenStatus PANEL_*: pre_game_menu.h PreGamePanel).
	int panel() const { return static_cast<int>(controller_.panel()); }

private:
	MenuDriver *driver_() const;

	opennova::inmatch::PreGameController controller_;
	std::unique_ptr<Host> host_;
	ObjectID driver_id_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::PreGameController::FollowStep);
