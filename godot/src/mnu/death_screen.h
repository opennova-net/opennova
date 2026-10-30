#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/menu/death_screen.h>

#include <memory>

#include "mnu/menu_driver.h"

namespace godot {

class RtxtStringFile;
class Simulation;

// The DEATH screen's controller (engine menu::DeathScreen, runtime/menu/death_screen.h carries the
// witnesses) over the session the presenter's world view names: its host reads the Simulation's
// deploy feed and sends its picks. DeployScreenPresenter claims it on the death.mnu driver at each
// open (MenuDriver.claim_screen) and releases it at the close; the device work (the frame, the MAP
// window, the cursor) stays the presenter's. `close_requested` asks the presenter to close the
// screen (a pick under the initial overlay, the exit's YES), `exit_confirmed` follows the YES's.
class DeathScreen : public MenuScreenBinding {
	GDCLASS(DeathScreen, MenuScreenBinding)

	class Host;

	opennova::menu::DeathScreen screen_;
	std::unique_ptr<Host> host_;
	ObjectID sim_id_;
	Ref<RtxtStringFile> gametext_;
	String medic_key_label_;

	Simulation *sim_() const;

protected:
	static void _bind_methods();

public:
	enum Frame {
		FRAME_KEEP = static_cast<int>(opennova::menu::DeathScreen::Frame::Keep),
		FRAME_CLOSE = static_cast<int>(opennova::menu::DeathScreen::Frame::Close),
		FRAME_TEARDOWN = static_cast<int>(opennova::menu::DeathScreen::Frame::Teardown),
	};

	DeathScreen();
	~DeathScreen() override;

	opennova::menu::MenuScreenController *screen_controller() override { return &screen_; }

	// The session (held by id: the world view keeps it alive).
	void set_simulation(const Ref<Simulation> &p_sim);
	// The tables the screen's text reads (gametext: the statics and the zone names; menutxt and
	// Game.bin: the Default row's Menu tokens), and the MedicReq binding's display string the
	// STROVER_CALLMEDIC static takes.
	void set_text_tables(const Ref<RtxtStringFile> &p_gametext, const Ref<RtxtStringFile> &p_menutxt,
			const Ref<RtxtStringFile> &p_gameui);
	void set_medic_key_label(const String &p_label);

	bool can_open() const;
	// One frame of the open screen over `p_driver`'s runtime: FRAME_*.
	int frame(const Ref<MenuDriver> &p_driver, bool p_scene_drawn);
	// A deploy pick of `p_param` (the screen open or not); true when the screen closes on it.
	bool send_pick(int p_param);
	bool is_input_pass_through() const { return screen_.pass_through(); }
	// The visible SPAWNPOINTS_LIST rows, each {label, param} (a read seam).
	Array get_spawn_rows() const;
	void reset() { screen_.reset(); }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::DeathScreen::Frame);
