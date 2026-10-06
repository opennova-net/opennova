#include <net/admin/admin_server.h>

#include <net/admin/admin_protocol.h>

#include <base/io/le.h>
#include <base/io/strutil.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace opennova {

namespace {

constexpr uint32_t kPacketMarker = 0x0A0D0000u;

std::vector<uint8_t> reply_packet(const std::string &text) {
	std::vector<uint8_t> payload(text.begin(), text.end());
	payload.push_back(0);
	return admin_encode_packet(payload.data(), payload.size());
}

// The C string at `p` inside a buffer that ends at `end` (retail's reads stop at a NUL; this
// one stops at the buffer's end too).
std::string c_string(const uint8_t *p, const uint8_t *end) {
	const uint8_t *nul = std::find(p, end, uint8_t{0});
	return std::string(reinterpret_cast<const char *>(p), static_cast<size_t>(nul - p));
}

std::string hex_dword(uint32_t value) {
	char text[16];
	std::snprintf(text, sizeof(text), "%08x", value);
	return text;
}

} // namespace

AdminServer::AdminServer(admincfg::AdminConfig config, AdminCommandHandler &handler, io::CrtRand &rand,
		LogSink log)
		: config_(std::move(config)), handler_(handler), rand_(rand), log_(std::move(log)) {}

AdminServer::Connection *AdminServer::find(uint32_t connection) {
	for (Connection &c : connections_)
		if (c.id == connection) return &c;
	return nullptr;
}

const AdminServer::Connection *AdminServer::find(uint32_t connection) const {
	for (const Connection &c : connections_)
		if (c.id == connection) return &c;
	return nullptr;
}

void AdminServer::log(const std::string &line) const {
	if (log_) log_(line);
}

// [orig: CAdminServer_AcceptConnection @0x405580 — the whitelist @0x4055BF..0x40567C and its
//  blocked log @0x40567C (the socket closed @0x4056AD); the new slot zeroed with a 1024-byte
//  buffer @0x405731..0x40577B; 32 draws of `rand() % 255 + 1` @0x405783..0x4057AC, the NUL
//  @0x4057C1, byte 0 forced to 1 @0x4057D5; the accepted log @0x4057F2; the 41-byte packet,
//  marker and length @0x40581F..0x405825, the 33 challenge bytes @0x40582C, sent @0x405848]
AdminServer::Accepted AdminServer::accept(uint32_t source_ip) {
	Accepted out;
	if (!admincfg::admits(config_, source_ip)) {
		log("New connection blocked - invalid IP (" + hex_dword(source_ip) + ")\n");
		return out;
	}
	Connection conn;
	conn.id = next_id_++;
	if (next_id_ == 0) next_id_ = 1;
	for (size_t i = 0; i < 32; ++i)
		conn.challenge[i] = static_cast<uint8_t>(rand_.next() % 255u + 1u);
	conn.challenge[32] = 0;
	conn.challenge[0] = 1;
	log("New connection accepted (" + hex_dword(source_ip) + ")\n");
	out.admitted = true;
	out.connection = conn.id;
	out.send = admin_encode_packet(conn.challenge.data(), conn.challenge.size());
	connections_.push_back(conn);
	return out;
}

size_t AdminServer::receive_room(uint32_t connection) const {
	const Connection *conn = find(connection);
	return conn == nullptr ? 0 : ADMIN_SERVER_BUFFER_BYTES - conn->received;
}

bool AdminServer::authenticated(uint32_t connection) const {
	const Connection *conn = find(connection);
	return conn != nullptr && conn->authenticated;
}

// [orig: CAdminServer_ProcessFrame @0x406f50 — the recv into the buffer's room @0x406f9e, the
//  count advanced @0x406fbc, ProcessClientData @0x406fc7, a 0 closing the slot @0x406fd0]
AdminServer::Received AdminServer::receive(uint32_t connection, const uint8_t *data, size_t size) {
	Received out;
	Connection *conn = find(connection);
	if (conn == nullptr) {
		out.keep_open = false;
		return out;
	}
	const size_t take = std::min(size, ADMIN_SERVER_BUFFER_BYTES - conn->received);
	if (take != 0) std::memcpy(conn->buffer.data() + conn->received, data, take);
	conn->received += take;
	out.keep_open = handle_packet(*conn, out.send);
	if (!out.keep_open) close(connection);
	return out;
}

