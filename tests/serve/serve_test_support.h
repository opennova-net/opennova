// What the opennova-serve tests share: the synthetic deathmatch mission a
// loose game directory serves, a free loopback port, and the scoped working
// directory every server run happens in (the server reads and writes game.cfg
// and activesrvr.txt in the process's working directory, ADR 0051 d2).
#pragma once

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>

#include "common/synthetic_mission.h"
#include "net_sockets.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace serve_test {

namespace fs = std::filesystem;

inline bool write_bytes(const fs::path &path, const std::vector<uint8_t> &bytes) {
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return static_cast<bool>(out);
}

inline bool write_text(const fs::path &path, const std::string &text) {
	std::ofstream out(path, std::ios::binary);
	out << text;
	return static_cast<bool>(out);
}

inline std::string read_text(const fs::path &path) {
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// A deathmatch mission: the two placed entities every kernel test boots, and
// the solo start marker (6002) a deathmatch player spawns at.
inline std::vector<uint8_t> deathmatch_mission(const char *name = "Serve Test Map") {
	using namespace opennova;
	bms::File m = test_mission::two_entity_mission();
	m.header.magic[0] = 'B';
	m.header.magic[1] = 'M';
	m.header.magic[2] = 'S';
	m.header.magic[3] = static_cast<char>(bms::kMinVersion);
	std::snprintf(m.header.mission_name, sizeof(m.header.mission_name), "%s", name);
	m.header.attrib_flags = bms::AttribFlags::Deathmatch;
	bms::Entity marker{};
	marker.type = bms::ItemType::Marker;
	marker.type_id = 6002;
	marker.x = 40 << 16;
	marker.y = 40 << 16;
	marker.id = 41;
	m.items.push_back(marker);
	mission::sync_counts(m);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!bms::write(m, bytes, error)) std::printf("bms::write: %s\n", error.c_str());
	return bytes;
}

inline uint16_t free_udp_port() {
	uint16_t port = 0;
	opennova::net::ScopedSocket probe(opennova::net::udp_bind(0, &port));
	return probe.is_valid() ? port : 0;
}

// A fresh directory under the system temp dir.
inline fs::path fresh_dir(const std::string &tag) {
	const fs::path dir = fs::temp_directory_path() /
			("opennova_serve_" + tag + "_" +
					std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(dir);
	return dir;
}

// The process's working directory moved for a scope and restored after it.
class ScopedCwd {
public:
	explicit ScopedCwd(const fs::path &dir) : saved_(fs::current_path()) { fs::current_path(dir); }
	~ScopedCwd() { restore(); }
	ScopedCwd(const ScopedCwd &) = delete;
	ScopedCwd &operator=(const ScopedCwd &) = delete;
	// Back to the saved directory now (a directory in use cannot be removed).
	void restore() {
		if (restored_) return;
		restored_ = true;
		std::error_code ec;
		fs::current_path(saved_, ec);
	}

private:
	fs::path saved_;
	bool restored_ = false;
};

} // namespace serve_test
