#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace opennova {

// Flat TLV format used by session-level messages (ClientHello, ServerHello,
// ClientJoin, ServerJoin, ClientGoodBye, ServerGoodBye). Witnessed byte-exact
// at NapiBuffer_AddField@0x5e65e0 and NapiNPSession_SendDescription@0x5e9840:
//
//   <tag ascii> 0x00 <LE16 size> <size bytes of value>  (concatenated)
//
// Distinct from the napi container TLV (0x02/0x03/0x04/0x05) — no markers
// or containers here, just a flat list of named fields.
//
// READ side confirmed against [orig: NapiNPProtocol_HandleClientHello @
// 0x6213B0] (grill wave 1, docs/net/novaworld-net-re.md §8): retail reads
// tags NVS/CO/AP/BDAT/PN/PG/PV1/PV2 (plus PV3/PM/CI/EIP/EPN/ET) and
// validates NVS == the Milota version string @ 0x7DFCF0, PN == server game
// id, PG == server key (16 B). SESSION NWU key @ 0x7DFC50.

// ClientHello, sent C2S as opcode 0x41 payload. Fields from
// NapiNPSession_SendDescription's TLV writes plus onnet's parser.
struct ClientHello {
	std::string nvs;  // NAPI version string (e.g. "NAPI NP Version 0.0.1 1/12/2004 - 2/20/2004 Milota Copyright 2004 NovaLogic")
	std::string co;   // Company (e.g. "NovaLogic Inc, Calabasas CA U.S.A.")
	std::string ap;   // Application (e.g. "JOINTOPS.EXE")
	std::string bdat; // Build date (e.g. "Jul 21 2009 18:54:41")
	std::string pn;   // Protocol name (e.g. "NOVAWORLDUDP")
	std::array<uint8_t, 16> pg{};  // Protocol GUID
	bool pg_present = false;
	std::string pv1;  // Protocol Version 1 (e.g. "0.0.0 2/10/2004 EM")
	std::string pv2;  // Protocol Version 2 (e.g. "1")
	uint32_t ci = 0;  // Client/Connection Index
	uint32_t eip = 0; // External IP (as uint32, big-endian wire form per inet)
	uint32_t epn = 0; // External Port Number
};

// Server-identity name defaults — ONE home for the three name-bearing wire
// fields, which deliberately differ: the 0x81 ServerHello carries our own
// AP/SN identity, while the 0x82 ServerAuth CU "NovaworldName" must stay the
// literal "NWServer" retail emits (notes/retail_capture_findings.md; the
// earlier "OpenNova" value there was retired for retail parity). Keeping all
// three side by side is what stops one being rebranded without the others
// being reconsidered.
inline constexpr char kServerHelloAppName[] = "OpenNova NWServer"; // 0x81 AP
inline constexpr char kServerHelloServerName[] = "OpenNova";       // 0x81 SN
inline constexpr char kNovaworldNameDefault[] = "NWServer";        // 0x82 CU "NovaworldName"

// ServerHello, sent S2C as opcode 0x81 payload.
struct ServerHello {
	uint32_t ci = 0;
	std::string co = "NovaLogic Inc, Calabasas CA U.S.A.";
	std::string ap = kServerHelloAppName;
	std::string bdat = "Jan  1 2026 00:00:00";
	uint32_t ut = 0; // uptime / unix time
	std::string pn = "NOVAWORLDUDP";
	std::array<uint8_t, 16> pg{};
	std::string pv1 = "0.0.0 2/10/2004 EM";
	std::string pv2 = "1";
	std::string pv3 = "1.6.4r opennova";
	uint32_t hk = 0x0FE0E112u; // host key (opaque to the client beyond echo in ClientJoin)
	std::string sn = kServerHelloServerName;
	std::string pl = "WIN32";
	uint32_t nc = 0; // node count
	uint32_t rip = 0; // reflected IP (client's apparent IP)
	uint32_t rpn = 0; // reflected port
	uint32_t eip = 0; // external IP (echo of ClientHello.eip)
	uint32_t epn = 0; // external port (echo of ClientHello.epn)

