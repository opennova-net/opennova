#include <net/admin/admin_protocol.h>

#include <base/io/le.h>
#include <base/io/strutil.h>
#include <net/novacrypto/nwu.h>

#include <algorithm>
#include <cstring>

namespace opennova {

namespace {

constexpr uint32_t kPacketMarker = 0x0A0D0000u;
// A sanity bound on a reply; the server's largest (PLAYER LIST, MISSION AVAILABLE) stay far
// below it.
constexpr uint32_t kReplyMaxBytes = 1u << 20;

std::vector<std::string_view> split_lines(std::string_view text) {
	std::vector<std::string_view> out;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string_view::npos) end = text.size();
		std::string_view line = text.substr(start, end - start);
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		out.push_back(line);
		start = end + 1;
	}
	return out;
}

std::string_view trim_right(std::string_view s) {
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
	return s;
}

} // namespace

std::vector<uint8_t> admin_encode_packet(const uint8_t *payload, size_t size) {
	std::vector<uint8_t> out;
	out.reserve(ADMIN_PACKET_HEADER_BYTES + size);
	io::append_u32_le(out, kPacketMarker);
	io::append_u32_le(out, static_cast<uint32_t>(ADMIN_PACKET_HEADER_BYTES + size));
	if (size != 0) out.insert(out.end(), payload, payload + size);
	return out;
}

// [orig: CAdminServer_ProcessClientData @0x406ef0..0x406f06 (the marker, then the length)]
bool admin_decode_header(const uint8_t *header, size_t &payload_size) {
	if (io::read_u32_le(header) != kPacketMarker) return false;
	const uint32_t total = io::read_u32_le(header + 4);
	if (total < ADMIN_PACKET_HEADER_BYTES || total > kReplyMaxBytes) return false;
	payload_size = total - ADMIN_PACKET_HEADER_BYTES;
	return true;
}

bool admin_challenge_valid(const std::vector<uint8_t> &payload) {
	return payload.size() == ADMIN_CHALLENGE_BYTES && payload[0] == 0x01 && payload.back() == 0;
}

namespace {

// The cipher's three LCG words, keyed by the challenge string.
struct CipherWords {
	uint32_t s1 = 0;
	uint32_t s2 = 0;
	uint32_t s3 = 0;
};

// The seed: Σ(int8(k)² + i) + len + 0x32, then three steps of the 16-bit LCG (the NWU
// multiplier). [orig: Crypto_ComputeSeedFromBuffer @0x437250 (movsx @0x437280,
//  lea eax,[esi+edx+32h] @0x437295); Crypto_EncryptBuffer @0x43753e..0x43755d]
CipherWords cipher_words(const uint8_t *key, size_t key_len) {
	uint32_t seed = 0;
	for (size_t i = 0; i < key_len; ++i) {
		const int32_t v = static_cast<int8_t>(key[i]);
		seed += static_cast<uint32_t>(v * v) + static_cast<uint32_t>(i);
	}
	seed += static_cast<uint32_t>(key_len) + 0x32u;
	const uint32_t m = NWU_LCG_MAGIC;
	CipherWords w;
	w.s1 = (seed * m + 1) & 0xFFFFu;
	w.s2 = (m * w.s1 + 1) & 0xFFFFu;
	w.s3 = (m * w.s2 + 1) & 0xFFFFu;
	return w;
}

} // namespace

