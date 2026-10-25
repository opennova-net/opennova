#pragma once

#include <runtime/inmatch/pre_game_menu.h>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class RtxtStringFile;

// One read of the join screen (engine inmatch/pre_game_menu.h): where the
// join stands, the host's queue record and the joiner's clock at the read, so
// the shell's PRE_GAME_MENU presenter writes the same status line retail's
// join machine does. The panel-window table rides here as statics.
class JoinScreenStatus : public RefCounted {
	GDCLASS(JoinScreenStatus, RefCounted)

	opennova::inmatch::JoinScreenStage stage_ = opennova::inmatch::JoinScreenStage::Joining;
	opennova::inmatch::JoinQueueRecord queue_;
	uint64_t now_ms_ = 0;

protected:
	static void _bind_methods();

public:
	enum Stage {
		STAGE_JOINING = static_cast<int>(opennova::inmatch::JoinScreenStage::Joining),
		STAGE_CONNECTING = static_cast<int>(opennova::inmatch::JoinScreenStage::Connecting),
		STAGE_VERIFYING = static_cast<int>(opennova::inmatch::JoinScreenStage::Verifying),
		STAGE_QUEUED = static_cast<int>(opennova::inmatch::JoinScreenStage::Queued),
		STAGE_STARTING = static_cast<int>(opennova::inmatch::JoinScreenStage::Starting),
		STAGE_ENUMERATING = static_cast<int>(opennova::inmatch::JoinScreenStage::Enumerating),
	};
	enum Panel {
		PANEL_ERROR = static_cast<int>(opennova::inmatch::PreGamePanel::Error),
		PANEL_PROGRESS = static_cast<int>(opennova::inmatch::PreGamePanel::Progress),
		PANEL_GAME_PASSWORD = static_cast<int>(opennova::inmatch::PreGamePanel::GamePassword),
		PANEL_SPECTATE = static_cast<int>(opennova::inmatch::PreGamePanel::Spectate),
		PANEL_SPECTATOR_PASSWORD =
				static_cast<int>(opennova::inmatch::PreGamePanel::SpectatorPassword),
		PANEL_TEAM_PASSWORD = static_cast<int>(opennova::inmatch::PreGamePanel::TeamPassword),
	};

	static Ref<JoinScreenStatus> make(opennova::inmatch::JoinScreenStage p_stage,
			const opennova::inmatch::JoinQueueRecord &p_queue, uint64_t p_now_ms);

	int get_stage() const { return static_cast<int>(stage_); }
	// The MESSAGES line for this read (inmatch::join_screen_text).
	String message_text(const Ref<RtxtStringFile> &p_override_table,
			const Ref<RtxtStringFile> &p_gameerr) const;

	// The MESSAGES line of a stage that has no queue record, read before any
	// connection exists (the enumerate-and-join's PRE_ENUMSESSION).
	static String stage_text(int p_stage, const Ref<RtxtStringFile> &p_override_table,
			const Ref<RtxtStringFile> &p_gameerr);

	// pre.mnu's panel windows: how many, each one's name, and whether a panel
	// state shows it (inmatch::pre_game_windows).
	static int window_count();
	static String window_name(int p_index);
	static bool panel_shows_window(int p_panel, int p_index);

	// The choice panels (inmatch::pre_game_entry_panel / pre_game_accept): the panel a
	// discovered flag word opens on (PANEL_PROGRESS = no choice), and ACCEPT's step on a
	// panel: the next panel, or ACCEPT_COMMIT, ACCEPT_ABORT, ACCEPT_NONE.
	enum AcceptStep {
		ACCEPT_COMMIT = -100,
		ACCEPT_ABORT = -101,
		ACCEPT_NONE = -102,
	};
	static int entry_panel(int p_server_flags, bool p_authority);
	static int accept_step(int p_panel, int p_server_flags, bool p_spectate_checked,
			bool p_authority);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::JoinScreenStatus::Stage);
VARIANT_ENUM_CAST(godot::JoinScreenStatus::Panel);
VARIANT_ENUM_CAST(godot::JoinScreenStatus::AcceptStep);
