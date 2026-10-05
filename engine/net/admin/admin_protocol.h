#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {

// The retail remote-admin protocol, client side: the TCP console the game server opens on
// `remote_admin_port` (CAdminServer), as the retail admin client RAT.exe speaks it. The server
// sends a challenge, the client answers with the encrypted login, then sends one NUL-terminated
// command per packet and reads the text replies. Only the read-only commands a server listing
// needs are named here. The witness record is docs/net/novaworld-net-re.md §6.9.
// [orig: CAdminServer_ProcessFrame @0x406f50; CAdminServer_ProcessClientData @0x406ec0;
//  CAdminServer_HandleLogin @0x405870; CAdminServer_DispatchCommand @0x406720]

// The server's port is `remote_admin_port`, 0 (the listener off) by default; RAT.exe dials
// 0x8020 when its `open` names no port. [orig: Config_SetDefaults @0x54d454;
//  Game_InitSubsystems @0x4a72c7; RAT.exe @0x401057]
inline constexpr uint16_t ADMIN_CLIENT_DEFAULT_PORT = 32800;

// Every packet: the u32le marker 0x0A0D0000, the u32le total length (this header included),
// the payload. [orig: CAdminServer_SendResponse @0x402d40 @0x402d6b..0x402d85]
inline constexpr size_t ADMIN_PACKET_HEADER_BYTES = 8;
// The server buffers at most 1024 bytes of one inbound packet; a longer one wedges the slot.
// [orig: CAdminServer_AcceptConnection @0x405751..0x40577b]
inline constexpr size_t ADMIN_SERVER_PACKET_MAX_BYTES = 1024;
// The challenge payload: 32 key bytes and a NUL. [orig: AcceptConnection @0x40582c (0x21)]
inline constexpr size_t ADMIN_CHALLENGE_BYTES = 33;
// The login payload: user[32] pass[32] and a zero byte, encrypted whole; the server keeps 31
// characters of each field. [orig: HandleLogin @0x40588c (0x41), @0x4058e7/@0x4058f0]
inline constexpr size_t ADMIN_LOGIN_BYTES = 65;
inline constexpr size_t ADMIN_LOGIN_FIELD_MAX_CHARS = 31;

inline constexpr const char *ADMIN_COMMAND_PLAYER_LIST = "PLAYER LIST";
inline constexpr const char *ADMIN_COMMAND_MISSION_LIST = "MISSION LIST";
inline constexpr const char *ADMIN_COMMAND_GET_GAMESETTINGS = "GET GAMESETTINGS";
// The orderly end: the server closes the connection itself, freeing its slot.
// [orig: DispatchCommand @0x406811; RAT.exe @0x401475]
inline constexpr const char *ADMIN_COMMAND_QUIT = "QUIT";

// One packet around `payload`.
std::vector<uint8_t> admin_encode_packet(const uint8_t *payload, size_t size);
// The payload size a received header announces; false for a bad marker or a length below the
// header.
bool admin_decode_header(const uint8_t *header, size_t &payload_size);

// The challenge is 32 bytes and a NUL with byte 0 always 0x01; RAT.exe refuses any other.
// [orig: AcceptConnection @0x4057d5; RAT.exe @0x401323]
bool admin_challenge_valid(const std::vector<uint8_t> &payload);
// The login payload for `challenge`: user and password (31 characters each at most) in their
// 32-byte fields, encrypted under the challenge string. [orig: Crypto_EncryptBuffer @0x437510]
std::array<uint8_t, ADMIN_LOGIN_BYTES> admin_encode_login(const std::vector<uint8_t> &challenge,
                                                          std::string_view user, std::string_view password);

// The cipher's inverse, as the server runs it over a login payload: the LCG stream off, the
// progressive key off, the reverse when s3 is odd, the key off, each step over all `size` bytes;
// the key is the challenge string (its 32 key bytes before the NUL). The server keeps 31
// characters of each field by writing NULs at bytes 31 and 63 afterwards (AdminServer's login).
// [orig: Crypto_DecryptBuffer @0x4375b0 -> Crypto_PRNGDecryptSub @0x4372a0,
//  Crypto_SubProgressiveKey @0x4372f0, Buffer_ReverseInPlace2 @0x437330, sub_437370]
void admin_decrypt_buffer(uint8_t *buf, size_t size, const uint8_t *key, size_t key_len);

// A command packet's payload: the text and its NUL (the server closes on a command without it).
std::vector<uint8_t> admin_encode_command(std::string_view command);
// A reply payload's text without its NUL; false when it carries none.
bool admin_reply_text(const std::vector<uint8_t> &payload, std::string &out);
// "OK - User: <user> successfully logged in."; a refused login gets no reply, only the close.
bool admin_login_accepted(std::string_view reply);
// An "ERROR - " or "USAGE - " reply rather than the command's answer.
bool admin_reply_is_error(std::string_view reply);

// One PLAYER LIST row a listing carries.
struct AdminPlayer {
	int slot = 0;
	std::string name;
	std::string team;
};
// PLAYER LIST: the player rows. The header and the host's own slot-0 "Host" row are not players.
std::vector<AdminPlayer> admin_parse_player_list(std::string_view reply);
// MISSION LIST: the <CURRENT MISSION> entry's file name without its .bms/.npj/.npz extension;
// "" when no entry is current.
std::string admin_parse_current_mission(std::string_view reply);
// GET GAMESETTINGS: the round's remaining whole minutes; -1 when untimed or run out.
int admin_parse_time_left_minutes(std::string_view reply);

} // namespace opennova
