#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/gdvirtual.gen.inc>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "network/connection_error.h"
#include "network/join_target.h"

#include <runtime/inmatch/join_flow.h>

namespace godot {

// The front end's join flow (engine inmatch::JoinFlow: the enumerate, the choice, the dial, state
// 9, the launch flags' sessions, g_ReturnToNovaWorldMenu) over the game shell's device. The flow
// decides; the shell implements its device through the hooks below (a browse's LanSession, the
// join screen, the mount, the loads). The hooks are the engine's JoinFlowDevice, one each; a
// target crosses as the JoinTarget the join was asked for, its record the flow's.
class JoinFlow : public Node {
	GDCLASS(JoinFlow, Node)

protected:
	static void _bind_methods();

	GDVIRTUAL2R(bool, _begin_browse, Ref<JoinTarget>, int)
	GDVIRTUAL1(_adopt_browse, Ref<JoinTarget>)
	GDVIRTUAL0(_end_browse)
	GDVIRTUAL0(_drain_browse)
	GDVIRTUAL0(_drain_enumerator)
	GDVIRTUAL0R(bool, _show_enumerating)
	GDVIRTUAL1R(bool, _show_choice, int)
	GDVIRTUAL1R(bool, _show_refusal, String)
	GDVIRTUAL0(_close_join_screen)
	GDVIRTUAL0R(String, _callsign)
	GDVIRTUAL1R(String, _switch_expansion, String)
	GDVIRTUAL0(_save_profile)
	GDVIRTUAL0(_reload_profile)
	GDVIRTUAL0(_save_config)
	GDVIRTUAL1(_start_join, Ref<JoinTarget>)
	GDVIRTUAL6(_start_launch_host, String, PackedStringArray, int, int, int, int)
	GDVIRTUAL0(_cancel_pre_game)
	GDVIRTUAL0(_show_main_menu)
	GDVIRTUAL1(_route_novaworld_session, bool)

public:
	JoinFlow() { device_.owner = this; }

	// "Join this server" (inmatch::JoinFlow::join).
	void join(const Ref<JoinTarget> &p_target);
	// A LAN row refused before its dial: the join screen shows `text` with Cancel only.
	void refuse(const String &p_text);
	// The NovaWorld page's resolved join of listed server `rid`.
	void join_from_novaworld(const Ref<JoinTarget> &p_target, int64_t p_rid);
	void host_from_novaworld() { flow_.host_from_novaworld(); }
	// The launch flags' session (LaunchFlags); false when they name none.
	bool launch_from_flags();
	// A browse's rows (LanServerRow each) and its window's end, as `serial`'s.
	void browse_rows(const Array &p_rows, int p_serial);
	void browse_finished(int p_serial);
	void commit_choice(
			const String &p_game_password, bool p_spectate, const String &p_team_password);
	// ABORT_CANCEL / BACK / Cancel.
	void cancel();
	// The listed NovaWorld server a failed join's error refuses this install for, 0 for none.
	int64_t join_failed(const Ref<ConnectionError> &p_error);
	bool take_return_to_novaworld() { return flow_.take_return_to_novaworld(); }
	void release_novaworld() { flow_.release_novaworld(); }
	// The join choice's panels are up for a target (the test and probe read).
	bool is_join_choice_pending() const { return flow_.choice_pending(); }

private:
	struct Device final : opennova::inmatch::JoinFlowDevice {
		JoinFlow *owner = nullptr;
		bool begin_browse(
				const opennova::inmatch::JoinTargetRecord *target, uint32_t serial) override;
		void adopt_browse() override;
		void end_browse() override;
		void drain_browse() override;
		void drain_enumerator() override;
		bool show_enumerating() override;
		bool show_choice(uint32_t server_flags) override;
		bool show_refusal(const std::string &text) override;
		void close_join_screen() override;
		std::string callsign() override;
		std::string switch_expansion(const std::string &expansion) override;
		void save_profile() override;
		void reload_profile() override;
		void save_config() override;
		void start_join(const opennova::inmatch::JoinTargetRecord &target) override;
		void start_launch_host(const opennova::inmatch::LaunchHost &host) override;
		void destroy_pre_dial_session(bool novaworld) override;
		void cancel_pre_game() override;
		void show_main_menu() override;
		void route_novaworld_session(bool reenter) override;
	};

	// The JoinTarget the flow's target crosses as, its record the flow's at each crossing.
	const Ref<JoinTarget> &current_target();
	// The flow back in Idle: no join stands in front of the dial, so the target is the joiner
	// load's (bound into its callable) or abandoned, and this node lets it go.
	void settle_target();

	Device device_{};
	opennova::inmatch::JoinFlow flow_{ device_ };
	Ref<JoinTarget> current_;
};

} // namespace godot