	// Game-server fields. The flat retail builder emits SF unconditionally and
	// gates the remaining numeric/string fields individually; `is_game_server`
	// is therefore a parse-side marker rather than an encoder switch. The
	// SF/P1/P2/NP/MP fields appear between SN and NC, and SUS1/SUS2 appear
	// between RPN and EIP. Witnessed in the retail capture
	// (notes/retail_capture2_decoded.txt frame 62334) — the host's
	// ServerHello on the game-server UDP port carries these extra fields
	// so the client knows the game type, current/max players, expansion,
	// and game-session id. Without them the client receives a generic
	// matchmaking-shaped Hello and never enters the gametype-specific
	// game UI (e.g. cmap.mnu spawn-selection).
	bool is_game_server = false;
	uint32_t sf = 0;          // server flags; retail observed = 0
	uint32_t p1 = 0;          // gametype; supplied by the live host configuration
	uint32_t p2 = 0;          // game-config; omitted until its live producer is modeled
	uint32_t np = 0;          // current player count; supplied by the live host
	uint32_t mp = 0;          // max players; supplied by the live host
	std::string sus1;         // unique session id (retail format: GSID-NN-XXXXXXXX-timestamp-hash)
	std::string sus2;         // expansion-pack archive name, supplied by the live host configuration
};

// Parse a ClientHello TLV payload (the bytes AFTER the 0x41 opcode and
// AFTER NWU-decryption). Returns true on success. Ignores unknown tags.
bool parse_client_hello(const uint8_t *data, size_t len, ClientHello &out);

// Serialize a ClientHello back to flat-TLV bytes (inverse of
// parse_client_hello). Field order matches the ClientHello struct
// declaration so the on-wire byte stream is reproducible from the same
// inputs (useful for roundtrip tests + Wireshark diffing).
std::vector<uint8_t> client_hello_to_bytes(const ClientHello &msg);

// Build a minimal-valid ServerHello by echoing a ClientHello's key fields
// and filling the rest with server-identity defaults. `client_ip_net` is
// the client's remote IP in network byte order (big-endian: first byte
// is the most significant octet) and `client_port` is its UDP port.
ServerHello build_server_hello(const ClientHello &client,
                               uint32_t client_ip_net,
                               uint16_t client_port);

// Serialize a ServerHello to flat-TLV bytes.
std::vector<uint8_t> server_hello_to_bytes(const ServerHello &msg);

// Parse a ServerHello TLV payload (the bytes AFTER the 0x81 opcode and
// AFTER NWU-decryption). The client uses this to recover the server's host
// key `hk` (which it must echo in ClientAuth.hk) plus the reflected
// IP/port. Inverse of server_hello_to_bytes; ignores unknown tags and is
// tolerant of either the matchmaking field set (with PL) or the
// game-server set (SF/P1/P2/NP/MP + SUS1/SUS2). Returns true on success.
bool parse_server_hello(const uint8_t *data, size_t len, ServerHello &out);

// ---- ClientAuth / ServerAuth (NP connection admission) -----------------
//
// NOTE ON NAMING: onnet calls these "ClientJoin" / "ServerJoin". The same
// NP/NAPI exchange admits a connection to either a service session or a game
// session, so neither lobby-specific nor game-specific semantics belong in
// this neutral wire layer. We use `Auth` for connection admission; higher
// layers decide what kind of session the connection carries.
//
// Opcodes: `0x42` ClientAuth (C→S), `0x82` ServerAuth (S→C).

struct ClientAuth {
	// Identity block — REQUIRED on the wire. The real NovaWorld server re-runs
	// the SAME version gate on the 0x42 join that it runs on the 0x41 hello:
	// NapiNPProtocol_HandleClientJoin @ 0x62B750 silently drops the join
	// (return 0 -> no ServerAuth -> the client times out in session_join)
	// unless NVS == the Milota string && PN == proto+220 && PG == proto+284
	// (16 B) && PV1 == proto+300; its is_server branch additionally rejects
	// unless PV2 == proto+364 ("1"). Retail's own 0x42 builder
	// (NapiNPConnection_SendClientHello @ 0x61fe20 — a Kong misnomer; the
	// packet type is 0x42='B', not 0x41) emits this whole identity block
	// ahead of the auth fields, with the same values as the ClientHello.
	// [orig: gate @ 0x62b750, builder @ 0x61fe20, identity @ 0x4d3be0]
	std::string nvs;  // NAPI version string (== Milota @ 0x7DFCF0; gate-checked)
	std::string co;   // Company (parsed, never validated — free)
	std::string ap;   // Application (free)
	std::string bdat; // Build date (free)
	std::string pn;   // Protocol name "NOVAWORLDUDP" (== proto+220; gate-checked)
	std::array<uint8_t, 16> pg{};  // Protocol GUID (== proto+284, 16 B; gate-checked)
	bool pg_present = false;
	std::string pv1;  // Protocol Version 1 "0.0.0 2/10/2004 EM" (== proto+300; gate)
	std::string pv2;  // Protocol Version 2 "1" (== proto+364; is_server-checked)

