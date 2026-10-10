#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <net/npwire/character_id.h>

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
	uint32_t de = 0;  // session_info[54], emitted only when nonzero
	std::string pn;   // Protocol name (e.g. "NOVAWORLDUDP")
	std::array<uint8_t, 16> pg{};  // Protocol GUID
	bool pg_present = false;
	std::string pv1;  // Protocol Version 1 (e.g. "0.0.0 2/10/2004 EM")
	std::string pv2;  // Protocol Version 2 (e.g. "1")
	std::string pv3;  // Optional third version string
	uint32_t ci = 0;  // Client/Connection Index
	uint32_t pm = 0;  // transport player count; a NONZERO PM is also the host-side
	                  // identity bypass (see client_hello_admits). An absent and a
	                  // zero PM are the same state: the writer never emits a zero PM
	                  // [orig: NapiNPSession_SendAnnouncePacket @0x61fa00 gates
	                  // @0x61fcca / @0x61fcda] and the reader keeps no presence
	                  // marker [orig: NapiNPProtocol_HandleClientHello @0x6213B0 —
	                  // PM dword @0x62173C, zero-initialised @0x621504]
	uint32_t eip = 0; // External IP (as uint32, big-endian wire form per inet)
	uint32_t epn = 0; // External Port Number
	uint32_t et = 0;  // optional extra parameter
};

// Server-identity name defaults — ONE home for the three name-bearing wire
// fields, which deliberately differ: the 0x81 ServerHello carries our own
// AP/SN identity, while the 0x82 ServerAuth CU "NovaworldName" must stay the
// literal "NWServer" retail emits (docs/net/novaworld-net-re.md §7; the
// earlier "OpenNova" value there was retired for retail parity). Keeping all
// three side by side is what stops one being rebranded without the others
// being reconsidered.
inline constexpr char kServerHelloAppName[] = "OpenNova NWServer"; // 0x81 AP
inline constexpr char kServerHelloServerName[] = "OpenNova";       // 0x81 SN
inline constexpr char kNovaworldNameDefault[] = "NWServer";        // 0x82 CU "NovaworldName"

// ServerHello, sent S2C as opcode 0x81 payload. The field set, order and
// per-field gates are the retail writer's: CI, HK, SN and SF are written
// unconditionally; every other string field only when non-empty, every other
// numeric field only when nonzero, PG only when the GUID is non-null, and the
// CN/LNG/TZB trio as one block behind its own enable. The receiver walks the
// tags case-insensitively with zero defaults.
// [orig: writer NapiNPProtocol_SendServerInfoPacket @0x6204b0 (CI @0x62057e ..
//  ET @0x620c0c); reader Nwu_HandleServerHello @0x626d20 (CI @0x626f53 .. ET
//  @0x6274f4, SUS3/SUS4 stored at session+1788/+2300 @0x627de8/@0x627e01)]
struct ServerHello {
	uint32_t ci = 0;
	std::string co = "NovaLogic Inc, Calabasas CA U.S.A.";
	std::string ap = kServerHelloAppName;
	std::string bdat = "Jan  1 2026 00:00:00";
	uint32_t de = 0; // proto->unk_0xD8, written only when nonzero @0x620647
	uint32_t ut = 0; // uptime ms (GetTickCount - host_start_tick), nonzero-gated
	std::string pn = "NOVAWORLDUDP";
	std::array<uint8_t, 16> pg{}; // written only when non-null [orig: NapiGUID_IsNull @0x62ed40 gate @0x6206d5]
	std::string pv1 = "0.0.0 2/10/2004 EM";
	std::string pv2 = "1";
	std::string pv3 = "1.6.4r opennova";
	uint32_t hk = 0x0FE0E112u; // host key (opaque to the client beyond echo in ClientJoin)
	std::string sn = kServerHelloServerName;
	// The locale block: CN (country name), LNG (language) and TZB (time-zone
	// bias) are written together, unconditionally, when the protocol object's
	// enable at +0xD48 is set; the two strings ship their NUL even when empty.
	// [orig: @0x6207e2 gate; CN @0x62081a, LNG (byte_7CA07C) @0x62084a, TZB @0x62086a]
	bool locale_block = false;
	std::string cn;
	std::string lng;
	uint32_t tzb = 0;
	uint32_t nc = 0; // node count, nonzero-gated @0x620a59
	uint32_t rip = 0; // reflected IP (client's apparent IP), nonzero-gated @0x620a81
	uint32_t rpn = 0; // reflected port, nonzero-gated @0x620aa9
	uint32_t eip = 0; // external IP (echo of ClientHello.eip), nonzero-gated @0x620baa
	uint32_t epn = 0; // external port (echo of ClientHello.epn), nonzero-gated @0x620bd0
	uint32_t et = 0;  // external type (echo of ClientHello.et), nonzero-gated @0x620bf6

