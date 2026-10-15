#pragma once

// The update server's side of the updater's wire: one connection's requests in, its replies
// out, as UPDATE.EXE's gateway and update server must answer them (docs/update/updater-re.md
// §3-§5). One machine plays both roles: a gateway login, the server list, then the update
// server's login, the command file's CRC and the files. Socket I/O stays in the embedder,
// which feeds each read's bytes in and sends what comes back (as net/admin/admin_server.h).

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::update {

// What the server serves, by product login (JOTRus, JOEus, ...).
class UpdateCatalog {
public:
	virtual ~UpdateCatalog() = default;
	// True when `login` names a product; a login reply refuses any other.
	virtual bool has_product(std::string_view login) const = 0;
	// The hosts SERVER.LST lists for the product, the names its client reconnects to
	// (Cupdate2Dlg_PickBestServer copies the chosen name; a retail install resolves it through
	// the Switcher's hosts block). One host means the client never load-tests.
	virtual std::vector<std::string> server_hosts(std::string_view login) const = 0;
	// The product's command file name (JOPTR.CMD), whose CRC a CRC request asks for.
	virtual std::string command_file_name(std::string_view login) const = 0;
	// A file the product serves by name (its command file, a row's blob), case-insensitively;
	// false when it has none of that name.
	virtual bool read_file(
			std::string_view login, std::string_view name, std::vector<uint8_t> &out) const = 0;
};

class UpdateServer {
public:
	explicit UpdateServer(const UpdateCatalog &catalog) : catalog_(catalog) {}

	// A new connection's id.
	uint32_t open();

	struct Received {
		std::vector<uint8_t> send; // the replies to send, in order
		bool keep_open = true; // false after CLOSE, a malformed request, or a request
							   // the connection may not make
	};
	// The bytes one read brought: every complete request answered.
	Received receive(uint32_t connection, const uint8_t *data, size_t size);

	void close(uint32_t connection);
	size_t connection_count() const { return connections_.size(); }

private:
	struct Connection {
		std::string buffer;
		std::string login; // the product logged in, empty before
	};

	const UpdateCatalog &catalog_;
	std::map<uint32_t, Connection> connections_;
	uint32_t next_ = 1;
};

} // namespace opennova::update