	uint32_t ci = 0;   // Client Index (echo from Hello)
	uint32_t hk = 0;   // Host Key (echo from ServerHello; checked == proto+1332)
	uint32_t ck = 0;   // Client Key (client-generated)
	std::string na;    // gate/game identifier (e.g. "jop:cus2"); must be non-empty
	uint32_t sip = 0;  // Source IP (retail omits the tag when 0)
	uint32_t spn = 0;  // Source Port Number (retail omits the tag when 0)
	std::string scrk;  // Client-side Session CRypto Key
	std::vector<std::vector<uint8_t>> cu; // Custom/User blobs (raw bytes, unparsed)
};

bool parse_client_auth(const uint8_t *data, size_t len, ClientAuth &out);

// Serialize a ClientAuth to flat-TLV bytes (inverse of parse_client_auth).
// Field order mirrors retail's 0x42 builder NapiNPConnection_SendClientHello
// @ 0x61fe20: the identity block NVS/CO/AP/BDAT/PN/PG/PV1/PV2 FIRST, then
// CI/HK/CK/NA, SIP/SPN (each omitted when 0, as retail does), the CU blobs,
// and SCRK last. The identity block is MANDATORY: the real NovaWorld server
// validates it in HandleClientJoin @ 0x62B750 and drops the join without it
// (the original "real NW never sends ServerAuth" bug — RE doc NW-S2).
std::vector<uint8_t> client_auth_to_bytes(const ClientAuth &msg);

// C2S 0x46 ClientGoodBye body: [u32 remote session key][DS][DC][DP1][DP2][DSTR][DPC][DDSTR].
// The key dword is validated by the receiver against its local session key; the TLVs carry the
// sender's disconnect-event stats and a cleanly-leaving client ships them zeroed with empty
// strings (retail's receiver discards DS and re-derives the role locally; the rest feed logs).
// The NWU crypt over bytes [1..] rides the ordinary nw_encode_outbound wrap.
// [orig: CNapiNPConnection_SendDisconnectPacket @0x61f2a0 (builder);
//  Nwu_HandleDisconnect @0x623ce0 (receiver key check + lenient TLV walk)]
std::vector<uint8_t> client_goodbye_to_bytes(uint32_t remote_session_key);

// The CONNECTION-DESCRIPTION record — the transport's own disconnect event carried as an INNER
// protocol message instead of a session opcode: high/settings flag set, low tag 3, i.e. full tag
// 0x103. Both directions route it through the same high-bit msginfo table, so a host sends the
// identical record a leaving client sends. Its TLV field set is exactly the 0x46 ClientGoodBye's
// minus the leading session-key dword, and the receiver walks the names case-insensitively with
// zero defaults, in ANY order, stopping at an empty name. Receipt is terminal: state 5 -> 6 (which
// tears the active connection down) or, mid-connect, a pending-disconnect latch.
// [orig: builder NapiNPDataTransfer_SendDescription @0x628c80 ->
//  NapiNPMessage_Create(msg_id 3, msg_class 1) @0x627fc0; receiver
//  CNapiNPConnection_HandleDescriptionPacket @0x621ae0 (g_np_msginfo_highbit @0x849e80 row 3),
//  TLV walk @0x621b8c..0x621c7d, terminal state @0x621d53..0x621d6b]
// The tag constant PROTOCOL_TAG_CONNECTION_DESCRIPTION lives in
// npwire/protocol_message.h (the full_tag/high-bit home); only the TLV body
// parser lives here, beside its ClientGoodBye TLV sibling.

struct DisconnectEvent {
	uint32_t ds = 0;    // sender role (1 server / 2 client) — the receiver DISCARDS it and
	                    // re-derives the role from its own connection [orig: @0x621ba5 no store]
	uint32_t dc = 0;    // disconnect class; 2 is the description family the client dispatches on
	                    // [orig: the `== 2` gate @0x4c6563]
	uint32_t dp1 = 0;
	uint32_t dp2 = 0;
	std::string dstr;   // free-form text (retail keeps the first 128 bytes)
	uint32_t dpc = 0;   // reason code; the client's exit-reason switch reads THIS [orig: @0x4c6569]
	std::string ddstr;  // event tag (retail keeps the first 32 bytes)
};