	// Game-server fields. The flat retail builder emits SF unconditionally and
	// gates the remaining numeric/string fields individually; `is_game_server`
	// is therefore a parse-side marker rather than an encoder switch. The
	// SF/P1..P8/NP/MP/NPW fields appear between SN and NC, and SUS1..SUS4
	// appear between RPN and EIP. Witnessed in the retail capture
	// (docs/net/novaworld-net-re.md §5.9) — the host's
	// ServerHello on the game-server UDP port carries these extra fields
	// so the client knows the game type, current/max players, expansion,
	// and game-session id. Without them the client receives a generic
	// matchmaking-shaped Hello and never enters the gametype-specific
	// game UI (e.g. cmap.mnu spawn-selection).
	bool is_game_server = false;
	uint32_t sf = 0;          // server flags; retail observed = 0
	uint32_t p1 = 0;          // gametype; supplied by the live host configuration
	uint32_t p2 = 0;          // game-config; live CNapiServerConfig_BuildFlags snapshot
	uint32_t p3 = 0;          // proto->p3_count .. p8_count, each nonzero-gated @0x6208fa..@0x6209bd
	uint32_t p4 = 0;
	uint32_t p5 = 0;
	uint32_t p6 = 0;
	uint32_t p7 = 0;
	uint32_t p8 = 0;
	uint32_t np = 0;          // current player count; supplied by the live host
	uint32_t mp = 0;          // max players; supplied by the live host
	uint32_t npw = 0;         // proto->npw_count, nonzero-gated @0x620a32
	std::string sus1;         // unique session id (retail format: GSID-NN-XXXXXXXX-timestamp-hash)
	std::string sus2;         // expansion-pack archive name, supplied by the live host configuration
	std::string sus3;         // server user strings 3/4: written when non-empty @0x620b32/@0x620b68,
	std::string sus4;         // stored by the receiver (512 B each); semantics unwitnessed
};

// Parse a ClientHello TLV payload (the bytes AFTER the 0x41 opcode and
// AFTER NWU-decryption). Walks the tags case-insensitively with zero
// defaults and stops at the first malformed field; no tag is required (a
// PM-only announce is a valid hello — see client_hello_admits). Returns
// false only for a null buffer. Ignores unknown tags. A CI/PM/EIP/EPN/ET
// shorter than four bytes takes the bytes that follow it, as retail's dword
// load does (zero past the buffer's end).
// [orig: NapiNPProtocol_HandleClientHello @0x6213b0 TLV walk]
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
// AFTER NWU-decryption) into a zeroed record: the tags are walked
// case-insensitively, the walk stops at the first malformed or empty-named
// tag and keeps what it gathered, and an absent tag reads as zero/empty.
// There is one flat tag set, the writer's. Returns true when HK was seen
// (the value the client echoes in ClientAuth).
// [orig: Nwu_HandleServerHello @0x626d20]
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
	// (CNapiNPConnection_SendClientJoin @ 0x61fe20, renamed from the Kong
	// misnomer SendClientHello; the packet type is 0x42='B', not 0x41) emits
	// this whole identity block ahead of the auth fields, with the same
	// values as the ClientHello.
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
	std::string pw;    // optional server password, distinct from the spectator JSPP CU
	uint32_t sip = 0;  // Source IP (retail omits the tag when 0)
	uint32_t spn = 0;  // Source Port Number (retail omits the tag when 0)
	std::string scrk;  // Client-side Session CRypto Key
	std::vector<std::vector<uint8_t>> cu; // Custom/User blobs (raw bytes, unparsed)
	// The reconnect trailer, each tag emitted only when nonzero: NF (the connection's byte
	// flag at +0x7BC), DCNT (the client connection's counted disconnects, +0x734) and RCNT
	// (the last 0x82's echo, +0x738). A first join carries none of them.
	// [orig: CNapiNPConnection_SendClientJoin @0x61fe20 — NF @0x620309..0x620329, DCNT
	//  @0x62033d..0x620354, RCNT @0x620368..0x62037f; the server reads them in
	//  NapiNPProtocol_HandleClientJoin @0x62b750 and stores DCNT / RCNT(+1) @0x62c28d..0x62c2a3]
	uint32_t nf = 0;
	uint32_t dcnt = 0;
	uint32_t rcnt = 0;
};

