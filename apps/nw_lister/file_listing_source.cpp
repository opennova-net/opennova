#include "file_listing_source.h"

#include "policy.h"

#include <base/io/log.h>

#include <set>
#include <system_error>

namespace opennova::nw_lister {

using io::LogLevel;

namespace {

constexpr uint32_t kListingCheckMs = 2000;

} // namespace

bool FileListingSource::start(int &exit_code) {
	std::string error;
	if (!load_listing(options_.listing_path, listing_, error)) {
		io::logf(LogLevel::kError, "[listing] %s", error.c_str());
		exit_code = kExitBadInput;
		return false;
	}
	std::error_code ec;
	listing_mtime_ = std::filesystem::last_write_time(options_.listing_path, ec);
	if (!options_.admin_host.empty()) {
		net::Endpoint server;
		server.port = options_.admin_port;
		if (!resolve_destination(options_.admin_host, server, options_.allow_public, "admin server")) {
			exit_code = kExitNetwork;
			return false;
		}
		admin_.start(server, options_.admin_user, options_.admin_pass);
		io::logf(LogLevel::kInfo, "[admin] reading %s every %d s for the players, map and time left",
		         net::endpoint_to_string(server).c_str(), kAdminPollSeconds);
	}
	return true;
}

// The listing file, re-read when it changed, and the admin port's latest answer over it.
bool FileListingSource::refresh(uint32_t now_ms, bool force) {
	if (!force && now_ms - listing_checked_ms_ < kListingCheckMs) return false;
	listing_checked_ms_ = now_ms;
	bool changed = false;
	std::error_code ec;
	const auto mtime = std::filesystem::last_write_time(options_.listing_path, ec);
	if (!ec && mtime != listing_mtime_) {
		Listing fresh;
		std::string error;
		if (load_listing(options_.listing_path, fresh, error)) {
			listing_ = std::move(fresh);
			changed = true;
			io::logf(LogLevel::kInfo, "[listing] reloaded: '%s' on %s", listing_.columns.server_name.c_str(),
			         listing_.columns.mission_name.c_str());
		} else {
			io::logf(LogLevel::kWarn, "[listing] %s (keeping the previous listing)", error.c_str());
		}
		listing_mtime_ = mtime;
	}
	if (!options_.admin_host.empty()) {
		const AdminSnapshot snapshot = admin_.snapshot();
		if (snapshot.seq != admin_snapshot_.seq) {
			changed = true;
			if (snapshot.ok) {
				io::logf(LogLevel::kInfo, "[admin] %zu player(s) on %s, %d min left", snapshot.players.size(),
				         snapshot.mission.c_str(), snapshot.time_left_minutes);
			} else {
				io::logf(LogLevel::kWarn, "[admin] the server is not answering (%s): listing no players",
				         snapshot.status.c_str());
			}
			admin_snapshot_ = snapshot;
		}
	}
	return changed;
}

// The listing columns; the admin port's map and time left replace the file's.
HostRegistration FileListingSource::registration() const {
	HostRegistration r = listing_.columns;
	if (admin_snapshot_.seq != 0 && admin_snapshot_.ok) {
		if (!admin_snapshot_.mission.empty()) r.mission_name = admin_snapshot_.mission;
		const int minutes = admin_snapshot_.time_left_minutes;
		r.round_time_remaining_ticks = minutes < 0 ? -1 : minutes * 3720 + 3719;
	}
	return r;
}

// The roster the PlayerList should carry: the admin port's players (their server slots) once it
// answered, else the file's, where a player without a slot keeps the one it has and a new name
// takes the lowest free one, so one player's change never moves another.
std::vector<HostPlayerSlot> FileListingSource::wanted_roster(const std::map<int, HostPlayerSlot> &current) const {
	std::vector<HostPlayerSlot> out;
	if (admin_snapshot_.seq != 0) {
		if (!admin_snapshot_.ok) return out;
		for (const AdminPlayer &player : admin_snapshot_.players) {
			HostPlayerSlot slot;
			slot.slot = player.slot;
			slot.player_name = player.name;
			slot.team = player.team;
			slot.type = "0";
			out.push_back(std::move(slot));
		}
		return out;
	}
	std::set<int> taken;
	for (const HostPlayerSlot &player : listing_.players) {
		if (player.slot >= 0) {
			out.push_back(player);
			taken.insert(player.slot);
		}
	}
	for (HostPlayerSlot player : listing_.players) {
		if (player.slot >= 0) continue;
		for (const auto &entry : current) {
			if (entry.second.player_name == player.player_name && taken.count(entry.first) == 0) {
				player.slot = entry.first;
				break;
			}
		}
		if (player.slot < 0) {
			int free_slot = 0;
			while (taken.count(free_slot) != 0) ++free_slot;
			player.slot = free_slot;
		}
		taken.insert(player.slot);
		out.push_back(std::move(player));
	}
	return out;
}

void FileListingSource::on_command(const ServerCommand &command) {
	io::logf(LogLevel::kInfo, "[host] ServerCommand %s ignored: no game behind this listing",
	         server_command_verb_name(command.verb));
}

} // namespace opennova::nw_lister
