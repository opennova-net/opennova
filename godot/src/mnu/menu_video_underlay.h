#pragma once

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/video_stream_player.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <menu/menu_video.h>

#include <array>

namespace godot {

// The menu backdrop device leg: plays the witnessed backdrop movie slots
// (policy in engine menu/menu_video.h — the three slots, the STARTUP/strips
// draw gate, expansion-first resolution) and draws each visible slot's
// current frame as one stretched quad in the fixed 800x600 design space,
// anamorphically scaled and int-truncated like every widget. It sits UNDER
// the MenuFrame surface, so the movies show through the regions the authored
// custom appearances leave unpainted.
//
// The witnessed selection picks the .bik; playback uses that movie's
// converted `.ogv` sibling (produced by `onimport menu-movies` — Godot has
// no Bink decoder). A selected movie without a converted sibling stays
// empty, exactly like retail's silent missing-file skip, and is counted for
// diagnostics. Hidden slots keep playing (decode continues; only the draw is
// gated), and loop by rewinding on finish.
class MenuVideoUnderlay : public Control {
	GDCLASS(MenuVideoUnderlay, Control)

public:
	// Resolve + (re)create the slot players from the resource root
	// directory and mounted expansion folder name ("" = none). The
	// witnessed lifecycle re-creates on every menu-mode enter.
	void set_source(const String &p_root_dir, const String &p_expansion);
	// The active screen name drives the draw gate (STARTUP -> center movie;
	// anything else -> header+footer strips).
	void set_screen(const String &p_screen_name);
	// Close every slot (menu-mode exit; movies never tick in-game).
	void stop();

	int get_active_slot_count() const;
	int get_unconverted_count() const;
	bool is_startup_layout() const;
	// The converted source an active slot plays (root-relative), "" when the
	// slot is empty — the witnessed expansion-first pick, observable.
	String get_slot_source(int p_slot) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Slot {
		VideoStreamPlayer *player = nullptr;
		String source;
	};

	void draw_slots_();
	void on_slot_finished_(int p_index);

	std::array<Slot, opennova::menu::kMenuVideoSlotCount> slots_{};
	int unconverted_ = 0;
	bool startup_ = true;
};

}  // namespace godot