bool parse_client_auth(const uint8_t *data, size_t len, ClientAuth &out);

// Serialize a ClientAuth to flat-TLV bytes (inverse of parse_client_auth).
// Field order mirrors retail's 0x42 builder CNapiNPConnection_SendClientJoin
// @ 0x61fe20: the identity block NVS/CO/AP/BDAT/PN/PG/PV1/PV2 FIRST, then
// CI/HK/CK/NA/PW, SIP/SPN (each omitted when 0, as retail does), the CU blobs,
// and SCRK last. The identity block is MANDATORY: the real NovaWorld server
// validates it in HandleClientJoin @ 0x62B750 and drops the join without it
// (the original "real NW never sends ServerAuth" bug — RE doc NW-S2).
std::vector<uint8_t> client_auth_to_bytes(const ClientAuth &msg);

// The CONNECTION-DESCRIPTION record — the transport's own disconnect event carried as an INNER
// protocol message instead of a session opcode: high/settings flag set, low tag 3, i.e. full tag
// 0x103. Both directions route it through the same high-bit msginfo table, so a host sends the
// identical record a leaving client sends. Its TLV field set is exactly the 0x46 ClientGoodBye's
// minus the leading session-key dword, and the receiver walks the names case-insensitively with
// zero defaults, in ANY order, stopping at an empty name. Receipt is terminal: state 5 -> 6 (which
// tears the active connection down) or, mid-connect, a pending-disconnect latch.
// [orig: builder NapiNPDataTransfer_SendDescription @0x628c80 ->
//  NapiNPMessage_Create(msg_id 3, msg_class 1) @0x627fc0; receiver
//  CNapiNPConnection_HandleDescriptionPacket @0x621ae0 (g_NPMsgInfoHighBit @0x849e80 row 3),
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

// The retail NapiNPDisconnectEvent stores DSTR in `char message[128]` and DDSTR
// in `char extra[32]`, each filled by Napi_CopyString(dst, src, N), which copies
// at most N-1 characters and always NUL-terminates — so a LATCHED record carries
// at most 127 / 31 characters. Every latch site applies this cap; the wire
// writers serialize the latched strings verbatim (strlen + 1).
// [orig: Napi_CopyString @0x617e10; the 128/32 copies at Nwu_HandleDisconnect
//  @0x624019/@0x624097, HandleDescriptionPacket @0x621cca.., PumpStateMachine
//  @0x6293b7.., NapiNPProtocol_StopServer @0x62a8db/@0x62a8f6]
inline constexpr std::size_t kDisconnectEventDstrChars = 127;
inline constexpr std::size_t kDisconnectEventDdstrChars = 31;

// Build one disconnect record with the latch-side string caps applied.
inline DisconnectEvent make_disconnect_event(uint32_t ds, uint32_t dc, uint32_t dp1,
		uint32_t dp2, std::string_view dstr, uint32_t dpc, std::string_view ddstr) {
	DisconnectEvent event;
	event.ds = ds;
	event.dc = dc;
	event.dp1 = dp1;
	event.dp2 = dp2;
	event.dstr.assign(dstr.substr(0, kDisconnectEventDstrChars));
	event.dpc = dpc;
	event.ddstr.assign(ddstr.substr(0, kDisconnectEventDdstrChars));
	return event;
}

