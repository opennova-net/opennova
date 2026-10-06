#pragma once

#include "admin_feed.h"
#include "listing.h"
#include "listing_source.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace opennova::nw_lister {

// opennova-nw-lister's source: the listing JSON, re-read when it changes, and with an admin host
// the game server's remote-admin port, whose players, map and time left replace the file's. No
// game stands behind it, so the session's ServerCommands are logged and dropped.
struct FileListingOptions {
	std::string listing_path;
	bool allow_public = false;  // the destination policy for the admin host
	std::string admin_host;     // empty: the listing file alone
	uint16_t admin_port = kAdminDefaultPort;
	std::string admin_user;
	std::string admin_pass;
};

class FileListingSource final : public ListingSource {
public:
	explicit FileListingSource(FileListingOptions options) : options_(std::move(options)) {}
	~FileListingSource() override { admin_.stop(); }

	bool start(int &exit_code) override;
	bool refresh(uint32_t now_ms, bool force) override;
	HostRegistration registration() const override;
	std::vector<HostPlayerSlot> wanted_roster(const std::map<int, HostPlayerSlot> &current) const override;
	void on_command(const ServerCommand &command) override;
	void stop() override { admin_.stop(); }

private:
	FileListingOptions options_;
	Listing listing_;
	std::filesystem::file_time_type listing_mtime_{};
	uint32_t listing_checked_ms_ = 0;
	AdminFeed admin_;
	AdminSnapshot admin_snapshot_;
};

} // namespace opennova::nw_lister