// [orig: CAdminServer_ProcessClientData @0x406ec0 — fewer than 8 bytes waits (1) @0x406edf; a
//  wrong marker closes (0) @0x406ef6; fewer bytes than the declared length waits @0x406f06;
//  an unauthenticated slot hands the payload to HandleLogin @0x406f19, an authenticated one
//  closes on a last byte that is not NUL @0x406f20..0x406f27 and else dispatches @0x406f32;
//  either way the count goes back to 0 @0x406f3d]
bool AdminServer::handle_packet(Connection &conn, std::vector<std::vector<uint8_t>> &send) {
	if (conn.received < ADMIN_PACKET_HEADER_BYTES) return true;
	if (io::read_u32_le(conn.buffer.data()) != kPacketMarker) return false;
	const uint32_t declared = io::read_u32_le(conn.buffer.data() + 4);
	if (conn.received < declared) return true;
	bool keep = false;
	if (!conn.authenticated) {
		// The unsigned `length - 8`: a length below the header reads as huge and is refused.
		keep = handle_login(conn, static_cast<uint32_t>(declared - ADMIN_PACKET_HEADER_BYTES), send);
	} else if (declared >= ADMIN_PACKET_HEADER_BYTES && conn.buffer[declared - 1] == 0) {
		// A length below the header reads the byte before or inside it and then an unbounded
		// string (D-NET-360); that packet closes here.
		const uint8_t *text = conn.buffer.data() + ADMIN_PACKET_HEADER_BYTES;
		const std::string line = c_string(text, conn.buffer.data() + declared);
		// DispatchCommand's first act. [orig: @0x40676B, "User command (%s) - %s\n"]
		log("User command (" + conn.session.user + ") - " + line + "\n");
		std::vector<std::string> replies;
		keep = handler_.dispatch(conn.session, line, replies);
		for (const std::string &reply : replies) send.push_back(reply_packet(reply));
	}
	conn.received = 0;
	return keep;
}

// [orig: CAdminServer_HandleLogin @0x405870 — more than 65 bytes closes @0x405891; a plaintext
//  QUERY (stricmp) answers HandleStatus and closes @0x405899..0x4058C7; else the decrypt keyed
//  by the challenge @0x4058E2 and the NULs at bytes 31 and 63 @0x4058E7/@0x4058F0; the first
//  user whose stored name is a case-insensitive prefix of the typed one (strnicmp over the
//  stored length) @0x405900..0x405950, then only that user's password, a case-sensitive
//  prefix (strncmp over the stored length) @0x40595F..0x405983; success marks the slot
//  @0x405995..0x4059C0, replies @0x405A0C..0x405A23 and logs @0x405A39; a failure logs
//  @0x4059E3 and closes]
bool AdminServer::handle_login(Connection &conn, size_t data_len, std::vector<std::vector<uint8_t>> &send) {
	if (data_len > ADMIN_LOGIN_BYTES) return false;
	uint8_t *login = conn.buffer.data() + ADMIN_PACKET_HEADER_BYTES;
	uint8_t *const end = conn.buffer.data() + conn.buffer.size();
	if (strutil::iequals(c_string(login, end), "QUERY")) {
		send.push_back(reply_packet(handler_.status_report()));
		return false;
	}
	admin_decrypt_buffer(login, data_len, conn.challenge.data(), 32);
	login[31] = 0;
	login[63] = 0;
	const std::string user = c_string(login, end);
	const std::string password = c_string(login + 32, end);
	for (const admincfg::AdminUser &entry : config_.users) {
		if (!strutil::starts_with_icase(user, entry.name)) continue;
		if (password.compare(0, entry.password.size(), entry.password) == 0) {
			conn.authenticated = true;
			conn.session.rights = entry.rights;
			conn.session.user = entry.name;
		}
		break;
	}
	if (!conn.authenticated) {
		log("User failed log in (" + user + "/" + password + ")\n");
		return false;
	}
	send.push_back(reply_packet("OK - User: " + user + " successfully logged in."));
	log("User logged in (" + user + "/" + password + ")\n");
	return true;
}

// [orig: CAdminServer_DisconnectClient @0x402cb0 — the socket closed, the buffer freed, the
//  later slots shifted down]
void AdminServer::close(uint32_t connection) {
	connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
			                   [&](const Connection &c) { return c.id == connection; }),
			connections_.end());
}

} // namespace opennova