// Serialize the seven-field connection-description body in retail's exact
// DS/DC/DP1/DP2/DSTR/DPC/DDSTR order. Unlike C2S 0x46 ClientGoodBye this is an
// INNER H:0x03 payload and therefore has no leading session-key dword.
// [orig: NapiNPDataTransfer_SendDescription @0x628c80]
std::vector<uint8_t> connection_description_to_bytes(
		const DisconnectEvent &event);

// The SESSION-OPCODE disconnect packet body shared by C2S 0x46 ClientGoodBye and S2C 0x86
// ServerGoodBye: [u32 peer key][DS][DC][DP1][DP2][DSTR][DPC][DDSTR]. The key dword is the
// RECEIVER's local key (a client sends the host's SK, the host sends the client's CK) and is the
// only thing the receiver validates; the TLVs are the sender's latched disconnect record, written
// unconditionally (DSTR/DDSTR ship their NUL even when empty). The NWU crypt over bytes [1..] rides
// the ordinary nw_encode_outbound wrap.
// [orig: CNapiNPConnection_SendDisconnectPacket @0x61f2a0 — key dword @0x61f3af, the seven
//  TLVs @0x61f3d0..0x61f4aa, NWU encrypt @0x61f4cd; receiver Nwu_HandleDisconnect @0x623ce0
//  (key check @0x623e74 + lenient TLV walk @0x623eb2..0x623fbd)]
std::vector<uint8_t> disconnect_packet_body_to_bytes(uint32_t peer_key,
		const DisconnectEvent &event);

// C2S 0x46 ClientGoodBye: `remote_session_key` = the host's SK, `event` = the client's latched
// record (a user leave latches {2, 2, 0, 0, "I.C:CIDEMIS", 0, ""}; a host punt/goodbye is echoed).
std::vector<uint8_t> client_goodbye_to_bytes(uint32_t remote_session_key,
		const DisconnectEvent &event);
// The zero-record form: the NOVAWORLDUDP lobby ClientSession, whose user leave latches nothing
// (the zero record), ships all-zero stats with empty strings.
// [orig: CNapiGameSession_ResetToDisconnected @0x4D0890 -> CNapiNPConnection_RequestDisconnect
//  @0x61E0F0 (no latch) -> CNapiNPConnection_Destroy @0x62A4B0 -> TeardownActiveConnection
//  @0x6253C0 -> SendDisconnectPacket @0x61F2A0 (writes disconnect_event unconditionally)]
std::vector<uint8_t> client_goodbye_to_bytes(uint32_t remote_session_key);

// S2C 0x86 ServerGoodBye: `client_ck` = the departing client's CK (its local key), `event` =
// the host-side latched record (SERTMOUT reap, STOP, MSGCRE, the echoed client goodbye, ...).
std::vector<uint8_t> server_goodbye_to_bytes(uint32_t client_ck, const DisconnectEvent &event);

// Parse a connection-description body (the inner message payload, already SCRK-decrypted). Unknown
// names are skipped by their length and field order is not assumed, matching the retail walk. The
// walk stops at a malformed field or an empty name and KEEPS the fields gathered so far; it returns
// true for every non-null body, because the retail receiver records the description and moves the
// connection to state 6 no matter what the walk yielded — a body carrying none of the seven names,
// or a valid run with a truncated tail, still disconnects. The dispatcher gates on the H:0x03 tag
// alone. [orig: CNapiNPConnection_HandleDescriptionPacket @0x621ae0 — walk exits on read failure
//  @0x621b8c, terminal SetState(6) @0x621d59 unconditional on the parsed content]
bool parse_disconnect_event(const uint8_t *data, size_t len, DisconnectEvent &out);

// ---- ClientAuth character_id bit-pack (the CI0/CI1 join vars) ----------
// Lives in npwire/character_id.h (included above): the one packing shared by
// ClientAuth CI0/CI1, the entity+0x15C wire NetId, and the weapon.sav header.

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