// Parse a connection-description body (the inner message payload, already SCRK-decrypted). Unknown
// names are skipped by their length and field order is not assumed, matching the retail walk.
// Returns false when the body is not a well-formed flat-TLV run or carries none of the seven known
// names — the narrow shape gate a dispatcher needs before treating a settings-flagged message as a
// disconnect. [orig: CNapiNPConnection_HandleDescriptionPacket @0x621ae0]
bool parse_disconnect_event(const uint8_t *data, size_t len, DisconnectEvent &out);

// Retail JO game-session identity shared by LAN enumeration and the actual
// game connection. These values come from CNapiNetwork_Init @ 0x4ca4a0 and
// the retail LAN ClientAuth capture; they are unrelated to NovaWorld's lobby
// identity. `player_name` is the game ClientAuth NA/callsign.
std::array<uint8_t, 16> jointoperations_protocol_guid();
bool is_jointoperations_protocol_name(std::string_view protocol_name);
ClientHello make_jointoperations_client_hello(uint32_t client_index);
ClientAuth make_jointoperations_client_auth(uint32_t client_index, uint32_t client_key,
		uint32_t host_key, std::string_view player_name, std::string_view client_scrk);

// Retail's game host silently drops 0x41/0x42 messages unless their version
// block matches the identity installed by CNapiNetwork_Init. The 0x42 gate
// additionally compares PV2; CO/AP/BDAT remain intentionally free because the
// retail handlers parse but do not compare them.
bool matches_jointoperations_identity(const ClientHello &hello);
bool matches_jointoperations_identity(const ClientAuth &auth);

// ---- ClientAuth CU chunks (NW-S3) --------------------------------------
//
// The retail client carries a set of named CU chunks in its 0x42 join,
// built from CNapiGameSession_ConnectToNovaWorld @ 0x4d4640's var list
// (Application, BuildDateAndTime, Debug, CountryName, Language,
// TimeZoneBias, GateTag, MetTag, UdpCode1, UdpCode2, MaxPacketSize) and
// emitted by NapiNPConnection_SendClientHello @ 0x61fe20. The last codes
// (UdpCode1/UdpCode2) are session-auth tokens the gate issues
// (gate VAR keys UDPCODE1/UDPCODE2 -> ProcessResponse @ 0x4ced20); the live
// NW server's join callbacks (cb_server_0/cb_server_1) validate the join
// against them. These chunks are NOT required by the OpenNova server (its
// callbacks are permissive) but ARE required to authenticate against live
// NovaLogic NW (see docs/net §8 NW-S3).
//
// Wire shape of one CU chunk's value (the inner bytes after the "CU" flat-TLV
// name+size), mirroring NapiNPChunk_Create @ 0x624720 + SendClientHello's
// writer: [type:1B][name + NUL][LE16 data_len][value + NUL], where
// data_len = value.size()+1 (retail stores strlen(value)+1). `type` is 1 or 2
// (the only values HandleClientJoin @ 0x62B750's CU loop accepts).
std::vector<uint8_t> make_client_cu_chunk(uint8_t type, std::string_view name,
                                          std::string_view value);

// Decode one CU chunk produced by make_client_cu_chunk (inverse). Returns
// false if the shape doesn't hold; out_value has the trailing NUL stripped.
bool parse_client_cu_chunk(const uint8_t *data, size_t len, uint8_t &out_type,
                           std::string &out_name, std::string &out_value);

// Control-setting entry — (direction_byte, field_index, uint32 value).
// CLIENT_* direction=1, SERVER_* direction=0. The per-channel timeout_ms values are the WITNESSED
// engine template (D-NET-1, identical both directions): {0:240000,1:4,4:60000,5:1000,6:0xFFFFFFFF,
// 8:2048,9:128,10:100,11:500,12:1,13:1300(MTU),14:0xFFFFFFFF}; idx 2/3/7 = 0. [orig:
// CNapiGameSession_InitNPConnection @0x4d3e1f / CNapiNPConnection_SendSessionInit @0x620ef0]
struct CsField {
	uint8_t field_index;
	uint32_t value;
};
std::vector<CsField> default_client_cs_fields();
std::vector<CsField> default_server_cs_fields();

