#pragma once

#include <array>
#include <cstdint>
#include <string>
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

// ServerHello, sent S2C as opcode 0x81 payload.
struct ServerHello {
	uint32_t ci = 0;
	std::string co = "NovaLogic Inc, Calabasas CA U.S.A.";
	std::string ap = "OpenNova NWServer";
	std::string bdat = "Jan  1 2026 00:00:00";
	uint32_t ut = 0; // uptime / unix time
	std::string pn = "NOVAWORLDUDP";
	std::array<uint8_t, 16> pg{};
	std::string pv1 = "0.0.0 2/10/2004 EM";
	std::string pv2 = "1";
	std::string pv3 = "1.6.4r opennova";
	uint32_t hk = 0x0FE0E112u; // host key (opaque to the client beyond echo in ClientJoin)
	std::string sn = "OpenNova";
	std::string pl = "WIN32";
	uint32_t nc = 0; // node count
	uint32_t rip = 0; // reflected IP (client's apparent IP)
	uint32_t rpn = 0; // reflected port
	uint32_t eip = 0; // external IP (echo of ClientHello.eip)
	uint32_t epn = 0; // external port (echo of ClientHello.epn)

	// Game-server-only fields. When `is_game_server` is true these are
	// emitted between SN/PL and NC (SF/P1/P2/NP/MP), and SUS1/SUS2 are
	// emitted between RPN and EIP. Witnessed in the retail capture
	// (notes/retail_capture2_decoded.txt frame 62334) — the host's
	// ServerHello on the game-server UDP port carries these extra fields
	// so the client knows the game type, current/max players, expansion,
	// and game-session id. Without them the client receives a generic
	// matchmaking-shaped Hello and never enters the gametype-specific
	// game UI (e.g. cmap.mnu spawn-selection).
	bool is_game_server = false;
	uint32_t sf = 0;          // server flags; retail observed = 0
	uint32_t p1 = 0x00010010; // gametype: 0x10000 = AS (Advance & Secure); low 0x10 unwitnessed (some feature flag)
	uint32_t p2 = 0x00000404; // game-config (purpose unwitnessed beyond observation)
	uint32_t np = 1;          // current player count
	uint32_t mp = 2;          // max-players or game-mode parameter (retail had 2)
	std::string sus1 = "GSID-10-OPENNOVA-DEV";  // unique session id (retail format: GSID-NN-XXXXXXXX-timestamp-hash)
	std::string sus2 = "jox01";                 // expansion-pack archive name (Kendari expansion; one of its terrains is "dvxi5", used by ASH_I5A.bms)
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

// ---- ClientAuth / ServerAuth (novaworld-service auth step) -------------
//
// NOTE ON NAMING: onnet calls these "ClientJoin" / "ServerJoin". That's
// misleading — this is NOT joining a game server. It's the client
// authenticating itself to the NovaWorld matchmaking service after the
// Hello exchange. The real "join a game" event happens much later, on a
// fresh connection to a specific game server. We use `Auth` in code;
// `Join` appears only in doc comments as an onnet-compat alias.
//
// Opcodes: `0x42` ClientAuth (C→S), `0x82` ServerAuth (S→C).

struct ClientAuth {
	uint32_t ci = 0;   // Client Index (echo from Hello)
	uint32_t hk = 0;   // Host Key (echo from ServerHello)
	uint32_t ck = 0;   // Client Key (client-generated)
	std::string na;    // Name / alias (e.g. "jop:cus2")
	uint32_t sip = 0;  // Source IP
	uint32_t spn = 0;  // Source Port Number
	std::string scrk;  // Client-side Session CRypto Key
	std::vector<std::vector<uint8_t>> cu; // Custom/User blobs (raw bytes, unparsed)
};

bool parse_client_auth(const uint8_t *data, size_t len, ClientAuth &out);

// Serialize a ClientAuth back to flat-TLV bytes (inverse of
// parse_client_auth). Field order: CI, HK, CK, NA, SIP, SPN, SCRK,
// followed by any CU blobs in order. Note: retail jodemo also re-emits
// the ClientHello header fields (NVS/CO/AP/...) inside its ClientAuth
// payload — we don't, since the server's parser ignores them anyway.
std::vector<uint8_t> client_auth_to_bytes(const ClientAuth &msg);

// Control-setting entry — (direction_byte, field_index, uint32 value).
// CLIENT_* direction=1, SERVER_* direction=0. Values are currently from
// onnet's nwu_protocol.py CLIENT_CS_FIELD_VALUES/SERVER_CS_FIELD_VALUES
// and [UNVERIFIED — from onnet, not IDA] until witnessed in the binary's
// CS builder.
struct CsField {
	uint8_t field_index;
	uint32_t value;
};
std::vector<CsField> default_client_cs_fields();
std::vector<CsField> default_server_cs_fields();

struct ServerAuth {
	uint32_t ci = 0;      // echo client.ci
	uint32_t mi = 0x113fu; // Machine ID — retail capture pcap1 frame 28 = 4415 (0x113f); was 15582 from onnet, but retail's value disagrees so we follow the wire.
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
ServerAuth build_server_auth(const ClientAuth &client,
                             uint32_t client_ip_net,
                             uint16_t client_port,
                             uint32_t server_sk,
                             std::string_view server_scrk,
                             std::string_view novaworld_name = "NWServer",
                             std::string_view novaworld_web_url = "http://127.0.0.1:8080",
                             std::string_view nwuid = "0accd1b2cffac0e3f1efe6c80000000000000000000000000000000000000000");

// Serialize a ServerAuth to flat-TLV bytes (ready to opcode-prefix +
// NWU-encrypt + CRC-envelope).
std::vector<uint8_t> server_auth_to_bytes(const ServerAuth &msg);

} // namespace opennova
