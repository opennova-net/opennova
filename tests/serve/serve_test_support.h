// What the opennova-serve tests share: the synthetic deathmatch mission a
// loose game directory serves, a free TCP port for the remote admin, and the
// scoped working directory every server run happens in (the server reads and
// writes game.cfg and activesrvr.txt in the process's working directory, ADR
// 0051 d2).
#pragma once

#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/rtxt/rtxt.h>
#include <net/napi/envelope.h>
#include <net/novacrypto/nwu.h>
#include <net/novaworld/gate_probe.h>

#include "common/synthetic_mission.h"
#include "net_sockets.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
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

// A gametext.bin with the given (section, [(key, text)]) rows, written by the
// format's own writer.
inline std::vector<uint8_t> gametext(
		const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> &sections) {
	using namespace opennova;
	rtxt::File file;
	for (const auto &section : sections) {
		const uint32_t index = static_cast<uint32_t>(file.sections.size());
		file.sections.push_back({section.first, static_cast<uint32_t>(section.second.size())});
		for (const auto &row : section.second) {
			rtxt::Entry entry;
			entry.key = row.first;
			entry.text = row.second;
			entry.section_index = index;
			file.entries.push_back(std::move(entry));
		}
	}
	std::vector<uint8_t> bytes;
	std::string error;
	if (!rtxt::write(file, bytes, error)) std::printf("rtxt::write: %s\n", error.c_str());
	return bytes;
}

// A NovaWorld gate on `gate` that answers every probe until `stop`, naming
// 127.0.0.1:`nw_port` (an in-test NwUdpListener) as UDPNOVAWORLD; `met_ext`
// asks for the extended metrics (the gate's METEXT).
inline void serve_gate(opennova::net::Socket &gate, uint16_t nw_port, bool met_ext, std::atomic<bool> &stop) {
	using namespace opennova;
	std::string body = "VAR \"LOBBYNAME\" \"jop_2_consumer\"\r\n"
	                   "VAR \"UDPNOVAWORLD\" \"127.0.0.1:" + std::to_string(nw_port) + "\"\r\n";
	if (met_ext) body += "VAR \"METEXT\" \"1\"\r\n";
	std::vector<uint8_t> inner(body.begin(), body.end());
	nwu_decrypt(inner.data(), inner.size(), GATE_NWU_KEY);
	std::vector<uint8_t> reply(inner.size() + 16);
	std::size_t reply_size = 0;
	napi_envelope_encode(inner.data(), inner.size(), reply.data(), reply.size(), &reply_size);
	reply.resize(reply_size);
	while (!stop) {
		uint8_t rx[1024];
		net::Endpoint from{};
		if (net::udp_recv_from(gate, rx, sizeof(rx), from, 50) <= 0) continue;
		net::udp_send_to(gate, from, reply.data(), reply.size());
	}
}

// A TCP port that was free a moment ago: bound, read back and released, so
// another process can take it before the caller listens. Only game.cfg's
// remote_admin_port needs one: retail's 0 there means no listener, so the
// server cannot be handed port 0 to pick its own, and the caller retries a
// listen that lost the race. Every UDP socket a test serves binds port 0 and
// reads back what it got (--lan-port 0, a LAN server range of 0..0, or a held
// net::ScopedSocket).
inline uint16_t free_tcp_port() {
	uint16_t port = 0;
	opennova::net::ScopedSocket probe(opennova::net::tcp_listen(0, 1, &port));
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