// The 0x41 admission a host applies before answering with a ServerHello: a
// hello whose PM is nonzero is answered WITHOUT any identity validation (the
// version block is only checked when PM == 0), so a retail host's unsolicited
// announce — which carries its player count as PM — always draws a reply.
// [orig: NapiNPProtocol_HandleClientHello @0x6213b0 — `if (!protocol_version)`
//  around the NVS/PN/PG/PV1 compares, jnz @0x6217c2 -> SendServerInfoPacket
//  @0x6218fc]
bool client_hello_admits(const ClientHello &hello);

// ---- ClientAuth CU chunks (NW-S3) --------------------------------------
//
// The retail client carries a set of named CU chunks in its 0x42 join,
// built from CNapiGameSession_ConnectToNovaWorld @ 0x4d4640's var list
// (Application, BuildDateAndTime, Debug, CountryName, Language,
// TimeZoneBias, GateTag, MetTag, UdpCode1, UdpCode2, MaxPacketSize) and
// emitted by CNapiNPConnection_SendClientJoin @ 0x61fe20. The last codes
// (UdpCode1/UdpCode2) are session-auth tokens the gate issues
// (gate VAR keys UDPCODE1/UDPCODE2 -> ProcessResponse @ 0x4ced20); the live
// NW server's join callbacks (cb_server_0/cb_server_1) validate the join
// against them. These chunks are NOT required by the OpenNova server (its
// callbacks are permissive) but ARE required to authenticate against live
// NovaLogic NW (see docs/net §8 NW-S3).
//
// Wire shape of one CU chunk's value (the inner bytes after the "CU" flat-TLV
// name+size), mirroring NapiNPChunk_Create @ 0x624720 + the
// CNapiNPConnection_SendClientJoin writer: [type:1B][name + NUL][LE16 data_len][value + NUL], where
// data_len = value.size()+1 (retail stores strlen(value)+1). `type` is 1 or 2
// (the only values HandleClientJoin @ 0x62B750's CU loop accepts).
std::vector<uint8_t> make_client_cu_chunk(uint8_t type, std::string_view name,
                                          std::string_view value);

// Decode one CU chunk produced by make_client_cu_chunk (inverse). Returns
// false if the shape doesn't hold; out_value has the trailing NUL stripped.
bool parse_client_cu_chunk(const uint8_t *data, size_t len, uint8_t &out_type,
                           std::string &out_name, std::string &out_value);

