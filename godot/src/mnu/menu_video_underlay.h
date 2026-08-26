#pragma once

#include <bink/bink.h>

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <menu/menu_video.h>

#include <array>
#include <memory>

namespace godot {

// The menu backdrop device leg: plays the witnessed backdrop movie slots
// (policy in engine menu/menu_video.h — the three slots, the STARTUP/strips
// draw gate, expansion-first resolution) and draws each visible slot's
// current frame as one stretched quad in the fixed 800x600 design space,
// anamorphically scaled and int-truncated like every widget. It sits UNDER
// the MenuFrame surface, so the movies show through the regions the authored
// custom appearances leave unpainted. An invisible menu suspends decoding and
// texture uploads while retaining the slots for an eventual return.
//
// The witnessed selection picks and plays the .bik directly through the
// engine's portable BIKi decoder. A selected movie that cannot be opened or
// decoded stays empty and is counted for diagnostics. Hidden slots keep
// decoding (only the draw is gated), and loop by rewinding on finish.
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
	int get_failed_count() const;
	bool is_startup_layout() const;
	// F3 Stats ownership: disabled in ordinary play, so decode carries no
	// profiling clocks. The shell consumes the previous process sample once.
	void set_runtime_profiling_enabled(bool p_enabled);
	bool is_runtime_profiling_enabled() const { return runtime_profiling_enabled_; }
	int64_t consume_process_us();
	// The native .bik source an active slot plays (root-relative), "" when
	// the slot is empty — the expansion-first pick, observable.
	String get_slot_source(int p_slot) const;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	struct Slot {
		Ref<FileAccess> file;
		std::unique_ptr<opennova::bink::BinkMovie> movie;
		Ref<ImageTexture> texture;
		String source;
		double elapsed_seconds = 0.0;
	};

	void draw_slots_();
	bool upload_frame_(Slot &p_slot);
	bool advance_slot_(Slot &p_slot, double p_delta);
	void fail_slot_(Slot &p_slot);
	void update_process_state_();

	std::array<Slot, opennova::menu::kMenuVideoSlotCount> slots_{};
	int failed_ = 0;
	bool startup_ = true;
	bool runtime_profiling_enabled_ = false;
	uint64_t last_process_us_ = 0;
};

}  // namespace godot
