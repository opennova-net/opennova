#include <runtime/inmatch/special_key_chain.h>

#include <runtime/hud/hud_scoreboard.h> // scoreboard_takes_page_keys
#include <runtime/inmatch/role_feeds.h> // RoleView, round_over_key, deploy_key_pick
#include <runtime/inmatch/session.h>    // Role, RoleKind
#include <runtime/mission/mission_kernel.h>

namespace opennova::inmatch {

namespace {

constexpr int kVkPrior = 0x21; // PgUp
constexpr int kVkNext = 0x22;  // PgDn

} // namespace

SpecialKeyOutcome special_key_chain(const RoleView &view, Role &role, hud::HudToggleState &toggles,
		controls::PlayerActions &actions, const world::SpawnZoneRegistry &zones,
		const SpecialKeyInput &input) {
	SpecialKeyOutcome out;
	if (view.kernel == nullptr) return out;
	const bool in_session = view.kernel->world.rules.mp_session; // g_NapiNPCtx.is_in_session
	const int vk = input.vk;
	const bool page_key = vk == kVkPrior || vk == kVkNext;

	// The quit dialog leads: its YES, NO and RESTART keys and its digits are taken; any other
	// key it leaves goes to the tail, past every exclusive arm.
	// [orig: Input_HandleSpecialKeys @0x49c5c0 - @0x49c5e6..0x49c6d1; the RESTART's
	//  Input_QueueEvent(12, 0) @0x49c658]
	hud::HudQuitDialogKeyInput quit;
	quit.vk = vk;
	quit.yes_vk = input.yes_vk;
	quit.no_vk = input.no_vk;
	quit.restart_vk = input.restart_vk;
	quit.in_session = in_session;
	const uint32_t dialog = hud::hud_toggles_quit_dialog_key(toggles, quit);
	out.quit_confirmed = (dialog & hud::hud_special_key::kQuitConfirmed) != 0;
	if ((dialog & hud::hud_special_key::kRestartQueued) != 0) out.pick = 0;
	if ((dialog & hud::hud_special_key::kConsumed) != 0) {
		out.taken = true;
		return out;
	}

	if ((dialog & hud::hud_special_key::kChainTaken) == 0) {
		if (actions.use_held() || toggles.emotes_menu_open || toggles.radio_menu_open) {
			// The held-USE seat arm, else the Emotes menu's, else the Radio menu's: a digit is
			// taken (a menu's pick closes that menu), a non-digit goes to the tail.
			// [orig: @0x49c6d8..0x49c7c8; the menus' clears @0x49c75c / @0x49c7a8]
			const controls::ChainDigitTake take =
					actions.chain_digit_key(vk, toggles.emotes_menu_open, toggles.radio_menu_open);
			if (take.taken) {
				out.taken = true;
				out.digit_pick = take.pick;
				if (take.pick && (take.pick->action == controls::PlayerAction::EmotePick ||
								take.pick->action == controls::PlayerAction::RadioPick))
					hud::hud_toggles_close_voice_menu(toggles,
							take.pick->action == controls::PlayerAction::RadioPick);
				return out;
			}
		} else {
			// The round-over arm (the world's and the role's halves, round_over_key): out of a
			// session or in co-op every key is taken, RESTART queueing event 12 (out of a
			// session behind the splash re-run); a session's board arm takes PgUp / PgDn and
			// sends every other key to the tail.
			// [orig: @0x49c7cf..0x49c864 - the splash @0x49c883..0x49c899,
			//  Input_QueueEvent(12, 0) @0x49c8a3]
			const uint32_t over = round_over_key(view, role, vk, input.restart_vk);
			if ((over & hud::hud_round_over::kArm) != 0) {
				if ((over & hud::hud_round_over::kRestart) != 0) {
					out.restart_splash = !in_session;
					out.pick = 0;
				}
				if ((over & hud::hud_round_over::kConsumed) != 0) {
					out.taken = true;
					return out;
				}
			} else if (in_session) {
				// The Tab board's page keys, then the status page's.
				// [orig: @0x49c903..0x49c95f, @0x49c960..0x49c9c7]
				if (page_key && hud::scoreboard_takes_page_keys(in_session, toggles.scoreboard_open)) {
					out.taken = true;
					out.page = SpecialKeyPage::Scoreboard;
					out.page_forward = vk == kVkNext;
					return out;
				}
				const RoleKind kind = role.kind();
				if (hud::hud_toggles_server_status_page_key(toggles, vk, in_session,
							kind != RoleKind::Joiner, kind != RoleKind::DedicatedHost)) {
					out.taken = true;
					return out;
				}
			}
		}
	}

	// The tail: the deploy keys, then the help page's PgUp / PgDn, else the briefing's.
	// [orig: @0x49c9c9..0x49ca97 (Input_QueueEvent(12, ...) @0x49c658 / @0x49ca14 /
	//  @0x49ca73); @0x49ca9e..0x49cb53]
	const int param = deploy_key_pick(view, zones, vk);
	if (param >= 0) {
		out.taken = true;
		out.pick = param;
		return out;
	}
	if (page_key && (toggles.help_open || (toggles.briefing_mode & hud::kBriefingModeOpen) != 0)) {
		out.taken = true;
		out.page = toggles.help_open ? SpecialKeyPage::Help : SpecialKeyPage::Briefing;
		out.page_forward = vk == kVkNext;
	}
	return out;
}

} // namespace opennova::inmatch
