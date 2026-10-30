#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/hud/loading_screen.h>

namespace godot {

class LoadingScreenInfo;

// The loading screen's model (engine hud::LoadingScreenModel, runtime/hud/loading_screen.h
// carries the witnesses): the session block's four strings, the band edge, the exact
// checkpoint and the present cadence. The LoadingScreen control holds one per load and does the
// device work it names (the texture loads, the composite, the forced draw).
class LoadingScreenModel : public RefCounted {
	GDCLASS(LoadingScreenModel, RefCounted)

	opennova::hud::LoadingScreenModel model_;

protected:
	static void _bind_methods();

public:
	void setup(const Ref<LoadingScreenInfo> &p_info, bool p_custom_background,
			bool p_background_loaded);
	void adopt_background(bool p_custom_background);
	void update_session_info(const Ref<LoadingScreenInfo> &p_info);
	void set_progress(int p_percent) { model_.set_progress(p_percent); }
	int get_progress() const { return model_.progress(); }
	// `p_now_ms` is the process's GetTickCount reading (GameWorld.tick_count_ms), its low 32 bits.
	bool present_due(int64_t p_now_ms, bool p_force) {
		return model_.present_due(static_cast<uint32_t>(p_now_ms), p_force);
	}
	bool is_in_session() const { return model_.in_session(); }
	bool has_custom_background() const { return model_.custom_background(); }
	int get_band_right() const { return model_.band_right(); }
	// The block's strings in draw order: the server name, the mission, the game type's line, the
	// server message.
	PackedStringArray session_overlay_lines() const;
	// The server message's label and the splash's continue line, off the registered gametext.
	static String server_message_label();
	static String splash_continue_text();
};

} // namespace godot