struct ServerAuth {
	uint32_t ci = 0;      // echo client.ci
	uint32_t mi = 0x113fu; // MI TLV = the host-assigned ConnectionId (dcb), NOT a "machine id": the
	                       // client stores it as its own NapiNPConnection.connection_id (+0x18,
	                       // NapiNP_GetLocalConnectionId @0x4c6d40) and the host stamps it into the
	                       // joiner's 0x0C ownerConnectionId (+0x78) for Player_FindLocalPlayerEntity
	                       // @0x4e0090. Host-assigned join-order on LAN [orig: ++protocol[947] @
	                       // NapiNPConnection_Create 0x62acb0; emitted by SendSessionInit @0x620ef0];
	                       // the 0x113f default is a placeholder for callers that don't set it. Retail
	                       // LAN capture frame 28 = 3 (= the 0x48 ack = the 0x0C eFlags).
	uint32_t ck = 0;      // echo client.ck
	uint32_t cr = 1;      // Connection Result (1 = OK)
	uint32_t sk = 0;      // Server Key (server-generated)

	std::vector<CsField> client_cs; // written with direction byte 1
	std::vector<CsField> server_cs; // written with direction byte 0

	// Custom/User fields: (name, value). Serialised with onnet's
	// `\x03 <name>\0 <LE16 value_len> <value>\0` inner shape.
	std::vector<std::pair<std::string, std::string>> cu;

	std::string scrk;     // Server-side Session CRypto Key (61 chars; retail capture)
	std::string na;       // echo client.na
	uint32_t rip = 0;     // Reflected IP (client's remote IP)
	uint32_t rpn = 0;     // Reflected Port Number

	// Rejected-join fields. Retail can send these without SCRK when CR != 1;
	// the packet is valid and should surface as a rejection, not malformed.
	uint32_t jfc = 0;     // Join failure code
	uint32_t jfp = 0;     // Join failure parameter
	std::string jfs;      // Join failure string
};

// Build a minimal-valid ServerAuth echoing client fields + server
// identity defaults. `client_ip_net` is the client's IP in network byte
// order (high byte first); `client_port` is its UDP port; `server_sk` and
// `server_scrk` are caller-chosen values (deterministic values OK for
// development; production would use a secure random).
// Defaults per retail capture (notes/retail_capture_findings.md):
//   novaworld_name = "NWServer" (was "OpenNova"; retail emits the literal "NWServer")
//   nwuid          = 60-char ASCII hex ID; placeholder default is fine for dev, the
//                    UDP listener overrides with a freshly-generated value per session.
// [D-NET Wave 3] `include_novaworld_cu` gates the three NovaworldName/url/NWUID CU entries. The
// witnessed 0x82 builder [orig: CNapiNPConnection_SendSessionInit @0x620ef0] sources CU from the host's
// type-3 msg_out queue, which only the NovaWorld lobby flow populates — a LAN host queues none and emits
// NO CU. Pass false for a LAN/SP host (the default stays true for the NovaWorld + standalone-server callers).
ServerAuth build_server_auth(const ClientAuth &client,
                             uint32_t client_ip_net,
                             uint16_t client_port,
                             uint32_t server_sk,
                             std::string_view server_scrk,
                             std::string_view novaworld_name = kNovaworldNameDefault,
                             std::string_view novaworld_web_url = "http://127.0.0.1:8080",
                             std::string_view nwuid = "0accd1b2cffac0e3f1efe6c80000000000000000000000000000000000000000",
                             bool include_novaworld_cu = true);

// Serialize a ServerAuth to flat-TLV bytes (ready to opcode-prefix +
// NWU-encrypt + CRC-envelope).
std::vector<uint8_t> server_auth_to_bytes(const ServerAuth &msg);

// Parse a ServerAuth TLV payload (the bytes AFTER the 0x82 opcode and
// AFTER NWU-decryption). The client uses this to recover the connection
// result `cr` (1 = accepted), the server key `sk` (which becomes the
// session_id on the client's outbound 0x43 ProtocolMessages), and the
// server-side `scrk` (which decrypts inbound 0x83 ProtocolMessages). Also
// recovers the CS/CU control fields for round-trip fidelity. Inverse of
// server_auth_to_bytes; ignores unknown tags. Returns true on success.
bool parse_server_auth(const uint8_t *data, size_t len, ServerAuth &out);

} // namespace opennova
