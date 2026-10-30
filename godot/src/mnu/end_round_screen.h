#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>

#include <runtime/inmatch/end_round_screen.h>

#include <memory>

#include "mnu/menu_driver.h"

namespace godot {

class RtxtStringFile;
class Simulation;

// The end of round's STAT screen controller (engine inmatch::EndRoundScreen,
// runtime/inmatch/end_round_screen.h carries the witnesses) over the session the presenter's world
// view names: its host reads the Simulation's end-of-round feed (the columns, the rows, the local
// slot, the game type's team arm). EndRoundPresenter claims it on the stat.mnu driver when the
// transition opens the screen and releases it at the close. `close_requested` asks the presenter
// to close the screen, `exit_to_menu_requested` follows the exit's YES.
class EndRoundScreen : public MenuScreenBinding {
	GDCLASS(EndRoundScreen, MenuScreenBinding)

	class Host;

	opennova::inmatch::EndRoundScreen screen_;
	std::unique_ptr<Host> host_;
	ObjectID sim_id_;
	Ref<RtxtStringFile> gametext_;

	Simulation *sim_() const;

protected:
	static void _bind_methods();

public:
	EndRoundScreen();
	~EndRoundScreen() override;

	opennova::menu::MenuScreenController *screen_controller() override { return &screen_; }

	// The session (held by id: the world view keeps it alive) and the gametext table the column
	// headers resolve through.
	void set_simulation(const Ref<Simulation> &p_sim);
	void set_gametext(const Ref<RtxtStringFile> &p_gametext) { gametext_ = p_gametext; }
	// The tab the stat filter shows last (0 all, 1 team 2, 2 team 1), -1 before any.
	int get_tab() const { return screen_.tab(); }
};

} // namespace godot
