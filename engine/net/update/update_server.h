#pragma once

// The update server's side of the updater's wire: one connection's requests in, its replies
// out, as UPDATE.EXE's gateway and update server must answer them (docs/update/updater-re.md
// §3-§5). One machine plays both roles: a gateway login, the server list, then the update
// server's login, the command file's CRC and the files. Socket I/O stays in the embedder,
// which feeds each read's bytes in and sends what comes back (as net/admin/admin_server.h).

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace opennova::update {

// A file a GET reply sends: its size, then its bytes in order, read as the reply goes out, so
// a file the server serves from disk is never held whole. Service plumbing, not a port.
class UpdateFile {
public:
	virtual ~UpdateFile() = default;
	virtual uint64_t size() const = 0;
	// The next `count` bytes into `out`; false when they do not read.
	virtual bool read(uint8_t *out, size_t count) = 0;

	// A file held in memory.
	static std::unique_ptr<UpdateFile> from_bytes(std::vector<uint8_t> bytes);
	// The file at the UTF-8 `path`, read from disk as it goes out; null when it does not open.
	static std::unique_ptr<UpdateFile> from_disk(const std::string &path);
};

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
	// The same file opened for a GET reply; null when the product has none of that name. The
	// default reads it whole through read_file (a catalog that holds its files in memory); a
	// catalog serving files from disk opens it there (UpdateFile::from_disk).
	virtual std::unique_ptr<UpdateFile> open_file(
			std::string_view login, std::string_view name) const;
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
		bool more = false; // replies wait past this call's kSendBudget: once `send` went
						   // out, receive(connection, nullptr, 0) answers the next
	};
	// The bytes one read brought (none to go on): the complete requests answered in order,
	// until what this call returns reaches kSendBudget (a file's body is cut there, the rest
	// sent by the next call; a reply begun under the budget, a GET header or a small reply,
	// may end past it by its own size, at most a few hundred bytes), so one connection's
	// replies are made as fast as its client takes them and no faster. The bytes on the
	// wire are the same however they are cut.
	Received receive(uint32_t connection, const uint8_t *data, size_t size);
	static constexpr size_t kSendBudget = 64 * 1024;

	void close(uint32_t connection);
	size_t connection_count() const { return connections_.size(); }

private:
	struct Connection {
		std::string buffer;
		std::string login; // the product logged in, empty before
		// The GET being answered: its id and names (the next at `next_name`), and the body
		// going out (`body_left` bytes of `body` still to send).
		int32_t get_id = 0;
		std::vector<std::string> get_names;
		size_t next_name = 0;
		std::unique_ptr<UpdateFile> body;
		uint64_t body_left = 0;
	};

	const UpdateCatalog &catalog_;
	std::map<uint32_t, Connection> connections_;
	uint32_t next_ = 1;
};

} // namespace opennova::update