// The challenge string keys the cipher; the server decrypts the whole payload with the inverse
// (Crypto_DecryptBuffer @0x4375b0), so the zero byte after the fields is encrypted too.
// [orig: Crypto_EncryptBuffer @0x437510; RAT.exe @0x4012d2..0x401361 (65 bytes, zero-filled)]
std::array<uint8_t, ADMIN_LOGIN_BYTES> admin_encode_login(const std::vector<uint8_t> &challenge,
                                                          std::string_view user, std::string_view password) {
	std::array<uint8_t, ADMIN_LOGIN_BYTES> buf{};
	std::memcpy(buf.data(), user.data(), std::min(user.size(), ADMIN_LOGIN_FIELD_MAX_CHARS));
	std::memcpy(buf.data() + 32, password.data(), std::min(password.size(), ADMIN_LOGIN_FIELD_MAX_CHARS));

	const size_t key_len = static_cast<size_t>(
			std::find(challenge.begin(), challenge.end(), uint8_t{0}) - challenge.begin());
	if (key_len == 0) return buf;
	const uint8_t *key = challenge.data();
	const CipherWords w = cipher_words(key, key_len);
	const uint32_t m = NWU_LCG_MAGIC;

	// The key added cyclically. [orig: Crypto_AddKeyString @0x437170]
	for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<uint8_t>(buf[i] + key[i % key_len]);
	// Reversed when s3 is odd. [orig: Buffer_ReverseInPlace2 @0x437330]
	if ((w.s3 & 1u) != 0) std::reverse(buf.begin(), buf.end());
	// buf[i] += i + add, add stepping by (u8)s2 from (u8)s1. [orig: Crypto_AddProgressiveKey @0x4371c0]
	uint8_t add = static_cast<uint8_t>(w.s1);
	const uint8_t step = static_cast<uint8_t>(w.s2);
	for (size_t i = 0; i < buf.size(); ++i) {
		buf[i] = static_cast<uint8_t>(buf[i] + i + add);
		add = static_cast<uint8_t>(add + step);
	}
	// The LCG stream from s3. [orig: Crypto_PRNGEncrypt @0x437200]
	uint32_t state = w.s3;
	for (uint8_t &b : buf) {
		state = (m * state + 1) & 0xFFFFu;
		b = static_cast<uint8_t>(b + (state & 0xFFu));
	}
	return buf;
}

void admin_decrypt_buffer(uint8_t *buf, size_t size, const uint8_t *key, size_t key_len) {
	if (buf == nullptr || size < 1 || key_len == 0) return; // [orig: @0x4375b4 `length >= 1`]
	const CipherWords w = cipher_words(key, key_len);
	const uint32_t m = NWU_LCG_MAGIC;
	uint32_t state = w.s3;
	for (size_t i = 0; i < size; ++i) {
		state = (m * state + 1) & 0xFFFFu;
		buf[i] = static_cast<uint8_t>(buf[i] - (state & 0xFFu));
	}
	uint8_t add = static_cast<uint8_t>(w.s1);
	for (size_t i = 0; i < size; ++i) {
		buf[i] = static_cast<uint8_t>(buf[i] - i - add);
		add = static_cast<uint8_t>(add + static_cast<uint8_t>(w.s2));
	}
	if ((w.s3 & 1u) != 0) std::reverse(buf, buf + size);
	for (size_t i = 0; i < size; ++i) buf[i] = static_cast<uint8_t>(buf[i] - key[i % key_len]);
}

// [orig: RAT.exe @0x401492..0x4014f3 (strlen + 1 bytes after the header);
//  ProcessClientData @0x406f20..0x406f27 (an authenticated packet must end in its NUL)]
std::vector<uint8_t> admin_encode_command(std::string_view command) {
	std::vector<uint8_t> out(command.begin(), command.end());
	out.push_back(0);
	return out;
}

// Every reply is SendResponse's strcpy, its length counting the NUL.
// [orig: CAdminServer_SendResponse @0x402d40]
bool admin_reply_text(const std::vector<uint8_t> &payload, std::string &out) {
	const auto nul = std::find(payload.begin(), payload.end(), uint8_t{0});
	out.assign(payload.begin(), nul);
	return nul != payload.end();
}

// [orig: HandleLogin @0x405a0c ("OK - User: %s successfully logged in.")]
bool admin_login_accepted(std::string_view reply) {
	return strutil::starts_with_icase(reply, "OK - ") &&
	       reply.find("successfully logged in.") != std::string_view::npos;
}

// The handlers' failures are "ERROR - <text>", a refused or malformed verb "USAGE - [...]".
// [orig: DispatchCommand @0x406a8d..0x406d58 (the usage reply); HandlePlayer @0x403f63
//  ("ERROR - Not in Game State.")]
bool admin_reply_is_error(std::string_view reply) {
	return strutil::starts_with_icase(reply, "ERROR - ") || strutil::starts_with_icase(reply, "USAGE - ");
}

