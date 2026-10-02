#pragma once

#include <net/napi/session.h> // SessionIdRing (the host's cookie-key table)

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova {

struct PubJoinIdentityPlaintexts {
	std::vector<uint8_t> pcid;
	std::vector<uint8_t> name_info;
	std::vector<uint8_t> squad_info;
};

std::vector<uint8_t> build_pub_pcid_plaintext(std::string_view pcid);
std::vector<uint8_t> build_pub_nameinfo_plaintext(std::string_view nwhandle);
std::vector<uint8_t> build_pub_squadinfo_plaintext(std::string_view nwhandle);

PubJoinIdentityPlaintexts build_pub_join_identity_plaintexts(
	std::string_view pcid,
	std::string_view nwhandle);

// The joiner's CD identity cookie as the host's JOIN handler keeps it: the
// [name\0][value\0] pairs of the C2S 0x00 JOIN's "CD" field.
using JoinCookiePairs = std::vector<std::pair<std::string, std::string>>;

// The NovaWorld account fields of a player's net config: the PCID, and the
// squad id, name and tag the SQUADINFO cookie carries. Empty / 0 on LAN.
// [orig: NapiNetConfig (NapiNPPlayer+0xCC) — pcid +0x184, squad_id +0x1A4,
//  squad name +0x1A8, squad tag +0x1E8; cleared by NapiNetConfig_Init
//  @0x4c35f2..0x4c3604]
struct JoinAccount {
	std::string pcid;       // at most 31 chars [orig: NapiNetConfig_SetPcid @0x4c2730, cap 32]
	uint32_t squad_id = 0;
	std::string squad_name; // at most 64 chars [orig: PlayerSlot_SetDisplayName @0x4f9c10, cap 65]
	std::string squad_tag;  // at most 8 chars [orig: PlayerSlot_SetShortName @0x4f9c30, cap 9]
};

// The PCID store: Napi_CopyString's 32-byte field keeps 31 chars.
void set_join_account_pcid(JoinAccount &account, std::string_view pcid);

// One CD cookie field decrypted under the host's cookie-key table. The value
// is found by name (case-insensitively, the first match), A-P decoded, and
// each key of the table is tried in slot order by its decimal spelling until
// the trailing CRC verifies (an empty table tries the key 0 once). `out` is the
// plaintext plus the NUL that overwrites the CRC, so its size is the retail
// out-length; false when the field is absent, malformed, no longer than its
// CRC, no key verifies, or it would not fit `out_capacity`.
// [orig: NapiPacket_DecryptAndVerify @0x4c2ad0 — KeyValueBuffer_FindValue
//  @0x4c2b50 (Napi_StrCaseEqual @0x4c2a99), NetPacket_WriteStringAndShort
//  @0x4c2b71, the <= 4 reject @0x4c2b78, keys[0..key_count) @0x4c2b9c..0x4c2c02
//  with sprintf "%ld" @0x4c2bb8, the capacity reject @0x4c2c27, the NUL
//  @0x4c2c2f, *outLength = decodedLen - 3 @0x4c2c50]
bool decrypt_join_cookie_field(const JoinCookiePairs &cookie, const std::string &field_name,
		const SessionIdRing &key_table, std::size_t out_capacity, std::vector<uint8_t> &out);

// The host JOIN handler's cookie leg: "<local>PCID" sets the PCID and
// "<local>SQUADINFO" the squad id (the leading dword when the out-length
// reaches 4, else 0), then the squad name and tag as the next two strings, each
// bounded by the decrypted length. A field that does not decrypt leaves its
// values untouched. The caller gates it on a remote connection with a local
// address (a NovaWorld session).
// [orig: NapiNPServer_HandlePlayerJoinMessage @0x512AA0 — the gate @0x512ca2 /
//  @0x512cdc, PCID @0x512cf9..0x512d39, SQUADINFO @0x512d55..0x512e50]
void load_join_account(const JoinCookiePairs &cookie, const std::string &local_address,
		const SessionIdRing &key_table, JoinAccount &account);

} // namespace opennova
