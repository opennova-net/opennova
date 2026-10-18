#include "admin_link_status.h"

namespace opennova::novaworld_server::hosted {

const char *admin_link_state_name(AdminLinkState state) {
	switch (state) {
		case AdminLinkState::Connecting: return "connecting";
		case AdminLinkState::Up: return "up";
		case AdminLinkState::Down: return "down";
		case AdminLinkState::Stopped: return "stopped";
	}
	return "down";
}

std::optional<AdminLinkState> parse_admin_link_state(std::string_view text) {
	for (const AdminLinkState state : { AdminLinkState::Connecting, AdminLinkState::Up,
				 AdminLinkState::Down, AdminLinkState::Stopped }) {
		if (text == admin_link_state_name(state))
			return state;
	}
	return std::nullopt;
}

std::optional<AdminCommandOutcome> parse_admin_command_outcome(std::string_view text) {
	for (const AdminCommandOutcome outcome :
			{ AdminCommandOutcome::Answered, AdminCommandOutcome::Refused,
					AdminCommandOutcome::LinkDown, AdminCommandOutcome::Busy,
					AdminCommandOutcome::TimedOut, AdminCommandOutcome::Stopped }) {
		if (text == admin_command_outcome_name(outcome))
			return outcome;
	}
	return std::nullopt;
}

const char *admin_command_outcome_name(AdminCommandOutcome outcome) {
	switch (outcome) {
		case AdminCommandOutcome::Answered: return "answered";
		case AdminCommandOutcome::Refused: return "refused";
		case AdminCommandOutcome::LinkDown: return "link_down";
		case AdminCommandOutcome::Busy: return "busy";
		case AdminCommandOutcome::TimedOut: return "timed_out";
		case AdminCommandOutcome::Stopped: return "stopped";
	}
	return "link_down";
}

std::optional<AdminLinkStatus> admin_link_status(db::Database &db, int64_t server_id) {
	const auto rows = db.query(
			"SELECT state, game_state, last_poll_at, up_since_at, error FROM hosted_admin_links "
			"WHERE server_id = $1 AND seen_at > now() - make_interval(secs => $2);",
			{ db::BindValue(server_id), db::BindValue(static_cast<double>(kAdminLinkStaleS)) });
	if (rows.empty())
		return std::nullopt;
	const db::Row &r = rows.front();
	AdminLinkStatus status;
	status.state = parse_admin_link_state(r.as_text(0).value_or("")).value_or(AdminLinkState::Down);
	status.game_state = r.as_text(1).value_or("");
	status.last_poll_at = r.as_text(2).value_or("");
	status.up_since = r.as_text(3).value_or("");
	status.error = r.as_text(4).value_or("");
	return status;
}

} // namespace opennova::novaworld_server::hosted