// Control-setting entry — (direction_byte, field_index, uint32 value).
// CLIENT_* direction=1, SERVER_* direction=0. TWO templates exist, each identical across its own
// two directions, so ServerAuth.client_cs and .server_cs take the same list: the NOVAWORLDUDP
// SERVICE protocol's {0:240000,1:4,4:60000,5:1000,6:0xFFFFFFFF,8:2048,9:128,10:100,11:500,12:1,
// 13:1300(MTU),14:0xFFFFFFFF} (novaworld_service_cs_fields; [orig:
// CNapiGameSession_InitNPConnection @0x4d3e1f]) and the JOINTOPERATIONS in-game protocol's
// {0:120000,1:4,4:30000,5:10000,6:0xFFFFFFFF,8:512,9:256,10:100,11:1200,12:1,13:1300,
// 14:0xFFFFFFFF} (jointoperations_cs_fields; [orig: CNapiNetwork_Init @0x4ca4a0]); idx 2/3/7
// = 0. A GAME host's 0x82 carries the latter [orig: CNapiNPConnection_Create @0x62acb0 copies
// the protocol object's +0xE44/+0xE80 blocks; CNapiNPConnection_SendSessionInit @0x620ef0].
struct CsField {
	uint8_t field_index;
	uint32_t value;
};
// Field 13 (the datagram ceiling) is the one configured entry: both templates derive it from the
// game.cfg `mpmaxpacketsize` value through the same SIGNED ladder — 0 -> 1300, below 100 (a
// negative value included) -> 100, above the ceiling -> the ceiling — and the ceiling differs per
// template: 0x4000 for the JOINTOPERATIONS game session, 0x10000 for the NOVAWORLDUDP service.
// `max_packet_bytes` is that configured value; 0 (the default) yields 1300.
// [orig: CNapiNetwork_Init @0x4ca4a0 clamp @0x4caa53..0x4caa76 (`jge` @0x4caa66, `jle`
//  @0x4caa74, 0x4000); CNapiGameSession_InitNPConnection @0x4d3be0 clamp @0x4d3df4..0x4d3e17
//  (`jge` @0x4d3e07, `jle` @0x4d3e15, 0x10000); source g_GameConfigState.maxPacketSize_338,
//  game.cfg key `mpmaxpacketsize`]
inline constexpr int32_t kCsMaxPacketDefault = 1300;
inline constexpr int32_t kCsMaxPacketFloor = 100;
inline constexpr int32_t kCsMaxPacketCeilingGame = 0x4000;
inline constexpr int32_t kCsMaxPacketCeilingService = 0x10000;
uint32_t cs_max_packet_bytes(int32_t configured, int32_t ceiling);
std::vector<CsField> novaworld_service_cs_fields(int32_t max_packet_bytes = 0); // NOVAWORLDUDP service
std::vector<CsField> jointoperations_cs_fields(int32_t max_packet_bytes = 0);   // the in-game session

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

	std::string scrk;     // Server-side Session CRypto Key (up to 63 chars; see make_dev_scrk)
	std::string na;       // echo client.na
	uint32_t rip = 0;     // Reflected IP (client's remote IP)
	uint32_t rpn = 0;     // Reflected Port Number
	// The server connection's reconnect count, emitted after RPN only when nonzero: the
	// joining 0x42's RCNT + 1 when that 0x42 counted a disconnect (DCNT > 0). The client stores
	// it (0 when absent) and echoes it on its next 0x42.
	// [orig: CNapiNPConnection_SendSessionInit @0x620ef0 @0x62125d..0x62127c; the client's
	//  store NapiNP_HandleServerJoinResponse @0x629840 @0x629e2e]
	uint32_t rcnt = 0;

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
// Defaults per retail capture (docs/net/novaworld-net-re.md §7):
//   novaworld_name = "NWServer" (was "OpenNova"; retail emits the literal "NWServer")
//   novaworld_web_url = bare "host:port", NO scheme: the retail client builds
//                    "http://%s" from this CU itself, so a scheme here yields
//                    "http://http://..." on a stock client
//                    [orig: CNapiGameSession_OnNovaWorldConnected @0x4d1570 sprintf @0x4d1627]
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
                             std::string_view novaworld_web_url = "127.0.0.1:8080",
                             std::string_view nwuid = "0accd1b2cffac0e3f1efe6c8000000000000000000000000000000000000",
                             bool include_novaworld_cu = true);

// Serialize a ServerAuth to flat-TLV bytes (ready to opcode-prefix +
// NWU-encrypt + CRC-envelope).
std::vector<uint8_t> server_auth_to_bytes(const ServerAuth &msg);

// The 0x42-time join-REJECTION form of the 0x82: exactly CI, CK, CR(=0), JFC
// (failure family), JFP (sub-reason), and JFS only when non-empty. The
// validate-callback family is JFC=14 with JFP 2 locked / 3 banned / 4 full /
// 5 full-with-positive-spectator-slots. [orig: NapiNPProtocol_SendJoinRejection
// @0x620cd0; reasons CNapiNetwork_ValidateJoinRequest @0x4c61b0]
std::vector<uint8_t> server_auth_rejection_to_bytes(const ServerAuth &msg);

// Parse a ServerAuth TLV payload (the bytes AFTER the 0x82 opcode and
// AFTER NWU-decryption). The client uses this to recover the connection
// result `cr` (1 = accepted), the server key `sk` (which becomes the
// session_id on the client's outbound 0x43 ProtocolMessages), and the
// server-side `scrk` (which decrypts inbound 0x83 ProtocolMessages). Also
// recovers the CS/CU control fields for round-trip fidelity. Inverse of
// server_auth_to_bytes; ignores unknown tags. Returns true on success.
bool parse_server_auth(const uint8_t *data, size_t len, ServerAuth &out);

} // namespace opennova