// The header "NAME            \t #\tTEAM\tClass\tKills\tDeaths\tPING\n", then one
// "%-16s\t%2d\t%2d\t%d\t%d\t%d\t%d\n" row per active slot: the name padded (never cut) to 16,
// the slot number, the team. The host's own slot 0 is named by GameText Server/STRSRV19, "Host"
// (a NovaWorld listen host's takes its character name and is a player).
// [orig: CAdminServer_HandlePlayer @0x403f7c (the header), @0x403fe9 (the row);
//  Server_InitNewRoundState @0x51c9c3..0x51ca2a]
std::vector<AdminPlayer> admin_parse_player_list(std::string_view reply) {
	std::vector<AdminPlayer> out;
	for (std::string_view line : split_lines(reply)) {
		const size_t tab1 = line.find('\t');
		if (tab1 == std::string_view::npos) continue;
		const size_t tab2 = line.find('\t', tab1 + 1);
		const size_t tab3 = tab2 == std::string_view::npos ? tab2 : line.find('\t', tab2 + 1);
		if (tab3 == std::string_view::npos) continue;
		const auto slot = strutil::parse_int(std::string(strutil::trim_view(line.substr(tab1 + 1, tab2 - tab1 - 1))));
		if (!slot) continue; // the header row
		AdminPlayer player;
		player.slot = *slot;
		player.name = std::string(trim_right(line.substr(0, tab1)));
		player.team = std::string(strutil::trim_view(line.substr(tab2 + 1, tab3 - tab2 - 1)));
		if (player.name.empty() || (player.slot == 0 && player.name == "Host")) continue;
		out.push_back(std::move(player));
	}
	return out;
}

// One "%d: %s - %s %s %s %s %s\n" line per queue entry: the index, the mission file, then
// "(2x)"|"()", "(IS FLIPPED)"|"()", "(ONE_SHOT)"|"()", "<CURRENT MISSION>"|"<>",
// "<NEXT MISSION>"|"<>"; or "No missions in queue.". A file name can itself hold " - ", so the
// tags are found from the end. [orig: CAdminServer_HandleMissionCommand @0x406447 (the format
//  @0x7c0874), @0x40634f (the empty queue)]
std::string admin_parse_current_mission(std::string_view reply) {
	for (std::string_view line : split_lines(reply)) {
		const size_t colon = line.find(": ");
		const size_t tags = line.rfind(" - (");
		if (colon == std::string_view::npos || tags == std::string_view::npos || tags <= colon + 2) continue;
		if (line.substr(tags).find("<CURRENT MISSION>") == std::string_view::npos) continue;
		std::string_view file = line.substr(colon + 2, tags - colon - 2);
		for (const char *ext : {".bms", ".npj", ".npz"}) {
			if (strutil::ends_with_icase(file, ext)) {
				file.remove_suffix(4);
				break;
			}
		}
		return std::string(file);
	}
	return {};
}

// "GameTime             = %d/%d": the remaining whole minutes (g_RoundTimeRemaining / 60 / 62,
// truncated) over the limit in minutes; an untimed round reads 0/0 and a spent one 0/N.
// [orig: CAdminServer_HandleGet @0x403888..0x4038c5 (the format @0x7c0b78)]
int admin_parse_time_left_minutes(std::string_view reply) {
	for (std::string_view line : split_lines(reply)) {
		const size_t eq = line.find('=');
		if (eq == std::string_view::npos || strutil::trim_view(line.substr(0, eq)) != "GameTime") continue;
		const std::string_view value = strutil::trim_view(line.substr(eq + 1));
		const size_t slash = value.find('/');
		if (slash == std::string_view::npos) return -1;
		const auto left = strutil::parse_int(std::string(strutil::trim_view(value.substr(0, slash))));
		const auto limit = strutil::parse_int(std::string(strutil::trim_view(value.substr(slash + 1))));
		if (!left || !limit) return -1;
		return (*limit > 0 && *left > 0) ? *left : -1;
	}
	return -1;
}

} // namespace opennova
